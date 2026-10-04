// Guest I/O APC queue (io agent). See io_apc.h.
#include "kernel/io_apc.h"

#include "core/log.h"

#include <atomic>
#include <deque>
#include <unordered_map>

namespace krnl::io {
namespace {

struct IoApc {
    IoApcRoutine routine;
    void* context;
    xbox::IO_STATUS_BLOCK* ioStatusBlock;
    NTSTATUS status;
    std::uint32_t information;
};

struct ApcQueues {
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<DWORD, std::deque<IoApc>> perThread;
};

ApcQueues& Queues() {
    static ApcQueues* q = new ApcQueues();  // never destroyed
    return *q;
}

std::atomic<std::uint64_t> g_queued{0};
std::atomic<std::uint64_t> g_delivered{0};

constexpr std::uint32_t kMaxDeliveriesPerCall = 4096;

bool PopFront(DWORD tid, IoApc& out) noexcept {
    ApcQueues& q = Queues();
    AcquireSRWLockExclusive(&q.lock);
    bool ok = false;
    const auto it = q.perThread.find(tid);
    if (it != q.perThread.end() && !it->second.empty()) {
        out = it->second.front();
        it->second.pop_front();
        ok = true;
        if (it->second.empty()) {
            q.perThread.erase(it);
        }
    }
    ReleaseSRWLockExclusive(&q.lock);
    return ok;
}

}  // namespace

bool QueueIoApc(IoApcRoutine routine, void* context, xbox::IO_STATUS_BLOCK* ioStatusBlock, NTSTATUS status,
                std::uint32_t information) noexcept {
    ApcQueues& q = Queues();
    const DWORD tid = GetCurrentThreadId();
    bool ok = true;
    AcquireSRWLockExclusive(&q.lock);
    try {
        q.perThread[tid].push_back(IoApc{routine, context, ioStatusBlock, status, information});
    } catch (...) {
        ok = false;
    }
    ReleaseSRWLockExclusive(&q.lock);
    if (ok) {
        g_queued.fetch_add(1, std::memory_order_relaxed);
        FUT_LOGT(Krnl, "I/O APC queued: routine %p ctx %p iosb %p status 0x%08lX info %u",
                 reinterpret_cast<void*>(routine), context, static_cast<void*>(ioStatusBlock),
                 static_cast<unsigned long>(status), information);
    }
    return ok;
}

std::uint32_t DeliverIoApcs() noexcept {
    const DWORD tid = GetCurrentThreadId();
    std::uint32_t delivered = 0;
    IoApc apc{};
    while (delivered < kMaxDeliveriesPerCall && PopFront(tid, apc)) {
        if (apc.ioStatusBlock != nullptr) {
            apc.ioStatusBlock->Status = apc.status;
            apc.ioStatusBlock->Information = apc.information;
        }
        FUT_LOGT(Krnl, "I/O APC deliver: routine %p ctx %p iosb %p status 0x%08lX info %u",
                 reinterpret_cast<void*>(apc.routine), apc.context, static_cast<void*>(apc.ioStatusBlock),
                 static_cast<unsigned long>(apc.status), apc.information);
        if (apc.routine != nullptr) {
            apc.routine(apc.context, apc.ioStatusBlock, 0);  // guest/HLE code, stdcall (ret 0xC)
        }
        ++delivered;
        g_delivered.fetch_add(1, std::memory_order_relaxed);
    }
    return delivered;
}

std::uint32_t PendingIoApcCount() noexcept {
    ApcQueues& q = Queues();
    const DWORD tid = GetCurrentThreadId();
    AcquireSRWLockShared(&q.lock);
    const auto it = q.perThread.find(tid);
    const std::uint32_t n = it == q.perThread.end() ? 0u : static_cast<std::uint32_t>(it->second.size());
    ReleaseSRWLockShared(&q.lock);
    return n;
}

IoApcStats GetIoApcStats() noexcept {
    return IoApcStats{g_queued.load(std::memory_order_relaxed), g_delivered.load(std::memory_order_relaxed)};
}

}  // namespace krnl::io
