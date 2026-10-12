// Widget tests of the launcher (offscreen platform): editing + saving in the main window, presets, the
// language switch, a full game run with the test double (exit code 3 -> result panel) and the wizard.
#include "core/audio_output.h"
#include "core/bindings.h"
#include "core/l10n.h"
#include "core/pad_reader.h"
#include "test_support.h"
#include "ui/app_context.h"
#include "ui/audio_panel.h"
#include "ui/binding_capture.h"
#include "ui/controls_panel.h"
#include "ui/key_editors.h"
#include "ui/main_window.h"
#include "ui/pad_poller.h"
#include "ui/screen_info.h"
#include "ui/settings_tab.h"
#include "ui/app_style.h"
#include "ui/wizard.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QToolButton>
#include <QWheelEvent>
#include <QTimer>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTest>
#include <QThread>

#include <atomic>

#include <windows.h>

using namespace fl;
using namespace fl::ui;

namespace {
// Confirmation boxes of the resets (ui/app_style.h ConfirmChange): answered by the test, the last question kept.
bool g_confirmAnswer = true;
QString g_confirmText;
int g_confirmCount = 0;
bool TestConfirm(const QString& text) {
    g_confirmText = text;
    ++g_confirmCount;
    return g_confirmAnswer;
}
}  // namespace

class UiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void editPresetAndSave();
    void languageSwitch();
    void playShowsResult();
    void wizardFlow();
    void displayHelpers();
    void dependentKeysAndIsoStatus();
    void failureCauseFromStderr();
    void unsupportedIsoAsksBeforePlay();
    void quotedPathInput();
    void controlsBindingEditor();
    void controlsPadCaptureAndTest();
    void controlsInvalidBinding();
    void controlsReviewFixes();
    void audioTab();

private:
    void makeContext(AppContext& ctx, const QString& name);
    QString iso_;
    QString gameDir_;
    StartupOptions options_;
};

void UiTest::initTestCase() {
    SetLanguage(Language::German);
    SetConfirmHook(&TestConfirm);
    const QString dir = test::MakeTestDir(QStringLiteral("ui images (Jun 17, 2003)"));
    iso_ = QDir::toNativeSeparators(QDir(dir).filePath(test::SpecialIsoName()));
    QByteArray data(0x10000 + 0x800, '\0');
    const QByteArray magic("MICROSOFT*XBOX*MEDIA");
    data.replace(0x10000, magic.size(), magic);
    QVERIFY(test::WriteFile(iso_, data));
    gameDir_ = QFileInfo(QStringLiteral(LAUNCHER_FAKE_GAME)).absolutePath();
}

void UiTest::makeContext(AppContext& ctx, const QString& name) {
    const QString dir = test::MakeTestDir(name);
    options_ = StartupOptions{};
    options_.gameDir = gameDir_;
    options_.config = QDir(dir).filePath(QStringLiteral("futurama.ini"));
    ctx.launcherDir = QCoreApplication::applicationDirPath();
    InitPaths(ctx, options_);
    QString error;
    QVERIFY2(LoadSchema(ctx, options_, &error), qPrintable(error));
    LoadGpu(ctx, options_);
    QVERIFY2(LoadIni(ctx, &error), qPrintable(error));
    // The game's user data next to the INI (never %LOCALAPPDATA% in tests).
    QVERIFY(ctx.model.setValue(QStringLiteral("paths.userdata"),
                               QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral("user data")))));
}

void UiTest::editPresetAndSave() {
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui edit"));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto* msaa = w.findChild<KeyEditor*>(QStringLiteral("render.msaa"));
    QVERIFY(msaa != nullptr);
    auto* combo = msaa->findChild<QComboBox*>();
    const int eight = combo->findData(QStringLiteral("8"));
    QVERIFY(eight >= 0);
    combo->setCurrentIndex(eight);
    emit combo->activated(eight);
    QCOMPARE(ctx.model.value(*ctx.model.schema().find(QStringLiteral("render.msaa"))), QStringLiteral("8"));
    QVERIFY(w.findChild<QLabel*>(QStringLiteral("footerStatus"))
                ->text()
                .contains(QLatin1String("Ungespeichert")));

    auto* isoEditor = w.findChild<KeyEditor*>(QStringLiteral("paths.iso"));
    auto* isoLine = isoEditor->findChild<QLineEdit*>();
    isoLine->setText(iso_);
    emit isoLine->textEdited(iso_);

    // Preset "Maximum" through the graphics tab.
    auto* preset = w.findChild<QComboBox*>(QStringLiteral("presetCombo"));
    const int maximum = preset->findData(static_cast<int>(QualityPreset::Maximum));
    preset->setCurrentIndex(maximum);
    emit preset->activated(maximum);
    QCOMPARE(ctx.model.value(*ctx.model.schema().find(QStringLiteral("render.render_scale"))),
             QStringLiteral("2.0"));
    QCOMPARE(preset->currentData().toInt(), static_cast<int>(QualityPreset::Maximum));

    QTest::mouseClick(w.findChild<QPushButton*>(QStringLiteral("saveButton")), Qt::LeftButton);
    QVERIFY(!ctx.model.isDirty());
    const QString ini = QString::fromUtf8(test::ReadFile(ctx.paths.iniPath));
    QVERIFY2(ini.contains(QRegularExpression(QStringLiteral("\nmsaa *= 8\r\n"))), qPrintable(ini));
    QVERIFY2(ini.contains(QRegularExpression(QStringLiteral("\nrender_scale *= 2.0\r\n"))), qPrintable(ini));
    QVERIFY(ini.contains(QStringLiteral("iso = ") + iso_));
    QVERIFY(
        w.findChild<QLabel*>(QStringLiteral("footerStatus"))->text().contains(QLatin1String("gespeichert")));
}

void UiTest::languageSwitch() {
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui language"));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* lang = w.findChild<QComboBox*>(QStringLiteral("languageCombo"));
    lang->setCurrentIndex(1);
    emit lang->activated(1);
    QTRY_VERIFY(w.tabWidget() != nullptr && w.tabWidget()->tabText(1).startsWith(QLatin1String("Graphics")));
    QCOMPARE(CurrentLanguage(), Language::English);
    QCOMPARE(ctx.setting(QStringLiteral("ui/language")), QStringLiteral("en"));
    // The pending value from makeContext survived the rebuild.
    QVERIFY(ctx.model.isDirty());
    auto* lang2 = w.findChild<QComboBox*>(QStringLiteral("languageCombo"));
    lang2->setCurrentIndex(0);
    emit lang2->activated(0);
    QTRY_VERIFY(w.tabWidget()->tabText(1).startsWith(QLatin1String("Grafik")));
}

