// Keyboard and mouse source (see kbm_source.h).
#include "input/kbm_source.h"

#include "input/input_config.h"

#include <algorithm>

namespace inp {
namespace {

constexpr int kMaxQueuedNotches = 3;
constexpr std::int64_t kMaxAccumulatedCounts = 1 << 20;  // a port nobody samples must not overflow

constexpr std::uint16_t kVkShift = 0x10, kVkControl = 0x11, kVkMenu = 0x12;
constexpr std::uint16_t kVkLShift = 0xA0, kVkRShift = 0xA1, kVkLControl = 0xA2, kVkRControl = 0xA3;
constexpr std::uint16_t kVkLMenu = 0xA4, kVkRMenu = 0xA5;
constexpr std::uint16_t kRiKeyE0 = 2;

std::int64_t Accumulate(std::int64_t total, int delta) noexcept {
    return std::clamp<std::int64_t>(total + delta, -kMaxAccumulatedCounts, kMaxAccumulatedCounts);
}

}  // namespace

std::uint16_t NormalizeRawKey(std::uint16_t vkey, std::uint16_t makeCode, std::uint16_t flags) noexcept {
    const bool e0 = (flags & kRiKeyE0) != 0;
    switch (vkey) {
    case 0:
    case 0xFF:
        return 0;
    case kVkShift:
        return makeCode == 0x36 ? kVkRShift : kVkLShift;
    case kVkControl:
        return e0 ? kVkRControl : kVkLControl;
    case kVkMenu:
        return e0 ? kVkRMenu : kVkLMenu;
    default:
        return vkey < 256 ? vkey : 0;
    }
}

bool KeyboardMouseSource::WheelLatch::Step() noexcept {
    if (pressLeft > 0) {
        if (--pressLeft == 0) {
            needRelease = true;
        }
        return true;
    }
    if (needRelease) {
        needRelease = false;
        return false;
    }
    if (pending > 0) {
        --pending;
        pressLeft = 1;  // this tick and the next one
        return true;
    }
    return false;
}

void KeyboardMouseSource::Configure(const BindingTable* bindings, float mouseSensitivity, bool menuEscape) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    bindings_ = bindings;
    sensitivity_ = mouseSensitivity;
    menuEscape_ = menuEscape;
}

void KeyboardMouseSource::SetMenuQuery(MenuQuery query) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    menuQuery_ = query;
}

void KeyboardMouseSource::SetEnabled(bool enabled) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled;
    }
    if (!enabled) {
        ReleaseAll();
    }
}

void KeyboardMouseSource::KeyEvent(std::uint16_t vk, bool down) noexcept {
    if (vk == 0 || vk > 0xFF) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        return;
    }
    ++stats_.keyEvents;
    if (down && !down_[vk]) {
        tapped_[vk] = true;
    }
    down_[vk] = down;
}

void KeyboardMouseSource::MouseButtonEvent(int button, bool down) noexcept {
    if (button < 0 || button > 4) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        return;
    }
    ++stats_.mouseEvents;
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << button);
    if (down && (buttonsDown_ & bit) == 0) {
        buttonsTapped_ = static_cast<std::uint8_t>(buttonsTapped_ | bit);
    }
    buttonsDown_ = static_cast<std::uint8_t>(down ? (buttonsDown_ | bit) : (buttonsDown_ & ~bit));
}

void KeyboardMouseSource::WheelEvent(int notches) noexcept {
    if (notches == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        return;
    }
    ++stats_.mouseEvents;
    WheelNotchesLocked(notches);
}

void KeyboardMouseSource::WheelDeltaEvent(int delta) noexcept {
    if (delta == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        return;
    }
    ++stats_.mouseEvents;
    // High-resolution wheels send several records with |delta| < WHEEL_DELTA per detent: a notch is queued only when
    // the sum crosses a multiple of kWheelDelta; a change of direction drops the partial notch.
    if (wheelRemainder_ != 0 && (wheelRemainder_ > 0) != (delta > 0)) {
        wheelRemainder_ = 0;
    }
    wheelRemainder_ += delta;
    const int notches = wheelRemainder_ / kWheelDelta;
    wheelRemainder_ -= notches * kWheelDelta;
    if (notches != 0) {
        WheelNotchesLocked(notches);
    }
}

void KeyboardMouseSource::WheelNotchesLocked(int notches) noexcept {
    rawWheel_ += notches;
    WheelLatch& w = wheel_[notches > 0 ? 0 : 1];
    w.pending = std::min(kMaxQueuedNotches, w.pending + (notches > 0 ? notches : -notches));
}

