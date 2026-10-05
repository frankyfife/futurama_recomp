// Guest I/O APC queue (io agent). See io_apc.h.
#include "kernel/io_apc.h"

#include "kernel/io_common.h"
#include "kernel/kernel_hle.h"

#include "core/guest.h"
#include "core/guest_call.h"
#include "core/host_main.h"
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
std::atomic<std::uint64_t> g_errorCompletions{0};
std::atomic<std::uint64_t> g_failedRequests{0};
std::atomic<std::uint32_t> g_maxPending{0};
std::atomic<std::uint64_t> g_deliveredXbe{0};
std::atomic<bool> g_firstXbeDeliveryLogged{false};

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

void UpdateMaxPending(std::uint32_t pending) noexcept {
    std::uint32_t seen = g_maxPending.load(std::memory_order_relaxed);
    while (pending > seen && !g_maxPending.compare_exchange_weak(seen, pending, std::memory_order_relaxed)) {
    }
}

// Delivers one APC on the current thread (IoStatusBlock already written).
void CallApc(const IoApc& apc) noexcept {
    if (IsNtUserIoApcDispatcher(apc.routine)) {
        // XAPI ReadFileEx/WriteFileEx: ApcContext = LPOVERLAPPED_COMPLETION_ROUTINE (stdcall, ret 0xC).
        const std::uint32_t error = IsNtError(apc.status) ? NtStatusToDosError(apc.status) : 0;
        if (apc.context == nullptr) {
            FUT_LOGE(Krnl, "I/O APC: NtUserIoApcDispatcher without completion routine (iosb %p) - dropped",
                     static_cast<void*>(apc.ioStatusBlock));
            return;
        }
        if (error != 0) {
            g_errorCompletions.fetch_add(1, std::memory_order_relaxed);
            FUT_LOGE(Krnl,
                     "I/O APC: completion %s called with error %u (status %s) - the stream completion "
                     "0x13F1D0 calls the disc error handler (DAT_0030E68C) for this",
                     guest::Symbolize(guest::Addr(apc.context)).c_str(), error,
                     StatusString(apc.status).c_str());
        }
        FUT_LOGT(Krnl, "I/O APC: completion %s(err %u, bytes 0x%X, overlapped %p)",
                 guest::Symbolize(guest::Addr(apc.context)).c_str(), error, apc.information,
                 static_cast<void*>(apc.ioStatusBlock));
        const guest::GuestAddr completion = guest::Addr(apc.context);
        core::CallGuest(completion, {error, apc.information, guest::Addr(apc.ioStatusBlock)});
        if (guest::InXbeImage(completion)) {
            // M2 exit criterion 5 evidence: a completion routine of the game (0x13F1D0 / 0xE01A0) ran.
            g_deliveredXbe.fetch_add(1, std::memory_order_relaxed);
            if (!g_firstXbeDeliveryLogged.exchange(true)) {
                FUT_LOGI(Krnl,
                         "I/O APC: first XBE completion delivered on thread %lu: %s(err %u, bytes 0x%X, "
                         "overlapped %p)",
                         GetCurrentThreadId(), guest::Symbolize(completion).c_str(), error, apc.information,
                         static_cast<void*>(apc.ioStatusBlock));
            }
        }
        return;
    }
    if (apc.routine != nullptr) {
        // Generic PIO_APC_ROUTINE (not used by the XBE; host tests).
        FUT_LOGT(Krnl, "I/O APC: routine %p(ctx %p, iosb %p, 0)", reinterpret_cast<void*>(apc.routine),
                 apc.context, static_cast<void*>(apc.ioStatusBlock));
        core::CallGuest(static_cast<guest::GuestAddr>(reinterpret_cast<std::uintptr_t>(apc.routine)),
                        {guest::Addr(apc.context), guest::Addr(apc.ioStatusBlock), 0});
    }
}

}  // namespace

