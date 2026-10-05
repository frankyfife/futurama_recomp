// Internal helpers of the vfs module (io agent). Not part of the cross-module contract (vfs.h).
#pragma once

#include "vfs/vfs.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vfs::detail {

// NT file attribute bits used by the guest (FATX/XDVDFS keep the FAT meanings).
inline constexpr std::uint32_t kAttrReadOnly = 0x01;
inline constexpr std::uint32_t kAttrHidden = 0x02;
inline constexpr std::uint32_t kAttrSystem = 0x04;
inline constexpr std::uint32_t kAttrDirectory = 0x10;
inline constexpr std::uint32_t kAttrArchive = 0x20;
inline constexpr std::uint32_t kAttrNormal = 0x80;
// Attributes the guest may store on a host file (FILE_ATTRIBUTE_VALID_SET_FLAGS of FATX).
inline constexpr std::uint32_t kAttrSettableMask = kAttrReadOnly | kAttrHidden | kAttrSystem | kAttrArchive;

// NtCreateFile CreateDisposition / CreateOptions / create actions (ntioapi.h; files.md 2.5).
inline constexpr std::uint32_t kFileSupersede = 0;
inline constexpr std::uint32_t kFileOpen = 1;
inline constexpr std::uint32_t kFileCreate = 2;
inline constexpr std::uint32_t kFileOpenIf = 3;
inline constexpr std::uint32_t kFileOverwrite = 4;
inline constexpr std::uint32_t kFileOverwriteIf = 5;

inline constexpr std::uint32_t kOptDirectoryFile = 0x00000001;
inline constexpr std::uint32_t kOptNonDirectoryFile = 0x00000040;
inline constexpr std::uint32_t kOptDeleteOnClose = 0x00001000;

inline constexpr std::uint32_t kActionSuperseded = 0;
inline constexpr std::uint32_t kActionOpened = 1;
inline constexpr std::uint32_t kActionCreated = 2;
inline constexpr std::uint32_t kActionOverwritten = 3;

// Access-mask groups (winnt.h): data writes (FILE_WRITE_DATA, FILE_APPEND_DATA, GENERIC_WRITE,
// GENERIC_ALL), attribute/EA writes, DELETE, MAXIMUM_ALLOWED.
inline constexpr std::uint32_t kAccessWriteData = 0x00000002 | 0x00000004 | 0x40000000 | 0x10000000;
inline constexpr std::uint32_t kAccessWriteAttributes = 0x00000010 | 0x00000100;
inline constexpr std::uint32_t kAccessDelete = 0x00010000;
inline constexpr std::uint32_t kAccessMaximumAllowed = 0x02000000;
inline constexpr std::uint32_t kAccessAnyWrite = kAccessWriteData | kAccessWriteAttributes | kAccessDelete |
                                                 kAccessMaximumAllowed | 0x00040000 /*WRITE_DAC*/ |
                                                 0x00080000 /*WRITE_OWNER*/;

// FATX limit (files.md 5.3, design 4.2 item 5).
inline constexpr std::size_t kFatxMaxNameLength = 42;

// Host volume geometry reported for host-directory devices (design 4.3: 32 x 512 = 0x4000) and the
// cap on the reported free space (< 4 GiB because of the guest's `high | low` bug, files.md 4.4).
inline constexpr std::uint32_t kHostSectorsPerCluster = 32;
inline constexpr std::uint32_t kHostBytesPerSector = 512;
inline constexpr std::uint32_t kHostClusterSize = kHostSectorsPerCluster * kHostBytesPerSector;
inline constexpr std::uint64_t kHostMaxFreeBytes = 0xFFFF0000ull;

// Disc geometry (design 4.3: sector 2048 for D:).
inline constexpr std::uint32_t kDiscSectorSize = 2048;

// Splits a device-relative path ("\a\b", "a/b", "\") into its components (empty list = root).
std::vector<std::string> SplitPath(std::string_view relativePath);
// Joins components to a device-relative path ("\a\b"; "\" for none).
std::string JoinPath(const std::vector<std::string>& components);
// ASCII lower-case copy (the guest's names are ANSI; case folding is ASCII-only like RtlUpperChar).
std::string ToLowerAscii(std::string_view s);

// Guest ANSI <-> host UTF-16. The guest code page is treated as Latin-1 (byte n <-> U+00nn) so the
// mapping is lossless and independent of the host ANSI code page. WideToGuest fails (false) for
// characters above U+00FF.
std::wstring GuestToWide(std::string_view s);
bool WideToGuest(std::wstring_view s, std::string& out);

// Win32 error -> NTSTATUS for host-directory devices (unknown errors -> STATUS_UNSUCCESSFUL).
NTSTATUS Win32ErrorToStatus(unsigned long error) noexcept;
std::string StatusText(NTSTATUS status);  // "STATUS_..." or "0x........"

// A device without files: opening its root works, every other name is STATUS_OBJECT_NAME_NOT_FOUND.
// Used for \Device\CdRom0 when no disc image is configured (design 6, M1: "CdRom0 als Geraet ohne Dateien").
std::shared_ptr<Device> CreateEmptyDevice(const char* kind);

// (M2, VFS trace) Why a name the guest looks up does not exist, if that is documented in the analysis
// (files.md 1.2/3.2/4.1: FutPAL05.img ends the archive mount loop, Video/*.txt is optional, ...);
// nullptr for an undocumented miss. `devicePath` "\Device\CdRom0", `relativePath` "\FutPAL05.img".
// Table in vfs_known_missing.cpp.
const char* DocumentedMissReason(std::string_view devicePath, std::string_view relativePath) noexcept;

}  // namespace vfs::detail
