#include "ui/settings_tab.h"

#include "core/l10n.h"
#include "core/validation.h"
#include "ui/app_style.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fl::ui {
namespace {

// Keys that only have an effect together with another key (the editor is disabled with a note otherwise):
// alpha-to-coverage needs MSAA, linear_aa needs MSAA or supersampling (GFX_CONTRACT.md 12, schema help texts).
// Returns the note, empty if `key` applies.
QString InapplicableNote(const KeyDef& key, const SettingsModel& model) {
    const KeyDef* msaa = model.schema().find(QStringLiteral("render"), QStringLiteral("msaa"));
    const bool multisampled = msaa == nullptr || ParseIntText(model.value(*msaa)).value_or(0) > 1;
    if (key.fullName() == QLatin1String("render.alpha_to_coverage") && !multisampled) {
        return T("tab.needs_msaa");
    }
    if (key.fullName() == QLatin1String("render.linear_aa") && !multisampled) {
        const KeyDef* scale = model.schema().find(QStringLiteral("render"), QStringLiteral("render_scale"));
        if (scale == nullptr || ParseFloatText(model.value(*scale)).value_or(1.0) <= 1.0) {
            return T("tab.needs_aa");
        }
    }
    return QString();
}

// A hint below a key that applies but has a catch (review M3c finding 1: fractional render scales are filtered).
QString InfoNote(const KeyDef& key, const SettingsModel& model) {
    if (key.fullName() == QLatin1String("render.render_scale")) {
        const double s = ParseFloatText(model.value(key)).value_or(1.0);
        if (s > 1.0 + 1e-6 && std::fabs(s - 2.0) > 1e-6) {
            return T("tab.fractional_scale");
        }
    }
    return QString();
}

// "Auf Standard zurücksetzen" of the paths tab keeps the chosen game image (review M3c finding 19).
bool KeptOnTabReset(const KeyDef& key) {
    return key.fullName() == QLatin1String("paths.iso");
}

}  // namespace