bool QueueIoApc(IoApcRoutine routine, void* context, xbox::IO_STATUS_BLOCK* ioStatusBlock, NTSTATUS status,
                std::uint32_t information) noexcept {
    ApcQueues& q = Queues();
    const DWORD tid = GetCurrentThreadId();
    bool ok = true;
    std::uint32_t pending = 0;
    AcquireSRWLockExclusive(&q.lock);
    try {
        auto& queue = q.perThread[tid];
        queue.push_back(IoApc{routine, context, ioStatusBlock, status, information});
        pending = static_cast<std::uint32_t>(queue.size());
    } catch (...) {
        ok = false;
    }
    ReleaseSRWLockExclusive(&q.lock);
    if (ok) {
        g_queued.fetch_add(1, std::memory_order_relaxed);
        UpdateMaxPending(pending);
        FUT_LOGT(Krnl, "I/O APC queued: routine %p ctx %p iosb %p status 0x%08lX info 0x%X (pending %u)",
                 reinterpret_cast<void*>(routine), context, static_cast<void*>(ioStatusBlock),
                 static_cast<unsigned long>(status), information, pending);
    }
    return ok;
}

std::uint32_t DeliverIoApcs() noexcept {
    const DWORD tid = GetCurrentThreadId();
    std::uint32_t delivered = 0;
    IoApc apc{};
    while (delivered < kMaxDeliveriesPerCall && PopFront(tid, apc)) {
        WriteIoStatus(apc.ioStatusBlock, apc.status, apc.information);  // design 1.7: only now
        CallApc(apc);
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

std::uint32_t PendingIoApcCountAllThreads() noexcept {
    ApcQueues& q = Queues();
    std::uint32_t n = 0;
    AcquireSRWLockShared(&q.lock);
    for (const auto& entry : q.perThread) {
        n += static_cast<std::uint32_t>(entry.second.size());
    }
    ReleaseSRWLockShared(&q.lock);
    return n;
}

void NoteFailedIoApcRequest(const char* api, const char* path, IoApcRoutine routine,
                            NTSTATUS status) noexcept {
    if (routine == nullptr) {
        return;
    }
    if (IsNtUserIoApcDispatcher(routine)) {
        g_failedRequests.fetch_add(1, std::memory_order_relaxed);
        FUT_LOGE(Krnl,
                 "%s(%s) with NtUserIoApcDispatcher failed synchronously: %s - XAPI ReadFileEx/WriteFileEx "
                 "returns FALSE (NiUDSStream FUN_0013F660 then calls the disc error handler DAT_0030E68C)",
                 api, path != nullptr ? path : "?", StatusString(status).c_str());
    } else {
        FUT_LOGD(Krnl, "%s(%s) with ApcRoutine %p failed synchronously: %s (nothing queued)", api,
                 path != nullptr ? path : "?", reinterpret_cast<void*>(routine),
                 StatusString(status).c_str());
    }
}

IoApcStats GetIoApcStats() noexcept {
    return IoApcStats{
        g_queued.load(std::memory_order_relaxed),           g_delivered.load(std::memory_order_relaxed),
        g_errorCompletions.load(std::memory_order_relaxed), g_failedRequests.load(std::memory_order_relaxed),
        g_maxPending.load(std::memory_order_relaxed),       g_deliveredXbe.load(std::memory_order_relaxed)};
}

void LogIoApcStats(const char* reason) noexcept {
    const IoApcStats s = GetIoApcStats();
    FUT_LOGI(
        Krnl,
        "I/O APC stats (%s): queued %llu, delivered %llu (XBE completions %llu), pending %u, max pending "
        "%u, error completions %llu, failed APC requests %llu",
        reason != nullptr ? reason : "-", static_cast<unsigned long long>(s.queued),
        static_cast<unsigned long long>(s.delivered), static_cast<unsigned long long>(s.deliveredXbe),
        PendingIoApcCountAllThreads(), s.maxPending, static_cast<unsigned long long>(s.errorCompletions),
        static_cast<unsigned long long>(s.failedRequests));
}

namespace {
// Exit reporter (core/host_main.h): APC statistics at every orderly process end (M2 exit 5: deliveries
// > 0, error completions and failed APC requests 0).
void ExitReport() noexcept {
    LogIoApcStats("process exit");
}
const bool g_exitReporterRegistered = (core::AddExitReporter("io apc", &ExitReport), true);
}  // namespace

}  // namespace krnl::io
