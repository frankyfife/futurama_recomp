#include "tst_schema.h"

#include "core/config_schema.h"
#include "core/gpu_info.h"
#include "core/iso_check.h"
#include "test_support.h"

#include <QTest>

using namespace fl;

namespace {

ConfigSchema LoadSample() {
    ConfigSchema schema;
    QString error;
    QStringList warnings;
    const bool ok =
        ParseSchema(test::ReadFile(test::TestdataPath("sample_schema.json")), &schema, &error, &warnings);
    if (!ok || !warnings.isEmpty()) {
        qWarning("sample schema: %s %s", qPrintable(error), qPrintable(warnings.join(QStringLiteral("; "))));
    }
    return schema;
}

}  // namespace

void SchemaTest::sampleSchema() {
    const ConfigSchema schema = LoadSample();
    QCOMPARE(schema.version, 1);
    QVERIFY(schema.build.startsWith(QLatin1String("sample-schema")));
    QVERIFY(schema.allKeys().size() > 40);
    const KeyDef* msaa = schema.find(QStringLiteral("render.msaa"));
    QVERIFY(msaa != nullptr);
    QCOMPARE(msaa->type, KeyType::Choice);
    QCOMPARE(msaa->choices.size(), 4);
    QCOMPARE(msaa->choices[2].value, QStringLiteral("4"));
    QCOMPARE(msaa->choices[2].label.de, QStringLiteral("4× MSAA"));
    QCOMPARE(msaa->widget, UiWidget::Combo);
    QCOMPARE(msaa->tab, UiTab::Graphics);
    const KeyDef* scale = schema.find(QStringLiteral("RENDER"), QStringLiteral("Render_Scale"));
    QVERIFY(scale != nullptr);
    QCOMPARE(scale->type, KeyType::Float);
    QCOMPARE(scale->min.value_or(0), 1.0);
    QCOMPARE(scale->max.value_or(0), 2.0);
    QCOMPARE(scale->step.value_or(0), 0.25);
    QCOMPARE(schema.find(QStringLiteral("render.resolution"))->type, KeyType::Resolution);
    QCOMPARE(schema.find(QStringLiteral("paths.iso"))->type, KeyType::PathFile);
    QCOMPARE(schema.find(QStringLiteral("paths.userdata"))->widget, UiWidget::Dir);
    QVERIFY(schema.findByWidget(UiWidget::Monitor) != nullptr);
    QVERIFY(schema.find(QStringLiteral("render.nonexistent")) == nullptr);
    QVERIFY(schema.find(QStringLiteral("nodot")) == nullptr);
    // Hidden choices (accepted, not offered) include a numeric range like the host's kKnownKeys.
    const KeyDef* lang = schema.find(QStringLiteral("settings.language"));
    QCOMPARE(lang->choices.size(), 5);
    QVERIFY(lang->hiddenChoices.last().isRange());
    QVERIFY(lang->findChoice(QStringLiteral("GERMAN")) != nullptr);
    QVERIFY(lang->findChoice(QStringLiteral("japanese")) == nullptr);
    // Optional fields of the host schema.
    QCOMPARE(schema.find(QStringLiteral("paths.iso"))->fileFilter, QStringLiteral("*.iso *.xiso"));
    QVERIFY(schema.find(QStringLiteral("paths.userdata"))->dynamicDefault);
    QVERIFY(!schema.find(QStringLiteral("paths.userdata"))->placeholder.isEmpty());
    QVERIFY(schema.find(QStringLiteral("paths.xbe"))->developer);
    QVERIFY(!schema.find(QStringLiteral("paths.iso"))->developer);
    QVERIFY(!schema.find(QStringLiteral("render.window_scale"))->rangeEnforced);
    QVERIFY(schema.find(QStringLiteral("render.render_scale"))->rangeEnforced);
    QCOMPARE(schema.find(QStringLiteral("render.resolution"))->choices.size(), 6);
}

