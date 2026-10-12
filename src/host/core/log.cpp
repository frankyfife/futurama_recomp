// Logging implementation (design 5.4). See log.h.
#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

namespace core {
namespace detail {
std::uint8_t g_logLevels[kChannelCount] = {
    static_cast<std::uint8_t>(Level::Info), static_cast<std::uint8_t>(Level::Info),
    static_cast<std::uint8_t>(Level::Info), static_cast<std::uint8_t>(Level::Info),
    static_cast<std::uint8_t>(Level::Info), static_cast<std::uint8_t>(Level::Info),
    static_cast<std::uint8_t>(Level::Info), static_cast<std::uint8_t>(Level::Info),
    static_cast<std::uint8_t>(Level::Info), static_cast<std::uint8_t>(Level::Info)};
}  // namespace detail

namespace {

constexpr const char* kChannelNames[kChannelCount] = {"CORE", "KRNL", "VFS", "XAPI", "GFX",
                                                      "SND",  "VID",  "INP", "HOOK", "GAME"};
constexpr const char* kLevelNames[] = {"error", "warn", "info", "debug", "trace"};
constexpr const char* kLevelTags[] = {"ERROR", "WARN ", "INFO ", "DEBUG", "TRACE"};

SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_file = INVALID_HANDLE_VALUE;
HANDLE g_console = INVALID_HANDLE_VALUE;
bool g_initialized = false;
bool g_toConsole = false;
bool g_toDebugger = false;
bool g_fatalMessageBox = false;
std::string* g_early = nullptr;  // lines logged before LogInit (heap: avoids static-destructor order)
std::filesystem::path* g_filePath = nullptr;
FatalHook g_fatalHook = nullptr;
std::atomic<int> g_fatalEntered{0};
std::atomic<int> g_threadCounter{0};
LARGE_INTEGER g_qpcStart{};
LARGE_INTEGER g_qpcFreq{};
thread_local char t_threadName[24] = {};
char g_lastError[512] = {};  // body of the most recent error-level line (guarded by g_lock)
std::atomic<LogTap> g_tap{nullptr};  // (io, M2) SetLogTap
std::atomic<LogTap> g_observers[kMaxLogObservers] = {};  // (M4) AddLogObserver

void EnsureClock() noexcept {
    if (g_qpcFreq.QuadPart == 0) {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_qpcStart);
    }
}

const char* ThreadName() noexcept {
    if (t_threadName[0] == 0) {
        std::snprintf(t_threadName, sizeof(t_threadName), "t%d", ++g_threadCounter);
    }
    return t_threadName;
}

bool ValidHandle(HANDLE h) noexcept {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

void WriteAll(HANDLE h, const char* data, size_t size) noexcept {
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) || written == 0) {
            return;
        }
        data += written;
        size -= written;
    }
}

// Writes one formatted line to all sinks. Caller holds g_lock exclusively.
void EmitLocked(const char* line, size_t size) noexcept {
    if (ValidHandle(g_file)) {
        WriteAll(g_file, line, size);
    } else if (!g_initialized) {
        try {
            if (g_early == nullptr) {
                g_early = new std::string();
            }
            g_early->append(line, size);
        } catch (...) {
        }
    }
    if (g_toConsole && ValidHandle(g_console)) {
        WriteAll(g_console, line, size);
    }
    if (g_toDebugger) {
        OutputDebugStringA(line);
    }
}

void OpenConsoleSink() noexcept {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (ValidHandle(err) && GetFileType(err) != FILE_TYPE_UNKNOWN) {
        g_console = err;  // inherited pipe/file/console (e.g. started from bash or a CI runner)
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    }
}

void RotateFiles(const std::filesystem::path& dir, const std::wstring& name, int keep) noexcept {
    std::error_code ec;
    const std::filesystem::path base = dir / name;
    const std::wstring stem = base.stem().wstring();
    const std::wstring ext = base.extension().wstring();
    auto numbered = [&](int i) { return dir / (stem + L"." + std::to_wstring(i) + ext); };
    if (keep < 1) {
        keep = 1;
    }
    std::filesystem::remove(numbered(keep - 1), ec);
    for (int i = keep - 2; i >= 1; --i) {
        std::filesystem::rename(numbered(i), numbered(i + 1), ec);
    }
    if (keep > 1) {
        std::filesystem::rename(base, numbered(1), ec);
    }
}

