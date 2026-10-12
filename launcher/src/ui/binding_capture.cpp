#include "ui/binding_capture.h"

#include "core/l10n.h"
#include "ui/app_style.h"
#include "ui/pad_poller.h"

#include <QAbstractNativeEventFilter>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <windows.h>

namespace fl::ui {
namespace {

// The field that takes mouse buttons (outside of it the dialog's own buttons work normally).
class MouseCaptureField final : public QFrame {
public:
    MouseCaptureField(BindingCaptureDialog* dialog, QWidget* parent) : QFrame(parent), dialog_(dialog) {
        setObjectName(QStringLiteral("mouseCaptureField"));
        setFrameShape(QFrame::StyledPanel);
        setFrameShadow(QFrame::Sunken);
        setMinimumHeight(fontMetrics().height() * 5);
        setCursor(Qt::CrossCursor);
        auto* layout = new QVBoxLayout(this);
        auto* text = new QLabel(T("capture.mouse_field"), this);
        text->setAlignment(Qt::AlignCenter);
        text->setWordWrap(true);
        text->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(text);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (const auto token = MouseButtonToken(event->button())) {
            event->accept();
            dialog_->capture(*token);
            return;
        }
        QFrame::mousePressEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent* event) override { event->accept(); }

private:
    BindingCaptureDialog* dialog_;
};

}  // namespace

// Application-wide filter while a keyboard/mouse capture is open: the key message reaches the dialog's (or any
// other) window procedure; only key-down messages are recorded, nothing is filtered out.
class BindingCaptureDialog::NativeFilter final : public QAbstractNativeEventFilter {
public:
    explicit NativeFilter(BindingCaptureDialog* dialog) : dialog_(dialog) {}
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* /*result*/) override {
        if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
            dialog_->noteNativeMessage(message);
        }
        return false;
    }

private:
    BindingCaptureDialog* dialog_;
};

BindingCaptureDialog::BindingCaptureDialog(BindDevice device, const QString& title, PadPoller* poller,
                                           QWidget* parent)
    : QDialog(parent), device_(device), poller_(poller) {
    setObjectName(QStringLiteral("bindingCapture"));
    setWindowTitle(T("capture.window_title"));
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    const int unit = fontMetrics().height();
    layout->setSpacing(unit / 2);
    auto* heading = new QLabel(title, this);
    ScaleFont(heading, 1.2, true);
    heading->setWordWrap(true);
    layout->addWidget(heading);
    note_ = new QLabel(this);
    note_->setObjectName(QStringLiteral("captureNote"));
    note_->setWordWrap(true);
    note_->setVisible(false);
    layout->addWidget(note_);
    auto* prompt = new QLabel(device == BindDevice::Pad ? T("capture.prompt_pad") : T("capture.prompt_kbm"), this);
    prompt->setObjectName(QStringLiteral("capturePrompt"));
    prompt->setWordWrap(true);
    layout->addWidget(prompt);

    if (device == BindDevice::KeyboardMouse) {
        layout->addWidget(new MouseCaptureField(this, this));
        auto* moves = new QHBoxLayout();
        moves->addWidget(new QLabel(T("capture.mouse_move"), this));
        for (const char* name : {"move_up", "move_down", "move_left", "move_right"}) {
            const QString token = QStringLiteral("mouse:") + QLatin1String(name);
            auto* b = new QPushButton(TokenDisplayText(token), this);
            b->setObjectName(QStringLiteral("capture:") + token);
            b->setAutoDefault(false);
            b->setFocusPolicy(Qt::NoFocus);
            connect(b, &QPushButton::clicked, this, [this, token] { capture(token); });
            moves->addWidget(b);
        }
        moves->addStretch(1);
        layout->addLayout(moves);
    } else {
        // Without a controller (or without XInput) the status says why nothing happens; the list below works
        // either way (review M4 finding 8).
        padStatus_ = new QLabel(this);
        padStatus_->setObjectName(QStringLiteral("capturePadStatus"));
        padStatus_->setWordWrap(true);
        QPalette p = padStatus_->palette();
        p.setColor(QPalette::WindowText, SeverityColor(Severity::Warning));
        padStatus_->setPalette(p);
        layout->addWidget(padStatus_);
        auto* pick = new QHBoxLayout();
        pick->addWidget(new QLabel(T("capture.pad_list"), this));
        padList_ = new QComboBox(this);
        padList_->setObjectName(QStringLiteral("capturePadList"));
        for (const QString& token : AllTokens(BindDevice::Pad)) {
            padList_->addItem(TokenDisplayText(token), token);
        }
        pick->addWidget(padList_, 1);
        auto* use = new QPushButton(T("capture.pad_list_use"), this);
        use->setObjectName(QStringLiteral("capturePadListUse"));
        use->setAutoDefault(false);
        connect(use, &QPushButton::clicked, this, [this] { capture(padList_->currentData().toString()); });
        pick->addWidget(use);
        layout->addLayout(pick);
        if (poller_ != nullptr) {
            connect(poller_, &PadPoller::connectionChanged, this, [this](int, bool) { updatePadStatus(); });
        }
    }

    auto* bottom = new QHBoxLayout();
    countdownLabel_ = new QLabel(this);
    QPalette muted = countdownLabel_->palette();
    muted.setColor(QPalette::WindowText, MutedTextColor(countdownLabel_));
    countdownLabel_->setPalette(muted);
    bottom->addWidget(countdownLabel_, 1);
    auto* cancel = new QPushButton(T("common.cancel"), this);
    cancel->setObjectName(QStringLiteral("captureCancel"));
    cancel->setAutoDefault(false);
    cancel->setFocusPolicy(Qt::NoFocus);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    bottom->addWidget(cancel);
    layout->addLayout(bottom);

    countdown_ = new QTimer(this);
    countdown_->setInterval(1000);
    connect(countdown_, &QTimer::timeout, this, &BindingCaptureDialog::tick);
    pendingTimer_ = new QTimer(this);
    pendingTimer_->setSingleShot(true);
    pendingTimer_->setInterval(40);
    connect(pendingTimer_, &QTimer::timeout, this, &BindingCaptureDialog::finishPendingKey);
    countdownLabel_->setText(T("capture.countdown").arg(remaining_));
    setMinimumWidth(unit * 30);
    updatePadStatus();
}

