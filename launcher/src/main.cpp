// FuturamaLauncher.exe - settings editor and starter for the PC port (Qt 6 Widgets, 64 bit).
//
// Command line (all optional):
//   --config=<ini>            player INI (default: contract item 4 - %LOCALAPPDATA%\FuturamaPC\futurama.ini,
//                             or next to futurama.exe in portable mode)
//   --game-dir=<dir>          directory of futurama.exe (default: the launcher's directory, then
//   launcher.ini)
//   --lang=de|en              UI language (default: launcher.ini, else the system language)
//   --wizard                  open the first-run wizard
//   --refresh                 ignore the cached schema/GPU results
//   --schema-file=<json>      development: use this schema instead of futurama.exe --print-config-schema
//   --gpu-file=<json>         development: use this GPU description instead of futurama.exe --query-gpu
//   --audio-file=<json>       development: simulated output devices for the Audio tab (M5; default: WASAPI)
//   --set section.key=value   change a value (repeatable; validated against the schema)
//   --save                    save the INI and exit (unless --launch/--screenshots follow)
//   --screenshots=<dir>       render all tabs and wizard pages to PNG and exit (see ui/automation.h)
//   --screenshot-scale=<f>    QT_SCALE_FACTOR for the screenshots (e.g. 2.25)
//   --color-scheme=dark|light force a colour scheme
//   --launch [--wait] [--result=<file.json>]   start the game without the window
// Exit codes in automation modes: 0 ok, the game's exit code with --launch --wait, 64 usage (also unexpected
// positional arguments: an unquoted path with blanks), 65 invalid value, 66 game missing / not startable, 67 schema
// failed, 68 save failed (also an INI with invalid UTF-8, which automation does not overwrite), 69 screenshot
// failed, 70 another launcher uses the same INI.
// Review M3c finding 15: one launcher per INI (QLockFile in launcher_cache); a second GUI start brings the first
// window to the front.
#include "core/l10n.h"
#include "core/validation.h"
#include "ui/app_context.h"
#include "ui/app_style.h"
#include "ui/automation.h"
#include "ui/main_window.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include <windows.h>

#include <cstdio>
#include <memory>

