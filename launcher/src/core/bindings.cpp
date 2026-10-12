#include "core/bindings.h"

#include "core/l10n.h"

#include <algorithm>
#include <array>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fl {
namespace {

constexpr ControlInfo kControls[] = {
    {"a", ControlGroup::FaceButtons, "bind.ctl.a"},
    {"b", ControlGroup::FaceButtons, "bind.ctl.b"},
    {"x", ControlGroup::FaceButtons, "bind.ctl.x"},
    {"y", ControlGroup::FaceButtons, "bind.ctl.y"},
    {"white", ControlGroup::Shoulders, "bind.ctl.white"},
    {"black", ControlGroup::Shoulders, "bind.ctl.black"},
    {"lt", ControlGroup::Shoulders, "bind.ctl.lt"},
    {"rt", ControlGroup::Shoulders, "bind.ctl.rt"},
    {"start", ControlGroup::System, "bind.ctl.start"},
    {"back", ControlGroup::System, "bind.ctl.back"},
    {"dpad_up", ControlGroup::DPad, "bind.ctl.dpad_up"},
    {"dpad_down", ControlGroup::DPad, "bind.ctl.dpad_down"},
    {"dpad_left", ControlGroup::DPad, "bind.ctl.dpad_left"},
    {"dpad_right", ControlGroup::DPad, "bind.ctl.dpad_right"},
    {"ls_up", ControlGroup::LeftStick, "bind.ctl.ls_up"},
    {"ls_down", ControlGroup::LeftStick, "bind.ctl.ls_down"},
    {"ls_left", ControlGroup::LeftStick, "bind.ctl.ls_left"},
    {"ls_right", ControlGroup::LeftStick, "bind.ctl.ls_right"},
    {"ls_click", ControlGroup::LeftStick, "bind.ctl.ls_click"},
    {"rs_up", ControlGroup::RightStick, "bind.ctl.rs_up"},
    {"rs_down", ControlGroup::RightStick, "bind.ctl.rs_down"},
    {"rs_left", ControlGroup::RightStick, "bind.ctl.rs_left"},
    {"rs_right", ControlGroup::RightStick, "bind.ctl.rs_right"},
    {"rs_click", ControlGroup::RightStick, "bind.ctl.rs_click"},
};

// pad: names (XInput buttons, triggers, half axes) with their display ids, in display order.
struct PadName {
    const char* name;
    const char* displayId;
};
constexpr PadName kPadNames[] = {
    {"a", "bind.pad.a"},
    {"b", "bind.pad.b"},
    {"x", "bind.pad.x"},
    {"y", "bind.pad.y"},
    {"lb", "bind.pad.lb"},
    {"rb", "bind.pad.rb"},
    {"lt", "bind.pad.lt"},
    {"rt", "bind.pad.rt"},
    {"start", "bind.pad.start"},
    {"back", "bind.pad.back"},
    {"dpad_up", "bind.pad.dpad_up"},
    {"dpad_down", "bind.pad.dpad_down"},
    {"dpad_left", "bind.pad.dpad_left"},
    {"dpad_right", "bind.pad.dpad_right"},
    {"ls_click", "bind.pad.ls_click"},
    {"rs_click", "bind.pad.rs_click"},
    {"ly+", "bind.pad.ly_pos"},
    {"ly-", "bind.pad.ly_neg"},
    {"lx-", "bind.pad.lx_neg"},
    {"lx+", "bind.pad.lx_pos"},
    {"ry+", "bind.pad.ry_pos"},
    {"ry-", "bind.pad.ry_neg"},
    {"rx-", "bind.pad.rx_neg"},
    {"rx+", "bind.pad.rx_pos"},
};

constexpr PadName kMouseNames[] = {
    {"left", "bind.mouse.left"},
    {"right", "bind.mouse.right"},
    {"middle", "bind.mouse.middle"},
    {"x1", "bind.mouse.x1"},
    {"x2", "bind.mouse.x2"},
    {"wheel_up", "bind.mouse.wheel_up"},
    {"wheel_down", "bind.mouse.wheel_down"},
    {"move_up", "bind.mouse.move_up"},
    {"move_down", "bind.mouse.move_down"},
    {"move_left", "bind.mouse.move_left"},
    {"move_right", "bind.mouse.move_right"},
};

// key: names with Windows virtual-key codes (INTERFACES.md 4.26). Letters, digits, F-keys and the numpad digits
// are generated; `displayId` = localized name, `symbol` = US-layout fallback for the OEM keys (their real label
// depends on the keyboard layout and comes from Windows).
struct KeyName {
    const char* name;
    int vk;
    const char* displayId;
    const char* symbol;
};
constexpr KeyName kNamedKeys[] = {
    {"space", 0x20, "bind.key.space", nullptr},
    {"enter", 0x0D, "bind.key.enter", nullptr},
    {"escape", 0x1B, "bind.key.escape", nullptr},
    {"backspace", 0x08, "bind.key.backspace", nullptr},
    {"tab", 0x09, "bind.key.tab", nullptr},
    {"capslock", 0x14, "bind.key.capslock", nullptr},
    {"lshift", 0xA0, "bind.key.lshift", nullptr},
    {"rshift", 0xA1, "bind.key.rshift", nullptr},
    {"lctrl", 0xA2, "bind.key.lctrl", nullptr},
    {"rctrl", 0xA3, "bind.key.rctrl", nullptr},
    {"lalt", 0xA4, "bind.key.lalt", nullptr},
    {"ralt", 0xA5, "bind.key.ralt", nullptr},
    {"lwin", 0x5B, "bind.key.lwin", nullptr},
    {"rwin", 0x5C, "bind.key.rwin", nullptr},
    {"menu", 0x5D, "bind.key.menu", nullptr},
    {"up", 0x26, "bind.key.up", nullptr},
    {"down", 0x28, "bind.key.down", nullptr},
    {"left", 0x25, "bind.key.left", nullptr},
    {"right", 0x27, "bind.key.right", nullptr},
    {"insert", 0x2D, "bind.key.insert", nullptr},
    {"delete", 0x2E, "bind.key.delete", nullptr},
    {"home", 0x24, "bind.key.home", nullptr},
    {"end", 0x23, "bind.key.end", nullptr},
    {"pageup", 0x21, "bind.key.pageup", nullptr},
    {"pagedown", 0x22, "bind.key.pagedown", nullptr},
    {"pause", 0x13, "bind.key.pause", nullptr},
    {"scrolllock", 0x91, "bind.key.scrolllock", nullptr},
    {"numlock", 0x90, "bind.key.numlock", nullptr},
    {"printscreen", 0x2C, "bind.key.printscreen", nullptr},
    {"num_add", 0x6B, "bind.key.num_add", nullptr},
    {"num_sub", 0x6D, "bind.key.num_sub", nullptr},
    {"num_mul", 0x6A, "bind.key.num_mul", nullptr},
    {"num_div", 0x6F, "bind.key.num_div", nullptr},
    {"num_dec", 0x6E, "bind.key.num_dec", nullptr},
    {"minus", 0xBD, nullptr, "-"},
    {"plus", 0xBB, nullptr, "="},
    {"comma", 0xBC, nullptr, ","},
    {"period", 0xBE, nullptr, "."},
    {"semicolon", 0xBA, nullptr, ";"},
    {"slash", 0xBF, nullptr, "/"},
    {"grave", 0xC0, nullptr, "`"},
    {"lbracket", 0xDB, nullptr, "["},
    {"backslash", 0xDC, nullptr, "\\"},
    {"rbracket", 0xDD, nullptr, "]"},
    {"apostrophe", 0xDE, nullptr, "'"},
    {"oem102", 0xE2, nullptr, "<>"},
};

constexpr int kVkShift = 0x10;
constexpr int kVkControl = 0x11;
constexpr int kVkMenu = 0x12;

// key: name -> virtual-key code (0 if unknown).
int KeyNameToVk(QStringView name) {
    if (name.size() == 1) {
        const char16_t c = name[0].unicode();
        if (c >= u'a' && c <= u'z') {
            return 'A' + (c - u'a');
        }
        if (c >= u'0' && c <= u'9') {
            return '0' + (c - u'0');
        }
        return 0;
    }
    // Digits only (like ParseNumbered in src/host/input/bindings.cpp): QString::toInt would accept a sign.
    const auto digit = [](QChar c) { return c.unicode() >= u'0' && c.unicode() <= u'9'; };
    const auto hex = [](QChar c) {
        const char16_t u = c.unicode();
        return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
    };
    if (name.size() <= 3 && name.startsWith(u'f')) {
        const QStringView digits = name.mid(1);
        if (!std::all_of(digits.begin(), digits.end(), digit) || digits.startsWith(u'0')) {
            return 0;
        }
        const int n = digits.toInt();
        return n >= 1 && n <= 24 ? 0x70 + n - 1 : 0;
    }
    if (name.size() == 4 && name.startsWith(QLatin1String("num"))) {
        const char16_t c = name[3].unicode();
        if (c >= u'0' && c <= u'9') {
            return 0x60 + (c - u'0');
        }
        return 0;
    }
    if (name.size() == 5 && name.startsWith(QLatin1String("vk_"))) {
        if (!hex(name[3]) || !hex(name[4])) {
            return 0;  // two hex digits (HexDigit in the host), no sign or blank
        }
        const int vk = name.mid(3).toInt(nullptr, 16);
        return vk >= 0x01 && vk <= 0xFE ? vk : 0;
    }
    for (const KeyName& k : kNamedKeys) {
        if (name == QLatin1String(k.name)) {
            return k.vk;
        }
    }
    return 0;
}

// Virtual-key code -> canonical key: name (named keys first, then vk_xx).
QString VkToKeyName(int vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return QString(QChar(u'a' + (vk - 'A')));
    }
    if (vk >= '0' && vk <= '9') {
        return QString(QChar(u'0' + (vk - '0')));
    }
    if (vk >= 0x70 && vk <= 0x87) {
        return QStringLiteral("f") + QString::number(vk - 0x70 + 1);
    }
    if (vk >= 0x60 && vk <= 0x69) {
        return QStringLiteral("num") + QString::number(vk - 0x60);
    }
    for (const KeyName& k : kNamedKeys) {
        if (k.vk == vk) {
            return QLatin1String(k.name);
        }
    }
    if (vk >= 0x01 && vk <= 0xFE) {
        return QStringLiteral("vk_%1").arg(vk, 2, 16, QLatin1Char('0'));
    }
    return QString();
}