void UiTest::playShowsResult() {
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui play (x,y)"));
    QVERIFY(ctx.model.setValue(QStringLiteral("paths.iso"), iso_));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    qputenv("FAKE_FUTURAMA_EXIT", "3");
    qputenv("FAKE_FUTURAMA_SLEEP_MS", "300");
    QSignalSpy finished(&w, &MainWindow::gameFinished);
    QTest::mouseClick(w.findChild<QPushButton*>(QStringLiteral("playButton")), Qt::LeftButton);
    QVERIFY(!w.isVisible());  // hidden while the game runs
    QVERIFY(finished.wait(30000));
    qunsetenv("FAKE_FUTURAMA_EXIT");
    qunsetenv("FAKE_FUTURAMA_SLEEP_MS");
    QVERIFY(w.isVisible());
    QVERIFY(w.findChild<QWidget*>(QStringLiteral("statusPanel"))->isVisible());
    const QString title = w.findChild<QLabel*>(QStringLiteral("statusTitle"))->text();
    QVERIFY2(title.contains(QStringLiteral("Rückgabecode 3")), qPrintable(title));
    const auto result = finished.first().at(0).value<GameRunResult>();
    QCOMPARE(result.exitCode, 3);
    QVERIFY(result.logFromThisRun);
    QCOMPARE(result.logTail.size(), kLogTailLines);
    QVERIFY(test::ReadFile(ctx.paths.iniPath).contains("[paths]"));  // saved before the start
}

void UiTest::wizardFlow() {
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui wizard"));
    FirstRunWizard wizard(ctx);
    wizard.show();
    QVERIFY(QTest::qWaitForWindowExposed(&wizard));
    QCOMPARE(wizard.currentId(), static_cast<int>(FirstRunWizard::WelcomePage));
    wizard.next();
    QCOMPARE(wizard.currentId(), static_cast<int>(FirstRunWizard::IsoPage));
    QVERIFY(!wizard.currentPage()->isComplete());
    QSignalSpy checked(&wizard, &FirstRunWizard::isoStateChanged);
    auto* edit = wizard.findChild<QLineEdit*>(QStringLiteral("isoEdit"));
    edit->setText(iso_);
    emit edit->textEdited(iso_);
    QVERIFY(checked.wait(30000));
    QTRY_VERIFY(wizard.currentPage()->isComplete());
    QCOMPARE(wizard.state().check.verdict(), IsoVerdict::Supported);
    wizard.next();
    QCOMPARE(wizard.currentId(), static_cast<int>(FirstRunWizard::LanguagePage));
    wizard.next();
    QCOMPARE(wizard.currentId(), static_cast<int>(FirstRunWizard::DisplayPage));
    wizard.next();
    QCOMPARE(wizard.currentId(), static_cast<int>(FirstRunWizard::QualityPage));
    for (QRadioButton* radio : wizard.currentPage()->findChildren<QRadioButton*>()) {
        if (radio->text() == PresetTitle(QualityPreset::Classic)) {
            radio->click();
        }
    }
    QCOMPARE(wizard.state().preset, QualityPreset::Classic);
    wizard.next();
    QCOMPARE(wizard.currentId(), static_cast<int>(FirstRunWizard::FinishPage));
    wizard.applyToModel();
    const ConfigSchema& schema = ctx.model.schema();
    QCOMPARE(ctx.model.value(*schema.find(QStringLiteral("paths.iso"))), iso_);
    QCOMPARE(ctx.model.value(*schema.find(QStringLiteral("render.resolution"))), QStringLiteral("640x480"));
    QCOMPARE(ctx.model.value(*schema.find(QStringLiteral("render.msaa"))), QStringLiteral("off"));
    QCOMPARE(ctx.model.value(*schema.find(QStringLiteral("settings.language"))), QStringLiteral("german"));
}

void UiTest::displayHelpers() {
    // The 4:3 area the game renders with resolution auto (platform::FitAspectRect rounding).
    QCOMPARE(FitAspect43(QSize(3840, 2160)), QSize(2880, 2160));
    QCOMPARE(FitAspect43(QSize(1920, 1080)), QSize(1440, 1080));
    QCOMPARE(FitAspect43(QSize(800, 1000)), QSize(800, 600));
    QCOMPARE(FitAspect43(QSize(2631, 1973)), QSize(2631, 1973));
    QCOMPARE(FitAspect43(QSize(0, 100)), QSize());
    // window_scale 2 at 225 % on the 3840x2160 TV: 2880x2160 client pixels, shrunk to the work area.
    GpuOutput tv;
    tv.rect = QRect(0, 0, 3840, 2160);
    tv.dpi = 216;
    WindowSizeEstimate e = EstimateWindowSize(2, &tv);
    QCOMPARE(e.client, QSize(2880, 2160));
    QVERIFY(e.shrunk);
    QVERIFY(e.fitted.height() < 2160);
    QVERIFY(qAbs(e.fitted.width() * 3 - e.fitted.height() * 4) <= 4);  // still 4:3
    e = EstimateWindowSize(1, &tv);
    QCOMPARE(e.client, QSize(1440, 1080));
    QVERIFY(!e.shrunk);
    e = EstimateWindowSize(2, nullptr);  // unknown monitor: 100 %
    QCOMPARE(e.client, QSize(1280, 960));
    QVERIFY(!e.shrunk);
}