void SchemaTest::defaultsAsIniText() {
    const ConfigSchema schema = LoadSample();
    QCOMPARE(schema.find(QStringLiteral("render.render_scale"))->defaultText, QStringLiteral("1.0"));
    QCOMPARE(schema.find(QStringLiteral("render.alpha_to_coverage"))->defaultText, QStringLiteral("1"));
    QCOMPARE(schema.find(QStringLiteral("render.fullscreen"))->defaultText, QStringLiteral("0"));
    QCOMPARE(schema.find(QStringLiteral("render.window_scale"))->defaultText, QStringLiteral("2"));
    QCOMPARE(schema.find(QStringLiteral("game.tick_rate"))->defaultText, QStringLiteral("60"));
    QCOMPARE(schema.find(QStringLiteral("kernel.fiber_stack"))->defaultText, QStringLiteral("524288"));
    QCOMPARE(schema.find(QStringLiteral("paths.iso"))->defaultText, QString());
    QCOMPARE(DefaultTextFor(KeyType::Float, QJsonValue(1.25)), QStringLiteral("1.25"));
    QCOMPARE(DefaultTextFor(KeyType::Float, QJsonValue(2)), QStringLiteral("2.0"));
    QCOMPARE(DefaultTextFor(KeyType::Choice, QJsonValue(8)), QStringLiteral("8"));
    QCOMPARE(DefaultTextFor(KeyType::Bool, QJsonValue(QStringLiteral("on"))), QStringLiteral("on"));
    QCOMPARE(DefaultTextFor(KeyType::String, QJsonValue()), QString());
    QCOMPARE(FormatFloat(0.25), QStringLiteral("0.25"));
}

void SchemaTest::tabsAndOrder() {
    const ConfigSchema schema = LoadSample();
    const QList<const KeyDef*> graphics = schema.keysForTab(UiTab::Graphics);
    QVERIFY(!graphics.isEmpty());
    QCOMPARE(graphics.first()->name, QStringLiteral("fullscreen"));
    for (qsizetype i = 1; i < graphics.size(); ++i) {
        QVERIFY(graphics[i - 1]->order <= graphics[i]->order);
    }
    // Advanced keys are only on the advanced tab, whatever their ui.tab says.
    for (UiTab tab : {UiTab::Game, UiTab::Graphics, UiTab::Paths}) {
        for (const KeyDef* key : schema.keysForTab(tab)) {
            QVERIFY2(!key->advanced, qPrintable(key->fullName()));
        }
    }
    const KeyDef* xbe = schema.find(QStringLiteral("paths.xbe"));
    QVERIFY(xbe->advanced);
    QCOMPARE(xbe->tab, UiTab::Advanced);
    QCOMPARE(schema.find(QStringLiteral("render.screenshot_dir"))->tab, UiTab::Paths);
    QCOMPARE(schema.keysForTab(UiTab::Paths).first()->name, QStringLiteral("iso"));
    int total = 0;
    for (UiTab tab : kAllTabs) {
        total += static_cast<int>(schema.keysForTab(tab).size());
    }
    QCOMPARE(total, static_cast<int>(schema.allKeys().size()));
}

void SchemaTest::tolerantParsing() {
    const QByteArray json = R"({"schema_version":1,"build":"t","sections":[
        {"name":"Render","keys":[
          {"name":"Weird","type":"colour","default":"x"},
          {"name":"flag","type":"bool","default":true,"ui":{"widget":"spin","tab":"nowhere"}},
          {"name":"empty_choice","type":"choice","default":"a"},
          {"name":"steps","type":"int","default":3,"step":0},
          {"name":"flag","type":"bool"}
        ]},
        {"name":"debug","keys":[{"name":"x","type":"int","default":1}]},
        {"name":"render","keys":[]}
    ]})";
    ConfigSchema schema;
    QString error;
    QStringList warnings;
    QVERIFY2(ParseSchema(json, &schema, &error, &warnings), qPrintable(error));
    QCOMPARE(schema.find(QStringLiteral("render.weird"))->type, KeyType::String);
    const KeyDef* flag = schema.find(QStringLiteral("render.flag"));
    QCOMPARE(flag->widget, UiWidget::Check);  // spin does not fit bool
    QCOMPARE(flag->tab, UiTab::Graphics);     // unknown tab -> section fallback
    QCOMPARE(schema.find(QStringLiteral("render.empty_choice"))->type, KeyType::String);
    QVERIFY(!schema.find(QStringLiteral("render.steps"))->step.has_value());
    QCOMPARE(schema.find(QStringLiteral("debug.x"))->tab, UiTab::Advanced);
    QCOMPARE(schema.find(QStringLiteral("render.weird"))->label.text(), QString());
    QCOMPARE(schema.find(QStringLiteral("render.weird"))->displayLabel(), QStringLiteral("weird"));
    QVERIFY(warnings.size() >= 6);
}

