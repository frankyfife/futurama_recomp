// xboxkrnl file-I/O exports implemented by the io agent (kernel/io_*.cpp; design 4.3, Anhang A).
// Argument lists are the Xbox ones (kernel_imports.tsv `args`, verified by KRNL_FUNC against the
// generated purge): ANSI object names, no EaBuffer/EaLength, no Key, NtQueryDirectoryFile without
// ReturnSingleEntry. All entries are __stdcall and noexcept.
#pragma once

#include "kernel/io_types.h"

#include <cstdint>

namespace krnl {

// slot 25, Ord 190 - 9 args (purge 36).
NTSTATUS __stdcall NtCreateFile(HANDLE* FileHandle, xbox::ACCESS_MASK DesiredAccess,
                                xbox::OBJECT_ATTRIBUTES* ObjectAttributes,
                                xbox::IO_STATUS_BLOCK* IoStatusBlock, xbox::LARGE_INTEGER* AllocationSize,
                                ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition,
                                ULONG CreateOptions) noexcept;
// slot 43, Ord 202 - 6 args (purge 24).
NTSTATUS __stdcall NtOpenFile(HANDLE* FileHandle, xbox::ACCESS_MASK DesiredAccess,
                              xbox::OBJECT_ATTRIBUTES* ObjectAttributes, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                              ULONG ShareAccess, ULONG OpenOptions) noexcept;
// slot 34, Ord 219 - 8 args (purge 32).
NTSTATUS __stdcall NtReadFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine, void* ApcContext,
                              xbox::IO_STATUS_BLOCK* IoStatusBlock, void* Buffer, ULONG Length,
                              xbox::LARGE_INTEGER* ByteOffset) noexcept;
// slot 35, Ord 236 - 8 args (purge 32).
NTSTATUS __stdcall NtWriteFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine, void* ApcContext,
                               xbox::IO_STATUS_BLOCK* IoStatusBlock, const void* Buffer, ULONG Length,
                               xbox::LARGE_INTEGER* ByteOffset) noexcept;
// slot 36, Ord 211 - 5 args (purge 20).
NTSTATUS __stdcall NtQueryInformationFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                          void* FileInformation, ULONG Length,
                                          ULONG FileInformationClass) noexcept;
// slot 22, Ord 226 - 5 args (purge 20).
NTSTATUS __stdcall NtSetInformationFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                        void* FileInformation, ULONG Length,
                                        ULONG FileInformationClass) noexcept;
// slot 37, Ord 218 - 5 args (purge 20).
NTSTATUS __stdcall NtQueryVolumeInformationFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                                void* FsInformation, ULONG Length,
                                                ULONG FsInformationClass) noexcept;
// slot 38, Ord 198 - 2 args (purge 8).
NTSTATUS __stdcall NtFlushBuffersFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock) noexcept;
// slot 45, Ord 210 - 2 args (purge 8).
NTSTATUS __stdcall NtQueryFullAttributesFile(xbox::OBJECT_ATTRIBUTES* ObjectAttributes,
                                             io::GuestFileNetworkOpenInformation* FileInformation) noexcept;
// slot 46, Ord 207 - 10 args (purge 40).
NTSTATUS __stdcall NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine,
                                        void* ApcContext, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                        void* FileInformation, ULONG Length, ULONG FileInformationClass,
                                        xbox::OBJECT_STRING* FileName, BOOLEAN RestartScan) noexcept;
// slot 41, Ord 196 - 10 args (purge 40).
NTSTATUS __stdcall NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine,
                                         void* ApcContext, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                         ULONG IoControlCode, void* InputBuffer, ULONG InputBufferLength,
                                         void* OutputBuffer, ULONG OutputBufferLength) noexcept;
// slot 40, Ord 200 - 10 args (purge 40).
NTSTATUS __stdcall NtFsControlFile(HANDLE FileHandle, HANDLE Event, io::IoApcRoutine ApcRoutine,
                                   void* ApcContext, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                   ULONG FsControlCode, void* InputBuffer, ULONG InputBufferLength,
                                   void* OutputBuffer, ULONG OutputBufferLength) noexcept;
// slot 48, Ord 67 - 2 args (purge 8).
NTSTATUS __stdcall IoCreateSymbolicLink(xbox::OBJECT_STRING* SymbolicLinkName,
                                        xbox::OBJECT_STRING* DeviceName) noexcept;

}  // namespace krnl
