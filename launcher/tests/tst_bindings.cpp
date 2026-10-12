#include "tst_bindings.h"

#include "core/bindings.h"
#include "core/l10n.h"
#include "core/pad_reader.h"
#include "core/settings_model.h"
#include "core/validation.h"
#include "test_support.h"

#include <QRegularExpression>
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

void BindingsTest::tokens() {
    // Canonical form: lower case, named key instead of vk_xx.
    QCOMPARE(CanonicalToken(QStringLiteral(" KEY:Enter ")), std::optional<QString>(QStringLiteral("key:enter")));
    QCOMPARE(CanonicalToken(QStringLiteral("key:vk_20")), std::optional<QString>(QStringLiteral("key:space")));
    QCOMPARE(CanonicalToken(QStringLiteral("key:VK_E8")), std::optional<QString>(QStringLiteral("key:vk_e8")));
    QCOMPARE(CanonicalToken(QStringLiteral("key:f12")), std::optional<QString>(QStringLiteral("key:f12")));
    QCOMPARE(CanonicalToken(QStringLiteral("key:num7")), std::optional<QString>(QStringLiteral("key:num7")));
    QCOMPARE(CanonicalToken(QStringLiteral("Pad:LY+")), std::optional<QString>(QStringLiteral("pad:ly+")));
    QCOMPARE(CanonicalToken(QStringLiteral("mouse:Wheel_Up")), std::optional<QString>(QStringLiteral("mouse:wheel_up")));
    for (const char* bad : {"key:", "key:f25", "key:f0", "key:f01", "key:vk_00", "key:vk_ff", "key:vk_1", "key:ä",
                            "pad:lz+", "pad:white", "mouse:x3", "joy:a", "a", ":a", "",
                            // same edge cases as tests/input_bindings_tests.cpp (the host rejects them too)
                            "key:f+1", "key:vk_+1", "key:vk_+f", "key:f 1", "key:vk_ 1", "key:f-1", "pad:a:b"}) {
        QVERIFY2(!CanonicalToken(QString::fromUtf8(bad)).has_value(), bad);
    }
    // Blanks around ':' are ignored, as in inp::ParseToken.
    QCOMPARE(CanonicalToken(QStringLiteral("key : a")), std::optional<QString>(QStringLiteral("key:a")));
    QCOMPARE(CanonicalToken(QStringLiteral("key: space")), std::optional<QString>(QStringLiteral("key:space")));
    QCOMPARE(CanonicalToken(QStringLiteral("pad : a")), std::optional<QString>(QStringLiteral("pad:a")));
    QCOMPARE(CanonicalToken(QStringLiteral(" mouse :left ")), std::optional<QString>(QStringLiteral("mouse:left")));
    QCOMPARE(KeyTokenVk(QStringLiteral("key : a")), int('A'));
    // Every token of a device is canonical, belongs to the device and maps back to itself.
    for (BindDevice device : {BindDevice::Pad, BindDevice::KeyboardMouse}) {
        const QStringList all = AllTokens(device);
        QVERIFY(all.size() > (device == BindDevice::Pad ? 20 : 100));
        for (const QString& t : all) {
            QCOMPARE(CanonicalToken(t), std::optional<QString>(t));
            QCOMPARE(TokenDevice(t), std::optional<BindDevice>(device));
        }
    }
    QCOMPARE(AllTokens(BindDevice::Pad).size(), 24);
    // Virtual-key codes of the contract table (INTERFACES.md 4.26).
    QCOMPARE(KeyTokenVk(QStringLiteral("key:a")), 0x41);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:0")), 0x30);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:f1")), 0x70);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:f24")), 0x87);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:num0")), 0x60);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:lshift")), 0xA0);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:ralt")), 0xA5);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:escape")), 0x1B);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:enter")), 0x0D);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:minus")), 0xBD);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:apostrophe")), 0xDE);
    QCOMPARE(KeyTokenVk(QStringLiteral("key:oem102")), 0xE2);
    QCOMPARE(KeyTokenVk(QStringLiteral("mouse:left")), 0);
}

