// Controls configuration: the binding strings of the [input] keys (src/INTERFACES.md 4.26, the contract with the
// game's input layer) and the helpers of the "Steuerung" tab.
//
//   [input] pad_<control> = pad:a              (Xbox controller / XInput)
//           kbm_<control> = key:enter, mouse:left   (keyboard and mouse)
//
// A binding list is a ','-separated list of tokens "<device>:<name>" (blanks around tokens ignored, case-insensitive,
// blanks around ':' too, empty = not bound, at most kMaxBindingTokens different tokens; duplicates are ignored and not
// counted). The launcher writes lower case, ", "-separated, without duplicates. pad_* keys take only pad: tokens,
// kbm_* keys only key: and mouse: tokens. Controls are Xbox controls (the game's action table is fixed,
// analysis/loader/m4_input.md 5.1/8.6), not game actions. The token language is the host's (inp::ParseToken).
#pragma once

#include "core/config_schema.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>
#include <span>

namespace fl {

enum class BindDevice { Pad, KeyboardMouse };

inline constexpr int kMaxBindingTokens = 4;

QString BindDeviceKeyPrefix(BindDevice device);  // "pad_" / "kbm_"

// Visual groups of the binding tables (in this order).
enum class ControlGroup { FaceButtons, Shoulders, System, DPad, LeftStick, RightStick, Other };

struct ControlInfo {
    const char* id;          // "a", "white", "ls_up", ... (the key name without the device prefix)
    ControlGroup group;
    const char* nameId;      // translation id of the Xbox control name ("bind.ctl.a")
};

// The 24 Xbox controls of the contract, in the order of the binding tables.
std::span<const ControlInfo> XboxControls() noexcept;
const ControlInfo* FindControl(QStringView id);
QString ControlGroupTitle(ControlGroup group);  // localized
// Localized Xbox control name ("A", "Linker Stick hoch"); the id itself for unknown controls.
QString ControlDisplayName(QStringView id);

// A schema key that holds a binding list: ui.widget "binding", or a string key [input] pad_<control> /
// kbm_<control> with a known control (schemas that do not name the widget).
struct BindingKeyInfo {
    BindDevice device = BindDevice::Pad;
    QString control;
};
std::optional<BindingKeyInfo> ParseBindingKey(const KeyDef& key);
inline bool IsBindingKey(const KeyDef& key) {
    return ParseBindingKey(key).has_value();
}

// The schema label "<control> – <meaning>" split at the first en dash (" – ", also " - "): {control, meaning}.
// Without a dash: {ControlDisplayName(control), label}.
struct BindingLabel {
    QString control;
    QString meaning;
};
BindingLabel SplitBindingLabel(const KeyDef& key);

// ---- tokens ------------------------------------------------------------------------------------------------
// Canonical (lower-case) form of one token, or nullopt if it is not a token of the contract.
std::optional<QString> CanonicalToken(QStringView token);
// Device a canonical token belongs to (pad: -> Pad, key:/mouse: -> KeyboardMouse).
std::optional<BindDevice> TokenDevice(QStringView token);
// Localized display text ("A", "Linker Stick ↑", "Leertaste", "Linke Maustaste"). OEM keys and vk_xx use the
// Windows key name of the current keyboard layout when available.
QString TokenDisplayText(const QString& token);
// The canonical names of all tokens of a device (tests, completion).
QStringList AllTokens(BindDevice device);

struct BindingParse {
    QStringList tokens;         // canonical, without duplicates, in the order of the text
    QStringList invalid;        // items that are not tokens (as written)
    QStringList wrongDevice;    // valid tokens of the other device
    int itemCount = 0;          // non-empty items incl. duplicates
    // Like the host (inp::ParseBindingList): duplicates are ignored and do not count towards the limit.
    bool tooMany() const { return tokens.size() > kMaxBindingTokens; }
    bool ok() const { return invalid.isEmpty() && wrongDevice.isEmpty() && !tooMany(); }
};
BindingParse ParseBindingList(QStringView text, BindDevice device);
// "key:enter, mouse:left" (canonical tokens, duplicates removed).
QString FormatBindingList(const QStringList& tokens);
// Same set of tokens (order and case do not matter). Unparseable texts compare as trimmed strings.
bool BindingListsEquivalent(QStringView a, QStringView b, BindDevice device);
// Localized error text of a parse that is not ok() (empty if ok).
QString BindingProblemText(const BindingParse& parse, BindDevice device);

// ---- conflicts ---------------------------------------------------------------------------------------------
struct BindingRow {
    QString control;
    QStringList tokens;
};
// token -> controls that use it, only for tokens bound to two or more controls.
QHash<QString, QStringList> FindConflicts(const QList<BindingRow>& rows);
// Controls whose binding list contains one of `active` (live test view: which Xbox controls a pad state presses).
QStringList ControlsForTokens(const QSet<QString>& active, const QList<BindingRow>& rows);

// ---- capture -----------------------------------------------------------------------------------------------
// Token of a key press: the Windows virtual-key code (`nativeVk`, `nativeScan` with the extended bit 0x100 as Qt
// reports it) when known, else the Qt key (offscreen platform, tests). nullopt for keys without a token.
std::optional<QString> KeyTokenFromEvent(int qtKey, Qt::KeyboardModifiers modifiers, quint32 nativeVk,
                                         quint32 nativeScan);
std::optional<QString> MouseButtonToken(Qt::MouseButton button);
QString WheelToken(int angleDeltaY);  // "mouse:wheel_up" for > 0, "mouse:wheel_down" for < 0, else empty
// Windows virtual-key code of a key token (key:space -> 0x20, key:vk_41 -> 0x41), 0 if none.
int KeyTokenVk(QStringView token);

// XInput gamepad state (XINPUT_GAMEPAD layout) and the pad tokens it presses. Triggers and half axes count from
// `threshold` (0..1) of their range.
struct PadState {
    bool connected = false;
    quint32 packet = 0;
    quint16 buttons = 0;
    quint8 leftTrigger = 0;
    quint8 rightTrigger = 0;
    qint16 lx = 0;
    qint16 ly = 0;
    qint16 rx = 0;
    qint16 ry = 0;
};
inline constexpr int kPadSlots = 4;
QSet<QString> ActivePadTokens(const PadState& state, double threshold = 0.5);
// Pad tokens in a stable display order (buttons, triggers, d-pad, sticks).
QStringList SortedPadTokens(const QSet<QString>& tokens);

// The Xbox controls the game receives from `state` through the pad bindings `rows`, computed like the host
// (src/host/input/bindings.cpp MapPad; review M4 finding 10): [input] pad_deadzone (percent, radial) first, then per
// control the largest token value; analog-button controls (A B X Y White Black LT RT) count from the game's channel
// threshold 30/255, digital controls (START BACK d-pad stick clicks) from 0.5, stick directions once the composed
// stick leaves the game's dead zone 0.4 (8 directions: a component of at least sin 22.5 deg of the radius).
QStringList GameControlsForPad(const PadState& state, const QList<BindingRow>& rows, int deadzonePercent);
inline constexpr int kGameAnalogThreshold = 30;   // m4_input.md 3.2, inp::kAnalogPressThreshold
inline constexpr double kGameStickDeadzone = 0.4;  // avi.md, m4_input.md 3.2

}  // namespace fl
