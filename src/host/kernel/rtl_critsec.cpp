// RtlInitialize/Enter/LeaveCriticalSection (slots 14, 10, 9). Owned by the kernel agent.
//
// Design 1.7: the lock itself is a host CRITICAL_SECTION found through a map guest address -> host
// object, created lazily for addresses never passed to RtlInitializeCriticalSection (statically
// initialized guest locks, e.g. the XAPI thread-notify lock 0x2D70D8 used by
// XapiCallThreadNotifyRoutines/XRegisterThreadNotifyRoutine). Nothing depends on the Xbox layout;
// the guest fields are only initialized/mirrored (design risk R5; no direct field reads were found
// in XAPI/LIBC callers).
#include "core/guest.h"
#include "core/guest_thread.h"
#include "core/log.h"
#include "kernel/ke_types.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"

#include <unordered_map>

namespace krnl {
namespace {

using GuestCs = guestdef::RTL_CRITICAL_SECTION;

SRWLOCK g_csLock = SRWLOCK_INIT;

std::unordered_map<std::uint32_t, CRITICAL_SECTION*>& CsMap() {
    static auto* map = new std::unordered_map<std::uint32_t, CRITICAL_SECTION*>();  // process lifetime
    return *map;
}

CRITICAL_SECTION* NewHostCs() {
    auto* cs = new CRITICAL_SECTION;
    InitializeCriticalSectionEx(cs, 0, CRITICAL_SECTION_NO_DEBUG_INFO);
    return cs;
}

CRITICAL_SECTION* FindHostCs(std::uint32_t key) noexcept {
    AcquireSRWLockShared(&g_csLock);
    CRITICAL_SECTION* cs = nullptr;
    const auto& map = CsMap();
    if (auto it = map.find(key); it != map.end()) {
        cs = it->second;
    }
    ReleaseSRWLockShared(&g_csLock);
    return cs;
}

// Returns the host lock for `key`, creating it on first use. Throws std::bad_alloc.
CRITICAL_SECTION* GetOrCreateHostCs(std::uint32_t key, bool reinitialize, bool* created) {
    if (!reinitialize) {
        if (CRITICAL_SECTION* cs = FindHostCs(key)) {
            *created = false;
            return cs;
        }
    }
    CRITICAL_SECTION* fresh = NewHostCs();
    AcquireSRWLockExclusive(&g_csLock);
    CRITICAL_SECTION* result = fresh;
    try {
        auto& map = CsMap();
        auto [it, inserted] = map.try_emplace(key, fresh);
        if (!inserted) {
            if (reinitialize) {
                // The old host object is intentionally leaked: a guest that re-initializes a lock it
                // still holds must not crash the host (ASSUMPTION: rare, at most a few objects).
                it->second = fresh;
            } else {
                result = it->second;  // another thread created it first
            }
        }
        *created = (result == fresh);
    } catch (...) {
        ReleaseSRWLockExclusive(&g_csLock);
        DeleteCriticalSection(fresh);
        delete fresh;
        throw;
    }
    ReleaseSRWLockExclusive(&g_csLock);
    if (result != fresh) {
        DeleteCriticalSection(fresh);
        delete fresh;
    }
    return result;
}

bool HostOwnsCs(const CRITICAL_SECTION* cs) noexcept {
    return static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(cs->OwningThread)) == GetCurrentThreadId();
}

}  // namespace