void KeyboardMouseSource::MouseMoveEvent(int dx, int dy) noexcept {
    if (dx == 0 && dy == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        return;
    }
    ++stats_.mouseEvents;
    dx_ = Accumulate(dx_, dx);
    dy_ = Accumulate(dy_, dy);
    rawDx_ = Accumulate(rawDx_, dx);
    rawDy_ = Accumulate(rawDy_, dy);
}

void KeyboardMouseSource::ReleaseAll() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.releases;
    down_.fill(false);
    tapped_.fill(false);
    buttonsDown_ = 0;
    buttonsTapped_ = 0;
    wheel_[0] = {};
    wheel_[1] = {};
    dx_ = dy_ = 0;
    rawDx_ = rawDy_ = 0;
    rawWheel_ = 0;
    wheelRemainder_ = 0;
}

bool KeyboardMouseSource::KeyDown(std::uint16_t vk) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return vk < 256 && down_[vk];
}

void KeyboardMouseSource::SetPadEmulation(bool on) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    padEmulation_ = on;
    dx_ = dy_ = 0;
}

bool KeyboardMouseSource::Connected() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

bool KeyboardMouseSource::Sample(int port, std::uint64_t tick, PadState& out) noexcept {
    (void)port;
    (void)tick;
    std::unique_lock<std::mutex> lock(mutex_);
    if (!enabled_) {
        return false;
    }
    if (!padEmulation_) {
        tapped_.fill(false);
        buttonsTapped_ = 0;
        dx_ = dy_ = 0;
        return false;
    }
    ++stats_.samples;
    KbmFrame f;
    for (std::size_t i = 0; i < 256; ++i) {
        f.keys[i] = down_[i] || tapped_[i];
    }
    tapped_.fill(false);
    // The generic modifier codes follow their left/right keys (a binding written as key:vk_10 still works).
    f.keys[kVkShift] = f.keys[kVkShift] || f.keys[kVkLShift] || f.keys[kVkRShift];
    f.keys[kVkControl] = f.keys[kVkControl] || f.keys[kVkLControl] || f.keys[kVkRControl];
    f.keys[kVkMenu] = f.keys[kVkMenu] || f.keys[kVkLMenu] || f.keys[kVkRMenu];
    f.mouseButtons = static_cast<std::uint8_t>(buttonsDown_ | buttonsTapped_);
    buttonsTapped_ = 0;
    f.wheel = static_cast<std::uint8_t>((wheel_[0].Step() ? 1 : 0) | (wheel_[1].Step() ? 2 : 0));
    const float scale = sensitivity_ / kMouseCountsPerFullDeflection;
    f.move[0] = dy_ < 0 ? static_cast<float>(-dy_) * scale : 0.0f;  // up
    f.move[1] = dy_ > 0 ? static_cast<float>(dy_) * scale : 0.0f;   // down
    f.move[2] = dx_ < 0 ? static_cast<float>(-dx_) * scale : 0.0f;  // left
    f.move[3] = dx_ > 0 ? static_cast<float>(dx_) * scale : 0.0f;   // right
    dx_ = dy_ = 0;
    const BindingTable& b = bindings_ != nullptr ? *bindings_ : DefaultBindings();
    const bool menuEscape = menuEscape_;
    const MenuQuery query = menuQuery_;
    PadState s = MapKeyboardMouse(f, b);

    // Menu escape: decided when START goes down (outside the lock: the query reads guest memory).
    const bool startDown = (s.buttons & kButtonStart) != 0;
    const bool startEdge = startDown && !startWasDown_;
    startWasDown_ = startDown;
    if (startEdge) {
        lock.unlock();
        const bool inMenu = menuEscape && query != nullptr && query();
        lock.lock();
        startAsBack_ = inMenu;
        if (inMenu) {
            ++stats_.menuEscapes;
        }
    }
    if (startDown && startAsBack_) {
        s.buttons = static_cast<std::uint16_t>((s.buttons & ~kButtonStart) | kButtonBack);
    }
    if (!startDown) {
        startAsBack_ = false;
    }
    if (!s.Neutral()) {
        ++stats_.activeSamples;
    }
    out = s;
    return true;
}

KeyboardMouseSource::RawSnapshot KeyboardMouseSource::ReadRaw(bool consumeMotion) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    RawSnapshot r;
    r.keys = down_;
    r.mouseButtons = buttonsDown_;
    r.dx = rawDx_;
    r.dy = rawDy_;
    r.wheel = rawWheel_;
    if (consumeMotion) {
        rawDx_ = rawDy_ = 0;
        rawWheel_ = 0;
    }
    return r;
}

KeyboardMouseSource::Stats KeyboardMouseSource::GetStats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace inp
