#include "ui/key_editors.h"

#include "core/l10n.h"
#include "core/validation.h"
#include "ui/app_style.h"
#include "ui/screen_info.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <climits>
#include <cmath>

namespace fl::ui {
namespace {

QHBoxLayout* MakeRowLayout(QWidget* parent) {
    auto* layout = new QHBoxLayout(parent);
    layout->setContentsMargins(0, 0, 0, 0);
    return layout;
}

// --- combo (choices, or bool as on/off) -----------------------------------------------------------
class ComboEditor final : public KeyEditor {
public:
    ComboEditor(const KeyDef& key, const EditorContext& context, QWidget* parent)
        : KeyEditor(key, context, parent) {
        combo_ = new QComboBox(this);
        combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        MakeRowLayout(this)->addWidget(combo_, 0, Qt::AlignLeft);
        if (key.type == KeyType::Bool) {
            combo_->addItem(T("editor.on"), QStringLiteral("1"));
            combo_->addItem(T("editor.off"), QStringLiteral("0"));
        } else {
            for (const ChoiceDef& c : key.choices) {
                if (!c.isRange()) {
                    combo_->addItem(c.label.text(), c.value);
                }
            }
        }
        regularCount_ = combo_->count();
        connect(combo_, &QComboBox::activated, this, [this](int) { emitEdited(); });
    }

    void setValue(const QString& value) override {
        updating_ = true;
        last_ = value;
        int index = -1;
        for (int i = 0; i < regularCount_; ++i) {
            if (ValuesEquivalent(key_, combo_->itemData(i).toString(), value)) {
                index = i;
                break;
            }
        }
        while (combo_->count() > regularCount_) {
            combo_->removeItem(combo_->count() - 1);
        }
        if (index < 0) {
            // A hidden choice (accepted, not offered) or a value of a numeric range shows as itself.
            const bool valid = key_.type == KeyType::Choice && MatchesChoice(key_, value);
            combo_->addItem(valid ? ChoiceText(key_, value) : value + T("editor.invalid_suffix"), value);
            index = combo_->count() - 1;
        }
        combo_->setCurrentIndex(index);
        updating_ = false;
    }

    QString value() const override {
        const QString current = combo_->currentData().toString();
        if (key_.type == KeyType::Bool) {
            const auto b = ParseBoolText(current);
            return b ? BoolText(*b, last_) : current;
        }
        return current;
    }