void SchemaTest::rejectsBrokenDocuments() {
    ConfigSchema schema;
    QString error;
    QVERIFY(!ParseSchema("not json", &schema, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!ParseSchema(R"({"build":"x","sections":[{"name":"a","keys":[{"name":"k","type":"int"}]}]})",
                         &schema, &error));
    QVERIFY(!ParseSchema(R"({"schema_version":1,"sections":[]})", &schema, &error));
    QVERIFY(!ParseSchema(R"({"schema_version":1,"sections":[{"name":"a","keys":[]}]})", &schema, &error));
    QStringList warnings;
    QVERIFY(ParseSchema(
        R"({"schema_version":2,"sections":[{"name":"a","keys":[{"name":"k","type":"int","new_field":1}]}]})",
        &schema, &error, &warnings));
    QCOMPARE(warnings.size(), 1);
}

void SchemaTest::gpuInfo() {
    GpuInfo gpu;
    QString error;
    QVERIFY(ParseGpuInfo(test::ReadFile(test::TestdataPath("sample_gpu.json")), &gpu, &error));
    QVERIFY(gpu.valid);
    QCOMPARE(gpu.adapters.size(), 1);
    QCOMPARE(gpu.adapters[0].vendorId, 4318u);
    QCOMPARE(gpu.outputs.size(), 1);
    const GpuOutput* primary = gpu.primaryOutput();
    QVERIFY(primary != nullptr);
    QCOMPARE(primary->device, QStringLiteral("\\\\.\\DISPLAY1"));
    QCOMPARE(primary->nativeSize(), QSize(3840, 2160));
    QCOMPARE(primary->refreshHz(), 120.0);
    QCOMPARE(primary->modes.first(), QSize(3840, 2160));
    QCOMPARE(gpu.maxMsaa(), 8);
    QCOMPARE(gpu.maxAnisotropy, 16);
    QVERIFY(gpu.tearing);
    QVERIFY(gpu.findOutput(QStringLiteral("\\\\.\\display1")) == primary);
    QVERIFY(gpu.findOutput(QStringLiteral("LG TV SSCR2")) == primary);
    QVERIFY(gpu.findOutput(QStringLiteral("\\\\.\\DISPLAY2")) == nullptr);
    QVERIFY(!ParseGpuInfo("[]", &gpu, &error));
}

void SchemaTest::isoResult() {
    IsoCheckResult r;
    QString error;
    QVERIFY(ParseIsoCheckResult(
        R"({"ok":true,"reason":"","xdvdfs":true,"layout":"redump","xbe_sha1":"ab","version":"retail_eu","supported":true,"title":"Futurama","size":7825162240})",
        &r, &error));
    QCOMPARE(r.verdict(), IsoVerdict::Supported);
    QCOMPARE(r.size, Q_INT64_C(7825162240));
    QCOMPARE(r.layout, QStringLiteral("redump"));
    r.supported = false;
    QCOMPARE(r.verdict(), IsoVerdict::UnsupportedVersion);
    r.xdvdfs = false;
    QCOMPARE(r.verdict(), IsoVerdict::NotXboxImage);
    r.runError = QStringLiteral("x");
    QCOMPARE(r.verdict(), IsoVerdict::CheckFailed);
    QVERIFY(!ParseIsoCheckResult("garbage", &r, &error));
}
