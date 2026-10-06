#include "tst_paths.h"

#include "core/config_schema.h"
#include "core/game_cli.h"
#include "core/game_paths.h"
#include "core/game_session.h"
#include "core/gpu_info.h"
#include "test_support.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>

using namespace fl;

namespace {

// A fake game directory: futurama.exe (the test double) + futurama_host.dll placeholder, optionally portable.
QString MakeGameDir(const QString& name, bool portable) {
    const QString dir = test::MakeTestDir(name);
    QFile::copy(QStringLiteral(LAUNCHER_FAKE_GAME), QDir(dir).filePath(QStringLiteral("futurama.exe")));
    test::WriteFile(QDir(dir).filePath(QStringLiteral("futurama_host.dll")), QByteArray());
    if (portable) {
        test::WriteFile(QDir(dir).filePath(QStringLiteral("portable.txt")), QByteArray());
    }
    return dir;
}

// An XISO-like file: the XDVDFS magic at 0x10000.
void WriteFakeXiso(const QString& path) {
    QByteArray data(0x10000 + 0x800, '\0');
    const QByteArray magic("MICROSOFT*XBOX*MEDIA");
    data.replace(0x10000, magic.size(), magic);
    test::WriteFile(path, data);
}

}  // namespace

void PathsTest::gamePathsDefault() {
    const QString game = MakeGameDir(QStringLiteral("game default"), false);
    const GamePaths p = ResolveGamePaths(game, QString());
    QVERIFY(p.gameFound());
    QVERIFY(!p.portable);
    QCOMPARE(p.gameExe, QDir::toNativeSeparators(QDir(game).filePath(QStringLiteral("futurama.exe"))));
    QCOMPARE(p.defaultUserdata, LocalAppDataBase());
    QCOMPARE(p.iniPath,
             QDir::toNativeSeparators(QDir(LocalAppDataBase()).filePath(QStringLiteral("futurama.ini"))));
    QCOMPARE(p.configDir, LocalAppDataBase());
    QVERIFY(p.launcherIni().endsWith(QLatin1String("FuturamaPC\\launcher.ini")));
    QVERIFY(p.cacheDir().endsWith(QLatin1String("FuturamaPC\\launcher_cache")));
}

void PathsTest::gamePathsPortable() {
    const QString game = MakeGameDir(QStringLiteral("game portable (1,2)"), true);
    const GamePaths p = ResolveGamePaths(game, QString());
    QVERIFY(p.portable);
    QCOMPARE(p.defaultUserdata, NativeAbsolute(game));
    QCOMPARE(p.iniPath, QDir::toNativeSeparators(QDir(game).filePath(QStringLiteral("futurama.ini"))));
    QCOMPARE(p.configDir, NativeAbsolute(game));
}

void PathsTest::gamePathsExplicitConfig() {
    const QString game = MakeGameDir(QStringLiteral("game explicit"), true);
    const QString cfgDir = test::MakeTestDir(QStringLiteral("cfg (a), b"));
    const QString ini = QDir(cfgDir).filePath(QStringLiteral("my config.ini"));
    const GamePaths p = ResolveGamePaths(game, ini);
    QCOMPARE(p.iniPath, QDir::toNativeSeparators(ini));
    QCOMPARE(p.configDir, cfgDir);
    QCOMPARE(p.defaultUserdata, NativeAbsolute(game));  // portable still decides the user data default
    const GamePaths none = ResolveGamePaths(QString(), ini);
    QVERIFY(!none.gameFound());
    QCOMPARE(none.iniPath, QDir::toNativeSeparators(ini));
}

void PathsTest::findGameDir() {
    const QString game = MakeGameDir(QStringLiteral("game find"), false);
    const QString empty = test::MakeTestDir(QStringLiteral("no game"));
    QCOMPARE(FindGameDir(empty, game, QString()), NativeAbsolute(game));
    QCOMPARE(FindGameDir(QString(), empty, game), NativeAbsolute(game));
    QCOMPARE(FindGameDir(game, empty, empty), NativeAbsolute(game));
    QCOMPARE(FindGameDir(empty, empty, QString()), QString());
}

void PathsTest::userdataAndLog() {
    GamePaths p;
    p.configDir = QStringLiteral("C:\\cfg dir");
    p.defaultUserdata = QStringLiteral("C:\\default");
    QCOMPARE(UserdataDir(QString(), p), QStringLiteral("C:\\default"));
    QCOMPARE(UserdataDir(QStringLiteral("data (1)"), p), QStringLiteral("C:\\cfg dir\\data (1)"));
    QCOMPARE(UserdataDir(QStringLiteral("E:\\x,y"), p), QStringLiteral("E:\\x,y"));
    QCOMPARE(LogFile(QStringLiteral("E:\\x,y")), QStringLiteral("E:\\x,y\\logs\\futurama.log"));
    QCOMPARE(ScreenshotDir(QString(), QStringLiteral("E:\\u"), p.configDir),
             QStringLiteral("E:\\u\\screenshots"));
    QCOMPARE(ScreenshotDir(QStringLiteral("shots"), QStringLiteral("E:\\u"), p.configDir),
             QStringLiteral("C:\\cfg dir\\shots"));
}

