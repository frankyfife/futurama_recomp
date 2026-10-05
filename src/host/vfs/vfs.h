// Virtual file system interfaces (design 4, E8): object namespace, devices, symbolic links, open
// files and the XDVDFS disc image reader. Implemented by the io agent (vfs/*.cpp); the kernel file
// exports (kernel/io_*.cpp) translate guest NT calls into these interfaces.
//
// Paths: object paths use '\' (NormalizePath converts '/'), comparisons are ASCII case-insensitive
// (design 4.2 item 2). A device-relative path starts with '\' ("\" = device root).
#pragma once

#include "shared/xbox_types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vfs {

// Well-known object names (design 4.1).
inline constexpr char kCdRom0[] = "\\Device\\CdRom0";
inline constexpr char kPartition1[] = "\\Device\\Harddisk0\\partition1";
inline constexpr char kDosDevicesPrefix[] = "\\??\\";

// ---- path helpers ----------------------------------------------------------------------------
std::string NormalizePath(std::string_view path);  // '/' -> '\', no duplicate separators
bool IEquals(std::string_view a, std::string_view b) noexcept;
bool IStartsWith(std::string_view s, std::string_view prefix) noexcept;

// ---- information records (host side; kernel/io_* converts them to the guest FILE_* classes) ----
struct FileInfo {
    std::uint64_t size = 0;
    std::uint64_t allocationSize = 0;
    std::uint32_t attributes = 0;  // FILE_ATTRIBUTE_* (0x01 read-only, 0x10 directory, 0x80 normal)
    std::int64_t creationTime = 0;  // NT time (100 ns since 1601)
    std::int64_t lastAccessTime = 0;
    std::int64_t lastWriteTime = 0;
    std::int64_t changeTime = 0;
    bool isDirectory = false;
};

struct DirEntry {
    std::string name;  // ANSI file name without path
    FileInfo info;
};

struct VolumeSizeInfo {  // FileFsSizeInformation (class 3, design 4.3)
    std::uint64_t totalAllocationUnits = 0;
    std::uint64_t availableAllocationUnits = 0;
    std::uint32_t sectorsPerAllocationUnit = 0;
    std::uint32_t bytesPerSector = 0;
};

struct OpenRequest {  // device-relevant NtCreateFile/NtOpenFile parameters (raw NT values)
    std::uint32_t desiredAccess = 0;
    std::uint32_t fileAttributes = 0;
    std::uint32_t shareAccess = 0;
    std::uint32_t createDisposition = 1;  // FILE_OPEN
    std::uint32_t createOptions = 0;
    std::uint64_t allocationSize = 0;
};

// ---- open file / directory on a device ------------------------------------------------------
class File {
public:
    virtual ~File() = default;
    virtual bool IsDirectory() const noexcept = 0;
    // Positional I/O (no shared file pointer; the kernel layer keeps the position for synchronous
    // handles). Reads past EOF succeed with a smaller `transferred` (design 4.2 item 4).
    virtual NTSTATUS Read(std::uint64_t offset, void* buffer, std::uint32_t length, std::uint32_t& transferred) noexcept = 0;
    virtual NTSTATUS Write(std::uint64_t offset, const void* buffer, std::uint32_t length,
                           std::uint32_t& transferred) noexcept = 0;
    virtual NTSTATUS QueryInfo(FileInfo& info) noexcept = 0;
    virtual NTSTATUS SetEndOfFile(std::uint64_t size) noexcept = 0;
    virtual NTSTATUS SetTimesAndAttributes(const FileInfo& info) noexcept = 0;  // class 4 (Basic)
    virtual NTSTATUS SetDeleteOnClose(bool deleteOnClose) noexcept = 0;         // class 0xD (Disposition)
    virtual NTSTATUS Flush() noexcept = 0;
    // Directory enumeration, one entry per call (NtQueryDirectoryFile class 1, no "." / "..").
    // `mask` uses '*' and '?'; restart = rewind. STATUS_NO_MORE_FILES at the end.
    virtual NTSTATUS NextDirEntry(std::string_view mask, bool restart, DirEntry& out) noexcept = 0;
};

// ---- device object ---------------------------------------------------------------------------
class Device {
public:
    virtual ~Device() = default;
    virtual const char* Kind() const noexcept = 0;  // "IsoDevice", "HostDirDevice", ...
    virtual bool IsReadOnly() const noexcept = 0;
    // createAction receives FILE_SUPERSEDED (0), FILE_OPENED (1), FILE_CREATED (2), FILE_OVERWRITTEN (3).
    virtual NTSTATUS Open(std::string_view relativePath, const OpenRequest& request, std::unique_ptr<File>& file,
                          std::uint32_t& createAction) noexcept = 0;
    virtual NTSTATUS QueryAttributes(std::string_view relativePath, FileInfo& info) noexcept = 0;
    virtual NTSTATUS QueryVolumeSize(VolumeSizeInfo& info) noexcept = 0;
};

