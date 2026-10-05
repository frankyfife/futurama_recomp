// SwitchToFiber replacement: hook registration and the C++ part of the switch (design 1.7 "Fibers",
// E9, R2; m1_report 9 item 4). Owned by the fiber agent. The register/stack part is
// asm/fiber_switch.asm; the kernel stacks and their SEH records are kernel/mm_kernel_stack.cpp.
//
// Why the TEB stack fields must follow the fiber (experiment 2026-10-05, see mm_kernel_stack.cpp):
// Windows' RtlDispatchException/RtlUnwind ignore (and SEHOP rejects) every SEH registration record
// outside TEB [StackLimit, StackBase). Guest __try blocks (IsBadReadPtr, heap) and the C++ EH of HLE
// code running on a fiber register their records on the fiber stack, so without the switch every
// exception on a fiber would end the process. The guest never reads fs:[4]/fs:[8] itself (its 11
// fs:[4] sites are retargeted to S_TLS, design 1.6), so changing them is invisible to it.
#include "xapi/xapi_fiber.h"

#include "core/config.h"
#include "core/crash.h"
#include "core/guest.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "kernel/mm_kernel_stack.h"
#include "shared/launcher_info.h"
#include "shared/xbox_types.h"

#include <intrin.h>

#include <atomic>

namespace xapi {
namespace {

constexpr std::uint32_t kTebStackBase = 0x04;           // NT_TIB.StackBase
constexpr std::uint32_t kTebStackLimit = 0x08;          // NT_TIB.StackLimit
constexpr std::uint32_t kTebDeallocationStack = 0xE0C;  // TEB.DeallocationStack (32-bit TEB)
constexpr std::uint32_t kExceptionChainEnd = 0xFFFFFFFFu;

std::atomic<guest::GuestAddr> g_tlsIndexVar{guest::kTlsIndexVar};
std::atomic<std::uint64_t> g_switches{0};
std::atomic<std::uint64_t> g_toThreadFiber{0};
std::atomic<std::uint64_t> g_chainFixups{0};
std::atomic<std::uint64_t> g_unknownStacks{0};
std::atomic<bool> g_reporterAdded{false};

// Real TEB stack values of the calling thread, saved whenever it leaves its ConvertThreadToFiber
// fiber (design 1.7: "beim ersten Wechsel gesicherte echte TEB-Werte"; saving on every switch away
// also keeps a StackLimit the guard-page logic lowered in the meantime).
struct ThreadStack {
    bool valid;
    std::uint32_t stackBase, stackLimit, deallocationStack;
};
thread_local ThreadStack t_threadStack{};

[[noreturn]] void FailSwitch(const char* what, std::uint32_t fiber, std::uint32_t value) noexcept {
    core::Fatal("SwitchToFiber(0x%08X): %s (0x%08X); the original XAPILIB::SwitchToFiber 0x%08X would fault",
                fiber, what, value, guest::kXapiSwitchToFiber);
}

void CrashReporter() noexcept {
    LogFiberSummary();
}

}  // namespace

FiberStats GetFiberStats() noexcept {
    return FiberStats{g_switches.load(), g_toThreadFiber.load(), g_chainFixups.load(),
                      g_unknownStacks.load()};
}

std::uint64_t FiberSwitchCount() noexcept {
    return g_switches.load();
}

void LogFiberSummary() noexcept {
    const FiberStats f = GetFiberStats();
    const krnl::KernelStackStats k = krnl::GetKernelStackStats();
    FUT_LOGI(
        Xapi,
        "fiber summary: %llu switches (%llu to the thread fiber, %llu ExceptionList fixups, %llu unknown "
        "stacks); kernel stacks: %u created, %u deleted, %u live (peak %u), %u failed",
        static_cast<unsigned long long>(f.switches), static_cast<unsigned long long>(f.toThreadFiber),
        static_cast<unsigned long long>(f.chainFixups), static_cast<unsigned long long>(f.unknownStacks),
        k.created, k.deleted, k.live, k.peakLive, k.failed);
}

// Exit reporter (core/host_main.h): the fiber summary at every orderly process end (M2 exit 5).
const bool g_exitReporterRegistered = (core::AddExitReporter("fibers", &LogFiberSummary), true);

namespace detail {
guest::GuestAddr SetTlsIndexAddressForTests(guest::GuestAddr address) noexcept {
    return g_tlsIndexVar.exchange(address);
}
}  // namespace detail

}  // namespace xapi