void PathsTest::commandLineQuoting() {
    // The exact argument list of a game start and a check-iso call arrives unchanged in the child
    // (CommandLineToArgvW), including blanks, parentheses, commas and a trailing backslash.
    const QString dir = test::MakeTestDir(QStringLiteral("quoting (En,Fr) x"));
    const QString echo = QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral("args (1).json")));
    const QString ini = QStringLiteral("Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\futurama.ini");
    QStringList args = GameSession::Arguments(ini);
    args += GameCli::CheckIsoArguments(QStringLiteral("Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\") +
                                           test::SpecialIsoName(),
                                       QStringLiteral("C:\\out dir\\r.json"));
    args.append(QStringLiteral("--dir=C:\\trailing slash\\"));
    args.append(QStringLiteral("quote\"inside"));
    QProcess p;
    p.start(QStringLiteral(LAUNCHER_FAKE_GAME), QStringList{QStringLiteral("--echo-args=") + echo} + args);
    QVERIFY(p.waitForFinished(30000));
    QCOMPARE(p.exitCode(), 0);
    const QJsonArray received = QJsonDocument::fromJson(test::ReadFile(echo)).array();
    QStringList got;
    for (const QJsonValue& v : received) {
        got.append(v.toString());
    }
    QCOMPARE(got, QStringList{QStringLiteral("--echo-args=") + echo} + args);
    QCOMPARE(args.first(), QStringLiteral("--config=") + ini);
}

void PathsTest::checkIsoThroughCli() {
    const QString game = MakeGameDir(QStringLiteral("game iso"), false);
    const QString images = test::MakeTestDir(QStringLiteral("images (Jun 17, 2003 prototype)"));
    const QString good = QDir::toNativeSeparators(QDir(images).filePath(test::SpecialIsoName()));
    const QString proto =
        QDir::toNativeSeparators(QDir(images).filePath(QStringLiteral("Futurama (USA) (Proto).iso")));
    const QString text = QDir::toNativeSeparators(QDir(images).filePath(QStringLiteral("readme (1).txt")));
    WriteFakeXiso(good);
    WriteFakeXiso(proto);
    test::WriteFile(text, QByteArray("not an image"));
    const GameCli cli(QDir(game).filePath(QStringLiteral("futurama.exe")),
                      QDir(game).filePath(QStringLiteral("cache")));

    const IsoCheckResult ok = GameCli::ToIsoResult(cli.checkIso(good));
    QCOMPARE(ok.verdict(), IsoVerdict::Supported);
    QCOMPARE(ok.exitCode, 0);
    QCOMPARE(ok.layout, QStringLiteral("xiso"));
    QCOMPARE(ok.version, QStringLiteral("retail_eu"));

    const IsoCheckResult old = GameCli::ToIsoResult(cli.checkIso(proto));
    QCOMPARE(old.verdict(), IsoVerdict::UnsupportedVersion);
    QCOMPARE(old.exitCode, 1);

    const IsoCheckResult notXbox = GameCli::ToIsoResult(cli.checkIso(text));
    QCOMPARE(notXbox.verdict(), IsoVerdict::NotXboxImage);
    QVERIFY(!notXbox.reason.isEmpty());

    const IsoCheckResult missing =
        GameCli::ToIsoResult(cli.checkIso(QDir(images).filePath(QStringLiteral("gone.iso"))));
    QCOMPARE(missing.verdict(), IsoVerdict::NotXboxImage);

    const GameCli broken(QDir(game).filePath(QStringLiteral("does-not-exist.exe")));
    const IsoCheckResult failed = GameCli::ToIsoResult(broken.checkIso(good));
    QCOMPARE(failed.verdict(), IsoVerdict::CheckFailed);
    // The temporary result directories are cleaned up.
    QCOMPARE(QDir(QDir(game).filePath(QStringLiteral("cache"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot),
             QStringList());
}

void PathsTest::checkIsoJob() {
    const QString game = MakeGameDir(QStringLiteral("game job"), false);
    const QString iso = QDir::toNativeSeparators(
        QDir(test::MakeTestDir(QStringLiteral("job (x,y)"))).filePath(test::SpecialIsoName()));
    WriteFakeXiso(iso);
    IsoCheckJob job(QDir(game).filePath(QStringLiteral("futurama.exe")), QString());
    QSignalSpy spy(&job, &IsoCheckJob::finished);
    job.start(iso);
    QVERIFY(spy.wait(30000));
    const auto result = spy.first().at(0).value<IsoCheckResult>();
    QCOMPARE(spy.first().at(1).toString(), iso);
    QCOMPARE(result.verdict(), IsoVerdict::Supported);
    QVERIFY(!job.isRunning());
}

void PathsTest::printSchemaAndGpuThroughCli() {
    const QString game = MakeGameDir(QStringLiteral("game schema"), false);
    const GameCli cli(QDir(game).filePath(QStringLiteral("futurama.exe")),
                      QDir(game).filePath(QStringLiteral("cache")));
    const CliResult schema = cli.printConfigSchema();
    QVERIFY2(schema.hasJson(), qPrintable(schema.error));
    QCOMPARE(schema.exitCode, 0);
    ConfigSchema parsed;
    QString error;
    QVERIFY(ParseSchema(schema.json, &parsed, &error));
    const CliResult gpu = cli.queryGpu();
    GpuInfo info;
    QVERIFY(ParseGpuInfo(gpu.json, &info, &error));
    QCOMPARE(info.maxMsaa(), 8);
}
