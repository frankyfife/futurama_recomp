#include "ui/controls_panel.h"

#include "core/l10n.h"
#include "core/validation.h"
#include "ui/app_style.h"
#include "ui/binding_capture.h"
#include "ui/pad_poller.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <optional>

namespace fl::ui {
namespace {

QLabel* Muted(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, MutedTextColor(l));
    l->setPalette(p);
    return l;
}

void SetLabelColor(QLabel* label, const QColor& color) {
    QPalette p = label->palette();
    p.setColor(QPalette::WindowText, color);
    label->setPalette(p);
}

QString TokensText(const QStringList& tokens) {
    QStringList names;
    for (const QString& t : tokens) {
        names.append(TokenDisplayText(t));
    }
    return names.join(QStringLiteral(", "));
}

bool BoolSetting(const SettingsModel& model, const char* section, const char* name, bool fallback) {
    const KeyDef* key = model.schema().find(QLatin1String(section), QLatin1String(name));
    return key != nullptr ? ParseBoolText(model.value(*key)).value_or(fallback) : fallback;
}

int IntSetting(const SettingsModel& model, const char* section, const char* name, int fallback) {
    const KeyDef* key = model.schema().find(QLatin1String(section), QLatin1String(name));
    if (key == nullptr) {
        return fallback;
    }
    return static_cast<int>(std::lround(ParseFloatText(model.value(*key)).value_or(fallback)));
}

// Controls a player cannot do without (review M4 finding 14): confirm, pause, walking.
bool EssentialControl(QStringView control) {
    return control == QLatin1String("a") || control == QLatin1String("start") || control == QLatin1String("ls_up") ||
           control == QLatin1String("ls_down") || control == QLatin1String("ls_left") ||
           control == QLatin1String("ls_right");
}

// Binding keys of `device` (visible ones): grouped by control group (face buttons, shoulders, START/BACK, d-pad,
// left stick, right stick, others), within a group in the schema's order.
QList<const KeyDef*> BindingKeys(const ConfigSchema& schema, BindDevice device,
                                 const std::function<bool(const KeyDef&)>& visible) {
    QList<const KeyDef*> out;
    for (const KeyDef* key : schema.allKeys()) {
        const auto info = ParseBindingKey(*key);
        if (info && info->device == device && (!visible || visible(*key))) {
            out.append(key);
        }
    }
    auto group = [](const KeyDef* key) {
        const ControlInfo* info = FindControl(ParseBindingKey(*key)->control);
        return static_cast<int>(info != nullptr ? info->group : ControlGroup::Other);
    };
    std::stable_sort(out.begin(), out.end(), [&group](const KeyDef* a, const KeyDef* b) {
        if (group(a) != group(b)) {
            return group(a) < group(b);
        }
        if (a->order != b->order) {
            return a->order < b->order;
        }
        return a->schemaIndex < b->schemaIndex;
    });
    return out;
}

}  // namespace

// ================================================================================================================
// BindingsPanel