    QWidget* focusTarget() override { return combo_; }

private:
    QComboBox* combo_ = nullptr;
    int regularCount_ = 0;
    QString last_;
};

// --- check box -----------------------------------------------------------------------------------
class CheckEditor final : public KeyEditor {
public:
    CheckEditor(const KeyDef& key, const EditorContext& context, QWidget* parent)
        : KeyEditor(key, context, parent) {
        check_ = new QCheckBox(this);
        MakeRowLayout(this)->addWidget(check_, 0, Qt::AlignLeft);
        connect(check_, &QCheckBox::clicked, this, [this](bool) { emitEdited(); });
    }
    void setValue(const QString& value) override {
        updating_ = true;
        last_ = value;
        check_->setChecked(ParseBoolText(value).value_or(false));
        updating_ = false;
    }
    QString value() const override { return BoolText(check_->isChecked(), last_); }
    QWidget* focusTarget() override { return check_; }

private:
    QCheckBox* check_ = nullptr;
    QString last_;
};

// --- integer -------------------------------------------------------------------------------------
// [render] window_scale: the factor gets a "x" suffix and the resulting window size in pixels on the selected
// monitor (the game multiplies by the Windows scaling and shrinks a window that does not fit).
bool IsWindowScaleKey(const KeyDef& key) {
    return key.fullName() == QLatin1String("render.window_scale");
}

class IntEditor final : public KeyEditor {
public:
    IntEditor(const KeyDef& key, const EditorContext& context, QWidget* parent)
        : KeyEditor(key, context, parent) {
        const double lo = key.min.value_or(0.0);
        const double hi = key.max.value_or(static_cast<double>(INT_MAX));
        auto* layout = MakeRowLayout(this);
        if (lo >= static_cast<double>(INT_MIN) && hi <= static_cast<double>(INT_MAX)) {
            spin_ = new QSpinBox(this);
            spin_->setRange(static_cast<int>(lo), static_cast<int>(hi));
            spin_->setSingleStep(key.step ? std::max(1, static_cast<int>(*key.step)) : 1);
            spin_->setMinimumWidth(spin_->fontMetrics().horizontalAdvance(QString(10, QLatin1Char('9'))));
            layout->addWidget(spin_, 0, Qt::AlignLeft);
            connect(spin_, &QSpinBox::valueChanged, this, [this](int) {
                updateInfo();
                emitEdited();
            });
            if (IsWindowScaleKey(key)) {
                spin_->setSuffix(QString(QChar(0x00D7)));
                info_ = new QLabel(this);
                QPalette p = info_->palette();
                p.setColor(QPalette::WindowText, MutedTextColor(info_));
                info_->setPalette(p);
                layout->addWidget(info_, 1);
            }
        } else {
            edit_ = new QLineEdit(this);
            edit_->setMaximumWidth(edit_->fontMetrics().horizontalAdvance(QString(18, QLatin1Char('9'))));
            layout->addWidget(edit_, 0, Qt::AlignLeft);
            connect(edit_, &QLineEdit::textEdited, this, [this](const QString&) { emitEdited(); });
        }
    }
    void setValue(const QString& value) override {
        updating_ = true;
        if (spin_ != nullptr) {
            const auto n = ParseIntText(value);
            spin_->setValue(n ? static_cast<int>(std::clamp<qint64>(*n, spin_->minimum(), spin_->maximum()))
                              : spin_->minimum());
        } else {
            edit_->setText(value);
        }
        updateInfo();
        updating_ = false;
    }
    QString value() const override {
        return spin_ != nullptr ? QString::number(spin_->value()) : edit_->text().trimmed();
    }
    QWidget* focusTarget() override { return spin_ != nullptr ? static_cast<QWidget*>(spin_) : edit_; }
    void refreshContext() override { updateInfo(); }

private:
    void updateInfo() {
        if (info_ == nullptr) {
            return;
        }
        const WindowSizeEstimate e = EstimateWindowSize(spin_->value(), SelectedOutput(context_));
        QString text = e.dpi == 96 ? T("winscale.size").arg(SizeText(e.client))
                                   : T("winscale.size_dpi").arg(SizeText(e.client)).arg(qRound(e.dpi * 100.0 / 96));
        if (e.shrunk) {
            text += QStringLiteral(" \u2013 ") + T("winscale.shrunk");
        }
        info_->setText(text);
    }

