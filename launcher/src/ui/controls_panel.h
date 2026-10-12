// The "Steuerung" tab's own widgets (the remaining [input] keys - sliders, rumble, controller mode - are ordinary
// SettingsTab rows):
//   BindingsPanel  one binding table per device (Controller / Tastatur und Maus) built from the schema's binding
//                  keys (core/bindings.h): Xbox control, its meaning in the game (schema label), the bound inputs,
//                  "press a key/button" capture (replace or add), clear, per-key and per-device reset, conflict
//                  warnings (one physical input on two controls of the same device) and inline errors.
//   PadTestPanel   live controller test: 4 XInput slots with buttons, sticks and triggers, which Xbox controls the
//                  current pad bindings make of it, and a short rumble test.
#pragma once

#include "core/bindings.h"
#include "core/settings_model.h"
#include "ui/binding_capture.h"

#include <QGroupBox>
#include <QList>
#include <QPointer>
#include <QWidget>

#include <array>
#include <functional>

class QLabel;
class QPushButton;
class QTabWidget;
class QToolButton;

namespace fl::ui {

class PadPoller;

class BindingsPanel : public QWidget {
    Q_OBJECT
public:
    // `visible` filters keys like the settings tabs (developer keys); nullptr = all binding keys.
    BindingsPanel(SettingsModel& model, PadPoller* poller, std::function<bool(const KeyDef&)> visible,
                  QWidget* parent = nullptr);

    bool hasBindings() const { return !rows_.isEmpty(); }
    int rowCount(BindDevice device) const;
    void refreshFromModel();
    int errorCount() const;
    bool focusFirstError();
    void selectDevice(BindDevice device);
    QTabWidget* deviceTabs() const { return deviceTabs_; }
    // Resets every binding key of `device` to the schema default.
    void resetDevice(BindDevice device);
    void resetAll();

    // Opens the capture dialog (non-blocking) for `key`: replace = the captured input becomes the only binding,
    // add = it is added to the list.
    BindingCaptureDialog* openCapture(const KeyDef& key, bool add);
    BindingCaptureDialog* activeCapture() const { return capture_; }
    // What the capture does with a token (also used by tests).
    void applyToken(const KeyDef& key, const QString& token, bool add);
    // Conflict warning of `key` (empty if none).
    QString conflictText(const KeyDef& key) const;
    // Rows of `device` (all rows) that differ from the schema default (the resets ask before changing several).
    int changedRowCount(BindDevice device) const;
    int changedRowCount() const;

signals:
    void edited();

private:
    struct Row {
        const KeyDef* key = nullptr;
        BindDevice device = BindDevice::Pad;
        QString control;
        QLabel* name = nullptr;
        QLabel* meaning = nullptr;
        QPushButton* binding = nullptr;
        QToolButton* add = nullptr;
        QToolButton* clear = nullptr;
        QToolButton* reset = nullptr;
        QLabel* message = nullptr;
    };
    QWidget* buildDevicePage(BindDevice device);
    QString reservedText(const Row& row, const QStringList& tokens) const;
    void updateRow(Row& row, const QHash<QString, QStringList>& conflicts);
    QHash<QString, QStringList> conflictsOf(BindDevice device) const;
    QList<BindingRow> bindingRows(BindDevice device) const;
    void changed();

    SettingsModel& model_;
    PadPoller* poller_ = nullptr;
    QList<Row> rows_;
    QTabWidget* deviceTabs_ = nullptr;
    std::array<QLabel*, 2> summary_{};
    QPointer<BindingCaptureDialog> capture_;
};

class PadView;

class PadTestPanel : public QGroupBox {
    Q_OBJECT
public:
    PadTestPanel(SettingsModel& model, PadPoller* poller, QWidget* parent = nullptr);
    ~PadTestPanel() override;
    // Re-reads the bindings and rumble settings (after edits).
    void refreshFromModel();
    // Text of the "pressed" line of a slot (tests).
    QString pressedText(int slot) const;
    QString gameText(int slot) const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updateFromPoller();
    double rumbleStrength(bool* enabled) const;

    struct SlotWidgets {
        QWidget* frame = nullptr;
        QLabel* title = nullptr;
        PadView* view = nullptr;
        QLabel* pressed = nullptr;
        QLabel* game = nullptr;
        QPushButton* rumble = nullptr;
    };
    SettingsModel& model_;
    PadPoller* poller_ = nullptr;
    bool polling_ = false;
    QLabel* status_ = nullptr;
    QLabel* absent_ = nullptr;
    QLabel* note_ = nullptr;
    std::array<SlotWidgets, kPadSlots> slotWidgets_{};
};

}  // namespace fl::ui
