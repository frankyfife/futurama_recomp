// Unit tests of the guest I/O APC queue and the alertable waits (io agent, M2; design 1.7 "APCs",
// 5.1 test "apc_queue"; kernel/io_apc.h, kernel/ke_wait.h). Run without the XBE: completion routines
// are host functions with the guest conventions (stdcall, ret 0xC).
#include "doctest.h"

#include "kernel/io_apc.h"
#include "kernel/io_common.h"
#include "kernel/io_types.h"
#include "kernel/ke_wait.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "kernel/kernel_hle.h"
#include "kernel/ob_handles.h"

#include <cstdint>
#include <thread>
#include <vector>

namespace {

xbox::LARGE_INTEGER Zero() {
    xbox::LARGE_INTEGER z;
    z.QuadPart = 0;
    return z;
}

krnl::io::IoApcRoutine Dispatcher() {
    return reinterpret_cast<krnl::io::IoApcRoutine>(
        krnl::GetBinding(krnl::exp::NtUserIoApcDispatcher::slot).address);
}

// Empties the current thread's queue (a failed test must not leak APCs into the next one).
void Drain() {
    while (krnl::io::DeliverIoApcs() != 0) {
    }
}

// LPOVERLAPPED_COMPLETION_ROUTINE like the game's 0x13F1D0 / 0xE01A0 (stdcall, ret 0xC).
struct CompletionCall {
    std::uint32_t error;
    std::uint32_t bytes;
    void* overlapped;
};
std::vector<CompletionCall> g_calls;
void __stdcall RecordingCompletion(std::uint32_t error, std::uint32_t bytes, void* overlapped) {
    g_calls.push_back(CompletionCall{error, bytes, overlapped});
}

// Completion that issues another asynchronous request from inside the APC (queues a new APC).
xbox::OVERLAPPED g_chainedOverlapped{};
int g_chainedCalls = 0;
void __stdcall ChainingCompletion(std::uint32_t, std::uint32_t, void*) {
    ++g_chainedCalls;
    if (g_chainedCalls == 1) {
        // CHECK, not REQUIRE: no C++ exception may unwind through the guest-call frames.
        CHECK(krnl::io::QueueIoApc(Dispatcher(), reinterpret_cast<void*>(&ChainingCompletion),
                                   reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&g_chainedOverlapped),
                                   STATUS_SUCCESS, 0x800));
    }
}

// A completion with the WRONG convention (cdecl, no purge): the delivery path restores esp itself
// (core::CallGuest), so a guest routine with a different purge cannot corrupt the host stack.
int g_cdeclCalls = 0;
void __cdecl CdeclCompletion(std::uint32_t, std::uint32_t bytes, void*) {
    g_cdeclCalls += bytes == 0x1234 ? 1 : 100;
}

// Generic PIO_APC_ROUTINE (not NtUserIoApcDispatcher).
std::vector<std::uintptr_t> g_routineOrder;
void __stdcall OrderRoutine(void* context, xbox::IO_STATUS_BLOCK* iosb, std::uint32_t reserved) {
    CHECK(reserved == 0);
    CHECK(iosb != nullptr);
    g_routineOrder.push_back(reinterpret_cast<std::uintptr_t>(context));
}

}  // namespace

