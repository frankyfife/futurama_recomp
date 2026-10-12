// "Press a key / button" dialog of the binding tables. Keyboard and mouse: every key incl. Esc, Tab and Enter is
// captured (the keyboard is grabbed; cancel with the button or wait for the countdown), mouse buttons inside the
// capture field, the wheel anywhere in the dialog, mouse movement through four direction buttons. Controller: the
// first button, trigger or stick direction that becomes active after the dialog opened (inputs held at that moment
// count only after they were released), or an input picked from the list (works without a connected controller);
// while no controller is connected the countdown waits.
//
// Left/right Ctrl and Alt: Qt's nativeScanCode does not reliably carry the extended bit on the Windows platform, so
// the dialog reads it from the WM_KEYDOWN/WM_SYSKEYDOWN message itself (lParam bit 24, KF_EXTENDED) through a
// native event filter (review M4 finding 7). Print Screen only reports key-up and is captured on the release.
#pragma once

#include "core/bindings.h"

#include <QDialog>
#include <QSet>
#include <QString>

class QComboBox;
class QLabel;
class QTimer;

namespace fl::ui {

class PadPoller;

class BindingCaptureDialog : public QDialog {
    Q_OBJECT
public:
    // `title`: what is being bound ("A – Bestätigen / Angriff 1"); `poller` is used for the controller (may be nullptr).
    BindingCaptureDialog(BindDevice device, const QString& title, PadPoller* poller, QWidget* parent = nullptr);
    ~BindingCaptureDialog() override;

    BindDevice device() const { return device_; }
    // The captured token after accept(), empty otherwise.
    const QString& token() const { return token_; }
    static constexpr int kTimeoutSeconds = 10;

    // Delivers a token as if it was captured (direction buttons, tests).
    void capture(const QString& token);
    // An extra line under the heading (e.g. what a replacement overwrites); empty hides it.
    void setNote(const QString& text);
    // A native Windows message (MSG*) seen by the dialog's event filter: WM_KEYDOWN/WM_SYSKEYDOWN record the
    // virtual-key code and the extended bit for the next key press. Public for tests.
    void noteNativeMessage(const void* msg);
    // Pad dialog: no controller connected (the countdown waits, the list still works).
    bool waitingForPad() const { return waitingForPad_; }

signals:
    void captured(const QString& token);

protected:
    bool event(QEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void onPadUpdate();
    void updatePadStatus();
    void tick();
    void finishPendingKey();
    quint32 scanCodeFor(const QKeyEvent* event) const;

    BindDevice device_;
    PadPoller* poller_ = nullptr;
    bool polling_ = false;
    bool baselineTaken_ = false;
    QSet<QString> held_;  // pad tokens active when the dialog opened (ignored until released)
    QString token_;
    QString pendingKey_;  // left Ctrl waits a moment for AltGr (Windows sends LCtrl + RAlt)
    QTimer* pendingTimer_ = nullptr;
    QTimer* countdown_ = nullptr;
    int remaining_ = kTimeoutSeconds;
    QLabel* countdownLabel_ = nullptr;
    QLabel* note_ = nullptr;
    QLabel* padStatus_ = nullptr;
    QComboBox* padList_ = nullptr;
    bool waitingForPad_ = false;
    // Last native key-down message (noteNativeMessage).
    quint32 nativeVk_ = 0;
    bool nativeExtended_ = false;
    bool nativeValid_ = false;
    class NativeFilter;
    NativeFilter* nativeFilter_ = nullptr;
};

}  // namespace fl::ui