void BindingsTest::bindingLists() {
    BindingParse p = ParseBindingList(QStringLiteral(" Key:Enter ,mouse:LEFT,, key:enter "), BindDevice::KeyboardMouse);
    QVERIFY(p.ok());
    QCOMPARE(p.tokens, (QStringList{QStringLiteral("key:enter"), QStringLiteral("mouse:left")}));
    QCOMPARE(p.itemCount, 3);
    QCOMPARE(FormatBindingList(p.tokens), QStringLiteral("key:enter, mouse:left"));
    QCOMPARE(FormatBindingList({QStringLiteral("KEY:W"), QStringLiteral("key:w"), QStringLiteral("key:vk_57")}),
             QStringLiteral("key:w"));
    QCOMPARE(FormatBindingList({}), QString());

    p = ParseBindingList(QString(), BindDevice::Pad);
    QVERIFY(p.ok());
    QVERIFY(p.tokens.isEmpty());

    p = ParseBindingList(QStringLiteral("pad:a, key:w"), BindDevice::Pad);
    QVERIFY(!p.ok());
    QCOMPARE(p.wrongDevice, QStringList{QStringLiteral("key:w")});
    QVERIFY(BindingProblemText(p, BindDevice::Pad).contains(QLatin1String("key:w")));

    p = ParseBindingList(QStringLiteral("pad:a"), BindDevice::KeyboardMouse);
    QCOMPARE(p.wrongDevice, QStringList{QStringLiteral("pad:a")});

    p = ParseBindingList(QStringLiteral("key:w, key:nope"), BindDevice::KeyboardMouse);
    QCOMPARE(p.invalid, QStringList{QStringLiteral("key:nope")});
    QCOMPARE(p.tokens, QStringList{QStringLiteral("key:w")});

    p = ParseBindingList(QStringLiteral("key:a, key:b, key:c, key:d"), BindDevice::KeyboardMouse);
    QVERIFY(p.ok());
    p = ParseBindingList(QStringLiteral("key:a, key:b, key:c, key:d, key:e"), BindDevice::KeyboardMouse);
    QVERIFY(p.tooMany());
    QVERIFY(!p.ok());
    QVERIFY(!BindingProblemText(p, BindDevice::KeyboardMouse).isEmpty());
    // Duplicates do not count towards the limit (like the host).
    p = ParseBindingList(QStringLiteral("pad:a, pad:a, pad:a, pad:a, pad:a"), BindDevice::Pad);
    QVERIFY(p.ok());
    QCOMPARE(p.tokens, QStringList{QStringLiteral("pad:a")});

    QVERIFY(BindingListsEquivalent(QStringLiteral("key:enter, mouse:left"), QStringLiteral("MOUSE:left,key:vk_0d"),
                                   BindDevice::KeyboardMouse));
    QVERIFY(!BindingListsEquivalent(QStringLiteral("key:enter"), QStringLiteral("key:enter, mouse:left"),
                                    BindDevice::KeyboardMouse));
    QVERIFY(BindingListsEquivalent(QString(), QStringLiteral(" "), BindDevice::Pad));
}

