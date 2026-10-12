// One tab (Spiel / Grafik / Pfade / Erweitert) built from the schema: the keys of the tab grouped by
// INI section, label + editor + per-key reset button + inline validation message, tooltips from the
// schema's help text, and "Auf Standard zuruecksetzen" for the whole tab.
#pragma once

#include "core/settings_model.h"
#include "ui/key_editors.h"

#include <QList>
#include <QWidget>

#include <functional>

class QLabel;
class QToolButton;
class QVBoxLayout;

namespace fl::ui {

class SettingsTab : public QWidget {
    Q_OBJECT
public:
    // `visible` filters the keys of the tab (e.g. developer keys); nullptr = all keys.
    SettingsTab(UiTab tab, SettingsModel& model, const EditorContext& context,
                std::function<bool(const KeyDef&)> visible = nullptr, QWidget* parent = nullptr);

    UiTab tab() const { return tab_; }
    // Re-reads every value from the model and refreshes the messages.
    void refreshFromModel();
    int errorCount() const;
    int keyCount() const { return static_cast<int>(rows_.size()); }
    // Focuses the first editor with an error; false if there is none.
    bool focusFirstError();
    // Extra content above the key groups / below them (before the reset button). Labels in it that carry the
    // property kFormLabelProperty get the width of the tab's label column (rows line up with the key rows).
    void addTopWidget(QWidget* widget);
    void addBottomWidget(QWidget* widget);
    static constexpr const char* kFormLabelProperty = "flFormLabel";
    // The scrolled content (screenshots of the whole tab).
    QWidget* contentWidget() const { return content_; }
    // M4: errors of editors outside the key rows (the binding tables of the controls tab) count for the footer
    // and focusFirstError(); `focus` focuses the first of them.
    void setExtraErrors(std::function<int()> count, std::function<bool()> focus);
    // M4: rows outside the generic list that "Auf Standard zurücksetzen" also resets (the binding tables); counted for
    // the confirmation.
    void setExtraResetCount(std::function<int()> count);
    // M4: hides "this game build has no settings here" (the tab has other content, e.g. the binding tables).
    void setEmptyNoteVisible(bool visible);

signals:
    void valueEdited(const fl::KeyDef* key);
    void tabReset();

private:
    struct Row {
        const KeyDef* key = nullptr;
        KeyEditor* editor = nullptr;
        QLabel* label = nullptr;
        QLabel* message = nullptr;
        QToolButton* reset = nullptr;
    };
    void updateRow(Row& row);
    void alignFormLabels(QWidget* widget);
    QString tooltipFor(const KeyDef& key) const;

    UiTab tab_;
    SettingsModel& model_;
    EditorContext context_;
    QList<Row> rows_;
    QVBoxLayout* topLayout_ = nullptr;
    QVBoxLayout* bottomLayout_ = nullptr;
    QWidget* content_ = nullptr;
    QLabel* emptyNote_ = nullptr;
    std::function<int()> extraErrorCount_;
    std::function<bool()> extraFocus_;
    std::function<int()> extraResetCount_;
    int labelWidth_ = 0;
};

}  // namespace fl::ui