    QSpinBox* spin_ = nullptr;
    QLineEdit* edit_ = nullptr;
    QLabel* info_ = nullptr;
};

// --- float ---------------------------------------------------------------------------------------
class FloatEditor final : public KeyEditor {
public:
    FloatEditor(const KeyDef& key, const EditorContext& context, QWidget* parent)
        : KeyEditor(key, context, parent) {
        spin_ = new QDoubleSpinBox(this);
        spin_->setDecimals(2);
        spin_->setRange(key.min.value_or(0.0), key.max.value_or(1e6));
        spin_->setSingleStep(key.step.value_or(0.1));
        spin_->setMinimumWidth(spin_->fontMetrics().horizontalAdvance(QStringLiteral("0000,000")));
        MakeRowLayout(this)->addWidget(spin_, 0, Qt::AlignLeft);
        connect(spin_, &QDoubleSpinBox::valueChanged, this, [this](double) { emitEdited(); });
    }
    void setValue(const QString& value) override {
        updating_ = true;
        spin_->setValue(ParseFloatText(value).value_or(key_.min.value_or(0.0)));
        updating_ = false;
    }
    QString value() const override { return FormatFloat(spin_->value()); }
    QWidget* focusTarget() override { return spin_; }

private:
    QDoubleSpinBox* spin_ = nullptr;
};

// --- free text -----------------------------------------------------------------------------------
class TextEditor final : public KeyEditor {
public:
    TextEditor(const KeyDef& key, const EditorContext& context, QWidget* parent)
        : KeyEditor(key, context, parent) {
        edit_ = new QLineEdit(this);
        MakeRowLayout(this)->addWidget(edit_);
        if (!key.defaultText.isEmpty()) {
            edit_->setPlaceholderText(key.defaultText);
        }
        connect(edit_, &QLineEdit::textEdited, this, [this](const QString&) { emitEdited(); });
    }
    void setValue(const QString& value) override {
        updating_ = true;
        edit_->setText(value);
        updating_ = false;
    }
    QString value() const override { return edit_->text(); }
    QWidget* focusTarget() override { return edit_; }

private:
    QLineEdit* edit_ = nullptr;
};

// --- file / directory ----------------------------------------------------------------------------
class PathEditor final : public KeyEditor {
public:
    PathEditor(const KeyDef& key, const EditorContext& context, QWidget* parent)
        : KeyEditor(key, context, parent) {
        auto* layout = MakeRowLayout(this);
        edit_ = new QLineEdit(this);
        edit_->setClearButtonEnabled(true);
        browse_ = new QToolButton(this);
        browse_->setText(T("editor.browse"));
        browse_->setToolTip(isDir() ? T("editor.browse_dir.tip") : T("editor.browse_file.tip"));
        open_ = new QToolButton(this);
        open_->setText(T("editor.open_folder"));
        open_->setToolTip(T("editor.open_folder.tip"));
        layout->addWidget(edit_, 1);
        layout->addWidget(browse_);
        layout->addWidget(open_);
        if (context_.placeholder) {
            edit_->setPlaceholderText(context_.placeholder(key));
        }
        connect(edit_, &QLineEdit::textEdited, this, [this](const QString&) { emitEdited(); });
        connect(browse_, &QToolButton::clicked, this, [this] { browse(); });
        connect(open_, &QToolButton::clicked, this, [this] { openFolder(); });
    }
    void setValue(const QString& value) override {
        updating_ = true;
        edit_->setText(value);
        updating_ = false;
    }
    // Review M3c finding 10: without enclosing quotes (a path pasted from Explorer's "Als Pfad kopieren").
    QString value() const override { return CleanPathInput(edit_->text()); }
    QWidget* focusTarget() override { return edit_; }

private:
    bool isDir() const { return key_.type == KeyType::PathDir; }

    QString resolved() const {
        QString text = value();
        if (text.isEmpty() && context_.placeholder) {
            text = context_.placeholder(key_);
        }
        return ResolvePath(text, context_.iniDir ? context_.iniDir() : QString());
    }

    void browse() {
        const QString current = resolved();
        QString start = current;
        if (!current.isEmpty() && !QFileInfo(current).isDir()) {
            start = QFileInfo(current).absolutePath();
        }
        if (start.isEmpty() || !QFileInfo::exists(start)) {
            start = context_.iniDir ? context_.iniDir() : QString();
        }
        QString chosen;
        if (isDir()) {
            chosen = QFileDialog::getExistingDirectory(this, key_.displayLabel(), start);
        } else {
            QString filter = T("editor.all_files_filter");
            if (!key_.fileFilter.isEmpty()) {
                filter = T("editor.pattern_filter").arg(key_.displayLabel(), key_.fileFilter) +
                         QStringLiteral(";;") + filter;
            } else if (key_.fullName() == QLatin1String("paths.iso")) {
                filter = T("editor.iso_filter");
            }
            chosen = QFileDialog::getOpenFileName(this, key_.displayLabel(), start, filter);
        }
        if (!chosen.isEmpty()) {
            edit_->setText(QDir::toNativeSeparators(chosen));
            emitEdited();
        }
    }

    void openFolder() {
        QString target = resolved();
        if (!target.isEmpty() && !isDir()) {
            target = QFileInfo(target).absolutePath();
        }
        if (target.isEmpty() || !QFileInfo(target).isDir()) {
            target = context_.iniDir ? context_.iniDir() : QString();
        }
        if (!target.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(target));
        }
    }