// RtlInitializeCriticalSection - slot 14, Ord 291, stdcall, 1 arg (purge 4): VOID (PRTL_CRITICAL_SECTION).
// Evidence: kernel_imports.tsv; callers XAPI heap creation FUN_000f4ea8 (lock behind heap+0x580) and
// LIBC FUN_0016fd04 (CRT locks, 0x1C-byte malloc in FUN_0016ebfd); design 1.7 (map-based).
// ASSUMPTION: guest fields initialized like the Xbox kernel (KeInitializeEvent(SynchronizationEvent,
// FALSE), LockCount -1, RecursionCount 0, OwningThread 0).
void __stdcall RtlInitializeCriticalSection(GuestCs* CriticalSection) noexcept {
    if (CriticalSection == nullptr) {
        FUT_LOGE(Krnl, "RtlInitializeCriticalSection(NULL)");
        return;
    }
    FUT_HLE_TRY {
        CriticalSection->Event.Type = guestdef::kEventSynchronizationObject;
        CriticalSection->Event.Absolute = 0;
        CriticalSection->Event.Size = sizeof(guestdef::DISPATCHER_HEADER) / 4;
        CriticalSection->Event.Inserted = 0;
        CriticalSection->Event.SignalState = 0;
        CriticalSection->Event.WaitListHead.Flink = &CriticalSection->Event.WaitListHead;
        CriticalSection->Event.WaitListHead.Blink = &CriticalSection->Event.WaitListHead;
        CriticalSection->LockCount = -1;
        CriticalSection->RecursionCount = 0;
        CriticalSection->OwningThread = 0;
        bool created = false;
        GetOrCreateHostCs(guest::Addr(CriticalSection), true, &created);
        FUT_LOGT(Krnl, "RtlInitializeCriticalSection(%p)%s", static_cast<void*>(CriticalSection),
                 created ? "" : " (re-initialized)");
    }
    FUT_HLE_CATCH("RtlInitializeCriticalSection")
}
KRNL_FUNC(RtlInitializeCriticalSection, &RtlInitializeCriticalSection);

// RtlEnterCriticalSection - slot 10, Ord 277, stdcall, 1 arg (purge 4): VOID (PRTL_CRITICAL_SECTION).
// Evidence: kernel_imports.tsv; 12 sites: XAPI heap FUN_000f52cc/FUN_000f5a83/FUN_000f5c77, thread
// notify 0xF98B5/0xF9955 (static lock 0x2D70D8), LIBC _lock/_lock_fhandle (FUN_0016ec79,
// FUN_0016eca7), DSOUND; design 1.7.
void __stdcall RtlEnterCriticalSection(GuestCs* CriticalSection) noexcept {
    if (CriticalSection == nullptr) {
        FUT_LOGE(Krnl, "RtlEnterCriticalSection(NULL)");
        return;
    }
    FUT_HLE_TRY {
        bool created = false;
        CRITICAL_SECTION* cs = GetOrCreateHostCs(guest::Addr(CriticalSection), false, &created);
        if (created) {
            FUT_LOGD(Krnl,
                     "RtlEnterCriticalSection(%p): lock was never initialized - created lazily (design 1.7)",
                     static_cast<void*>(CriticalSection));
        }
        EnterCriticalSection(cs);
        // Mirror the Xbox fields (we own the lock now).
        CriticalSection->RecursionCount += 1;
        CriticalSection->LockCount = CriticalSection->RecursionCount - 1;
        CriticalSection->OwningThread = core::GetGuestSlot(FUT_TLS_S_KTHREAD);
    }
    FUT_HLE_CATCH("RtlEnterCriticalSection")
}
KRNL_FUNC(RtlEnterCriticalSection, &RtlEnterCriticalSection);

// RtlLeaveCriticalSection - slot 9, Ord 294, stdcall, 1 arg (purge 4): VOID (PRTL_CRITICAL_SECTION).
// Evidence: kernel_imports.tsv (108 sites, 96 in DSOUND); XAPI heap, thread notify, LIBC _unlock
// (FUN_0016ebeb) and __unlock_fhandle 0x16F76F; design 1.7.
void __stdcall RtlLeaveCriticalSection(GuestCs* CriticalSection) noexcept {
    if (CriticalSection == nullptr) {
        FUT_LOGE(Krnl, "RtlLeaveCriticalSection(NULL)");
        return;
    }
    CRITICAL_SECTION* cs = FindHostCs(guest::Addr(CriticalSection));
    if (cs == nullptr || !HostOwnsCs(cs)) {
        FUT_LOGE(Krnl, "RtlLeaveCriticalSection(%p): lock %s by the calling thread - ignored",
                 static_cast<void*>(CriticalSection), cs == nullptr ? "unknown" : "not owned");
        return;
    }
    CriticalSection->RecursionCount -= 1;
    if (CriticalSection->RecursionCount <= 0) {
        CriticalSection->RecursionCount = 0;
        CriticalSection->LockCount = -1;
        CriticalSection->OwningThread = 0;
    } else {
        CriticalSection->LockCount = CriticalSection->RecursionCount - 1;
    }
    LeaveCriticalSection(cs);
}
KRNL_FUNC(RtlLeaveCriticalSection, &RtlLeaveCriticalSection);

}  // namespace krnl
