#include "ui/automation.h"

#include "core/game_session.h"
#include "core/l10n.h"
#include "core/pad_reader.h"
#include "ui/binding_capture.h"
#include "ui/controls_panel.h"
#include "ui/main_window.h"
#include "ui/pad_poller.h"
#include "ui/settings_tab.h"
#include "ui/wizard.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QSaveFile>
#include <QTabWidget>

#include <memory>

namespace fl::ui {
namespace {

void Settle() {
    for (int i = 0; i < 5; ++i) {
        QApplication::processEvents();
    }
}

bool SaveGrab(QWidget* widget, const QString& dir, const QString& name, QStringList* written,
              QString* error) {
    Settle();
    // render() instead of grab(): a child widget without its own background would come out transparent
    // (white text of the dark scheme on a transparent PNG).
    const qreal dpr = widget->devicePixelRatioF();
    QPixmap pixmap((QSizeF(widget->size()) * dpr).toSize());
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(widget->window()->palette().color(QPalette::Window));
    widget->render(&pixmap, QPoint(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);
    const QString path = QDir(dir).filePath(name + QStringLiteral(".png"));
    if (pixmap.isNull() || !pixmap.save(path, "PNG")) {
        *error = QStringLiteral("cannot write ") + QDir::toNativeSeparators(path);
        return false;
    }
    written->append(name);
    return true;
}

GameRunResult SampleResult(const AppContext& ctx, int exitCode) {
    GameRunResult r;
    r.started = true;
    r.exitCode = exitCode;
    r.startTime = QDateTime::currentDateTime().addSecs(-75);
    r.endTime = QDateTime::currentDateTime();
    r.exe = ctx.paths.gameExe;
    r.arguments = GameSession::Arguments(ctx.paths.iniPath);
    r.logFile = ctx.logFile();
    r.logFromThisRun = true;
    for (int i = 0; i < kLogTailLines; ++i) {
        r.logTail.append(QStringLiteral("[   %1] guest-main GAME  info  %2 %3")
                             .arg(70.0 + i * 0.1, 7, 'f', 3)
                             .arg(T("shot.sample_line"))
                             .arg(i + 1));
    }
    if (exitCode == 3) {
        r.logTail.last() =
            QStringLiteral("[   73.000] guest-main CORE  error %1").arg(T("shot.sample_fatal"));
    }
    return r;
}

const char* TabFileName(UiTab tab) {
    switch (tab) {
    case UiTab::Game:
        return "game";
    case UiTab::Graphics:
        return "graphics";
    case UiTab::Controls:
        return "controls";
    case UiTab::Audio:
        return "audio";
    case UiTab::Paths:
        return "paths";
    case UiTab::Advanced:
        return "advanced";
    }
    return "tab";
}

const char* WizardPageFileName(int id) {
    switch (id) {
    case FirstRunWizard::WelcomePage:
        return "1_welcome";
    case FirstRunWizard::IsoPage:
        return "2_iso";
    case FirstRunWizard::LanguagePage:
        return "3_language";
    case FirstRunWizard::DisplayPage:
        return "4_display";
    case FirstRunWizard::QualityPage:
        return "5_quality";
    case FirstRunWizard::FinishPage:
        return "6_finish";
    default:
        return "page";
    }
}

}  // namespace

int RunScreenshots(AppContext& ctx, const StartupOptions& options, const QString& dir, QString* error) {
    if (!QDir().mkpath(dir)) {
        *error = QStringLiteral("cannot create ") + QDir::toNativeSeparators(dir);
        return kLauncherScreenshotFailed;
    }
    const Language original = CurrentLanguage();
    QStringList written;
    for (Language lang : {Language::German, Language::English}) {
        SetLanguage(lang);
        const QString prefix = LanguageCode(lang) + QLatin1Char('_');
        {
            MainWindow window(ctx, options);
            window.setAttribute(Qt::WA_DontShowOnScreen, true);
            const int unit = window.fontMetrics().height();
            window.resize(unit * 64, unit * 50);  // the default size of the main window
            window.show();
            window.prepareScreenshot();
            for (UiTab tab : kAllTabs) {
                window.selectTab(tab);
                if (!SaveGrab(&window, dir,
                              prefix + QStringLiteral("main_") + QLatin1String(TabFileName(tab)), &written,
                              error)) {
                    return kLauncherScreenshotFailed;
                }
                // The whole scrolled content of the tab.
                auto* page = qobject_cast<SettingsTab*>(window.tabWidget()->currentWidget());
                if (page != nullptr &&
                    !SaveGrab(page->contentWidget(), dir,
                              prefix + QStringLiteral("tab_") + QLatin1String(TabFileName(tab)) +
                                  QStringLiteral("_full"),
                              &written, error)) {
                    return kLauncherScreenshotFailed;
                }
            }
            // M4: the keyboard/mouse table of the controls tab (the first grab shows the controller table).
            if (BindingsPanel* bindings = window.bindingsPanel(); bindings != nullptr && bindings->hasBindings()) {
                window.selectTab(UiTab::Controls);
                bindings->selectDevice(BindDevice::KeyboardMouse);
                auto* page = qobject_cast<SettingsTab*>(window.tabWidget()->currentWidget());
                if (page != nullptr &&
                    !SaveGrab(page->contentWidget(), dir, prefix + QStringLiteral("tab_controls_kbm_full"), &written,
                              error)) {
                    return kLauncherScreenshotFailed;
                }
                // The capture dialog ("press a key") for the keyboard/mouse binding of A.
                if (const KeyDef* kbmA = ctx.model.schema().find(QStringLiteral("input.kbm_a"))) {
                    if (BindingCaptureDialog* capture = bindings->openCapture(*kbmA, false)) {
                        // Lay the dialog out at its real size first (offscreen it kept the size of its first
                        // layout pass and clipped the mouse buttons at 225 %; review M4 finding 17).
                        Settle();
                        capture->adjustSize();
                        capture->resize(capture->sizeHint().expandedTo(capture->minimumSizeHint()));
                        if (!SaveGrab(capture, dir, prefix + QStringLiteral("controls_capture_kbm"), &written, error)) {
                            return kLauncherScreenshotFailed;
                        }
                        capture->reject();
                    }
                }
                // The controller capture dialog (with the machine's controllers - usually none: the dialog says
                // so and offers the list).
                if (const KeyDef* padA = ctx.model.schema().find(QStringLiteral("input.pad_a"))) {
                    if (BindingCaptureDialog* capture = bindings->openCapture(*padA, false)) {
                        Settle();
                        capture->adjustSize();
                        capture->resize(capture->sizeHint().expandedTo(capture->minimumSizeHint()));
                        if (!SaveGrab(capture, dir, prefix + QStringLiteral("controls_capture_pad"), &written, error)) {
                            return kLauncherScreenshotFailed;
                        }
                        capture->reject();
                    }
                }
                bindings->selectDevice(BindDevice::Pad);
            }
            window.selectTab(UiTab::Game);
            window.showRunResult(SampleResult(ctx, 3));
            if (!SaveGrab(&window, dir, prefix + QStringLiteral("status_fatal"), &written, error)) {
                return kLauncherScreenshotFailed;
            }
            window.showRunResult(SampleResult(ctx, 0));
            if (!SaveGrab(&window, dir, prefix + QStringLiteral("status_ok"), &written, error)) {
                return kLauncherScreenshotFailed;
            }
            // Review M3c finding 12: a notebook's free area (1920 x 1080 at 150 %: about 1280 x 690 logical
            // pixels) with the result panel open - the footer and the tabs must stay usable.
            window.showRunResult(SampleResult(ctx, 3));
            window.resize(1280, 690);
            if (!SaveGrab(&window, dir, prefix + QStringLiteral("main_small_status"), &written, error)) {
                return kLauncherScreenshotFailed;
            }
            window.resize(unit * 64, unit * 50);
            window.hide();
        }
        {
            // M4: the controller test with a simulated controller (the machine may have none; the panel says
            // "simuliert"): A and RT pressed, left stick up-right, right stick left.
            auto reader = std::make_unique<SimulatedPadReader>();
            PadState demo;
            demo.connected = true;
            demo.buttons = 0x1000;
            demo.rightTrigger = 220;
            demo.lx = 16000;
            demo.ly = 26000;
            demo.rx = -30000;
            reader->setState(0, demo);
            PadPoller poller(std::move(reader));
            PadTestPanel panel(ctx.model, &poller);
            panel.setAttribute(Qt::WA_DontShowOnScreen, true);
            panel.resize(panel.sizeHint().expandedTo(QSize(panel.fontMetrics().height() * 60, 0)));
            panel.show();
            poller.pollNow();
            // The connected slot appears only after the first poll: lay the panel out again (its height grows).
            Settle();
            panel.resize(panel.sizeHint().expandedTo(QSize(panel.fontMetrics().height() * 60, 0)));
            if (!SaveGrab(&panel, dir, prefix + QStringLiteral("controls_pad_test_simulated"), &written, error)) {
                return kLauncherScreenshotFailed;
            }
            panel.hide();
        }
        {
            FirstRunWizard wizard(ctx);
            wizard.setAttribute(Qt::WA_DontShowOnScreen, true);
            wizard.show();
            wizard.checkIsoNow();
            Settle();
            int guard = 0;
            while (wizard.currentPage() != nullptr && guard++ < 10) {
                const int id = wizard.currentId();
                if (!SaveGrab(&wizard, dir,
                              prefix + QStringLiteral("wizard_") + QLatin1String(WizardPageFileName(id)),
                              &written, error)) {
                    return kLauncherScreenshotFailed;
                }
                if (wizard.nextPageAfter(id) < 0) {
                    break;
                }
                wizard.next();
            }
            wizard.hide();
        }
    }
    SetLanguage(original);
    // Index of what was rendered (for the checks).
    QSaveFile index(QDir(dir).filePath(QStringLiteral("index.txt")));
    if (index.open(QIODevice::WriteOnly)) {
        index.write((written.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
        index.commit();
    }
    return 0;
}

void WriteLauncherError(const QString& resultFile, const QString& message, int code) {
    if (resultFile.isEmpty()) {
        return;
    }
    QJsonObject o;
    o.insert(QStringLiteral("launcher_error"), message);
    o.insert(QStringLiteral("launcher_exit_code"), code);
    QSaveFile f(resultFile);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson());
        f.commit();
    }
}

int RunLaunch(AppContext& ctx, bool wait, const QString& resultFile, QString* error) {
    if (!ctx.paths.gameFound()) {
        *error = T("startup.game_missing");
        WriteLauncherError(resultFile, *error, kLauncherGameMissing);
        return kLauncherGameMissing;
    }
    if (!wait) {
        const bool started =
            QProcess::startDetached(ctx.paths.gameExe, GameSession::Arguments(ctx.paths.iniPath),
                                    QFileInfo(ctx.paths.gameExe).absolutePath());
        if (!started) {
            *error = T("cli.start_failed").arg(ctx.paths.gameExe, QStringLiteral("startDetached"));
            WriteLauncherError(resultFile, *error, kLauncherGameMissing);
            return kLauncherGameMissing;
        }
        return 0;
    }
    GameSession session;
    session.start(ctx.paths.gameExe, ctx.paths.iniPath, ctx.logFile());
    const GameRunResult result = session.waitForFinished();
    if (!resultFile.isEmpty()) {
        QSaveFile f(resultFile);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(result.toJson()).toJson());
            f.commit();
        }
    }
    if (!result.started) {
        *error = result.startError;
        return kLauncherGameMissing;
    }
    return result.exitCode;
}

}  // namespace fl::ui