const KeyName* FindNamedKey(QStringView name) {
    for (const KeyName& k : kNamedKeys) {
        if (name == QLatin1String(k.name)) {
            return &k;
        }
    }
    return nullptr;
}

template <size_t N>
const PadName* FindName(const PadName (&table)[N], QStringView name) {
    for (const PadName& p : table) {
        if (name == QLatin1String(p.name)) {
            return &p;
        }
    }
    return nullptr;
}

// Windows' name of a key in the current keyboard layout ("Ü", "#"), empty if unknown.
QString WindowsKeyName(int vk) {
#ifdef _WIN32
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (scan == 0) {
        return QString();
    }
    wchar_t buffer[64] = {};
    const int n = GetKeyNameTextW(static_cast<LONG>(scan << 16), buffer, 64);
    if (n <= 0) {
        return QString();
    }
    return QString::fromWCharArray(buffer, n);
#else
    (void)vk;
    return QString();
#endif
}

// Qt key -> virtual-key code (offscreen platform and tests: no native codes).
int VkFromQtKey(int qtKey, Qt::KeyboardModifiers modifiers) {
    const bool keypad = modifiers.testFlag(Qt::KeypadModifier);
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        return 'A' + (qtKey - Qt::Key_A);
    }
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
        return keypad ? 0x60 + (qtKey - Qt::Key_0) : '0' + (qtKey - Qt::Key_0);
    }
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) {
        return 0x70 + (qtKey - Qt::Key_F1);
    }
    switch (qtKey) {
    case Qt::Key_Space:
        return 0x20;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return 0x0D;
    case Qt::Key_Escape:
        return 0x1B;
    case Qt::Key_Backspace:
        return 0x08;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return 0x09;
    case Qt::Key_CapsLock:
        return 0x14;
    case Qt::Key_Shift:
        return 0xA0;
    case Qt::Key_Control:
        return 0xA2;
    case Qt::Key_Alt:
        return 0xA4;
    case Qt::Key_AltGr:
        return 0xA5;
    case Qt::Key_Meta:
        return 0x5B;
    case Qt::Key_Menu:
        return 0x5D;
    case Qt::Key_Up:
        return 0x26;
    case Qt::Key_Down:
        return 0x28;
    case Qt::Key_Left:
        return 0x25;
    case Qt::Key_Right:
        return 0x27;
    case Qt::Key_Insert:
        return 0x2D;
    case Qt::Key_Delete:
        return 0x2E;
    case Qt::Key_Home:
        return 0x24;
    case Qt::Key_End:
        return 0x23;
    case Qt::Key_PageUp:
        return 0x21;
    case Qt::Key_PageDown:
        return 0x22;
    case Qt::Key_Pause:
        return 0x13;
    case Qt::Key_ScrollLock:
        return 0x91;
    case Qt::Key_NumLock:
        return 0x90;
    case Qt::Key_Print:
        return 0x2C;
    case Qt::Key_Plus:
        return keypad ? 0x6B : 0xBB;
    case Qt::Key_Equal:
        return 0xBB;
    case Qt::Key_Minus:
        return keypad ? 0x6D : 0xBD;
    case Qt::Key_Asterisk:
        return keypad ? 0x6A : 0;
    case Qt::Key_Slash:
        return keypad ? 0x6F : 0xBF;
    case Qt::Key_Period:
        return keypad ? 0x6E : 0xBE;
    case Qt::Key_Comma:
        return keypad ? 0x6E : 0xBC;
    case Qt::Key_Semicolon:
        return 0xBA;
    case Qt::Key_QuoteLeft:
        return 0xC0;
    case Qt::Key_BracketLeft:
        return 0xDB;
    case Qt::Key_Backslash:
        return 0xDC;
    case Qt::Key_BracketRight:
        return 0xDD;
    case Qt::Key_Apostrophe:
        return 0xDE;
    case Qt::Key_Less:
    case Qt::Key_Greater:
    case Qt::Key_Bar:
        return 0xE2;
    default:
        return 0;
    }
}

