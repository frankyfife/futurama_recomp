#include "tst_misc.h"

#include "core/cli_cache.h"
#include "core/config_schema.h"
#include "core/exit_codes.h"
#include "core/game_paths.h"
#include "core/game_session.h"
#include "core/gpu_info.h"
#include "core/l10n.h"
#include "core/log_tail.h"
#include "core/presets.h"
#include "core/settings_model.h"
#include "test_support.h"

#include <QDirIterator>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTest>

using namespace fl;

namespace {

ConfigSchema g_schema;
GpuInfo g_gpu;

QSet<QString> Placeholders(const char* text) {
    QSet<QString> out;
    const QRegularExpression re(QStringLiteral("%(\\d)"));
    auto it = re.globalMatch(QString::fromUtf8(text));
    while (it.hasNext()) {
        out.insert(it.next().captured(1));
    }
    return out;
}

SettingsModel MakeModel(const QString& ini) {
    SettingsModel model;
    model.setSchema(g_schema);
    QString error;
    model.load(ini, &error);
    return model;
}

const KeyDef& Key(const SettingsModel& m, const char* name) {
    return *m.schema().find(QLatin1String(name));
}

}  // namespace

void MiscTest::initTestCase() {
    SetLanguage(Language::German);
    QString error;
    QVERIFY(ParseSchema(test::ReadFile(test::TestdataPath("sample_schema.json")), &g_schema, &error));
    QVERIFY(ParseGpuInfo(test::ReadFile(test::TestdataPath("sample_gpu.json")), &g_gpu, &error));
}