    QLineEdit* edit_ = nullptr;
    QToolButton* browse_ = nullptr;
    QToolButton* open_ = nullptr;
};

// --- resolution ----------------------------------------------------------------------------------
// "Automatisch" (what the game's resolution auto gives: the 4:3 area of the window or, in fullscreen, of the
// monitor) and the 4:3 sizes that fit the selected monitor. Free input of any <W>x<H> stays possible (the game
// renders the largest 4:3 area inside it).
class ResolutionEditor final : public KeyEditor {
public:
    ResolutionEditor(const KeyDef& key, const EditorContext& context, QWidget* parent)
        : KeyEditor(key, context, parent) {
        combo_ = new QComboBox(this);
        combo_->setEditable(true);
        combo_->setInsertPolicy(QComboBox::NoInsert);
        combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        MakeRowLayout(this)->addWidget(combo_, 0, Qt::AlignLeft);
        refreshContext();
        connect(combo_, &QComboBox::activated, this, [this](int) { emitEdited(); });
        connect(combo_->lineEdit(), &QLineEdit::textEdited, this, [this](const QString&) { emitEdited(); });
    }

    void refreshContext() override {
        const bool wasUpdating = updating_;
        updating_ = true;
        const QString current = combo_->count() > 0 ? value() : key_.defaultText;
        combo_->clear();
        const GpuOutput* output = SelectedOutput(context_);
        const QSize native = output != nullptr ? output->nativeSize() : QSize();
        const QSize full = native.isValid() ? FitAspect43(native) : QSize();
        combo_->addItem(autoText(output, full), QStringLiteral("auto"));
        QList<QSize> sizes;
        auto add = [&sizes, full](QSize s, bool always) {
            const bool fits = !full.isValid() || (s.width() <= full.width() && s.height() <= full.height());
            if (s.isValid() && (fits || always) && !sizes.contains(s)) {
                sizes.append(s);
            }
        };
        add(full, false);
        // Suggestions of the schema ("choices" of a resolution key) that are 4:3.
        for (const ChoiceDef& c : key_.choices) {
            if (const auto s = ParseResolutionText(c.value); s && s->width() * 3 == s->height() * 4) {
                add(*s, false);
            }
        }
        for (int k = 1; k <= 8; ++k) {
            add(QSize(640 * k, 480 * k), k == 1);
        }
        std::sort(sizes.begin(), sizes.end(), [](QSize a, QSize b) {
            const qint64 aa = qint64(a.width()) * a.height();
            const qint64 bb = qint64(b.width()) * b.height();
            return aa != bb ? aa > bb : a.width() > b.width();
        });
        for (QSize s : sizes) {
            QString text = SizeText(s);
            QStringList notes;
            if (s == full) {
                notes.append(T("res.full_height"));
            }
            if (s.width() % 640 == 0 && s.width() / 640 > 1 && s.width() / 640 * 480 == s.height()) {
                notes.append(QStringLiteral("%1").arg(s.width() / 640) + QChar(0x00D7));
            }
            if (s == QSize(640, 480)) {
                notes.append(T("res.original"));
            }
            if (!notes.isEmpty()) {
                text += QStringLiteral(" (") + notes.join(QStringLiteral(", ")) + QLatin1Char(')');
            }
            combo_->addItem(text, FormatResolution(s));
        }
        setValueInternal(current);
        updating_ = wasUpdating;
    }

    void setValue(const QString& value) override {
        updating_ = true;
        setValueInternal(value);
        updating_ = false;
    }

    QString value() const override {
        const QString text = combo_->currentText().trimmed();
        for (int i = 0; i < combo_->count(); ++i) {
            if (combo_->itemText(i) == text) {
                return combo_->itemData(i).toString();
            }
        }
        // Free input: "1600 x 1200", "1600X1200 (4:3)" -> "1600x1200".
        QString v = text;
        const qsizetype paren = v.indexOf(QLatin1Char('('));
        if (paren > 0) {
            v = v.left(paren);
        }
        v.remove(QLatin1Char(' '));
        v.replace(QChar(0x00D7), QLatin1Char('x'));
        v.replace(QLatin1Char('X'), QLatin1Char('x'));
        if (IsAutoResolution(v)) {
            return QStringLiteral("auto");
        }
        return v;
    }