void UiTest::dependentKeysAndIsoStatus() {
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui depend"));
    QVERIFY(ctx.model.setValue(QStringLiteral("paths.iso"), iso_));
    QVERIFY(ctx.model.setValue(QStringLiteral("render.msaa"), QStringLiteral("off")));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    // Alpha-to-coverage only has an effect with MSAA: disabled with a note until MSAA is on.
    QVERIFY(!w.findChild<KeyEditor*>(QStringLiteral("render.alpha_to_coverage"))->isEnabled());
    auto* msaa = w.findChild<KeyEditor*>(QStringLiteral("render.msaa"))->findChild<QComboBox*>();
    const int four = msaa->findData(QStringLiteral("4"));
    QVERIFY(four >= 0);
    msaa->setCurrentIndex(four);
    emit msaa->activated(four);
    QVERIFY(w.findChild<KeyEditor*>(QStringLiteral("render.alpha_to_coverage"))->isEnabled());
    // Window size: the factor and the resulting size in pixels.
    auto* scale = w.findChild<KeyEditor*>(QStringLiteral("render.window_scale"));
    QVERIFY(scale != nullptr);
    QLabel* info = scale->findChild<QLabel*>();
    QVERIFY(info != nullptr);
    QVERIFY2(info->text().contains(QLatin1String("Pixel")), qPrintable(info->text()));
    // M3c review nit (M3d): the window size is greyed out while borderless fullscreen is on.
    QVERIFY(scale->isEnabled());
    auto* fullscreenBox = w.findChild<KeyEditor*>(QStringLiteral("render.fullscreen"))->findChild<QCheckBox*>();
    QVERIFY(fullscreenBox != nullptr);
    QVERIFY(!fullscreenBox->isChecked());
    fullscreenBox->click();
    QVERIFY(!scale->isEnabled());
    fullscreenBox->click();
    QVERIFY(scale->isEnabled());
    // M3d: frame rate 60 / 120 / display on the graphics tab and what it gives on the selected monitor (the sample
    // TV: 120 Hz).
    const auto hasLabel = [&w](const QString& text) {
        for (const QLabel* l : w.findChildren<QLabel*>()) {
            if (!l->isHidden() && l->text() == text) {
                return true;
            }
        }
        return false;
    };
    auto* rateCombo = w.findChild<KeyEditor*>(QStringLiteral("render.frame_rate"))->findChild<QComboBox*>();
    QVERIFY(rateCombo != nullptr);
    QCOMPARE(rateCombo->count(), 3);
    QCOMPARE(rateCombo->itemText(0), QStringLiteral("60"));  // the labels of the user requirement (review finding 12)
    QCOMPARE(rateCombo->itemText(1), QStringLiteral("120 (interpoliert)"));
    QCOMPARE(rateCombo->itemText(2), QStringLiteral("Monitor (max. 120)"));
    QCOMPARE(rateCombo->currentData().toString(), QStringLiteral("60"));  // unset: the logic rate
    const QString note120 = QStringLiteral("Auf diesem Bildschirm (120 Hz): 120 Bilder pro Sekunde mit Zwischenbildern.");
    QVERIFY(!hasLabel(note120));
    const int display = rateCombo->findData(QStringLiteral("display"));
    QVERIFY(display >= 0);
    rateCombo->setCurrentIndex(display);
    emit rateCombo->activated(display);
    QCOMPARE(ctx.model.value(*ctx.model.schema().find(QStringLiteral("render.frame_rate"))), QStringLiteral("display"));
    QVERIFY(hasLabel(note120));
    // [render] interpolation = off: 120 frames per second without in-between frames (every logic picture twice).
    auto* interpolationBox = w.findChild<KeyEditor*>(QStringLiteral("render.interpolation"))->findChild<QCheckBox*>();
    QVERIFY(interpolationBox != nullptr);
    QVERIFY(interpolationBox->isChecked());
    interpolationBox->click();
    QVERIFY(!hasLabel(note120));
    QVERIFY(hasLabel(QStringLiteral("Auf diesem Bildschirm (120 Hz): 120 Bilder pro Sekunde.")));
    interpolationBox->click();
    QVERIFY(hasLabel(note120));
    // Resolution auto names what the game renders (window or fullscreen), not the monitor's native size.
    auto* res = w.findChild<KeyEditor*>(QStringLiteral("render.resolution"))->findChild<QComboBox*>();
    QVERIFY2(res->itemText(0).startsWith(QLatin1String("Automatisch (Fenster")), qPrintable(res->itemText(0)));
    QVERIFY(ctx.model.setValue(QStringLiteral("render.fullscreen"), QStringLiteral("1")));
    w.findChild<KeyEditor*>(QStringLiteral("render.resolution"))->refreshContext();
    QVERIFY2(res->itemText(0).startsWith(QLatin1String("Automatisch (Vollbild")), qPrintable(res->itemText(0)));
    for (int i = 1; i < res->count(); ++i) {
        const auto size = res->itemData(i).toString().split(QLatin1Char('x'));
        QCOMPARE(size.size(), 2);
        QCOMPARE(size[0].toInt() * 3, size[1].toInt() * 4);  // only 4:3 sizes are offered
    }
    // Game tab: the verdict of --check-iso for the configured image (test double: XDVDFS magic = supported).
    QTRY_VERIFY_WITH_TIMEOUT(
        w.findChild<QLabel*>(QStringLiteral("isoStatusText"))->text().contains(QStringLiteral("unterstützt")), 30000);
}

void UiTest::failureCauseFromStderr() {
    // Review M3c findings 7 and 8: the host prints "futurama: <error>" and then "log file: <path>"; the panel shows
    // the error as the cause, and the log view starts at the beginning of the lines.
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui cause"));
    QVERIFY(ctx.model.setValue(QStringLiteral("paths.iso"), iso_));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    qputenv("FAKE_FUTURAMA_EXIT", "2");
    qputenv("FAKE_FUTURAMA_STDERR",
            "futurama: stage LoadXbe failed (exit code 2): wrong XBE (SHA-1 f443d255eab39584dcd5dcea6f7c057182c4b854, "
            "expected 58f76de5)\nlog file: C:\\Users\\someone with a long name\\AppData\\Local\\FuturamaPC\\logs\\"
            "futurama.log");
    QSignalSpy finished(&w, &MainWindow::gameFinished);
    QTest::mouseClick(w.findChild<QPushButton*>(QStringLiteral("playButton")), Qt::LeftButton);
    QVERIFY(finished.wait(30000));
    qunsetenv("FAKE_FUTURAMA_EXIT");
    qunsetenv("FAKE_FUTURAMA_STDERR");
    QString cause;
    for (QLabel* l : w.findChild<QWidget*>(QStringLiteral("statusPanel"))->findChildren<QLabel*>()) {
        if (l->text().contains(QLatin1String("Ursache"))) {
            cause = l->text();
        }
    }
    QVERIFY2(cause.contains(QLatin1String("stage LoadXbe failed")), qPrintable(cause));
    QVERIFY2(!cause.contains(QLatin1String("log file")), qPrintable(cause));
    QVERIFY2(!cause.contains(QLatin1String("futurama:")), qPrintable(cause));
    auto* lines = w.findChild<QWidget*>(QStringLiteral("statusPanel"))->findChild<QPlainTextEdit*>();
    QVERIFY(lines != nullptr);
    QCOMPARE(lines->horizontalScrollBar()->value(), 0);
    // A failed start offers "Erneut versuchen"; a normal end "Erneut spielen" (finding 17).
    bool retry = false;
    for (QPushButton* b : w.findChild<QWidget*>(QStringLiteral("statusPanel"))->findChildren<QPushButton*>()) {
        retry = retry || b->text() == QLatin1String("Erneut versuchen");
    }
    QVERIFY(retry);
}

void UiTest::unsupportedIsoAsksBeforePlay() {
    // Review M3c finding 9: a known unsupported image (the test double treats "proto" in the name as the NTSC
    // prototype) is not started without asking; the question defaults to "Abbrechen".
    const QString dir = test::MakeTestDir(QStringLiteral("ui proto (Jun 17, 2003)"));
    const QString proto = QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral("Xbox - Futurama proto.bin")));
    QVERIFY(QFile::copy(iso_, proto));
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui proto"));
    QVERIFY(ctx.model.setValue(QStringLiteral("paths.iso"), proto));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QString question, defaultButton;
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &w, [&] {
        // The box runs its own event loop: read it and press Escape (= "Abbrechen").
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box != nullptr) {
            question = box->text();
            defaultButton = box->defaultButton() != nullptr ? box->defaultButton()->text() : QString();
            poll.stop();
            box->reject();
        }
    });
    poll.start();
    QSignalSpy finished(&w, &MainWindow::gameFinished);
    QTest::mouseClick(w.findChild<QPushButton*>(QStringLiteral("playButton")), Qt::LeftButton);
    poll.stop();
    QVERIFY2(question.contains(QLatin1String("startet das Spiel nicht")), qPrintable(question));
    QCOMPARE(defaultButton, QStringLiteral("Abbrechen"));
    QVERIFY(w.isVisible());  // not started
    QVERIFY(!finished.wait(500));
}

