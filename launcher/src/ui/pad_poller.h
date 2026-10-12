// Polls the controllers for the "Steuerung" tab while someone needs the states (the live test view or a binding
// capture): connected slots every 16 ms, empty slots only once per second (XInputGetState on an empty slot is the
// expensive call). Users call acquire()/release(); the timer runs while the count is above zero.
#pragma once

#include "core/pad_reader.h"

#include <QObject>
#include <QTimer>

#include <array>
#include <memory>

namespace fl::ui {

class PadPoller : public QObject {
    Q_OBJECT
public:
    explicit PadPoller(std::unique_ptr<PadReader> reader, QObject* parent = nullptr);
    ~PadPoller() override;

    void acquire();
    void release();
    bool active() const { return users_ > 0; }

    // Reads every slot now (empty slots included) and emits updated().
    void pollNow();
    const PadState& state(int slot) const { return states_[static_cast<size_t>(slot)]; }
    int connectedCount() const;
    PadReader& reader() { return *reader_; }
    const PadReader& reader() const { return *reader_; }

    // Short rumble on `slot` (strength 0..1 on both motors) that stops after `ms`.
    bool testRumble(int slot, double strength, int ms);

    static constexpr int kIntervalMs = 16;
    static constexpr int kEmptySlotIntervalMs = 1000;

signals:
    void updated();
    void connectionChanged(int slot, bool connected);

private:
    void poll(bool all);

    std::unique_ptr<PadReader> reader_;
    QTimer timer_;
    int users_ = 0;
    qint64 lastFullPoll_ = 0;
    std::array<PadState, kPadSlots> states_{};
    std::array<QTimer*, kPadSlots> rumbleStop_{};
};

}  // namespace fl::ui
