// Shared helpers of the io_* kernel exports (io agent). See io_common.h.
#include "kernel/io_common.h"

#include "kernel/io_apc.h"
#include "kernel/kernel_api.h"
#include "vfs/vfs_internal.h"

#include "core/guest.h"
#include "core/log.h"
#include "core/trace.h"
#include "shared/nt_status.h"

#include <atomic>
#include <cstdio>

namespace krnl::io {

NTSTATUS ResolveObjectAttributes(const xbox::OBJECT_ATTRIBUTES* objectAttributes, OpenTarget& out) {
    out = OpenTarget{};
    if (objectAttributes == nullptr || objectAttributes->ObjectName == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    out.objectName = guest::ReadAnsiString(objectAttributes->ObjectName);
    const HANDLE root = objectAttributes->RootDirectory;

    if (root == nullptr || root == xbox::ObDosDevicesDirectory) {
        std::string path;
        if (root == xbox::ObDosDevicesDirectory) {
            path = vfs::kDosDevicesPrefix;  // "\??\" + "D:\Video\x.xmv"
            path += out.objectName;
        } else {
            path = out.objectName;
            if (path.empty() || (path[0] != '\\' && path[0] != '/')) {
                return STATUS_OBJECT_NAME_INVALID;  // an absolute object name must start at the root
            }
        }
        if (root == xbox::ObDosDevicesDirectory) {
            out.objectName = path;  // logs / VFS trace show the full object name, also on failure
        }
        vfs::ResolvedPath resolved;
        const NTSTATUS s = vfs::Resolve(path, resolved);
        if (!NT_SUCCESS(s)) {
            return s;
        }
        out.device = std::move(resolved.device);
        out.devicePath = std::move(resolved.devicePath);
        out.relativePath = std::move(resolved.relativePath);
        return STATUS_SUCCESS;
    }

    // Relative to an open directory (same device; no symbolic-link processing, like NT).
    Ref<FileObject> dir = LookupFile(root);
    if (!dir) {
        return HandleTable::Instance().Lookup(root) ? STATUS_OBJECT_TYPE_MISMATCH : STATUS_INVALID_HANDLE;
    }
    if (!dir->File().IsDirectory()) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!out.objectName.empty() && (out.objectName[0] == '\\' || out.objectName[0] == '/')) {
        return STATUS_OBJECT_NAME_INVALID;  // a relative name must not start at the root
    }
    std::vector<std::string> parts = vfs::detail::SplitPath(dir->RelativePath());
    for (std::string& p : vfs::detail::SplitPath(out.objectName)) {
        parts.push_back(std::move(p));
    }
    out.device = dir->Device();
    out.devicePath = dir->DevicePath();
    out.relativePath = vfs::detail::JoinPath(parts);
    out.objectName = dir->FullPath() + "\\" + out.objectName;
    return STATUS_SUCCESS;
}

NTSTATUS ReferenceEvent(const char* api, HANDLE event, Ref<EventObject>& out) noexcept {
    out = nullptr;
    if (event == nullptr) {
        return STATUS_SUCCESS;
    }
    Ref<KObject> object = HandleTable::Instance().Lookup(event);
    if (!object) {
        FUT_LOGW(Krnl, "%s: Event %p is not a valid handle -> STATUS_INVALID_HANDLE (request not started)",
                 api, event);
        return STATUS_INVALID_HANDLE;
    }
    out = HandleTable::Instance().LookupAs<EventObject>(event);
    if (!out) {
        FUT_LOGW(Krnl,
                 "%s: Event %p is a %s, not an event -> STATUS_OBJECT_TYPE_MISMATCH (request not started)",
                 api, event, object->TypeName());
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    out->Reset();  // NT: KeClearEvent before the request starts
    return STATUS_SUCCESS;
}

NTSTATUS CompleteIo(const IoRequest& request, NTSTATUS status, std::uint32_t information) noexcept {
    if (IsNtError(status)) {
        // Failed synchronously: nothing is written, signalled or queued (NT IopCompleteRequest).
        if (request.apcRoutine != nullptr) {
            std::string path;
            try {
                path = request.file != nullptr ? request.file->FullPath() : std::string();
            } catch (...) {
            }
            NoteFailedIoApcRequest(request.api, path.c_str(), request.apcRoutine, status);
        }
        return status;
    }
    const bool synchronous = request.file != nullptr && request.file->Synchronous();
    const bool deferIosb = request.apcRoutine != nullptr && !synchronous && request.event == nullptr;
    if (!deferIosb) {
        WriteIoStatus(request.ioStatusBlock, status, information);
    }
    if (request.event != nullptr) {
        request.event->Set();
    }
    if (request.apcRoutine == nullptr) {
        return status;
    }
    if (!IsNtUserIoApcDispatcher(request.apcRoutine)) {
        static std::atomic<bool> warnedRoutine{false};
        if (!warnedRoutine.exchange(true)) {
            FUT_LOGW(Krnl, "%s: ApcRoutine %p is not NtUserIoApcDispatcher - queued as PIO_APC_ROUTINE",
                     request.api, reinterpret_cast<void*>(request.apcRoutine));
        }
    }
    // Design 1.7: the I/O is done; the completion runs in the next alertable wait of this thread
    // (KeDelayExecutionThread / NtWaitForSingleObjectEx), which also writes the IoStatusBlock.
    if (!QueueIoApc(request.apcRoutine, request.apcContext, request.ioStatusBlock, status, information)) {
        FUT_LOGE(Krnl, "%s: cannot queue the I/O APC (out of memory) - completing without APC", request.api);
        WriteIoStatus(request.ioStatusBlock, status, information);
        return status;
    }
    return synchronous ? status : STATUS_PENDING;
}

GuestFileNetworkOpenInformation MakeNetworkOpenInformation(const vfs::FileInfo& info) noexcept {
    GuestFileNetworkOpenInformation n{};
    n.CreationTime = MakeLargeInteger(info.creationTime);
    n.LastAccessTime = MakeLargeInteger(info.lastAccessTime);
    n.LastWriteTime = MakeLargeInteger(info.lastWriteTime);
    n.ChangeTime = MakeLargeInteger(info.changeTime);
    n.AllocationSize = MakeLargeInteger(static_cast<std::int64_t>(info.allocationSize));
    n.EndOfFile = MakeLargeInteger(static_cast<std::int64_t>(info.size));
    n.FileAttributes = info.attributes;
    return n;
}

std::string StatusText(NTSTATUS status) {
    return vfs::detail::StatusText(status);
}

StatusString::StatusString(NTSTATUS status) noexcept : hex_{}, text_(xbox::NtStatusName(status)) {
    if (text_ == nullptr) {
        std::snprintf(hex_, sizeof(hex_), "0x%08lX", static_cast<unsigned long>(status));
        text_ = hex_;
    }
}

void TraceVfsLookup(const char* api, const OpenTarget& target, NTSTATUS status, const char* action,
                    HANDLE handle) noexcept {
    if (!core::trace::VfsEnabled()) {
        return;
    }
    try {
        const char* guestPath = target.objectName.empty() ? "<invalid>" : target.objectName.c_str();
        std::string device =
            target.devicePath.empty() ? std::string("<unresolved>") : target.devicePath + target.relativePath;
        const StatusString st(status);
        if (NT_SUCCESS(status)) {
            char h[24] = {};
            if (handle != nullptr) {
                std::snprintf(h, sizeof(h), " h=%p", handle);
            }
            core::trace::VfsLine("%-25s \"%s\" -> %s : hit %s%s", api, guestPath, device.c_str(),
                                 action != nullptr ? action : st.c_str(), h);
        } else if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND) {
            const char* reason = vfs::detail::DocumentedMissReason(target.devicePath, target.relativePath);
            if (reason != nullptr) {
                core::trace::VfsLine("%-25s \"%s\" -> %s : missing %s (documented: %s)", api, guestPath,
                                     device.c_str(), st.c_str(), reason);
            } else {
                core::trace::VfsLine("%-25s \"%s\" -> %s : missing %s UNDOCUMENTED", api, guestPath,
                                     device.c_str(), st.c_str());
            }
        } else if (status == STATUS_OBJECT_NAME_COLLISION) {
            core::trace::VfsLine("%-25s \"%s\" -> %s : hit %s (exists)", api, guestPath, device.c_str(),
                                 st.c_str());
        } else {
            core::trace::VfsLine("%-25s \"%s\" -> %s : error %s", api, guestPath, device.c_str(), st.c_str());
        }
    } catch (...) {
    }
}

bool IsNtUserIoApcDispatcher(IoApcRoutine routine) noexcept {
    const Binding b = GetBinding(exp::NtUserIoApcDispatcher::slot);
    return reinterpret_cast<std::uintptr_t>(routine) == b.address;
}

}  // namespace krnl::io
