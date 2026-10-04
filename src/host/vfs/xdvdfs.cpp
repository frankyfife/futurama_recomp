// XDVDFS (Xbox DVD file system) reader - C++ port of tools/xdvdfs.py (design 4.2 item 4, files.md 5.2).
//
// Layout (all little endian, sector = 2048 bytes, offsets relative to the partition start):
//   sector 32  volume descriptor: "MICROSOFT*XBOX*MEDIA" (20), root dir sector (u32), root dir size
//              (u32), FILETIME (u64), ..., magic again at 0x7EC
//   directory  table of entries forming a binary search tree; entry = u16 left, u16 right (child
//              offsets in DWORDs from the table start, 0 = none), u32 start sector, u32 size,
//              u8 attributes, u8 name length, name (ANSI). 0xFFFF/0xFFFF marks padding.
//   file       one contiguous extent: [sector * 2048, + size).
// The partition starts at 0 for a plain XISO (the configured retail EU image: files.md header) and at
// 0x18300000 for a redump image of an XGD1 disc; XGD2/XGD3 offsets are accepted as well.
//
// The whole tree (59 entries on the Futurama disc) is read once at Open(); afterwards the object is
// immutable and safe to use from several threads (positional ReadFile on a synchronous handle).
#include "vfs/vfs.h"
#include "vfs/vfs_internal.h"

#include "core/config.h"
#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_set>

namespace vfs {
namespace {

constexpr std::uint32_t kSectorSize = 2048;
constexpr std::uint32_t kVolumeDescriptorSector = 32;
constexpr char kVolumeMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr std::size_t kVolumeMagicLength = 20;
constexpr std::uint32_t kVolumeMagicTailOffset = 0x7EC;
// Partition offsets: XISO, XGD1 redump (design 4.2 item 4), XGD2, XGD3.
constexpr std::uint64_t kPartitionOffsets[] = {0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull};
constexpr std::uint32_t kMaxDirectoryTableSize = 16u << 20;  // sanity limit
constexpr int kMaxDirectoryDepth = 32;
constexpr std::size_t kDirEntryHeaderSize = 14;
constexpr std::uint8_t kAttrDirectory = 0x10;

std::uint16_t LoadU16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t LoadU32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

struct XdvdfsImage::Impl {
    struct Node {
        Entry entry;
        std::vector<Node> children;
    };

    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path path;
    std::uint64_t base = 0;
    std::uint64_t imageSize = 0;
    std::int64_t timestamp = 0;
    std::uint32_t entryCount = 0;
    Node root;
    mutable std::atomic<bool> warnedShortRead{false};

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }

    // Positional read from the image file; `got` < len at the end of the image. False on I/O errors.
    bool ReadAt(std::uint64_t offset, void* buffer, std::uint32_t length, std::uint32_t& got) const noexcept {
        got = 0;
        auto* out = static_cast<std::uint8_t*>(buffer);
        while (got < length) {
            OVERLAPPED ov{};
            const std::uint64_t pos = offset + got;
            ov.Offset = static_cast<DWORD>(pos);
            ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
            DWORD n = 0;
            if (!ReadFile(file, out + got, length - got, &n, &ov)) {
                if (GetLastError() == ERROR_HANDLE_EOF) {
                    return true;
                }
                return false;
            }
            if (n == 0) {
                return true;
            }
            got += n;
        }
        return true;
    }