constexpr quint16 kPadButtonBits[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0010, 0x0020,
                                      0x0001, 0x0002, 0x0004, 0x0008, 0x0040, 0x0080};
constexpr const char* kPadButtonTokens[] = {"pad:a",       "pad:b",         "pad:x",         "pad:y",
                                            "pad:lb",      "pad:rb",        "pad:start",     "pad:back",
                                            "pad:dpad_up", "pad:dpad_down", "pad:dpad_left", "pad:dpad_right",
                                            "pad:ls_click", "pad:rs_click"};
static_assert(std::size(kPadButtonBits) == std::size(kPadButtonTokens));

}  // namespace

QString BindDeviceKeyPrefix(BindDevice device) {
    return device == BindDevice::Pad ? QStringLiteral("pad_") : QStringLiteral("kbm_");
}

std::span<const ControlInfo> XboxControls() noexcept {
    return kControls;
}

const ControlInfo* FindControl(QStringView id) {
    for (const ControlInfo& c : kControls) {
        if (id.compare(QLatin1String(c.id), Qt::CaseInsensitive) == 0) {
            return &c;
        }
    }
    return nullptr;
}

QString ControlGroupTitle(ControlGroup group) {
    switch (group) {
    case ControlGroup::FaceButtons:
        return T("bind.group.face");
    case ControlGroup::Shoulders:
        return T("bind.group.shoulders");
    case ControlGroup::System:
        return T("bind.group.system");
    case ControlGroup::DPad:
        return T("bind.group.dpad");
    case ControlGroup::LeftStick:
        return T("bind.group.left_stick");
    case ControlGroup::RightStick:
        return T("bind.group.right_stick");
    case ControlGroup::Other:
        return T("bind.group.other");
    }
    return QString();
}