void UiTest::quotedPathInput() {
    // Review M3c finding 10: "C:\...\x.iso" with quotes (Explorer's "Als Pfad kopieren") works everywhere.
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui quoted"));
    const QString quoted = QLatin1Char('"') + iso_ + QLatin1Char('"');
    FirstRunWizard wizard(ctx);
    wizard.show();
    QVERIFY(QTest::qWaitForWindowExposed(&wizard));
    wizard.next();
    auto* edit = wizard.findChild<QLineEdit*>(QStringLiteral("isoEdit"));
    edit->setText(quoted);
    emit edit->textEdited(quoted);
    QCOMPARE(wizard.state().iso, iso_);
    QTRY_VERIFY_WITH_TIMEOUT(wizard.currentPage()->isComplete(), 30000);
    // The settings model and the path editor of the paths tab strip them as well.
    const KeyDef* isoKey = ctx.model.schema().find(QStringLiteral("paths.iso"));
    ctx.model.setValue(*isoKey, quoted);
    QCOMPARE(ctx.model.value(*isoKey), iso_);
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* line = w.findChild<KeyEditor*>(QStringLiteral("paths.iso"))->findChild<QLineEdit*>();
    line->setText(quoted);
    emit line->textEdited(quoted);
    QCOMPARE(ctx.model.value(*isoKey), iso_);
    QVERIFY(!ctx.model.validate(*isoKey).isError());
}

// ---- M4: controls tab ------------------------------------------------------------------------------------------
namespace {

// The simulated controller of the window under test (the factory hands one to every new PadPoller).
SimulatedPadReader* g_pad = nullptr;

void InstallSimulatedPads() {
    SetPadReaderFactory([] {
        auto reader = std::make_unique<SimulatedPadReader>();
        g_pad = reader.get();
        return reader;
    });
}

PadState Pressed(quint16 buttons) {
    PadState s;
    s.connected = true;
    s.buttons = buttons;
    return s;
}

QString ValueOf(AppContext& ctx, const char* key) {
    return ctx.model.value(*ctx.model.schema().find(QLatin1String(key)));
}

}  // namespace

