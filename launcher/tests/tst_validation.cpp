#include "tst_validation.h"

#include "core/config_schema.h"
#include "core/l10n.h"
#include "core/validation.h"
#include "test_support.h"

#include <QTest>

using namespace fl;

namespace {

ConfigSchema g_schema;

const KeyDef& Key(const char* name) {
    const KeyDef* key = g_schema.find(QLatin1String(name));
    Q_ASSERT(key != nullptr);
    return *key;
}

IssueCode Code(const char* key, const QString& value, const QString& base = QString()) {
    return ValidateValue(Key(key), value, base).code;
}

}  // namespace

void ValidationTest::initTestCase() {
    SetLanguage(Language::English);
    QString error;
    QVERIFY(ParseSchema(test::ReadFile(test::TestdataPath("sample_schema.json")), &g_schema, &error));
}

void ValidationTest::choices() {
    QCOMPARE(Code("render.msaa", QStringLiteral("4")), IssueCode::None);
    QCOMPARE(Code("render.msaa", QStringLiteral("OFF")), IssueCode::None);
    QCOMPARE(Code("render.msaa", QStringLiteral(" 8 ")), IssueCode::None);
    QCOMPARE(Code("render.msaa", QStringLiteral("16")), IssueCode::NotAChoice);
    QCOMPARE(Code("render.msaa", QString()), IssueCode::NotAChoice);
    // Numeric range "1..9" like the host's MatchesChoice.
    QCOMPARE(Code("settings.language", QStringLiteral("german")), IssueCode::None);
    QCOMPARE(Code("settings.language", QStringLiteral("3")), IssueCode::None);
    QCOMPARE(Code("settings.language", QStringLiteral("10")), IssueCode::NotAChoice);
    QCOMPARE(Code("settings.language", QStringLiteral("klingon")), IssueCode::NotAChoice);
    QCOMPARE(Code("settings.language", QStringLiteral("Japanese")), IssueCode::None);  // hidden choice
    QCOMPARE(Code("log.level", QStringLiteral("warning")), IssueCode::None);
    const ValidationResult r = ValidateValue(Key("render.msaa"), QStringLiteral("16"), QString());
    QVERIFY(r.isError());
    QVERIFY(r.message.contains(QLatin1String("off, 2, 4, 8")));
}

void ValidationTest::booleans() {
    for (const char* ok : {"1", "0", "true", "FALSE", "yes", "No", "on", "off"}) {
        QCOMPARE(Code("render.vsync", QLatin1String(ok)), IssueCode::None);
    }
    QCOMPARE(Code("render.vsync", QStringLiteral("2")), IssueCode::NotABool);
    QCOMPARE(Code("render.vsync", QStringLiteral("enabled")), IssueCode::NotABool);
    QCOMPARE(ParseBoolText(QStringLiteral("  On ")), std::optional<bool>(true));
}

void ValidationTest::integers() {
    QCOMPARE(Code("render.window_scale", QStringLiteral("3")), IssueCode::None);
    QCOMPARE(Code("render.window_scale", QStringLiteral("0")), IssueCode::BelowMin);
    QCOMPARE(Code("render.window_scale", QStringLiteral("9")), IssueCode::AboveMax);
    // "range_enforced": false -> the host accepts it, the launcher only warns.
    QCOMPARE(ValidateValue(Key("render.window_scale"), QStringLiteral("9"), QString()).severity,
             Severity::Warning);
    QCOMPARE(ValidateValue(Key("kernel.fiber_stack"), QStringLiteral("0x1000"), QString()).severity,
             Severity::Warning);
    QCOMPARE(ValidateValue(Key("kernel.fiber_stack"), QStringLiteral("100000"), QString()).code,
             IssueCode::OffStep);
    QCOMPARE(Code("render.window_scale", QStringLiteral("2.5")), IssueCode::NotAnInteger);
    QCOMPARE(Code("render.window_scale", QStringLiteral("2x")), IssueCode::NotAnInteger);
    QCOMPARE(Code("kernel.fiber_stack", QStringLiteral("0x80000")), IssueCode::None);
    QCOMPARE(Code("kernel.fiber_stack", QStringLiteral("0x1000")), IssueCode::BelowMin);
    QCOMPARE(Code("settings.game_region", QStringLiteral("4294967295")), IssueCode::None);
    QCOMPARE(Code("settings.game_region", QStringLiteral("4294967296")), IssueCode::AboveMax);
    QCOMPARE(ParseIntText(QStringLiteral("0x10")), std::optional<qint64>(16));
    QCOMPARE(ParseIntText(QStringLiteral("")), std::optional<qint64>());
}