QString ControlDisplayName(QStringView id) {
    if (const ControlInfo* c = FindControl(id)) {
        return T(c->nameId);
    }
    return id.toString();
}

std::optional<BindingKeyInfo> ParseBindingKey(const KeyDef& key) {
    const bool declared = key.widget == UiWidget::Binding;
    if (!declared && (key.type != KeyType::String || key.section != QLatin1String("input"))) {
        return std::nullopt;
    }
    for (BindDevice device : {BindDevice::Pad, BindDevice::KeyboardMouse}) {
        const QString prefix = BindDeviceKeyPrefix(device);
        if (key.name.startsWith(prefix)) {
            const QString control = key.name.mid(prefix.size());
            if (control.isEmpty() || (!declared && FindControl(control) == nullptr)) {
                return std::nullopt;
            }
            return BindingKeyInfo{device, control};
        }
    }
    return std::nullopt;
}

BindingLabel SplitBindingLabel(const KeyDef& key) {
    BindingLabel out;
    const auto info = ParseBindingKey(key);
    const QString control = info ? info->control : key.name;
    const QString label = key.label.text().trimmed();
    for (const QString& dash : {QStringLiteral(" – "), QStringLiteral(" - ")}) {
        const qsizetype at = label.indexOf(dash);
        if (at > 0) {
            out.control = label.left(at).trimmed();
            out.meaning = label.mid(at + dash.size()).trimmed();
            return out;
        }
    }
    out.control = ControlDisplayName(control);
    out.meaning = label == out.control ? QString() : label;
    return out;
}

