// Waits, user-APC delivery rule and guest event objects (io agent since M2; implementation ke_wait.cpp).
//
// The wait exports themselves (KeDelayExecutionThread slot 21, NtWaitForSingleObjectEx slot 20,
// NtWaitForSingleObject slot 33) are declared in kernel/kernel_exports.h.
//
// Event objects: NtCreateEvent is not imported by this XBE (startup.md 4.5: KEVENTs only inline in XDK
// code; NtSetEvent only from the replaced DSOUND), so the guest cannot create event handles itself.
// EventObject is the handle-table type for events created by host code (HLE of XDK libraries, tests)
// and handed to the guest - e.g. the Event parameter of NtReadFile/NtWriteFile/NtQueryDirectoryFile/
// NtDeviceIoControlFile/NtFsControlFile, which the io layer resets when the request starts and
// signals when it completes (NT semantics), and NtWaitForSingleObject(Ex) waits on it.
#pragma once

#include "kernel/ob_handles.h"

#include <cstdint>

namespace krnl {

// KPROCESSOR_MODE values passed by XAPI (SleepEx 0xF64E7 / WaitForSingleObjectEx push 1 = UserMode).
inline constexpr CHAR kKernelMode = 0;
inline constexpr CHAR kUserMode = 1;

// Design 1.7: queued I/O APCs (kernel/io_apc.h) are delivered only by an alertable user-mode wait
// (NT KiDeliverApc rule for user APCs: Alertable && WaitMode == UserMode).
constexpr bool DeliversUserApcs(CHAR waitMode, BOOLEAN alertable) noexcept {
    return alertable != FALSE && waitMode == kUserMode;
}

class EventObject final : public KObject {
public:
    // NT EVENT_TYPE: NotificationEvent (manual reset) or SynchronizationEvent (auto reset).
    enum class Type : std::uint8_t { Notification = 0, Synchronization = 1 };

    EventObject(Type type, bool signaled) noexcept;
    ~EventObject() override;
    const char* TypeName() const noexcept override { return "Event"; }

    Type EventType() const noexcept { return type_; }
    void Set() noexcept;    // KeSetEvent / NtSetEvent
    void Reset() noexcept;  // KeClearEvent / NtClearEvent
    // True if the event is signaled (consumes the signal of a synchronization event, like a wait).
    bool TryWait() noexcept;
    HANDLE HostHandle() const noexcept { return host_; }  // Win32 event with the same reset semantics

private:
    Type type_;
    HANDLE host_;
};

// Creates an event object and inserts it into the guest handle table (nullptr if out of resources).
// The guest closes it with NtClose like any other handle.
HANDLE CreateEventHandle(EventObject::Type type, bool signaled) noexcept;
// Event object behind a guest handle (null Ref for unknown handles and other object types).
Ref<EventObject> LookupEvent(HANDLE handle) noexcept;

}  // namespace krnl