void BindingsTest::schemaKeys() {
    const ConfigSchema schema = LoadSample();
    int pad = 0;
    int kbm = 0;
    for (const KeyDef* key : schema.allKeys()) {
        const auto info = ParseBindingKey(*key);
        if (!info) {
            continue;
        }
        (info->device == BindDevice::Pad ? pad : kbm) += 1;
        QCOMPARE(key->tab, UiTab::Controls);
        QCOMPARE(key->widget, UiWidget::Binding);
        QVERIFY2(FindControl(info->control) != nullptr, qPrintable(key->fullName()));
        // Every default is a valid list of the key's device.
        QVERIFY2(ParseBindingList(key->defaultText, info->device).ok(), qPrintable(key->fullName()));
        QVERIFY2(!ParseBindingList(key->defaultText, info->device).tokens.isEmpty(), qPrintable(key->fullName()));
        const BindingLabel label = SplitBindingLabel(*key);
        QVERIFY2(!label.control.isEmpty() && !label.meaning.isEmpty(), qPrintable(key->fullName()));
    }
    QCOMPARE(pad, static_cast<int>(XboxControls().size()));
    QCOMPARE(kbm, static_cast<int>(XboxControls().size()));
    // Same order for both devices; the other [input] keys are no binding keys.
    QCOMPARE(schema.find(QStringLiteral("input.pad_rt"))->order, schema.find(QStringLiteral("input.kbm_rt"))->order);
    for (const char* name : {"input.pad_mode", "input.pad_deadzone", "input.mouse_sensitivity", "input.rumble",
                             "input.rumble_strength"}) {
        const KeyDef* key = schema.find(QLatin1String(name));
        QVERIFY2(key != nullptr, name);
        QVERIFY2(!IsBindingKey(*key), name);
        QCOMPARE(key->tab, UiTab::Controls);
    }
    QCOMPARE(schema.find(QStringLiteral("input.pad_deadzone"))->widget, UiWidget::Slider);
    QCOMPARE(schema.find(QStringLiteral("input.pad_deadzone"))->unit, QStringLiteral("%"));
    QCOMPARE(schema.find(QStringLiteral("input.mouse_sensitivity"))->widget, UiWidget::Slider);
    // Defaults of m4_input.md 9 / INTERFACES.md 4.26.
    QCOMPARE(schema.find(QStringLiteral("input.kbm_a"))->defaultText, QStringLiteral("key:enter, mouse:left"));
    QCOMPARE(schema.find(QStringLiteral("input.kbm_start"))->defaultText, QStringLiteral("key:escape"));
    QCOMPARE(schema.find(QStringLiteral("input.pad_white"))->defaultText, QStringLiteral("pad:lb"));
    QCOMPARE(schema.find(QStringLiteral("input.pad_ls_up"))->defaultText, QStringLiteral("pad:ly+"));
    // The label split: "A – Bestätigen / ..." in both languages.
    SetLanguage(Language::English);
    QCOMPARE(SplitBindingLabel(*schema.find(QStringLiteral("input.kbm_b"))).control, QStringLiteral("B"));
    QCOMPARE(SplitBindingLabel(*schema.find(QStringLiteral("input.kbm_b"))).meaning, QStringLiteral("Back / jump"));
    SetLanguage(Language::German);
    QCOMPARE(SplitBindingLabel(*schema.find(QStringLiteral("input.kbm_white"))).control, QStringLiteral("Weiß"));

    // Schemas that do not name the widget: [input] pad_<control> / kbm_<control> strings are binding keys,
    // pad_deadzone is not (no such control); a declared binding widget is always one.
    const QByteArray json = R"({"schema_version":1,"build":"t","sections":[
        {"name":"input","keys":[
          {"name":"pad_a","type":"string","default":"pad:a"},
          {"name":"kbm_ls_up","type":"string","default":"key:w"},
          {"name":"pad_deadzone","type":"string","default":"0"},
          {"name":"kbm_freecam","type":"string","default":"key:f6","ui":{"widget":"binding"}},
          {"name":"strength","type":"int","default":5,"ui":{"widget":"slider"}},
          {"name":"bad","type":"bool","default":1,"ui":{"widget":"binding"}}
        ]}]})";
    ConfigSchema plain;
    QString error;
    QStringList warnings;
    QVERIFY2(ParseSchema(json, &plain, &error, &warnings), qPrintable(error));
    QVERIFY(IsBindingKey(*plain.find(QStringLiteral("input.pad_a"))));
    QCOMPARE(ParseBindingKey(*plain.find(QStringLiteral("input.kbm_ls_up")))->device, BindDevice::KeyboardMouse);
    QVERIFY(!IsBindingKey(*plain.find(QStringLiteral("input.pad_deadzone"))));
    QCOMPARE(ParseBindingKey(*plain.find(QStringLiteral("input.kbm_freecam")))->control, QStringLiteral("freecam"));
    QCOMPARE(plain.find(QStringLiteral("input.strength"))->widget, UiWidget::Slider);
    QCOMPARE(plain.find(QStringLiteral("input.bad"))->widget, UiWidget::Check);  // binding does not fit bool
    QCOMPARE(plain.find(QStringLiteral("input.pad_a"))->tab, UiTab::Controls);   // section fallback
    QCOMPARE(warnings.size(), 1);
}

