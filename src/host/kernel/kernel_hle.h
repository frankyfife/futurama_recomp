// Public helpers of the kernel HLE for other host modules (core, io). Owned by the kernel agent.
// The xboxkrnl exports themselves are registered with kernel_api.h macros and are called by the
// guest through the thunk table; this header only exposes what host code needs in addition.
#pragma once

#include "shared/xbox_types.h"

#include <cstdint>

namespace krnl {

// --- status mapping (rtl_status.cpp) -------------------------------------------------------------
// Win32 error for an NTSTATUS, exactly as the guest-visible RtlNtStatusToDosError (slot 24) returns
// it. Table of every status the HLE can hand to the guest, values taken from ntdll; unknown codes
// fall back to ntdll!RtlNtStatusToDosErrorNoTeb.
std::uint32_t NtStatusToDosError(NTSTATUS status) noexcept;

// --- timeouts (ke_event.cpp) ---------------------------------------------------------------------
// Converts an NT/Xbox timeout (nullptr = infinite, negative = relative, positive = absolute system
// time, unit 100 ns) into milliseconds for Win32 waits (INFINITE for nullptr, rounded up).
std::uint32_t TimeoutToMilliseconds(const xbox::LARGE_INTEGER* timeout) noexcept;

// --- IRQL (ke_irql.cpp) --------------------------------------------------------------------------
// Current emulated IRQL of the calling thread (low byte of TLS slot S_IRQL, design 1.6/1.7).
xbox::KIRQL CurrentIrql() noexcept;

// --- guest threads (ke_thread.cpp) ---------------------------------------------------------------
// Manual-reset event that becomes signaled when the last thread created by PsCreateSystemThreadEx
// has terminated (PsTerminateSystemThread). The boot thread's message loop may wait on it.
HANDLE GuestThreadsExitedEvent() noexcept;
// Number of running guest threads created by PsCreateSystemThreadEx.
std::uint32_t LiveGuestThreadCount() noexcept;

// --- late initialization (data_exports.cpp) ------------------------------------------------------
// Finishes data exports that need the mapped XBE (signature keys in [saves] key_mode = xbox derive
// from the certificate at 0x10180). Idempotent; called automatically by PsCreateSystemThreadEx
// before the first guest thread starts, core may call it right after mapping.
void OnXbeMapped() noexcept;

// --- ADDR export NtUserIoApcDispatcher (slot 39, Ord 232) ----------------------------------------
// stdcall (ApcContext, IoStatusBlock, Reserved), ret 0xC. Calls the guest completion routine
// ApcContext(dwErrorCode, IoStatusBlock->Information, (LPOVERLAPPED)IoStatusBlock) (stdcall, ret 0xC)
// with dwErrorCode = NT_ERROR(Status) ? RtlNtStatusToDosError(Status) : 0 (design 1.7, files.md 5.1).
// The io agent compares NtReadFile/NtWriteFile ApcRoutine against
// krnl::GetBinding(krnl::exp::NtUserIoApcDispatcher::slot).address and may call it when delivering
// the queued APC on the guest thread.
void __stdcall NtUserIoApcDispatcher(void* ApcContext, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                     std::uint32_t Reserved) noexcept;

}  // namespace krnl