    bool ParseDirectory(std::uint32_t sector, std::uint32_t size, int depth, std::vector<Node>& out,
                        std::string& error) {
        out.clear();
        if (size == 0 || sector == 0) {
            return true;  // empty directory
        }
        if (size > kMaxDirectoryTableSize) {
            error = "directory table too large (" + std::to_string(size) + " bytes at sector " +
                    std::to_string(sector) + ")";
            return false;
        }
        std::vector<std::uint8_t> table(size);
        std::uint32_t got = 0;
        if (!ReadAt(base + static_cast<std::uint64_t>(sector) * kSectorSize, table.data(), size, got) ||
            got != size) {
            error = "cannot read directory table at sector " + std::to_string(sector);
            return false;
        }
        // In-order walk of the search tree (left, node, right) -> entries sorted as stored on the disc.
        std::unordered_set<std::uint32_t> seen;
        std::vector<std::uint32_t> stack;
        std::uint32_t current = 0;
        bool hasCurrent = true;
        auto valid = [&](std::uint32_t off) {
            if (off + kDirEntryHeaderSize > size) {
                return false;
            }
            const std::uint8_t* e = table.data() + off;
            if (LoadU16(e) == 0xFFFF && LoadU16(e + 2) == 0xFFFF) {
                return false;  // padding
            }
            return off + kDirEntryHeaderSize + e[13] <= size;
        };
        while (hasCurrent || !stack.empty()) {
            while (hasCurrent) {
                if (!valid(current) || !seen.insert(current).second) {
                    hasCurrent = false;
                    break;
                }
                stack.push_back(current);
                const std::uint16_t left = LoadU16(table.data() + current);
                hasCurrent = left != 0;
                current = static_cast<std::uint32_t>(left) * 4;
            }
            if (stack.empty()) {
                break;
            }
            const std::uint32_t off = stack.back();
            stack.pop_back();
            const std::uint8_t* e = table.data() + off;
            Node node;
            node.entry.sector = LoadU32(e + 4);
            node.entry.size = LoadU32(e + 8);
            node.entry.attributes = e[12];
            node.entry.name.assign(reinterpret_cast<const char*>(e + kDirEntryHeaderSize), e[13]);
            if (node.entry.IsDirectory()) {
                if (depth >= kMaxDirectoryDepth) {
                    error = "directory tree too deep";
                    return false;
                }
                if (!ParseDirectory(node.entry.sector, node.entry.size, depth + 1, node.children, error)) {
                    return false;
                }
            }
            ++entryCount;
            out.push_back(std::move(node));
            const std::uint16_t right = LoadU16(e + 2);
            hasCurrent = right != 0;
            current = static_cast<std::uint32_t>(right) * 4;
        }
        return true;
    }

