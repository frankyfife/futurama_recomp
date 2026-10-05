// Self-tests of the gfx module (M2; run with --selftest, phase Mapped = after mapping and hooks, before
// the guest runs):
//   GfxRendererSlotTable   the generated slot table equals the mapped NiXBoxRenderer vtable 0x2AA7C0,
//                          NiPCRenderer slots 2..10 are the original entries, all others host code
//   GfxRendererThunkPurge  the stack purge of every NiPCRenderer vtable thunk (66) and of the two texture
//                          data vtables (11 + 14) is measured at runtime (purge_probe.h) and compared with
//                          the ret N of the original (renderer_slots.gen.inc / renderer_retn.py)
//   GfxD3DShimTrapPurge    the same for the 8 D3D shim functions and the 79 D3D trap stubs
#include "core/guest.h"
#include "core/hooks.h"
#include "core/selftest.h"
#include "gfx/d3d_shim.h"
#include "gfx/d3d_traps.h"
#include "gfx/guest_memory.h"
#include "gfx/nipc_renderer.h"
#include "gfx/purge_probe.h"

#include <cstdio>
#include <string>

namespace gfx {
namespace {

// Appends "name: measured X != expected Y" for a failed measurement; returns true if it matched.
bool CheckPurge(const char* what, std::uint32_t index, const void* entry, std::uint32_t ecx,
                std::uint32_t stackDwords, std::uint32_t expectedBytes, std::string& failures) {
    const PurgeMeasurement m = MeasureCalleePurge(entry, ecx, 0, stackDwords);
    char buf[160];
    if (!m.ok) {
        std::snprintf(buf, sizeof(buf), " %s %u: %s;", what, index, m.error.c_str());
        failures += buf;
        return false;
    }
    if (m.purgeBytes != expectedBytes) {
        std::snprintf(buf, sizeof(buf), " %s %u: ret 0x%X measured, 0x%X expected;", what, index, m.purgeBytes,
                      expectedBytes);
        failures += buf;
        return false;
    }
    return true;
}

}  // namespace
}  // namespace gfx

FUT_SELFTEST(GfxRendererSlotTable, Mapped) {
    const std::uint32_t* vt = gfx::RendererVtable();
    if (vt == nullptr) {
        detail = "NiPCRenderer vtable could not be allocated";
        return false;
    }
    for (std::uint32_t s = 0; s < gfx::kRendererSlotCount; ++s) {
        const gfx::RendererSlot& row = gfx::kRendererSlots[s];
        const std::uint32_t original = guest::Read<std::uint32_t>(guest::kNiXBoxRendererVtable + 4 * s);
        char buf[160];
        if (row.slot != s || row.vtableOffset != 4 * s || original != row.originalAddress) {
            std::snprintf(buf, sizeof(buf), "slot %u: table 0x%08X, mapped vtable 0x%08X", s, row.originalAddress,
                          original);
            detail = buf;
            return false;
        }
        const bool keepOriginal = s >= gfx::kFirstOriginalSlot && s <= gfx::kLastOriginalSlot;
        if (keepOriginal ? vt[s] != original : (vt[s] == 0 || guest::InXbeImage(vt[s]))) {
            std::snprintf(buf, sizeof(buf), "slot %u: NiPCRenderer entry 0x%08X (original 0x%08X)", s, vt[s], original);
            detail = buf;
            return false;
        }
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "75 slots = vtable 0x%08X; slots 2..10 original, 66 host thunks at 0x%08X",
                  guest::kNiXBoxRendererVtable, guest::Addr(vt));
    detail = buf;
    return true;
}

FUT_SELFTEST(GfxRendererThunkPurge, Mapped) {
    const std::uint32_t* vt = gfx::RendererVtable();
    const std::uint32_t* source = gfx::SourceTextureDataVtable();
    const std::uint32_t* rendered = gfx::RenderedTextureDataVtable();
    void* scratch = gfx::AllocGuestPages(0x1000);  // zeroed stand-in for `this` (probe mode ignores it)
    if (vt == nullptr || source == nullptr || rendered == nullptr || scratch == nullptr) {
        gfx::FreeGuestPages(scratch);
        detail = "tables or scratch memory missing";
        return false;
    }
    const std::uint32_t self = guest::Addr(scratch);
    std::string failures;
    std::uint32_t measured = 0;
    for (std::uint32_t s = 0; s < gfx::kRendererSlotCount; ++s) {
        if (s >= gfx::kFirstOriginalSlot && s <= gfx::kLastOriginalSlot) {
            continue;
        }
        const std::uint32_t args = gfx::kRendererSlots[s].stackArgs;
        gfx::CheckPurge("renderer slot", s, guest::Ptr<const void>(vt[s]), self, args, 4 * args, failures);
        ++measured;
    }
    for (std::uint32_t s = 0; s < gfx::kSourceDataSlotCount; ++s) {
        const std::uint32_t args = gfx::kSourceDataStackArgs[s];
        gfx::CheckPurge("source data slot", s, guest::Ptr<const void>(source[s]), self, args, 4 * args, failures);
        ++measured;
    }
    for (std::uint32_t s = 0; s < gfx::kRenderedDataSlotCount; ++s) {
        const std::uint32_t args = gfx::kRenderedDataStackArgs[s];
        gfx::CheckPurge("rendered data slot", s, guest::Ptr<const void>(rendered[s]), self, args, 4 * args, failures);
        ++measured;
    }
    gfx::FreeGuestPages(scratch);
    if (!failures.empty()) {
        detail = "purge mismatch:" + failures;
        return false;
    }
    detail = std::to_string(measured) + " thunks measured (66 renderer slots, 11 source + 14 rendered texture data "
             "slots): every ret N equals the original";
    return true;
}

FUT_SELFTEST(GfxD3DShimTrapPurge, Mapped) {
    std::string failures;
    std::uint32_t measured = 0, installed = 0, hooks = 0;
    gfx::d3d::ShimEntry e{};
    for (std::uint32_t i = 0; gfx::d3d::GetShimEntry(i, e); ++i) {
        gfx::CheckPurge("shim", i, e.entry, 0, e.stackDwords, 4 * e.stackDwords, failures);
        ++measured;
        ++hooks;
        installed += core::IsHookInstalled(e.guestAddress) ? 1u : 0u;
    }
    for (std::uint32_t i = 0; i < gfx::d3d::kTrapCount; ++i) {
        const gfx::d3d::TrapRow& row = gfx::d3d::kTrapRows[i];
        gfx::CheckPurge("trap", i, gfx::d3d::TrapEntry(i), 0, row.purgeBytes / 4, row.purgeBytes, failures);
        ++measured;
        ++hooks;
        installed += core::IsHookInstalled(row.address) ? 1u : 0u;
    }
    if (!failures.empty()) {
        detail = "purge mismatch:" + failures;
        return false;
    }
    detail = std::to_string(measured) + " D3D entries measured (8 shims, " + std::to_string(gfx::d3d::kTrapCount) +
             " traps): every ret N equals d3d_functions.tsv; " + std::to_string(installed) + "/" +
             std::to_string(hooks) + " hooks installed";
    return true;
}
