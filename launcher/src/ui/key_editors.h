// One editor widget per schema key, chosen by ui.widget: combo, check, spin, dspin, text, file, dir,
// resolution (monitor modes + "Automatic (native)" + free input), monitor (outputs of query-gpu / QScreen).
// Editors speak INI text: setValue() shows a value without emitting, a user change emits edited().
#pragma once

#include "core/config_schema.h"
#include "core/gpu_info.h"

#include <QString>
#include <QWidget>

#include <functional>

class QComboBox;
class QLineEdit;

namespace fl::ui {

struct EditorContext {
    const GpuInfo* gpu = nullptr;
    const ConfigSchema* schema = nullptr;
    std::function<QString()> iniDir;                     // base of relative paths
    std::function<QString(const KeyDef&)> currentValue;  // effective value of another key
    std::function<QString(const KeyDef&)> placeholder;   // shown in empty path fields
    // Effective value of a key with a dynamic default while it is not set (empty = unknown: the schema default).
    std::function<QString(const KeyDef&)> dynamicDefault;
};

class KeyEditor : public QWidget {
    Q_OBJECT
public:
    KeyEditor(const KeyDef& key, const EditorContext& context, QWidget* parent);
    const KeyDef& key() const { return key_; }

    virtual void setValue(const QString& value) = 0;
    virtual QString value() const = 0;
    // Another key changed (e.g. the monitor for the resolution list).
    virtual void refreshContext() {}
    // The widget that should get the focus / the label buddy.
    virtual QWidget* focusTarget() = 0;

signals:
    void edited(const QString& value);

protected:
    void emitEdited();

    const KeyDef& key_;
    EditorContext context_;
    bool updating_ = false;
};

KeyEditor* CreateKeyEditor(const KeyDef& key, const EditorContext& context, QWidget* parent);

// Display text of a choice value (label of the choice, or the value itself).
QString ChoiceText(const KeyDef& key, const QString& value);

// The output a monitor key value refers to (device name, name, or index for int keys), else nullptr.
const GpuOutput* OutputForMonitorValue(const KeyDef& key, const QString& value, const GpuInfo& gpu);
// The output to use for resolution lists: the monitor key's output, else the primary one.
const GpuOutput* SelectedOutput(const EditorContext& context);

}  // namespace fl::ui