    QWidget* focusTarget() override { return combo_; }

private:
    // What resolution auto gives with the current window keys: "Automatisch (Vollbild: 2880 x 2160)",
    // "Automatisch (Fenster: 1440 x 1080)" or "Automatisch (Fenstergroesse, bis 2880 x 2160)".
    QString autoText(const GpuOutput* output, QSize full) const {
        if (!full.isValid()) {
            return T("res.auto");
        }
        const KeyDef* fullscreen = nullptr;
        const KeyDef* scale = nullptr;
        if (context_.schema != nullptr) {
            fullscreen = context_.schema->find(QStringLiteral("render"), QStringLiteral("fullscreen"));
            scale = context_.schema->find(QStringLiteral("render"), QStringLiteral("window_scale"));
        }
        const bool isFullscreen = fullscreen != nullptr && context_.currentValue &&
                                  ParseBoolText(context_.currentValue(*fullscreen)).value_or(false);
        if (isFullscreen) {
            return T("res.auto_fullscreen").arg(SizeText(full));
        }
        if (scale != nullptr && context_.currentValue) {
            const int factor = static_cast<int>(ParseIntText(context_.currentValue(*scale)).value_or(2));
            const WindowSizeEstimate e = EstimateWindowSize(factor, output);
            if (!e.shrunk) {
                return T("res.auto_window").arg(SizeText(e.client));
            }
        }
        return T("res.auto_window_max").arg(SizeText(full));
    }

    void setValueInternal(const QString& value) {
        for (int i = 0; i < combo_->count(); ++i) {
            if (combo_->itemData(i).toString().compare(value.trimmed(), Qt::CaseInsensitive) == 0) {
                combo_->setCurrentIndex(i);
                return;
            }
        }
        combo_->setCurrentIndex(-1);
        combo_->setEditText(value);
    }

    QComboBox* combo_ = nullptr;
};

// --- monitor -------------------------------------------------------------------------------------
class MonitorEditor final : public KeyEditor {
public:
    MonitorEditor(const KeyDef& key, const EditorContext& context, QWidget* parent)
        : KeyEditor(key, context, parent) {
        combo_ = new QComboBox(this);
        combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        MakeRowLayout(this)->addWidget(combo_, 0, Qt::AlignLeft);
        build();
        connect(combo_, &QComboBox::activated, this, [this](int) { emitEdited(); });
    }

    void setValue(const QString& value) override {
        updating_ = true;
        while (combo_->count() > regularCount_) {
            combo_->removeItem(combo_->count() - 1);
        }
        int index = -1;
        for (int i = 0; i < regularCount_; ++i) {
            if (combo_->itemData(i).toString().compare(value.trimmed(), Qt::CaseInsensitive) == 0) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            combo_->addItem(value + QStringLiteral(" (") + T("monitor.not_connected") + QLatin1Char(')'),
                            value);
            index = combo_->count() - 1;
        }
        combo_->setCurrentIndex(index);
        updating_ = false;
    }

