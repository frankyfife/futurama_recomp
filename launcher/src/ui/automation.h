// Automation modes for tests and scripts (no dialogs):
//   --screenshots=<dir>  renders every tab, the result panel and every wizard page in German and English
//                        to <dir>\<de|en>_*.png (works with -platform offscreen; --screenshot-scale=2.25
//                        renders at 225 %, --color-scheme=dark|light forces a scheme)
//   --launch [--wait] [--result=<file.json>]
//                        starts futurama.exe --config=<ini>; with --wait the launcher waits, writes the
//                        result JSON (GameRunResult::toJson) and exits with the game's exit code
#pragma once

#include "ui/app_context.h"

#include <QString>

namespace fl::ui {

// Exit codes of the launcher itself in automation modes (the game's codes are 0..14 or exceptions).
inline constexpr int kLauncherUsageError = 64;
inline constexpr int kLauncherInvalidValue = 65;
inline constexpr int kLauncherGameMissing = 66;
inline constexpr int kLauncherSchemaFailed = 67;
inline constexpr int kLauncherSaveFailed = 68;
inline constexpr int kLauncherScreenshotFailed = 69;
// Review M3c finding 15: another launcher holds the lock of the same INI (<config dir>\launcher_cache\<ini>.lock).
inline constexpr int kLauncherAlreadyRunning = 70;

int RunScreenshots(AppContext& ctx, const StartupOptions& options, const QString& dir, QString* error);
int RunLaunch(AppContext& ctx, bool wait, const QString& resultFile, QString* error);
// Writes {"launcher_error": message, "launcher_exit_code": code} (used when --launch fails early).
void WriteLauncherError(const QString& resultFile, const QString& message, int code);

}  // namespace fl::ui