void BindingsTest::validationAndEquivalence() {
    const ConfigSchema schema = LoadSample();
    const KeyDef& kbmA = *schema.find(QStringLiteral("input.kbm_a"));
    const KeyDef& padA = *schema.find(QStringLiteral("input.pad_a"));
    QVERIFY(!ValidateValue(kbmA, QStringLiteral("key:enter, mouse:left"), QString()).isError());
    QVERIFY(!ValidateValue(kbmA, QString(), QString()).isError());  // not bound
    ValidationResult r = ValidateValue(kbmA, QStringLiteral("pad:a"), QString());
    QVERIFY(r.isError());
    QCOMPARE(r.code, IssueCode::BadBinding);
    r = ValidateValue(padA, QStringLiteral("pad:a, pad:jump"), QString());
    QVERIFY(r.isError());
    QVERIFY(r.message.contains(QLatin1String("pad:jump")));
    QVERIFY(ValidateValue(padA, QStringLiteral("pad:a,pad:b,pad:x,pad:y,pad:lb"), QString()).isError());
    QVERIFY(ValuesEquivalent(kbmA, QStringLiteral("mouse:left, key:enter"), QStringLiteral("key:enter, mouse:left")));
    QVERIFY(!ValuesEquivalent(kbmA, QStringLiteral("key:enter"), QStringLiteral("key:enter, mouse:left")));
}

void BindingsTest::iniRoundTrip() {
    const QString dir = test::MakeTestDir(QStringLiteral("bindings ini"));
    const QString ini = QDir(dir).filePath(QStringLiteral("futurama.ini"));
    SettingsModel m;
    m.setSchema(LoadSample());
    QString error;
    QVERIFY(m.load(ini, &error));
    const ConfigSchema& s = m.schema();
    const KeyDef& kbmA = *s.find(QStringLiteral("input.kbm_a"));
    const KeyDef& kbmB = *s.find(QStringLiteral("input.kbm_b"));
    const KeyDef& padY = *s.find(QStringLiteral("input.pad_y"));
    const KeyDef& kbmX = *s.find(QStringLiteral("input.kbm_x"));

    // The default in another order is no edit (the file stays minimal).
    m.setValue(kbmA, QStringLiteral("mouse:left, key:enter"));
    QVERIFY(!m.isPending(kbmA));
    QVERIFY(m.isAtDefault(kbmA));

    m.setValue(kbmA, FormatBindingList({QStringLiteral("key:k"), QStringLiteral("mouse:right")}));
    m.setValue(kbmB, QString());  // not bound
    m.setValue(padY, QStringLiteral("PAD:Y,pad:rs_click , pad:y"));  // written canonically
    QVERIFY(m.isPending(kbmA));
    QVERIFY(m.save(&error));
    const QString text = QString::fromUtf8(test::ReadFile(ini));
    QVERIFY2(text.contains(QStringLiteral("[input]")), qPrintable(text));
    QVERIFY2(text.contains(QRegularExpression(QStringLiteral("\nkbm_a *= key:k, mouse:right\r?\n"))), qPrintable(text));
    QVERIFY2(text.contains(QRegularExpression(QStringLiteral("\nkbm_b *=[ ]*\r?\n"))), qPrintable(text));
    QVERIFY2(text.contains(QRegularExpression(QStringLiteral("\npad_y *= pad:y, pad:rs_click\r?\n"))), qPrintable(text));
    QVERIFY(!text.contains(QLatin1String("kbm_x")));  // untouched keys are not written

    SettingsModel again;
    again.setSchema(LoadSample());
    QVERIFY(again.load(ini, &error));
    const ConfigSchema& s2 = again.schema();
    QCOMPARE(again.value(*s2.find(QStringLiteral("input.kbm_a"))), QStringLiteral("key:k, mouse:right"));
    QCOMPARE(again.value(*s2.find(QStringLiteral("input.kbm_b"))), QString());
    QVERIFY(again.isExplicit(*s2.find(QStringLiteral("input.kbm_b"))));
    QVERIFY(!again.isAtDefault(*s2.find(QStringLiteral("input.kbm_b"))));
    QCOMPARE(again.value(*s2.find(QStringLiteral("input.kbm_x"))), kbmX.defaultText);
    QVERIFY(again.keysWithErrors().isEmpty());

    // A hand-written list is equivalent to the canonical one; an invalid one blocks saving.
    QVERIFY(test::WriteFile(ini, "[input]\r\nkbm_a = MOUSE:Right,Key:K\r\npad_b = key:space\r\n"));
    SettingsModel hand;
    hand.setSchema(LoadSample());
    QVERIFY(hand.load(ini, &error));
    const ConfigSchema& s3 = hand.schema();
    QVERIFY(BindingListsEquivalent(hand.value(*s3.find(QStringLiteral("input.kbm_a"))),
                                   QStringLiteral("key:k, mouse:right"), BindDevice::KeyboardMouse));
    const QList<const KeyDef*> errors = hand.keysWithErrors();
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.first()->fullName(), QStringLiteral("input.pad_b"));
}

