// PsCreateSystemThreadEx / PsTerminateSystemThread (slots 77, 76). Owned by the kernel agent.
//
// Design 1.6/1.7 and E10: the only guest thread besides the boot thread is the main thread created
// by XAPI CreateThread 0xF9A72 (single call site 0xF9AAB, startup.md 4.1). The HLE creates a host
// thread with a stack reservation of max(KernelStackSize, [kernel] min_guest_stack = 2 MiB). The new
// thread gets a zeroed fake KTHREAD (0x140) and TlsData (TlsDataSize bytes, separate from the stack)
// owned by its ThreadObject; core::AdoptCurrentThreadWith fills StackBase/StackLimit/TlsData/ThreadId
// and the TLS slots S_TLS..S_DPC (design 1.6). Then SystemRoutine(StartRoutine, StartContext) is
// called exactly like the Xbox kernel does, through the core MASM helper (design 3.3).
//
// SystemRoutine = XapiThreadStartup 0xF99DA (not a Ghidra function; disassembly): __SEH_prolog,
// StartRoutine at [ebp+8] and StartContext at [ebp+0xC] (two stack arguments, PKSYSTEM_ROUTINE),
// [TlsData] = TlsData+4 (0xF99FC), zero-fills the TLS block, XapiCallThreadNotifyRoutines(1),
// StartRoutine(StartContext) (0xF9A49 `call [ebp+8]`), notify(0), then PsTerminateSystemThread(result)
// (0xF9A6B) followed by int3 - it never returns.
#include "kernel/ke_thread.h"

#include "core/guest.h"
#include "core/guest_call.h"
#include "core/guest_thread.h"
#include "core/log.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "kernel/kernel_hle.h"
#include "kernel/kernel_internal.h"
#include "kernel/kernel_settings.h"
#include "shared/nt_status.h"

#include <atomic>
#include <cstdio>

