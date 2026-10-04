// Guest-visible Xbox kernel structures shared by several host modules.
// All structs are POD with the exact Xbox layout (32-bit pointers = host pointers, x86 only).
// Names follow the xboxkrnl headers; layouts are verified against the guest code where noted.
// Use them as xbox::ANSI_STRING etc. (never `using namespace xbox;` - Windows headers define
// NT structs with the same names but different layouts).
#pragma once

#include "shared/win32.h"

#include <cstddef>
#include <cstdint>

namespace xbox {

using NTSTATUS = ::NTSTATUS;
using KIRQL = std::uint8_t;
using ACCESS_MASK = std::uint32_t;
using LARGE_INTEGER = ::LARGE_INTEGER;  // identical layout on Xbox and Windows (8 bytes)
using PLARGE_INTEGER = ::LARGE_INTEGER*;

static_assert(sizeof(LARGE_INTEGER) == 8);

// ---------------------------------------------------------------------------------------------
// Strings. On the Xbox all object names are ANSI: OBJECT_ATTRIBUTES.ObjectName is a
// POBJECT_STRING = PANSI_STRING (STRING). Evidence: XAPI CreateFileA 0xF76E0 calls
// RtlInitAnsiString on the path and passes it as ObjectName (files.md 5.1 item 1, kernel.md 7).
// ---------------------------------------------------------------------------------------------
struct ANSI_STRING {
    std::uint16_t Length;         // bytes, without terminator
    std::uint16_t MaximumLength;  // bytes, including terminator if any
    char* Buffer;
};
static_assert(sizeof(ANSI_STRING) == 8);
static_assert(offsetof(ANSI_STRING, Length) == 0);
static_assert(offsetof(ANSI_STRING, MaximumLength) == 2);
static_assert(offsetof(ANSI_STRING, Buffer) == 4);
using STRING = ANSI_STRING;
using OBJECT_STRING = ANSI_STRING;
using PANSI_STRING = ANSI_STRING*;
using POBJECT_STRING = ANSI_STRING*;

// Present for completeness; the guest's kernel calls use ANSI names (see above).
struct UNICODE_STRING {
    std::uint16_t Length;         // bytes
    std::uint16_t MaximumLength;  // bytes
    wchar_t* Buffer;
};
static_assert(sizeof(UNICODE_STRING) == 8);
static_assert(offsetof(UNICODE_STRING, Buffer) == 4);

// ---------------------------------------------------------------------------------------------
// OBJECT_ATTRIBUTES (Xbox): {RootDirectory, ObjectName, Attributes} - no Length, no security
// fields. Evidence: CreateFileA 0xF76E0 builds it on the stack as local_24 = 0xFFFFFFFD
// (RootDirectory), local_20 = &ansiString (ObjectName), local_1c = 0x40 (Attributes) and passes
// &local_24 to NtCreateFile (decomp _text_000f0000.c); files.md 5.1 item 1; design 4.2 item 1.
// RootDirectory == (HANDLE)-3 means "relative to \??\" (ObDosDevicesDirectory).
// ---------------------------------------------------------------------------------------------
struct OBJECT_ATTRIBUTES {
    HANDLE RootDirectory;
    POBJECT_STRING ObjectName;
    std::uint32_t Attributes;
};
static_assert(sizeof(OBJECT_ATTRIBUTES) == 12);
static_assert(offsetof(OBJECT_ATTRIBUTES, RootDirectory) == 0);
static_assert(offsetof(OBJECT_ATTRIBUTES, ObjectName) == 4);
static_assert(offsetof(OBJECT_ATTRIBUTES, Attributes) == 8);
using POBJECT_ATTRIBUTES = OBJECT_ATTRIBUTES*;

inline constexpr std::uint32_t OBJ_CASE_INSENSITIVE = 0x40;
inline const HANDLE ObDosDevicesDirectory = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-3));
inline const HANDLE NtCurrentProcess = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1));
inline const HANDLE NtCurrentThread = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-2));