SettingsTab::SettingsTab(UiTab tab, SettingsModel& model, const EditorContext& context,
                         std::function<bool(const KeyDef&)> visible, QWidget* parent)
    : QWidget(parent), tab_(tab), model_(model), context_(context) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    content_ = content;
    auto* layout = new QVBoxLayout(content);
    const int gap = fontMetrics().height() / 2;
    layout->setContentsMargins(gap, gap, gap, gap);
    layout->setSpacing(gap);
    topLayout_ = new QVBoxLayout();
    topLayout_->setContentsMargins(0, 0, 0, 0);
    topLayout_->setSpacing(gap);
    layout->addLayout(topLayout_);

    // Groups per INI section, in order of their first key. Graphics and paths are one topic each and get a
    // single group (the screenshot folder of [render] belongs with the other paths).
    QList<const KeyDef*> keys = model_.schema().keysForTab(tab);
    if (visible) {
        keys.removeIf([&visible](const KeyDef* key) { return !visible(*key); });
    }
    const bool singleGroup = tab == UiTab::Graphics || tab == UiTab::Paths;
    auto groupOf = [singleGroup](const KeyDef* key) { return singleGroup ? QString() : key->section; };
    QStringList sectionOrder;
    for (const KeyDef* key : keys) {
        if (!sectionOrder.contains(groupOf(key))) {
            sectionOrder.append(groupOf(key));
        }
    }
    for (const QString& section : sectionOrder) {
        const SectionDef* def = section.isEmpty() ? nullptr : model_.schema().findSection(section);
        QString title = def != nullptr && !def->label.isEmpty() ? def->label.text() : section;
        if (section.isEmpty()) {
            title = tab == UiTab::Graphics ? T("tab.graphics_group") : T("tab.paths_group");
        }
        auto* group = new QGroupBox(title, content);
        auto* grid = new QGridLayout(group);
        grid->setColumnStretch(1, 1);
        grid->setHorizontalSpacing(fontMetrics().height());
        grid->setVerticalSpacing(gap / 2);
        int r = 0;
        for (const KeyDef* key : keys) {
            if (groupOf(key) != section) {
                continue;
            }
            Row row;
            row.key = key;
            row.label = new QLabel(group);
            row.editor = CreateKeyEditor(*key, context_, group);
            row.label->setBuddy(row.editor->focusTarget());
            row.reset = new QToolButton(group);
            row.reset->setText(QStringLiteral("↺"));
            row.reset->setAutoRaise(true);
            QSizePolicy keep = row.reset->sizePolicy();
            keep.setRetainSizeWhenHidden(true);
            row.reset->setSizePolicy(keep);
            row.message = new QLabel(group);
            row.message->setWordWrap(true);
            row.message->setVisible(false);
            const QString tip = tooltipFor(*key);
            row.label->setToolTip(tip);
            row.editor->setToolTip(tip);
            grid->addWidget(row.label, r, 0, Qt::AlignLeft | Qt::AlignVCenter);
            grid->addWidget(row.editor, r, 1);
            grid->addWidget(row.reset, r, 2);
            grid->addWidget(row.message, r + 1, 1, 1, 2);
            r += 2;
            const qsizetype index = rows_.size();
            rows_.append(row);
            connect(row.editor, &KeyEditor::edited, this, [this, index](const QString& value) {
                Row& changed = rows_[index];
                model_.setValue(*changed.key, value);
                updateRow(changed);
                for (Row& other : rows_) {
                    if (other.editor != changed.editor) {
                        other.editor->refreshContext();
                    }
                }
                emit valueEdited(changed.key);
            });
            connect(row.reset, &QToolButton::clicked, this, [this, index] {
                Row& target = rows_[index];
                model_.resetToDefault(*target.key);
                target.editor->setValue(model_.value(*target.key));
                updateRow(target);
                for (Row& other : rows_) {
                    other.editor->refreshContext();
                }
                emit valueEdited(target.key);
            });
        }
        layout->addWidget(group);
    }
    // One label column for all groups of the tab (the editors line up across the group boxes).
    int labelWidth = 0;
    for (const Row& row : rows_) {
        labelWidth = std::max(labelWidth, row.label->fontMetrics().horizontalAdvance(row.key->displayLabel() +
                                                                                     QStringLiteral(" *")));
    }
    for (Row& row : rows_) {
        row.label->setMinimumWidth(labelWidth);
    }
    labelWidth_ = labelWidth;
    if (keys.isEmpty() && tab != UiTab::Advanced) {
        auto* none = new QLabel(T("tab.no_keys"), content);
        none->setWordWrap(true);
        layout->addWidget(none);
    }
    bottomLayout_ = new QVBoxLayout();
    bottomLayout_->setContentsMargins(0, 0, 0, 0);
    bottomLayout_->setSpacing(gap);
    layout->addLayout(bottomLayout_);
    layout->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(gap, 0, gap, gap / 2);
    footer->addStretch(1);
    auto* resetAll = new QPushButton(T("tab.reset_all"), this);
    resetAll->setToolTip(tab == UiTab::Paths ? T("tab.reset_all.tip_paths") : T("tab.reset_all.tip"));
    resetAll->setEnabled(!keys.isEmpty());
    footer->addWidget(resetAll);
    outer->addLayout(footer);
    connect(resetAll, &QPushButton::clicked, this, [this] {
        for (Row& row : rows_) {
            if (!KeptOnTabReset(*row.key)) {
                model_.resetToDefault(*row.key);
            }
        }
        refreshFromModel();
        emit tabReset();
    });
    refreshFromModel();
}

QString SettingsTab::tooltipFor(const KeyDef& key) const {
    QString tip;
    const QString help = key.help.text();
    if (!help.isEmpty()) {
        tip = help.toHtmlEscaped() + QStringLiteral("<br><br>");
    }
    QString def = key.defaultText.isEmpty() ? T("tab.default_empty") : ChoiceText(key, key.defaultText);
    if (key.type == KeyType::Resolution && IsAutoResolution(key.defaultText)) {
        def = T("res.auto");
    }
    tip += (key.dynamicDefault ? T("tab.tooltip_default_dynamic") : T("tab.tooltip_default"))
               .arg(def.toHtmlEscaped());
    if (!key.rangeEnforced && (key.min || key.max)) {
        tip += QStringLiteral("<br>") + T("tab.tooltip_range_hint").toHtmlEscaped();
    }
    tip += QStringLiteral("<br><span style=\"font-family:Consolas,monospace\">[%1] %2</span>")
               .arg(key.section, key.name);
    return QStringLiteral("<qt>") + tip + QStringLiteral("</qt>");
}

