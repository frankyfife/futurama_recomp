// Unit tests of the core module (no XBE needed): MASM host->guest call helpers, guest thread
// adoption (TLS slots read through fs:[0xE10+4k]), hook disable rules, generated tables.
#include "doctest.h"

#include "core/config.h"
#include "core/crash.h"
#include "core/fs_patcher.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/guest_thread.h"
#include "core/hooks.h"
#include "shared/launcher_info.h"
#include "shared/xbox_types.h"

#include <intrin.h>

#include <cstring>
#include <thread>

namespace {

std::uint32_t __stdcall Stdcall4(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    return a + 2 * b + 3 * c + 4 * d;
}
std::uint32_t __cdecl Cdecl2(std::uint32_t a, std::uint32_t b) {
    return a ^ b;
}
std::uint32_t __fastcall Fastcall4(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    return (a << 24) | (b << 16) | (c << 8) | d;
}
struct Counter {
    std::uint32_t base;
    std::uint32_t Add(std::uint32_t x, std::uint32_t y) { return base + x + y; }  // thiscall
};
std::uint32_t NoArgs() {
    return 0x5A5A5A5A;
}

guest::GuestAddr AddrOf(const void* p) {
    return guest::Addr(p);
}

// Process-wide fake launcher info with real TlsAlloc indices (the tests run without the launcher).
const LauncherInfo& TestLauncherInfo() {
    static LauncherInfo info = [] {
        LauncherInfo li{};
        li.size = sizeof(li);
        li.version = FUT_LAUNCHER_INFO_VERSION;
        for (auto& slot : li.tlsSlots) {
            slot = TlsAlloc();
        }
        li.xbeSpace = FUT_XBE_SPACE_ADDRESS;
        li.xbeSpaceSize = FUT_XBE_SPACE_SIZE;
        li.imageBase = FUT_LAUNCHER_IMAGE_BASE;
        return li;
    }();
    core::SetLauncherInfo(&info);
    return info;
}

}  // namespace

TEST_CASE("core: guest call helpers pass arguments and restore the stack") {
    CHECK(fut_guest_call0(AddrOf(reinterpret_cast<const void*>(&NoArgs))) == 0x5A5A5A5Au);
    CHECK(core::CallGuest(AddrOf(reinterpret_cast<const void*>(&Stdcall4)), {1, 2, 3, 4}) == 30u);
    CHECK(core::CallGuest(AddrOf(reinterpret_cast<const void*>(&Cdecl2)), {0xF0F0, 0x0FF0}) == 0xFF00u);
    CHECK(core::CallGuestFastcall(AddrOf(reinterpret_cast<const void*>(&Fastcall4)), 1, 2, {3, 4}) ==
          0x01020304u);
    Counter counter{100};
    const auto method = &Counter::Add;
    CHECK(core::CallGuestThiscall(guest::Addr(core::detail::CodeAddress(method)), guest::Addr(&counter),
                                  {20, 3}) == 123u);
    // Many calls in a row: a wrong esp restore would corrupt this frame quickly.
    std::uint32_t sum = 0;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        sum += core::CallGuest(AddrOf(reinterpret_cast<const void*>(&Stdcall4)), {i, 0, 0, 0});
    }
    CHECK(sum == 999u * 1000u / 2u);
}