// ---- XDVDFS disc image reader (design 4.2 item 4, tools/xdvdfs.py) ----------------------------
class XdvdfsImage {
public:
    struct Entry {
        std::string name;
        std::uint32_t sector = 0;  // first sector (2048 bytes) relative to the partition start
        std::uint32_t size = 0;    // bytes (directories: size of the directory table)
        std::uint8_t attributes = 0;  // FILE_ATTRIBUTE_* (0x10 = directory)
        bool IsDirectory() const noexcept { return (attributes & 0x10) != 0; }
    };

    XdvdfsImage();
    ~XdvdfsImage();
    XdvdfsImage(const XdvdfsImage&) = delete;
    XdvdfsImage& operator=(const XdvdfsImage&) = delete;

    // Opens the image read-only and finds the volume descriptor "MICROSOFT*XBOX*MEDIA" in sector 32
    // relative to the partition base (0 for XISO, 0x18300000 for a redump image).
    bool Open(const std::filesystem::path& image, std::string& error);
    bool IsOpen() const noexcept;
    std::uint64_t PartitionOffset() const noexcept;
    // Case-insensitive lookup of a path relative to the disc root ("default.xbe", "\Video\a.xmv").
    bool Lookup(std::string_view path, Entry& out) const;
    bool ListDirectory(std::string_view path, std::vector<Entry>& out) const;
    // Positional read; reads past the end of the file are clipped (transferred < length).
    bool Read(const Entry& file, std::uint64_t offset, void* buffer, std::uint32_t length,
              std::uint32_t& transferred) const;

    // (added by io) Timestamp of the volume descriptor (NT time; XDVDFS has no per-file times, so it
    // is reported for every entry), bytes from the partition start to the end of the image file, and
    // the number of entries (files + directories) in the tree.
    std::int64_t VolumeTimestamp() const noexcept;
    std::uint64_t PartitionSize() const noexcept;
    std::uint32_t EntryCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reads a whole file from a disc image (core XBE loader: "default.xbe").
bool ReadIsoFile(const std::filesystem::path& image, std::string_view path, std::vector<std::uint8_t>& out,
                 std::string& error);

// ---- device factories ------------------------------------------------------------------------
std::shared_ptr<Device> CreateIsoDevice(std::shared_ptr<XdvdfsImage> image);
std::shared_ptr<Device> CreateHostDirDevice(const std::filesystem::path& root, bool readOnly);
// Creates `dir` and all missing parents through the "\\?\" form of the path, so the MAX_PATH limit
// does not apply (review finding 11; HostDirDevice opens its files the same way). Returns false with
// a message if a component cannot be created or is not a directory.
bool CreateHostDirectories(const std::filesystem::path& dir, std::string& error);

// ---- object namespace (design 4.1) -----------------------------------------------------------
// Device objects, e.g. AddDevice("\\Device\\CdRom0", iso). Names are case-insensitive.
bool AddDevice(std::string_view name, std::shared_ptr<Device> device);
std::shared_ptr<Device> FindDevice(std::string_view name);
// Symbolic links (IoCreateSymbolicLink): "\\??\\D:" -> "\\Device\\CdRom0".
// STATUS_OBJECT_NAME_COLLISION if the link exists (design 4.1).
NTSTATUS CreateSymbolicLink(std::string_view link, std::string_view target);
NTSTATUS DeleteSymbolicLink(std::string_view link);
bool QuerySymbolicLink(std::string_view link, std::string& target);

struct ResolvedPath {
    std::shared_ptr<Device> device;
    std::string devicePath;    // e.g. "\\Device\\Harddisk0\\partition1"
    std::string relativePath;  // e.g. "\\UDATA\\46490002\\x" ("\\" for the device root)
};
// Resolves an absolute object path (RootDirectory already applied): normalizes, follows symbolic
// links, then picks the longest device prefix. STATUS_OBJECT_NAME_NOT_FOUND /
// STATUS_OBJECT_PATH_NOT_FOUND if nothing matches.
NTSTATUS Resolve(std::string_view objectPath, ResolvedPath& out);

// (added by io) Case-insensitive wildcard match of one file name against an NtQueryDirectoryFile
// mask ('*', '?'; the DOS forms '<' '>' '"' behave like '*' '?' '.'); an empty mask matches all.
bool MatchFileMask(std::string_view name, std::string_view mask) noexcept;
// (added by io) Removes all devices and symbolic links (unit tests; never called by the loader).
void ResetNamespace();

}  // namespace vfs