    const Node* Find(std::string_view relativePath) const {
        const Node* node = &root;
        for (const std::string& part : detail::SplitPath(relativePath)) {
            if (!node->entry.IsDirectory()) {
                return nullptr;
            }
            const Node* next = nullptr;
            for (const Node& child : node->children) {
                if (IEquals(child.entry.name, part)) {
                    next = &child;
                    break;
                }
            }
            if (next == nullptr) {
                return nullptr;
            }
            node = next;
        }
        return node;
    }
};

XdvdfsImage::XdvdfsImage() = default;
XdvdfsImage::~XdvdfsImage() = default;

bool XdvdfsImage::Open(const std::filesystem::path& image, std::string& error) {
    impl_.reset();
    auto impl = std::make_unique<Impl>();
    impl->path = image;
    impl->file = CreateFileW(image.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (impl->file == INVALID_HANDLE_VALUE) {
        error = "cannot open disc image (Win32 error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(impl->file, &fileSize)) {
        error = "cannot query the size of the disc image";
        return false;
    }
    impl->imageSize = static_cast<std::uint64_t>(fileSize.QuadPart);

    std::uint8_t vd[kSectorSize];
    bool found = false;
    for (std::uint64_t base : kPartitionOffsets) {
        const std::uint64_t vdOffset =
            base + static_cast<std::uint64_t>(kVolumeDescriptorSector) * kSectorSize;
        std::uint32_t got = 0;
        if (vdOffset + kSectorSize > impl->imageSize || !impl->ReadAt(vdOffset, vd, kSectorSize, got) ||
            got != kSectorSize) {
            continue;
        }
        if (std::memcmp(vd, kVolumeMagic, kVolumeMagicLength) == 0) {
            impl->base = base;
            found = true;
            break;
        }
    }
    if (!found) {
        error =
            "no XDVDFS volume descriptor (MICROSOFT*XBOX*MEDIA) at sector 32 of any known partition offset";
        return false;
    }
    if (std::memcmp(vd + kVolumeMagicTailOffset, kVolumeMagic, kVolumeMagicLength) != 0) {
        FUT_LOGW(Vfs, "XDVDFS: trailing volume descriptor magic missing (continuing)");
    }
    const std::uint32_t rootSector = LoadU32(vd + 20);
    const std::uint32_t rootSize = LoadU32(vd + 24);
    std::memcpy(&impl->timestamp, vd + 28, sizeof(impl->timestamp));

    impl->root.entry.name.clear();
    impl->root.entry.sector = rootSector;
    impl->root.entry.size = rootSize;
    impl->root.entry.attributes = kAttrDirectory;
    if (!impl->ParseDirectory(rootSector, rootSize, 0, impl->root.children, error)) {
        error = "XDVDFS: " + error;
        return false;
    }
    FUT_LOGD(Vfs, "XDVDFS %s: partition offset 0x%llX, root sector %u (%u bytes), %u entries",
             core::WideToUtf8(image.wstring()).c_str(), static_cast<unsigned long long>(impl->base),
             rootSector, rootSize, impl->entryCount);
    impl_ = std::move(impl);
    return true;
}

bool XdvdfsImage::IsOpen() const noexcept {
    return impl_ != nullptr;
}

std::uint64_t XdvdfsImage::PartitionOffset() const noexcept {
    return impl_ ? impl_->base : 0;
}

std::int64_t XdvdfsImage::VolumeTimestamp() const noexcept {
    return impl_ ? impl_->timestamp : 0;
}

std::uint64_t XdvdfsImage::PartitionSize() const noexcept {
    return impl_ && impl_->imageSize > impl_->base ? impl_->imageSize - impl_->base : 0;
}

std::uint32_t XdvdfsImage::EntryCount() const noexcept {
    return impl_ ? impl_->entryCount : 0;
}

bool XdvdfsImage::Lookup(std::string_view path, Entry& out) const {
    if (!impl_) {
        return false;
    }
    const Impl::Node* node = impl_->Find(path);
    if (node == nullptr) {
        return false;
    }
    out = node->entry;
    return true;
}

bool XdvdfsImage::ListDirectory(std::string_view path, std::vector<Entry>& out) const {
    out.clear();
    if (!impl_) {
        return false;
    }
    const Impl::Node* node = impl_->Find(path);
    if (node == nullptr || !node->entry.IsDirectory()) {
        return false;
    }
    out.reserve(node->children.size());
    for (const Impl::Node& child : node->children) {
        out.push_back(child.entry);
    }
    return true;
}

bool XdvdfsImage::Read(const Entry& file, std::uint64_t offset, void* buffer, std::uint32_t length,
                       std::uint32_t& transferred) const {
    transferred = 0;
    if (!impl_ || file.IsDirectory()) {
        return false;
    }
    if (offset >= file.size || length == 0) {
        return true;
    }
    const std::uint32_t wanted =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(length, file.size - offset));
    const std::uint64_t position =
        impl_->base + static_cast<std::uint64_t>(file.sector) * kSectorSize + offset;
    std::uint32_t got = 0;
    if (!impl_->ReadAt(position, buffer, wanted, got)) {
        FUT_LOGE(Vfs, "XDVDFS read error at image offset 0x%llX (+%u): Win32 error %lu",
                 static_cast<unsigned long long>(position), wanted, GetLastError());
        return false;
    }
    if (got < wanted) {
        // Truncated (over-trimmed) image: the extent ends beyond the image file. Never fail a disc read
        // (files.md 2.4: a failed read loops the disc-error screen) - deliver zeros instead.
        std::memset(static_cast<std::uint8_t*>(buffer) + got, 0, wanted - got);
        if (!impl_->warnedShortRead.exchange(true)) {
            FUT_LOGW(Vfs,
                     "XDVDFS: '%s' extends beyond the end of the image file - padding with zeros (image "
                     "truncated?)",
                     file.name.c_str());
        }
    }
    transferred = wanted;
    return true;
}

bool ReadIsoFile(const std::filesystem::path& image, std::string_view path, std::vector<std::uint8_t>& out,
                 std::string& error) {
    out.clear();
    XdvdfsImage iso;
    if (!iso.Open(image, error)) {
        return false;
    }
    XdvdfsImage::Entry entry;
    if (!iso.Lookup(path, entry)) {
        error = "'" + std::string(path) + "' not found in the disc image";
        return false;
    }
    if (entry.IsDirectory()) {
        error = "'" + std::string(path) + "' is a directory";
        return false;
    }
    try {
        out.resize(entry.size);
    } catch (const std::bad_alloc&) {
        error = "out of memory reading '" + std::string(path) + "'";
        return false;
    }
    std::uint32_t got = 0;
    if (!iso.Read(entry, 0, out.data(), entry.size, got) || got != entry.size) {
        out.clear();
        error = "read error in '" + std::string(path) + "'";
        return false;
    }
    return true;
}

}  // namespace vfs