void ValidationTest::floats() {
    QCOMPARE(Code("render.render_scale", QStringLiteral("1.5")), IssueCode::None);
    QCOMPARE(Code("render.render_scale", QStringLiteral("2")), IssueCode::None);
    QCOMPARE(Code("render.render_scale", QStringLiteral("1.25")), IssueCode::None);
    QCOMPARE(Code("render.render_scale", QStringLiteral("1.3")), IssueCode::OffStep);
    QCOMPARE(ValidateValue(Key("render.render_scale"), QStringLiteral("1.3"), QString()).severity,
             Severity::Warning);
    QCOMPARE(Code("render.render_scale", QStringLiteral("0.5")), IssueCode::BelowMin);
    QCOMPARE(ValidateValue(Key("render.render_scale"), QStringLiteral("0.5"), QString()).severity,
             Severity::Error);
    QCOMPARE(Code("render.render_scale", QStringLiteral("2.25")), IssueCode::AboveMax);
    QCOMPARE(Code("render.render_scale", QStringLiteral("1,5")),
             IssueCode::NotANumber);  // German decimal comma
    QCOMPARE(Code("render.render_scale", QStringLiteral("nan")), IssueCode::NotANumber);
}

void ValidationTest::resolutions() {
    QCOMPARE(Code("render.resolution", QStringLiteral("auto")), IssueCode::None);
    QCOMPARE(Code("render.resolution", QStringLiteral("AUTO")), IssueCode::None);
    QCOMPARE(Code("render.resolution", QStringLiteral("640x480")), IssueCode::None);
    QCOMPARE(Code("render.resolution", QStringLiteral("3840X2160")), IssueCode::None);
    QCOMPARE(Code("render.resolution", QStringLiteral("1920 x 1080")),
             IssueCode::None);  // host trims the numbers
    QCOMPARE(Code("render.resolution", QStringLiteral("1920x10 80")), IssueCode::BadResolution);
    QCOMPARE(Code("render.resolution", QStringLiteral("160x160")), IssueCode::None);  // schema min 160
    QCOMPARE(Code("render.resolution", QStringLiteral("160x120")), IssueCode::ResolutionOutOfRange);
    QCOMPARE(Code("render.resolution", QStringLiteral("1920x")), IssueCode::BadResolution);
    QCOMPARE(Code("render.resolution", QStringLiteral("native")), IssueCode::BadResolution);
    QCOMPARE(Code("render.resolution", QStringLiteral("-640x480")), IssueCode::BadResolution);
    QCOMPARE(Code("render.resolution", QStringLiteral("100x100")), IssueCode::ResolutionOutOfRange);
    QCOMPARE(Code("render.resolution", QStringLiteral("20000x480")), IssueCode::ResolutionOutOfRange);
    QCOMPARE(ParseResolutionText(QStringLiteral("1280x960")), std::optional<QSize>(QSize(1280, 960)));
    QCOMPARE(FormatResolution(QSize(640, 480)), QStringLiteral("640x480"));
}