void BindingsTest::captureMapping() {
    // Native virtual-key codes (Windows): Shift/Ctrl/Alt resolved to left/right by the scan code.
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Shift, Qt::ShiftModifier, 0x10, 0x2A), std::optional<QString>(QStringLiteral("key:lshift")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Shift, Qt::ShiftModifier, 0x10, 0x36), std::optional<QString>(QStringLiteral("key:rshift")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Control, Qt::ControlModifier, 0x11, 0x1D), std::optional<QString>(QStringLiteral("key:lctrl")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Control, Qt::ControlModifier, 0x11, 0x11D), std::optional<QString>(QStringLiteral("key:rctrl")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Alt, Qt::AltModifier, 0x12, 0x138), std::optional<QString>(QStringLiteral("key:ralt")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Z, Qt::NoModifier, 0x59, 0x15), std::optional<QString>(QStringLiteral("key:y")));  // QWERTZ: the VK wins
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Udiaeresis, Qt::NoModifier, 0xBA, 0x1A), std::optional<QString>(QStringLiteral("key:semicolon")));
    QCOMPARE(KeyTokenFromEvent(0, Qt::NoModifier, 0xE8, 0), std::optional<QString>(QStringLiteral("key:vk_e8")));
    // Without native codes (offscreen platform): the Qt key.
    QCOMPARE(KeyTokenFromEvent(Qt::Key_W, Qt::NoModifier, 0, 0), std::optional<QString>(QStringLiteral("key:w")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_5, Qt::KeypadModifier, 0, 0), std::optional<QString>(QStringLiteral("key:num5")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_5, Qt::NoModifier, 0, 0), std::optional<QString>(QStringLiteral("key:5")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Enter, Qt::KeypadModifier, 0, 0), std::optional<QString>(QStringLiteral("key:enter")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Escape, Qt::NoModifier, 0, 0), std::optional<QString>(QStringLiteral("key:escape")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Backtab, Qt::ShiftModifier, 0, 0), std::optional<QString>(QStringLiteral("key:tab")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_F11, Qt::NoModifier, 0, 0), std::optional<QString>(QStringLiteral("key:f11")));
    QCOMPARE(KeyTokenFromEvent(Qt::Key_Minus, Qt::KeypadModifier, 0, 0), std::optional<QString>(QStringLiteral("key:num_sub")));
    QVERIFY(!KeyTokenFromEvent(Qt::Key_unknown, Qt::NoModifier, 0, 0).has_value());
    QVERIFY(!KeyTokenFromEvent(0, Qt::NoModifier, 0x01, 0).has_value());  // VK_LBUTTON is no key
    QCOMPARE(MouseButtonToken(Qt::LeftButton), std::optional<QString>(QStringLiteral("mouse:left")));
    QCOMPARE(MouseButtonToken(Qt::BackButton), std::optional<QString>(QStringLiteral("mouse:x1")));
    QCOMPARE(MouseButtonToken(Qt::ForwardButton), std::optional<QString>(QStringLiteral("mouse:x2")));
    QVERIFY(!MouseButtonToken(Qt::NoButton).has_value());
    QCOMPARE(WheelToken(120), QStringLiteral("mouse:wheel_up"));
    QCOMPARE(WheelToken(-120), QStringLiteral("mouse:wheel_down"));
    QCOMPARE(WheelToken(0), QString());
}