namespace krnl {
namespace {

// Handed from PsCreateSystemThreadEx to the new thread (POD; freed by the new thread).
struct ThreadStart {
    ThreadObject* object;         // holds one reference for the running thread
    std::uint32_t startRoutine;   // StartContext1
    std::uint32_t startContext;   // StartContext2
    std::uint32_t systemRoutine;  // may be 0
    std::uint32_t number;         // 1-based creation counter (thread name)
    bool abort;                   // creation failed after CreateThread: exit immediately
};

std::atomic<std::uint32_t> g_liveThreads{0};
std::atomic<std::uint32_t> g_createdThreads{0};
thread_local ThreadObject* t_threadObject = nullptr;

HANDLE ExitedEvent() noexcept {
    static HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset, non-signaled
    return event;
}

// Host thread procedure of every guest thread. No C++ objects with destructors may be alive across
// the guest call: the thread ends inside the guest via PsTerminateSystemThread -> ExitThread.
DWORD WINAPI GuestThreadMain(void* param) noexcept {
    auto* start = static_cast<ThreadStart*>(param);
    if (start->abort) {
        start->object->Release();
        delete start;
        return 1;
    }
    const ThreadStart s = *start;
    delete start;
    t_threadObject = s.object;

    char name[32];
    if (s.startRoutine == guest::kMainThreadRoutine) {
        std::snprintf(name, sizeof(name), "guest-main");
    } else {
        std::snprintf(name, sizeof(name), "guest-%u", s.number);
    }
    core::GuestThread gt{};
    const bool adopted = s.object->TlsDataSize() != 0
                             ? core::AdoptCurrentThreadWith(s.object->KThread(), s.object->TlsData(),
                                                            s.object->TlsDataSize(), name, &gt)
                             : core::AdoptCurrentThread(
                                   0, name, &gt);  // ASSUMPTION: TlsDataSize 0 -> TlsData = NULL (boot style)
    if (!adopted) {
        core::Fatal("PsCreateSystemThreadEx: adopting thread %lu (TlsDataSize 0x%X) failed",
                    GetCurrentThreadId(), s.object->TlsDataSize());
    }
    // Info lines avoid host thread ids (M1 exit criterion 6: run-to-run identical logs); see debug.
    FUT_LOGI(Krnl, "guest thread %s running: KTHREAD %p, TlsData %p, anchor 0x%08X, stack 0x%08X..0x%08X",
             name, static_cast<void*>(gt.kthread), static_cast<void*>(gt.tlsData), gt.tlsAnchor,
             gt.kthread ? gt.kthread->StackLimit : 0u, gt.kthread ? gt.kthread->StackBase : 0u);
    FUT_LOGD(Krnl, "guest thread %s = host thread %lu", name, GetCurrentThreadId());
    if (s.systemRoutine != 0) {
        // SystemRoutine(StartRoutine, StartContext): stdcall, two stack arguments (see header comment).
        core::CallGuest(s.systemRoutine, {s.startRoutine, s.startContext});
        FUT_LOGE(Krnl, "SystemRoutine 0x%08X returned (expected PsTerminateSystemThread)", s.systemRoutine);
    } else {
        // ASSUMPTION: without SystemRoutine the Xbox kernel calls StartRoutine(StartContext) and ends the
        // thread with STATUS_SUCCESS when it returns (NT PspSystemThreadStartup).
        core::CallGuest(s.startRoutine, {s.startContext});
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

}  // namespace

bool InitThreading() noexcept {
    return ExitedEvent() != nullptr;
}

HANDLE GuestThreadsExitedEvent() noexcept {
    return ExitedEvent();
}

std::uint32_t LiveGuestThreadCount() noexcept {
    return g_liveThreads.load(std::memory_order_acquire);
}

// PsCreateSystemThreadEx - slot 77, Ord 255, stdcall, 10 args (purge 40):
// NTSTATUS (PHANDLE ThreadHandle, SIZE_T ThreadExtensionSize, SIZE_T KernelStackSize, SIZE_T TlsDataSize,
//           PULONG ThreadId OPTIONAL, PKSTART_ROUTINE StartRoutine (StartContext1),
//           PVOID StartContext (StartContext2), BOOLEAN CreateSuspended, BOOLEAN DebuggerThread,
//           PKSYSTEM_ROUTINE SystemRoutine OPTIONAL).
// Evidence: kernel_imports.tsv; single site XAPI CreateThread 0xF9AAB, pushes 0xF9A84..0xF9AAA:
// (&handle, 0, dwStackSize or [0x10130] = 0x10000, [0x310394] = 0x24, lpThreadId, lpStartAddress,
//  lpParameter, (dwCreationFlags >> 2) & 0xFFFFFF01 (only the low byte is the BOOLEAN), 0, 0xF99DA);
// on failure CreateThread calls XapiSetLastNTError (0xF9AD0). entry 0xF8531 passes lpThreadId = 0 and
// closes the handle afterwards (0xF8547). kernel.md 7.4; design 1.6/1.7.
NTSTATUS __stdcall PsCreateSystemThreadEx(HANDLE* ThreadHandle, SIZE_T ThreadExtensionSize,
                                          SIZE_T KernelStackSize, SIZE_T TlsDataSize, ULONG* ThreadId,
                                          void* StartRoutine, void* StartContext, BOOLEAN CreateSuspended,
                                          BOOLEAN DebuggerThread, void* SystemRoutine) noexcept {
    OnXbeMapped();  // finish data exports that depend on the mapped XBE before guest threads run
    if (ThreadHandle == nullptr) {
        FUT_LOGE(Krnl, "PsCreateSystemThreadEx: NULL ThreadHandle");
        return STATUS_INVALID_PARAMETER;
    }
    if (TlsDataSize > ThreadObject::kMaxTlsDataSize || (TlsDataSize != 0 && TlsDataSize < 4)) {
        FUT_LOGE(Krnl, "PsCreateSystemThreadEx: unsupported TlsDataSize 0x%lX",
                 static_cast<unsigned long>(TlsDataSize));
        return STATUS_INVALID_PARAMETER;
    }
    if (ThreadExtensionSize != 0 || DebuggerThread) {
        FUT_LOGW(Krnl, "PsCreateSystemThreadEx: ThreadExtensionSize 0x%lX / DebuggerThread %u ignored",
                 static_cast<unsigned long>(ThreadExtensionSize), static_cast<unsigned>(DebuggerThread));
    }
    const std::uint32_t minStack = Settings().minGuestStack;
    std::uint32_t stack = KernelStackSize > minStack ? static_cast<std::uint32_t>(KernelStackSize) : minStack;
    stack = (stack + 0xFFFF) & ~0xFFFFu;

    // Guest-visible per-thread memory: fake KTHREAD + TlsData, zeroed, below 0x80000000 (design 1.2).
    void* block = VirtualAlloc(nullptr, ThreadObject::kBlockSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (block == nullptr || guest::Addr(block) + ThreadObject::kBlockSize > guest::kGuestMemoryLimit) {
        FUT_LOGE(Krnl, "PsCreateSystemThreadEx: no thread memory below 0x80000000 (%p)", block);
        if (block != nullptr) {
            VirtualFree(block, 0, MEM_RELEASE);
        }
        return STATUS_NO_MEMORY;
    }

    NTSTATUS status = STATUS_SUCCESS;
    HANDLE host = nullptr;
    DWORD tid = 0;
    HANDLE guestHandle = nullptr;
    FUT_HLE_TRY {
        Ref<ThreadObject> object = MakeObject<ThreadObject>(block, static_cast<std::uint32_t>(TlsDataSize));
        const std::uint32_t number = g_createdThreads.fetch_add(1, std::memory_order_relaxed) + 1;
        auto* start = new ThreadStart{
            nullptr, guest::Addr(StartRoutine), guest::Addr(StartContext), guest::Addr(SystemRoutine), number,
            false};
        host = CreateThread(nullptr, stack, &GuestThreadMain, start,
                            CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
        if (host == nullptr) {
            FUT_LOGE(Krnl, "PsCreateSystemThreadEx: CreateThread(stack 0x%X) failed (%lu)", stack,
                     GetLastError());
            delete start;
            return STATUS_INSUFFICIENT_RESOURCES;  // `object` frees the block
        }
        object->SetHostThread(host, tid);
        object->AddRef();  // reference of the running thread (released in PsTerminateSystemThread)
        start->object = object.get();
        guestHandle = HandleTable::Instance().Insert(object);
        if (guestHandle == nullptr) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            start->abort = true;
            ResumeThread(host);  // the thread sees `abort`, drops its reference and exits
        }
    }
    FUT_HLE_CATCH("PsCreateSystemThreadEx")

    if (!NT_SUCCESS(status)) {
        FUT_LOGE(Krnl, "PsCreateSystemThreadEx: no guest handle available -> 0x%08lX",
                 static_cast<unsigned long>(status));
        return status;
    }
    *ThreadHandle = guestHandle;
    if (ThreadId != nullptr) {
        *ThreadId = tid;  // = KTHREAD+0x12C (GetCurrentThreadId 0xF9AE0) of the new thread
    }
    g_liveThreads.fetch_add(1, std::memory_order_acq_rel);
    ResetEvent(ExitedEvent());
    FUT_LOGD(Krnl, "PsCreateSystemThreadEx: handle %p = host thread %lu", guestHandle, tid);
    FUT_LOGI(Krnl,
             "PsCreateSystemThreadEx: handle %p, stack 0x%X (requested 0x%lX), TlsDataSize 0x%lX, "
             "StartRoutine %p, StartContext %p, SystemRoutine %p%s%s",
             guestHandle, stack, static_cast<unsigned long>(KernelStackSize),
             static_cast<unsigned long>(TlsDataSize), StartRoutine, StartContext, SystemRoutine,
             guest::Addr(SystemRoutine) == guest::kXapiThreadStartup ? " (XapiThreadStartup)" : "",
             CreateSuspended ? ", suspended" : "");
    if (CreateSuspended) {
        // NtResumeThread is not imported by this XBE (kernel.md 3): the thread would never run.
        FUT_LOGW(
            Krnl,
            "PsCreateSystemThreadEx: CreateSuspended - the guest cannot resume threads (no NtResumeThread)");
    } else {
        ResumeThread(host);
    }
    return STATUS_SUCCESS;
}
KRNL_FUNC(PsCreateSystemThreadEx, &PsCreateSystemThreadEx);

// PsTerminateSystemThread - slot 76, Ord 258, stdcall, 1 arg (purge 4): VOID (NTSTATUS ExitStatus), no
// return. Evidence: kernel_imports.tsv; single site 0x0F9A6B at the end of XapiThreadStartup (`push
// [ebp-0x28]` = result of the thread routine, followed by int3; kernel.md 3); design 1.7: release the thread
// state, ExitThread(status).
void __stdcall PsTerminateSystemThread(NTSTATUS ExitStatus) noexcept {
    ThreadObject* object = t_threadObject;
    if (object == nullptr) {
        core::Fatal(
            "PsTerminateSystemThread(0x%08lX) on thread %lu, which was not created by PsCreateSystemThreadEx",
            static_cast<unsigned long>(ExitStatus), GetCurrentThreadId());
    }
    t_threadObject = nullptr;
    if (CurrentIrql() >= xbox::kIrqlDispatch) {
        FUT_LOGE(Krnl, "PsTerminateSystemThread at IRQL %u - lowering to PASSIVE_LEVEL", CurrentIrql());
        KfLowerIrql(xbox::kIrqlPassive);
    }
    const char* name = xbox::NtStatusName(ExitStatus);
    FUT_LOGI(Krnl, "guest thread terminated: PsTerminateSystemThread(0x%08lX %s)",
             static_cast<unsigned long>(ExitStatus), name ? name : "");
    FUT_LOGD(Krnl, "terminated host thread %lu", GetCurrentThreadId());
    core::ReleaseCurrentThread();  // clears the TLS slots (the memory belongs to the object)
    object->MarkExited(ExitStatus);
    object->Release();  // may free KTHREAD/TlsData: nothing reads them any more
    if (g_liveThreads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        FUT_LOGW(Krnl, "last guest thread terminated - signaling krnl::GuestThreadsExitedEvent()");
        SetEvent(ExitedEvent());
    }
    core::LogFlush();
    ExitThread(static_cast<DWORD>(ExitStatus));
}
KRNL_FUNC(PsTerminateSystemThread, &PsTerminateSystemThread);

}  // namespace krnl