void ValidationTest::paths() {
    const QString dir = test::MakeTestDir(QStringLiteral("validation (1,2) x"));
    const QString iso = QDir(dir).filePath(test::SpecialIsoName());
    QVERIFY(test::WriteFile(iso, QByteArray("x")));
    QCOMPARE(Code("paths.iso", QDir::toNativeSeparators(iso)), IssueCode::None);
    QCOMPARE(Code("paths.iso", test::SpecialIsoName(), dir),
             IssueCode::None);  // relative to the INI directory
    QCOMPARE(Code("paths.iso", QStringLiteral("missing (1).iso"), dir), IssueCode::FileMissing);
    QCOMPARE(ValidateValue(Key("paths.iso"), QStringLiteral("missing.iso"), dir).severity, Severity::Warning);
    QCOMPARE(Code("paths.iso", dir), IssueCode::NotAFile);
    QCOMPARE(Code("paths.iso", QString()), IssueCode::None);
    QCOMPARE(Code("paths.userdata", dir), IssueCode::None);
    QCOMPARE(Code("paths.userdata", QDir::toNativeSeparators(iso)), IssueCode::NotADirectory);
    QCOMPARE(Code("paths.userdata", QStringLiteral("does not exist yet")), IssueCode::DirMissing);
    QCOMPARE(Code("paths.iso", QStringLiteral("a\nb")), IssueCode::NotRepresentable);
    // Review M3c finding 10: paths pasted with quotes ("Als Pfad kopieren").
    const QString quoted = QLatin1Char('"') + QDir::toNativeSeparators(iso) + QLatin1Char('"');
    QCOMPARE(CleanPathInput(quoted), QDir::toNativeSeparators(iso));
    QCOMPARE(CleanPathInput(QStringLiteral("  \"C:\\x y\\a.iso\"  ")), QStringLiteral("C:\\x y\\a.iso"));
    QCOMPARE(CleanPathInput(QStringLiteral("C:\\plain.iso")), QStringLiteral("C:\\plain.iso"));
    QCOMPARE(CleanPathInput(QStringLiteral("\"")), QStringLiteral("\""));  // a lone quote is not a pair
    QCOMPARE(Code("paths.iso", quoted), IssueCode::QuotedPath);
    QCOMPARE(ValidateValue(Key("paths.iso"), quoted, dir).severity, Severity::Error);
    QCOMPARE(ResolvePath(QStringLiteral("sub/x (1).iso"), QStringLiteral("C:\\base dir")),
             QStringLiteral("C:\\base dir\\sub\\x (1).iso"));
    QCOMPARE(ResolvePath(QStringLiteral("D:\\abs\\y.iso"), QStringLiteral("C:\\base")),
             QStringLiteral("D:\\abs\\y.iso"));
    QCOMPARE(ResolvePath(QStringLiteral("..\\up.iso"), QStringLiteral("C:\\a\\b")),
             QStringLiteral("C:\\a\\up.iso"));
    QCOMPARE(ResolvePath(QString(), QStringLiteral("C:\\a")), QString());
}

void ValidationTest::equivalence() {
    QVERIFY(ValuesEquivalent(Key("render.vsync"), QStringLiteral("1"), QStringLiteral("on")));
    QVERIFY(!ValuesEquivalent(Key("render.vsync"), QStringLiteral("1"), QStringLiteral("0")));
    QVERIFY(ValuesEquivalent(Key("kernel.fiber_stack"), QStringLiteral("0x80000"), QStringLiteral("524288")));
    QVERIFY(ValuesEquivalent(Key("render.render_scale"), QStringLiteral("1"), QStringLiteral("1.0")));
    QVERIFY(ValuesEquivalent(Key("render.msaa"), QStringLiteral("Off"), QStringLiteral("off")));
    QVERIFY(ValuesEquivalent(Key("render.resolution"), QStringLiteral("AUTO"), QStringLiteral("auto")));
    QVERIFY(
        ValuesEquivalent(Key("paths.iso"), QStringLiteral("C:/A b/x.iso"), QStringLiteral("c:\\a b\\X.ISO")));
    QVERIFY(!ValuesEquivalent(Key("paths.iso"), QString(), QStringLiteral("x.iso")));
    QVERIFY(!ValuesEquivalent(Key("debug.stop_at"), QStringLiteral("Main"), QStringLiteral("main")));
}

void ValidationTest::boolStyle() {
    QCOMPARE(BoolText(true), QStringLiteral("1"));
    QCOMPARE(BoolText(false, QStringLiteral("On")), QStringLiteral("off"));
    QCOMPARE(BoolText(true, QStringLiteral("false")), QStringLiteral("true"));
    QCOMPARE(BoolText(false, QStringLiteral("yes")), QStringLiteral("no"));
    QCOMPARE(BoolText(true, QStringLiteral("garbage")), QStringLiteral("1"));
}
