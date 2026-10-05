// GENERATED FILE - DO NOT EDIT.
// Generator: tools/gen/gen_kernel_table.py (run with --check to verify).
// Sources:   analysis/loader/kernel_imports.tsv, docs/loader_design.md (Anhang A milestone plan)
//
// Thunk table of the retail EU XBE at 0x293560: 111 slots, each 0x80000000|ordinal, terminator at 0x29371C
// (kernel.md section 1). For every export this header provides:
//   - a row in krnl::gen::kExports (indexed by slot), and
//   - a trait struct krnl::exp::<Name> with constexpr slot/ordinal/kind/conv/purge used by the
//     registration macros in kernel/kernel_api.h (static_assert on convention and stack purge).
// purgeBytes = bytes the CALLEE removes from the stack (stdcall: 4*args, fastcall: 0, cdecl: 0).
#pragma once

#include <cstdint>

namespace krnl::gen {

enum class Kind : std::uint8_t { Func, Data, Addr };
enum class Conv : std::uint8_t { None, Stdcall, Fastcall, Cdecl };
// Implementation plan per docs/loader_design.md Anhang A.
enum class Milestone : std::uint8_t { M1, M2, M7, Defensive, ReplacedLibs };
// Return kind of a FUNC export (tools/gen/gen_kernel_table.py RETURN_KIND). Generated stubs return
// STATUS_NOT_IMPLEMENTED only for Ntstatus and 0 (NULL/FALSE) otherwise (design 5.4).
enum class Ret : std::uint8_t { Void, Ntstatus, Pointer, Boolean, Ulong };

struct ExportInfo {
    std::uint16_t slot;
    std::uint16_t ordinal;
    Kind kind;
    Conv conv;
    std::uint8_t argDwords;   // DWORD parameters of the generated stub
    std::uint8_t regArgs;     // fastcall register arguments (ecx, edx)
    std::uint16_t purgeBytes; // callee stack cleanup
    bool variadic;            // cdecl DbgPrint
    bool onlyReplacedLibs;    // all callers are in D3D/DSOUND/XMV/XPP (kernel.md section 6)
    Milestone milestone;
    Ret ret;                  // return kind (FUNC); Void for DATA/ADDR
    bool stubFatal;           // generated stub aborts even without [kernel] strict (FATAL_STUBS)
    const char* name;
};

inline constexpr std::uint32_t kSlotCount = 111;
inline constexpr std::uint32_t kThunkTableVa = 0x00293560u;
inline constexpr std::uint32_t kThunkTableEnd = 0x0029371Cu;  // terminator DWORD (0)

inline constexpr ExportInfo kExports[kSlotCount] = {
    {  0, 161, Kind::Func, Conv::Fastcall,  1, 1,  0, false, false, Milestone::M1, Ret::Void, false, "KfLowerIrql"},
    {  1, 129, Kind::Func, Conv::Stdcall,   0, 0,  0, false, false, Milestone::M1, Ret::Ulong, false, "KeRaiseIrqlToDpcLevel"},
    {  2,  65, Kind::Func, Conv::Stdcall,   6, 0, 24, false, true, Milestone::ReplacedLibs, Ret::Ntstatus, false, "IoCreateDevice"},
    {  3, 289, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M1, Ret::Void, false, "RtlInitAnsiString"},
    {  4, 145, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::Defensive, Ret::Ulong, false, "KeSetEvent"},
    {  5, 159, Kind::Func, Conv::Stdcall,   5, 0, 20, false, false, Milestone::Defensive, Ret::Ntstatus, false, "KeWaitForSingleObject"},
    {  6, 113, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M1, Ret::Void, false, "KeInitializeTimerEx"},
    {  7, 107, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M1, Ret::Void, false, "KeInitializeDpc"},
    {  8,  74, Kind::Addr, Conv::None,      0, 0,  0, false, true, Milestone::M1, Ret::Void, false, "IoInvalidDeviceRequest"},
    {  9, 294, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "RtlLeaveCriticalSection"},
    { 10, 277, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "RtlEnterCriticalSection"},
    { 11, 199, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M1, Ret::Ntstatus, false, "NtFreeVirtualMemory"},
    { 12, 184, Kind::Func, Conv::Stdcall,   5, 0, 20, false, false, Milestone::M1, Ret::Ntstatus, false, "NtAllocateVirtualMemory"},
    { 13, 269, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M1, Ret::Ulong, false, "RtlCompareMemoryUlong"},
    { 14, 291, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "RtlInitializeCriticalSection"},
    { 15, 217, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M1, Ret::Ntstatus, false, "NtQueryVirtualMemory"},
    { 16, 302, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "RtlRaiseException"},
    { 17,  24, Kind::Func, Conv::Stdcall,   5, 0, 20, false, false, Milestone::M1, Ret::Ntstatus, false, "ExQueryNonVolatileSetting"},
    { 18,  16, Kind::Data, Conv::None,      0, 0,  0, false, true, Milestone::M1, Ret::Void, false, "ExEventObjectType"},
    { 19, 225, Kind::Func, Conv::Stdcall,   2, 0,  8, false, true, Milestone::ReplacedLibs, Ret::Ntstatus, false, "NtSetEvent"},
    { 20, 234, Kind::Func, Conv::Stdcall,   4, 0, 16, false, false, Milestone::M2, Ret::Ntstatus, false, "NtWaitForSingleObjectEx"},
    { 21,  99, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M2, Ret::Ntstatus, false, "KeDelayExecutionThread"},
    { 22, 226, Kind::Func, Conv::Stdcall,   5, 0, 20, false, false, Milestone::M1, Ret::Ntstatus, false, "NtSetInformationFile"},
    { 23, 128, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "KeQuerySystemTime"},
    { 24, 301, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Ulong, false, "RtlNtStatusToDosError"},
    { 25, 190, Kind::Func, Conv::Stdcall,   9, 0, 36, false, false, Milestone::M1, Ret::Ntstatus, false, "NtCreateFile"},
    { 26, 187, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Ntstatus, false, "NtClose"},
    { 27, 336, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M1, Ret::Void, false, "XcSHAUpdate"},
    { 28, 335, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "XcSHAInit"},
    { 29, 337, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M1, Ret::Void, false, "XcSHAFinal"},
    { 30, 323, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "XboxHDKey"},
    { 31, 325, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "XboxSignatureKey"},
    { 32, 354, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "XboxAlternateSignatureKeys"},
    { 33, 233, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M2, Ret::Ntstatus, false, "NtWaitForSingleObject"},
    { 34, 219, Kind::Func, Conv::Stdcall,   8, 0, 32, false, false, Milestone::M1, Ret::Ntstatus, false, "NtReadFile"},
    { 35, 236, Kind::Func, Conv::Stdcall,   8, 0, 32, false, false, Milestone::M1, Ret::Ntstatus, false, "NtWriteFile"},
    { 36, 211, Kind::Func, Conv::Stdcall,   5, 0, 20, false, false, Milestone::M1, Ret::Ntstatus, false, "NtQueryInformationFile"},
    { 37, 218, Kind::Func, Conv::Stdcall,   5, 0, 20, false, false, Milestone::M1, Ret::Ntstatus, false, "NtQueryVolumeInformationFile"},
    { 38, 198, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M2, Ret::Ntstatus, false, "NtFlushBuffersFile"},
    { 39, 232, Kind::Addr, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "NtUserIoApcDispatcher"},
    { 40, 200, Kind::Func, Conv::Stdcall,  10, 0, 40, false, false, Milestone::Defensive, Ret::Ntstatus, false, "NtFsControlFile"},
    { 41, 196, Kind::Func, Conv::Stdcall,  10, 0, 40, false, false, Milestone::M1, Ret::Ntstatus, false, "NtDeviceIoControlFile"},
    { 42, 172, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M7, Ret::Ulong, false, "MmFreeSystemMemory"},
    { 43, 202, Kind::Func, Conv::Stdcall,   6, 0, 24, false, false, Milestone::M1, Ret::Ntstatus, false, "NtOpenFile"},
    { 44, 167, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M7, Ret::Pointer, false, "MmAllocateSystemMemory"},
    { 45, 210, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M2, Ret::Ntstatus, false, "NtQueryFullAttributesFile"},
    { 46, 207, Kind::Func, Conv::Stdcall,  10, 0, 40, false, false, Milestone::M2, Ret::Ntstatus, false, "NtQueryDirectoryFile"},
    { 47,  40, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "HalDiskCachePartitionCount"},
    { 48,  67, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M1, Ret::Ntstatus, false, "IoCreateSymbolicLink"},
    { 49, 215, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M7, Ret::Ntstatus, false, "NtQuerySymbolicLinkObject"},
    { 50, 203, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M7, Ret::Ntstatus, false, "NtOpenSymbolicLinkObject"},
    { 51, 178, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M2, Ret::Void, false, "MmPersistContiguousMemory"},
    { 52, 165, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M2, Ret::Pointer, false, "MmAllocateContiguousMemory"},
    { 53, 164, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "LaunchDataPage"},
    { 54, 171, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "MmFreeContiguousMemory"},
    { 55,  49, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "HalReturnToFirmware"},
    { 56, 166, Kind::Func, Conv::Stdcall,   5, 0, 20, false, false, Milestone::M2, Ret::Pointer, false, "MmAllocateContiguousMemoryEx"},
    { 57, 180, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Ulong, false, "MmQueryAllocationSize"},
    { 58, 182, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M1, Ret::Void, false, "MmSetAddressProtect"},
    { 59, 179, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Ulong, false, "MmQueryAddressProtect"},
    { 60, 149, Kind::Func, Conv::Stdcall,   4, 0, 16, false, false, Milestone::M1, Ret::Boolean, false, "KeSetTimer"},
    { 61,  97, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Boolean, false, "KeCancelTimer"},
    { 62, 250, Kind::Func, Conv::Fastcall,  1, 1,  0, false, true, Milestone::ReplacedLibs, Ret::Void, false, "ObfDereferenceObject"},
    { 63, 246, Kind::Func, Conv::Stdcall,   3, 0, 12, false, true, Milestone::ReplacedLibs, Ret::Ntstatus, false, "ObReferenceObjectByHandle"},
    { 64, 322, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "XboxHardwareInfo"},
    { 65,  83, Kind::Func, Conv::Stdcall,   3, 0, 12, false, true, Milestone::ReplacedLibs, Ret::Void, false, "IoStartPacket"},
    { 66,  87, Kind::Func, Conv::Fastcall,  2, 2,  0, false, true, Milestone::ReplacedLibs, Ret::Void, false, "IofCompleteRequest"},
    { 67,  81, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Void, false, "IoStartNextPacket"},
    { 68,  17, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Void, false, "ExFreePool"},
    { 69,  15, Kind::Func, Conv::Stdcall,   2, 0,  8, false, true, Milestone::ReplacedLibs, Ret::Pointer, false, "ExAllocatePoolWithTag"},
    { 70, 359, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Void, false, "IoMarkIrpMustComplete"},
    { 71, 358, Kind::Func, Conv::Stdcall,   0, 0,  0, false, true, Milestone::ReplacedLibs, Ret::Boolean, false, "HalIsResetOrShutdownPending"},
    { 72, 344, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M1, Ret::Boolean, false, "XcVerifyPKCS1Signature"},
    { 73, 355, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "XePublicKeyData"},
    { 74,  47, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M1, Ret::Void, false, "HalRegisterShutdownNotification"},
    { 75, 324, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "XboxKrnlVersion"},
    { 76, 258, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "PsTerminateSystemThread"},
    { 77, 255, Kind::Func, Conv::Stdcall,  10, 0, 40, false, false, Milestone::M1, Ret::Ntstatus, false, "PsCreateSystemThreadEx"},
    { 78, 360, Kind::Func, Conv::Stdcall,   0, 0,  0, false, false, Milestone::Defensive, Ret::Void, false, "HalInitiateShutdown"},
    { 79,  44, Kind::Func, Conv::Stdcall,   2, 0,  8, false, true, Milestone::ReplacedLibs, Ret::Ulong, false, "HalGetInterruptVector"},
    { 80, 160, Kind::Func, Conv::Fastcall,  1, 1,  0, false, true, Milestone::ReplacedLibs, Ret::Ulong, false, "KfRaiseIrql"},
    { 81,  98, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Boolean, false, "KeConnectInterrupt"},
    { 82, 109, Kind::Func, Conv::Stdcall,   7, 0, 28, false, true, Milestone::ReplacedLibs, Ret::Void, false, "KeInitializeInterrupt"},
    { 83, 151, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Void, false, "KeStallExecutionProcessor"},
    { 84, 279, Kind::Func, Conv::Stdcall,   3, 0, 12, false, false, Milestone::M1, Ret::Boolean, false, "RtlEqualString"},
    { 85, 327, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Ntstatus, false, "XeLoadSection"},
    { 86, 328, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Ntstatus, false, "XeUnloadSection"},
    { 87, 357, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "IdexChannelObject"},
    { 88, 173, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Ulong, false, "MmGetPhysicalAddress"},
    { 89, 175, Kind::Func, Conv::Stdcall,   3, 0, 12, false, true, Milestone::ReplacedLibs, Ret::Void, false, "MmLockUnlockBufferPages"},
    { 90, 119, Kind::Func, Conv::Stdcall,   3, 0, 12, false, true, Milestone::ReplacedLibs, Ret::Boolean, false, "KeInsertQueueDpc"},
    { 91, 176, Kind::Func, Conv::Stdcall,   2, 0,  8, false, true, Milestone::ReplacedLibs, Ret::Void, false, "MmLockUnlockPhysicalPage"},
    { 92,   1, Kind::Func, Conv::Stdcall,   0, 0,  0, false, true, Milestone::ReplacedLibs, Ret::Pointer, false, "AvGetSavedDataAddress"},
    { 93,   2, Kind::Func, Conv::Stdcall,   4, 0, 16, false, true, Milestone::ReplacedLibs, Ret::Void, false, "AvSendTVEncoderOption"},
    { 94,   3, Kind::Func, Conv::Stdcall,   6, 0, 24, false, true, Milestone::ReplacedLibs, Ret::Ulong, false, "AvSetDisplayMode"},
    { 95,   4, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Void, false, "AvSetSavedDataAddress"},
    { 96,  46, Kind::Func, Conv::Stdcall,   6, 0, 24, false, true, Milestone::ReplacedLibs, Ret::Void, false, "HalReadWritePCISpace"},
    { 97, 100, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Boolean, false, "KeDisconnectInterrupt"},
    { 98,   8, Kind::Func, Conv::Cdecl,     1, 0,  0, true, true, Milestone::ReplacedLibs, Ret::Ntstatus, false, "DbgPrint"},
    { 99, 168, Kind::Func, Conv::Stdcall,   2, 0,  8, false, true, Milestone::ReplacedLibs, Ret::Pointer, false, "MmClaimGpuInstanceMemory"},
    {100, 142, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Ntstatus, false, "KeSaveFloatingPointState"},
    {101, 139, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Ntstatus, false, "KeRestoreFloatingPointState"},
    {102,  23, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Ulong, false, "ExQueryPoolBlockSize"},
    {103, 137, Kind::Func, Conv::Stdcall,   1, 0,  4, false, true, Milestone::ReplacedLibs, Ret::Boolean, false, "KeRemoveQueueDpc"},
    {104, 153, Kind::Func, Conv::Stdcall,   3, 0, 12, false, true, Milestone::ReplacedLibs, Ret::Boolean, false, "KeSynchronizeExecution"},
    {105, 312, Kind::Func, Conv::Stdcall,   4, 0, 16, false, false, Milestone::M1, Ret::Void, false, "RtlUnwind"},
    {106,  95, Kind::Func, Conv::Stdcall,   1, 0,  4, false, false, Milestone::M1, Ret::Void, false, "KeBugCheck"},
    {107, 169, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M2, Ret::Pointer, true, "MmCreateKernelStack"},
    {108, 170, Kind::Func, Conv::Stdcall,   2, 0,  8, false, false, Milestone::M2, Ret::Void, true, "MmDeleteKernelStack"},
    {109,   5, Kind::Func, Conv::Stdcall,   0, 0,  0, false, false, Milestone::M2, Ret::Void, false, "DbgBreakPoint"},
    {110, 356, Kind::Data, Conv::None,      0, 0,  0, false, false, Milestone::M1, Ret::Void, false, "HalBootSMCVideoMode"},
};

// Address of the generated stub (FUNC), zeroed placeholder buffer (DATA) or trap routine (ADDR).
std::uintptr_t StubAddress(std::uint32_t slot) noexcept;

}  // namespace krnl::gen