void SettingsTab::updateRow(Row& row) {
    const KeyDef& key = *row.key;
    ValidationResult result = model_.validate(key);
    if (result.severity == Severity::Ok && key.fullName() == QLatin1String("paths.iso") &&
        model_.value(key).trimmed().isEmpty()) {
        result = ValidationResult{Severity::Warning, IssueCode::FileMissing, T("tab.iso_missing")};
    }
    const QString inapplicable = InapplicableNote(key, model_);
    const QString note = inapplicable.isEmpty() ? InfoNote(key, model_) : inapplicable;
    row.editor->setEnabled(inapplicable.isEmpty());
    if (result.severity == Severity::Ok && !note.isEmpty()) {
        row.message->setText(note);
        QPalette p = row.message->palette();
        p.setColor(QPalette::WindowText, MutedTextColor(row.message));
        row.message->setPalette(p);
        row.message->setVisible(true);
    } else if (result.severity == Severity::Ok) {
        row.message->setVisible(false);
    } else {
        row.message->setText(result.message);
        QPalette p = row.message->palette();
        p.setColor(QPalette::WindowText, SeverityColor(result.severity));
        row.message->setPalette(p);
        row.message->setVisible(true);
    }
    row.label->setText(key.displayLabel() + (model_.isPending(key) ? QStringLiteral(" *") : QString()));
    const bool atDefault = model_.isAtDefault(key);
    QString def = key.defaultText.isEmpty() ? T("tab.default_empty") : ChoiceText(key, key.defaultText);
    if (key.type == KeyType::Resolution && IsAutoResolution(key.defaultText)) {
        def = T("res.auto");
    }
    row.reset->setToolTip(T("tab.reset_key.tip").arg(def));
    row.reset->setVisible(!atDefault);
}

void SettingsTab::refreshFromModel() {
    for (Row& row : rows_) {
        row.editor->refreshContext();
        QString value = model_.value(*row.key);
        if (row.key->dynamicDefault && !model_.isExplicit(*row.key) && context_.dynamicDefault) {
            // Review M3c finding 18: show what applies (frame_rate = the logic rate), not the schema's usual value.
            const QString effective = context_.dynamicDefault(*row.key);
            if (!effective.isEmpty()) {
                value = effective;
            }
        }
        row.editor->setValue(value);
    }
    for (Row& row : rows_) {
        updateRow(row);
    }
}

int SettingsTab::errorCount() const {
    int n = 0;
    for (const Row& row : rows_) {
        n += model_.validate(*row.key).isError() ? 1 : 0;
    }
    return n;
}

bool SettingsTab::focusFirstError() {
    for (Row& row : rows_) {
        if (model_.validate(*row.key).isError()) {
            row.editor->focusTarget()->setFocus(Qt::OtherFocusReason);
            return true;
        }
    }
    return false;
}

void SettingsTab::addTopWidget(QWidget* widget) {
    alignFormLabels(widget);
    topLayout_->addWidget(widget);
}

void SettingsTab::addBottomWidget(QWidget* widget) {
    alignFormLabels(widget);
    bottomLayout_->addWidget(widget);
}

void SettingsTab::alignFormLabels(QWidget* widget) {
    const QList<QLabel*> labels = widget->findChildren<QLabel*>();
    int width = labelWidth_;
    for (const QLabel* l : labels) {
        if (l->property(kFormLabelProperty).toBool()) {
            width = std::max(width, l->sizeHint().width());
        }
    }
    if (width > labelWidth_) {
        labelWidth_ = width;
        for (Row& row : rows_) {
            row.label->setMinimumWidth(width);
        }
    }
    for (QLabel* l : labels) {
        if (l->property(kFormLabelProperty).toBool()) {
            l->setMinimumWidth(width);
        }
    }
}

}  // namespace fl::ui
