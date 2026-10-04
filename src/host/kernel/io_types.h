// Guest-visible structures and constants of the Xbox file-I/O kernel exports (io agent).
//
// Layouts are those of the NT DDK (identical on the Xbox, except that names are ANSI); every offset
// that XAPI reads is asserted below together with the XAPI code that reads it (decomp
// _text_000f0000.c). Guest buffers are not necessarily 8-byte aligned (stack locals), so the kernel
// layer fills a local copy and memcpy's it into guest memory.
#pragma once

#include "shared/xbox_types.h"

#include <cstddef>
#include <cstdint>

namespace krnl::io {

// ---- FILE_INFORMATION_CLASS values used by XAPI (files.md 2.2, 5.1 item 4) ----------------------
inline constexpr std::uint32_t kFileDirectoryInformation = 1;  // NtQueryDirectoryFile (FindFirstFileA)
inline constexpr std::uint32_t kFileBasicInformation = 4;      // SetFileAttributesA, CopyFileExA, MapLetter
inline constexpr std::uint32_t kFileStandardInformation = 5;
inline constexpr std::uint32_t kFileDispositionInformation = 13;  // 0xD RemoveDirectoryA, recursive delete
inline constexpr std::uint32_t kFilePositionInformation = 14;     // 0xE SetFilePointer(Ex), SetEndOfFile
inline constexpr std::uint32_t kFileAllocationInformation = 19;   // 0x13 SetEndOfFile
inline constexpr std::uint32_t kFileEndOfFileInformation = 20;    // 0x14 SetEndOfFile, CopyFileExA
inline constexpr std::uint32_t kFileNetworkOpenInformation =
    34;  // 0x22 GetFileSize(Ex), SetFilePointer(FILE_END)

// ---- FS_INFORMATION_CLASS ----
inline constexpr std::uint32_t kFileFsSizeInformation =
    3;  // GetDiskFreeSpaceExA, XGetDiskCluster/SectorSizeA
inline constexpr std::uint32_t kFileFsFullSizeInformation = 7;  // (not used by XAPI; cheap)

// ---- special ByteOffset values of NtReadFile/NtWriteFile (ntioapi.h) ----
inline constexpr std::uint32_t kFileUseFilePointerPositionLow = 0xFFFFFFFE;  // HighPart -1
inline constexpr std::uint32_t kFileWriteToEndOfFileLow = 0xFFFFFFFF;        // HighPart -1

// ---- CreateOptions ----
inline constexpr std::uint32_t kOptSynchronousIoAlert = 0x00000010;
inline constexpr std::uint32_t kOptSynchronousIoNonAlert = 0x00000020;

// PIO_APC_ROUTINE (stdcall, 3 args): NtUserIoApcDispatcher(ApcContext = completion routine,
// IoStatusBlock = OVERLAPPED, Reserved) (files.md 5.1 item 2, design 1.7).
using IoApcRoutine = void(__stdcall*)(void* apcContext, xbox::IO_STATUS_BLOCK* ioStatusBlock,
                                      std::uint32_t reserved);

#pragma pack(push, 8)

// FileBasicInformation (0x28). XAPI: FUN_000f7b6c writes FileAttributes at +0x20 (local_48/local_28),
// XapiMapLetterToDirectory writes LastWriteTime at +0x10 (local_58/local_48).
struct GuestFileBasicInformation {
    xbox::LARGE_INTEGER CreationTime;
    xbox::LARGE_INTEGER LastAccessTime;
    xbox::LARGE_INTEGER LastWriteTime;
    xbox::LARGE_INTEGER ChangeTime;
    std::uint32_t FileAttributes;
};
static_assert(sizeof(GuestFileBasicInformation) == 0x28);
static_assert(offsetof(GuestFileBasicInformation, LastWriteTime) == 0x10);
static_assert(offsetof(GuestFileBasicInformation, FileAttributes) == 0x20);

struct GuestFileStandardInformation {
    xbox::LARGE_INTEGER AllocationSize;
    xbox::LARGE_INTEGER EndOfFile;
    std::uint32_t NumberOfLinks;
    std::uint8_t DeletePending;
    std::uint8_t Directory;
};
static_assert(sizeof(GuestFileStandardInformation) == 0x18);

struct GuestFilePositionInformation {
    xbox::LARGE_INTEGER CurrentByteOffset;
};
static_assert(sizeof(GuestFilePositionInformation) == 8);

struct GuestFileDispositionInformation {
    std::uint8_t DeleteFile;  // BOOLEAN; XAPI passes Length 1 (FUN_000f8616)
};
static_assert(sizeof(GuestFileDispositionInformation) == 1);

struct GuestFileAllocationInformation {
    xbox::LARGE_INTEGER AllocationSize;
};
struct GuestFileEndOfFileInformation {
    xbox::LARGE_INTEGER EndOfFile;
};
static_assert(sizeof(GuestFileAllocationInformation) == 8 && sizeof(GuestFileEndOfFileInformation) == 8);

// FileNetworkOpenInformation (0x38). XAPI: GetFileSizeEx reads EndOfFile at +0x28 (local_44/local_1c),
// GetFileAttributesA reads FileAttributes at +0x30 (local_50/local_20).
struct GuestFileNetworkOpenInformation {
    xbox::LARGE_INTEGER CreationTime;
    xbox::LARGE_INTEGER LastAccessTime;
    xbox::LARGE_INTEGER LastWriteTime;
    xbox::LARGE_INTEGER ChangeTime;
    xbox::LARGE_INTEGER AllocationSize;
    xbox::LARGE_INTEGER EndOfFile;
    std::uint32_t FileAttributes;
};
static_assert(sizeof(GuestFileNetworkOpenInformation) == 0x38);
static_assert(offsetof(GuestFileNetworkOpenInformation, EndOfFile) == 0x28);
static_assert(offsetof(GuestFileNetworkOpenInformation, FileAttributes) == 0x30);

// FileDirectoryInformation with an ANSI name (Xbox). XAPI FUN_000f88f5 copies +0x38 attributes,
// +0x08/+0x10/+0x18 times, +0x2C/+0x28 EndOfFile high/low, +0x3C name length, +0x40 name.
struct GuestFileDirectoryInformation {
    std::uint32_t NextEntryOffset;
    std::uint32_t FileIndex;
    xbox::LARGE_INTEGER CreationTime;
    xbox::LARGE_INTEGER LastAccessTime;
    xbox::LARGE_INTEGER LastWriteTime;
    xbox::LARGE_INTEGER ChangeTime;
    xbox::LARGE_INTEGER EndOfFile;
    xbox::LARGE_INTEGER AllocationSize;
    std::uint32_t FileAttributes;
    std::uint32_t FileNameLength;  // bytes
    char FileName[1];
};
inline constexpr std::uint32_t kFileDirectoryInformationHeaderSize = 0x40;
static_assert(offsetof(GuestFileDirectoryInformation, CreationTime) == 0x08);
static_assert(offsetof(GuestFileDirectoryInformation, EndOfFile) == 0x28);
static_assert(offsetof(GuestFileDirectoryInformation, FileAttributes) == 0x38);
static_assert(offsetof(GuestFileDirectoryInformation, FileNameLength) == 0x3C);
static_assert(offsetof(GuestFileDirectoryInformation, FileName) == kFileDirectoryInformationHeaderSize);

// FileFsSizeInformation (0x18). XAPI: XGetDiskClusterSizeA (FUN_000f8081) multiplies +0x10 x +0x14,
// FUN_000f8ae2 requires the product 0x4000 for partition1, GetDiskFreeSpaceExA reads +0x00/+0x08.
struct GuestFileFsSizeInformation {
    xbox::LARGE_INTEGER TotalAllocationUnits;
    xbox::LARGE_INTEGER AvailableAllocationUnits;
    std::uint32_t SectorsPerAllocationUnit;
    std::uint32_t BytesPerSector;
};
static_assert(sizeof(GuestFileFsSizeInformation) == 0x18);
static_assert(offsetof(GuestFileFsSizeInformation, SectorsPerAllocationUnit) == 0x10);
static_assert(offsetof(GuestFileFsSizeInformation, BytesPerSector) == 0x14);

struct GuestFileFsFullSizeInformation {
    xbox::LARGE_INTEGER TotalAllocationUnits;
    xbox::LARGE_INTEGER CallerAvailableAllocationUnits;
    xbox::LARGE_INTEGER ActualAvailableAllocationUnits;
    std::uint32_t SectorsPerAllocationUnit;
    std::uint32_t BytesPerSector;
};
static_assert(sizeof(GuestFileFsFullSizeInformation) == 0x20);

#pragma pack(pop)

}  // namespace krnl::io