BindingsPanel::BindingsPanel(SettingsModel& model, PadPoller* poller, std::function<bool(const KeyDef&)> visible,
                             QWidget* parent)
    : QWidget(parent), model_(model), poller_(poller) {
    setObjectName(QStringLiteral("bindingsPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    const int unit = fontMetrics().height();
    layout->setSpacing(unit / 2);

    for (BindDevice device : {BindDevice::Pad, BindDevice::KeyboardMouse}) {
        for (const KeyDef* key : BindingKeys(model_.schema(), device, visible)) {
            Row row;
            row.key = key;
            row.device = device;
            row.control = ParseBindingKey(*key)->control;
            rows_.append(row);
        }
    }
    if (rows_.isEmpty()) {
        setVisible(false);
        return;
    }
    auto* box = new QGroupBox(T("bind.title"), this);
    auto* boxLayout = new QVBoxLayout(box);
    boxLayout->addWidget(Muted(T("bind.intro"), box));
    deviceTabs_ = new QTabWidget(box);
    deviceTabs_->setObjectName(QStringLiteral("bindingDevices"));
    deviceTabs_->addTab(buildDevicePage(BindDevice::Pad), T("bind.device.pad"));
    deviceTabs_->addTab(buildDevicePage(BindDevice::KeyboardMouse), T("bind.device.kbm"));
    boxLayout->addWidget(deviceTabs_);
    layout->addWidget(box);
    refreshFromModel();
}

int BindingsPanel::rowCount(BindDevice device) const {
    return static_cast<int>(std::count_if(rows_.begin(), rows_.end(), [device](const Row& r) { return r.device == device; }));
}

QWidget* BindingsPanel::buildDevicePage(BindDevice device) {
    auto* page = new QWidget(this);
    auto* outer = new QVBoxLayout(page);
    const int unit = fontMetrics().height();
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(unit);
    grid->setVerticalSpacing(unit / 4);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    int r = 0;
    auto header = [page](const QString& text) {
        auto* l = new QLabel(text, page);
        QFont f = l->font();
        f.setBold(true);
        l->setFont(f);
        return l;
    };
    grid->addWidget(header(T("bind.col.control")), r, 0);
    grid->addWidget(header(T("bind.col.meaning")), r, 1);
    grid->addWidget(header(device == BindDevice::Pad ? T("bind.col.pad") : T("bind.col.kbm")), r, 2, 1, 4);
    ++r;
    std::optional<ControlGroup> lastGroup;
    for (qsizetype i = 0; i < rows_.size(); ++i) {
        Row& row = rows_[i];
        if (row.device != device) {
            continue;
        }
        const ControlInfo* info = FindControl(row.control);
        const ControlGroup group = info != nullptr ? info->group : ControlGroup::Other;
        if (!lastGroup || *lastGroup != group) {
            auto* g = Muted(ControlGroupTitle(group), page);
            g->setContentsMargins(0, unit / 3, 0, 0);
            grid->addWidget(g, r++, 0, 1, 6);
            lastGroup = group;
        }
        const BindingLabel label = SplitBindingLabel(*row.key);
        const QString full = row.key->fullName();
        row.name = new QLabel(label.control, page);
        row.meaning = new QLabel(label.meaning, page);
        row.meaning->setWordWrap(true);
        row.binding = new QPushButton(page);
        row.binding->setObjectName(QStringLiteral("bind:") + full);
        row.binding->setMinimumWidth(unit * 12);
        row.binding->setAutoDefault(false);
        row.binding->setToolTip(T("bind.change.tip"));
        row.add = new QToolButton(page);
        row.add->setObjectName(QStringLiteral("bindadd:") + full);
        row.add->setText(QStringLiteral("+"));
        row.add->setToolTip(T("bind.add.tip"));
        row.clear = new QToolButton(page);
        row.clear->setObjectName(QStringLiteral("bindclear:") + full);
        row.clear->setText(QStringLiteral("✕"));
        row.clear->setToolTip(T("bind.clear.tip"));
        row.reset = new QToolButton(page);
        row.reset->setObjectName(QStringLiteral("bindreset:") + full);
        row.reset->setText(QStringLiteral("↺"));
        row.reset->setAutoRaise(true);
        QSizePolicy keep = row.reset->sizePolicy();
        keep.setRetainSizeWhenHidden(true);
        row.reset->setSizePolicy(keep);
        row.message = new QLabel(page);
        row.message->setObjectName(QStringLiteral("bindmsg:") + full);
        row.message->setWordWrap(true);
        row.message->setVisible(false);
        const QString help = row.key->help.text();
        const QString tip = QStringLiteral("<qt>") + (help.isEmpty() ? QString() : help.toHtmlEscaped() + QStringLiteral("<br><br>")) +
                            QStringLiteral("<span style=\"font-family:Consolas,monospace\">[%1] %2</span></qt>")
                                .arg(row.key->section, row.key->name);
        row.name->setToolTip(tip);
        row.meaning->setToolTip(tip);
        grid->addWidget(row.name, r, 0, Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget(row.meaning, r, 1, Qt::AlignVCenter);
        grid->addWidget(row.binding, r, 2);
        grid->addWidget(row.add, r, 3);
        grid->addWidget(row.clear, r, 4);
        grid->addWidget(row.reset, r, 5);
        grid->addWidget(row.message, r + 1, 1, 1, 5);
        r += 2;
        const KeyDef* key = row.key;
        connect(row.binding, &QPushButton::clicked, this, [this, key] { openCapture(*key, false); });
        connect(row.add, &QToolButton::clicked, this, [this, key] { openCapture(*key, true); });
        connect(row.clear, &QToolButton::clicked, this, [this, key] {
            model_.setValue(*key, QString());
            changed();
        });
        connect(row.reset, &QToolButton::clicked, this, [this, key] {
            model_.resetToDefault(*key);
            changed();
        });
    }
    outer->addLayout(grid);
    auto* footer = new QHBoxLayout();
    const size_t index = device == BindDevice::Pad ? 0 : 1;
    summary_[index] = new QLabel(page);
    summary_[index]->setObjectName(device == BindDevice::Pad ? QStringLiteral("bindSummaryPad")
                                                             : QStringLiteral("bindSummaryKbm"));
    summary_[index]->setWordWrap(true);
    footer->addWidget(summary_[index], 1);
    auto* reset = new QPushButton(device == BindDevice::Pad ? T("bind.reset_pad") : T("bind.reset_kbm"), page);
    reset->setObjectName(device == BindDevice::Pad ? QStringLiteral("bindResetPad") : QStringLiteral("bindResetKbm"));
    reset->setAutoDefault(false);
    connect(reset, &QPushButton::clicked, this, [this, device] {
        // Several rows at once: ask first (review M4 finding 14).
        const int changed = changedRowCount(device);
        if (changed == 0) {
            return;
        }
        if (changed > 1 &&
            !ConfirmChange(this,
                           (device == BindDevice::Pad ? T("bind.reset_pad.confirm") : T("bind.reset_kbm.confirm"))
                               .arg(changed),
                           T("bind.reset.confirm_button"))) {
            return;
        }
        resetDevice(device);
    });
    footer->addWidget(reset, 0, Qt::AlignTop);
    outer->addLayout(footer);
    return page;
}

QList<BindingRow> BindingsPanel::bindingRows(BindDevice device) const {
    QList<BindingRow> out;
    for (const Row& row : rows_) {
        if (row.device == device) {
            out.append(BindingRow{row.control, ParseBindingList(model_.value(*row.key), device).tokens});
        }
    }
    return out;
}

QHash<QString, QStringList> BindingsPanel::conflictsOf(BindDevice device) const {
    return FindConflicts(bindingRows(device));
}

QString BindingsPanel::conflictText(const KeyDef& key) const {
    const auto info = ParseBindingKey(key);
    if (!info) {
        return QString();
    }
    const QHash<QString, QStringList> conflicts = conflictsOf(info->device);
    QStringList parts;
    for (const QString& token : ParseBindingList(model_.value(key), info->device).tokens) {
        const auto it = conflicts.constFind(token);
        if (it == conflicts.constEnd()) {
            continue;
        }
        QStringList others;
        for (const QString& control : it.value()) {
            if (control != info->control) {
                others.append(ControlDisplayName(control));
            }
        }
        parts.append(T("bind.conflict").arg(TokenDisplayText(token), others.join(QStringLiteral(", "))));
    }
    return parts.join(QLatin1Char(' '));
}

void BindingsPanel::updateRow(Row& row, const QHash<QString, QStringList>& conflicts) {
    const QString value = model_.value(*row.key);
    const BindingParse parse = ParseBindingList(value, row.device);
    const ValidationResult result = model_.validate(*row.key);
    QString text;
    if (!parse.ok()) {
        text = value.trimmed() + T("editor.invalid_suffix");
    } else if (parse.tokens.isEmpty()) {
        text = T("bind.unbound");
    } else {
        text = TokensText(parse.tokens);
    }
    row.binding->setText(text);
    // Plain control name (without the pending marker " *"); the change is a separate description.
    row.binding->setAccessibleName(SplitBindingLabel(*row.key).control + QStringLiteral(": ") + text);
    row.binding->setAccessibleDescription(model_.isPending(*row.key) ? T("bind.pending") : QString());
    row.add->setEnabled(parse.ok() && parse.tokens.size() < kMaxBindingTokens);
    row.clear->setEnabled(!value.trimmed().isEmpty());
    const bool atDefault = model_.isAtDefault(*row.key);
    row.reset->setVisible(!atDefault);
    const QString def = row.key->defaultText.trimmed().isEmpty()
                            ? T("bind.unbound")
                            : TokensText(ParseBindingList(row.key->defaultText, row.device).tokens);
    row.reset->setToolTip(T("tab.reset_key.tip").arg(def));
    row.name->setText(SplitBindingLabel(*row.key).control + (model_.isPending(*row.key) ? QStringLiteral(" *") : QString()));
    // Keyboard START with [input] menu_escape: in the frontend menus it goes back (review M4 finding 13).
    if (row.device == BindDevice::KeyboardMouse && row.control == QLatin1String("start")) {
        const QString meaning = SplitBindingLabel(*row.key).meaning;
        row.meaning->setText(BoolSetting(model_, "input", "menu_escape", false) ? T("bind.kbm_start_menu_escape")
                                                                                : meaning);
    }

    QString conflict;
    bool hasConflict = false;
    for (const QString& token : parse.tokens) {
        hasConflict = hasConflict || conflicts.contains(token);
    }
    if (hasConflict) {
        conflict = conflictText(*row.key);
    }
    QStringList warnings;
    if (!conflict.isEmpty()) {
        warnings.append(conflict);
    }
    if (parse.ok()) {
        const QString reserved = reservedText(row, parse.tokens);
        if (!reserved.isEmpty()) {
            warnings.append(reserved);
        }
        if (parse.tokens.isEmpty() && EssentialControl(row.control)) {
            warnings.append(row.device == BindDevice::Pad ? T("bind.essential_unbound_pad")
                                                          : T("bind.essential_unbound_kbm"));
        }
    }
    if (result.isError()) {
        row.message->setText(result.message);
        SetLabelColor(row.message, SeverityColor(Severity::Error));
        row.message->setVisible(true);
    } else if (!warnings.isEmpty()) {
        row.message->setText(warnings.join(QLatin1Char(' ')));
        SetLabelColor(row.message, SeverityColor(Severity::Warning));
        row.message->setVisible(true);
    } else {
        row.message->setVisible(false);
    }
}

// Keys the game itself uses (src/host/platform/window.cpp): F12 and Print Screen take a screenshot, Alt+Enter
// switches fullscreen (review M4 finding 12). Raw Input still feeds the key to the bindings, so both happen.
QString BindingsPanel::reservedText(const Row& row, const QStringList& tokens) const {
    if (row.device != BindDevice::KeyboardMouse) {
        return QString();
    }
    QStringList parts;
    for (const QString& token : tokens) {
        if (token == QLatin1String("key:f12") || token == QLatin1String("key:printscreen")) {
            parts.append(T("bind.reserved_screenshot").arg(TokenDisplayText(token)));
        }
    }
    const bool alt = tokens.contains(QStringLiteral("key:lalt")) || tokens.contains(QStringLiteral("key:ralt"));
    if (alt) {
        bool enterBound = false;
        for (const Row& other : rows_) {
            if (other.device == BindDevice::KeyboardMouse &&
                ParseBindingList(model_.value(*other.key), BindDevice::KeyboardMouse)
                    .tokens.contains(QStringLiteral("key:enter"))) {
                enterBound = true;
            }
        }
        if (enterBound) {
            parts.append(T("bind.reserved_altenter"));
        }
    }
    return parts.join(QLatin1Char(' '));
}

int BindingsPanel::changedRowCount(BindDevice device) const {
    int n = 0;
    for (const Row& row : rows_) {
        if ((row.device == device) && !model_.isAtDefault(*row.key)) {
            ++n;
        }
    }
    return n;
}

int BindingsPanel::changedRowCount() const {
    return changedRowCount(BindDevice::Pad) + changedRowCount(BindDevice::KeyboardMouse);
}

void BindingsPanel::refreshFromModel() {
    if (rows_.isEmpty()) {
        return;
    }
    for (BindDevice device : {BindDevice::Pad, BindDevice::KeyboardMouse}) {
        const QHash<QString, QStringList> conflicts = conflictsOf(device);
        int errors = 0;
        for (Row& row : rows_) {
            if (row.device == device) {
                updateRow(row, conflicts);
                errors += model_.validate(*row.key).isError() ? 1 : 0;
            }
        }
        QLabel* summary = summary_[device == BindDevice::Pad ? 0 : 1];
        if (errors > 0) {
            summary->setText(errors == 1 ? T("bind.summary_error_one") : T("bind.summary_errors").arg(errors));
            SetLabelColor(summary, SeverityColor(Severity::Error));
        } else if (!conflicts.isEmpty()) {
            summary->setText(T("bind.summary_conflicts").arg(conflicts.size()));
            SetLabelColor(summary, SeverityColor(Severity::Warning));
        } else {
            summary->setText(T("bind.summary_ok"));
            SetLabelColor(summary, MutedTextColor(summary));
        }
        const int tab = device == BindDevice::Pad ? 0 : 1;
        const QString title = device == BindDevice::Pad ? T("bind.device.pad") : T("bind.device.kbm");
        deviceTabs_->setTabText(tab, errors > 0 ? title + QStringLiteral(" ⚠") : title);
    }
}

int BindingsPanel::errorCount() const {
    int n = 0;
    for (const Row& row : rows_) {
        n += model_.validate(*row.key).isError() ? 1 : 0;
    }
    return n;
}

bool BindingsPanel::focusFirstError() {
    for (Row& row : rows_) {
        if (model_.validate(*row.key).isError()) {
            selectDevice(row.device);
            row.binding->setFocus(Qt::OtherFocusReason);
            return true;
        }
    }
    return false;
}

void BindingsPanel::selectDevice(BindDevice device) {
    if (deviceTabs_ != nullptr) {
        deviceTabs_->setCurrentIndex(device == BindDevice::Pad ? 0 : 1);
    }
}

void BindingsPanel::resetDevice(BindDevice device) {
    for (const Row& row : rows_) {
        if (row.device == device) {
            model_.resetToDefault(*row.key);
        }
    }
    changed();
}

void BindingsPanel::resetAll() {
    for (const Row& row : rows_) {
        model_.resetToDefault(*row.key);
    }
    refreshFromModel();
}

BindingCaptureDialog* BindingsPanel::openCapture(const KeyDef& key, bool add) {
    const auto info = ParseBindingKey(key);
    if (!info) {
        return nullptr;
    }
    if (capture_ != nullptr) {
        capture_->reject();
    }
    const BindingLabel label = SplitBindingLabel(key);
    const QString title = label.meaning.isEmpty() ? label.control : label.control + QStringLiteral(" – ") + label.meaning;
    auto* dialog = new BindingCaptureDialog(info->device, add ? T("capture.add_title").arg(title) : title, poller_, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (add) {
        dialog->setWindowTitle(T("capture.window_title_add"));
    } else {
        // Clicking the binding replaces the whole list (review M4 finding 14): say what goes away.
        const QStringList current = ParseBindingList(model_.value(key), info->device).tokens;
        if (!current.isEmpty()) {
            dialog->setNote(T("capture.replaces").arg(TokensText(current)));
        }
    }
    const KeyDef* target = &key;
    connect(dialog, &BindingCaptureDialog::captured, this,
            [this, target, add](const QString& token) { applyToken(*target, token, add); });
    capture_ = dialog;
    dialog->open();
    return dialog;
}

void BindingsPanel::applyToken(const KeyDef& key, const QString& token, bool add) {
    const auto info = ParseBindingKey(key);
    const auto canonical = CanonicalToken(token);
    if (!info || !canonical || TokenDevice(*canonical) != info->device) {
        return;
    }
    QStringList tokens;
    if (add) {
        tokens = ParseBindingList(model_.value(key), info->device).tokens;
        if (tokens.contains(*canonical) || tokens.size() >= kMaxBindingTokens) {
            return;
        }
    }
    tokens.append(*canonical);
    model_.setValue(key, FormatBindingList(tokens));
    changed();
}

void BindingsPanel::changed() {
    refreshFromModel();
    emit edited();
}

// ================================================================================================================
// PadView: a schematic controller (no artwork): triggers, shoulder buttons, sticks with their position, d-pad,
// Back/Start and the face buttons in the usual Xbox colours when pressed.

class PadView final : public QWidget {
public:
    explicit PadView(QWidget* parent) : QWidget(parent) {
        setMinimumSize(fontMetrics().height() * 15, fontMetrics().height() * 8);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setState(const PadState& state) {
        state_ = state;
        update();
    }
    QSize sizeHint() const override { return QSize(fontMetrics().height() * 18, fontMetrics().height() * 9); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF area = QRectF(rect()).adjusted(2, 2, -2, -2);
        const double w = std::min(area.width(), area.height() * 2.0);
        const double h = w / 2.0;
        const QPointF o(area.left() + (area.width() - w) / 2.0, area.top() + (area.height() - h) / 2.0);
        auto at = [&](double x, double y) { return QPointF(o.x() + x * w, o.y() + y * h); };
        const bool on = state_.connected;
        QColor line = palette().color(QPalette::WindowText);
        line.setAlphaF(on ? 0.7 : 0.25);
        const QColor accent = palette().color(QPalette::Highlight);
        QFont small = font();
        small.setPixelSize(std::max(8, static_cast<int>(h * 0.085)));
        p.setFont(small);
        const QPen pen(line, 1.2);

        auto bar = [&](double x, double value, const QString& text) {
            const QRectF r(at(x, 0.02), QSizeF(0.2 * w, 0.09 * h));
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, 3, 3);
            if (on && value > 0.0) {
                p.setPen(Qt::NoPen);
                p.setBrush(accent);
                p.drawRoundedRect(QRectF(r.topLeft(), QSizeF(r.width() * value, r.height())), 3, 3);
            }
            p.setPen(line);
            p.drawText(r, Qt::AlignCenter, text);
        };
        bar(0.06, state_.leftTrigger / 255.0, QStringLiteral("LT"));
        bar(0.74, state_.rightTrigger / 255.0, QStringLiteral("RT"));

        auto button = [&](const QRectF& r, bool pressed, const QColor& fill, const QString& text, bool round) {
            p.setPen(pen);
            p.setBrush(pressed && on ? fill : QColor(Qt::transparent));
            if (round) {
                p.drawEllipse(r);
            } else {
                p.drawRoundedRect(r, 4, 4);
            }
            p.setPen(pressed && on ? QColor(Qt::white) : line);
            p.drawText(r, Qt::AlignCenter, text);
        };
        auto pressed = [this](quint16 bit) { return (state_.buttons & bit) != 0; };
        button(QRectF(at(0.06, 0.14), QSizeF(0.2 * w, 0.1 * h)), pressed(0x0100), accent, QStringLiteral("LB"), false);
        button(QRectF(at(0.74, 0.14), QSizeF(0.2 * w, 0.1 * h)), pressed(0x0200), accent, QStringLiteral("RB"), false);

        // Stick rings of radius 0.17 h: left at (0.2, 0.55), right at (0.62, 0.8) - everything stays inside h.
        const double ring = 0.17 * h;
        auto stick = [&](QPointF c, qint16 x, qint16 y, bool click) {
            p.setPen(pen);
            p.setBrush(click && on ? accent : QColor(Qt::transparent));
            p.drawEllipse(c, ring, ring);
            p.setBrush(Qt::NoBrush);
            p.drawLine(c - QPointF(ring, 0), c + QPointF(ring, 0));
            p.drawLine(c - QPointF(0, ring), c + QPointF(0, ring));
            if (on) {
                const QPointF d(x / 32767.0 * ring, -y / 32767.0 * ring);
                p.setPen(Qt::NoPen);
                p.setBrush(accent);
                p.drawEllipse(c + d, ring * 0.28, ring * 0.28);
            }
        };
        stick(at(0.2, 0.55), state_.lx, state_.ly, pressed(0x0040));
        stick(at(0.62, 0.8), state_.rx, state_.ry, pressed(0x0080));

        const double ds = 0.08 * h;
        const QPointF dc = at(0.38, 0.8);
        button(QRectF(dc + QPointF(-ds / 2, -ds * 1.6), QSizeF(ds, ds)), pressed(0x0001), accent, QString(), false);
        button(QRectF(dc + QPointF(-ds / 2, ds * 0.6), QSizeF(ds, ds)), pressed(0x0002), accent, QString(), false);
        button(QRectF(dc + QPointF(-ds * 1.6, -ds / 2), QSizeF(ds, ds)), pressed(0x0004), accent, QString(), false);
        button(QRectF(dc + QPointF(ds * 0.6, -ds / 2), QSizeF(ds, ds)), pressed(0x0008), accent, QString(), false);

        const QSizeF small2(0.1 * w, 0.1 * h);
        button(QRectF(at(0.37, 0.4), small2), pressed(0x0020), accent, QStringLiteral("Back"), false);
        button(QRectF(at(0.53, 0.4), small2), pressed(0x0010), accent, QStringLiteral("Start"), false);

        const double br = 0.065 * h;
        const QPointF fc = at(0.8, 0.52);
        auto face = [&](QPointF c, quint16 bit, const QColor& color, const QString& text) {
            button(QRectF(c - QPointF(br, br), QSizeF(br * 2, br * 2)), pressed(bit), color, text, true);
        };
        face(fc + QPointF(0, br * 2.1), 0x1000, QColor(0x2e, 0x9e, 0x3e), QStringLiteral("A"));
        face(fc + QPointF(br * 2.1, 0), 0x2000, QColor(0xd0, 0x3a, 0x2f), QStringLiteral("B"));
        face(fc + QPointF(-br * 2.1, 0), 0x4000, QColor(0x2f, 0x6a, 0xd0), QStringLiteral("X"));
        face(fc + QPointF(0, -br * 2.1), 0x8000, QColor(0xc8, 0x9a, 0x10), QStringLiteral("Y"));
    }

private:
    PadState state_;
};

// ================================================================================================================
// PadTestPanel

PadTestPanel::PadTestPanel(SettingsModel& model, PadPoller* poller, QWidget* parent)
    : QGroupBox(T("padtest.title"), parent), model_(model), poller_(poller) {
    setObjectName(QStringLiteral("padTestPanel"));
    auto* layout = new QVBoxLayout(this);
    const int unit = fontMetrics().height();
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("padStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(unit);
    grid->setVerticalSpacing(unit / 2);
    for (int slot = 0; slot < kPadSlots; ++slot) {
        SlotWidgets& s = slotWidgets_[static_cast<size_t>(slot)];
        auto* frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("padSlot%1").arg(slot));
        frame->setFrameShape(QFrame::StyledPanel);
        auto* v = new QVBoxLayout(frame);
        s.title = new QLabel(frame);
        QFont bold = s.title->font();
        bold.setBold(true);
        s.title->setFont(bold);
        s.view = new PadView(frame);
        s.pressed = new QLabel(frame);
        s.pressed->setObjectName(QStringLiteral("padPressed%1").arg(slot));
        s.pressed->setWordWrap(true);
        s.game = Muted(QString(), frame);
        s.game->setObjectName(QStringLiteral("padGame%1").arg(slot));
        s.rumble = new QPushButton(T("padtest.rumble"), frame);
        s.rumble->setObjectName(QStringLiteral("padRumble%1").arg(slot));
        s.rumble->setAutoDefault(false);
        auto* top = new QHBoxLayout();
        top->addWidget(s.title, 1);
        top->addWidget(s.rumble);
        v->addLayout(top);
        v->addWidget(s.view);
        v->addWidget(s.pressed);
        v->addWidget(s.game);
        s.frame = frame;
        grid->addWidget(frame, slot / 2, slot % 2);
        connect(s.rumble, &QPushButton::clicked, this, [this, slot] {
            bool enabled = true;
            const double strength = rumbleStrength(&enabled);
            if (poller_ != nullptr && enabled) {
                poller_->testRumble(slot, strength, 600);
            }
        });
    }
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);
    // Slots without a controller: one line instead of empty frames (review M4 finding 16).
    absent_ = Muted(QString(), this);
    absent_->setObjectName(QStringLiteral("padAbsent"));
    layout->addWidget(absent_);
    note_ = Muted(T("padtest.note"), this);
    note_->setObjectName(QStringLiteral("padNote"));
    layout->addWidget(note_);
    if (poller_ != nullptr) {
        connect(poller_, &PadPoller::updated, this, &PadTestPanel::updateFromPoller);
    }
    updateFromPoller();
}

PadTestPanel::~PadTestPanel() {
    // A visible panel that is deleted gets no hide event (rebuild of the window): give the poller back.
    if (poller_ != nullptr && polling_) {
        poller_->release();
    }
}

void PadTestPanel::showEvent(QShowEvent* event) {
    QGroupBox::showEvent(event);
    if (poller_ != nullptr && !polling_) {
        polling_ = true;
        poller_->acquire();
    }
}

void PadTestPanel::hideEvent(QHideEvent* event) {
    if (poller_ != nullptr && polling_) {
        polling_ = false;
        poller_->release();
    }
    QGroupBox::hideEvent(event);
}

double PadTestPanel::rumbleStrength(bool* enabled) const {
    *enabled = true;
    double strength = 1.0;
    if (const KeyDef* on = model_.schema().find(QStringLiteral("input"), QStringLiteral("rumble"))) {
        *enabled = ParseBoolText(model_.value(*on)).value_or(true);
    }
    if (const KeyDef* s = model_.schema().find(QStringLiteral("input"), QStringLiteral("rumble_strength"))) {
        strength = std::clamp(ParseFloatText(model_.value(*s)).value_or(100.0) / 100.0, 0.0, 1.0);
    }
    return strength;
}

void PadTestPanel::refreshFromModel() {
    updateFromPoller();
}

QString PadTestPanel::pressedText(int slot) const {
    return slotWidgets_[static_cast<size_t>(slot)].pressed->text();
}

QString PadTestPanel::gameText(int slot) const {
    return slotWidgets_[static_cast<size_t>(slot)].game->text();
}

void PadTestPanel::updateFromPoller() {
    const bool available = poller_ != nullptr && poller_->reader().available();
    if (!available) {
        status_->setText(T("padtest.no_xinput"));
        SetLabelColor(status_, SeverityColor(Severity::Warning));
    } else {
        const int connected = poller_->connectedCount();
        status_->setText(poller_->reader().simulated()
                             ? T("padtest.status_simulated").arg(connected)
                             : T("padtest.status").arg(poller_->reader().backendName()).arg(connected));
        SetLabelColor(status_, MutedTextColor(status_));
    }
    // Pad bindings of the configuration: which Xbox controls the pressed inputs give the game.
    QList<BindingRow> rows;
    QHash<QString, QString> names;
    for (const KeyDef* key : model_.schema().allKeys()) {
        const auto info = ParseBindingKey(*key);
        if (info && info->device == BindDevice::Pad) {
            rows.append(BindingRow{info->control, ParseBindingList(model_.value(*key), BindDevice::Pad).tokens});
            names.insert(info->control, SplitBindingLabel(*key).control);
        }
    }
    bool rumbleEnabled = true;
    rumbleStrength(&rumbleEnabled);
    const int deadzone = IntSetting(model_, "input", "pad_deadzone", 0);
    const KeyDef* modeKey = model_.schema().find(QStringLiteral("input"), QStringLiteral("pad_mode"));
    const bool perPort = modeKey != nullptr && model_.value(*modeKey).trimmed() == QLatin1String("per_port");
    QStringList absent;
    for (int slot = 0; slot < kPadSlots; ++slot) {
        SlotWidgets& s = slotWidgets_[static_cast<size_t>(slot)];
        const PadState state = poller_ != nullptr ? poller_->state(slot) : PadState{};
        s.title->setText(T("padtest.slot").arg(slot + 1) + QStringLiteral(" – ") +
                         (state.connected ? T("padtest.connected") : T("padtest.not_connected")));
        s.view->setState(state);
        s.frame->setVisible(state.connected);
        s.rumble->setEnabled(state.connected && rumbleEnabled);
        s.rumble->setToolTip(rumbleEnabled ? T("padtest.rumble.tip") : T("padtest.rumble_off"));
        if (!state.connected) {
            absent.append(QString::number(slot + 1));
            s.pressed->setText(QString());
            s.game->setText(QString());
            continue;
        }
        const QSet<QString> active = ActivePadTokens(state);
        QStringList pressed;
        for (const QString& t : SortedPadTokens(active)) {
            pressed.append(TokenDisplayText(t));
        }
        s.pressed->setText(T("padtest.pressed").arg(pressed.isEmpty() ? T("padtest.nothing") : pressed.join(QStringLiteral(", "))));
        if (rows.isEmpty()) {
            s.game->setText(QString());
            continue;
        }
        // Like the host: analog buttons from 30/255, digital controls from 0.5, sticks after the dead zones.
        QStringList controls;
        for (const QString& c : GameControlsForPad(state, rows, deadzone)) {
            controls.append(names.value(c, ControlDisplayName(c)));
        }
        const QString list = controls.isEmpty() ? T("padtest.nothing") : controls.join(QStringLiteral(", "));
        s.game->setText(perPort ? T("padtest.game_port").arg(slot + 1).arg(list) : T("padtest.game").arg(list));
    }
    const int connected = kPadSlots - static_cast<int>(absent.size());
    if (absent.isEmpty()) {
        absent_->setVisible(false);
    } else {
        absent_->setText(connected == 0 ? T("padtest.none_connected")
                                        : T("padtest.absent").arg(absent.join(QStringLiteral(", "))));
        absent_->setVisible(true);
    }
    note_->setVisible(connected > 0);
}

}  // namespace fl::ui
