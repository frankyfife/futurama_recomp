// Declarations of the xboxkrnl HLE functions implemented by the kernel agent (one per slot, see the
// header comment at each definition for ordinal, convention, arguments and evidence). The guest calls
// them through the thunk table; host code (tests, other kernel units) may call them directly.
// File-I/O exports (Nt*File, Io*, NtClose) are declared by the io agent (kernel/io_exports.h) and
// kernel/ob_handles.h. Owned by the kernel agent.
#pragma once

#include "kernel/ke_types.h"
#include "shared/xbe_format.h"
#include "shared/xbox_types.h"

#include <cstdint>

namespace krnl {

// --- IRQL (ke_irql.cpp) ---
void __fastcall KfLowerIrql(xbox::KIRQL NewIrql) noexcept;         // slot 0
xbox::KIRQL __stdcall KeRaiseIrqlToDpcLevel() noexcept;            // slot 1
xbox::KIRQL __fastcall KfRaiseIrql(xbox::KIRQL NewIrql) noexcept;  // slot 80

// --- events, waits, timers, DPCs (ke_event.cpp, ke_wait.cpp, ke_timer.cpp) ---
LONG __stdcall KeSetEvent(guestdef::DISPATCHER_HEADER* Event, LONG Increment,
                          BOOLEAN Wait) noexcept;  // slot 4
NTSTATUS __stdcall KeWaitForSingleObject(guestdef::DISPATCHER_HEADER* Object, ULONG WaitReason, CHAR WaitMode,
                                         BOOLEAN Alertable, xbox::LARGE_INTEGER* Timeout) noexcept;  // slot 5
void __stdcall KeInitializeTimerEx(void* Timer, ULONG Type) noexcept;                                // slot 6
void __stdcall KeInitializeDpc(void* Dpc, void* DeferredRoutine, void* DeferredContext) noexcept;    // slot 7
NTSTATUS __stdcall NtWaitForSingleObjectEx(HANDLE Handle, CHAR WaitMode, BOOLEAN Alertable,
                                           xbox::LARGE_INTEGER* Timeout) noexcept;  // slot 20
NTSTATUS __stdcall KeDelayExecutionThread(CHAR WaitMode, BOOLEAN Alertable,
                                          xbox::LARGE_INTEGER* Interval) noexcept;  // 21
void __stdcall KeQuerySystemTime(xbox::LARGE_INTEGER* CurrentTime) noexcept;        // slot 23
NTSTATUS __stdcall NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable,
                                         xbox::LARGE_INTEGER* Timeout) noexcept;             // 33
BOOLEAN __stdcall KeSetTimer(void* Timer, xbox::LARGE_INTEGER DueTime, void* Dpc) noexcept;  // slot 60
BOOLEAN __stdcall KeCancelTimer(void* Timer) noexcept;                                       // slot 61

// --- Rtl (rtl.cpp, rtl_critsec.cpp, rtl_status.cpp; RtlUnwind/RtlRaiseException are ntdll's own) ---
void __stdcall RtlInitAnsiString(xbox::ANSI_STRING* DestinationString,
                                 const char* SourceString) noexcept;                                // slot 3
void __stdcall RtlLeaveCriticalSection(guestdef::RTL_CRITICAL_SECTION* CriticalSection) noexcept;   // slot 9
void __stdcall RtlEnterCriticalSection(guestdef::RTL_CRITICAL_SECTION* CriticalSection) noexcept;   // slot 10
SIZE_T __stdcall RtlCompareMemoryUlong(const void* Source, SIZE_T Length, ULONG Pattern) noexcept;  // slot 13
void __stdcall
RtlInitializeCriticalSection(guestdef::RTL_CRITICAL_SECTION* CriticalSection) noexcept;  // slot 14
ULONG __stdcall RtlNtStatusToDosError(NTSTATUS Status) noexcept;                         // slot 24
BOOLEAN __stdcall RtlEqualString(const xbox::ANSI_STRING* String1, const xbox::ANSI_STRING* String2,
                                 BOOLEAN CaseInSensitive) noexcept;  // slot 84

// --- memory (mm.cpp) ---
NTSTATUS __stdcall NtFreeVirtualMemory(void** BaseAddress, SIZE_T* RegionSize,
                                       ULONG FreeType) noexcept;  // slot 11
NTSTATUS __stdcall NtAllocateVirtualMemory(void** BaseAddress, ULONG_PTR ZeroBits, SIZE_T* RegionSize,
                                           ULONG AllocationType, ULONG Protect) noexcept;  // slot 12
