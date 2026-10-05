// MmCreateKernelStack / MmDeleteKernelStack (slots 107/108) with the stack bookkeeping used by the
// SwitchToFiber replacement (design 1.7 "Fibers", E9, R2). Owned by the fiber agent; layout in
// mm_kernel_stack.h.
//
// Evidence:
//  - kernel_imports.tsv rows 169/170: both stdcall 2 args (purge 8); only caller of 107 is XAPI
//    CreateFiber 0x1708DA, only caller of 108 is XAPI DeleteFiber 0x170946.
//  - CreateFiber 0x1708B0..0x170939: size = max(dwStackSize or [0x10130], 0x3000) rounded to pages,
//    MmCreateKernelStack(size, FALSE); NULL -> SetLastError(8) and NULL; otherwise the fiber struct
//    at top-0x10 = {param, top, top - size, saved esp} and the initial frame down to top-0x34 whose
//    first DWORD (saved ExceptionList) is -1 (0x170931). StackBase/StackLimit come from the return
//    value only, so a larger HLE stack is transparent (startup.md 9 item 6).
//  - DeleteFiber 0x17093C: MmDeleteKernelStack([fiber+4], [fiber+8]).
//  - SEH on a foreign stack (experiment 2026-10-05, Windows 11 WoW64, ntdll 32-bit): RtlDispatchException
//    rejects every registration record outside TEB [StackLimit, StackBase) -> the stack limits must be
//    switched. RtlpIsValidExceptionChain (SEHOP, runs unless ProcessExecuteFlags has 0x40) demands
//    ascending records with record < StackBase - 8 (strict), handlers outside the stack, and - once
//    the thread's chain was initialised by RtlInitializeExceptionChain (TEB SameTebFlags 0x200) - a
//    last record whose handler is ntdll's FinalExceptionHandler (ntdll global). By default Windows 11
//    only audits a failure; with the SEHOP mitigation enforced a chain that ends in -1, or a terminal
//    record exactly at StackBase - 8, kills the process. RtlInitializeExceptionChain(record) writes
//    {-1, FinalExceptionHandler} unless SEHOP is disabled for the process. Without a fiber-top record
//    an unhandled exception on the stack reaches only that final handler: the process ends without
//    calling the top-level filter (no host crash report).
#include "kernel/mm_kernel_stack.h"

#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"
#include "kernel/kernel_api.h"

#include <intrin.h>

#include <atomic>
#include <unordered_map>