BindingCaptureDialog::~BindingCaptureDialog() {
    if (polling_ && poller_ != nullptr) {
        poller_->release();
    }
    if (nativeFilter_ != nullptr) {
        QCoreApplication::instance()->removeNativeEventFilter(nativeFilter_);
        delete nativeFilter_;
    }
}

void BindingCaptureDialog::setNote(const QString& text) {
    note_->setText(text);
    note_->setVisible(!text.isEmpty());
}

void BindingCaptureDialog::noteNativeMessage(const void* message) {
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg == nullptr || (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN)) {
        return;
    }
    nativeVk_ = static_cast<quint32>(msg->wParam);
    nativeExtended_ = (HIWORD(msg->lParam) & KF_EXTENDED) != 0;
    nativeValid_ = true;
}

// Scan code with the extended bit 0x100 as KeyTokenFromEvent expects it: from the native message when it belongs to
// this key press, else Qt's value; for Ctrl/Alt without a native message the keyboard state decides.
quint32 BindingCaptureDialog::scanCodeFor(const QKeyEvent* event) const {
    quint32 scan = event->nativeScanCode();
    const quint32 vk = event->nativeVirtualKey();
    if (nativeValid_ && nativeVk_ == vk) {
        return (scan & 0xFF) | (nativeExtended_ ? 0x100u : 0u);
    }
    if (vk == VK_CONTROL && (scan & 0x100) == 0 && (GetKeyState(VK_RCONTROL) & 0x8000) != 0 &&
        (GetKeyState(VK_LCONTROL) & 0x8000) == 0) {
        scan |= 0x100;
    } else if (vk == VK_MENU && (scan & 0x100) == 0 && (GetKeyState(VK_RMENU) & 0x8000) != 0 &&
               (GetKeyState(VK_LMENU) & 0x8000) == 0) {
        scan |= 0x100;
    }
    return scan;
}

void BindingCaptureDialog::updatePadStatus() {
    if (padStatus_ == nullptr) {
        return;
    }
    const bool available = poller_ != nullptr && poller_->reader().available();
    const bool none = !available || poller_->connectedCount() == 0;
    waitingForPad_ = none;
    padStatus_->setText(!available ? T("capture.no_xinput") : T("capture.no_pad"));
    padStatus_->setVisible(none);
    countdownLabel_->setText(none ? T("capture.waiting") : T("capture.countdown").arg(remaining_));
}

void BindingCaptureDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    remaining_ = kTimeoutSeconds;
    countdownLabel_->setText(T("capture.countdown").arg(remaining_));
    countdown_->start();
    if (device_ == BindDevice::KeyboardMouse) {
        nativeValid_ = false;
        if (nativeFilter_ == nullptr) {
            nativeFilter_ = new NativeFilter(this);
            QCoreApplication::instance()->installNativeEventFilter(nativeFilter_);
        }
        grabKeyboard();
    } else if (poller_ != nullptr && !polling_) {
        polling_ = true;
        baselineTaken_ = false;
        connect(poller_, &PadPoller::updated, this, &BindingCaptureDialog::onPadUpdate);
        poller_->acquire();
    }
    updatePadStatus();
}