// C++ part of the SwitchToFiber replacement (called from asm/fiber_switch.asm on the CURRENT fiber's
// stack, below its saved context). cdecl, 3 arguments: new fiber, saved ESP of the current fiber,
// out target. Guest accesses in the order of the original 0x17094F: [_tls_index], S_TLS slot
// (patched fs:[4] -> fs:[0xE10+4k]), [anchor + _tls_index*4] = TLS block, [block+0xC] = current
// fiber, [current+0xC] = saved ESP, [new+0xC] -> ESP, [block+0xC] = new.
extern "C" void __cdecl fut_fiber_switch_prepare(std::uint32_t fiber, std::uint32_t savedEsp,
                                                 xapi::detail::FiberSwitchTarget* out) noexcept {
    using namespace xapi;
    const std::int32_t tlsIndex = guest::Read<std::int32_t>(g_tlsIndexVar.load(std::memory_order_relaxed));
    const std::uint32_t anchor = static_cast<std::uint32_t>(
        __readfsdword(FUT_TEB_TLS_SLOTS_OFFSET + 4 * core::TlsSlotIndex(FUT_TLS_S_TLS)));
    if (anchor == 0) {
        FailSwitch("calling thread has no guest TLS anchor (not adopted)", fiber, anchor);
    }
    const std::uint32_t block =
        guest::Read<std::uint32_t>(anchor + static_cast<std::uint32_t>(tlsIndex) * 4u);
    if (block == 0) {
        FailSwitch("TLS block pointer is NULL (_tls_index not set?)", fiber, anchor);
    }
    const std::uint32_t current = guest::Read<std::uint32_t>(block + kTlsBlockCurrentFiber);
    if (current == 0) {
        FailSwitch("no current fiber (ConvertThreadToFiber not called)", fiber, block);
    }
    if (fiber == 0) {
        FailSwitch("NULL fiber", fiber, current);
    }

    // The original's two stores and one load (0x170974, 0x170977, 0x17097A).
    guest::Write<std::uint32_t>(current + kFiberSavedEsp, savedEsp);
    const std::uint32_t newEsp = guest::Read<std::uint32_t>(fiber + kFiberSavedEsp);
    guest::Write<std::uint32_t>(block + kTlsBlockCurrentFiber, fiber);

    // Leaving the thread fiber: remember the thread's real TEB stack values.
    if (guest::Read<std::uint32_t>(current + kFiberStackLimit) == 0) {
        t_threadStack = ThreadStack{true, static_cast<std::uint32_t>(__readfsdword(kTebStackBase)),
                                    static_cast<std::uint32_t>(__readfsdword(kTebStackLimit)),
                                    static_cast<std::uint32_t>(__readfsdword(kTebDeallocationStack))};
    }

    detail::FiberSwitchTarget target{newEsp, 0, 0, 0};
    const char* kind = "fiber";
    if (guest::Read<std::uint32_t>(fiber + kFiberStackLimit) == 0) {
        // ConvertThreadToFiber fiber ([+8] = 0): back to the thread's own stack.
        kind = "thread fiber";
        if (!t_threadStack.valid) {
            // Switch from the thread fiber to itself before any other switch: values are current.
            t_threadStack = ThreadStack{true, static_cast<std::uint32_t>(__readfsdword(kTebStackBase)),
                                        static_cast<std::uint32_t>(__readfsdword(kTebStackLimit)),
                                        static_cast<std::uint32_t>(__readfsdword(kTebDeallocationStack))};
        }
        target.stackBase = t_threadStack.stackBase;
        target.stackLimit = t_threadStack.stackLimit;
        target.deallocationStack = t_threadStack.deallocationStack;
        g_toThreadFiber.fetch_add(1, std::memory_order_relaxed);
    } else {
        krnl::KernelStackInfo info{};
        const std::uint32_t top = guest::Read<std::uint32_t>(fiber + kFiberStackBase);
        if (krnl::FindKernelStack(top, &info)) {
            target.stackBase = info.stackBase;
            target.stackLimit = info.stackLimit;
            target.deallocationStack = info.allocationBase;
            // Fresh fiber from CreateFiber (0x170931: saved ExceptionList = -1): start its chain with the
            // fiber-top record -> terminal record instead (SEHOP, mm_kernel_stack.h).
            if (guest::Read<std::uint32_t>(newEsp + kSavedContextExceptionList) == kExceptionChainEnd) {
                guest::Write<std::uint32_t>(newEsp + kSavedContextExceptionList, info.exceptionList);
                g_chainFixups.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            // Not an MmCreateKernelStack stack: use the fiber's own fields (CreateFiber layout).
            kind = "fiber (unknown stack)";
            target.stackBase = top;
            target.stackLimit = guest::Read<std::uint32_t>(fiber + kFiberStackLimit);
            target.deallocationStack = target.stackLimit;
            if (g_unknownStacks.fetch_add(1, std::memory_order_relaxed) == 0) {
                FUT_LOGW(
                    Xapi,
                    "SwitchToFiber(0x%08X): stack top 0x%08X is not from MmCreateKernelStack - using the "
                    "fiber's StackBase/StackLimit, no SEH chain fixup",
                    fiber, top);
            }
        }
    }
    if (newEsp < target.stackLimit || newEsp + kSavedContextBytes + 4 > target.stackBase) {
        core::Fatal("SwitchToFiber(0x%08X): saved ESP 0x%08X outside the %s stack 0x%08X..0x%08X", fiber,
                    newEsp, kind, target.stackLimit, target.stackBase);
    }

    const std::uint64_t n = g_switches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        if (!g_reporterAdded.exchange(true)) {
            core::AddCrashReporter("fibers", &CrashReporter);
        }
        FUT_LOGI(Xapi,
                 "fiber: first SwitchToFiber 0x%08X -> 0x%08X (%s, esp 0x%08X, TEB stack 0x%08X..0x%08X, "
                 "DeallocationStack 0x%08X)",
                 current, fiber, kind, newEsp, target.stackLimit, target.stackBase, target.deallocationStack);
    } else if (core::LogEnabled(core::Channel::Xapi, core::Level::Trace)) {
        FUT_LOGT(Xapi, "fiber: switch #%llu 0x%08X -> 0x%08X (%s, esp 0x%08X, stack 0x%08X..0x%08X)",
                 static_cast<unsigned long long>(n), current, fiber, kind, newEsp, target.stackLimit,
                 target.stackBase);
    }
    *out = target;
}

namespace xapi {

// XAPILIB::SwitchToFiber 0x17094F (63 bytes), stdcall 1 argument (lpFiber), ret 4, no return value.
// Replacement: fut_switch_to_fiber (asm/fiber_switch.asm). Evidence: disassembly 0x17094F..0x17098B,
// startup.md 4.2, design 1.7 / E9 / Anhang B. Expected bytes: `mov edx,[0x2F3D18]` (the following
// fs:[4] load is an fs patch site, so only the first 6 bytes are compared).
FUT_HOOK_EX(guest::kXapiSwitchToFiber, &fut_switch_to_fiber, Stdcall, "XAPILIB::SwitchToFiber",
            "8B 15 18 3D 2F 00", true);

}  // namespace xapi
