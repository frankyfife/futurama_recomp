// Meaning of the exit codes of futurama.exe (src/host/core/host_main.h, src/launcher/launcher.c):
//   0 ok (normal end, frame limit, stop-at)     1 startup/configuration error
//   2 a startup stage failed                     3 fatal error (core::Fatal, KeBugCheck-style)
//   4 the game ended itself (dashboard, ...)     5 watchdog ([debug] timeout, thread dump in the log)
//   6 another game already runs with the same user data folder (review M3c finding 15; nothing written)
//   10..14 errors of the x86 starter (TLS slots, placeholder, futurama_host.dll, HostMain, base address)
//   >= 0xC0000000 an unhandled Windows exception (crash)
#pragma once

#include <QString>

namespace fl {

inline constexpr int kGameExitAlreadyRunning = 6;  // core::kExitAlreadyRunning (src/host/core/host_main.h)

enum class ExitCategory {
    Ok,
    ConfigError,
    StageFailed,
    Fatal,
    GameEnded,
    Watchdog,
    AlreadyRunning,
    Starter,
    Crash,
    NotStarted,
    Unknown
};
enum class ExitTone { Good, Neutral, Bad };

struct ExitInfo {
    ExitCategory category = ExitCategory::Unknown;
    ExitTone tone = ExitTone::Bad;
    QString title;   // localized one-liner
    QString detail;  // localized explanation / hint
};

// `started` false = the process could not be started at all.
ExitInfo DescribeExit(int exitCode, bool crashed, bool started = true);
QString ExitCategoryName(ExitCategory category);  // "ok", "config", "stage", ... (result JSON)
QString FormatExitCode(int exitCode);             // "3" or "0xC0000005"

}  // namespace fl
