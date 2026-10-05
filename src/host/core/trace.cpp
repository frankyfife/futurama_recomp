// Tracing (design 5.4): kernel trace ring buffer and VFS trace file. See trace.h. Owned by the io agent.
#include "core/trace.h"

#include "core/crash.h"
#include "core/log.h"
#include "core/selftest.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace core::trace {
namespace detail {
std::atomic<bool> g_krnlEnabled{false};
std::atomic<bool> g_vfsEnabled{false};
}  // namespace detail

namespace {

// ---- kernel trace ring ----
SRWLOCK g_ringLock = SRWLOCK_INIT;
char g_ring[kKrnlRingEntries][kKrnlRingLineBytes];  // guarded by g_ringLock
std::uint64_t g_ringCount = 0;                      // lines recorded since the last Configure
std::atomic<bool> g_reporterRegistered{false};

// ---- VFS trace file ----
SRWLOCK g_vfsLock = SRWLOCK_INIT;
HANDLE g_vfsFile = INVALID_HANDLE_VALUE;     // guarded by g_vfsLock
std::filesystem::path* g_vfsPath = nullptr;  // leaked on purpose (process lifetime)
std::atomic<std::uint64_t> g_vfsLines{0};

// Configure() itself is serialized.
SRWLOCK g_configLock = SRWLOCK_INIT;

void KrnlTap(Channel channel, Level /*level*/, const char* line, std::size_t size) noexcept {
    if (channel == Channel::Krnl) {
        RecordKrnl(line, size);
    }
}

void CrashReporterKrnlRing() noexcept {
    DumpKrnlRing();
}

void WriteRaw(HANDLE h, const char* data, std::size_t size) noexcept {
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) || written == 0) {
            return;
        }
        data += written;
        size -= written;
    }
}

void CloseVfsFileLocked() noexcept {
    if (g_vfsFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_vfsFile);
        g_vfsFile = INVALID_HANDLE_VALUE;
    }
}

bool OpenVfsFile(const std::filesystem::path& directory) noexcept {
    try {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);  // normally exists (LogInit)
        const std::filesystem::path path = directory / kVfsTraceFileName;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            FUT_LOGW(Core, "VFS trace: cannot create %s (error %lu)", WideToUtf8(path.wstring()).c_str(),
                     GetLastError());
            return false;
        }
        static const char kHeader[] =
            "# Futurama PC loader - VFS trace (--trace=vfs, design 5.4, M2 exit criterion 7)\n"
            "# [seconds, same clock as futurama.log] thread-id:thread-name (the log's thread column)\n"
            "# api  \"guest path\" -> device path : result\n"
            "# result: hit <action> | missing <status> (documented: <reason>) | missing <status> + the\n"
            "# marker for misses without analysis (grep for it: must not occur) | error <status>\n"
            "# a trailing [self-test] marks the loader's own self-tests (--selftest), not the game\n"
            "# game probes (xapi/game_probes.cpp): \"archive mount\" (mounted | not mounted) and\n"
            "# \"archive lookup\" per asset (hit archive #n = member of a mounted .img | not archived =\n"
            "# the loose-file open that follows decides)\n";
        WriteRaw(h, kHeader, sizeof(kHeader) - 1);
        AcquireSRWLockExclusive(&g_vfsLock);
        CloseVfsFileLocked();
        g_vfsFile = h;
        if (g_vfsPath == nullptr) {
            g_vfsPath = new std::filesystem::path();
        }
        *g_vfsPath = path;
        ReleaseSRWLockExclusive(&g_vfsLock);
        g_vfsLines.store(0);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

Options OptionsFromConfig(const Config& config) {
    Options o;
    for (const std::string& t : config.trace) {
        if (t == "all") {
            o.krnl = o.vfs = true;
        } else if (t == "krnl") {
            o.krnl = true;
        } else if (t == "vfs") {
            o.vfs = true;
        }
    }
    o.directory = config.LogDirectory();
    return o;
}

bool Configure(const Options& options) noexcept {
    AcquireSRWLockExclusive(&g_configLock);
    bool ok = true;

    // Kernel trace ring: reset, then (un)install the log tap.
    detail::g_krnlEnabled.store(false);
    AcquireSRWLockExclusive(&g_ringLock);
    g_ringCount = 0;
    ReleaseSRWLockExclusive(&g_ringLock);
    if (options.krnl) {
        detail::g_krnlEnabled.store(true);
        SetLogTap(&KrnlTap);
        if (!g_reporterRegistered.exchange(true)) {
            AddCrashReporter("kernel trace ring (--trace=krnl)", &CrashReporterKrnlRing);
        }
    } else {
        SetLogTap(nullptr);
    }

    // VFS trace file.
    detail::g_vfsEnabled.store(false);
    AcquireSRWLockExclusive(&g_vfsLock);
    CloseVfsFileLocked();
    ReleaseSRWLockExclusive(&g_vfsLock);
    if (options.vfs) {
        if (OpenVfsFile(options.directory)) {
            detail::g_vfsEnabled.store(true);
        } else {
            ok = false;
        }
    }
    ReleaseSRWLockExclusive(&g_configLock);

    if (options.krnl) {
        FUT_LOGI(Core, "kernel trace: ring buffer of the last %u KRNL lines, dumped by the crash report",
                 static_cast<unsigned>(kKrnlRingEntries));
    }
    if (options.vfs && ok) {
        FUT_LOGI(Core, "VFS trace: %s", WideToUtf8(VfsTracePath().wstring()).c_str());
    }
    return ok;
}