std::optional<QString> CanonicalToken(QStringView token) {
    const QString t = token.trimmed().toString().toLower();
    const qsizetype colon = t.indexOf(QLatin1Char(':'));
    if (colon <= 0) {
        return std::nullopt;
    }
    // Blanks around the device and the name are ignored, as in inp::ParseToken ("key : a" == "key:a").
    const QStringView device = QStringView(t).left(colon).trimmed();
    const QStringView name = QStringView(t).mid(colon + 1).trimmed();
    if (device == QLatin1String("pad")) {
        return FindName(kPadNames, name) != nullptr ? std::optional<QString>(QStringLiteral("pad:") + name)
                                                    : std::nullopt;
    }
    if (device == QLatin1String("mouse")) {
        return FindName(kMouseNames, name) != nullptr ? std::optional<QString>(QStringLiteral("mouse:") + name)
                                                      : std::nullopt;
    }
    if (device == QLatin1String("key")) {
        const int vk = KeyNameToVk(name);
        if (vk == 0) {
            return std::nullopt;
        }
        // vk_xx of a named key is accepted, the canonical form is the name (key:vk_20 == key:space).
        return QStringLiteral("key:") + VkToKeyName(vk);
    }
    return std::nullopt;
}

std::optional<BindDevice> TokenDevice(QStringView token) {
    if (token.startsWith(QLatin1String("pad:"), Qt::CaseInsensitive)) {
        return BindDevice::Pad;
    }
    if (token.startsWith(QLatin1String("key:"), Qt::CaseInsensitive) ||
        token.startsWith(QLatin1String("mouse:"), Qt::CaseInsensitive)) {
        return BindDevice::KeyboardMouse;
    }
    return std::nullopt;
}

int KeyTokenVk(QStringView token) {
    const auto canonical = CanonicalToken(token);
    if (!canonical || !canonical->startsWith(QLatin1String("key:"))) {
        return 0;
    }
    return KeyNameToVk(QStringView(*canonical).mid(4));
}

