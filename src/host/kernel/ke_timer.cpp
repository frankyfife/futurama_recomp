// Timers, DPCs and system time: KeInitializeTimerEx, KeInitializeDpc, KeSetTimer, KeCancelTimer,
// KeQuerySystemTime (slots 6, 7, 60, 61, 23). Owned by the kernel agent.
//
// Design 1.7: KeInitializeDpc / KeInitializeTimerEx do NOT fill the guest structures; they create
// host records in maps (guest address -> host). KeSetTimer only remembers the timer and NEVER fires
// it: the only non-library user is the AutoPowerDown timer (6 h, kernel.md 8.6) whose DPC would call
// HalInitiateShutdown, and with NV setting 0x11 = 0 it does nothing anyway. DPCs never run because
// D3D, DSOUND and XPP are replaced.
#include "core/guest.h"
#include "core/log.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"

#include <unordered_map>

namespace krnl {
namespace {

struct HostDpc {
    std::uint32_t routine;
    std::uint32_t context;
};

struct HostTimer {
    std::uint32_t type;      // TIMER_TYPE: 0 NotificationTimer, 1 SynchronizationTimer
    bool inserted;           // "in the timer queue" (KeSetTimer until KeCancelTimer)
    std::int64_t dueTime;    // last DueTime (100 ns; negative = relative)
    std::uint32_t dpc;       // last Dpc argument
    std::uint32_t setCount;  // number of KeSetTimer calls (diagnostics)
};

SRWLOCK g_timerLock = SRWLOCK_INIT;

std::unordered_map<std::uint32_t, HostDpc>& Dpcs() {
    static auto* map = new std::unordered_map<std::uint32_t, HostDpc>();  // process lifetime
    return *map;
}
std::unordered_map<std::uint32_t, HostTimer>& Timers() {
    static auto* map = new std::unordered_map<std::uint32_t, HostTimer>();  // process lifetime
    return *map;
}

const char* KnownObjectName(std::uint32_t addr) noexcept {
    if (addr == guest::kAutoPowerDownTimer) {
        return " (AutoPowerDown timer)";
    }
    if (addr == guest::kAutoPowerDownDpc) {
        return " (AutoPowerDown DPC)";
    }
    return "";
}

}  // namespace

// KeInitializeDpc - slot 7, Ord 107, stdcall, 3 args (purge 12):
// VOID (PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext).
// Evidence: kernel_imports.tsv; FUN_000f9b1a 0x0F9B2C: KeInitializeDpc(0x310340, 0xF9B08, 0)
// (AutoPowerDown; the routine ends with `ret 0x10`, kernel.md Verifikation 16); design 1.7.
void __stdcall KeInitializeDpc(void* Dpc, void* DeferredRoutine, void* DeferredContext) noexcept {
    try {
        AcquireSRWLockExclusive(&g_timerLock);
        try {
            Dpcs()[guest::Addr(Dpc)] = HostDpc{guest::Addr(DeferredRoutine), guest::Addr(DeferredContext)};
        } catch (...) {
            ReleaseSRWLockExclusive(&g_timerLock);
            throw;
        }
        ReleaseSRWLockExclusive(&g_timerLock);
        FUT_LOGD(Krnl, "KeInitializeDpc(%p%s, routine %p, context %p): host record only (design 1.7)", Dpc,
                 KnownObjectName(guest::Addr(Dpc)), DeferredRoutine, DeferredContext);
    }
    FUT_HLE_CATCH("KeInitializeDpc")
}
KRNL_FUNC(KeInitializeDpc, &KeInitializeDpc);

// KeInitializeTimerEx - slot 6, Ord 113, stdcall, 2 args (purge 8): VOID (PKTIMER Timer, TIMER_TYPE Type).
// Evidence: kernel_imports.tsv; FUN_000f9b1a: KeInitializeTimerEx(0x310360, 0) (AutoPowerDown,
// NotificationTimer); design 1.7.
void __stdcall KeInitializeTimerEx(void* Timer, ULONG Type) noexcept {
    try {
        AcquireSRWLockExclusive(&g_timerLock);
        try {
            Timers()[guest::Addr(Timer)] = HostTimer{Type, false, 0, 0, 0};
        } catch (...) {
            ReleaseSRWLockExclusive(&g_timerLock);
            throw;
        }
        ReleaseSRWLockExclusive(&g_timerLock);
        FUT_LOGD(Krnl, "KeInitializeTimerEx(%p%s, type %lu): host record only (design 1.7)", Timer,
                 KnownObjectName(guest::Addr(Timer)), Type);
    }
    FUT_HLE_CATCH("KeInitializeTimerEx")
}
KRNL_FUNC(KeInitializeTimerEx, &KeInitializeTimerEx);

// KeSetTimer - slot 60, Ord 149, stdcall, 4 DWORDs (purge 16):
// BOOLEAN (PKTIMER Timer, LARGE_INTEGER DueTime (by value, 2 DWORDs), PKDPC Dpc OPTIONAL).
// Returns TRUE if the timer was already set. Evidence: kernel_imports.tsv; kernel.md 4 (16 bytes);
// XAutoPowerDownResetTimer 0xF9AED: KeSetTimer(0x310360, 0xFFFFFFCD_B5659000 = -6 h, 0x310340);
// design 1.7: remember only, never fires.
BOOLEAN __stdcall KeSetTimer(void* Timer, xbox::LARGE_INTEGER DueTime, void* Dpc) noexcept {
    bool wasInserted = false;
    bool known = true;
    FUT_HLE_TRY {
        AcquireSRWLockExclusive(&g_timerLock);
        try {
            auto& timers = Timers();
            auto it = timers.find(guest::Addr(Timer));
            if (it == timers.end()) {
                known = false;
                it = timers.emplace(guest::Addr(Timer), HostTimer{0, false, 0, 0, 0}).first;
            }
            wasInserted = it->second.inserted;
            it->second.inserted = true;
            it->second.dueTime = DueTime.QuadPart;
            it->second.dpc = guest::Addr(Dpc);
            it->second.setCount += 1;
        } catch (...) {
            ReleaseSRWLockExclusive(&g_timerLock);
            throw;
        }
        ReleaseSRWLockExclusive(&g_timerLock);
    }
    FUT_HLE_CATCH("KeSetTimer")
    if (!known) {
        FUT_LOGW(Krnl, "KeSetTimer(%p): timer was never initialized", Timer);
    }
    const double hours = static_cast<double>(DueTime.QuadPart) / -36000000000.0;
    FUT_LOGD(Krnl,
             "KeSetTimer(%p%s, due %lld (%.2f h %s), dpc %p) -> %u: remembered, never fires (design 1.7)",
             Timer, KnownObjectName(guest::Addr(Timer)), static_cast<long long>(DueTime.QuadPart),
             hours < 0 ? -hours : hours, DueTime.QuadPart < 0 ? "relative" : "absolute", Dpc,
             wasInserted ? 1u : 0u);
    return wasInserted ? TRUE : FALSE;
}
KRNL_FUNC(KeSetTimer, &KeSetTimer);

// KeCancelTimer - slot 61, Ord 97, stdcall, 1 arg (purge 4): BOOLEAN (PKTIMER Timer) - TRUE if it
// was set. Evidence: kernel_imports.tsv (only DSOUND/XPP); kernel.md 7 group 2. Trivial with the map.
BOOLEAN __stdcall KeCancelTimer(void* Timer) noexcept {
    bool wasInserted = false;
    AcquireSRWLockExclusive(&g_timerLock);
    auto& timers = Timers();
    if (auto it = timers.find(guest::Addr(Timer)); it != timers.end()) {
        wasInserted = it->second.inserted;
        it->second.inserted = false;
    }
    ReleaseSRWLockExclusive(&g_timerLock);
    FUT_LOGD(Krnl, "KeCancelTimer(%p%s) -> %u", Timer, KnownObjectName(guest::Addr(Timer)),
             wasInserted ? 1u : 0u);
    return wasInserted ? TRUE : FALSE;
}
KRNL_FUNC(KeCancelTimer, &KeCancelTimer);

// KeQuerySystemTime - slot 23, Ord 128, stdcall, 1 arg (purge 4): VOID (PLARGE_INTEGER CurrentTime).
// Evidence: kernel_imports.tsv; XapiMapLetterToDirectory 0xF8C41 (TitleMeta timestamps, then
// NtSetInformationFile class 4), FUN_000f946d, XAPI time functions; design 1.9 (GetSystemTimeAsFileTime,
// same epoch 1601 and unit 100 ns).
void __stdcall KeQuerySystemTime(xbox::LARGE_INTEGER* CurrentTime) noexcept {
    if (CurrentTime == nullptr) {
        FUT_LOGE(Krnl, "KeQuerySystemTime(NULL)");
        return;
    }
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    CurrentTime->LowPart = ft.dwLowDateTime;
    CurrentTime->HighPart = static_cast<LONG>(ft.dwHighDateTime);
    FUT_LOGT(Krnl, "KeQuerySystemTime() -> 0x%08lX%08lX", ft.dwHighDateTime, ft.dwLowDateTime);
}
KRNL_FUNC(KeQuerySystemTime, &KeQuerySystemTime);

}  // namespace krnl