void BindingsTest::padStates() {
    PadState s;
    QVERIFY(ActivePadTokens(s).isEmpty());  // not connected
    s.connected = true;
    s.buttons = 0x1000 | 0x0100 | 0x0010 | 0x0001 | 0x0080;  // A, LB, START, d-pad up, right stick click
    s.leftTrigger = 120;   // below half
    s.rightTrigger = 200;  // above half
    s.lx = -20000;
    s.ly = 16000;  // just below half (16383.5)
    s.rx = 32767;
    s.ry = -32768;
    const QSet<QString> active = ActivePadTokens(s);
    const QStringList sorted = SortedPadTokens(active);
    QCOMPARE(sorted, (QStringList{QStringLiteral("pad:a"), QStringLiteral("pad:lb"), QStringLiteral("pad:rt"),
                                  QStringLiteral("pad:start"), QStringLiteral("pad:dpad_up"),
                                  QStringLiteral("pad:rs_click"), QStringLiteral("pad:lx-"), QStringLiteral("pad:ry-"),
                                  QStringLiteral("pad:rx+")}));
    QVERIFY(ActivePadTokens(s, 0.4).contains(QStringLiteral("pad:ly+")));
    QVERIFY(ActivePadTokens(s, 0.4).contains(QStringLiteral("pad:lt")));

    SimulatedPadReader reader;
    QVERIFY(reader.available());
    QVERIFY(!reader.read(0).connected);
    QVERIFY(!reader.setRumble(0, 1.0, 1.0));  // not connected
    reader.setState(2, s);
    QCOMPARE(reader.read(2).buttons, s.buttons);
    QVERIFY(reader.setRumble(2, 0.5, 0.25));
    QCOMPARE(reader.rumble(2).left, 0.5);
    QCOMPARE(reader.rumble(2).right, 0.25);
    QCOMPARE(reader.rumble(2).calls, 1);
    QCOMPARE(reader.readCount(2), 1);
    QVERIFY(!reader.read(7).connected);

    // The real XInput backend loads (Windows 8+: xinput1_4.dll); without a controller every slot is empty. Real
    // controllers are not part of the automated tests.
    auto xinput = CreateXInputReader();
    QVERIFY(xinput->available());
    QVERIFY(xinput->backendName().startsWith(QLatin1String("xinput")));
    QVERIFY(!xinput->read(-1).connected);
    QVERIFY(!xinput->read(kPadSlots).connected);
}

void BindingsTest::conflicts() {
    const QList<BindingRow> rows = {
        {QStringLiteral("a"), {QStringLiteral("key:enter"), QStringLiteral("mouse:left")}},
        {QStringLiteral("x"), {QStringLiteral("key:f"), QStringLiteral("mouse:left")}},
        {QStringLiteral("y"), {QStringLiteral("key:e")}},
        {QStringLiteral("start"), {QStringLiteral("key:enter")}},
    };
    const QHash<QString, QStringList> c = FindConflicts(rows);
    QCOMPARE(c.size(), 2);
    QCOMPARE(c.value(QStringLiteral("mouse:left")), (QStringList{QStringLiteral("a"), QStringLiteral("x")}));
    QCOMPARE(c.value(QStringLiteral("key:enter")), (QStringList{QStringLiteral("a"), QStringLiteral("start")}));
    QVERIFY(!c.contains(QStringLiteral("key:e")));
    QCOMPARE(ControlsForTokens({QStringLiteral("mouse:left")}, rows), (QStringList{QStringLiteral("a"), QStringLiteral("x")}));
    QVERIFY(ControlsForTokens({QStringLiteral("key:q")}, rows).isEmpty());

    // The defaults of the sample schema have no conflicts on either device.
    const ConfigSchema schema = LoadSample();
    for (BindDevice device : {BindDevice::Pad, BindDevice::KeyboardMouse}) {
        QList<BindingRow> defaults;
        for (const KeyDef* key : schema.allKeys()) {
            const auto info = ParseBindingKey(*key);
            if (info && info->device == device) {
                defaults.append({info->control, ParseBindingList(key->defaultText, device).tokens});
            }
        }
        QVERIFY(FindConflicts(defaults).isEmpty());
    }
}