TEST_CASE("apc: queued I/O APCs wait for an alertable user-mode wait (design 1.7)") {
    REQUIRE(krnl::IsImplemented(krnl::exp::NtUserIoApcDispatcher::slot));
    Drain();
    g_calls.clear();
    xbox::LARGE_INTEGER zero = Zero();
    const krnl::io::IoApcStats before = krnl::io::GetIoApcStats();

    xbox::OVERLAPPED ovl[3] = {};
    for (int i = 0; i < 3; ++i) {
        REQUIRE(krnl::io::QueueIoApc(Dispatcher(), reinterpret_cast<void*>(&RecordingCompletion),
                                     reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl[i]), STATUS_SUCCESS,
                                     0x800u * (i + 1)));
    }
    CHECK(krnl::io::PendingIoApcCount() == 3);
    CHECK(ovl[0].Internal == 0);  // the OVERLAPPED is written at delivery only

    // not alertable / kernel mode / NtWaitForSingleObject: nothing is delivered
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, FALSE, &zero) == STATUS_SUCCESS);
    CHECK(krnl::KeDelayExecutionThread(krnl::kKernelMode, TRUE, &zero) == STATUS_SUCCESS);  // XPP style
    const HANDLE ev = krnl::CreateEventHandle(krnl::EventObject::Type::Notification, true);
    REQUIRE(ev != nullptr);
    CHECK(krnl::NtWaitForSingleObject(ev, TRUE, &zero) == STATUS_SUCCESS);  // Xbox: KernelMode (ASSUMPTION)
    CHECK(krnl::NtWaitForSingleObjectEx(ev, krnl::kUserMode, FALSE, &zero) == STATUS_SUCCESS);
    CHECK(g_calls.empty());
    CHECK(krnl::io::PendingIoApcCount() == 3);

    // another thread has its own (empty) queue and cannot steal this thread's APCs
    std::uint32_t otherPending = 99, otherDelivered = 99;
    std::thread other([&] {
        otherPending = krnl::io::PendingIoApcCount();
        otherDelivered = krnl::io::DeliverIoApcs();
    });
    other.join();
    CHECK(otherPending == 0);
    CHECK(otherDelivered == 0);
    CHECK(krnl::io::PendingIoApcCountAllThreads() >= 3);

    // XAPI SleepEx(0, TRUE): KeDelayExecutionThread(UserMode, TRUE, 0) -> all three, FIFO, STATUS_USER_APC
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    REQUIRE(g_calls.size() == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(g_calls[i].error == 0);
        CHECK(g_calls[i].bytes == 0x800u * (i + 1));
        CHECK(g_calls[i].overlapped == &ovl[i]);
        CHECK(ovl[i].Internal == static_cast<std::uint32_t>(STATUS_SUCCESS));
        CHECK(ovl[i].InternalHigh == 0x800u * (i + 1));
    }
    CHECK(krnl::io::PendingIoApcCount() == 0);
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, TRUE, &zero) == STATUS_SUCCESS);  // queue empty

    // XAPI WaitForSingleObjectEx(h, .., TRUE): NtWaitForSingleObjectEx(h, UserMode, TRUE, t) delivers too
    REQUIRE(krnl::io::QueueIoApc(Dispatcher(), reinterpret_cast<void*>(&RecordingCompletion),
                                 reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl[0]), STATUS_SUCCESS, 7));
    CHECK(krnl::NtWaitForSingleObjectEx(ev, krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    CHECK(g_calls.size() == 4);
    CHECK(krnl::NtClose(ev) == STATUS_SUCCESS);

    const krnl::io::IoApcStats after = krnl::io::GetIoApcStats();
    CHECK(after.queued - before.queued == 4);
    CHECK(after.delivered - before.delivered == 4);
    CHECK(after.maxPending >= 3);
    CHECK(after.errorCompletions == before.errorCompletions);
}

TEST_CASE("apc: completion arguments (dwErrorCode mapping, warnings, PIO_APC_ROUTINE)") {
    Drain();
    g_calls.clear();
    xbox::LARGE_INTEGER zero = Zero();
    const krnl::io::IoApcStats before = krnl::io::GetIoApcStats();
    xbox::OVERLAPPED a{}, b{}, c{};
    // error status -> RtlNtStatusToDosError (the game's completion would call its disc error handler)
    REQUIRE(krnl::io::QueueIoApc(Dispatcher(), reinterpret_cast<void*>(&RecordingCompletion),
                                 reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&a), STATUS_END_OF_FILE, 0));
    // warning status (NT_WARNING) -> 0 like Win32 BasepIoCompletion
    REQUIRE(krnl::io::QueueIoApc(Dispatcher(), reinterpret_cast<void*>(&RecordingCompletion),
                                 reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&b), STATUS_BUFFER_OVERFLOW, 0x40));
    // generic APC routine: routine(context, IoStatusBlock, 0)
    g_routineOrder.clear();
    REQUIRE(krnl::io::QueueIoApc(&OrderRoutine, reinterpret_cast<void*>(std::uintptr_t{0x1111}),
                                 reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&c), STATUS_SUCCESS, 1));
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    REQUIRE(g_calls.size() == 2);
    CHECK(g_calls[0].error == ERROR_HANDLE_EOF);
    CHECK(g_calls[0].error == krnl::NtStatusToDosError(STATUS_END_OF_FILE));
    CHECK(a.Internal == static_cast<std::uint32_t>(STATUS_END_OF_FILE));
    CHECK(g_calls[1].error == 0);
    CHECK(g_calls[1].bytes == 0x40);
    REQUIRE(g_routineOrder.size() == 1);
    CHECK(g_routineOrder[0] == 0x1111);
    CHECK(c.InternalHigh == 1);
    const krnl::io::IoApcStats after = krnl::io::GetIoApcStats();
    CHECK(after.errorCompletions - before.errorCompletions == 1);
}