void BindingCaptureDialog::hideEvent(QHideEvent* event) {
    countdown_->stop();
    if (device_ == BindDevice::KeyboardMouse) {
        releaseKeyboard();
        if (nativeFilter_ != nullptr) {
            QCoreApplication::instance()->removeNativeEventFilter(nativeFilter_);
            delete nativeFilter_;
            nativeFilter_ = nullptr;
        }
    }
    if (polling_ && poller_ != nullptr) {
        disconnect(poller_, &PadPoller::updated, this, &BindingCaptureDialog::onPadUpdate);
        poller_->release();
        polling_ = false;
    }
    QDialog::hideEvent(event);
}

void BindingCaptureDialog::tick() {
    if (waitingForPad_) {
        updatePadStatus();  // no controller: the countdown waits (the list still works)
        if (waitingForPad_) {
            return;
        }
    }
    if (--remaining_ <= 0) {
        reject();
        return;
    }
    countdownLabel_->setText(T("capture.countdown").arg(remaining_));
}

void BindingCaptureDialog::capture(const QString& token) {
    if (!token_.isEmpty() || token.isEmpty()) {
        return;
    }
    if (TokenDevice(token) != device_) {
        return;
    }
    token_ = token;
    emit captured(token);
    accept();
}

bool BindingCaptureDialog::event(QEvent* event) {
    if (device_ == BindDevice::KeyboardMouse) {
        // Keep Tab/Esc/Enter and shortcuts away from the dialog's own handling: they are keys to bind.
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            keyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        }
        if (event->type() == QEvent::KeyRelease) {
            keyReleaseEvent(static_cast<QKeyEvent*>(event));
            return true;
        }
    }
    return QDialog::event(event);
}

void BindingCaptureDialog::keyPressEvent(QKeyEvent* event) {
    if (device_ != BindDevice::KeyboardMouse) {
        QDialog::keyPressEvent(event);
        return;
    }
    event->accept();
    if (event->isAutoRepeat()) {
        return;
    }
    const auto token = KeyTokenFromEvent(event->key(), event->modifiers(), event->nativeVirtualKey(),
                                         scanCodeFor(event));
    nativeValid_ = false;
    if (!token) {
        return;
    }
    if (*token == QLatin1String("key:lctrl") && event->nativeVirtualKey() != 0) {
        // AltGr arrives as left Ctrl followed by right Alt: wait a moment for the second message.
        pendingKey_ = *token;
        pendingTimer_->start();
        return;
    }
    pendingTimer_->stop();
    pendingKey_.clear();
    capture(*token);
}

// Print Screen: Windows reports only the key release (WM_KEYUP VK_SNAPSHOT).
void BindingCaptureDialog::keyReleaseEvent(QKeyEvent* event) {
    if (device_ != BindDevice::KeyboardMouse) {
        QDialog::keyReleaseEvent(event);
        return;
    }
    event->accept();
    if (!event->isAutoRepeat() && (event->key() == Qt::Key_Print || event->nativeVirtualKey() == VK_SNAPSHOT)) {
        capture(QStringLiteral("key:printscreen"));
    }
}

void BindingCaptureDialog::finishPendingKey() {
    if (!pendingKey_.isEmpty()) {
        const QString key = pendingKey_;
        pendingKey_.clear();
        capture(key);
    }
}

void BindingCaptureDialog::wheelEvent(QWheelEvent* event) {
    if (device_ == BindDevice::KeyboardMouse) {
        const QString token = WheelToken(event->angleDelta().y());
        if (!token.isEmpty()) {
            event->accept();
            capture(token);
            return;
        }
    }
    QDialog::wheelEvent(event);
}

void BindingCaptureDialog::onPadUpdate() {
    if (waitingForPad_ != (poller_->connectedCount() == 0)) {
        updatePadStatus();
    }
    QSet<QString> active;
    for (int slot = 0; slot < kPadSlots; ++slot) {
        active.unite(ActivePadTokens(poller_->state(slot)));
    }
    if (!baselineTaken_) {
        held_ = active;
        baselineTaken_ = true;
        return;
    }
    held_.intersect(active);  // released inputs leave the baseline
    QSet<QString> fresh = active;
    fresh.subtract(held_);
    const QStringList sorted = SortedPadTokens(fresh);
    if (!sorted.isEmpty()) {
        capture(sorted.first());
    }
}

}  // namespace fl::ui
