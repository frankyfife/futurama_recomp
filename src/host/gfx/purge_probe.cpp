// Stack purge measurement through a hijacked thread context (see purge_probe.h).
#include "gfx/purge_probe.h"

#include "core/guest.h"

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace gfx {
namespace {

std::atomic<DWORD> g_probeThreadId{0};
std::atomic<std::uint32_t> g_probeEntryEsp{0};

constexpr std::uint32_t kProbeStackCommit = 0x10000;  // committed at thread creation
constexpr std::uint32_t kProbeFrameDepth = 0x2000;    // distance of the probe frame below the initial esp
constexpr std::uint32_t kSpareDwords = 16;
constexpr DWORD kProbeTimeoutMs = 5000;

// Entered by the `ret N` of the measured function (never called). Its own "return address" slot is
// the first DWORD the callee did not pop.
[[noreturn]] void __cdecl PurgeProbeLanding() noexcept {
    const std::uint32_t esp = guest::Addr(_AddressOfReturnAddress());
    ExitThread(static_cast<DWORD>(esp - g_probeEntryEsp.load() - 4));
}

DWORD WINAPI ProbeThreadStart(void*) {
    return 0xFFFFFFFFu;  // not reached: the context is replaced before the thread first runs
}

}  // namespace

bool InPurgeProbe() noexcept {
    const DWORD id = g_probeThreadId.load(std::memory_order_relaxed);
    return id != 0 && id == GetCurrentThreadId();
}

namespace {

PurgeMeasurement Measure(const void* entry, std::uint32_t ecx, std::uint32_t edx, std::uint32_t stackDwords) {
    static std::mutex serial;
    PurgeMeasurement m;
    if (entry == nullptr || stackDwords > 64) {
        m.error = "bad arguments";
        return m;
    }
    std::lock_guard<std::mutex> lock(serial);
    DWORD tid = 0;
    HANDLE thread = CreateThread(nullptr, kProbeStackCommit, &ProbeThreadStart, nullptr, CREATE_SUSPENDED, &tid);
    if (thread == nullptr) {
        m.error = "CreateThread failed (" + std::to_string(GetLastError()) + ")";
        return m;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) {
        m.error = "GetThreadContext failed (" + std::to_string(GetLastError()) + ")";
        TerminateThread(thread, 0);
        CloseHandle(thread);
        return m;
    }
    // Probe frame inside the committed part of the new thread's own stack (TEB stack bounds stay
    // valid): [esp] = landing, [esp+4..] = arguments + spare DWORDs, all zero.
    const std::uint32_t esp = ((ctx.Esp - kProbeFrameDepth) & ~0xFu) - 4;
    auto* frame = guest::Ptr<std::uint32_t>(esp);
    std::memset(frame, 0, (1 + stackDwords + kSpareDwords) * sizeof(std::uint32_t));
    frame[0] = guest::Addr(reinterpret_cast<const void*>(&PurgeProbeLanding));
    ctx.Esp = esp;
    ctx.Eip = guest::Addr(entry);
    ctx.Ecx = ecx;
    ctx.Edx = edx;
    ctx.Ebp = 0;
    if (!SetThreadContext(thread, &ctx)) {
        m.error = "SetThreadContext failed (" + std::to_string(GetLastError()) + ")";
        TerminateThread(thread, 0);
        CloseHandle(thread);
        return m;
    }
    g_probeEntryEsp.store(esp);
    g_probeThreadId.store(tid);
    ResumeThread(thread);
    const DWORD wait = WaitForSingleObject(thread, kProbeTimeoutMs);
    DWORD code = 0;
    if (wait != WAIT_OBJECT_0) {
        m.error = "probe thread did not finish within 5 s";
        TerminateThread(thread, 0);
        WaitForSingleObject(thread, kProbeTimeoutMs);
    } else if (!GetExitCodeThread(thread, &code)) {
        m.error = "GetExitCodeThread failed";
    } else {
        m.ok = true;
        m.purgeBytes = code;
    }
    g_probeThreadId.store(0);
    CloseHandle(thread);
    return m;
}

}  // namespace

PurgeMeasurement MeasureCalleePurge(const void* entry, std::uint32_t ecx, std::uint32_t edx,
                                    std::uint32_t stackDwords) noexcept {
    try {
        return Measure(entry, ecx, edx, stackDwords);
    } catch (...) {
        g_probeThreadId.store(0);
        return PurgeMeasurement{false, 0, {}};
    }
}

}  // namespace gfx