void FormatAndEmit(Channel channel, Level level, const char* fmt, va_list args) noexcept {
    if (!LogEnabled(channel, level)) {
        return;
    }
    const double seconds = LogSeconds();

    char stackBuf[2048];
    int prefix = std::snprintf(stackBuf, sizeof(stackBuf), "[%11.6f] %-10s %-4s %s ", seconds, ThreadName(),
                               ChannelName(channel), kLevelTags[static_cast<int>(level)]);
    if (prefix < 0) {
        return;
    }
    va_list copy;
    va_copy(copy, args);
    const int bodyLen = std::vsnprintf(stackBuf + prefix, sizeof(stackBuf) - prefix - 1, fmt, copy);
    va_end(copy);
    if (bodyLen < 0) {
        return;
    }
    std::string heapBuf;
    char* line = stackBuf;
    size_t size = 0;
    if (static_cast<size_t>(prefix + bodyLen) + 2 <= sizeof(stackBuf)) {
        size = static_cast<size_t>(prefix + bodyLen);
    } else {
        try {
            heapBuf.assign(stackBuf, static_cast<size_t>(prefix));
            heapBuf.resize(static_cast<size_t>(prefix + bodyLen) + 2);
            std::vsnprintf(heapBuf.data() + prefix, static_cast<size_t>(bodyLen) + 1, fmt, args);
            line = heapBuf.data();
            size = static_cast<size_t>(prefix + bodyLen);
        } catch (...) {
            size = sizeof(stackBuf) - 2;  // truncated
        }
    }
    line[size++] = '\n';
    line[size] = 0;
    if (const LogTap tap = g_tap.load(std::memory_order_acquire)) {
        tap(channel, level, line, size);
    }
    for (std::atomic<LogTap>& slot : g_observers) {
        if (const LogTap observer = slot.load(std::memory_order_acquire)) {
            observer(channel, level, line, size);
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    EmitLocked(line, size);
    if (level == Level::Error) {
        const size_t body = size - 1 - static_cast<size_t>(prefix);  // without prefix and newline
        const size_t n = body < sizeof(g_lastError) - 1 ? body : sizeof(g_lastError) - 1;
        std::memcpy(g_lastError, line + prefix, n);
        g_lastError[n] = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace

bool LogInit(const LogOptions& options, std::string* error) noexcept {
    try {
        EnsureClock();
        AcquireSRWLockExclusive(&g_lock);
        g_toConsole = options.console;
        g_toDebugger = options.debugger;
        if (g_toConsole && !ValidHandle(g_console)) {
            OpenConsoleSink();
        }
        bool ok = true;
        if (!options.directory.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(options.directory, ec);
            RotateFiles(options.directory, options.fileName, options.keepFiles);
            const std::filesystem::path path = options.directory / options.fileName;
            g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
            if (!ValidHandle(g_file)) {
                ok = false;
                if (error) {
                    *error = "cannot create log file " + path.string() + " (error " +
                             std::to_string(GetLastError()) + ")";
                }
            } else {
                if (g_filePath == nullptr) {
                    g_filePath = new std::filesystem::path();
                }
                *g_filePath = path;
            }
        }
        if (g_early != nullptr) {
            if (ValidHandle(g_file)) {
                WriteAll(g_file, g_early->data(), g_early->size());
            }
            delete g_early;
            g_early = nullptr;
        }
        g_initialized = true;
        ReleaseSRWLockExclusive(&g_lock);
        LogSetAllLevels(options.defaultLevel);
        return ok;
    } catch (...) {
        ReleaseSRWLockExclusive(&g_lock);
        if (error) {
            *error = "exception in LogInit";
        }
        return false;
    }
}

void LogShutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (ValidHandle(g_file)) {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void LogFlush() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (ValidHandle(g_file)) {
        FlushFileBuffers(g_file);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::string LogLastError() {
    AcquireSRWLockShared(&g_lock);
    std::string text(g_lastError);
    ReleaseSRWLockShared(&g_lock);
    return text;
}

const std::filesystem::path& LogFilePath() noexcept {
    static const std::filesystem::path empty;
    return g_filePath != nullptr ? *g_filePath : empty;
}

void LogSetLevel(Channel channel, Level level) noexcept {
    if (channel < Channel::Count) {
        detail::g_logLevels[static_cast<int>(channel)] = static_cast<std::uint8_t>(level);
    }
}

void LogSetAllLevels(Level level) noexcept {
    for (auto& l : detail::g_logLevels) {
        l = static_cast<std::uint8_t>(level);
    }
}

Level LogGetLevel(Channel channel) noexcept {
    return channel < Channel::Count ? static_cast<Level>(detail::g_logLevels[static_cast<int>(channel)])
                                    : Level::Info;
}

void LogWrite(Channel channel, Level level, const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    FormatAndEmit(channel, level, fmt, args);
    va_end(args);
}

void LogWriteV(Channel channel, Level level, const char* fmt, va_list args) noexcept {
    FormatAndEmit(channel, level, fmt, args);
}

void LogSetThreadName(const char* name) noexcept {
    std::snprintf(t_threadName, sizeof(t_threadName), "%s", name ? name : "?");
}

double LogSeconds() noexcept {
    EnsureClock();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_qpcStart.QuadPart) / static_cast<double>(g_qpcFreq.QuadPart);
}

const char* LogThreadName() noexcept {
    return ThreadName();
}

void SetLogTap(LogTap tap) noexcept {
    g_tap.store(tap, std::memory_order_release);
}

bool AddLogObserver(LogTap observer) noexcept {
    if (observer == nullptr) {
        return false;
    }
    for (std::atomic<LogTap>& slot : g_observers) {
        if (slot.load(std::memory_order_acquire) == observer) {
            return true;
        }
    }
    for (std::atomic<LogTap>& slot : g_observers) {
        LogTap expected = nullptr;
        if (slot.compare_exchange_strong(expected, observer, std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}

void RemoveLogObserver(LogTap observer) noexcept {
    for (std::atomic<LogTap>& slot : g_observers) {
        LogTap expected = observer;
        slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
}

const char* ChannelName(Channel channel) noexcept {
    return channel < Channel::Count ? kChannelNames[static_cast<int>(channel)] : "????";
}

const char* LevelName(Level level) noexcept {
    return static_cast<int>(level) <= static_cast<int>(Level::Trace) ? kLevelNames[static_cast<int>(level)]
                                                                     : "?";
}

namespace {
bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y - 'A' + 'a');
        if (x != y) {
            return false;
        }
    }
    return true;
}
}  // namespace

bool ParseChannel(std::string_view text, Channel& out) noexcept {
    for (int i = 0; i < kChannelCount; ++i) {
        if (IEquals(text, kChannelNames[i])) {
            out = static_cast<Channel>(i);
            return true;
        }
    }
    return false;
}

bool ParseLevel(std::string_view text, Level& out) noexcept {
    for (int i = 0; i <= static_cast<int>(Level::Trace); ++i) {
        if (IEquals(text, kLevelNames[i])) {
            out = static_cast<Level>(i);
            return true;
        }
    }
    if (IEquals(text, "warning")) {
        out = Level::Warn;
        return true;
    }
    return false;
}

void SetFatalHook(FatalHook hook) noexcept {
    g_fatalHook = hook;
}

void SetFatalMessageBox(bool enabled) noexcept {
    g_fatalMessageBox = enabled;
}

void FatalV(const char* fmt, va_list args) noexcept {
    if (g_fatalEntered.fetch_add(1) != 0) {
        TerminateProcess(GetCurrentProcess(), kExitFatal);  // fatal error while handling a fatal error
    }
    char message[1024];
    std::vsnprintf(message, sizeof(message), fmt, args);
    LogWrite(Channel::Core, Level::Error, "FATAL: %s", message);
    if (g_fatalHook != nullptr) {
        g_fatalHook(message);
    }
    LogFlush();
    if (!g_toConsole) {
        // Without the console sink the message would otherwise only be in the log file (finding 5).
        const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (ValidHandle(err) && GetFileType(err) != FILE_TYPE_UNKNOWN) {
            try {
                const std::string logPath =
                    g_filePath != nullptr ? g_filePath->string() : std::string("(none)");
                char line[1400];
                const int n = std::snprintf(line, sizeof(line), "futurama: FATAL: %s\nlog file: %s\n",
                                            message, logPath.c_str());
                if (n > 0) {
                    WriteAll(err, line,
                             static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n)
                                                                   : sizeof(line) - 1);
                }
            } catch (...) {
            }
        }
    }
    if (g_fatalMessageBox) {
        MessageBoxA(nullptr, message, "Futurama - fatal error", MB_OK | MB_ICONERROR);
    }
    TerminateProcess(GetCurrentProcess(), kExitFatal);
    __assume(0);
}

void Fatal(const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    FatalV(fmt, args);
}

}  // namespace core