namespace krnl {
namespace {

constexpr std::uint32_t kPageSize = 0x1000;
constexpr std::uint32_t kExceptionChainEnd = 0xFFFFFFFFu;
// ProcessExecuteFlags (NtQueryInformationProcess class 0x22):
// MEM_EXECUTE_OPTION_DISABLE_EXCEPTION_CHAIN_VALIDATION.
constexpr ULONG kProcessExecuteFlags = 0x22;
constexpr ULONG kExecuteOptionNoChainValidation = 0x40;

struct SehRecord {
    std::uint32_t next;
    std::uint32_t handler;
};
static_assert(sizeof(SehRecord) == 8);

SRWLOCK g_lock = SRWLOCK_INIT;
KernelStackStats g_stats{};
std::atomic<std::uint32_t> g_terminalHandler{0};
std::atomic<bool> g_terminalIsNtdll{false};
std::atomic<bool> g_firstLogged{false};

std::unordered_map<std::uint32_t, KernelStackInfo>& Stacks() {
    static auto* map = new std::unordered_map<std::uint32_t, KernelStackInfo>();  // process lifetime
    return *map;
}

// Terminal record handler when SEHOP is disabled (RtlInitializeExceptionChain leaves the record):
// nothing on a fiber stack lies beyond it.
EXCEPTION_DISPOSITION __cdecl FiberTerminalSehHandler(EXCEPTION_RECORD*, void*, CONTEXT*, void*) noexcept {
    return ExceptionContinueSearch;
}

// Fiber-top SEH record handler: what RtlUserThreadStart's __except(UnhandledExceptionFilter(...)) is
// for a thread. Without it an exception no guest/host frame handles on a fiber stack would skip the
// top-level filter (core crash handler: log, backtrace, minidump, exit code 3).
EXCEPTION_DISPOSITION __cdecl FiberTopSehHandler(EXCEPTION_RECORD* record, void*, CONTEXT* context,
                                                 void*) noexcept {
    if (record == nullptr || (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) != 0) {
        return ExceptionContinueSearch;
    }
    FUT_LOGE(Krnl, "unhandled exception 0x%08lX at %p on a fiber stack (esp 0x%08lX) - top-level filter",
             record->ExceptionCode, record->ExceptionAddress, context != nullptr ? context->Esp : 0ul);
    EXCEPTION_POINTERS pointers{record, context};
    const LONG filter = UnhandledExceptionFilter(&pointers);  // calls the host's top-level filter
    if (filter == EXCEPTION_CONTINUE_EXECUTION) {
        return ExceptionContinueExecution;
    }
    if (filter == EXCEPTION_CONTINUE_SEARCH) {
        return ExceptionContinueSearch;  // e.g. a debugger is attached: second chance
    }
    core::Fatal("unhandled exception 0x%08lX at %p on a fiber stack", record->ExceptionCode,
                record->ExceptionAddress);
}

using RtlInitializeExceptionChainFn = void(NTAPI*)(void* record);
using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);

RtlInitializeExceptionChainFn InitExceptionChainFn() noexcept {
    static const auto fn = reinterpret_cast<RtlInitializeExceptionChainFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlInitializeExceptionChain"));
    return fn;
}

// Fills the terminal record exactly like a Windows thread's last record (see the evidence above).
void InitTerminalRecord(SehRecord* terminal) noexcept {
    const std::uint32_t host = guest::Addr(reinterpret_cast<const void*>(&FiberTerminalSehHandler));
    terminal->next = kExceptionChainEnd;
    terminal->handler = host;
    if (const auto init = InitExceptionChainFn(); init != nullptr) {
        // Only touches TEB.ExceptionList if it is -1, which never holds on an adopted guest thread.
        init(terminal);
    }
    terminal->next = kExceptionChainEnd;
    g_terminalHandler.store(terminal->handler);
    g_terminalIsNtdll.store(terminal->handler != host);
}

void LogFirstStack(const KernelStackInfo& info) noexcept {
    if (g_firstLogged.exchange(true)) {
        return;
    }
    ULONG executeFlags = 0;
    LONG status = -1;
    if (const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"))) {
        status =
            query(GetCurrentProcess(), kProcessExecuteFlags, &executeFlags, sizeof(executeFlags), nullptr);
    }
    const bool chainValidation = status >= 0 && (executeFlags & kExecuteOptionNoChainValidation) == 0;
    FUT_LOGI(Krnl,
             "fiber stacks: %u KiB committed + guard page; first stack 0x%08X..0x%08X (top 0x%08X); SEH "
             "terminal handler 0x%08X (%s), exception chain validation %s (ExecuteFlags 0x%lX)",
             (info.stackBase - info.stackLimit) / 1024, info.stackLimit, info.stackBase, info.top,
             g_terminalHandler.load(),
             g_terminalIsNtdll.load() ? "ntdll FinalExceptionHandler via RtlInitializeExceptionChain"
                                      : "host, SEHOP disabled",
             status < 0 ? "unknown" : (chainValidation ? "on (enforced or audit)" : "off"), executeFlags);
}

std::uint32_t RoundUpPage(std::uint64_t bytes) noexcept {
    return static_cast<std::uint32_t>((bytes + kPageSize - 1) & ~static_cast<std::uint64_t>(kPageSize - 1));
}

}  // namespace