void UiTest::controlsBindingEditor() {
    InstallSimulatedPads();
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui controls"));
    {
        MainWindow w(ctx, options_);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.selectTab(UiTab::Controls);
        QCOMPARE(w.tabWidget()->tabText(w.tabWidget()->currentIndex()), QStringLiteral("Steuerung"));
        BindingsPanel* panel = w.bindingsPanel();
        QVERIFY(panel != nullptr && panel->hasBindings());
        QCOMPARE(panel->rowCount(BindDevice::Pad), 24);
        QCOMPARE(panel->rowCount(BindDevice::KeyboardMouse), 24);
        // No generic text editor for binding keys; the remaining [input] keys are ordinary rows (slider, combo).
        QVERIFY(w.findChild<KeyEditor*>(QStringLiteral("input.kbm_a")) == nullptr);
        QVERIFY(w.findChild<KeyEditor*>(QStringLiteral("input.pad_mode")) != nullptr);
        panel->selectDevice(BindDevice::KeyboardMouse);

        auto* button = w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_a"));
        QVERIFY(button != nullptr);
        QCOMPARE(button->text(), QStringLiteral("Eingabe, Linke Maustaste"));

        // Replace: click the binding, press K.
        button->click();
        BindingCaptureDialog* dialog = panel->activeCapture();
        QVERIFY(dialog != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        QCOMPARE(dialog->device(), BindDevice::KeyboardMouse);
        QTest::keyClick(dialog, Qt::Key_K);
        QCOMPARE(ValueOf(ctx, "input.kbm_a"), QStringLiteral("key:k"));
        QTRY_VERIFY(panel->activeCapture() == nullptr);  // closed and deleted
        QCOMPARE(button->text(), QStringLiteral("K"));

        // Add: "+" and the right mouse button inside the capture field -> conflict with X (mouse:right).
        w.findChild<QToolButton*>(QStringLiteral("bindadd:input.kbm_a"))->click();
        dialog = panel->activeCapture();
        QVERIFY(dialog != nullptr);
        auto* field = dialog->findChild<QFrame*>(QStringLiteral("mouseCaptureField"));
        QVERIFY(field != nullptr);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), field->mapToGlobal(QPointF(5, 5)), Qt::RightButton,
                          Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(field, &press);
        QCOMPARE(ValueOf(ctx, "input.kbm_a"), QStringLiteral("key:k, mouse:right"));
        auto* messageA = w.findChild<QLabel*>(QStringLiteral("bindmsg:input.kbm_a"));
        auto* messageX = w.findChild<QLabel*>(QStringLiteral("bindmsg:input.kbm_x"));
        QVERIFY(!messageA->isHidden());
        QVERIFY2(messageA->text().contains(QLatin1String("Rechte Maustaste")) && messageA->text().contains(QLatin1Char('X')),
                 qPrintable(messageA->text()));
        QVERIFY(!messageX->isHidden());
        QVERIFY(w.findChild<QLabel*>(QStringLiteral("bindSummaryKbm"))->text().contains(QLatin1String("1 Eingabe")));
        QTRY_VERIFY(panel->activeCapture() == nullptr);

        // Esc, Tab and Enter are keys to bind, not dialog shortcuts.
        w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_back"))->click();
        QTest::keyClick(panel->activeCapture(), Qt::Key_Tab);
        QCOMPARE(ValueOf(ctx, "input.kbm_back"), QStringLiteral("key:tab"));
        QTRY_VERIFY(panel->activeCapture() == nullptr);
        w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_start"))->click();
        QTest::keyClick(panel->activeCapture(), Qt::Key_Escape);
        QCOMPARE(ValueOf(ctx, "input.kbm_start"), QStringLiteral("key:escape"));
        QTRY_VERIFY(panel->activeCapture() == nullptr);
        QVERIFY(ctx.model.isAtDefault(*ctx.model.schema().find(QStringLiteral("input.kbm_start"))));

        // Mouse wheel and the movement buttons.
        w.findChild<QToolButton*>(QStringLiteral("bindadd:input.kbm_y"))->click();
        dialog = panel->activeCapture();
        QWheelEvent wheel(QPointF(10, 10), dialog->mapToGlobal(QPointF(10, 10)), QPoint(), QPoint(0, -120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(dialog, &wheel);
        QCOMPARE(ValueOf(ctx, "input.kbm_y"), QStringLiteral("key:e, mouse:wheel_down"));
        QTRY_VERIFY(panel->activeCapture() == nullptr);
        w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_rs_up"))->click();
        dialog = panel->activeCapture();
        dialog->findChild<QPushButton*>(QStringLiteral("capture:mouse:move_left"))->click();
        QCOMPARE(ValueOf(ctx, "input.kbm_rs_up"), QStringLiteral("mouse:move_left"));
        QTRY_VERIFY(panel->activeCapture() == nullptr);

        // Cancel keeps the binding; clear = not bound.
        w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_b"))->click();
        dialog = panel->activeCapture();
        dialog->findChild<QPushButton*>(QStringLiteral("captureCancel"))->click();
        QTRY_VERIFY(panel->activeCapture() == nullptr);
        QCOMPARE(ValueOf(ctx, "input.kbm_b"), QStringLiteral("key:space"));
        w.findChild<QToolButton*>(QStringLiteral("bindclear:input.kbm_ls_click"))->click();
        QCOMPARE(ValueOf(ctx, "input.kbm_ls_click"), QString());
        QCOMPARE(w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_ls_click"))->text(),
                 QStringLiteral("– nicht belegt –"));

        // Slider of the mouse sensitivity (step 0.1).
        auto* slider = w.findChild<KeyEditor*>(QStringLiteral("input.mouse_sensitivity"))->findChild<QSlider*>();
        QVERIFY(slider != nullptr);
        slider->setValue(14);  // 0.1 + 14 * 0.1
        QCOMPARE(ValueOf(ctx, "input.mouse_sensitivity"), QStringLiteral("1.5"));

        // Save: the INI carries exactly the binding strings of the contract.
        w.findChild<QPushButton*>(QStringLiteral("saveButton"))->click();
        QVERIFY(!ctx.model.isDirty());
    }
    const QString ini = QString::fromUtf8(test::ReadFile(ctx.paths.iniPath));
    QVERIFY2(ini.contains(QStringLiteral("[input]")), qPrintable(ini));
    QVERIFY2(ini.contains(QRegularExpression(QStringLiteral("\nkbm_a *= key:k, mouse:right\r\n"))), qPrintable(ini));
    QVERIFY2(ini.contains(QRegularExpression(QStringLiteral("\nkbm_back *= key:tab\r\n"))), qPrintable(ini));
    QVERIFY2(ini.contains(QRegularExpression(QStringLiteral("\nkbm_y *= key:e, mouse:wheel_down\r\n"))), qPrintable(ini));
    QVERIFY2(ini.contains(QRegularExpression(QStringLiteral("\nkbm_rs_up *= mouse:move_left\r\n"))), qPrintable(ini));
    QVERIFY2(ini.contains(QRegularExpression(QStringLiteral("\nkbm_ls_click *=[ ]*\r\n"))), qPrintable(ini));
    QVERIFY2(ini.contains(QRegularExpression(QStringLiteral("\nmouse_sensitivity *= 1.5\r\n"))), qPrintable(ini));
    QVERIFY2(!ini.contains(QLatin1String("kbm_start")) && !ini.contains(QLatin1String("kbm_b ")), qPrintable(ini));

    // Read back: a new window shows the saved bindings; the per-device reset restores the defaults.
    QString error;
    QVERIFY(LoadIni(ctx, &error));
    {
        MainWindow w(ctx, options_);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QCOMPARE(w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_a"))->text(),
                 QStringLiteral("K, Rechte Maustaste"));
        QVERIFY(!w.findChild<QToolButton*>(QStringLiteral("bindreset:input.kbm_a"))->isHidden());
        QVERIFY(w.findChild<QToolButton*>(QStringLiteral("bindreset:input.pad_a"))->isHidden());
        // Several rows: the reset asks first (review M4 finding 14); "Abbrechen" keeps everything.
        g_confirmAnswer = false;
        const int asked = g_confirmCount;
        w.findChild<QPushButton*>(QStringLiteral("bindResetKbm"))->click();
        QCOMPARE(g_confirmCount, asked + 1);
        QVERIFY2(g_confirmText.contains(QLatin1String("Tastatur und Maus")), qPrintable(g_confirmText));
        QCOMPARE(ValueOf(ctx, "input.kbm_a"), QStringLiteral("key:k, mouse:right"));
        g_confirmAnswer = true;
        w.findChild<QPushButton*>(QStringLiteral("bindResetKbm"))->click();
        QCOMPARE(ValueOf(ctx, "input.kbm_a"), QStringLiteral("key:enter, mouse:left"));
        QVERIFY(ctx.model.isAtDefault(*ctx.model.schema().find(QStringLiteral("input.kbm_ls_click"))));
        QCOMPARE(ValueOf(ctx, "input.mouse_sensitivity"), QStringLiteral("1.5"));  // not a binding
        w.findChild<QPushButton*>(QStringLiteral("saveButton"))->click();
    }
    const QString reset = QString::fromUtf8(test::ReadFile(ctx.paths.iniPath));
    QVERIFY2(!reset.contains(QLatin1String("kbm_")), qPrintable(reset));
    SetPadReaderFactory(nullptr);
}

void UiTest::controlsPadCaptureAndTest() {
    InstallSimulatedPads();
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui pad"));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(g_pad != nullptr);
    w.selectTab(UiTab::Controls);
    BindingsPanel* panel = w.bindingsPanel();
    PadPoller* poller = w.padPoller();
    QVERIFY(poller->active());  // the visible controller test polls

    // Capture B for the Xbox A control.
    g_pad->setState(0, Pressed(0));
    w.findChild<QPushButton*>(QStringLiteral("bind:input.pad_a"))->click();
    BindingCaptureDialog* dialog = panel->activeCapture();
    QVERIFY(dialog != nullptr);
    QCOMPARE(dialog->device(), BindDevice::Pad);
    poller->pollNow();
    QCOMPARE(ValueOf(ctx, "input.pad_a"), QStringLiteral("pad:a"));  // nothing pressed yet
    g_pad->setState(0, Pressed(0x2000));
    poller->pollNow();
    QCOMPARE(ValueOf(ctx, "input.pad_a"), QStringLiteral("pad:b"));
    QTRY_VERIFY(panel->activeCapture() == nullptr);

    // An input held while the dialog opens counts only after it was released (controller 2, left stick left).
    PadState held = Pressed(0);
    held.lx = -30000;
    g_pad->setState(0, Pressed(0));
    g_pad->setState(1, held);
    poller->pollNow();
    w.findChild<QToolButton*>(QStringLiteral("bindadd:input.pad_x"))->click();
    dialog = panel->activeCapture();
    poller->pollNow();
    poller->pollNow();
    QCOMPARE(ValueOf(ctx, "input.pad_x"), QStringLiteral("pad:x"));
    g_pad->setState(1, Pressed(0));
    poller->pollNow();
    g_pad->setState(1, held);
    poller->pollNow();
    QCOMPARE(ValueOf(ctx, "input.pad_x"), QStringLiteral("pad:x, pad:lx-"));
    QTRY_VERIFY(panel->activeCapture() == nullptr);
    // pad:lx- is also "left stick left": a conflict on the controller table.
    QVERIFY(!w.findChild<QLabel*>(QStringLiteral("bindmsg:input.pad_ls_left"))->isHidden());

    // Live test: controller 1 with B (bound to A now) and RT pressed.
    PadState s = Pressed(0x2000);
    s.rightTrigger = 255;
    g_pad->setState(0, s);
    g_pad->setState(1, PadState{});
    poller->pollNow();
    PadTestPanel* test = w.padTestPanel();
    QVERIFY2(test->pressedText(0).contains(QLatin1String("B")) && test->pressedText(0).contains(QLatin1String("RT")),
             qPrintable(test->pressedText(0)));
    QCOMPARE(test->gameText(0), QStringLiteral("Für das Spiel: A, B, Rechter Trigger"));  // pad:b is on A and B now
    QVERIFY(!w.findChild<QLabel*>(QStringLiteral("bindmsg:input.pad_b"))->isHidden());
    QVERIFY(test->pressedText(1).isEmpty());
    QVERIFY2(w.findChild<QLabel*>(QStringLiteral("padStatus"))->text().contains(QLatin1String("Simulierte Controller")) &&
                 w.findChild<QLabel*>(QStringLiteral("padStatus"))->text().contains(QLatin1String("1 verbunden")),
             qPrintable(w.findChild<QLabel*>(QStringLiteral("padStatus"))->text()));
    // Slots without a controller are one line, not empty frames; the note only appears with a controller.
    QVERIFY(w.findChild<QFrame*>(QStringLiteral("padSlot1"))->isHidden());
    QVERIFY(!w.findChild<QFrame*>(QStringLiteral("padSlot0"))->isHidden());
    QCOMPARE(w.findChild<QLabel*>(QStringLiteral("padAbsent"))->text(), QStringLiteral("Controller 2, 3, 4: nicht verbunden."));
    QVERIFY(!w.findChild<QLabel*>(QStringLiteral("padNote"))->isHidden());
    // "Für das Spiel" like the host: a trigger at 20 % already counts (channel threshold 30/255), a stick inside
    // [input] pad_deadzone does not.
    PadState light = Pressed(0);
    light.leftTrigger = 51;
    light.lx = -10000;  // pad:lx- is bound to X (analog button): 0.31 of the range would press it ...
    g_pad->setState(0, light);
    QVERIFY(ctx.model.setValue(QStringLiteral("input.pad_deadzone"), QStringLiteral("40")));  // ... but 40 % cut it
    test->refreshFromModel();
    poller->pollNow();
    QCOMPARE(test->gameText(0), QStringLiteral("Für das Spiel: Linker Trigger"));
    QVERIFY(test->pressedText(0).contains(QLatin1String("nichts")));  // below the 0.5 display threshold
    g_pad->setState(0, s);

    // Rumble test with the configured strength; off = button disabled.
    auto* rumble = w.findChild<QPushButton*>(QStringLiteral("padRumble0"));
    QVERIFY(rumble->isEnabled());
    QVERIFY(!w.findChild<QPushButton*>(QStringLiteral("padRumble1"))->isEnabled());
    QVERIFY(ctx.model.setValue(QStringLiteral("input.rumble_strength"), QStringLiteral("40")));
    rumble->click();
    QCOMPARE(g_pad->rumble(0).calls, 1);
    QCOMPARE(g_pad->rumble(0).left, 0.4);
    QTRY_COMPARE_WITH_TIMEOUT(g_pad->rumble(0).left, 0.0, 3000);  // stopped after the test pulse
    auto* rumbleCheck = w.findChild<KeyEditor*>(QStringLiteral("input.rumble"))->findChild<QCheckBox*>();
    rumbleCheck->click();
    QCOMPARE(ValueOf(ctx, "input.rumble"), QStringLiteral("0"));
    poller->pollNow();
    QVERIFY(!rumble->isEnabled());
    QVERIFY(!w.findChild<KeyEditor*>(QStringLiteral("input.rumble_strength"))->isEnabled());

    // Leaving the tab stops the polling.
    w.selectTab(UiTab::Game);
    QVERIFY(!poller->active());
    SetPadReaderFactory(nullptr);
}

void UiTest::controlsInvalidBinding() {
    InstallSimulatedPads();
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui bad binding"));
    QVERIFY(test::WriteFile(ctx.paths.iniPath, "[input]\r\nkbm_b = key:space, joystick:1\r\n"));
    QString error;
    QVERIFY(LoadIni(ctx, &error));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* message = w.findChild<QLabel*>(QStringLiteral("bindmsg:input.kbm_b"));
    QVERIFY(!message->isHidden());
    QVERIFY2(message->text().contains(QLatin1String("joystick:1")), qPrintable(message->text()));
    QVERIFY(w.findChild<QLabel*>(QStringLiteral("footerStatus"))->text().contains(QLatin1Char('1')));
    QVERIFY(w.tabWidget()->tabText(3).endsWith(QStringLiteral("⚠")));  // M5: Audio is tab 2
    QVERIFY(!w.saveSettings(false));
    // A new binding fixes it.
    w.bindingsPanel()->applyToken(*ctx.model.schema().find(QStringLiteral("input.kbm_b")), QStringLiteral("key:j"), false);
    QCOMPARE(ValueOf(ctx, "input.kbm_b"), QStringLiteral("key:j"));
    QVERIFY(message->isHidden());
    QVERIFY(w.saveSettings(false));
    QVERIFY(test::ReadFile(ctx.paths.iniPath).contains("kbm_b = key:j"));
    SetPadReaderFactory(nullptr);
}

void UiTest::controlsReviewFixes() {
    InstallSimulatedPads();
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui review m4"));
    QVERIFY(test::WriteFile(ctx.paths.iniPath, "[input]\r\nmouse_sensitivity = 1.25\r\nrumble_strength = 33\r\n"
                                               "pad_deadzone = 60\r\n"));
    QString error;
    QVERIFY(LoadIni(ctx, &error));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.selectTab(UiTab::Controls);
    BindingsPanel* panel = w.bindingsPanel();

    // Finding 9: the slider label shows the configured value; the value stays until the slider is moved.
    auto* sensitivity = w.findChild<KeyEditor*>(QStringLiteral("input.mouse_sensitivity"));
    QCOMPARE(sensitivity->findChild<QLabel*>(QStringLiteral("sliderValue"))->text(), QStringLiteral("1,25×"));
    QCOMPARE(sensitivity->value(), QStringLiteral("1.25"));
    auto* strength = w.findChild<KeyEditor*>(QStringLiteral("input.rumble_strength"));
    QCOMPARE(strength->findChild<QLabel*>(QStringLiteral("sliderValue"))->text(), QStringLiteral("33\u00A0%"));
    auto* deadzone = w.findChild<KeyEditor*>(QStringLiteral("input.pad_deadzone"));
    QVERIFY2(deadzone->findChild<QLabel*>(QStringLiteral("sliderValue"))->text().startsWith(QStringLiteral("60\u00A0%")) &&
                 deadzone->findChild<QLabel*>(QStringLiteral("sliderValue"))->text().contains(QStringLiteral("ungültig")),
             qPrintable(deadzone->findChild<QLabel*>(QStringLiteral("sliderValue"))->text()));
    QCOMPARE(ValueOf(ctx, "input.mouse_sensitivity"), QStringLiteral("1.25"));
    sensitivity->findChild<QSlider*>()->setValue(14);
    QCOMPARE(ValueOf(ctx, "input.mouse_sensitivity"), QStringLiteral("1.5"));
    QCOMPARE(sensitivity->findChild<QLabel*>(QStringLiteral("sliderValue"))->text(), QStringLiteral("1,5×"));

    // Finding 7: right Ctrl from the native message's extended bit (lParam bit 24), not from Qt's scan code.
    panel->selectDevice(BindDevice::KeyboardMouse);
    w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_y"))->click();
    BindingCaptureDialog* dialog = panel->activeCapture();
    QVERIFY(dialog != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(dialog));
    // Finding 14: the replacement says what it overwrites.
    QVERIFY2(dialog->findChild<QLabel*>(QStringLiteral("captureNote"))->text().contains(QLatin1String("Ersetzt")),
             qPrintable(dialog->findChild<QLabel*>(QStringLiteral("captureNote"))->text()));
    MSG msg{};
    msg.message = WM_KEYDOWN;
    msg.wParam = VK_CONTROL;
    msg.lParam = static_cast<LPARAM>((1u << 24) | (0x1Du << 16) | 1u);
    dialog->noteNativeMessage(&msg);
    QKeyEvent rctrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier, 0x1D, VK_CONTROL, 0);
    QApplication::sendEvent(dialog, &rctrl);
    QCOMPARE(ValueOf(ctx, "input.kbm_y"), QStringLiteral("key:rctrl"));
    QTRY_VERIFY(panel->activeCapture() == nullptr);
    // Right Alt (AltGr: Windows sends left Ctrl first, then right Alt with the extended bit).
    w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_x"))->click();
    dialog = panel->activeCapture();
    QVERIFY(QTest::qWaitForWindowExposed(dialog));
    msg.message = WM_SYSKEYDOWN;
    msg.wParam = VK_MENU;
    msg.lParam = static_cast<LPARAM>((1u << 24) | (0x38u << 16) | 1u);
    dialog->noteNativeMessage(&msg);
    QKeyEvent ralt(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier, 0x38, VK_MENU, 0);
    QApplication::sendEvent(dialog, &ralt);
    QCOMPARE(ValueOf(ctx, "input.kbm_x"), QStringLiteral("key:ralt"));
    QTRY_VERIFY(panel->activeCapture() == nullptr);
    // Finding 12: Print Screen arrives only as a release; F12 and Print warn (screenshot keys of the game).
    w.findChild<QToolButton*>(QStringLiteral("bindadd:input.kbm_white"))->click();
    dialog = panel->activeCapture();
    QCOMPARE(dialog->windowTitle(), QStringLiteral("Belegung hinzufügen"));  // finding 15
    QKeyEvent print(QEvent::KeyRelease, Qt::Key_Print, Qt::NoModifier, 0x37, VK_SNAPSHOT, 0);
    QApplication::sendEvent(dialog, &print);
    QVERIFY2(ValueOf(ctx, "input.kbm_white").contains(QLatin1String("key:printscreen")), qPrintable(ValueOf(ctx, "input.kbm_white")));
    QTRY_VERIFY(panel->activeCapture() == nullptr);
    auto* white = w.findChild<QLabel*>(QStringLiteral("bindmsg:input.kbm_white"));
    QVERIFY2(!white->isHidden() && white->text().contains(QLatin1String("Screenshot")), qPrintable(white->text()));
    // Alt + Enter (A is bound to Enter by default) switches fullscreen in the game.
    auto* xmsg = w.findChild<QLabel*>(QStringLiteral("bindmsg:input.kbm_x"));
    QVERIFY2(!xmsg->isHidden() && xmsg->text().contains(QLatin1String("Vollbild")), qPrintable(xmsg->text()));
    // Finding 15: the accessible name has no pending marker.
    QCOMPARE(w.findChild<QPushButton*>(QStringLiteral("bind:input.kbm_x"))->accessibleName(), QStringLiteral("X: Alt Gr"));
    // Finding 14: an essential control without binding warns.
    w.findChild<QToolButton*>(QStringLiteral("bindclear:input.kbm_start"))->click();
    auto* start = w.findChild<QLabel*>(QStringLiteral("bindmsg:input.kbm_start"));
    QVERIFY2(!start->isHidden() && start->text().contains(QLatin1String("wichtige Taste")), qPrintable(start->text()));

    // Finding 8: the controller capture without a controller waits and offers the list.
    panel->selectDevice(BindDevice::Pad);
    g_pad->setState(0, PadState{});
    w.padPoller()->pollNow();
    w.findChild<QPushButton*>(QStringLiteral("bind:input.pad_y"))->click();
    dialog = panel->activeCapture();
    QVERIFY(QTest::qWaitForWindowExposed(dialog));
    QVERIFY(dialog->waitingForPad());
    QVERIFY(!dialog->findChild<QLabel*>(QStringLiteral("capturePadStatus"))->isHidden());
    auto* list = dialog->findChild<QComboBox*>(QStringLiteral("capturePadList"));
    QVERIFY(list != nullptr && list->count() >= 24);
    list->setCurrentIndex(list->findData(QStringLiteral("pad:rb")));
    dialog->findChild<QPushButton*>(QStringLiteral("capturePadListUse"))->click();
    QCOMPARE(ValueOf(ctx, "input.pad_y"), QStringLiteral("pad:rb"));
    QTRY_VERIFY(panel->activeCapture() == nullptr);
    // No controller: one line instead of four frames, no note about the "Für das Spiel" line.
    QVERIFY(w.findChild<QFrame*>(QStringLiteral("padSlot0"))->isHidden());
    QVERIFY(w.findChild<QLabel*>(QStringLiteral("padNote"))->isHidden());
    QVERIFY(w.findChild<QLabel*>(QStringLiteral("padAbsent"))->text().startsWith(QLatin1String("Kein Controller")));

    // Finding 14: the tab's reset asks before resetting several settings.
    g_confirmAnswer = false;
    const int asked = g_confirmCount;
    auto* page = qobject_cast<SettingsTab*>(w.tabWidget()->currentWidget());
    QVERIFY(page != nullptr);
    QPushButton* tabReset = nullptr;
    for (QPushButton* b : page->findChildren<QPushButton*>()) {
        if (b->text() == QStringLiteral("Auf Standard zurücksetzen")) {
            tabReset = b;
        }
    }
    QVERIFY(tabReset != nullptr);
    tabReset->click();
    QCOMPARE(g_confirmCount, asked + 1);
    QCOMPARE(ValueOf(ctx, "input.pad_y"), QStringLiteral("pad:rb"));
    g_confirmAnswer = true;
    tabReset->click();
    QCOMPARE(ValueOf(ctx, "input.pad_y"), QStringLiteral("pad:y"));
    QCOMPARE(ValueOf(ctx, "input.mouse_sensitivity"), QStringLiteral("1.0"));
    SetPadReaderFactory(nullptr);
}

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        const QString windir = qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"));
        qputenv("QT_QPA_FONTDIR", (windir + QStringLiteral("/Fonts")).toLocal8Bit());
    }
    QApplication app(argc, argv);
    // Fusion: the native Windows styles need window handles the offscreen platform does not have.
    QApplication::setStyle(QStringLiteral("Fusion"));
    UiTest test;
    QStringList args = QCoreApplication::arguments();
    QString file;
    if (!args.contains(QStringLiteral("-o"))) {  // see test_main.cpp: Qt Test's stdout logger is silent here
        const QString dir =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("test_results"));
        QDir().mkpath(dir);
        file = QDir(dir).filePath(QStringLiteral("UiTest.txt"));
        args << QStringLiteral("-o") << file + QStringLiteral(",txt");
    }
    const int failed = QTest::qExec(&test, args);
    if (!file.isEmpty()) {
        const QByteArray text = test::ReadFile(file);
        std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stdout);
    }
    std::printf("launcher_ui_tests: %s\n", failed == 0 ? "PASS" : "FAIL");
    return failed == 0 ? 0 : 1;
}

