// Object namespace of the VFS (design 4.1/4.2, E8): device objects, symbolic links (\??\X:) and the
// resolution of absolute object paths to {device, device-relative path}.
//
// Evidence for the names the guest uses (strings.tsv, files.md 1.1):
//   "\Device\CdRom0"                          XapiInitProcess IoCreateSymbolicLink("\??\D:", ...)
//   "\Device\Harddisk0\partition1\"           FUN_000f92af -> FUN_000f8ae2 (cluster-size check)
//   "\Device\Harddisk0\partition1\TDATA|UDATA" XapiMapLetterToDirectory (T:, U:)
//   "\??\U:\..."                              XDeleteSaveGame (absolute, RootDirectory = 0)
// Names are compared ASCII case-insensitively ("partition1" vs "Partition1", "Cdrom0" in
// XLaunchNewImageA), '/' and '\' are equivalent (design 4.2 item 2).
#include "vfs/vfs.h"
#include "vfs/vfs_internal.h"

#include "core/log.h"
#include "core/trace.h"

#include <algorithm>

namespace vfs {
namespace {

struct DeviceEntry {
    std::string name;
    std::shared_ptr<Device> device;
};

struct LinkEntry {
    std::string name;
    std::string target;
};

struct Namespace {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<DeviceEntry> devices;
    std::vector<LinkEntry> links;
};

Namespace& Ns() {
    static Namespace* ns = new Namespace();  // never destroyed: guest handles may outlive static dtors
    return *ns;
}

class SharedLock {
public:
    explicit SharedLock(SRWLOCK& l) noexcept : l_(l) { AcquireSRWLockShared(&l_); }
    ~SharedLock() { ReleaseSRWLockShared(&l_); }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    SRWLOCK& l_;
};

class ExclusiveLock {
public:
    explicit ExclusiveLock(SRWLOCK& l) noexcept : l_(l) { AcquireSRWLockExclusive(&l_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&l_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;

private:
    SRWLOCK& l_;
};

constexpr int kMaxLinkDepth = 16;

// Normalized object name without trailing separators ("\" stays "\").
std::string CanonicalName(std::string_view name) {
    std::string out = NormalizePath(name);
    while (out.size() > 1 && out.back() == '\\') {
        out.pop_back();
    }
    return out;
}

// True if `prefix` names `path` itself or one of its parent objects (component boundary).
bool MatchesPrefix(std::string_view path, std::string_view prefix) noexcept {
    if (!IStartsWith(path, prefix)) {
        return false;
    }
    return path.size() == prefix.size() || prefix == "\\" || path[prefix.size()] == '\\';
}

std::size_t ComponentCount(std::string_view path) noexcept {
    std::size_t n = 0;
    bool inName = false;
    for (char c : path) {
        if (c == '\\') {
            inName = false;
        } else if (!inName) {
            inName = true;
            ++n;
        }
    }
    return n;
}

}  // namespace

bool AddDevice(std::string_view name, std::shared_ptr<Device> device) {
    const std::string canonical = CanonicalName(name);
    if (!device || canonical.size() < 2 || canonical[0] != '\\') {
        FUT_LOGE(Vfs, "AddDevice('%.*s'): invalid name or null device", static_cast<int>(name.size()),
                 name.data());
        return false;
    }
    Namespace& ns = Ns();
    {
        ExclusiveLock lock(ns.lock);
        for (const DeviceEntry& d : ns.devices) {
            if (IEquals(d.name, canonical)) {
                FUT_LOGW(Vfs, "AddDevice(%s): a device with this name exists already", canonical.c_str());
                return false;
            }
        }
        ns.devices.push_back(DeviceEntry{canonical, device});
    }
    FUT_LOGI(Vfs, "device %s -> %s%s", canonical.c_str(), device->Kind(),
             device->IsReadOnly() ? " (read-only)" : "");
    return true;
}

std::shared_ptr<Device> FindDevice(std::string_view name) {
    const std::string canonical = CanonicalName(name);
    Namespace& ns = Ns();
    SharedLock lock(ns.lock);
    for (const DeviceEntry& d : ns.devices) {
        if (IEquals(d.name, canonical)) {
            return d.device;
        }
    }
    return nullptr;
}

NTSTATUS CreateSymbolicLink(std::string_view link, std::string_view target) {
    const std::string name = CanonicalName(link);
    const std::string dest = CanonicalName(target);
    if (name.size() < 2 || name[0] != '\\' || dest.empty() || dest[0] != '\\') {
        FUT_LOGW(Vfs, "CreateSymbolicLink('%s' -> '%s'): invalid name", name.c_str(), dest.c_str());
        return STATUS_OBJECT_NAME_INVALID;
    }
    Namespace& ns = Ns();
    {
        ExclusiveLock lock(ns.lock);
        const bool exists = std::any_of(ns.links.begin(), ns.links.end(),
                                        [&](const LinkEntry& l) { return IEquals(l.name, name); }) ||
                            std::any_of(ns.devices.begin(), ns.devices.end(),
                                        [&](const DeviceEntry& d) { return IEquals(d.name, name); });
        if (exists) {
            FUT_LOGD(Vfs, "symlink %s exists already -> STATUS_OBJECT_NAME_COLLISION", name.c_str());
            core::trace::VfsLine("%-25s \"%s\" -> %s : exists STATUS_OBJECT_NAME_COLLISION", "symbolic link",
                                 name.c_str(), dest.c_str());
            return STATUS_OBJECT_NAME_COLLISION;
        }
        ns.links.push_back(LinkEntry{name, dest});
    }
    FUT_LOGI(Vfs, "symlink %s -> %s", name.c_str(), dest.c_str());
    core::trace::VfsLine("%-25s \"%s\" -> %s : created", "symbolic link", name.c_str(), dest.c_str());
    return STATUS_SUCCESS;
}

NTSTATUS DeleteSymbolicLink(std::string_view link) {
    const std::string name = CanonicalName(link);
    Namespace& ns = Ns();
    ExclusiveLock lock(ns.lock);
    const auto it = std::find_if(ns.links.begin(), ns.links.end(),
                                 [&](const LinkEntry& l) { return IEquals(l.name, name); });
    if (it == ns.links.end()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    ns.links.erase(it);
    return STATUS_SUCCESS;
}

bool QuerySymbolicLink(std::string_view link, std::string& target) {
    const std::string name = CanonicalName(link);
    Namespace& ns = Ns();
    SharedLock lock(ns.lock);
    for (const LinkEntry& l : ns.links) {
        if (IEquals(l.name, name)) {
            target = l.target;
            return true;
        }
    }
    return false;
}

NTSTATUS Resolve(std::string_view objectPath, ResolvedPath& out) {
    out = ResolvedPath{};
    std::string path = CanonicalName(objectPath);
    if (path.empty() || path[0] != '\\') {
        return STATUS_OBJECT_NAME_INVALID;
    }
    Namespace& ns = Ns();
    SharedLock lock(ns.lock);

    // 1. symbolic links (longest prefix at a component boundary; links may point to links)
    int depth = 0;
    for (;; ++depth) {
        if (depth > kMaxLinkDepth) {
            FUT_LOGW(Vfs, "Resolve(%.*s): symbolic link loop", static_cast<int>(objectPath.size()),
                     objectPath.data());
            return STATUS_OBJECT_NAME_INVALID;
        }
        const LinkEntry* best = nullptr;
        for (const LinkEntry& l : ns.links) {
            if (MatchesPrefix(path, l.name) && (best == nullptr || l.name.size() > best->name.size())) {
                best = &l;
            }
        }
        if (best == nullptr) {
            break;
        }
        path = CanonicalName(best->target + path.substr(best->name.size()));
    }

    // 2. device (longest prefix at a component boundary)
    const DeviceEntry* dev = nullptr;
    for (const DeviceEntry& d : ns.devices) {
        if (MatchesPrefix(path, d.name) && (dev == nullptr || d.name.size() > dev->name.size())) {
            dev = &d;
        }
    }
    if (dev == nullptr) {
        // "\??\X:" or "\Device\Foo" (an unknown name in an object directory) vs. a deeper path.
        return ComponentCount(path) <= 2 ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
    }
    out.device = dev->device;
    out.devicePath = dev->name;
    out.relativePath = path.size() > dev->name.size() ? path.substr(dev->name.size()) : std::string("\\");
    return STATUS_SUCCESS;
}

void ResetNamespace() {
    Namespace& ns = Ns();
    std::vector<DeviceEntry> devices;
    {
        ExclusiveLock lock(ns.lock);
        devices.swap(ns.devices);  // destroyed outside the lock
        ns.links.clear();
    }
}

}  // namespace vfs