TEST_CASE("core: AdoptCurrentThread sets the five TLS slots (design 1.6)") {
    const LauncherInfo& li = TestLauncherInfo();
    for (auto slot : li.tlsSlots) {
        REQUIRE(slot < FUT_TEB_TLS_SLOTS_INLINE);
    }
    std::thread worker([&] {
        core::GuestThread t{};
        REQUIRE(core::AdoptCurrentThread(xbox::kTlsDataSize, "test-guest", &t));
        CHECK(t.tlsData != nullptr);
        CHECK(t.kthread->TlsData == guest::Addr(t.tlsData));
        CHECK(t.tlsAnchor == guest::Addr(t.tlsData) + xbox::kTlsDataSize);
        CHECK(t.kthread->ThreadId == GetCurrentThreadId());
        CHECK(t.kthread->StackBase > t.kthread->StackLimit);
        // The patched guest code reads these DWORDs as fs:[0xE10 + 4k].
        CHECK(__readfsdword(FUT_TEB_TLS_SLOTS_OFFSET + 4 * li.tlsSlots[FUT_TLS_S_TLS]) == t.tlsAnchor);
        CHECK(__readfsdword(FUT_TEB_TLS_SLOTS_OFFSET + 4 * li.tlsSlots[FUT_TLS_S_KTHREAD]) ==
              guest::Addr(t.kthread));
        CHECK(__readfsdword(FUT_TEB_TLS_SLOTS_OFFSET + 4 * li.tlsSlots[FUT_TLS_S_IRQL]) == 0u);
        CHECK(__readfsdword(FUT_TEB_TLS_SLOTS_OFFSET + 4 * li.tlsSlots[FUT_TLS_S_DPC]) == 0u);
        const auto* prcb = guest::Ptr<const xbox::FakeKPrcb>(core::GetGuestSlot(FUT_TLS_S_PRCB));
        REQUIRE(prcb != nullptr);
        CHECK(prcb->DebugMonitorData == 0u);
        CHECK(guest::Addr(t.kthread) < guest::kGuestMemoryLimit);
        CHECK(core::CurrentGuestThread() != nullptr);
        core::ReleaseCurrentThread();
        CHECK(core::GetGuestSlot(FUT_TLS_S_TLS) == 0u);
        CHECK(core::CurrentGuestThread() == nullptr);
    });
    worker.join();
}

TEST_CASE("core: boot-thread adoption has TlsData = 0 and a valid dummy anchor (cpu.md 2.2)") {
    TestLauncherInfo();
    std::thread worker([] {
        core::GuestThread t{};
        REQUIRE(core::AdoptCurrentThread(0, "test-boot", &t));
        CHECK(t.tlsData == nullptr);
        CHECK(t.kthread->TlsData == 0u);
        // [anchor + _tls_index * 4] = [anchor - 0x24] = pointer to a zeroed 0x20-byte TLS block
        const std::uint32_t blockPtr = guest::Read<std::uint32_t>(t.tlsAnchor - 0x24);
        CHECK(blockPtr != 0u);
        CHECK(guest::IsReadable(blockPtr, 0x20));
        CHECK(guest::Read<std::uint32_t>(blockPtr + xbox::kTlsBlockOffsetLastError) == 0u);
        core::ReleaseCurrentThread();
    });
    worker.join();
}

TEST_CASE("core: AdoptCurrentThreadWith uses caller memory") {
    TestLauncherInfo();
    std::thread worker([] {
        void* mem = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        REQUIRE(mem != nullptr);
        auto* kthread = static_cast<xbox::FakeKThread*>(mem);
        auto* tls = static_cast<std::uint8_t*>(mem) + 0x800;
        core::GuestThread t{};
        REQUIRE(core::AdoptCurrentThreadWith(kthread, tls, xbox::kTlsDataSize, "test-with", &t));
        CHECK(kthread->TlsData == guest::Addr(tls));
        CHECK(core::GetGuestSlot(FUT_TLS_S_KTHREAD) == guest::Addr(kthread));
        CHECK(core::GetGuestSlot(FUT_TLS_S_TLS) == guest::Addr(tls) + xbox::kTlsDataSize);
        core::ReleaseCurrentThread();
        VirtualFree(mem, 0, MEM_RELEASE);  // still owned by the caller
    });
    worker.join();
}