    QString value() const override { return combo_->currentData().toString(); }
    QWidget* focusTarget() override { return combo_; }

private:
    void build() {
        const GpuInfo empty;
        const GpuInfo& gpu = context_.gpu != nullptr ? *context_.gpu : empty;
        if (key_.type == KeyType::Choice) {
            for (const ChoiceDef& c : key_.choices) {
                if (c.isRange()) {
                    continue;
                }
                QString text = c.label.text();
                if (const GpuOutput* o = OutputForMonitorValue(key_, c.value, gpu)) {
                    text += QStringLiteral(" – ") + OutputDisplayText(*o);
                }
                combo_->addItem(text, c.value);
            }
        } else {
            const GpuOutput* primary = gpu.primaryOutput();
            QStringList outputValues;
            for (qsizetype i = 0; i < gpu.outputs.size(); ++i) {
                outputValues.append(valueForOutput(i, gpu.outputs[i]));
            }
            if (!outputValues.contains(key_.defaultText, Qt::CaseInsensitive)) {
                combo_->addItem(primary != nullptr ? T("monitor.auto_named").arg(primary->name)
                                                   : T("monitor.auto"),
                                key_.defaultText);
            }
            for (qsizetype i = 0; i < gpu.outputs.size(); ++i) {
                combo_->addItem(QStringLiteral("%1: ").arg(i + 1) + OutputDisplayText(gpu.outputs[i]),
                                outputValues[i]);
            }
        }
        regularCount_ = combo_->count();
    }

    QString valueForOutput(qsizetype index, const GpuOutput& output) const {
        if (key_.type == KeyType::Int) {
            const int offset = key_.min && *key_.min >= 1.0 ? 1 : 0;
            return QString::number(index + offset);
        }
        return output.device.isEmpty() ? output.name : output.device;
    }

    QComboBox* combo_ = nullptr;
    int regularCount_ = 0;
};

}  // namespace

KeyEditor::KeyEditor(const KeyDef& key, const EditorContext& context, QWidget* parent)
    : QWidget(parent), key_(key), context_(context) {
    setObjectName(key.fullName());
    setAccessibleName(key.displayLabel());
}

void KeyEditor::emitEdited() {
    if (!updating_) {
        emit edited(value());
    }
}

QString ChoiceText(const KeyDef& key, const QString& value) {
    if (const ChoiceDef* c = key.findChoice(value)) {
        return c->label.text();
    }
    for (const ChoiceDef& c : key.hiddenChoices) {
        if (!c.isRange() && c.value.compare(value.trimmed(), Qt::CaseInsensitive) == 0) {
            return c.label.text();
        }
    }
    if (key.type == KeyType::Bool) {
        const auto b = ParseBoolText(value);
        if (b) {
            return *b ? T("editor.on") : T("editor.off");
        }
    }
    return value;
}

const GpuOutput* OutputForMonitorValue(const KeyDef& key, const QString& value, const GpuInfo& gpu) {
    if (key.type == KeyType::Int) {
        const auto n = ParseIntText(value);
        const int offset = key.min && *key.min >= 1.0 ? 1 : 0;
        if (n && *n - offset >= 0 && *n - offset < gpu.outputs.size()) {
            return &gpu.outputs[static_cast<qsizetype>(*n - offset)];
        }
        return nullptr;
    }
    return gpu.findOutput(value);
}

const GpuOutput* SelectedOutput(const EditorContext& context) {
    if (context.gpu == nullptr) {
        return nullptr;
    }
    if (context.schema != nullptr && context.currentValue) {
        if (const KeyDef* monitor = context.schema->findByWidget(UiWidget::Monitor)) {
            if (const GpuOutput* o =
                    OutputForMonitorValue(*monitor, context.currentValue(*monitor), *context.gpu)) {
                return o;
            }
        }
    }
    return context.gpu->primaryOutput();
}

KeyEditor* CreateKeyEditor(const KeyDef& key, const EditorContext& context, QWidget* parent) {
    switch (key.widget) {
    case UiWidget::Combo:
        return new ComboEditor(key, context, parent);
    case UiWidget::Check:
        return new CheckEditor(key, context, parent);
    case UiWidget::Spin:
        return new IntEditor(key, context, parent);
    case UiWidget::DSpin:
        return new FloatEditor(key, context, parent);
    case UiWidget::File:
    case UiWidget::Dir:
        return new PathEditor(key, context, parent);
    case UiWidget::Resolution:
        return new ResolutionEditor(key, context, parent);
    case UiWidget::Monitor:
        return new MonitorEditor(key, context, parent);
    case UiWidget::Text:
        break;
    }
    return new TextEditor(key, context, parent);
}

}  // namespace fl::ui
