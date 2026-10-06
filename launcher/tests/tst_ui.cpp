// Widget tests of the launcher (offscreen platform): editing + saving in the main window, presets, the
// language switch, a full game run with the test double (exit code 3 -> result panel) and the wizard.
#include "core/l10n.h"
#include "test_support.h"
#include "ui/app_context.h"
#include "ui/key_editors.h"
#include "ui/main_window.h"
#include "ui/screen_info.h"
#include "ui/wizard.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTest>

using namespace fl;
using namespace fl::ui;

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

private:
    void makeContext(AppContext& ctx, const QString& name);
    QString iso_;
    QString gameDir_;
    StartupOptions options_;
};

void UiTest::initTestCase() {
    SetLanguage(Language::German);
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
