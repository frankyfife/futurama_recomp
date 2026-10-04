// Shared helpers of the io_* kernel exports (io agent). See io_common.h.
#include "kernel/io_common.h"

#include "kernel/io_apc.h"
#include "kernel/kernel_api.h"
#include "vfs/vfs_internal.h"

#include "core/guest.h"
#include "core/log.h"

#include <atomic>

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
        vfs::ResolvedPath resolved;
        const NTSTATUS s = vfs::Resolve(path, resolved);
        if (!NT_SUCCESS(s)) {
            return s;
        }
        out.device = std::move(resolved.device);
        out.devicePath = std::move(resolved.devicePath);
        out.relativePath = std::move(resolved.relativePath);
        if (root == xbox::ObDosDevicesDirectory) {
            out.objectName = path;
        }
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

NTSTATUS CompleteIo(const char* api, HANDLE event, IoApcRoutine apcRoutine, void* apcContext,
                    xbox::IO_STATUS_BLOCK* ioStatusBlock, NTSTATUS status,
                    std::uint32_t information) noexcept {
    if ((static_cast<std::uint32_t>(status) >> 30) == 3) {  // NT_ERROR
        return status;  // failed synchronously: nothing is signalled or queued (NT IopCompleteRequest)
    }
    if (event != nullptr) {
        // ASSUMPTION: no caller on the game's paths passes an event (ReadFile/WriteFile with an
        // OVERLAPPED.hEvent are only used by the inactive NiTCache and the CRT, files.md 2.2). The
        // request completes synchronously, so a caller that only waits when STATUS_PENDING is
        // returned is unaffected; event objects belong to the kernel agent.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            FUT_LOGW(Krnl,
                     "%s: completion event %p is not signalled (event objects not wired to file I/O yet)",
                     api, event);
        }
    }
    if (apcRoutine != nullptr) {
        if (!IsNtUserIoApcDispatcher(apcRoutine)) {
            static std::atomic<bool> warnedRoutine{false};
            if (!warnedRoutine.exchange(true)) {
                FUT_LOGW(Krnl, "%s: ApcRoutine %p is not NtUserIoApcDispatcher - queued anyway", api,
                         reinterpret_cast<void*>(apcRoutine));
            }
        }
        // Design 1.7: the I/O is done; IoStatusBlock (= OVERLAPPED) is written when the APC is
        // delivered in an alertable wait (KeDelayExecutionThread / NtWaitForSingleObjectEx).
        if (QueueIoApc(apcRoutine, apcContext, ioStatusBlock, status, information)) {
            return STATUS_PENDING;
        }
        FUT_LOGE(Krnl, "%s: cannot queue the I/O APC (out of memory) - completing without APC", api);
    }
    WriteIoStatus(ioStatusBlock, status, information);
    return status;
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

bool IsNtUserIoApcDispatcher(IoApcRoutine routine) noexcept {
    const Binding b = GetBinding(exp::NtUserIoApcDispatcher::slot);
    return reinterpret_cast<std::uintptr_t>(routine) == b.address;
}

}  // namespace krnl::io
