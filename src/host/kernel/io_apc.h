// Guest I/O APC queue (io agent; design 1.7 "APCs", files.md 2.4/5.1 item 2, startup.md 4.3).
//
// NtReadFile/NtWriteFile with an ApcRoutine (XAPI ReadFileEx 0xF7603 / WriteFileEx 0xF7654 push
// NtUserIoApcDispatcher [slot 39] as ApcRoutine, ApcContext = the LPOVERLAPPED_COMPLETION_ROUTINE and
// IoStatusBlock = the guest OVERLAPPED) perform the transfer at once, return STATUS_PENDING and queue
// {routine, context, IoStatusBlock, status, information} for the CALLING guest thread. Each guest
// thread has its own queue (keyed by the host thread id; fibers share their thread's queue, like the
// Xbox kernel's per-KTHREAD user APC list). There is no QueueUserAPC, so no APC can land in an
// alertable wait of host code (drivers, XAudio2).
//
// Delivery (DeliverIoApcs) happens only in the alertable waits of ke_wait.cpp:
// KeDelayExecutionThread(UserMode, Alertable = TRUE, ..) and NtWaitForSingleObjectEx(.., UserMode,
// Alertable = TRUE, ..) - XAPI SleepEx 0xF64BE / WaitForSingleObjectEx 0xF647A, game call sites
// SleepEx(0, TRUE) at 0x13F4C0, 0x13F590 (NiUDSStream) and 0xE0030 (save layer). The wait then returns
// STATUS_USER_APC (0xC0), which SleepEx turns into WAIT_IO_COMPLETION.
//
// For each APC, oldest first, delivery
//   1. writes IoStatusBlock = {status, information} (= OVERLAPPED.Internal/InternalHigh; written only
//      now, design 1.7),
//   2. if routine == NtUserIoApcDispatcher (the only routine the XBE uses): calls the completion
//      routine directly, Completion(dwErrorCode, dwNumberOfBytesTransfered, lpOverlapped), stdcall
//      ret 0xC (verified: 0x13F1D0 and 0xE01A0 both end in `ret 0xC`), with
//      dwErrorCode = NT_ERROR(status) ? RtlNtStatusToDosError(status) : 0 (Win32 BasepIoCompletion
//      convention; the game only tests != 0) and lpOverlapped = IoStatusBlock;
//      any other routine is called as PIO_APC_ROUTINE routine(context, IoStatusBlock, 0) (stdcall).
//   Both calls go through core::CallGuest (esp restored, callee-saved registers kept).
//
// Disc error handler (M2 exit criterion 5): DAT_0030E68C (handler object 0x2D707C, vf0 0xF2FD0, set by
// FUN_000F33E0 through FUN_0013FAD0) is read at exactly two places (raw xref scan: 0x13F1E3, 0x13F6B2;
// 0x13FAD5 is the setter): the completion 0x13F1D0 calls it when dwErrorCode != 0, and FUN_0013F660
// calls it when ReadFileEx returns FALSE (= NtReadFile returned an NT_ERROR status). The io layer
// never queues a failed transfer (errors are returned synchronously, NT IopCompleteRequest), so
// path 1 cannot happen; path 2 is counted by NoteFailedIoApcRequest. Both counters must stay 0 in
// the M2 run (GetIoApcStats: errorCompletions, failedRequests).
#pragma once

#include "kernel/io_types.h"

#include <cstdint>

namespace krnl::io {

// Queues an I/O completion APC for the current thread. Returns false only if out of memory.
bool QueueIoApc(IoApcRoutine routine, void* context, xbox::IO_STATUS_BLOCK* ioStatusBlock, NTSTATUS status,
                std::uint32_t information) noexcept;

// Delivers all I/O APCs queued for the current thread (FIFO, including APCs queued by the completion
// routines themselves; at most 4096 per call). Returns the number delivered. Must be called on the
// guest thread at passive IRQL from an alertable user-mode wait (see above).
std::uint32_t DeliverIoApcs() noexcept;

// Number of APCs waiting for the current thread.
std::uint32_t PendingIoApcCount() noexcept;
// (M2) Number of APCs waiting for all threads together.
std::uint32_t PendingIoApcCountAllThreads() noexcept;

// (M2) Records an APC request (ApcRoutine != NULL) that failed synchronously, i.e. nothing was queued
// and the caller sees an NT_ERROR status. With ApcRoutine == NtUserIoApcDispatcher this is XAPI
// ReadFileEx/WriteFileEx returning FALSE - for a read, FUN_0013F660 then calls the disc error
// handler - so it is logged as an error; other routines are logged at debug level.
void NoteFailedIoApcRequest(const char* api, const char* path, IoApcRoutine routine,
                            NTSTATUS status) noexcept;

// Process-wide counters (M2 exit criterion 5: "APC-Zustellungen > 0", disc error handler never called).
struct IoApcStats {
    std::uint64_t queued;            // QueueIoApc
    std::uint64_t delivered;         // APCs delivered (completion/routine called)
    std::uint64_t errorCompletions;  // (M2) completions called with dwErrorCode != 0
    std::uint64_t failedRequests;    // (M2) NoteFailedIoApcRequest with NtUserIoApcDispatcher
    std::uint32_t maxPending;        // (M2) longest queue of one thread seen
    std::uint64_t deliveredXbe;      // (M2) completions inside the XBE image (the game's 0x13F1D0/0xE01A0)
};
IoApcStats GetIoApcStats() noexcept;
// (M2) Logs "I/O APC stats (<reason>): queued N, delivered N (XBE completions N), ..." on KRNL/info - for
// the orderly end of a run (e.g. --frames) and the M2 check. The first delivery to a completion routine
// inside the XBE image is logged on KRNL/info as "I/O APC: first XBE completion delivered ..." by
// DeliverIoApcs itself (host completions of self-tests/unit tests do not count there).
void LogIoApcStats(const char* reason) noexcept;

}  // namespace krnl::io