TEST_CASE("apc: APCs queued by a completion are delivered in the same wait; wrong purge is harmless") {
    Drain();
    xbox::LARGE_INTEGER zero = Zero();
    g_chainedCalls = 0;
    xbox::OVERLAPPED first{};
    REQUIRE(krnl::io::QueueIoApc(Dispatcher(), reinterpret_cast<void*>(&ChainingCompletion),
                                 reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&first), STATUS_SUCCESS, 0x800));
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    CHECK(g_chainedCalls == 2);
    CHECK(g_chainedOverlapped.InternalHigh == 0x800);
    CHECK(krnl::io::PendingIoApcCount() == 0);

    g_cdeclCalls = 0;
    xbox::OVERLAPPED o{};
    for (int i = 0; i < 3; ++i) {
        REQUIRE(krnl::io::QueueIoApc(Dispatcher(), reinterpret_cast<void*>(&CdeclCompletion),
                                     reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&o), STATUS_SUCCESS, 0x1234));
    }
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    CHECK(g_cdeclCalls == 3);
}

TEST_CASE("apc: wait exports on event and unknown handles") {
    Drain();
    xbox::LARGE_INTEGER zero = Zero();
    const std::size_t baseHandles = krnl::HandleTable::Instance().Count();
    const HANDLE sync = krnl::CreateEventHandle(krnl::EventObject::Type::Synchronization, false);
    REQUIRE(sync != nullptr);
    CHECK(krnl::NtWaitForSingleObjectEx(sync, krnl::kUserMode, FALSE, &zero) == STATUS_TIMEOUT);
    krnl::LookupEvent(sync)->Set();
    CHECK(krnl::NtWaitForSingleObjectEx(sync, krnl::kUserMode, FALSE, &zero) == STATUS_SUCCESS);
    CHECK(krnl::NtWaitForSingleObjectEx(sync, krnl::kUserMode, FALSE, &zero) ==
          STATUS_TIMEOUT);  // auto reset
    const HANDLE note = krnl::CreateEventHandle(krnl::EventObject::Type::Notification, true);
    CHECK(krnl::NtWaitForSingleObject(note, FALSE, nullptr) == STATUS_SUCCESS);
    CHECK(krnl::NtWaitForSingleObject(note, FALSE, nullptr) == STATUS_SUCCESS);  // stays signaled
    CHECK_FALSE(krnl::LookupEvent(reinterpret_cast<HANDLE>(0x7777)));
    CHECK(krnl::NtWaitForSingleObject(reinterpret_cast<HANDLE>(0x7777), FALSE, &zero) ==
          STATUS_INVALID_HANDLE);
    CHECK(krnl::NtClose(sync) == STATUS_SUCCESS);
    CHECK(krnl::NtClose(note) == STATUS_SUCCESS);
    CHECK(krnl::HandleTable::Instance().Count() == baseHandles);
    CHECK(krnl::DeliversUserApcs(krnl::kUserMode, TRUE));
    CHECK_FALSE(krnl::DeliversUserApcs(krnl::kKernelMode, TRUE));
    CHECK_FALSE(krnl::DeliversUserApcs(krnl::kUserMode, FALSE));
}

// M2 review finding 2: only a failed ReadFileEx (NiUDSStream FUN_0013F660) leads to the disc error
// handler; a failed WriteFileEx (save layer FUN_000E0B00) is counted separately and must not fail M2
// exit criterion 5 ("failed APC reads 0").
TEST_CASE("apc: failed APC requests are split into reads (disc error path) and writes (save layer)") {
    const krnl::io::IoApcStats before = krnl::io::GetIoApcStats();
    krnl::io::NoteFailedIoApcRequest("NtWriteFile", R"(\Device\Harddisk0\partition1\x.dat)", Dispatcher(),
                                     STATUS_DISK_FULL);
    krnl::io::IoApcStats after = krnl::io::GetIoApcStats();
    CHECK(after.failedWriteRequests - before.failedWriteRequests == 1);
    CHECK(after.failedRequests == before.failedRequests);
    krnl::io::NoteFailedIoApcRequest("NtReadFile", R"(\Device\CdRom0\a.img)", Dispatcher(),
                                     STATUS_END_OF_FILE);
    after = krnl::io::GetIoApcStats();
    CHECK(after.failedRequests - before.failedRequests == 1);
    CHECK(after.failedWriteRequests - before.failedWriteRequests == 1);
    // other APC routines and requests without APC are not counted
    krnl::io::NoteFailedIoApcRequest("NtReadFile", "x", nullptr, STATUS_END_OF_FILE);
    after = krnl::io::GetIoApcStats();
    CHECK(after.failedRequests - before.failedRequests == 1);
    CHECK(after.errorCompletions == before.errorCompletions);
}
