// Guest threads created by PsCreateSystemThreadEx (design 1.6/1.7, E10). Owned by the kernel agent.
//
// Every thread gets a krnl::ThreadObject in the guest handle table (handle returned to the guest;
// entry closes it right away: 0xF8546 -> CloseHandle 0xF7177 -> NtClose). The object owns the
// guest-visible per-thread memory (fake KTHREAD 0x140 + TlsData, one VirtualAlloc block below
// 0x80000000) and the host thread handle. The running thread holds a reference until
// PsTerminateSystemThread. Waits on thread handles (NtWaitForSingleObject(Ex)) use HostHandle().
#pragma once

#include "kernel/ob_handles.h"

#include <atomic>
#include <cstdint>

namespace krnl {

class ThreadObject final : public KObject {
public:
    // `block` (VirtualAlloc, zeroed) holds the fake KTHREAD at +0 and TlsData at +kTlsDataOffset.
    ThreadObject(void* block, std::uint32_t tlsDataSize) noexcept
        : block_(block), tlsDataSize_(tlsDataSize) {}
    ~ThreadObject() override {
        if (host_ != nullptr) {
            CloseHandle(host_);
        }
        if (block_ != nullptr) {
            VirtualFree(block_, 0, MEM_RELEASE);
        }
    }
    const char* TypeName() const noexcept override { return "Thread"; }

    static constexpr std::uint32_t kTlsDataOffset = 0x200;  // fake KTHREAD (0x140) fits below
    static constexpr std::uint32_t kBlockSize = 0x1000;
    static constexpr std::uint32_t kMaxTlsDataSize = kBlockSize - kTlsDataOffset;

    void SetHostThread(HANDLE host, std::uint32_t threadId) noexcept {
        host_ = host;
        threadId_ = threadId;
    }
    HANDLE HostHandle() const noexcept { return host_; }  // synchronize/wait handle of the host thread
    std::uint32_t ThreadId() const noexcept { return threadId_; }
    xbox::FakeKThread* KThread() const noexcept { return static_cast<xbox::FakeKThread*>(block_); }
    std::uint8_t* TlsData() const noexcept {
        return tlsDataSize_ != 0 ? static_cast<std::uint8_t*>(block_) + kTlsDataOffset : nullptr;
    }
    std::uint32_t TlsDataSize() const noexcept { return tlsDataSize_; }
    bool HasExited() const noexcept { return exited_.load(std::memory_order_acquire); }
    NTSTATUS ExitStatus() const noexcept { return exitStatus_; }
    void MarkExited(NTSTATUS status) noexcept {
        exitStatus_ = status;
        exited_.store(true, std::memory_order_release);
    }

private:
    void* block_;
    std::uint32_t tlsDataSize_;
    HANDLE host_ = nullptr;
    std::uint32_t threadId_ = 0;
    NTSTATUS exitStatus_ = STATUS_PENDING;
    std::atomic<bool> exited_{false};
};

}  // namespace krnl