std::uint32_t ConfiguredFiberStackBytes() noexcept {
    static const std::uint32_t bytes = [] {
        std::uint32_t v = kDefaultFiberStackBytes;
        try {
            v = core::GetConfig().GetU32("kernel", "fiber_stack", kDefaultFiberStackBytes);
        } catch (...) {
        }
        if (v < kMinFiberStackBytes || v > kMaxFiberStackBytes) {
            const std::uint32_t clamped = v < kMinFiberStackBytes ? kMinFiberStackBytes : kMaxFiberStackBytes;
            FUT_LOGW(Krnl, "[kernel] fiber_stack = 0x%X outside 0x%X..0x%X - using 0x%X", v,
                     kMinFiberStackBytes, kMaxFiberStackBytes, clamped);
            v = clamped;
        }
        return RoundUpPage(v);
    }();
    return bytes;
}

bool FindKernelStack(std::uint32_t top, KernelStackInfo* out) noexcept {
    AcquireSRWLockShared(&g_lock);
    const auto& map = Stacks();
    const auto it = map.find(top);
    const bool found = it != map.end();
    if (found && out != nullptr) {
        *out = it->second;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

bool FindKernelStackContaining(std::uint32_t address, KernelStackInfo* out) noexcept {
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    for (const auto& [top, info] : Stacks()) {
        if (address >= info.stackLimit && address < info.stackBase) {
            if (out != nullptr) {
                *out = info;
            }
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

KernelStackStats GetKernelStackStats() noexcept {
    AcquireSRWLockShared(&g_lock);
    const KernelStackStats s = g_stats;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

std::uint32_t KernelStackTerminalHandler() noexcept {
    return g_terminalHandler.load();
}

bool KernelStackTerminalHandlerIsNtdll() noexcept {
    return g_terminalIsNtdll.load();
}

// MmCreateKernelStack - slot 107, Ord 169, stdcall, 2 args (purge 8), returns PVOID (top of stack).
// Args: NumberOfBytes (page-rounded CreateFiber size), DebuggerThread (always FALSE, ignored).
// Evidence: kernel_imports.tsv; CreateFiber 0x1708C1..0x1708E4 (push 0; push size; call [0x29370C];
// NULL check only); design 1.7 / E9; layout and SEH records: mm_kernel_stack.h and the header comment.
PVOID __stdcall MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread) noexcept {
    FUT_HLE_TRY{const std::uint32_t minimum = ConfiguredFiberStackBytes();
if (NumberOfBytes > kMaxFiberStackBytes) {
    FUT_LOGE(Krnl, "MmCreateKernelStack(0x%lX, %u): size above 0x%X - returning NULL", NumberOfBytes,
             DebuggerThread, kMaxFiberStackBytes);
    AcquireSRWLockExclusive(&g_lock);
    ++g_stats.failed;
    ReleaseSRWLockExclusive(&g_lock);
    return nullptr;
}
const std::uint32_t usable = RoundUpPage(NumberOfBytes > minimum ? NumberOfBytes : minimum);
const std::uint32_t total = usable + kKernelStackGuardBytes;
void* base = VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
if (base == nullptr || static_cast<std::uint64_t>(guest::Addr(base)) + total > guest::kGuestMemoryLimit) {
    FUT_LOGE(Krnl, "MmCreateKernelStack(0x%lX): VirtualAlloc(0x%X) %s (error %lu)", NumberOfBytes, total,
             base == nullptr ? "failed" : "returned memory above 0x80000000", GetLastError());
    if (base != nullptr) {
        VirtualFree(base, 0, MEM_RELEASE);
    }
    AcquireSRWLockExclusive(&g_lock);
    ++g_stats.failed;
    ReleaseSRWLockExclusive(&g_lock);
    return nullptr;
}
DWORD oldProtect = 0;
if (!VirtualProtect(base, kKernelStackGuardBytes, PAGE_READWRITE | PAGE_GUARD, &oldProtect)) {
    FUT_LOGW(Krnl, "MmCreateKernelStack: guard page at %p not set (error %lu)", base, GetLastError());
}
KernelStackInfo info{};
info.allocationBase = guest::Addr(base);
info.stackBase = info.allocationBase + total;
info.stackLimit = info.allocationBase + kKernelStackGuardBytes;
info.top = info.stackBase - kKernelStackTopReserve;
info.requestedBytes = NumberOfBytes;
info.exceptionList = info.stackBase - kKernelStackFiberTopRecord;

auto* terminal = guest::Ptr<SehRecord>(info.stackBase - kKernelStackTerminalRecord);
InitTerminalRecord(terminal);
auto* fiberTop = guest::Ptr<SehRecord>(info.exceptionList);
fiberTop->next = guest::Addr(terminal);
fiberTop->handler = guest::Addr(reinterpret_cast<const void*>(&FiberTopSehHandler));

AcquireSRWLockExclusive(&g_lock);
try {
    Stacks()[info.top] = info;
} catch (...) {
    ReleaseSRWLockExclusive(&g_lock);
    VirtualFree(base, 0, MEM_RELEASE);
    throw;
}
++g_stats.created;
++g_stats.live;
g_stats.liveBytes += total;
if (g_stats.live > g_stats.peakLive) {
    g_stats.peakLive = g_stats.live;
}
ReleaseSRWLockExclusive(&g_lock);

LogFirstStack(info);
FUT_LOGD(Krnl,
         "MmCreateKernelStack(0x%lX, %u) -> top 0x%08X (stack 0x%08X..0x%08X, guard 0x%08X, SEH "
         "0x%08X -> 0x%08X)",
         NumberOfBytes, DebuggerThread, info.top, info.stackLimit, info.stackBase, info.allocationBase,
         info.exceptionList, guest::Addr(terminal));
return guest::Ptr<void>(info.top);
}  // namespace krnl
FUT_HLE_CATCH("MmCreateKernelStack")
}
KRNL_FUNC(MmCreateKernelStack, &MmCreateKernelStack);

// MmDeleteKernelStack - slot 108, Ord 170, stdcall, 2 args (purge 8), void.
// Args: StackBase = fiber [+4] (our top), StackLimit = fiber [+8] (top - CreateFiber size).
// Evidence: kernel_imports.tsv; DeleteFiber 0x17093C..0x17094C; design 1.7.
VOID __stdcall MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit) noexcept {
    FUT_HLE_TRY{const std::uint32_t top = guest::Addr(StackBase);
const std::uint32_t esp = guest::Addr(_AddressOfReturnAddress());
AcquireSRWLockExclusive(&g_lock);
auto& map = Stacks();
const auto it = map.find(top);
if (it == map.end() || (esp >= it->second.allocationBase && esp < it->second.stackBase)) {
    const bool running = it != map.end();
    ++g_stats.failed;
    ReleaseSRWLockExclusive(&g_lock);
    FUT_LOGE(Krnl, "MmDeleteKernelStack(%p, %p): %s - not freed", StackBase, StackLimit,
             running ? "the calling fiber runs on this stack" : "not a stack from MmCreateKernelStack");
    return;
}
const KernelStackInfo info = it->second;
map.erase(it);
++g_stats.deleted;
--g_stats.live;
g_stats.liveBytes -= info.stackBase - info.allocationBase;
ReleaseSRWLockExclusive(&g_lock);
if (guest::Addr(StackLimit) != info.top - info.requestedBytes) {
    FUT_LOGD(Krnl, "MmDeleteKernelStack: StackLimit %p differs from top - requested size 0x%08X", StackLimit,
             info.top - info.requestedBytes);
}
if (!VirtualFree(guest::Ptr<void>(info.allocationBase), 0, MEM_RELEASE)) {
    FUT_LOGE(Krnl, "MmDeleteKernelStack: VirtualFree(0x%08X) failed (error %lu)", info.allocationBase,
             GetLastError());
    return;
}
FUT_LOGD(Krnl, "MmDeleteKernelStack(%p, %p): freed 0x%08X..0x%08X", StackBase, StackLimit,
         info.allocationBase, info.stackBase);
}
FUT_HLE_CATCH("MmDeleteKernelStack")
}
KRNL_FUNC(MmDeleteKernelStack, &MmDeleteKernelStack);

}  // namespace krnl