QString TokenDisplayText(const QString& token) {
    const auto canonical = CanonicalToken(token);
    if (!canonical) {
        return token;
    }
    const QStringView name = QStringView(*canonical).mid(canonical->indexOf(QLatin1Char(':')) + 1);
    if (canonical->startsWith(QLatin1String("pad:"))) {
        return T(FindName(kPadNames, name)->displayId);
    }
    if (canonical->startsWith(QLatin1String("mouse:"))) {
        return T(FindName(kMouseNames, name)->displayId);
    }
    const int vk = KeyNameToVk(name);
    if (name.size() == 1) {
        return name.toString().toUpper();
    }
    if (vk >= 0x70 && vk <= 0x87) {
        return name.toString().toUpper();
    }
    if (vk >= 0x60 && vk <= 0x69) {
        return T("bind.key.numpad").arg(vk - 0x60);
    }
    if (const KeyName* k = FindNamedKey(name)) {
        if (k->displayId != nullptr) {
            return T(k->displayId);
        }
        const QString windows = WindowsKeyName(vk);
        return windows.isEmpty() ? QLatin1String(k->symbol) : windows;
    }
    const QString windows = WindowsKeyName(vk);
    return windows.isEmpty() ? T("bind.key.vk").arg(vk, 2, 16, QLatin1Char('0')) : windows;
}

QStringList AllTokens(BindDevice device) {
    QStringList out;
    if (device == BindDevice::Pad) {
        for (const PadName& p : kPadNames) {
            out.append(QStringLiteral("pad:") + QLatin1String(p.name));
        }
        return out;
    }
    for (int vk = 0x01; vk <= 0xFE; ++vk) {
        const QString name = VkToKeyName(vk);
        if (!name.startsWith(QLatin1String("vk_"))) {
            out.append(QStringLiteral("key:") + name);
        }
    }
    for (const PadName& m : kMouseNames) {
        out.append(QStringLiteral("mouse:") + QLatin1String(m.name));
    }
    return out;
}

BindingParse ParseBindingList(QStringView text, BindDevice device) {
    BindingParse out;
    for (QStringView item : text.split(QLatin1Char(','))) {
        item = item.trimmed();
        if (item.isEmpty()) {
            continue;
        }
        ++out.itemCount;
        const auto token = CanonicalToken(item);
        if (!token) {
            out.invalid.append(item.toString());
            continue;
        }
        if (TokenDevice(*token) != device) {
            out.wrongDevice.append(*token);
            continue;
        }
        if (!out.tokens.contains(*token)) {
            out.tokens.append(*token);
        }
    }
    return out;
}

QString FormatBindingList(const QStringList& tokens) {
    QStringList unique;
    for (const QString& t : tokens) {
        const QString c = CanonicalToken(t).value_or(t.trimmed().toLower());
        if (!c.isEmpty() && !unique.contains(c)) {
            unique.append(c);
        }
    }
    return unique.join(QStringLiteral(", "));
}

bool BindingListsEquivalent(QStringView a, QStringView b, BindDevice device) {
    const BindingParse pa = ParseBindingList(a, device);
    const BindingParse pb = ParseBindingList(b, device);
    if (!pa.ok() || !pb.ok()) {
        return a.trimmed() == b.trimmed();
    }
    const QSet<QString> sa(pa.tokens.begin(), pa.tokens.end());
    const QSet<QString> sb(pb.tokens.begin(), pb.tokens.end());
    return sa == sb;
}

QString BindingProblemText(const BindingParse& parse, BindDevice device) {
    if (!parse.invalid.isEmpty()) {
        return T("bind.err.invalid").arg(parse.invalid.join(QStringLiteral(", ")));
    }
    if (!parse.wrongDevice.isEmpty()) {
        return (device == BindDevice::Pad ? T("bind.err.not_pad") : T("bind.err.not_kbm"))
            .arg(parse.wrongDevice.join(QStringLiteral(", ")));
    }
    if (parse.tooMany()) {
        return T("bind.err.too_many").arg(kMaxBindingTokens);
    }
    return QString();
}

QHash<QString, QStringList> FindConflicts(const QList<BindingRow>& rows) {
    QHash<QString, QStringList> users;
    for (const BindingRow& row : rows) {
        for (const QString& token : row.tokens) {
            QStringList& list = users[token];
            if (!list.contains(row.control)) {
                list.append(row.control);
            }
        }
    }
    users.removeIf([](const QHash<QString, QStringList>::iterator it) { return it.value().size() < 2; });
    return users;
}