void BindingsTest::displayTexts() {
    for (Language lang : {Language::German, Language::English}) {
        SetLanguage(lang);
        for (BindDevice device : {BindDevice::Pad, BindDevice::KeyboardMouse}) {
            for (const QString& t : AllTokens(device)) {
                const QString text = TokenDisplayText(t);
                QVERIFY2(!text.isEmpty() && !text.startsWith(QLatin1Char('[')), qPrintable(t + QStringLiteral(" -> ") + text));
            }
        }
        for (const ControlInfo& c : XboxControls()) {
            QVERIFY2(!ControlDisplayName(QString::fromLatin1(c.id)).startsWith(QLatin1Char('[')), c.id);
        }
        for (ControlGroup g : {ControlGroup::FaceButtons, ControlGroup::Shoulders, ControlGroup::System,
                               ControlGroup::DPad, ControlGroup::LeftStick, ControlGroup::RightStick,
                               ControlGroup::Other}) {
            QVERIFY(!ControlGroupTitle(g).startsWith(QLatin1Char('[')));
        }
    }
    SetLanguage(Language::German);
    QCOMPARE(TokenDisplayText(QStringLiteral("key:space")), QStringLiteral("Leertaste"));
    QCOMPARE(TokenDisplayText(QStringLiteral("key:w")), QStringLiteral("W"));
    QCOMPARE(TokenDisplayText(QStringLiteral("key:f5")), QStringLiteral("F5"));
    QCOMPARE(TokenDisplayText(QStringLiteral("key:num4")), QStringLiteral("Num 4"));
    QCOMPARE(TokenDisplayText(QStringLiteral("mouse:left")), QStringLiteral("Linke Maustaste"));
    QCOMPARE(TokenDisplayText(QStringLiteral("pad:ly+")), QStringLiteral("Linker Stick ↑"));
    QCOMPARE(TokenDisplayText(QStringLiteral("nonsense")), QStringLiteral("nonsense"));
    SetLanguage(Language::English);
    QCOMPARE(TokenDisplayText(QStringLiteral("key:lshift")), QStringLiteral("Left Shift"));
    QCOMPARE(ControlDisplayName(QStringLiteral("white")), QStringLiteral("White"));
    SetLanguage(Language::German);
}