bool Init(const Config& config) noexcept {
    try {
        return Configure(OptionsFromConfig(config));
    } catch (...) {
        return false;
    }
}

void RecordKrnl(const char* line, std::size_t size) noexcept {
    if (!KrnlEnabled() || line == nullptr) {
        return;
    }
    while (size > 0 && (line[size - 1] == '\n' || line[size - 1] == '\r')) {
        --size;
    }
    const std::size_t n = size < kKrnlRingLineBytes - 1 ? size : kKrnlRingLineBytes - 1;
    AcquireSRWLockExclusive(&g_ringLock);
    char* slot = g_ring[g_ringCount % kKrnlRingEntries];
    std::memcpy(slot, line, n);
    slot[n] = 0;
    ++g_ringCount;
    ReleaseSRWLockExclusive(&g_ringLock);
}

std::vector<std::string> KrnlRingSnapshot() {
    std::vector<std::string> out;
    AcquireSRWLockShared(&g_ringLock);
    try {
        const std::uint64_t count = g_ringCount;
        const std::uint64_t first = count > kKrnlRingEntries ? count - kKrnlRingEntries : 0;
        out.reserve(static_cast<std::size_t>(count - first));
        for (std::uint64_t i = first; i < count; ++i) {
            out.emplace_back(g_ring[i % kKrnlRingEntries]);
        }
    } catch (...) {
        ReleaseSRWLockShared(&g_ringLock);
        throw;
    }
    ReleaseSRWLockShared(&g_ringLock);
    return out;
}

std::uint64_t KrnlRecordCount() noexcept {
    AcquireSRWLockShared(&g_ringLock);
    const std::uint64_t n = g_ringCount;
    ReleaseSRWLockShared(&g_ringLock);
    return n;
}

void DumpKrnlRing() noexcept {
    // Runs inside the fatal hook: never block on the ring lock (the crashing thread may hold it).
    // Lines are logged on CORE, so the KRNL log tap does not feed them back into the ring.
    const bool locked = TryAcquireSRWLockShared(&g_ringLock) != FALSE;
    const std::uint64_t count = g_ringCount;
    const std::uint64_t first = count > kKrnlRingEntries ? count - kKrnlRingEntries : 0;
    LogWrite(Channel::Core, Level::Error, "kernel trace ring: last %llu of %llu KRNL lines (oldest first)%s",
             static_cast<unsigned long long>(count - first), static_cast<unsigned long long>(count),
             locked ? "" : " [ring lock busy, read without lock]");
    for (std::uint64_t i = first; i < count; ++i) {
        LogWrite(Channel::Core, Level::Error, "  krnl| %s", g_ring[i % kKrnlRingEntries]);
    }
    if (locked) {
        ReleaseSRWLockShared(&g_ringLock);
    }
}

void VfsLine(const char* fmt, ...) noexcept {
    if (!VfsEnabled() || fmt == nullptr) {
        return;
    }
    char buf[1536];
    // Same clock and thread name as futurama.log (M2 review finding 9): one event has the same timestamp
    // in both files.
    const int n = std::snprintf(buf, sizeof(buf), "[%11.6f] %5lu:%-10s ", LogSeconds(), GetCurrentThreadId(),
                                LogThreadName());
    if (n < 0) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    const int body = std::vsnprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n) - 1, fmt, args);
    va_end(args);
    if (body < 0) {
        return;
    }
    std::size_t size = static_cast<std::size_t>(n) + static_cast<std::size_t>(body);
    if (size > sizeof(buf) - 2) {
        size = sizeof(buf) - 2;  // truncated
    }
    // Opens of the loader's own self-tests (core::InSelfTest, M2 review finding 8) are not the game's.
    static constexpr char kSelfTestMarker[] = " [self-test]";
    if (InSelfTest() && size + sizeof(kSelfTestMarker) <= sizeof(buf) - 1) {
        std::memcpy(buf + size, kSelfTestMarker, sizeof(kSelfTestMarker) - 1);
        size += sizeof(kSelfTestMarker) - 1;
    }
    buf[size++] = '\n';
    AcquireSRWLockExclusive(&g_vfsLock);
    if (g_vfsFile != INVALID_HANDLE_VALUE) {
        WriteRaw(g_vfsFile, buf, size);
        g_vfsLines.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_vfsLock);
}

std::filesystem::path VfsTracePath() {
    AcquireSRWLockShared(&g_vfsLock);
    std::filesystem::path p;
    try {
        if (g_vfsFile != INVALID_HANDLE_VALUE && g_vfsPath != nullptr) {
            p = *g_vfsPath;
        }
    } catch (...) {
    }
    ReleaseSRWLockShared(&g_vfsLock);
    return p;
}

std::uint64_t VfsLineCount() noexcept {
    return g_vfsLines.load(std::memory_order_relaxed);
}

}  // namespace core::trace