QStringList ControlsForTokens(const QSet<QString>& active, const QList<BindingRow>& rows) {
    QStringList out;
    for (const BindingRow& row : rows) {
        for (const QString& token : row.tokens) {
            if (active.contains(token)) {
                out.append(row.control);
                break;
            }
        }
    }
    return out;
}

std::optional<QString> KeyTokenFromEvent(int qtKey, Qt::KeyboardModifiers modifiers, quint32 nativeVk,
                                         quint32 nativeScan) {
    int vk = static_cast<int>(nativeVk);
    if (vk == kVkShift) {
        vk = (nativeScan & 0xFF) == 0x36 ? 0xA1 : 0xA0;
    } else if (vk == kVkControl) {
        vk = (nativeScan & 0x100) != 0 ? 0xA3 : 0xA2;
    } else if (vk == kVkMenu) {
        vk = (nativeScan & 0x100) != 0 ? 0xA5 : 0xA4;
    }
    if (vk <= 0 || vk > 0xFE) {
        vk = VkFromQtKey(qtKey, modifiers);
    }
    // Mouse buttons have virtual-key codes too (0x01, 0x02, 0x04-0x06) - never as keys.
    if (vk <= 0 || vk == 0x01 || vk == 0x02 || (vk >= 0x04 && vk <= 0x06)) {
        return std::nullopt;
    }
    return QStringLiteral("key:") + VkToKeyName(vk);
}

std::optional<QString> MouseButtonToken(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton:
        return QStringLiteral("mouse:left");
    case Qt::RightButton:
        return QStringLiteral("mouse:right");
    case Qt::MiddleButton:
        return QStringLiteral("mouse:middle");
    case Qt::BackButton:
        return QStringLiteral("mouse:x1");
    case Qt::ForwardButton:
        return QStringLiteral("mouse:x2");
    default:
        return std::nullopt;
    }
}

QString WheelToken(int angleDeltaY) {
    if (angleDeltaY > 0) {
        return QStringLiteral("mouse:wheel_up");
    }
    if (angleDeltaY < 0) {
        return QStringLiteral("mouse:wheel_down");
    }
    return QString();
}

QSet<QString> ActivePadTokens(const PadState& state, double threshold) {
    QSet<QString> out;
    if (!state.connected) {
        return out;
    }
    for (size_t i = 0; i < std::size(kPadButtonBits); ++i) {
        if ((state.buttons & kPadButtonBits[i]) != 0) {
            out.insert(QLatin1String(kPadButtonTokens[i]));
        }
    }
    const double triggerLimit = threshold * 255.0;
    if (state.leftTrigger > triggerLimit) {
        out.insert(QStringLiteral("pad:lt"));
    }
    if (state.rightTrigger > triggerLimit) {
        out.insert(QStringLiteral("pad:rt"));
    }
    const double axisLimit = threshold * 32767.0;
    auto axis = [&out, axisLimit](qint16 v, const char* positive, const char* negative) {
        if (v > axisLimit) {
            out.insert(QLatin1String(positive));
        } else if (v < -axisLimit) {
            out.insert(QLatin1String(negative));
        }
    };
    axis(state.lx, "pad:lx+", "pad:lx-");
    axis(state.ly, "pad:ly+", "pad:ly-");
    axis(state.rx, "pad:rx+", "pad:rx-");
    axis(state.ry, "pad:ry+", "pad:ry-");
    return out;
}