// ---------------------------------------------------------------------------------------------
// IO_STATUS_BLOCK: {Status/Pointer, Information}. XAPI ReadFileEx/WriteFileEx pass the guest
// OVERLAPPED as IoStatusBlock (Internal = Status, InternalHigh = Information; files.md 5.1 item 2).
// ---------------------------------------------------------------------------------------------
struct IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        void* Pointer;
    };
    std::uint32_t Information;
};
static_assert(sizeof(IO_STATUS_BLOCK) == 8);
static_assert(offsetof(IO_STATUS_BLOCK, Information) == 4);
using PIO_STATUS_BLOCK = IO_STATUS_BLOCK*;

// Guest OVERLAPPED as used by XAPI ReadFileEx/WriteFileEx (same layout as Win32 x86).
struct OVERLAPPED {
    std::uint32_t Internal;      // = IO_STATUS_BLOCK.Status
    std::uint32_t InternalHigh;  // = IO_STATUS_BLOCK.Information
    std::uint32_t Offset;
    std::uint32_t OffsetHigh;
    HANDLE hEvent;
};
static_assert(sizeof(OVERLAPPED) == 20);

// ---------------------------------------------------------------------------------------------
// Fake KTHREAD / KPRCB reachable through the TLS slots S_KTHREAD / S_PRCB (fs:[0x28] / fs:[0x20]).
// Only the fields the guest reads are named; everything else stays zero (cpu.md 2.2, design 1.6).
// ---------------------------------------------------------------------------------------------
struct FakeKThread {
    std::uint8_t Reserved00[0x1C];
    std::uint32_t StackBase;   // +0x1C  read by ConvertThreadToFiber 0x1709AF
    std::uint32_t StackLimit;  // +0x20
    std::uint8_t Reserved24[0x04];
    std::uint32_t TlsData;     // +0x28  XapiThreadStartup 0xF99F3, _getptd 0x1692EC; 0 on the boot thread
    std::uint8_t Reserved2C[0x12C - 0x2C];
    std::uint32_t ThreadId;    // +0x12C GetCurrentThreadId 0xF9AE6
    std::uint8_t Reserved130[0x140 - 0x130];
};
static_assert(sizeof(FakeKThread) == 0x140);
static_assert(offsetof(FakeKThread, StackBase) == 0x1C);
static_assert(offsetof(FakeKThread, StackLimit) == 0x20);
static_assert(offsetof(FakeKThread, TlsData) == 0x28);
static_assert(offsetof(FakeKThread, ThreadId) == 0x12C);

struct FakeKPrcb {
    std::uint8_t Reserved000[0x250];
    std::uint32_t DebugMonitorData;  // +0x250 must be 0: all debug-monitor hooks off (cpu.md 2.2)
    std::uint8_t Reserved254[0x260 - 0x254];
};
static_assert(sizeof(FakeKPrcb) >= 0x254);
static_assert(offsetof(FakeKPrcb, DebugMonitorData) == 0x250);

// TLS layout of an XAPI thread: TlsData is TlsDataSize (0x24) bytes; [TlsData] = TlsData + 4 =
// TLS block of 0x20 bytes (+0x04 LastError, +0x08 CRT ptd, +0x0C current fiber, +0x10.. fiber of
// ConvertThreadToFiber). The guest reads the block pointer as [S_TLS + _tls_index*4] with
// _tls_index = -9, so S_TLS = TlsData + 0x24 (cpu.md 2.2, design 1.6).
inline constexpr std::uint32_t kTlsDataSize = 0x24;
inline constexpr std::int32_t kTlsIndex = -9;
inline constexpr std::uint32_t kTlsBlockOffsetLastError = 0x04;
inline constexpr std::uint32_t kTlsBlockOffsetCrtPtd = 0x08;
inline constexpr std::uint32_t kTlsBlockOffsetCurrentFiber = 0x0C;

// IRQL values used by the guest (KfRaiseIrql / _getptd compare against DISPATCH_LEVEL = 2).
// (Named kIrql* because winnt.h defines PASSIVE_LEVEL etc. as macros.)
inline constexpr KIRQL kIrqlPassive = 0;
inline constexpr KIRQL kIrqlApc = 1;
inline constexpr KIRQL kIrqlDispatch = 2;

}  // namespace xbox