// The token names and virtual-key codes must match the game's parser (src/host/input/bindings.cpp, the other side of
// INTERFACES.md 4.26): every name of its tables is a launcher token with the same code, and vice versa.
void BindingsTest::sameNamesAsHost() {
    const QString path = QStringLiteral(LAUNCHER_SOURCE_DIR "/../src/host/input/bindings.cpp");
    const QString host = QString::fromUtf8(test::ReadFile(path));
    if (host.isEmpty()) {
        QSKIP("src/host/input/bindings.cpp not found (launcher built outside the repository)");
    }
    auto table = [&host](const char* name) {
        QHash<QString, int> out;
        const qsizetype at = host.indexOf(QStringLiteral("constexpr Name %1[] = {").arg(QLatin1String(name)));
        if (at < 0) {
            return out;
        }
        const qsizetype end = host.indexOf(QStringLiteral("};"), at);
        const QString block = host.mid(at, end - at);
        static const QRegularExpression entry(QStringLiteral("\\{\"([a-z0-9_+-]+)\", (0x[0-9A-Fa-f]+|\\d+|kXi\\w+)\\}"));
        auto it = entry.globalMatch(block);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            bool ok = false;
            const int code = m.captured(2).toInt(&ok, 0);
            out.insert(m.captured(1), ok ? code : -1);
        }
        return out;
    };
    QSet<QString> hostPad;
    for (const char* t : {"kPadButtons", "kPadTriggers", "kPadHalfAxes"}) {
        const auto names = table(t);
        QVERIFY2(!names.isEmpty(), t);
        for (auto it = names.begin(); it != names.end(); ++it) {
            hostPad.insert(QStringLiteral("pad:") + it.key());
        }
    }
    const QStringList launcherPad = AllTokens(BindDevice::Pad);
    QCOMPARE(hostPad, QSet<QString>(launcherPad.begin(), launcherPad.end()));
    QSet<QString> hostMouse;
    for (const char* t : {"kMouseButtons", "kMouseWheel", "kMouseMove"}) {
        const auto names = table(t);
        QVERIFY2(!names.isEmpty(), t);
        for (auto it = names.begin(); it != names.end(); ++it) {
            hostMouse.insert(QStringLiteral("mouse:") + it.key());
        }
    }
    QSet<QString> launcherMouse;
    for (const QString& t : AllTokens(BindDevice::KeyboardMouse)) {
        if (t.startsWith(QLatin1String("mouse:"))) {
            launcherMouse.insert(t);
        }
    }
    QCOMPARE(hostMouse, launcherMouse);
    const auto keys = table("kKeys");
    QVERIFY(keys.size() > 40);
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const QString token = QStringLiteral("key:") + it.key();
        QCOMPARE(CanonicalToken(token), std::optional<QString>(token));
        QCOMPARE(KeyTokenVk(token), it.value());
    }
    // Launcher names outside the generated ranges (a-z, 0-9, f1-f24, num0-num9) are all in the host table.
    static const QRegularExpression generated(QStringLiteral("^key:([a-z0-9]|f\\d+|num\\d)$"));
    for (const QString& t : AllTokens(BindDevice::KeyboardMouse)) {
        if (t.startsWith(QLatin1String("key:")) && !generated.match(t).hasMatch()) {
            QVERIFY2(keys.contains(t.mid(4)), qPrintable(t));
        }
    }
}

// Review M4 finding 10: the "Für das Spiel" line follows the host's thresholds (src/host/input/bindings.cpp
// Compose): analog-button controls from 30/255, digital ones from 0.5, sticks outside the game's dead zone 0.4.
void BindingsTest::gameControlsLikeHost() {
    const QList<BindingRow> rows = {
        {QStringLiteral("a"), {QStringLiteral("pad:a")}},
        {QStringLiteral("rt"), {QStringLiteral("pad:rt")}},
        {QStringLiteral("start"), {QStringLiteral("pad:lt")}},  // a trigger on a digital control: from 0.5
        {QStringLiteral("x"), {QStringLiteral("pad:rx+")}},     // a half axis on an analog button: from 30/255
        {QStringLiteral("ls_up"), {QStringLiteral("pad:ly+")}},
        {QStringLiteral("ls_right"), {QStringLiteral("pad:lx+")}},
    };
    PadState p;
    p.connected = true;
    p.rightTrigger = 40;    // 16 %: the game counts it, the 0.5 display threshold does not
    p.leftTrigger = 100;    // 39 %: START (digital) not yet
    p.rx = 5000;            // 0.15 -> 39/255 >= 30
    p.ly = 10000;           // 0.31: inside the game's stick dead zone
    QCOMPARE(GameControlsForPad(p, rows, 0), (QStringList{QStringLiteral("rt"), QStringLiteral("x")}));
    QVERIFY(!ActivePadTokens(p).contains(QStringLiteral("pad:rt")));
    p.leftTrigger = 200;
    p.ly = 20000;           // 0.61 up
    p.lx = 6000;            // 0.18 right: below sin 22.5 deg of the radius -> only "up"
    QCOMPARE(GameControlsForPad(p, rows, 0),
             (QStringList{QStringLiteral("rt"), QStringLiteral("start"), QStringLiteral("x"), QStringLiteral("ls_up")}));
    p.lx = 20000;           // diagonal: both directions
    QVERIFY(GameControlsForPad(p, rows, 0).contains(QStringLiteral("ls_right")));
    // pad_deadzone (radial, percent) cuts the right stick (radius 0.15) before the bindings.
    QVERIFY(!GameControlsForPad(p, rows, 20).contains(QStringLiteral("x")));
    p.connected = false;
    QVERIFY(GameControlsForPad(p, rows, 0).isEmpty());
}