NTSTATUS __stdcall
NtQueryVirtualMemory(void* BaseAddress,
                     guestdef::MEMORY_BASIC_INFORMATION* MemoryInformation) noexcept;  // slot 15
ULONG __stdcall MmFreeSystemMemory(void* BaseAddress, SIZE_T NumberOfBytes) noexcept;  // slot 42
void* __stdcall MmAllocateSystemMemory(SIZE_T NumberOfBytes, ULONG Protect) noexcept;  // slot 44
void __stdcall MmPersistContiguousMemory(void* BaseAddress, SIZE_T NumberOfBytes,
                                         BOOLEAN Persist) noexcept;         // 51
void* __stdcall MmAllocateContiguousMemory(SIZE_T NumberOfBytes) noexcept;  // slot 52
void __stdcall MmFreeContiguousMemory(void* BaseAddress) noexcept;          // slot 54
void* __stdcall MmAllocateContiguousMemoryEx(SIZE_T NumberOfBytes, ULONG_PTR LowestAcceptableAddress,
                                             ULONG_PTR HighestAcceptableAddress, ULONG_PTR Alignment,
                                             ULONG ProtectionType) noexcept;  // slot 56
SIZE_T __stdcall MmQueryAllocationSize(void* BaseAddress) noexcept;           // slot 57
void __stdcall MmSetAddressProtect(void* BaseAddress, ULONG NumberOfBytes,
                                   ULONG NewProtect) noexcept;         // slot 58
ULONG __stdcall MmQueryAddressProtect(void* VirtualAddress) noexcept;  // slot 59

// --- settings, crypto, sections (ex_settings.cpp, xc_crypto.cpp, xe_section.cpp) ---
NTSTATUS __stdcall ExQueryNonVolatileSetting(ULONG ValueIndex, ULONG* Type, void* Value, ULONG ValueLength,
                                             ULONG* ResultLength) noexcept;  // slot 17
void __stdcall XcSHAUpdate(unsigned char* pbSHAContext, const unsigned char* pbInput,
                           ULONG dwInputLength) noexcept;                                  // 27
void __stdcall XcSHAInit(unsigned char* pbSHAContext) noexcept;                            // slot 28
void __stdcall XcSHAFinal(unsigned char* pbSHAContext, unsigned char* pbDigest) noexcept;  // slot 29
BOOLEAN __stdcall XcVerifyPKCS1Signature(const unsigned char* pbSig, const unsigned char* pbPubKey,
                                         const unsigned char* pbDigest) noexcept;  // slot 72
NTSTATUS __stdcall XeLoadSection(xbe::SectionHeader* Section) noexcept;            // slot 85
NTSTATUS __stdcall XeUnloadSection(xbe::SectionHeader* Section) noexcept;          // slot 86

// --- HAL, threads, debug (hal.cpp, ke_thread.cpp, ke_debug.cpp) ---
[[noreturn]] void __stdcall HalReturnToFirmware(ULONG Routine) noexcept;  // slot 55
void __stdcall HalRegisterShutdownNotification(const std::uint32_t* ShutdownRegistration,
                                               BOOLEAN Register) noexcept;          // 74
[[noreturn]] void __stdcall PsTerminateSystemThread(NTSTATUS ExitStatus) noexcept;  // slot 76
NTSTATUS __stdcall PsCreateSystemThreadEx(HANDLE* ThreadHandle, SIZE_T ThreadExtensionSize,
                                          SIZE_T KernelStackSize, SIZE_T TlsDataSize, ULONG* ThreadId,
                                          void* StartRoutine, void* StartContext, BOOLEAN CreateSuspended,
                                          BOOLEAN DebuggerThread,
                                          void* SystemRoutine) noexcept;  // slot 77
void __stdcall HalInitiateShutdown() noexcept;                            // slot 78
ULONG __cdecl DbgPrint(const char* Format, ...) noexcept;                 // slot 98
[[noreturn]] void __stdcall KeBugCheck(ULONG BugCheckCode) noexcept;      // slot 106
void __stdcall DbgBreakPoint() noexcept;                                  // slot 109

// --- ADDR exports (data_exports.cpp; NtUserIoApcDispatcher is declared in kernel_hle.h) ---
NTSTATUS __stdcall IoInvalidDeviceRequest(void* DeviceObject, void* Irp) noexcept;  // slot 8

}  // namespace krnl
