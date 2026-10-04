// Guest I/O APC queue (io agent; design 1.7 "APCs", files.md 2.4/5.1 item 2).
//
// NtReadFile/NtWriteFile with an ApcRoutine (XAPI ReadFileEx/WriteFileEx pass NtUserIoApcDispatcher,
// ApcContext = the LPOVERLAPPED_COMPLETION_ROUTINE and IoStatusBlock = the guest OVERLAPPED) perform
// the transfer at once, return STATUS_PENDING and queue {routine, context, IoStatusBlock, status,
// information} for the CALLING guest thread (each guest thread has its own queue; no QueueUserAPC,
// so no APC can land in an alertable wait of host code).
//
// CONTRACT with the kernel agent (owner of the waits): an alertable wait of a guest thread -
// KeDelayExecutionThread(.., Alertable = TRUE, ..) and NtWaitForSingleObjectEx(.., Alertable = TRUE, ..),
// called by XAPI SleepEx / WaitForSingleObjectEx (alertable call sites 0x13F4C0, 0x13F590, 0xE0030) -
// calls DeliverIoApcs() first and returns STATUS_USER_APC (0xC0) without waiting if it delivered at
// least one APC (and also after the wait if APCs arrived meanwhile; with one guest thread they cannot).
// Delivery writes IoStatusBlock = {status, information} and then calls
// routine(context, IoStatusBlock, 0) - i.e. NtUserIoApcDispatcher, which calls the completion routine
// (dwErrorCode, dwNumberOfBytesTransfered, lpOverlapped).
#pragma once

#include "kernel/io_types.h"

#include <cstdint>

namespace krnl::io {

// Queues an I/O completion APC for the current thread. Returns false only if out of memory.
bool QueueIoApc(IoApcRoutine routine, void* context, xbox::IO_STATUS_BLOCK* ioStatusBlock, NTSTATUS status,
                std::uint32_t information) noexcept;

// Delivers all I/O APCs queued for the current thread (FIFO, including APCs queued by the completion
// routines themselves; at most 4096 per call). Returns the number delivered. Must be called on the
// guest thread at passive IRQL from an alertable wait (see above).
std::uint32_t DeliverIoApcs() noexcept;

// Number of APCs waiting for the current thread.
std::uint32_t PendingIoApcCount() noexcept;

// Process-wide counters (M2 exit criterion 5: "APC-Zustellungen > 0").
struct IoApcStats {
    std::uint64_t queued;
    std::uint64_t delivered;
};
IoApcStats GetIoApcStats() noexcept;

}  // namespace krnl::io
