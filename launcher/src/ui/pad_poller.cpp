#include "ui/pad_poller.h"

#include <QDateTime>

namespace fl::ui {

PadPoller::PadPoller(std::unique_ptr<PadReader> reader, QObject* parent)
    : QObject(parent), reader_(std::move(reader)) {
    timer_.setInterval(kIntervalMs);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool all = now - lastFullPoll_ >= kEmptySlotIntervalMs;
        poll(all);
    });
    for (size_t i = 0; i < rumbleStop_.size(); ++i) {
        auto* stop = new QTimer(this);
        stop->setSingleShot(true);
        const int slot = static_cast<int>(i);
        connect(stop, &QTimer::timeout, this, [this, slot] { reader_->setRumble(slot, 0.0, 0.0); });
        rumbleStop_[i] = stop;
    }
}

PadPoller::~PadPoller() {
    for (size_t i = 0; i < rumbleStop_.size(); ++i) {
        if (rumbleStop_[i]->isActive()) {
            reader_->setRumble(static_cast<int>(i), 0.0, 0.0);
        }
    }
}

void PadPoller::acquire() {
    if (users_++ == 0) {
        poll(true);
        timer_.start();
    }
}

void PadPoller::release() {
    if (users_ > 0 && --users_ == 0) {
        timer_.stop();
    }
}

void PadPoller::pollNow() {
    poll(true);
}

int PadPoller::connectedCount() const {
    int n = 0;
    for (const PadState& s : states_) {
        n += s.connected ? 1 : 0;
    }
    return n;
}

bool PadPoller::testRumble(int slot, double strength, int ms) {
    if (slot < 0 || slot >= kPadSlots || !states_[static_cast<size_t>(slot)].connected) {
        return false;
    }
    if (!reader_->setRumble(slot, strength, strength)) {
        return false;
    }
    rumbleStop_[static_cast<size_t>(slot)]->start(ms);
    return true;
}

void PadPoller::poll(bool all) {
    if (all) {
        lastFullPoll_ = QDateTime::currentMSecsSinceEpoch();
    }
    for (int slot = 0; slot < kPadSlots; ++slot) {
        PadState& current = states_[static_cast<size_t>(slot)];
        if (!all && !current.connected) {
            continue;
        }
        const PadState next = reader_->read(slot);
        const bool changed = next.connected != current.connected;
        current = next;
        if (changed) {
            emit connectionChanged(slot, next.connected);
        }
    }
    emit updated();
}

}  // namespace fl::ui
