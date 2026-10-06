// Unit tests of the gfx module (M2, gfx agent): NiPCRenderer slot table and thunk purges, D3D trap
// table, D3D shim resource semantics. Run without the XBE: the vtable thunks, shims and traps are host
// functions; their stack purge is measured with gfx::MeasureCalleePurge (probe mode, no side effects).
#include "doctest.h"

#include "core/guest.h"
#include "core/hooks.h"
#include "gfx/d3d_shim.h"
#include "gfx/d3d_traps.h"
#include "gfx/gfx_config.h"
#include "gfx/guest_memory.h"
#include "gfx/nipc_renderer.h"
#include "gfx/purge_probe.h"

#include <cstdint>
#include <set>

namespace {

std::uint32_t __stdcall PurgeStdcall3(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept {
    return a + b + c;
}
std::uint32_t __fastcall PurgeFastcall3(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept {
    return a ^ b ^ c;
}
std::uint32_t __cdecl PurgeCdecl2(std::uint32_t a, std::uint32_t b) noexcept {
    return a - b;
}

std::uint32_t Measure(const void* entry, std::uint32_t stackDwords) {
    const gfx::PurgeMeasurement m = gfx::MeasureCalleePurge(entry, 0, 0, stackDwords);
    REQUIRE_MESSAGE(m.ok, m.error);
    return m.purgeBytes;
}

const void* Code(std::uint32_t address) {
    return guest::Ptr<const void>(address);
}

}  // namespace

TEST_CASE("gfx: MeasureCalleePurge measures the callee's stack cleanup") {
    CHECK(Measure(core::detail::CodeAddress(&PurgeStdcall3), 3) == 12);
    CHECK(Measure(core::detail::CodeAddress(&PurgeFastcall3), 1) == 4);  // a/b in ecx/edx, c on the stack
    CHECK(Measure(core::detail::CodeAddress(&PurgeCdecl2), 2) == 0);
    CHECK_FALSE(gfx::InPurgeProbe());
}

TEST_CASE("gfx: renderer slot table matches renderer_retn.py and renderer.md 2") {
    REQUIRE(std::size(gfx::kRendererSlots) == 75);
    for (std::uint32_t s = 0; s < gfx::kRendererSlotCount; ++s) {
        CHECK(gfx::kRendererSlots[s].slot == s);
        CHECK(gfx::kRendererSlots[s].vtableOffset == 4 * s);
    }
    // Spot checks against renderer.md 2 (Args column) incl. the four slots renderer_retn.py cannot see.
    const std::pair<std::uint32_t, std::uint32_t> expected[] = {
        {0, 1},  {1, 0},  {15, 1}, {16, 0}, {17, 2}, {21, 0}, {22, 0}, {25, 4}, {27, 1}, {31, 3},
        {35, 6}, {39, 5}, {42, 0}, {47, 6}, {48, 4}, {52, 3}, {56, 4}, {59, 1}, {65, 1}, {66, 1},
        {71, 0}, {74, 5}};
    for (const auto& [slot, args] : expected) {
        CAPTURE(slot);
        CHECK(gfx::kRendererSlots[slot].stackArgs == args);
    }
    CHECK(gfx::kRendererSlots[0].originalAddress == 0x0014CB90);
    CHECK(gfx::kRendererSlots[16].originalAddress == 0x00146460);
    CHECK(gfx::kRendererSlots[65].originalAddress == 0x00147350);
}

TEST_CASE("gfx: every NiPCRenderer and texture data thunk pops exactly the original ret N") {
    const std::uint32_t* vt = gfx::RendererVtable();
    const std::uint32_t* source = gfx::SourceTextureDataVtable();
    const std::uint32_t* rendered = gfx::RenderedTextureDataVtable();
    REQUIRE(vt != nullptr);
    REQUIRE(source != nullptr);
    REQUIRE(rendered != nullptr);
    CHECK(static_cast<std::uint64_t>(guest::Addr(vt)) < guest::kGuestMemoryLimit);
    for (std::uint32_t s = 0; s < gfx::kRendererSlotCount; ++s) {
        CAPTURE(s);
        if (s >= gfx::kFirstOriginalSlot && s <= gfx::kLastOriginalSlot) {
            CHECK(vt[s] == gfx::kRendererSlots[s].originalAddress);  // design 2.2: original NiObject slots
            continue;
        }
        CHECK(Measure(Code(vt[s]), gfx::kRendererSlots[s].stackArgs) == 4 * gfx::kRendererSlots[s].stackArgs);
    }
    for (std::uint32_t s = 0; s < gfx::kSourceDataSlotCount; ++s) {
        CAPTURE(s);
        CHECK(Measure(Code(source[s]), gfx::kSourceDataStackArgs[s]) == 4 * gfx::kSourceDataStackArgs[s]);
    }
    for (std::uint32_t s = 0; s < gfx::kRenderedDataSlotCount; ++s) {
        CAPTURE(s);
        CHECK(Measure(Code(rendered[s]), gfx::kRenderedDataStackArgs[s]) == 4 * gfx::kRenderedDataStackArgs[s]);
    }
}

TEST_CASE("gfx: D3D trap table covers the external D3D entries except the 8 shims") {
    REQUIRE(gfx::d3d::kTrapCount == 79);
    std::set<std::uint32_t> shims;
    gfx::d3d::ShimEntry e{};
    for (std::uint32_t i = 0; gfx::d3d::GetShimEntry(i, e); ++i) {
        shims.insert(e.guestAddress);
    }
    REQUIRE(shims.size() == 8);
    std::uint32_t fastcalls = 0;
    for (std::uint32_t i = 0; i < gfx::d3d::kTrapCount; ++i) {
        const gfx::d3d::TrapRow& row = gfx::d3d::kTrapRows[i];
        CAPTURE(i);
        CHECK(shims.count(row.address) == 0);
        CHECK(row.address != 0x002417B0);  // MmFreeContiguousMemory import thunk is not a D3D function
        CHECK(row.extCallSites > 0);
        if (row.conv == gfx::d3d::TrapConv::Fastcall) {
            ++fastcalls;
            CHECK(row.address == 0x002357D0);  // D3DDevice_SetRenderState_Simple (d3d.md 3.4)
        }
        CHECK(Measure(gfx::d3d::TrapEntry(i), row.purgeBytes / 4) == row.purgeBytes);
    }
    CHECK(fastcalls == 1);
    // Tail-call entries resolved by the generator: FrontFace -> CullMode (4), Get2DSurfaceDesc (0xC),
    // DeletePixelShader -> LocalFree (4); D3DDevice_Clear ends behind Ghidra's function end (0x18).
    for (const auto& [address, purge] : {std::pair<std::uint32_t, std::uint32_t>{0x00235C10, 4},
                                         {0x00237190, 0xC}, {0x00241570, 4}, {0x0023B380, 0x18}}) {
        bool found = false;
        for (const gfx::d3d::TrapRow& row : gfx::d3d::kTrapRows) {
            if (row.address == address) {
                found = true;
                CHECK(row.purgeBytes == purge);
            }
        }
        CHECK(found);
    }
}

TEST_CASE("gfx: D3D shim entries pop the original ret N") {
    const std::uint32_t expected[] = {0, 0, 0x1C, 8, 4, 4, 4, 8};  // design 2.2 table / d3d_functions.tsv
    gfx::d3d::ShimEntry e{};
    for (std::uint32_t i = 0; gfx::d3d::GetShimEntry(i, e); ++i) {
        CAPTURE(e.name);
        CHECK(4 * e.stackDwords == expected[i]);
        CHECK(Measure(e.entry, e.stackDwords) == expected[i]);
    }
}

TEST_CASE("gfx: D3D shim texture and surface follow the Xbox refcount rules") {
    using namespace gfx::d3d;
    const std::size_t blocksBefore = gfx::GuestBlocksInUse();
    const std::uint32_t tex = DeviceCreateTexture2(320, 240, 1, 1, 0, 0x24, 3);  // movie player call shape
    REQUIRE(tex != 0);
    CHECK(static_cast<std::uint64_t>(tex) < guest::kGuestMemoryLimit);
    CHECK(guest::Read<std::uint32_t>(tex) == 0x01040001);  // Common as written at 0x237163
    ResourceInfo info{};
    REQUIRE(LookupResource(tex, info));
    CHECK(info.kind == ResourceKind::Texture);
    CHECK(info.width == 320);
    CHECK(info.height == 240);
    CHECK(info.format == 0x24);
    CHECK(info.pitch == 640);
    CHECK(info.pixelBytes == 320 * 240 * 2);
    CHECK(guest::Read<std::uint32_t>(tex + 4) == info.pixels);
    CHECK(((guest::Read<std::uint32_t>(tex + 0xC) >> 8) & 0xFF) == 0x24);

    const std::uint32_t surface = TextureGetSurfaceLevel2(tex, 0);
    REQUIRE(surface != 0);
    CHECK(guest::Read<std::uint32_t>(surface) == 0x01050001);
    CHECK(guest::Read<std::uint32_t>(surface + 0x14) == tex);  // parent
    REQUIRE(LookupResource(tex, info));
    CHECK(info.refCount == 2);  // FUN_0023CE60 AddRefs the parent

    CHECK(ResourceAddRef(surface) == 2);
    CHECK(ResourceRelease(surface) == 1);
    CHECK(ResourceRelease(surface) == 0);  // destroyed, parent released
    CHECK_FALSE(LookupResource(surface, info));
    REQUIRE(LookupResource(tex, info));
    CHECK(info.refCount == 1);
    CHECK(ResourceAddRef(tex) == 2);
    CHECK(ResourceRelease(tex) == 1);
    CHECK(ResourceRelease(tex) == 0);
    CHECK_FALSE(LookupResource(tex, info));
    CHECK(gfx::GuestBlocksInUse() == blocksBefore);

    // NULL, unknown pointers and unknown textures are ignored (design 2.2), never dereferenced.
    CHECK(ResourceRelease(0) == 0);
    CHECK(ResourceAddRef(0x12345678) == 0);
    CHECK(TextureGetSurfaceLevel2(0x12345678, 0) == 0);
    CHECK(DeviceCreateTexture2(64, 64, 1, 1, 0, 0x24, 5) == 0);  // cube map: unsupported -> NULL

    const std::uint32_t refs = DeviceAddRef();
    CHECK(DeviceRelease() == refs - 1);
    DeviceSetRenderStateYuvEnable(1);
    DeviceSetScreenSpaceOffset(-12.0f, 4.0f);
    const ShimState s = GetShimState();
    CHECK(s.yuvEnable);
    CHECK(s.screenOffsetX == -12.0f);
    CHECK(s.screenOffsetY == 4.0f);
    DeviceSetRenderStateYuvEnable(0);
    DeviceSetScreenSpaceOffset(0.0f, 0.0f);
    CHECK(DummyDevice() != 0);
}

TEST_CASE("gfx: a D3D trap in log mode returns 0, pops its arguments and counts the hit") {
    gfx::Settings logMode;
    logMode.trapMode = gfx::TrapMode::Log;
    gfx::OverrideSettingsForTest(&logMode);
    std::uint32_t stdcallIndex = gfx::d3d::kTrapCount, fastcallIndex = gfx::d3d::kTrapCount;
    for (std::uint32_t i = 0; i < gfx::d3d::kTrapCount; ++i) {
        const gfx::d3d::TrapRow& row = gfx::d3d::kTrapRows[i];
        if (row.conv == gfx::d3d::TrapConv::Fastcall) {
            fastcallIndex = i;
        } else if (row.purgeBytes == 8 && stdcallIndex == gfx::d3d::kTrapCount) {
            stdcallIndex = i;  // D3DDevice_SetRenderTarget (2 args)
        }
    }
    REQUIRE(stdcallIndex < gfx::d3d::kTrapCount);
    REQUIRE(fastcallIndex < gfx::d3d::kTrapCount);
    using Stdcall2 = std::uint32_t(__stdcall*)(std::uint32_t, std::uint32_t);
    using Fastcall2 = std::uint32_t(__fastcall*)(std::uint32_t, std::uint32_t);
    const std::uint32_t before = gfx::d3d::TrapHits(stdcallIndex);
    // A wrong purge would corrupt this frame's stack (MSVC /RTC checks esp after the call in Debug).
    CHECK(reinterpret_cast<Stdcall2>(const_cast<void*>(gfx::d3d::TrapEntry(stdcallIndex)))(0x11, 0x22) == 0);
    CHECK(reinterpret_cast<Stdcall2>(const_cast<void*>(gfx::d3d::TrapEntry(stdcallIndex)))(0x33, 0x44) == 0);
    CHECK(gfx::d3d::TrapHits(stdcallIndex) == before + 2);
    CHECK(reinterpret_cast<Fastcall2>(const_cast<void*>(gfx::d3d::TrapEntry(fastcallIndex)))(0x10, 0x20) == 0);
    CHECK(gfx::d3d::TrapHits(fastcallIndex) >= 1);
    gfx::OverrideSettingsForTest(nullptr);
}

TEST_CASE("gfx: settings defaults and test override") {
    gfx::Settings custom;
    custom.frameLimit = 7;
    custom.frameRate = 0;
    custom.trapMode = gfx::TrapMode::Log;
    gfx::OverrideSettingsForTest(&custom);
    CHECK(gfx::GetSettings().frameLimit == 7);
    CHECK(gfx::GetSettings().trapMode == gfx::TrapMode::Log);
    gfx::OverrideSettingsForTest(nullptr);
    // default (no [render] frame_rate in the test process) = [game] tick_rate = 60 since M3b (was 50)
    CHECK(gfx::GetSettings().frameRate == gfx::GetSettings().tickRate);
    CHECK(gfx::GetSettings().tickRate == 60);
    CHECK(gfx::GetSettings().trapMode == gfx::TrapMode::Abort);
}
