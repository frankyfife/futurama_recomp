// KeSetEvent / KeWaitForSingleObject (slots 4, 5; plan "Defensiv") and timeout conversion.
// Owned by the kernel agent.
//
// Design 1.7: only the XAPI shutdown routine sub_0f9c7f (waits on the IDE-idle KEVENT 0x2D7540) and
// its callback sub_0f9c63 (KeSetEvent) use these, and they never run because
// HalRegisterShutdownNotification is a no-op (kernel.md 8.7). The minimal implementation keeps a map
// guest dispatcher object -> host event, created from the guest DISPATCHER_HEADER (Type, SignalState)
// on first use; SignalState is mirrored back. APC delivery during waits is M2 (design 1.7).
#include "core/guest.h"
#include "core/log.h"
#include "kernel/ke_types.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "kernel/kernel_hle.h"

#include <unordered_map>

namespace krnl {
namespace {

using guestdef::DISPATCHER_HEADER;

struct HostEvent {
    HANDLE handle;
    bool autoReset;
};

SRWLOCK g_eventLock = SRWLOCK_INIT;

std::unordered_map<std::uint32_t, HostEvent>& Events() {
    static auto* map = new std::unordered_map<std::uint32_t, HostEvent>();  // process lifetime
    return *map;
}

bool IsSynchronizationType(std::uint8_t type) noexcept {
    return type == guestdef::kEventSynchronizationObject || type == guestdef::kTimerSynchronizationObject;
}

// Host event for a guest dispatcher object (created from the guest header on first use).
HostEvent GetHostEvent(DISPATCHER_HEADER* header) {
    const std::uint32_t key = guest::Addr(header);
    AcquireSRWLockExclusive(&g_eventLock);
    HostEvent result{};
    try {
        auto& map = Events();
        auto it = map.find(key);
        if (it == map.end()) {
            const bool autoReset = IsSynchronizationType(header->Type);
            HANDLE h = CreateEventW(nullptr, autoReset ? FALSE : TRUE, header->SignalState > 0 ? TRUE : FALSE,
                                    nullptr);
            it = map.emplace(key, HostEvent{h, autoReset}).first;
            FUT_LOGD(Krnl, "dispatcher object %p: type %u, SignalState %ld -> host event %p",
                     static_cast<void*>(header), header->Type, static_cast<long>(header->SignalState), h);
        }
        result = it->second;
    } catch (...) {
        ReleaseSRWLockExclusive(&g_eventLock);
        throw;
    }
    ReleaseSRWLockExclusive(&g_eventLock);
    return result;
}

}  // namespace

std::uint32_t TimeoutToMilliseconds(const xbox::LARGE_INTEGER* timeout) noexcept {
    if (timeout == nullptr) {
        return INFINITE;
    }
    std::int64_t interval = timeout->QuadPart;
    if (interval == 0) {
        return 0;
    }
    if (interval == INT64_MIN) {
        return INFINITE;  // XAPI SleepEx(INFINITE) passes {0, 0x80000000} (0xF64D6..0xF64E0)
    }
    if (interval < 0) {
        interval = -interval;
    } else {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const std::int64_t now = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        interval -= now;
        if (interval <= 0) {
            return 0;
        }
    }
    const std::int64_t ms = (interval + 9999) / 10000;
    return ms >= static_cast<std::int64_t>(INFINITE) ? INFINITE - 1 : static_cast<std::uint32_t>(ms);
}

// KeSetEvent - slot 4, Ord 145, stdcall, 3 args (purge 12):
// LONG (PRKEVENT Event, KPRIORITY Increment, BOOLEAN Wait) -> previous SignalState.
// Evidence: kernel_imports.tsv; XAPI sub_0f9c63: KeSetEvent(0x2D7540, 1, 0) (kernel.md 8.7,
// Verifikation); design 1.7 / Anhang A "Defensiv".
LONG __stdcall KeSetEvent(DISPATCHER_HEADER* Event, LONG Increment, BOOLEAN Wait) noexcept {
    if (Event == nullptr) {
        FUT_LOGE(Krnl, "KeSetEvent(NULL)");
        return 0;
    }
    LONG previous = 0;
    FUT_HLE_TRY {
        const HostEvent ev = GetHostEvent(Event);
        previous = Event->SignalState;
        Event->SignalState = 1;
        SetEvent(ev.handle);
        if (Wait) {
            FUT_LOGW(Krnl, "KeSetEvent(%p, Wait=TRUE): the following wait is not atomic in the HLE",
                     static_cast<void*>(Event));
        }
    }
    FUT_HLE_CATCH("KeSetEvent")
    FUT_LOGD(Krnl, "KeSetEvent(%p, %ld, %u) -> previous %ld", static_cast<void*>(Event),
             static_cast<long>(Increment), static_cast<unsigned>(Wait), static_cast<long>(previous));
    return previous;
}
KRNL_FUNC(KeSetEvent, &KeSetEvent);

// KeWaitForSingleObject - slot 5, Ord 159, stdcall, 5 args (purge 20):
// NTSTATUS (PVOID Object, KWAIT_REASON WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
// PLARGE_INTEGER Timeout OPTIONAL). Evidence: kernel_imports.tsv; XAPI sub_0f9c7f 0xF9CAA..0xF9CB4:
// KeWaitForSingleObject(0x2D7540, 0, 1, FALSE, NULL) only if IdexChannelObject+0x20 != 0 (never with
// the zeroed object); design 1.7 / Anhang A "Defensiv". Returns STATUS_SUCCESS or STATUS_TIMEOUT.
NTSTATUS __stdcall KeWaitForSingleObject(DISPATCHER_HEADER* Object, ULONG WaitReason, CHAR WaitMode,
                                         BOOLEAN Alertable, xbox::LARGE_INTEGER* Timeout) noexcept {
    if (Object == nullptr) {
        FUT_LOGE(Krnl, "KeWaitForSingleObject(NULL)");
        return STATUS_INVALID_PARAMETER;
    }
    NTSTATUS status = STATUS_SUCCESS;
    FUT_HLE_TRY {
        const HostEvent ev = GetHostEvent(Object);
        const std::uint32_t ms = TimeoutToMilliseconds(Timeout);
        if (Object->Type == guestdef::kTimerNotificationObject ||
            Object->Type == guestdef::kTimerSynchronizationObject) {
            FUT_LOGW(Krnl,
                     "KeWaitForSingleObject(%p): waiting on a timer - HLE timers never fire (design 1.7)",
                     static_cast<void*>(Object));
        }
        if (ms == INFINITE && WaitForSingleObject(ev.handle, 0) == WAIT_TIMEOUT) {
            FUT_LOGW(Krnl,
                     "KeWaitForSingleObject(%p) blocks without timeout (reason %lu, mode %d, alertable %u)",
                     static_cast<void*>(Object), WaitReason, static_cast<int>(WaitMode),
                     static_cast<unsigned>(Alertable));
        }
        const DWORD r = WaitForSingleObject(ev.handle, ms);
        if (r == WAIT_OBJECT_0) {
            if (ev.autoReset) {
                Object->SignalState = 0;
            }
            status = STATUS_SUCCESS;
        } else if (r == WAIT_TIMEOUT) {
            status = STATUS_TIMEOUT;
        } else {
            FUT_LOGE(Krnl, "KeWaitForSingleObject(%p): host wait failed (%lu)", static_cast<void*>(Object),
                     GetLastError());
            status = STATUS_UNSUCCESSFUL;
        }
    }
    FUT_HLE_CATCH("KeWaitForSingleObject")
    FUT_LOGD(Krnl, "KeWaitForSingleObject(%p, %lu, %d, %u, %p) -> 0x%08lX", static_cast<void*>(Object),
             WaitReason, static_cast<int>(WaitMode), static_cast<unsigned>(Alertable),
             static_cast<void*>(Timeout), static_cast<unsigned long>(status));
    return status;
}
KRNL_FUNC(KeWaitForSingleObject, &KeWaitForSingleObject);

}  // namespace krnl
