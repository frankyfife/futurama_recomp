// Alertable waits: KeDelayExecutionThread, NtWaitForSingleObject, NtWaitForSingleObjectEx
// (slots 21, 33, 20; plan M2, implemented with the io agent's APC queue). Owned by the kernel agent.
//
// Design 1.7 "APCs": every guest thread has its own guest APC queue (kernel/io_apc.h, io agent).
// I/O APCs are delivered only in KeDelayExecutionThread(.., Alertable = TRUE, ..) and
// NtWaitForSingleObjectEx(.., Alertable = TRUE, ..); afterwards the wait returns STATUS_USER_APC (0xC0)
// which XAPI SleepEx 0xF64BE turns into WAIT_IO_COMPLETION. Contract: io_apc.h header comment.
// Alertable waits of the game: SleepEx(0, TRUE) at 0x13F4C0, 0x13F590 and 0xE0030 (files.md 2.4).
#include "core/guest.h"
#include "core/log.h"
#include "kernel/io_apc.h"
#include "kernel/ke_thread.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "kernel/kernel_hle.h"
#include "kernel/ob_handles.h"
#include "shared/nt_status.h"

namespace krnl {
namespace {

// Delivers queued I/O APCs of the calling thread (alertable waits only).
bool DeliverApcs(BOOLEAN alertable) noexcept {
    return alertable && io::DeliverIoApcs() > 0;
}

NTSTATUS WaitOnHandle(HANDLE handle, BOOLEAN alertable, const xbox::LARGE_INTEGER* timeout,
                      const char* api) noexcept {
    if (DeliverApcs(alertable)) {
        return STATUS_USER_APC;
    }
    const std::uint32_t ms = TimeoutToMilliseconds(timeout);
    Ref<KObject> object = HandleTable::Instance().Lookup(handle);
    if (!object) {
        FUT_LOGW(Krnl, "%s(%p): invalid handle -> STATUS_INVALID_HANDLE", api, handle);
        return STATUS_INVALID_HANDLE;
    }
    NTSTATUS status = STATUS_SUCCESS;
    if (auto* thread = dynamic_cast<ThreadObject*>(object.get())) {
        const DWORD r = WaitForSingleObject(thread->HostHandle(), ms);
        status = r == WAIT_OBJECT_0  ? STATUS_SUCCESS
                 : r == WAIT_TIMEOUT ? STATUS_TIMEOUT
                                     : STATUS_UNSUCCESSFUL;
    } else {
        // File objects: the HLE completes every transfer before NtReadFile/NtWriteFile return, so the
        // file object is always signaled (XAPI ReadFile/WriteFile 0xF723B/0xF731C only wait after
        // STATUS_PENDING on synchronous handles, which the io layer never returns).
        FUT_LOGT(Krnl, "%s(%p %s): object is always signaled in the HLE", api, handle, object->TypeName());
    }
    if (status == STATUS_SUCCESS || status == STATUS_TIMEOUT) {
        if (DeliverApcs(alertable)) {
            status = STATUS_USER_APC;
        }
    }
    return status;
}

}  // namespace

// KeDelayExecutionThread - slot 21, Ord 99, stdcall, 3 args (purge 12):
// NTSTATUS (KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval).
// Evidence: kernel_imports.tsv; XAPI SleepEx 0xF64BE: KeDelayExecutionThread(1, bAlertable, &interval)
// at 0xF64E3..0xF64E9, repeats on 0x101 and maps 0xC0 to WAIT_IO_COMPLETION (startup.md 4.3);
// design 1.7. Interval {0, 0x80000000} = INFINITE.
NTSTATUS __stdcall KeDelayExecutionThread(CHAR WaitMode, BOOLEAN Alertable,
                                          xbox::LARGE_INTEGER* Interval) noexcept {
    if (DeliverApcs(Alertable)) {
        FUT_LOGT(Krnl, "KeDelayExecutionThread(%d, %u): APCs delivered -> STATUS_USER_APC",
                 static_cast<int>(WaitMode), static_cast<unsigned>(Alertable));
        return STATUS_USER_APC;
    }
    const std::uint32_t ms = TimeoutToMilliseconds(Interval);
    if (ms == INFINITE) {
        FUT_LOGW(Krnl, "KeDelayExecutionThread(%d, %u, INFINITE): sleeping forever",
                 static_cast<int>(WaitMode), static_cast<unsigned>(Alertable));
    }
    Sleep(ms);
    if (DeliverApcs(Alertable)) {
        return STATUS_USER_APC;
    }
    return STATUS_SUCCESS;
}
KRNL_FUNC(KeDelayExecutionThread, &KeDelayExecutionThread);

// NtWaitForSingleObjectEx - slot 20, Ord 234, stdcall, 4 args (purge 16):
// NTSTATUS (HANDLE Handle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout).
// Evidence: kernel_imports.tsv; XAPI WaitForSingleObjectEx 0xF647A: NtWaitForSingleObjectEx(h, 1,
// bAlertable, timeout) (repeats on 0x101); callers GetOverlappedResult (XMV) and D3D only; design 1.7.
NTSTATUS __stdcall NtWaitForSingleObjectEx(HANDLE Handle, CHAR WaitMode, BOOLEAN Alertable,
                                           xbox::LARGE_INTEGER* Timeout) noexcept {
    const NTSTATUS status = WaitOnHandle(Handle, Alertable, Timeout, "NtWaitForSingleObjectEx");
    FUT_LOGT(Krnl, "NtWaitForSingleObjectEx(%p, %d, %u) -> 0x%08lX", Handle, static_cast<int>(WaitMode),
             static_cast<unsigned>(Alertable), static_cast<unsigned long>(status));
    return status;
}
KRNL_FUNC(NtWaitForSingleObjectEx, &NtWaitForSingleObjectEx);

// NtWaitForSingleObject - slot 33, Ord 233, stdcall, 3 args (purge 12):
// NTSTATUS (HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout).
// Evidence: kernel_imports.tsv; XAPI ReadFile/WriteFile wait on the file handle after STATUS_PENDING
// with (h, 0, NULL) (0xF723B/0xF731C, kernel.md 7 group 2); design 1.7.
NTSTATUS __stdcall NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable,
                                         xbox::LARGE_INTEGER* Timeout) noexcept {
    const NTSTATUS status = WaitOnHandle(Handle, Alertable, Timeout, "NtWaitForSingleObject");
    FUT_LOGT(Krnl, "NtWaitForSingleObject(%p, %u) -> 0x%08lX", Handle, static_cast<unsigned>(Alertable),
             static_cast<unsigned long>(status));
    return status;
}
KRNL_FUNC(NtWaitForSingleObject, &NtWaitForSingleObject);

}  // namespace krnl