namespace {

// Value 0..1 of a pad token (host: PadTokenValue / HalfAxis).
double PadTokenValue(QStringView token, const PadState& p) {
    for (size_t i = 0; i < std::size(kPadButtonBits); ++i) {
        if (token == QLatin1String(kPadButtonTokens[i])) {
            return (p.buttons & kPadButtonBits[i]) != 0 ? 1.0 : 0.0;
        }
    }
    if (token == QLatin1String("pad:lt")) {
        return p.leftTrigger / 255.0;
    }
    if (token == QLatin1String("pad:rt")) {
        return p.rightTrigger / 255.0;
    }
    auto half = [](qint16 v, bool negative) {
        if (negative) {
            return v < 0 ? std::min(1.0, -static_cast<double>(v) / 32768.0) : 0.0;
        }
        return v > 0 ? std::min(1.0, static_cast<double>(v) / 32767.0) : 0.0;
    };
    static constexpr struct {
        const char* token;
        int axis;
        bool negative;
    } kHalf[] = {{"pad:lx+", 0, false}, {"pad:lx-", 0, true}, {"pad:ly+", 1, false}, {"pad:ly-", 1, true},
                 {"pad:rx+", 2, false}, {"pad:rx-", 2, true}, {"pad:ry+", 3, false}, {"pad:ry-", 3, true}};
    const qint16 axes[4] = {p.lx, p.ly, p.rx, p.ry};
    for (const auto& h : kHalf) {
        if (token == QLatin1String(h.token)) {
            return half(axes[h.axis], h.negative);
        }
    }
    return 0.0;
}

}  // namespace

QStringList GameControlsForPad(const PadState& state, const QList<BindingRow>& rows, int deadzonePercent) {
    QStringList out;
    if (!state.connected) {
        return out;
    }
    PadState p = state;
    if (deadzonePercent > 0) {
        const double dz = std::min(deadzonePercent, 90) / 100.0;
        auto cut = [dz](qint16& x, qint16& y) {
            const double fx = x / 32768.0;
            const double fy = y / 32768.0;
            if (std::sqrt(fx * fx + fy * fy) < dz) {
                x = 0;
                y = 0;
            }
        };
        cut(p.lx, p.ly);
        cut(p.rx, p.ry);
    }
    QHash<QString, double> value;
    for (const BindingRow& row : rows) {
        double v = 0.0;
        for (const QString& token : row.tokens) {
            v = std::max(v, PadTokenValue(token, p));
        }
        value.insert(row.control, v);
    }
    // Stick directions: the composed stick per side (host Compose: right - left, up - down).
    auto stickOn = [&value](const QString& pre, QStringView d) {
        const double dx = value.value(pre + QStringLiteral("right")) - value.value(pre + QStringLiteral("left"));
        const double dy = value.value(pre + QStringLiteral("up")) - value.value(pre + QStringLiteral("down"));
        const double radius = std::sqrt(dx * dx + dy * dy);
        if (radius <= kGameStickDeadzone) {
            return false;
        }
        const double limit = radius * 0.38268343;  // sin 22.5 deg
        if (d == QLatin1String("up")) {
            return dy >= limit;
        }
        if (d == QLatin1String("down")) {
            return -dy >= limit;
        }
        if (d == QLatin1String("right")) {
            return dx >= limit;
        }
        return -dx >= limit;
    };
    for (const BindingRow& row : rows) {
        const QString& c = row.control;
        const double v = value.value(c);
        bool on = false;
        if (c == QLatin1String("a") || c == QLatin1String("b") || c == QLatin1String("x") || c == QLatin1String("y") ||
            c == QLatin1String("white") || c == QLatin1String("black") || c == QLatin1String("lt") ||
            c == QLatin1String("rt")) {
            on = std::lround(255.0 * std::clamp(v, 0.0, 1.0)) >= kGameAnalogThreshold;
        } else if ((c.startsWith(QLatin1String("ls_")) || c.startsWith(QLatin1String("rs_"))) &&
                   !c.endsWith(QLatin1String("_click"))) {
            on = stickOn(c.left(3), QStringView(c).mid(3));
        } else {
            on = v >= 0.5;
        }
        if (on) {
            out.append(c);
        }
    }
    return out;
}

QStringList SortedPadTokens(const QSet<QString>& tokens) {
    QStringList out;
    for (const PadName& p : kPadNames) {
        const QString t = QStringLiteral("pad:") + QLatin1String(p.name);
        if (tokens.contains(t)) {
            out.append(t);
        }
    }
    return out;
}

}  // namespace fl