namespace {

using namespace fl;
using namespace fl::ui;

void PrintError(const QString& message) {
    std::fputs((QStringLiteral("FuturamaLauncher: ") + message + QLatin1Char('\n')).toLocal8Bit().constData(),
               stderr);
    std::fflush(stderr);
}

void ShowError(const QString& message) {
    PrintError(message);
    QMessageBox box(QMessageBox::Critical, T("main.window_title"), message, QMessageBox::NoButton);
    box.addButton(T("common.ok"), QMessageBox::AcceptRole);
    box.setWindowIcon(AppIcon());
    box.exec();
}

// The visible top-level window of process `pid` to the front (restored if minimized). False if it has none (e.g.
// the launcher is hidden while its game runs).
bool ActivateProcessWindow(qint64 pid) {
    struct Search {
        DWORD pid;
        HWND found;
    } search{static_cast<DWORD>(pid), nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner == s->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                s->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (search.found == nullptr) {
        return false;
    }
    if (IsIconic(search.found)) {
        ShowWindow(search.found, SW_RESTORE);
    }
    SetForegroundWindow(search.found);
    return true;
}

// GUI: futurama.exe was not found - let the user pick its directory.
bool AskForGameDir(AppContext& ctx, const StartupOptions& options) {
    while (!ctx.paths.gameFound()) {
        QMessageBox box(QMessageBox::Warning, T("main.window_title"), T("startup.pick_game_dir"),
                        QMessageBox::NoButton);
        box.setWindowIcon(AppIcon());
        QPushButton* pick = box.addButton(T("startup.pick"), QMessageBox::AcceptRole);
        box.addButton(T("startup.quit"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != pick) {
            return false;
        }
        const QString dir = QFileDialog::getExistingDirectory(nullptr, T("startup.pick_title"));
        if (!dir.isEmpty()) {
            SetGameDir(ctx, QDir::toNativeSeparators(dir), options);
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Must be set before QApplication exists. The offscreen platform (headless --screenshots) reads fonts
    // from QT_QPA_FONTDIR only; without it every glyph is a box. The windows platform ignores the variable.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        const QString windir = qEnvironmentVariable("WINDIR", QStringLiteral("C:\\Windows"));
        qputenv("QT_QPA_FONTDIR", (windir + QStringLiteral("\\Fonts")).toLocal8Bit());
    }
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg.startsWith("--screenshot-scale=")) {
            qputenv("QT_SCALE_FACTOR", arg.mid(19));
        }
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("FuturamaLauncher"));
    QApplication::setApplicationVersion(QStringLiteral(LAUNCHER_VERSION));
    QApplication::setWindowIcon(AppIcon());
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        // Headless screenshots: the same UI font as on the desktop (the offscreen font database would pick
        // the first font file it finds).
        QApplication::setFont(QFont(QStringLiteral("Segoe UI"), 9));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("FuturamaPC launcher"));
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption configOption(QStringLiteral("config"), QStringLiteral("Player INI file."),
                                          QStringLiteral("ini"));
    const QCommandLineOption gameDirOption(
        QStringLiteral("game-dir"), QStringLiteral("Directory of futurama.exe."), QStringLiteral("dir"));
    const QCommandLineOption langOption(QStringLiteral("lang"), QStringLiteral("UI language (de|en)."),
                                        QStringLiteral("lang"));
    const QCommandLineOption wizardOption(QStringLiteral("wizard"),
                                          QStringLiteral("Open the first-run wizard."));
    const QCommandLineOption refreshOption(QStringLiteral("refresh"),
                                           QStringLiteral("Ignore cached schema/GPU results."));
    const QCommandLineOption schemaOption(
        QStringLiteral("schema-file"), QStringLiteral("Schema JSON (development)."), QStringLiteral("json"));
    const QCommandLineOption gpuOption(QStringLiteral("gpu-file"), QStringLiteral("GPU JSON (development)."),
                                       QStringLiteral("json"));
    const QCommandLineOption audioOption(QStringLiteral("audio-file"),
                                         QStringLiteral("Audio device JSON (development)."), QStringLiteral("json"));
    const QCommandLineOption setOption(QStringLiteral("set"),
                                       QStringLiteral("section.key=value (repeatable)."),
                                       QStringLiteral("assignment"));
    const QCommandLineOption saveOption(QStringLiteral("save"), QStringLiteral("Save the INI."));
    const QCommandLineOption shotsOption(QStringLiteral("screenshots"),
                                         QStringLiteral("Render all pages to PNG."), QStringLiteral("dir"));
    const QCommandLineOption scaleOption(QStringLiteral("screenshot-scale"),
                                         QStringLiteral("Scale factor for screenshots."),
                                         QStringLiteral("factor"));
    const QCommandLineOption schemeOption(QStringLiteral("color-scheme"), QStringLiteral("dark|light."),
                                          QStringLiteral("scheme"));
    const QCommandLineOption launchOption(QStringLiteral("launch"),
                                          QStringLiteral("Start the game without the window."));
    const QCommandLineOption waitOption(QStringLiteral("wait"),
                                        QStringLiteral("With --launch: wait for the game."));
    const QCommandLineOption resultOption(QStringLiteral("result"),
                                          QStringLiteral("With --launch --wait: result JSON."),
                                          QStringLiteral("json"));
    for (const QCommandLineOption* o :
         {&configOption, &gameDirOption, &langOption, &wizardOption, &refreshOption, &schemaOption,
          &gpuOption, &audioOption, &setOption, &saveOption, &shotsOption, &scaleOption, &schemeOption, &launchOption,
          &waitOption, &resultOption}) {
        parser.addOption(*o);
    }
    const bool parsed = parser.parse(QApplication::arguments());
    const bool automation =
        parser.isSet(saveOption) || parser.isSet(shotsOption) || parser.isSet(launchOption);
    const QString resultFile = parser.value(resultOption);

    ApplyAppStyle(app, parser.value(schemeOption));

    StartupOptions options;
    options.gameDir = parser.value(gameDirOption);
    options.config = parser.value(configOption);
    options.schemaFile = parser.value(schemaOption);
    options.gpuFile = parser.value(gpuOption);
    options.audioFile = parser.value(audioOption);
    options.refresh = parser.isSet(refreshOption);

    AppContext ctx;
    ctx.launcherDir = QDir::toNativeSeparators(QApplication::applicationDirPath());
    InitPaths(ctx, options);

    // UI language: --lang, launcher.ini, system.
    Language language = SystemLanguage();
    if (const auto stored = ParseLanguageCode(ctx.setting(QStringLiteral("ui/language")))) {
        language = *stored;
    }
    if (parser.isSet(langOption)) {
        if (const auto lang = ParseLanguageCode(parser.value(langOption))) {
            language = *lang;
        }
    }
    SetLanguage(language);

    // Review M3c finding 11: QCommandLineParser accepts positional arguments silently - an unquoted path with blanks
    // ("--config=C:\My Games\futurama.ini") would lose its tail and the launcher would save to another file.
    QString usageError = parsed ? QString() : parser.errorText();
    if (parsed && !parser.positionalArguments().isEmpty()) {
        usageError = T("startup.positional").arg(parser.positionalArguments().join(QLatin1Char(' ')));
    }
    if (!usageError.isEmpty()) {
        const QString message = usageError;
        if (automation) {
            PrintError(message);
            WriteLauncherError(resultFile, message, kLauncherUsageError);
        } else {
            ShowError(message);
        }
        return kLauncherUsageError;
    }
    if (parser.isSet(helpOption)) {
        PrintError(QStringLiteral("\n") + parser.helpText());
        return 0;
    }

    if (!ctx.paths.gameFound() && options.schemaFile.isEmpty()) {
        if (automation || !AskForGameDir(ctx, options)) {
            const QString message = T("startup.game_missing");
            if (automation) {
                PrintError(message);
                WriteLauncherError(resultFile, message, kLauncherGameMissing);
            }
            return kLauncherGameMissing;
        }
    }

    // Review M3c finding 15: one launcher per INI. Screenshot runs without --save/--launch do not write and need none.
    std::unique_ptr<QLockFile> lock;
    const bool writes = !parser.isSet(shotsOption) || parser.isSet(saveOption) || parser.isSet(launchOption);
    if (writes && !ctx.paths.iniPath.isEmpty()) {
        const QString cacheDir = ctx.paths.cacheDir();
        QDir().mkpath(cacheDir);
        lock = std::make_unique<QLockFile>(
            QDir(cacheDir).filePath(QFileInfo(ctx.paths.iniPath).fileName() + QStringLiteral(".lock")));
        lock->setStaleLockTime(0);  // stale only when the holder's process is gone
        if (!lock->tryLock(0)) {
            if (lock->error() == QLockFile::LockFailedError) {
                qint64 pid = 0;
                QString lockHost, lockApp;
                lock->getLockInfo(&pid, &lockHost, &lockApp);
                if (automation) {
                    const QString message = T("startup.already_running_cli").arg(ctx.paths.iniPath).arg(pid);
                    PrintError(message);
                    WriteLauncherError(resultFile, message, kLauncherAlreadyRunning);
                    return kLauncherAlreadyRunning;
                }
                if (!ActivateProcessWindow(pid)) {
                    ShowError(T("startup.already_running").arg(ctx.paths.iniPath));
                }
                return 0;
            }
            lock.reset();  // no lock possible (read-only folder): saving reports its own error
        }
    }

    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool schemaOk = LoadSchema(ctx, options, &error);
    if (schemaOk) {
        LoadGpu(ctx, options);
        if (!parser.isSet(launchOption) && !parser.isSet(saveOption)) {
            LoadAudioDevices(ctx, options);  // M5: Audio tab (window and screenshots)
        }
    }
    QApplication::restoreOverrideCursor();
    if (!schemaOk) {
        if (automation) {
            PrintError(error);
            WriteLauncherError(resultFile, error, kLauncherSchemaFailed);
        } else {
            ShowError(error);
        }
        return kLauncherSchemaFailed;
    }
    if (!LoadIni(ctx, &error)) {
        if (automation) {
            PrintError(error);
            WriteLauncherError(resultFile, error, kLauncherSaveFailed);
        } else {
            ShowError(error);
        }
        return kLauncherSaveFailed;
    }

    // --set section.key=value
    for (const QString& assignment : parser.values(setOption)) {
        const qsizetype eq = assignment.indexOf(QLatin1Char('='));
        const QString name = eq > 0 ? assignment.left(eq).trimmed() : QString();
        QString value = eq > 0 ? assignment.mid(eq + 1) : QString();
        const KeyDef* key = name.isEmpty() ? nullptr : ctx.model.schema().find(name);
        if (key != nullptr && (key->type == KeyType::PathFile || key->type == KeyType::PathDir)) {
            value = CleanPathInput(value);  // review M3c finding 10: --set paths.iso="C:\x y.iso" with the quotes
        }
        QString message;
        if (key == nullptr) {
            message = T("startup.set_unknown").arg(assignment);
        } else if (const ValidationResult v = ValidateValue(*key, value, ctx.model.iniDir()); v.isError()) {
            message = T("startup.set_invalid").arg(assignment, v.message);
        }
        if (!message.isEmpty()) {
            if (automation) {
                PrintError(message);
                WriteLauncherError(resultFile, message, kLauncherInvalidValue);
            } else {
                ShowError(message);
            }
            return kLauncherInvalidValue;
        }
        ctx.model.setValue(*key, value);
    }

    // --launch starts the game with the INI on disk, so it saves like the "Spielen" button does.
    if (parser.isSet(saveOption) ||
        (parser.isSet(launchOption) && (ctx.model.isDirty() || !ctx.model.fileExists()))) {
        if (ctx.model.ini().hasInvalidUtf8()) {
            // Review M3c finding 20: saving would replace the invalid bytes with U+FFFD - not without a person.
            const QString message = T("startup.invalid_utf8").arg(ctx.paths.iniPath);
            PrintError(message);
            WriteLauncherError(resultFile, message, kLauncherSaveFailed);
            return kLauncherSaveFailed;
        }
        if (!ctx.model.keysWithErrors().isEmpty()) {
            QStringList names;
            for (const KeyDef* k : ctx.model.keysWithErrors()) {
                names.append(k->fullName() + QStringLiteral(" (") + ctx.model.validate(*k).message +
                             QLatin1Char(')'));
            }
            const QString message = T("main.save_blocked").arg(names.join(QStringLiteral(", ")));
            PrintError(message);
            WriteLauncherError(resultFile, message, kLauncherInvalidValue);
            return kLauncherInvalidValue;
        }
        if (!ctx.model.save(&error, ctx.newIniHeader())) {
            const QString message = T("main.save_failed").arg(ctx.paths.iniPath, error);
            PrintError(message);
            WriteLauncherError(resultFile, message, kLauncherSaveFailed);
            return kLauncherSaveFailed;
        }
    }

    if (parser.isSet(shotsOption)) {
        const int rc = RunScreenshots(ctx, options, parser.value(shotsOption), &error);
        if (rc != 0) {
            PrintError(error);
            return rc;
        }
        if (!parser.isSet(launchOption)) {
            return 0;
        }
    }

    if (parser.isSet(launchOption)) {
        const int rc = RunLaunch(ctx, parser.isSet(waitOption), resultFile, &error);
        if (!error.isEmpty()) {
            PrintError(error);
        }
        return rc;
    }
    if (automation) {
        return 0;  // --save alone
    }

    MainWindow window(ctx, options);
    window.show();
    const KeyDef* iso = ctx.model.schema().find(QStringLiteral("paths"), QStringLiteral("iso"));
    const bool firstRun =
        !ctx.model.fileExists() || (iso != nullptr && ctx.model.value(*iso).trimmed().isEmpty());
    if (firstRun || parser.isSet(wizardOption)) {
        QTimer::singleShot(0, &window, [&window] { window.runWizard(); });
    }
    return QApplication::exec();
}
