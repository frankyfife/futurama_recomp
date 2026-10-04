// Logging (design 5.4): channels CORE KRNL VFS XAPI GFX SND VID INP HOOK GAME, levels
// error warn info debug trace. Output: <userdata>\logs\futurama.log (rotated at start, 3 files kept),
// optionally stderr/parent console and OutputDebugString (default on in Debug builds).
//
// Usage:   FUT_LOGI(Krnl, "NtCreateFile(%s) -> 0x%08lX", path, status);
//          FUT_LOGW(Vfs, ...); FUT_LOGE(...); FUT_LOGD(...); FUT_LOGT(...)
//          core::Fatal("XBE SHA-1 mismatch: %s", hash);   // logs, runs the crash hook, terminates
// All functions are noexcept and thread-safe. Messages logged before LogInit() are buffered and
// written to the file when it opens.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

#include <sal.h>

namespace core {

enum class Channel : std::uint8_t { Core, Krnl, Vfs, Xapi, Gfx, Snd, Vid, Inp, Hook, Game, Count };
enum class Level : std::uint8_t { Error, Warn, Info, Debug, Trace };

inline constexpr int kChannelCount = static_cast<int>(Channel::Count);

struct LogOptions {
    std::filesystem::path directory;  // log file directory (created); empty = no file
    std::wstring fileName = L"futurama.log";
    Level defaultLevel = Level::Info;
    bool console = false;   // also write to stderr / the parent console
    bool debugger = false;  // also OutputDebugStringA
    int keepFiles = 3;      // futurama.log, futurama.1.log, futurama.2.log
};

// Opens the log file (rotating old ones) and flushes the early buffer. Returns false on I/O error
// (logging then continues to the other sinks).
bool LogInit(const LogOptions& options, std::string* error = nullptr) noexcept;
void LogShutdown() noexcept;
void LogFlush() noexcept;
const std::filesystem::path& LogFilePath() noexcept;

void LogSetLevel(Channel channel, Level level) noexcept;
void LogSetAllLevels(Level level) noexcept;
Level LogGetLevel(Channel channel) noexcept;

namespace detail {
extern std::uint8_t g_logLevels[kChannelCount];
}
inline bool LogEnabled(Channel channel, Level level) noexcept {
    return static_cast<std::uint8_t>(level) <= detail::g_logLevels[static_cast<int>(channel)];
}

// Writes one line if the level is enabled for the channel (the FUT_LOG* macros check this before
// evaluating their arguments).
void LogWrite(Channel channel, Level level, _Printf_format_string_ const char* fmt, ...) noexcept;
void LogWriteV(Channel channel, Level level, const char* fmt, va_list args) noexcept;

// Name shown in the thread column (e.g. "boot", "guest-main"). Default: "t<n>" in order of first use.
void LogSetThreadName(const char* name) noexcept;

const char* ChannelName(Channel channel) noexcept;  // "CORE", "KRNL", ...
const char* LevelName(Level level) noexcept;        // "error", ...
bool ParseChannel(std::string_view text, Channel& out) noexcept;  // case-insensitive
bool ParseLevel(std::string_view text, Level& out) noexcept;

// --- fatal errors (KeBugCheck-style abort) -----------------------------------------------------
inline constexpr int kExitFatal = 3;

// Called once by Fatal() before the process terminates (installed by core/crash: backtrace,
// ring buffer, minidump). Must not throw.
using FatalHook = void (*)(const char* message) noexcept;
void SetFatalHook(FatalHook hook) noexcept;
// Show a message box on fatal errors (default: only when no stderr handle exists).
void SetFatalMessageBox(bool enabled) noexcept;

// Logs "FATAL: <message>" on CORE/error, runs the fatal hook, flushes and terminates the process
// with exit code kExitFatal. Re-entrant calls terminate immediately.
[[noreturn]] void Fatal(_Printf_format_string_ const char* fmt, ...) noexcept;
[[noreturn]] void FatalV(const char* fmt, va_list args) noexcept;

}  // namespace core

#define FUT_LOG(ch, lvl, ...)                                                                   \
    do {                                                                                        \
        if (::core::LogEnabled(::core::Channel::ch, ::core::Level::lvl))                        \
            ::core::LogWrite(::core::Channel::ch, ::core::Level::lvl, __VA_ARGS__);             \
    } while (0)
#define FUT_LOGE(ch, ...) FUT_LOG(ch, Error, __VA_ARGS__)
#define FUT_LOGW(ch, ...) FUT_LOG(ch, Warn, __VA_ARGS__)
#define FUT_LOGI(ch, ...) FUT_LOG(ch, Info, __VA_ARGS__)
#define FUT_LOGD(ch, ...) FUT_LOG(ch, Debug, __VA_ARGS__)
#define FUT_LOGT(ch, ...) FUT_LOG(ch, Trace, __VA_ARGS__)

// Catch-all for HLE/hook entry points (design 5.3: no C++ exception may leave an HLE entry):
//   NTSTATUS __stdcall NtFoo(...) noexcept { FUT_HLE_TRY { ... } FUT_HLE_CATCH("NtFoo") }
#define FUT_HLE_TRY try
#define FUT_HLE_CATCH(name)                                                                     \
    catch (const std::exception& e) {                                                           \
        ::core::Fatal("C++ exception in %s: %s", name, e.what());                               \
    }                                                                                           \
    catch (...) {                                                                               \
        ::core::Fatal("unknown C++ exception in %s", name);                                    \
    }