namespace krnl::exp {

// slot 0, Ord 161 (0x0A1), FUNC fastcall, args 1 (ecx), purge 0, returns void, plan M1
struct KfLowerIrql {
    static constexpr std::uint32_t slot = 0, ordinal = 161;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Fastcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 1, purgeBytes = 0;
    static constexpr const char* name = "KfLowerIrql";
};
// slot 1, Ord 129 (0x081), FUNC stdcall, args 0, purge 0, returns ulong, plan M1
struct KeRaiseIrqlToDpcLevel {
    static constexpr std::uint32_t slot = 1, ordinal = 129;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "KeRaiseIrqlToDpcLevel";
};
// slot 2, Ord 65 (0x041), FUNC stdcall, args 6, purge 24, returns ntstatus, plan ReplacedLibs, only replaced libs
struct IoCreateDevice {
    static constexpr std::uint32_t slot = 2, ordinal = 65;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 6, regArgs = 0, purgeBytes = 24;
    static constexpr const char* name = "IoCreateDevice";
};
// slot 3, Ord 289 (0x121), FUNC stdcall, args 2, purge 8, returns void, plan M1
struct RtlInitAnsiString {
    static constexpr std::uint32_t slot = 3, ordinal = 289;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "RtlInitAnsiString";
};
// slot 4, Ord 145 (0x091), FUNC stdcall, args 3, purge 12, returns ulong, plan Defensive
struct KeSetEvent {
    static constexpr std::uint32_t slot = 4, ordinal = 145;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "KeSetEvent";
};
// slot 5, Ord 159 (0x09F), FUNC stdcall, args 5, purge 20, returns ntstatus, plan Defensive
struct KeWaitForSingleObject {
    static constexpr std::uint32_t slot = 5, ordinal = 159;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 5, regArgs = 0, purgeBytes = 20;
    static constexpr const char* name = "KeWaitForSingleObject";
};
// slot 6, Ord 113 (0x071), FUNC stdcall, args 2, purge 8, returns void, plan M1
struct KeInitializeTimerEx {
    static constexpr std::uint32_t slot = 6, ordinal = 113;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "KeInitializeTimerEx";
};
// slot 7, Ord 107 (0x06B), FUNC stdcall, args 3, purge 12, returns void, plan M1
struct KeInitializeDpc {
    static constexpr std::uint32_t slot = 7, ordinal = 107;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "KeInitializeDpc";
};
// slot 8, Ord 74 (0x04A), ADDR, plan M1, only replaced libs
struct IoInvalidDeviceRequest {
    static constexpr std::uint32_t slot = 8, ordinal = 74;
    static constexpr gen::Kind kind = gen::Kind::Addr;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "IoInvalidDeviceRequest";
};
// slot 9, Ord 294 (0x126), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct RtlLeaveCriticalSection {
    static constexpr std::uint32_t slot = 9, ordinal = 294;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "RtlLeaveCriticalSection";
};
// slot 10, Ord 277 (0x115), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct RtlEnterCriticalSection {
    static constexpr std::uint32_t slot = 10, ordinal = 277;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "RtlEnterCriticalSection";
};
// slot 11, Ord 199 (0x0C7), FUNC stdcall, args 3, purge 12, returns ntstatus, plan M1
struct NtFreeVirtualMemory {
    static constexpr std::uint32_t slot = 11, ordinal = 199;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "NtFreeVirtualMemory";
};
// slot 12, Ord 184 (0x0B8), FUNC stdcall, args 5, purge 20, returns ntstatus, plan M1
struct NtAllocateVirtualMemory {
    static constexpr std::uint32_t slot = 12, ordinal = 184;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 5, regArgs = 0, purgeBytes = 20;
    static constexpr const char* name = "NtAllocateVirtualMemory";
};
// slot 13, Ord 269 (0x10D), FUNC stdcall, args 3, purge 12, returns ulong, plan M1
struct RtlCompareMemoryUlong {
    static constexpr std::uint32_t slot = 13, ordinal = 269;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "RtlCompareMemoryUlong";
};
// slot 14, Ord 291 (0x123), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct RtlInitializeCriticalSection {
    static constexpr std::uint32_t slot = 14, ordinal = 291;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "RtlInitializeCriticalSection";
};
// slot 15, Ord 217 (0x0D9), FUNC stdcall, args 2, purge 8, returns ntstatus, plan M1
struct NtQueryVirtualMemory {
    static constexpr std::uint32_t slot = 15, ordinal = 217;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "NtQueryVirtualMemory";
};
// slot 16, Ord 302 (0x12E), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct RtlRaiseException {
    static constexpr std::uint32_t slot = 16, ordinal = 302;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "RtlRaiseException";
};
// slot 17, Ord 24 (0x018), FUNC stdcall, args 5, purge 20, returns ntstatus, plan M1
struct ExQueryNonVolatileSetting {
    static constexpr std::uint32_t slot = 17, ordinal = 24;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 5, regArgs = 0, purgeBytes = 20;
    static constexpr const char* name = "ExQueryNonVolatileSetting";
};
// slot 18, Ord 16 (0x010), DATA, plan M1, only replaced libs
struct ExEventObjectType {
    static constexpr std::uint32_t slot = 18, ordinal = 16;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "ExEventObjectType";
};
// slot 19, Ord 225 (0x0E1), FUNC stdcall, args 2, purge 8, returns ntstatus, plan ReplacedLibs, only replaced libs
struct NtSetEvent {
    static constexpr std::uint32_t slot = 19, ordinal = 225;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "NtSetEvent";
};
// slot 20, Ord 234 (0x0EA), FUNC stdcall, args 4, purge 16, returns ntstatus, plan M2
struct NtWaitForSingleObjectEx {
    static constexpr std::uint32_t slot = 20, ordinal = 234;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 4, regArgs = 0, purgeBytes = 16;
    static constexpr const char* name = "NtWaitForSingleObjectEx";
};
// slot 21, Ord 99 (0x063), FUNC stdcall, args 3, purge 12, returns ntstatus, plan M2
struct KeDelayExecutionThread {
    static constexpr std::uint32_t slot = 21, ordinal = 99;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "KeDelayExecutionThread";
};
// slot 22, Ord 226 (0x0E2), FUNC stdcall, args 5, purge 20, returns ntstatus, plan M1
struct NtSetInformationFile {
    static constexpr std::uint32_t slot = 22, ordinal = 226;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 5, regArgs = 0, purgeBytes = 20;
    static constexpr const char* name = "NtSetInformationFile";
};
// slot 23, Ord 128 (0x080), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct KeQuerySystemTime {
    static constexpr std::uint32_t slot = 23, ordinal = 128;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "KeQuerySystemTime";
};
// slot 24, Ord 301 (0x12D), FUNC stdcall, args 1, purge 4, returns ulong, plan M1
struct RtlNtStatusToDosError {
    static constexpr std::uint32_t slot = 24, ordinal = 301;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "RtlNtStatusToDosError";
};
// slot 25, Ord 190 (0x0BE), FUNC stdcall, args 9, purge 36, returns ntstatus, plan M1
struct NtCreateFile {
    static constexpr std::uint32_t slot = 25, ordinal = 190;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 9, regArgs = 0, purgeBytes = 36;
    static constexpr const char* name = "NtCreateFile";
};
// slot 26, Ord 187 (0x0BB), FUNC stdcall, args 1, purge 4, returns ntstatus, plan M1
struct NtClose {
    static constexpr std::uint32_t slot = 26, ordinal = 187;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "NtClose";
};
// slot 27, Ord 336 (0x150), FUNC stdcall, args 3, purge 12, returns void, plan M1
struct XcSHAUpdate {
    static constexpr std::uint32_t slot = 27, ordinal = 336;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "XcSHAUpdate";
};
// slot 28, Ord 335 (0x14F), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct XcSHAInit {
    static constexpr std::uint32_t slot = 28, ordinal = 335;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "XcSHAInit";
};
// slot 29, Ord 337 (0x151), FUNC stdcall, args 2, purge 8, returns void, plan M1
struct XcSHAFinal {
    static constexpr std::uint32_t slot = 29, ordinal = 337;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "XcSHAFinal";
};
// slot 30, Ord 323 (0x143), DATA, plan M1
struct XboxHDKey {
    static constexpr std::uint32_t slot = 30, ordinal = 323;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "XboxHDKey";
};
// slot 31, Ord 325 (0x145), DATA, plan M1
struct XboxSignatureKey {
    static constexpr std::uint32_t slot = 31, ordinal = 325;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "XboxSignatureKey";
};
// slot 32, Ord 354 (0x162), DATA, plan M1
struct XboxAlternateSignatureKeys {
    static constexpr std::uint32_t slot = 32, ordinal = 354;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "XboxAlternateSignatureKeys";
};
// slot 33, Ord 233 (0x0E9), FUNC stdcall, args 3, purge 12, returns ntstatus, plan M2
struct NtWaitForSingleObject {
    static constexpr std::uint32_t slot = 33, ordinal = 233;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "NtWaitForSingleObject";
};
// slot 34, Ord 219 (0x0DB), FUNC stdcall, args 8, purge 32, returns ntstatus, plan M1
struct NtReadFile {
    static constexpr std::uint32_t slot = 34, ordinal = 219;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 8, regArgs = 0, purgeBytes = 32;
    static constexpr const char* name = "NtReadFile";
};
// slot 35, Ord 236 (0x0EC), FUNC stdcall, args 8, purge 32, returns ntstatus, plan M1
struct NtWriteFile {
    static constexpr std::uint32_t slot = 35, ordinal = 236;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 8, regArgs = 0, purgeBytes = 32;
    static constexpr const char* name = "NtWriteFile";
};
// slot 36, Ord 211 (0x0D3), FUNC stdcall, args 5, purge 20, returns ntstatus, plan M1
struct NtQueryInformationFile {
    static constexpr std::uint32_t slot = 36, ordinal = 211;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 5, regArgs = 0, purgeBytes = 20;
    static constexpr const char* name = "NtQueryInformationFile";
};
// slot 37, Ord 218 (0x0DA), FUNC stdcall, args 5, purge 20, returns ntstatus, plan M1
struct NtQueryVolumeInformationFile {
    static constexpr std::uint32_t slot = 37, ordinal = 218;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 5, regArgs = 0, purgeBytes = 20;
    static constexpr const char* name = "NtQueryVolumeInformationFile";
};
// slot 38, Ord 198 (0x0C6), FUNC stdcall, args 2, purge 8, returns ntstatus, plan M2
struct NtFlushBuffersFile {
    static constexpr std::uint32_t slot = 38, ordinal = 198;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "NtFlushBuffersFile";
};
// slot 39, Ord 232 (0x0E8), ADDR, plan M1
struct NtUserIoApcDispatcher {
    static constexpr std::uint32_t slot = 39, ordinal = 232;
    static constexpr gen::Kind kind = gen::Kind::Addr;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "NtUserIoApcDispatcher";
};
// slot 40, Ord 200 (0x0C8), FUNC stdcall, args 10, purge 40, returns ntstatus, plan Defensive
struct NtFsControlFile {
    static constexpr std::uint32_t slot = 40, ordinal = 200;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 10, regArgs = 0, purgeBytes = 40;
    static constexpr const char* name = "NtFsControlFile";
};
// slot 41, Ord 196 (0x0C4), FUNC stdcall, args 10, purge 40, returns ntstatus, plan M1
struct NtDeviceIoControlFile {
    static constexpr std::uint32_t slot = 41, ordinal = 196;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 10, regArgs = 0, purgeBytes = 40;
    static constexpr const char* name = "NtDeviceIoControlFile";
};
// slot 42, Ord 172 (0x0AC), FUNC stdcall, args 2, purge 8, returns ulong, plan M7
struct MmFreeSystemMemory {
    static constexpr std::uint32_t slot = 42, ordinal = 172;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "MmFreeSystemMemory";
};
// slot 43, Ord 202 (0x0CA), FUNC stdcall, args 6, purge 24, returns ntstatus, plan M1
struct NtOpenFile {
    static constexpr std::uint32_t slot = 43, ordinal = 202;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 6, regArgs = 0, purgeBytes = 24;
    static constexpr const char* name = "NtOpenFile";
};
// slot 44, Ord 167 (0x0A7), FUNC stdcall, args 2, purge 8, returns pointer, plan M7
struct MmAllocateSystemMemory {
    static constexpr std::uint32_t slot = 44, ordinal = 167;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Pointer;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "MmAllocateSystemMemory";
};
// slot 45, Ord 210 (0x0D2), FUNC stdcall, args 2, purge 8, returns ntstatus, plan M2
struct NtQueryFullAttributesFile {
    static constexpr std::uint32_t slot = 45, ordinal = 210;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "NtQueryFullAttributesFile";
};
// slot 46, Ord 207 (0x0CF), FUNC stdcall, args 10, purge 40, returns ntstatus, plan M2
struct NtQueryDirectoryFile {
    static constexpr std::uint32_t slot = 46, ordinal = 207;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 10, regArgs = 0, purgeBytes = 40;
    static constexpr const char* name = "NtQueryDirectoryFile";
};
// slot 47, Ord 40 (0x028), DATA, plan M1
struct HalDiskCachePartitionCount {
    static constexpr std::uint32_t slot = 47, ordinal = 40;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "HalDiskCachePartitionCount";
};
// slot 48, Ord 67 (0x043), FUNC stdcall, args 2, purge 8, returns ntstatus, plan M1
struct IoCreateSymbolicLink {
    static constexpr std::uint32_t slot = 48, ordinal = 67;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "IoCreateSymbolicLink";
};
// slot 49, Ord 215 (0x0D7), FUNC stdcall, args 3, purge 12, returns ntstatus, plan M7
struct NtQuerySymbolicLinkObject {
    static constexpr std::uint32_t slot = 49, ordinal = 215;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "NtQuerySymbolicLinkObject";
};
// slot 50, Ord 203 (0x0CB), FUNC stdcall, args 2, purge 8, returns ntstatus, plan M7
struct NtOpenSymbolicLinkObject {
    static constexpr std::uint32_t slot = 50, ordinal = 203;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "NtOpenSymbolicLinkObject";
};
// slot 51, Ord 178 (0x0B2), FUNC stdcall, args 3, purge 12, returns void, plan M2
struct MmPersistContiguousMemory {
    static constexpr std::uint32_t slot = 51, ordinal = 178;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "MmPersistContiguousMemory";
};
// slot 52, Ord 165 (0x0A5), FUNC stdcall, args 1, purge 4, returns pointer, plan M2
struct MmAllocateContiguousMemory {
    static constexpr std::uint32_t slot = 52, ordinal = 165;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Pointer;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "MmAllocateContiguousMemory";
};
// slot 53, Ord 164 (0x0A4), DATA, plan M1
struct LaunchDataPage {
    static constexpr std::uint32_t slot = 53, ordinal = 164;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "LaunchDataPage";
};
// slot 54, Ord 171 (0x0AB), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct MmFreeContiguousMemory {
    static constexpr std::uint32_t slot = 54, ordinal = 171;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "MmFreeContiguousMemory";
};
// slot 55, Ord 49 (0x031), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct HalReturnToFirmware {
    static constexpr std::uint32_t slot = 55, ordinal = 49;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "HalReturnToFirmware";
};
// slot 56, Ord 166 (0x0A6), FUNC stdcall, args 5, purge 20, returns pointer, plan M2
struct MmAllocateContiguousMemoryEx {
    static constexpr std::uint32_t slot = 56, ordinal = 166;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Pointer;
    static constexpr std::uint32_t argDwords = 5, regArgs = 0, purgeBytes = 20;
    static constexpr const char* name = "MmAllocateContiguousMemoryEx";
};
// slot 57, Ord 180 (0x0B4), FUNC stdcall, args 1, purge 4, returns ulong, plan ReplacedLibs, only replaced libs
struct MmQueryAllocationSize {
    static constexpr std::uint32_t slot = 57, ordinal = 180;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "MmQueryAllocationSize";
};
// slot 58, Ord 182 (0x0B6), FUNC stdcall, args 3, purge 12, returns void, plan M1
struct MmSetAddressProtect {
    static constexpr std::uint32_t slot = 58, ordinal = 182;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "MmSetAddressProtect";
};
// slot 59, Ord 179 (0x0B3), FUNC stdcall, args 1, purge 4, returns ulong, plan M1
struct MmQueryAddressProtect {
    static constexpr std::uint32_t slot = 59, ordinal = 179;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "MmQueryAddressProtect";
};
// slot 60, Ord 149 (0x095), FUNC stdcall, args 4, purge 16, returns boolean, plan M1
struct KeSetTimer {
    static constexpr std::uint32_t slot = 60, ordinal = 149;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 4, regArgs = 0, purgeBytes = 16;
    static constexpr const char* name = "KeSetTimer";
};
// slot 61, Ord 97 (0x061), FUNC stdcall, args 1, purge 4, returns boolean, plan ReplacedLibs, only replaced libs
struct KeCancelTimer {
    static constexpr std::uint32_t slot = 61, ordinal = 97;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "KeCancelTimer";
};
// slot 62, Ord 250 (0x0FA), FUNC fastcall, args 1 (ecx), purge 0, returns void, plan ReplacedLibs, only replaced libs
struct ObfDereferenceObject {
    static constexpr std::uint32_t slot = 62, ordinal = 250;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Fastcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 1, purgeBytes = 0;
    static constexpr const char* name = "ObfDereferenceObject";
};
// slot 63, Ord 246 (0x0F6), FUNC stdcall, args 3, purge 12, returns ntstatus, plan ReplacedLibs, only replaced libs
struct ObReferenceObjectByHandle {
    static constexpr std::uint32_t slot = 63, ordinal = 246;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "ObReferenceObjectByHandle";
};
// slot 64, Ord 322 (0x142), DATA, plan M1
struct XboxHardwareInfo {
    static constexpr std::uint32_t slot = 64, ordinal = 322;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "XboxHardwareInfo";
};
// slot 65, Ord 83 (0x053), FUNC stdcall, args 3, purge 12, returns void, plan ReplacedLibs, only replaced libs
struct IoStartPacket {
    static constexpr std::uint32_t slot = 65, ordinal = 83;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "IoStartPacket";
};
// slot 66, Ord 87 (0x057), FUNC fastcall, args 2 (ecx,edx), purge 0, returns void, plan ReplacedLibs, only replaced libs
struct IofCompleteRequest {
    static constexpr std::uint32_t slot = 66, ordinal = 87;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Fastcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 2, regArgs = 2, purgeBytes = 0;
    static constexpr const char* name = "IofCompleteRequest";
};
// slot 67, Ord 81 (0x051), FUNC stdcall, args 1, purge 4, returns void, plan ReplacedLibs, only replaced libs
struct IoStartNextPacket {
    static constexpr std::uint32_t slot = 67, ordinal = 81;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "IoStartNextPacket";
};
// slot 68, Ord 17 (0x011), FUNC stdcall, args 1, purge 4, returns void, plan ReplacedLibs, only replaced libs
struct ExFreePool {
    static constexpr std::uint32_t slot = 68, ordinal = 17;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "ExFreePool";
};
// slot 69, Ord 15 (0x00F), FUNC stdcall, args 2, purge 8, returns pointer, plan ReplacedLibs, only replaced libs
struct ExAllocatePoolWithTag {
    static constexpr std::uint32_t slot = 69, ordinal = 15;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Pointer;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "ExAllocatePoolWithTag";
};
// slot 70, Ord 359 (0x167), FUNC stdcall, args 1, purge 4, returns void, plan ReplacedLibs, only replaced libs
struct IoMarkIrpMustComplete {
    static constexpr std::uint32_t slot = 70, ordinal = 359;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "IoMarkIrpMustComplete";
};
// slot 71, Ord 358 (0x166), FUNC stdcall, args 0, purge 0, returns boolean, plan ReplacedLibs, only replaced libs
struct HalIsResetOrShutdownPending {
    static constexpr std::uint32_t slot = 71, ordinal = 358;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "HalIsResetOrShutdownPending";
};
// slot 72, Ord 344 (0x158), FUNC stdcall, args 3, purge 12, returns boolean, plan M1
struct XcVerifyPKCS1Signature {
    static constexpr std::uint32_t slot = 72, ordinal = 344;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "XcVerifyPKCS1Signature";
};
// slot 73, Ord 355 (0x163), DATA, plan M1
struct XePublicKeyData {
    static constexpr std::uint32_t slot = 73, ordinal = 355;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "XePublicKeyData";
};
// slot 74, Ord 47 (0x02F), FUNC stdcall, args 2, purge 8, returns void, plan M1
struct HalRegisterShutdownNotification {
    static constexpr std::uint32_t slot = 74, ordinal = 47;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "HalRegisterShutdownNotification";
};
// slot 75, Ord 324 (0x144), DATA, plan M1
struct XboxKrnlVersion {
    static constexpr std::uint32_t slot = 75, ordinal = 324;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "XboxKrnlVersion";
};
// slot 76, Ord 258 (0x102), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct PsTerminateSystemThread {
    static constexpr std::uint32_t slot = 76, ordinal = 258;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "PsTerminateSystemThread";
};
// slot 77, Ord 255 (0x0FF), FUNC stdcall, args 10, purge 40, returns ntstatus, plan M1
struct PsCreateSystemThreadEx {
    static constexpr std::uint32_t slot = 77, ordinal = 255;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 10, regArgs = 0, purgeBytes = 40;
    static constexpr const char* name = "PsCreateSystemThreadEx";
};
// slot 78, Ord 360 (0x168), FUNC stdcall, args 0, purge 0, returns void, plan Defensive
struct HalInitiateShutdown {
    static constexpr std::uint32_t slot = 78, ordinal = 360;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "HalInitiateShutdown";
};
// slot 79, Ord 44 (0x02C), FUNC stdcall, args 2, purge 8, returns ulong, plan ReplacedLibs, only replaced libs
struct HalGetInterruptVector {
    static constexpr std::uint32_t slot = 79, ordinal = 44;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "HalGetInterruptVector";
};
// slot 80, Ord 160 (0x0A0), FUNC fastcall, args 1 (ecx), purge 0, returns ulong, plan ReplacedLibs, only replaced libs
struct KfRaiseIrql {
    static constexpr std::uint32_t slot = 80, ordinal = 160;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Fastcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 1, regArgs = 1, purgeBytes = 0;
    static constexpr const char* name = "KfRaiseIrql";
};
// slot 81, Ord 98 (0x062), FUNC stdcall, args 1, purge 4, returns boolean, plan ReplacedLibs, only replaced libs
struct KeConnectInterrupt {
    static constexpr std::uint32_t slot = 81, ordinal = 98;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "KeConnectInterrupt";
};
// slot 82, Ord 109 (0x06D), FUNC stdcall, args 7, purge 28, returns void, plan ReplacedLibs, only replaced libs
struct KeInitializeInterrupt {
    static constexpr std::uint32_t slot = 82, ordinal = 109;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 7, regArgs = 0, purgeBytes = 28;
    static constexpr const char* name = "KeInitializeInterrupt";
};
// slot 83, Ord 151 (0x097), FUNC stdcall, args 1, purge 4, returns void, plan ReplacedLibs, only replaced libs
struct KeStallExecutionProcessor {
    static constexpr std::uint32_t slot = 83, ordinal = 151;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "KeStallExecutionProcessor";
};
// slot 84, Ord 279 (0x117), FUNC stdcall, args 3, purge 12, returns boolean, plan M1
struct RtlEqualString {
    static constexpr std::uint32_t slot = 84, ordinal = 279;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "RtlEqualString";
};
// slot 85, Ord 327 (0x147), FUNC stdcall, args 1, purge 4, returns ntstatus, plan M1
struct XeLoadSection {
    static constexpr std::uint32_t slot = 85, ordinal = 327;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "XeLoadSection";
};
// slot 86, Ord 328 (0x148), FUNC stdcall, args 1, purge 4, returns ntstatus, plan M1
struct XeUnloadSection {
    static constexpr std::uint32_t slot = 86, ordinal = 328;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "XeUnloadSection";
};
// slot 87, Ord 357 (0x165), DATA, plan M1
struct IdexChannelObject {
    static constexpr std::uint32_t slot = 87, ordinal = 357;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "IdexChannelObject";
};
// slot 88, Ord 173 (0x0AD), FUNC stdcall, args 1, purge 4, returns ulong, plan ReplacedLibs, only replaced libs
struct MmGetPhysicalAddress {
    static constexpr std::uint32_t slot = 88, ordinal = 173;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "MmGetPhysicalAddress";
};
// slot 89, Ord 175 (0x0AF), FUNC stdcall, args 3, purge 12, returns void, plan ReplacedLibs, only replaced libs
struct MmLockUnlockBufferPages {
    static constexpr std::uint32_t slot = 89, ordinal = 175;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "MmLockUnlockBufferPages";
};
// slot 90, Ord 119 (0x077), FUNC stdcall, args 3, purge 12, returns boolean, plan ReplacedLibs, only replaced libs
struct KeInsertQueueDpc {
    static constexpr std::uint32_t slot = 90, ordinal = 119;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "KeInsertQueueDpc";
};
// slot 91, Ord 176 (0x0B0), FUNC stdcall, args 2, purge 8, returns void, plan ReplacedLibs, only replaced libs
struct MmLockUnlockPhysicalPage {
    static constexpr std::uint32_t slot = 91, ordinal = 176;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "MmLockUnlockPhysicalPage";
};
// slot 92, Ord 1 (0x001), FUNC stdcall, args 0, purge 0, returns pointer, plan ReplacedLibs, only replaced libs
struct AvGetSavedDataAddress {
    static constexpr std::uint32_t slot = 92, ordinal = 1;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Pointer;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "AvGetSavedDataAddress";
};
// slot 93, Ord 2 (0x002), FUNC stdcall, args 4, purge 16, returns void, plan ReplacedLibs, only replaced libs
struct AvSendTVEncoderOption {
    static constexpr std::uint32_t slot = 93, ordinal = 2;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 4, regArgs = 0, purgeBytes = 16;
    static constexpr const char* name = "AvSendTVEncoderOption";
};
// slot 94, Ord 3 (0x003), FUNC stdcall, args 6, purge 24, returns ulong, plan ReplacedLibs, only replaced libs
struct AvSetDisplayMode {
    static constexpr std::uint32_t slot = 94, ordinal = 3;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 6, regArgs = 0, purgeBytes = 24;
    static constexpr const char* name = "AvSetDisplayMode";
};
// slot 95, Ord 4 (0x004), FUNC stdcall, args 1, purge 4, returns void, plan ReplacedLibs, only replaced libs
struct AvSetSavedDataAddress {
    static constexpr std::uint32_t slot = 95, ordinal = 4;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "AvSetSavedDataAddress";
};
// slot 96, Ord 46 (0x02E), FUNC stdcall, args 6, purge 24, returns void, plan ReplacedLibs, only replaced libs
struct HalReadWritePCISpace {
    static constexpr std::uint32_t slot = 96, ordinal = 46;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 6, regArgs = 0, purgeBytes = 24;
    static constexpr const char* name = "HalReadWritePCISpace";
};
// slot 97, Ord 100 (0x064), FUNC stdcall, args 1, purge 4, returns boolean, plan ReplacedLibs, only replaced libs
struct KeDisconnectInterrupt {
    static constexpr std::uint32_t slot = 97, ordinal = 100;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "KeDisconnectInterrupt";
};
// slot 98, Ord 8 (0x008), FUNC cdecl, args var (1 Push an der Site), purge 0, returns ntstatus, plan ReplacedLibs, only replaced libs
struct DbgPrint {
    static constexpr std::uint32_t slot = 98, ordinal = 8;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Cdecl;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "DbgPrint";
};
// slot 99, Ord 168 (0x0A8), FUNC stdcall, args 2, purge 8, returns pointer, plan ReplacedLibs, only replaced libs
struct MmClaimGpuInstanceMemory {
    static constexpr std::uint32_t slot = 99, ordinal = 168;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Pointer;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "MmClaimGpuInstanceMemory";
};
// slot 100, Ord 142 (0x08E), FUNC stdcall, args 1, purge 4, returns ntstatus, plan ReplacedLibs, only replaced libs
struct KeSaveFloatingPointState {
    static constexpr std::uint32_t slot = 100, ordinal = 142;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "KeSaveFloatingPointState";
};
// slot 101, Ord 139 (0x08B), FUNC stdcall, args 1, purge 4, returns ntstatus, plan ReplacedLibs, only replaced libs
struct KeRestoreFloatingPointState {
    static constexpr std::uint32_t slot = 101, ordinal = 139;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ntstatus;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "KeRestoreFloatingPointState";
};
// slot 102, Ord 23 (0x017), FUNC stdcall, args 1, purge 4, returns ulong, plan ReplacedLibs, only replaced libs
struct ExQueryPoolBlockSize {
    static constexpr std::uint32_t slot = 102, ordinal = 23;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Ulong;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "ExQueryPoolBlockSize";
};
// slot 103, Ord 137 (0x089), FUNC stdcall, args 1, purge 4, returns boolean, plan ReplacedLibs, only replaced libs
struct KeRemoveQueueDpc {
    static constexpr std::uint32_t slot = 103, ordinal = 137;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "KeRemoveQueueDpc";
};
// slot 104, Ord 153 (0x099), FUNC stdcall, args 3, purge 12, returns boolean, plan ReplacedLibs, only replaced libs
struct KeSynchronizeExecution {
    static constexpr std::uint32_t slot = 104, ordinal = 153;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Boolean;
    static constexpr std::uint32_t argDwords = 3, regArgs = 0, purgeBytes = 12;
    static constexpr const char* name = "KeSynchronizeExecution";
};
// slot 105, Ord 312 (0x138), FUNC stdcall, args 4, purge 16, returns void, plan M1
struct RtlUnwind {
    static constexpr std::uint32_t slot = 105, ordinal = 312;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 4, regArgs = 0, purgeBytes = 16;
    static constexpr const char* name = "RtlUnwind";
};
// slot 106, Ord 95 (0x05F), FUNC stdcall, args 1, purge 4, returns void, plan M1
struct KeBugCheck {
    static constexpr std::uint32_t slot = 106, ordinal = 95;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 1, regArgs = 0, purgeBytes = 4;
    static constexpr const char* name = "KeBugCheck";
};
// slot 107, Ord 169 (0x0A9), FUNC stdcall, args 2, purge 8, returns pointer, plan M2
struct MmCreateKernelStack {
    static constexpr std::uint32_t slot = 107, ordinal = 169;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Pointer;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "MmCreateKernelStack";
};
// slot 108, Ord 170 (0x0AA), FUNC stdcall, args 2, purge 8, returns void, plan M2
struct MmDeleteKernelStack {
    static constexpr std::uint32_t slot = 108, ordinal = 170;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 2, regArgs = 0, purgeBytes = 8;
    static constexpr const char* name = "MmDeleteKernelStack";
};
// slot 109, Ord 5 (0x005), FUNC stdcall, args 0, purge 0, returns void, plan M2
struct DbgBreakPoint {
    static constexpr std::uint32_t slot = 109, ordinal = 5;
    static constexpr gen::Kind kind = gen::Kind::Func;
    static constexpr gen::Conv conv = gen::Conv::Stdcall;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "DbgBreakPoint";
};
// slot 110, Ord 356 (0x164), DATA, plan M1
struct HalBootSMCVideoMode {
    static constexpr std::uint32_t slot = 110, ordinal = 356;
    static constexpr gen::Kind kind = gen::Kind::Data;
    static constexpr gen::Conv conv = gen::Conv::None;
    static constexpr gen::Ret ret = gen::Ret::Void;
    static constexpr std::uint32_t argDwords = 0, regArgs = 0, purgeBytes = 0;
    static constexpr const char* name = "HalBootSMCVideoMode";
};

}  // namespace krnl::exp
