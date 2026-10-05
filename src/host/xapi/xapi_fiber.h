// XAPI fibers on Windows (design 1.7 "Fibers", E9, R2; m1_report 9 item 4). Owned by the fiber agent.
//
// CreateFiber 0x1708B0, DeleteFiber 0x17093C, ConvertThreadToFiber 0x17098E and the fiber trampoline
// 0x17086B stay original guest code; their kernel stacks come from krnl::MmCreateKernelStack
// (kernel/mm_kernel_stack.h). XAPILIB::SwitchToFiber 0x17094F is detoured to fut_switch_to_fiber
// (asm/fiber_switch.asm): the same register/ExceptionList/ESP switch as the original plus
// TEB.NtTib.StackBase/StackLimit and TEB.DeallocationStack, so that Windows dispatches SEH (guest
// __try, IsBadReadPtr, host C++ EH of HLE code) on fiber stacks, and the SEHOP-compatible chain end.
#pragma once

#include "core/guest.h"

#include <cstdint>

namespace xapi {

// Guest fiber structure (startup.md 4.2, CreateFiber 0x1708F5..0x170934, ConvertThreadToFiber
// 0x1709A1..0x1709B9, SwitchToFiber 0x17096E..0x17097A):
//   +0x00 lpParameter (read by the trampoline 0x170890)
//   +0x04 StackBase   (CreateFiber: top from MmCreateKernelStack; thread fiber: KTHREAD+0x1C)
//   +0x08 StackLimit  (CreateFiber: top - size; thread fiber: 0 - identifies ConvertThreadToFiber)
//   +0x0C saved ESP   (points at the saved context {ExceptionList, ebx, edi, esi, ebp, return})
// TLS block (startup.md 6): +0x0C current fiber, +0x10..+0x1B fiber structure of the thread.
inline constexpr std::uint32_t kFiberParameter = 0x00;
inline constexpr std::uint32_t kFiberStackBase = 0x04;
inline constexpr std::uint32_t kFiberStackLimit = 0x08;
inline constexpr std::uint32_t kFiberSavedEsp = 0x0C;
inline constexpr std::uint32_t kTlsBlockCurrentFiber = 0x0C;
inline constexpr std::uint32_t kTlsBlockThreadFiber = 0x10;
// Saved context of a suspended fiber, from its saved ESP upwards (push order 0x170960..0x170964).
inline constexpr std::uint32_t kSavedContextExceptionList = 0x00;
inline constexpr std::uint32_t kSavedContextBytes = 0x14;  // ExceptionList, ebx, edi, esi, ebp

struct FiberStats {
    std::uint64_t switches;          // SwitchToFiber calls (replacement)
    std::uint64_t toThreadFiber;     // ... whose target was a ConvertThreadToFiber fiber
    std::uint64_t chainFixups;       // fresh fibers whose ExceptionList -1 became the fiber-top record
    std::uint64_t unknownStacks;     // targets whose stack is not from MmCreateKernelStack (fallback)
    std::uint64_t selfTestSwitches;  // (M2 review) switches made while a self-test ran (core::InSelfTest)
};
FiberStats GetFiberStats() noexcept;
std::uint64_t FiberSwitchCount() noexcept;
// Logs "fiber summary: N switches (G game, S self-test; ...)" plus the kernel stack statistics (info).
// Meant for the end of a run (M2 exit criterion 5: G > 0); also registered as a crash reporter on the
// first switch. The first switch of each kind logs "fiber: first SwitchToFiber (game|self-test) ...".
void LogFiberSummary() noexcept;

namespace detail {

// Filled by fut_fiber_switch_prepare and consumed by asm/fiber_switch.asm (offsets are fixed).
struct FiberSwitchTarget {
    std::uint32_t esp;                // new fiber's saved ESP ([fiber+0xC])
    std::uint32_t stackBase;          // -> fs:[0x04] TEB.NtTib.StackBase
    std::uint32_t stackLimit;         // -> fs:[0x08] TEB.NtTib.StackLimit
    std::uint32_t deallocationStack;  // -> fs:[0xE0C] TEB.DeallocationStack
};
static_assert(sizeof(FiberSwitchTarget) == 16);

// Address of the guest variable _tls_index (default guest::kTlsIndexVar). Unit tests that run
// without the XBE point it at a host variable holding -9; returns the previous value.
guest::GuestAddr SetTlsIndexAddressForTests(guest::GuestAddr address) noexcept;

}  // namespace detail
}  // namespace xapi

extern "C" {
// Replacement of XAPILIB::SwitchToFiber 0x17094F: VOID WINAPI SwitchToFiber(LPVOID lpFiber), stdcall,
// 1 argument, ret 4 (asm/fiber_switch.asm). Callable from host code on an adopted thread.
void __stdcall fut_switch_to_fiber(std::uint32_t fiber);
// C++ part of the switch, called by fut_switch_to_fiber with the current fiber's context already pushed
// (savedEsp). Performs the guest stores of the original ([current+0xC] = savedEsp, [TLS+0xC] = fiber)
// and fills `out`. Never returns on fatal errors (core::Fatal).
void __cdecl fut_fiber_switch_prepare(std::uint32_t fiber, std::uint32_t savedEsp,
                                      xapi::detail::FiberSwitchTarget* out) noexcept;
// Self-test helper (asm/fiber_switch.asm): calls fn(arg) (cdecl or stdcall, 1 argument) inside a guest
// SEH3 frame built by the guest's own __SEH_prolog (handler _except_handler3 0x168DCC) with one
// __try/__except(EXCEPTION_EXECUTE_HANDLER) level, exactly like MSVC 7 guest code (e.g. IsBadReadPtr
// 0x1709C9). Returns 0 if fn returned, 1 if the __except block ran. sehProlog/sehEpilog are
// guest::kCrtSehProlog / guest::kCrtSehEpilog.
std::uint32_t __cdecl fut_seh3_frame_call(std::uint32_t sehProlog, std::uint32_t sehEpilog, std::uint32_t fn,
                                          std::uint32_t arg);
}