#include "tst_ui.moc"

// M5: the Audio tab - device list from the (simulated) Windows endpoints, an unknown device, the test tone with the
// tab's values (backend replaced, nothing plays), and the warnings without devices.
void UiTest::audioTab() {
    SetLanguage(Language::German);
    AppContext ctx;
    makeContext(ctx, QStringLiteral("ui audio"));
    QString error;
    QVERIFY(ParseAudioDeviceList(R"({"devices":[
        {"id":"{0.0.0.00000000}.{tv}","name":"LG TV","default":true,"channels":2,"rate":48000},
        {"id":"{0.0.0.00000000}.{hp}","name":"Kopfhörer","channels":2,"rate":44100}]})",
                                 &ctx.audio, &error));
    MainWindow w(ctx, options_);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    QCOMPARE(w.tabWidget()->tabText(2), QStringLiteral("Audio"));
    w.selectTab(UiTab::Audio);
    QCOMPARE(w.tabWidget()->currentIndex(), 2);

    auto* device = w.findChild<KeyEditor*>(QStringLiteral("audio.device"));
    QVERIFY(device != nullptr);
    auto* combo = device->findChild<QComboBox*>();
    QCOMPARE(combo->count(), 3);
    QCOMPARE(combo->itemText(0), QStringLiteral("Windows-Standardgerät (LG TV)"));
    QVERIFY(combo->itemText(2).startsWith(QStringLiteral("Kopfhörer (2 Kanäle, 44")));
    auto* info = w.findChild<QLabel*>(QStringLiteral("audioDeviceInfo"));
    QVERIFY2(info->text().contains(QStringLiteral("LG TV")), qPrintable(info->text()));
    QVERIFY(info->text().contains(QStringLiteral("Windows-Standardgerät")));
    combo->setCurrentIndex(2);
    emit combo->activated(2);
    QCOMPARE(ValueOf(ctx, "audio.device"), QStringLiteral("{0.0.0.00000000}.{hp}"));
    QVERIFY2(info->text().startsWith(QStringLiteral("Kopfhörer")), qPrintable(info->text()));
    auto* warning = w.findChild<QLabel*>(QStringLiteral("audioDeviceWarning"));
    QVERIFY(warning->isHidden());

    // An endpoint that is not connected stays selected and is marked.
    QVERIFY(ctx.model.setValue(QStringLiteral("audio.device"), QStringLiteral("{gone}")));
    QVERIFY(ctx.model.setValue(QStringLiteral("audio.speakers"), QStringLiteral("5.1")));
    QVERIFY(ctx.model.setValue(QStringLiteral("audio.master_volume"), QStringLiteral("50")));
    for (SettingsTab* t : w.findChildren<SettingsTab*>()) {
        t->refreshFromModel();
    }
    w.audioPanel()->refresh();
    QCOMPARE(combo->currentText(), QStringLiteral("{gone} (nicht angeschlossen)"));
    QVERIFY(!warning->isHidden());
    QVERIFY(warning->text().contains(QStringLiteral("nicht angeschlossen")));
    // The game falls back to the default device: the line shows that device, the warning says so, and 5.1 on a
    // 2-channel device gets the downmix note.
    QVERIFY2(info->text().startsWith(QStringLiteral("Windows-Standardgerät: LG TV")), qPrintable(info->text()));
    QVERIFY2(warning->text().contains(QStringLiteral("{gone}")) &&
                 warning->text().contains(QStringLiteral("Windows-Standardgerät")),
             qPrintable(warning->text()));
    QVERIFY2(warning->text().contains(QStringLiteral("6 Lautsprecher")) &&
                 warning->text().contains(QStringLiteral("nur 2 Kanäle")),
             qPrintable(warning->text()));

    // Test tone: the request carries the tab's values; the result is shown.
    ToneRequest seen;
    TonePlayer::SetBackendForTesting([&seen](const ToneRequest& r) {
        seen = r;
        ToneResult result;
        result.ok = true;
        result.channels = 6;
        result.sampleRate = 48000;
        result.speakers = {QStringLiteral("links"), QStringLiteral("rechts"), QStringLiteral("alle zusammen")};
        return result;
    });
    auto* button = w.findChild<QPushButton*>(QStringLiteral("audioTestButton"));
    QVERIFY(button->isEnabled());
    QSignalSpy done(w.audioPanel()->player(), &TonePlayer::finished);
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(done.wait(5000));
    QCOMPARE(seen.deviceId, QStringLiteral("{gone}"));
    QCOMPARE(seen.speakers, 6);
    QCOMPARE(seen.volume, 0.5);
    auto* status = w.findChild<QLabel*>(QStringLiteral("audioTestStatus"));
    QVERIFY2(status->text().contains(QStringLiteral("Testton gespielt: links, rechts, alle zusammen (6 Kanäle")),
             qPrintable(status->text()));
    QVERIFY(button->isEnabled());

    // While a tone plays, "search devices again" (it rebuilds the window) is disabled; a fallback is reported.
    std::atomic<bool> release{false};
    TonePlayer::SetBackendForTesting([&release](const ToneRequest&) {
        for (int i = 0; i < 500 && !release.load(); ++i) {
            QThread::msleep(10);
        }
        ToneResult result;
        result.ok = true;
        result.channels = 2;
        result.sampleRate = 48000;
        result.fellBack = true;
        result.speakers = {QStringLiteral("links"), QStringLiteral("rechts"), QStringLiteral("alle zusammen")};
        return result;
    });
    auto* refreshButton = w.findChild<QPushButton*>(QStringLiteral("audioRefreshButton"));
    QVERIFY(refreshButton != nullptr && refreshButton->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(!refreshButton->isEnabled());
    QVERIFY(!button->isEnabled());
    release.store(true);
    QVERIFY(done.wait(5000));
    QVERIFY(refreshButton->isEnabled());
    QVERIFY2(status->text().contains(QStringLiteral("Windows-Standardgerät")), qPrintable(status->text()));

    TonePlayer::SetBackendForTesting([](const ToneRequest&) {
        ToneResult result;
        result.error = T("audio.err.no_device");
        return result;
    });
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(done.wait(5000));
    QVERIFY2(status->text().contains(QStringLiteral("fehlgeschlagen")), qPrintable(status->text()));
    TonePlayer::SetBackendForTesting(nullptr);

    // Saving writes the endpoint ID unchanged.
    combo->setCurrentIndex(1);
    emit combo->activated(1);
    QVERIFY(w.saveSettings(false));
    QVERIFY(test::ReadFile(ctx.paths.iniPath).contains("device = {0.0.0.00000000}.{tv}"));

    // No active device (TV off): warning, test button disabled, "Windows default device" without a name.
    AppContext empty;
    makeContext(empty, QStringLiteral("ui audio none"));
    QVERIFY(ParseAudioDeviceList(R"({"devices":[]})", &empty.audio, &error));
    MainWindow w2(empty, options_);
    w2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w2));
    QVERIFY(!w2.findChild<QPushButton*>(QStringLiteral("audioTestButton"))->isEnabled());
    auto* warning2 = w2.findChild<QLabel*>(QStringLiteral("audioDeviceWarning"));
    QVERIFY2(warning2->text().contains(QStringLiteral("kein aktives Ausgabegerät")), qPrintable(warning2->text()));
    auto* combo2 = w2.findChild<KeyEditor*>(QStringLiteral("audio.device"))->findChild<QComboBox*>();
    QCOMPARE(combo2->count(), 1);
    QCOMPARE(combo2->itemText(0), QStringLiteral("Windows-Standardgerät"));
}