void MiscTest::translationsComplete() {
    QSet<QString> ids;
    for (const TranslationEntry& e : TranslationTable()) {
        QVERIFY2(!ids.contains(QLatin1String(e.id)), e.id);
        ids.insert(QLatin1String(e.id));
        QVERIFY2(qstrlen(e.de) > 0 && qstrlen(e.en) > 0, e.id);
        QVERIFY2(Placeholders(e.de) == Placeholders(e.en), e.id);
    }
    // Every T("id") in the sources has a row (ids listed in data tables are named here explicitly).
    QStringList used = {QStringLiteral("paths.config_file"), QStringLiteral("paths.log_dir"),
                        QStringLiteral("paths.saves_dir"), QStringLiteral("paths.game_dir")};
    const QRegularExpression re(QStringLiteral("\\bT\\(\"([a-z][a-z0-9_.]*)\""));
    QDirIterator it(QStringLiteral(LAUNCHER_SOURCE_DIR "/src"),
                    {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files,
                    QDirIterator::Subdirectories);
    int files = 0;
    while (it.hasNext()) {
        const QString text = QString::fromUtf8(test::ReadFile(it.next()));
        ++files;
        auto matches = re.globalMatch(text);
        while (matches.hasNext()) {
            used.append(matches.next().captured(1));
        }
    }
    QVERIFY(files > 20);
    QVERIFY(used.size() > 150);
    for (const QString& id : used) {
        QVERIFY2(ids.contains(id), qPrintable(QStringLiteral("missing translation row: ") + id));
    }
    QCOMPARE(T("tab.graphics", Language::German), QStringLiteral("Grafik"));
    QCOMPARE(T("tab.graphics", Language::English), QStringLiteral("Graphics"));
    QCOMPARE(T("no.such.id"), QStringLiteral("[no.such.id]"));
    QCOMPARE(ParseLanguageCode(QStringLiteral("DE")), std::optional<Language>(Language::German));
    QCOMPARE(ParseLanguageCode(QStringLiteral("english")), std::optional<Language>(Language::English));
    QVERIFY(!ParseLanguageCode(QStringLiteral("fr")).has_value());
}

void MiscTest::presets() {
    const PresetValues classic = ComputePreset(QualityPreset::Classic, g_schema, g_gpu);
    QCOMPARE(classic.value(QLatin1String(kKeyResolution)), QStringLiteral("640x480"));
    QCOMPARE(classic.value(QLatin1String(kKeyRenderScale)), QStringLiteral("1.0"));
    QCOMPARE(classic.value(QLatin1String(kKeyMsaa)), QStringLiteral("off"));
    QCOMPARE(classic.value(QLatin1String(kKeyAnisotropy)), QStringLiteral("off"));
    const PresetValues standard = ComputePreset(QualityPreset::Standard, g_schema, g_gpu);
    QCOMPARE(standard.value(QLatin1String(kKeyResolution)), QStringLiteral("auto"));
    QCOMPARE(standard.value(QLatin1String(kKeyMsaa)), QStringLiteral("4"));
    QCOMPARE(standard.value(QLatin1String(kKeyAnisotropy)), QStringLiteral("16"));
    QCOMPARE(standard.value(QLatin1String(kKeyAlphaToCoverage)), QStringLiteral("1"));
    const PresetValues maximum = ComputePreset(QualityPreset::Maximum, g_schema, g_gpu);
    QCOMPARE(maximum.value(QLatin1String(kKeyMsaa)), QStringLiteral("8"));
    // Review M3c finding 1: render scale 2 (exact 2x2 box) instead of the softer 1.5; gamma-correct resolve.
    QCOMPARE(maximum.value(QLatin1String(kKeyRenderScale)), QStringLiteral("2.0"));
    QCOMPARE(maximum.value(QLatin1String(kKeyLinearAa)), QStringLiteral("1"));
    QVERIFY(!maximum.limitedByMemory);
    QCOMPARE(standard.value(QLatin1String(kKeyLinearAa)), QStringLiteral("0"));
    // Clamped to a weaker GPU.
    GpuInfo weak = g_gpu;
    weak.msaa = {1, 2, 4};
    weak.maxAnisotropy = 8;
    const PresetValues clamped = ComputePreset(QualityPreset::Maximum, g_schema, weak);
    QCOMPARE(clamped.value(QLatin1String(kKeyMsaa)), QStringLiteral("4"));
    QCOMPARE(clamped.value(QLatin1String(kKeyAnisotropy)), QStringLiteral("8"));
    GpuInfo none = g_gpu;
    none.msaa = {1};
    QCOMPARE(ComputePreset(QualityPreset::Standard, g_schema, none).value(QLatin1String(kKeyMsaa)),
             QStringLiteral("off"));
    // Unknown GPU: no limit from the GPU, the game clamps at start.
    QCOMPARE(ComputePreset(QualityPreset::Maximum, g_schema, GpuInfo{}).value(QLatin1String(kKeyMsaa)),
             QStringLiteral("8"));
    // A schema without the M3c keys has no presets.
    ConfigSchema old;
    QString error;
    QVERIFY(ParseSchema(
        R"({"schema_version":1,"sections":[{"name":"render","keys":[{"name":"vsync","type":"bool"}]}]})",
        &old, &error));
    QVERIFY(ComputePreset(QualityPreset::Standard, old, g_gpu).values.isEmpty());
}

void MiscTest::presetDetection() {
    QHash<QString, QString> values;
    auto current = [&values](const KeyDef& k) { return values.value(k.fullName(), k.defaultText); };
    QCOMPARE(DetectPreset(g_schema, g_gpu, current),
             QualityPreset::Custom);  // defaults: msaa off + aniso 16 + auto
    for (const auto& [k, v] : ComputePreset(QualityPreset::Classic, g_schema, g_gpu).values) {
        values.insert(k, v);
    }
    QCOMPARE(DetectPreset(g_schema, g_gpu, current), QualityPreset::Classic);
    values.insert(QStringLiteral("render.msaa"), QStringLiteral("2"));
    QCOMPARE(DetectPreset(g_schema, g_gpu, current), QualityPreset::Custom);
    for (const auto& [k, v] : ComputePreset(QualityPreset::Maximum, g_schema, g_gpu).values) {
        values.insert(k, v);
    }
    values.insert(QStringLiteral("render.render_scale"), QStringLiteral("2.00"));  // another spelling of 2.0
    QCOMPARE(DetectPreset(g_schema, g_gpu, current), QualityPreset::Maximum);
    // The old Maximum (render scale 1.5) is custom now (review M3c finding 1).
    values.insert(QStringLiteral("render.render_scale"), QStringLiteral("1.5"));
    QCOMPARE(DetectPreset(g_schema, g_gpu, current), QualityPreset::Custom);
}

void MiscTest::settingsModelEdits() {
    const QString dir = test::MakeTestDir(QStringLiteral("model (a,b)"));
    const QString ini = QDir(dir).filePath(QStringLiteral("futurama.ini"));
    QVERIFY(test::WriteFile(ini, "[render]\nmsaa = 4 ; mine\n"));
    SettingsModel m = MakeModel(ini);
    const KeyDef& msaa = Key(m, "render.msaa");
    const KeyDef& vsync = Key(m, "render.vsync");
    QCOMPARE(m.value(msaa), QStringLiteral("4"));
    QCOMPARE(m.value(vsync), QStringLiteral("1"));  // default
    QVERIFY(m.isAtDefault(vsync));
    // Setting a key that is absent to its default is no edit.
    m.setValue(vsync, QStringLiteral("on"));
    QVERIFY(!m.isDirty());
    m.setValue(vsync, QStringLiteral("0"));
    QVERIFY(m.isDirty());
    m.setValue(vsync, QStringLiteral("1"));
    QVERIFY(!m.isDirty());
    // Same value in another spelling: no edit.
    m.setValue(msaa, QStringLiteral(" 4"));
    QVERIFY(!m.isDirty());
    m.setValue(msaa, QStringLiteral("8"));
    QCOMPARE(m.value(msaa), QStringLiteral("8"));
    QVERIFY(m.isPending(msaa));
    m.resetToDefault(msaa);
    QCOMPARE(m.value(msaa), QStringLiteral("off"));
    QVERIFY(m.isDirty());
    QString error;
    QVERIFY(m.save(&error));
    QCOMPARE(test::ReadFile(ini), QByteArray("[render]\n"));  // reset removes the key line
    QVERIFY(m.setValue(QStringLiteral("render.anisotropy"), QStringLiteral("off")));
    QVERIFY(!m.setValue(QStringLiteral("render.nonexistent"), QStringLiteral("1")));
    m.setValue(Key(m, "render.resolution"), QStringLiteral("bogus"));
    QCOMPARE(m.keysWithErrors().size(), 1);
    m.resetTab(UiTab::Graphics);
    QVERIFY(m.keysWithErrors().isEmpty());
    QVERIFY(!m.isDirty());
}

void MiscTest::settingsModelDynamicDefault() {
    const QString dir = test::MakeTestDir(QStringLiteral("model dynamic"));
    const QString ini = QDir(dir).filePath(QStringLiteral("futurama.ini"));
    SettingsModel m = MakeModel(ini);
    const KeyDef& console = Key(m, "log.console");  // dynamic default (static "0" in the sample)
    QVERIFY(console.dynamicDefault);
    QVERIFY(m.isAtDefault(console));
    QVERIFY(!m.isExplicit(console));
    // An explicit choice equal to the static default is still written.
    m.setValue(console, QStringLiteral("0"));
    QVERIFY(m.isDirty());
    QVERIFY(m.isExplicit(console));
    QVERIFY(!m.isAtDefault(console));
    QString error;
    QVERIFY(m.save(&error));
    QVERIFY(test::ReadFile(ini).contains("console = 0"));
    m.resetToDefault(console);
    QVERIFY(m.save(&error));
    QVERIFY(!test::ReadFile(ini).contains("console"));
}

void MiscTest::settingsModelSaveMerges() {
    const QString dir = test::MakeTestDir(QStringLiteral("model merge"));
    const QString ini = QDir(dir).filePath(QStringLiteral("new (1).ini"));
    SettingsModel m = MakeModel(ini);
    QVERIFY(!m.fileExists());
    const QString iso =
        QStringLiteral("Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\") + test::SpecialIsoName();
    m.setValue(Key(m, "paths.iso"), iso);
    m.setValue(Key(m, "render.msaa"), QStringLiteral("4"));
    QString error;
    QVERIFY2(m.save(&error, {QStringLiteral("header line")}), qPrintable(error));
    QCOMPARE(test::ReadFile(ini), QByteArray("; header line\r\n\r\n[paths]\r\niso = ") + iso.toUtf8() +
                                      QByteArray("\r\n\r\n[render]\r\nmsaa = 4\r\n"));
    // Someone edits the file meanwhile; the next save keeps that edit.
    QVERIFY(test::WriteFile(ini, test::ReadFile(ini) + "vsync = 0 ; edited by hand\r\n"));
    m.setValue(Key(m, "render.msaa"), QStringLiteral("8"));
    QVERIFY(m.save(&error));
    const QByteArray after = test::ReadFile(ini);
    QVERIFY(after.contains("msaa = 8\r\n"));
    QVERIFY(after.contains("vsync = 0 ; edited by hand\r\n"));
    QCOMPARE(m.value(Key(m, "render.vsync")), QStringLiteral("0"));
    QVERIFY(after.startsWith("; header line\r\n"));
    QCOMPARE(after.count("header line"), 1);
}

void MiscTest::settingsModelUnknownEntries() {
    const QString dir = test::MakeTestDir(QStringLiteral("model unknown"));
    const QString ini = QDir(dir).filePath(QStringLiteral("futurama.ini"));
    QVERIFY(test::WriteFile(ini, "msaa = 2\n[render]\nfuture = 1\nmsaa = 4\n[mods]\nx = y\n"));
    SettingsModel m = MakeModel(ini);
    const QList<UnknownEntry> unknown = m.unknownEntries();
    QCOMPARE(unknown.size(), 2);
    QCOMPARE(unknown[0].key, QStringLiteral("future"));
    QVERIFY(!unknown[0].unknownSection);
    QCOMPARE(unknown[1].section, QStringLiteral("mods"));
    QVERIFY(unknown[1].unknownSection);
    QVERIFY(m.removeUnknown(unknown[0]));
    QCOMPARE(m.unknownEntries().size(), 1);
    QString error;
    QVERIFY(m.save(&error));
    QCOMPARE(test::ReadFile(ini), QByteArray("msaa = 2\n[render]\nmsaa = 4\n[mods]\nx = y\n"));
}

void MiscTest::exitCodes() {
    QCOMPARE(DescribeExit(0, false).category, ExitCategory::Ok);
    QCOMPARE(DescribeExit(0, false).tone, ExitTone::Good);
    QCOMPARE(DescribeExit(1, false).category, ExitCategory::ConfigError);
    QCOMPARE(DescribeExit(2, false).category, ExitCategory::StageFailed);
    QCOMPARE(DescribeExit(3, false).category, ExitCategory::Fatal);
    QCOMPARE(DescribeExit(4, false).category, ExitCategory::GameEnded);
    QCOMPARE(DescribeExit(4, false).tone, ExitTone::Neutral);
    QCOMPARE(DescribeExit(5, false).category, ExitCategory::Watchdog);
    QCOMPARE(DescribeExit(12, false).category, ExitCategory::Starter);
    QCOMPARE(DescribeExit(static_cast<int>(0xC0000005u), true).category, ExitCategory::Crash);
    QVERIFY(DescribeExit(static_cast<int>(0xC0000005u), true).title.contains(QLatin1String("0xC0000005")));
    QCOMPARE(DescribeExit(42, false).category, ExitCategory::Unknown);
    QCOMPARE(DescribeExit(-1, false, false).category, ExitCategory::NotStarted);
    QCOMPARE(FormatExitCode(3), QStringLiteral("3"));
    QCOMPARE(ExitCategoryName(ExitCategory::Watchdog), QStringLiteral("watchdog"));
    // Review M3c finding 15: another game with the same user data.
    QCOMPARE(DescribeExit(kGameExitAlreadyRunning, false).category, ExitCategory::AlreadyRunning);
    QCOMPARE(DescribeExit(kGameExitAlreadyRunning, false).tone, ExitTone::Neutral);
    QCOMPARE(ExitCategoryName(ExitCategory::AlreadyRunning), QStringLiteral("already_running"));
    QVERIFY(T("status.exit_code", Language::German).contains(QStringLiteral("Rückgabecode")));
}

void MiscTest::failureCause() {
    // Review M3c finding 7: the host appends "log file: <path>" (or "no log file") to its error line.
    GameRunResult r;
    r.started = true;
    r.exitCode = 2;
    r.outputTail = {QStringLiteral("futurama: stage LoadXbe failed (exit code 2): wrong XBE (SHA-1 f443d255)"),
                    QStringLiteral("log file: C:\\Users\\x\\AppData\\Local\\FuturamaPC\\logs\\futurama.log"),
                    QString()};
    QCOMPARE(r.cause(), QStringLiteral("stage LoadXbe failed (exit code 2): wrong XBE (SHA-1 f443d255)"));
    r.exitCode = 3;
    r.outputTail = {QStringLiteral("futurama: FATAL: Direct3D 11 could not be started (x)"),
                    QStringLiteral("log file: (none)")};
    QCOMPARE(r.cause(), QStringLiteral("Direct3D 11 could not be started (x)"));
    r.exitCode = 1;
    r.outputTail = {QStringLiteral("futurama: configuration error: unknown config key 'x' in [render]"),
                    QStringLiteral("no log file")};
    QCOMPARE(r.cause(), QStringLiteral("configuration error: unknown config key 'x' in [render]"));
    r.outputTail = {QStringLiteral("some other line"), QStringLiteral("log file: y")};
    QCOMPARE(r.cause(), QStringLiteral("some other line"));
    r.exitCode = 0;  // a normal end has no cause
    QVERIFY(r.cause().isEmpty());
    r.startError = QStringLiteral("could not start");
    QCOMPARE(r.cause(), QStringLiteral("could not start"));
    QCOMPARE(r.toJson().value(QStringLiteral("cause")).toString(), QStringLiteral("could not start"));
}

void MiscTest::presetMemory() {
    // Review M3c finding 21: back-buffer memory of the game (GFX_CONTRACT.md 12.3, "render:" log line 2396 MiB).
    QVERIFY(qAbs(BackBufferMemoryMb(QSize(2880, 2160), 2.0, 8) - 2396) <= 2);
    QVERIFY(qAbs(BackBufferMemoryMb(QSize(2880, 2160), 1.0, 4) - 308) <= 2);  // "back buffers 308 MiB"
    qint64 largest = 0;
    BackBufferMemoryMb(QSize(2880, 2160), 2.0, 8, &largest);
    QCOMPARE(largest, 760);
    QVERIFY(FitsVideoMemory(QSize(2880, 2160), 2.0, 8, 32186));
    QVERIFY(!FitsVideoMemory(QSize(2880, 2160), 2.0, 8, 4096));  // 2.4 GB > VRAM/2
    QVERIFY(!FitsVideoMemory(QSize(2880, 2160), 2.0, 8, 3000));  // 760 MB > VRAM/4
    QVERIFY(!FitsVideoMemory(QSize(2880, 2160), 1.0, 4, 0));     // unknown memory
    QCOMPARE(PresetOutputSize(g_gpu), QSize(2880, 2160));
    QCOMPARE(PresetOutputSize(GpuInfo{}), QSize(2880, 2160));
    // A 2 GB card at 4K: 2x/8x and 2x/4x do not fit -> native MSAA 8x.
    GpuInfo small = g_gpu;
    small.adapters.first().dedicatedMb = 2048;
    const PresetValues limited = ComputePreset(QualityPreset::Maximum, g_schema, small);
    QCOMPARE(limited.value(QLatin1String(kKeyRenderScale)), QStringLiteral("1.0"));
    QCOMPARE(limited.value(QLatin1String(kKeyMsaa)), QStringLiteral("8"));
    QVERIFY(limited.limitedByMemory);
    // 4 GB: 2x with 4x MSAA (16 samples per pixel) still fits.
    small.adapters.first().dedicatedMb = 4096;
    const PresetValues mid = ComputePreset(QualityPreset::Maximum, g_schema, small);
    QCOMPARE(mid.value(QLatin1String(kKeyRenderScale)), QStringLiteral("2.0"));
    QCOMPARE(mid.value(QLatin1String(kKeyMsaa)), QStringLiteral("4"));
    // Standard fits everywhere but a tiny card.
    small.adapters.first().dedicatedMb = 256;
    QCOMPARE(ComputePreset(QualityPreset::Standard, g_schema, small).value(QLatin1String(kKeyMsaa)),
             QStringLiteral("off"));
    // Unknown memory: no supersampling for Maximum.
    GpuInfo unknown = g_gpu;
    unknown.adapters.clear();
    QCOMPARE(ComputePreset(QualityPreset::Maximum, g_schema, unknown).value(QLatin1String(kKeyRenderScale)),
             QStringLiteral("1.0"));
}

void MiscTest::logTail() {
    const QString dir = test::MakeTestDir(QStringLiteral("log tail"));
    const QString log = QDir(dir).filePath(QStringLiteral("futurama.log"));
    QByteArray data;
    for (int i = 1; i <= 100; ++i) {
        data += "line " + QByteArray::number(i) + "\r\n";
    }
    QVERIFY(test::WriteFile(log, data));
    QStringList last = ReadLastLines(log, 30);
    QCOMPARE(last.size(), 30);
    QCOMPARE(last.first(), QStringLiteral("line 71"));
    QCOMPARE(last.last(), QStringLiteral("line 100"));
    last = ReadLastLines(log, 5, 40);  // only the last 40 bytes: the cut first line is dropped
    QCOMPARE(last.last(), QStringLiteral("line 100"));
    QVERIFY(!last.first().startsWith(QLatin1String("ne")));
    QVERIFY(ReadLastLines(QDir(dir).filePath(QStringLiteral("missing.log")), 5).isEmpty());
    LineRing ring(3);
    ring.append("a\r\nb\n");
    ring.append("c\nd\npar");
    QCOMPARE(ring.lines(), (QStringList{QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d"),
                                        QStringLiteral("par")}));
    ring.append("tial\n");
    QCOMPARE(ring.lines().last(), QStringLiteral("partial"));
}

void MiscTest::cliCache() {
    const QString dir = test::MakeTestDir(QStringLiteral("cache"));
    const QString exe = QDir(dir).filePath(QStringLiteral("futurama.exe"));
    const QString dll = QDir(dir).filePath(QStringLiteral("futurama_host.dll"));
    QVERIFY(test::WriteFile(exe, "exe"));
    QVERIFY(test::WriteFile(dll, "dll"));
    const CliCache cache(QDir(dir).filePath(QStringLiteral("launcher_cache")));
    const QString fp = CliCache::Fingerprint(exe, dll);
    QVERIFY(!cache.loadSchema(fp).has_value());
    QVERIFY(cache.storeSchema(fp, QStringLiteral("build 1/2 \"x\""), "{\"schema_version\":1}"));
    QCOMPARE(cache.loadSchema(fp).value_or(QByteArray()), QByteArray("{\"schema_version\":1}"));
    QCOMPARE(cache.buildFor(fp).value_or(QString()), QStringLiteral("build 1/2 \"x\""));
    QVERIFY(
        cache.storeGpu(QStringLiteral("build 1/2 \"x\""), QStringLiteral("screens A"), "{\"msaa\":[1,2]}"));
    QVERIFY(cache.loadGpu(QStringLiteral("build 1/2 \"x\""), QStringLiteral("screens A")).has_value());
    QVERIFY(!cache.loadGpu(QStringLiteral("build 1/2 \"x\""), QStringLiteral("screens B")).has_value());
    // A rebuilt exe has another fingerprint.
    QTest::qWait(20);
    QVERIFY(test::WriteFile(exe, "exe v2"));
    QVERIFY(CliCache::Fingerprint(exe, dll) != fp);
    QCOMPARE(CliCache::SafeBuildName(QStringLiteral("a b/c")), QStringLiteral("a_b_c"));
    QVERIFY(CliCache::SafeBuildName(QString(200, QLatin1Char('x'))).size() <= 64);
    cache.clear();
    QVERIFY(!cache.loadSchema(fp).has_value());
}

void MiscTest::gameSessionRun() {
    const QString dir = test::MakeTestDir(QStringLiteral("session (Jun 17, 2003)"));
    const QString userdata = QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral("user data")));
    const QString ini = QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral("futurama (1).ini")));
    QVERIFY(test::WriteFile(ini, "[paths]\nuserdata = " + userdata.toUtf8() + "\n[render]\nmsaa = 4\n"));
    qputenv("FAKE_FUTURAMA_EXIT", "3");
    qputenv("FAKE_FUTURAMA_STDERR", "fatal: sample error");
    GameSession session;
    QVERIFY(session.start(QStringLiteral(LAUNCHER_FAKE_GAME), ini, LogFile(userdata)));
    QVERIFY(session.isRunning());
    const GameRunResult r = session.waitForFinished();
    qunsetenv("FAKE_FUTURAMA_EXIT");
    qunsetenv("FAKE_FUTURAMA_STDERR");
    QVERIFY(r.started);
    QCOMPARE(r.exitCode, 3);
    QCOMPARE(r.describe().category, ExitCategory::Fatal);
    QVERIFY(r.logFromThisRun);
    QCOMPARE(r.logTail.size(), kLogTailLines);
    QVERIFY(r.logTail.last().contains(QLatin1String("process exit: code 3")));
    QVERIFY(r.outputTail.contains(QStringLiteral("fatal: sample error")));
    QCOMPARE(r.arguments, QStringList{QStringLiteral("--config=") + ini});
    const QJsonObject json = r.toJson();
    QCOMPARE(json.value(QLatin1String("exit_code")).toInt(), 3);
    QCOMPARE(json.value(QLatin1String("category")).toString(), QStringLiteral("fatal"));
    QCOMPARE(json.value(QLatin1String("log_tail")).toArray().size(), kLogTailLines);
}

void MiscTest::gameSessionConfigError() {
    const QString dir = test::MakeTestDir(QStringLiteral("session missing"));
    GameSession session;
    session.start(QStringLiteral(LAUNCHER_FAKE_GAME), QDir(dir).filePath(QStringLiteral("absent.ini")),
                  QDir(dir).filePath(QStringLiteral("logs/futurama.log")));
    const GameRunResult r = session.waitForFinished();
    QCOMPARE(r.exitCode, 1);
    QCOMPARE(r.describe().category, ExitCategory::ConfigError);
    QVERIFY(!r.logFromThisRun);
    QVERIFY(!r.outputTail.isEmpty());
    QVERIFY(r.outputTail.first().startsWith(QLatin1String("config file not found")));
    GameSession missing;
    QVERIFY(
        !missing.start(QDir(dir).filePath(QStringLiteral("nope.exe")), QStringLiteral("x.ini"), QString()));
    QCOMPARE(missing.result().describe().category, ExitCategory::NotStarted);
}
