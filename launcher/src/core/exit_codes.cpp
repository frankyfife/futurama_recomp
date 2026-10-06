#include "core/exit_codes.h"

#include "core/l10n.h"

namespace fl {

QString FormatExitCode(int exitCode) {
    const auto u = static_cast<quint32>(exitCode);
    if (u >= 0x80000000u) {
        return QStringLiteral("0x") + QString::number(u, 16).toUpper().rightJustified(8, QLatin1Char('0'));
    }
    return QString::number(exitCode);
}

QString ExitCategoryName(ExitCategory category) {
    switch (category) {
    case ExitCategory::Ok:
        return QStringLiteral("ok");
    case ExitCategory::ConfigError:
        return QStringLiteral("config");
    case ExitCategory::StageFailed:
        return QStringLiteral("stage");
    case ExitCategory::Fatal:
        return QStringLiteral("fatal");
    case ExitCategory::GameEnded:
        return QStringLiteral("ended");
    case ExitCategory::Watchdog:
        return QStringLiteral("watchdog");
    case ExitCategory::AlreadyRunning:
        return QStringLiteral("already_running");
    case ExitCategory::Starter:
        return QStringLiteral("starter");
    case ExitCategory::Crash:
        return QStringLiteral("crash");
    case ExitCategory::NotStarted:
        return QStringLiteral("not_started");
    case ExitCategory::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

ExitInfo DescribeExit(int exitCode, bool crashed, bool started) {
    ExitInfo info;
    if (!started) {
        info.category = ExitCategory::NotStarted;
        info.tone = ExitTone::Bad;
        info.title = T("exit.not_started.title");
        info.detail = T("exit.not_started.detail");
        return info;
    }
    const auto u = static_cast<quint32>(exitCode);
    if (u >= 0xC0000000u || (crashed && exitCode != 0 && (exitCode < 0 || exitCode > 14))) {
        info.category = ExitCategory::Crash;
        info.tone = ExitTone::Bad;
        info.title = T("exit.crash.title").arg(FormatExitCode(exitCode));
        info.detail = T("exit.crash.detail");
        return info;
    }
    switch (exitCode) {
    case 0:
        info = {ExitCategory::Ok, ExitTone::Good, T("exit.0.title"), T("exit.0.detail")};
        break;
    case 1:
        info = {ExitCategory::ConfigError, ExitTone::Bad, T("exit.1.title"), T("exit.1.detail")};
        break;
    case 2:
        info = {ExitCategory::StageFailed, ExitTone::Bad, T("exit.2.title"), T("exit.2.detail")};
        break;
    case 3:
        info = {ExitCategory::Fatal, ExitTone::Bad, T("exit.3.title"), T("exit.3.detail")};
        break;
    case 4:
        info = {ExitCategory::GameEnded, ExitTone::Neutral, T("exit.4.title"), T("exit.4.detail")};
        break;
    case 5:
        info = {ExitCategory::Watchdog, ExitTone::Bad, T("exit.5.title"), T("exit.5.detail")};
        break;
    case kGameExitAlreadyRunning:
        info = {ExitCategory::AlreadyRunning, ExitTone::Neutral, T("exit.6.title"), T("exit.6.detail")};
        break;
    case 10:
        info = {ExitCategory::Starter, ExitTone::Bad, T("exit.starter.title"), T("exit.10.detail")};
        break;
    case 11:
        info = {ExitCategory::Starter, ExitTone::Bad, T("exit.starter.title"), T("exit.11.detail")};
        break;
    case 12:
        info = {ExitCategory::Starter, ExitTone::Bad, T("exit.starter.title"), T("exit.12.detail")};
        break;
    case 13:
        info = {ExitCategory::Starter, ExitTone::Bad, T("exit.starter.title"), T("exit.13.detail")};
        break;
    case 14:
        info = {ExitCategory::Starter, ExitTone::Bad, T("exit.starter.title"), T("exit.14.detail")};
        break;
    default:
        info = {ExitCategory::Unknown, ExitTone::Bad, T("exit.unknown.title").arg(FormatExitCode(exitCode)),
                T("exit.unknown.detail")};
        break;
    }
    return info;
}

}  // namespace fl