TEST_CASE("core: [hooks] disable matches name, prefix* and 0xADDR (design 3.2)") {
    core::Config cfg;
    const core::HookDesc desc{0x0013F250, nullptr, core::HookConv::Thiscall, "NiUDSStream::Ctor", nullptr,
                              true,       ""};
    CHECK_FALSE(core::IsHookDisabled(desc, cfg));
    cfg.hooksDisable = {"NiUDSStream::Ctor"};
    CHECK(core::IsHookDisabled(desc, cfg));
    cfg.hooksDisable = {"NiUDSStream::*"};
    CHECK(core::IsHookDisabled(desc, cfg));
    cfg.hooksDisable = {"0x13F250"};
    CHECK(core::IsHookDisabled(desc, cfg));
    cfg.hooksDisable = {"0x13f250"};
    CHECK(core::IsHookDisabled(desc, cfg));
    cfg.hooksDisable = {"Other::*", "0x13F251"};
    CHECK_FALSE(core::IsHookDisabled(desc, cfg));
}

TEST_CASE("core: M1 hook targets are hookable function starts (design 3.2, Anhang B)") {
    const guest::GuestAddr targets[] = {
        guest::kXapiQueryPerformanceCounter, guest::kXapiQueryPerformanceFrequency,
        guest::kXapiOutputDebugStringA,      guest::kXapiXMountUtilityDrive,
        guest::kXapiXLaunchNewImageA,        guest::kGameMain,
        guest::kXapiSwitchToFiber,           guest::kNiXBoxRendererCreate};
    for (const guest::GuestAddr a : targets) {
        CAPTURE(a);
        const guest::FunctionInfo* f = guest::FindFunctionStart(a);
        REQUIRE(f != nullptr);
        CHECK(f->size >= 5u);
        for (const auto& site : guest::FsPatchSites()) {
            CHECK_FALSE((site.address < a + 5 && a < site.address + site.length));
        }
    }
    CHECK(guest::Symbolize(0x0013F5D5) == "0x0013F5D5 FUN_0013f590+0x45");  // design 1.11 example
}

TEST_CASE("core: M1 boot probe targets leave room for their stolen bytes (xapi/boot_probes.cpp)") {
    struct Probe {
        guest::GuestAddr address;
        std::uint32_t stolen;
    };
    const Probe probes[] = {{guest::kXapiInitProcess, 6},          {guest::kXapiRtInit, 7},
                            {guest::kXapiCInit, 5},                {guest::kXapiXGetDiskClusterSizeA, 6},
                            {guest::kGameMemoryPoolInit, 6},       {guest::kXapiBootToDash, 9}};
    for (const Probe& p : probes) {
        CAPTURE(p.address);
        const guest::FunctionInfo* f = guest::FindFunctionStart(p.address);
        REQUIRE(f != nullptr);
        CHECK(f->size >= p.stolen);
        for (const auto& site : guest::FsPatchSites()) {
            CHECK_FALSE((site.address < p.address + p.stolen && p.address < site.address + site.length));
        }
    }
}

TEST_CASE("core: fs patch table rows are consistent (design 1.6)") {
    const auto sites = guest::FsPatchSites();
    REQUIRE(sites.size() == 38u);
    for (const auto& s : sites) {
        CAPTURE(s.address);
        CHECK(s.original[0] == 0x64);
        std::uint32_t disp = 0;
        std::memcpy(&disp, &s.original[s.dispOffset], sizeof(disp));
        CHECK(disp == s.originalDisp);
        CHECK(s.dispOffset + 4u == s.length);  // disp32 is always the last field (no immediates)
    }
    TestLauncherInfo();
    CHECK(core::FsPatchDisplacement(FUT_TLS_S_IRQL) ==
          FUT_TEB_TLS_SLOTS_OFFSET + 4 * core::TlsSlotIndex(FUT_TLS_S_IRQL));
}

TEST_CASE("core: LogAllThreads snapshots other threads and returns (watchdog diagnostics)") {
    const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(release != nullptr);
    std::thread sleeper([&] { WaitForSingleObject(release, INFINITE); });
    core::LogAllThreads("unit test");  // must not deadlock (threads are resumed before logging)
    SetEvent(release);
    sleeper.join();
    CloseHandle(release);
    CHECK(core::GuestExceptionCount() == 0u);
}
