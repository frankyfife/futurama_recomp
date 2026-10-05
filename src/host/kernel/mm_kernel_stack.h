// HLE kernel stacks for XAPI fibers: MmCreateKernelStack / MmDeleteKernelStack (slots 107/108) and
// the bookkeeping the SwitchToFiber replacement reads (design 1.7 "Fibers", E9, R2; m1_report 9.3).
// Owned by the fiber agent.
//
// Layout of one stack (all addresses below 0x80000000, design 1.2):
//
//   allocationBase              guard page (PAGE_READWRITE | PAGE_GUARD) = TEB.DeallocationStack
//   stackLimit = base + 0x1000  lowest usable byte                        = TEB.NtTib.StackLimit
//   ...                         committed stack, at least max(NumberOfBytes, [kernel] fiber_stack)
//   top = stackBase - 0x20      value returned to the guest; XAPI CreateFiber 0x1708B0 puts its fiber
//                               structure at top-0x10 and the initial frame below it
//   stackBase - 0x18            fiber-top SEH record {Next = terminal, Handler = host handler that
//                               calls UnhandledExceptionFilter, i.e. the host crash report}
//   stackBase - 0x10            terminal SEH record {Next = -1, Handler = ntdll's final exception
//                               handler (filled by RtlInitializeExceptionChain) or a host handler}
//   stackBase - 0x08            8 zero bytes: SEHOP accepts only records that end below
//                               StackBase - 8 (ntdll RtlpIsValidExceptionChain, see .cpp)
//   stackBase = base + size     TEB.NtTib.StackBase while a fiber on this stack runs
//
// The SwitchToFiber replacement (xapi/xapi_fiber.cpp) looks a stack up by the fiber's [+4] (= top)
// and sets TEB.NtTib.StackBase/StackLimit/DeallocationStack from it; it also replaces the initial
// ExceptionList -1 of a fresh fiber (CreateFiber 0x170931) by the fiber-top record.
#pragma once

#include "shared/win32.h"

#include <cstdint>

namespace krnl {

struct KernelStackInfo {
    std::uint32_t top;             // value MmCreateKernelStack returned (fiber [+4])
    std::uint32_t stackBase;       // TEB.NtTib.StackBase for fibers on this stack (end of allocation)
    std::uint32_t stackLimit;      // TEB.NtTib.StackLimit (above the guard page)
    std::uint32_t allocationBase;  // TEB.DeallocationStack (VirtualAlloc base, guard page)
    std::uint32_t requestedBytes;  // NumberOfBytes of the guest call
    std::uint32_t exceptionList;   // fiber-top SEH record: ExceptionList of a fresh fiber
};

inline constexpr std::uint32_t kKernelStackGuardBytes = 0x1000;
inline constexpr std::uint32_t kKernelStackTopReserve = 0x20;      // top = stackBase - 0x20
inline constexpr std::uint32_t kKernelStackFiberTopRecord = 0x18;  // record at stackBase - 0x18
inline constexpr std::uint32_t kKernelStackTerminalRecord = 0x10;  // record at stackBase - 0x10
inline constexpr std::uint32_t kDefaultFiberStackBytes = 0x80000;  // [kernel] fiber_stack default
inline constexpr std::uint32_t kMinFiberStackBytes = 0x10000;
inline constexpr std::uint32_t kMaxFiberStackBytes = 0x4000000;

// Bookkeeping lookups (thread-safe). `top` is the value MmCreateKernelStack returned.
bool FindKernelStack(std::uint32_t top, KernelStackInfo* out) noexcept;
// Stack whose committed range [stackLimit, stackBase) contains `address` (e.g. an ESP).
bool FindKernelStackContaining(std::uint32_t address, KernelStackInfo* out) noexcept;

struct KernelStackStats {
    std::uint32_t created;  // successful MmCreateKernelStack calls
    std::uint32_t deleted;  // successful MmDeleteKernelStack calls
    std::uint32_t live;     // created - deleted
    std::uint32_t peakLive;
    std::uint32_t failed;     // MmCreateKernelStack returned NULL / MmDeleteKernelStack refused
    std::uint64_t liveBytes;  // reserved bytes of the live stacks (incl. guard pages)
};
KernelStackStats GetKernelStackStats() noexcept;

// Effective minimum stack size: [kernel] fiber_stack (default 0x80000), clamped to
// kMinFiberStackBytes..kMaxFiberStackBytes and rounded up to whole pages.
std::uint32_t ConfiguredFiberStackBytes() noexcept;

// Handler stored in the terminal records (0 before the first MmCreateKernelStack). True if it is
// ntdll's final exception handler, i.e. RtlInitializeExceptionChain filled the record because SEHOP
// is enabled or in audit mode for this process.
std::uint32_t KernelStackTerminalHandler() noexcept;
bool KernelStackTerminalHandlerIsNtdll() noexcept;

// MmCreateKernelStack - slot 107, Ord 169, stdcall, 2 args (purge 8), returns PVOID.
// NumberOfBytes = CreateFiber size rounded to pages (>= 0x3000), DebuggerThread = FALSE (0x1708D2).
PVOID __stdcall MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread) noexcept;
// MmDeleteKernelStack - slot 108, Ord 170, stdcall, 2 args (purge 8), void.
// XAPI DeleteFiber 0x17093C passes fiber [+4] (= top) and [+8] (= top - size).
VOID __stdcall MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit) noexcept;

}  // namespace krnl
