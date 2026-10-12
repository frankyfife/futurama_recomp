// Input router of the PC port (see host_router.h).
#include "input/host_router.h"

#include "core/log.h"

#include <algorithm>
#include <cstdlib>

namespace inp {
namespace {

// A stick axis counts as held beyond the game's reduced dead zone 0.2 (m4_input.md 3.2).
constexpr int kHeldAxis = 6554;  // 0.2 * 32768

int PortOf(const InputSource* source) noexcept {
    return std::clamp(source->DefaultPort(), 0, kPortCount - 1);
}

RumbleState Scale(const RumbleState& r, std::uint32_t percent) noexcept {
    const std::uint32_t p = std::min<std::uint32_t>(percent, 100);
    return {static_cast<std::uint16_t>(r.left * p / 100u), static_cast<std::uint16_t>(r.right * p / 100u)};
}

bool NonZero(const RumbleState& r) noexcept {
    return r.left != 0 || r.right != 0;
}

// A deliberate input that makes a controller the port's rumble target ("most recently used", E7): a button, an
// analog button from the game's channel threshold on, or a stick beyond the held threshold 0.2. A real pad at rest
// reports small non-zero stick values (and [input] pad_deadzone is 0 by default), so exact neutrality would make
// every resting pad "used" on every tick.
bool Deliberate(const PadState& s) noexcept {
    if (s.buttons != 0) {
        return true;
    }
    for (int i = 0; i < kAnalogCount; ++i) {
        if (s.analog[i] >= kAnalogPressThreshold) {
            return true;
        }
    }
    for (int i = 0; i < kAxisCount; ++i) {
        if (std::abs(static_cast<int>(s.thumb[i])) > kHeldAxis) {
            return true;
        }
    }
    return false;
}

}  // namespace

void HostRouter::Configure(const Options& options) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    options_ = options;
    for (int p = 0; p < kPortCount; ++p) {
        ApplyRumbleLocked(p);
    }
}

void HostRouter::SetFocusQuery(FocusQuery query) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    focusQuery_ = query;
    lastFocused_ = FocusedLocked();
}

void HostRouter::UseSources(const std::vector<InputSource*>& sources) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
        sources_ = sources;
        explicitSources_ = true;
    } catch (...) {
        sources_.clear();
    }
}

void HostRouter::UseRegistry() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sources_.clear();
    explicitSources_ = false;
}

std::vector<InputSource*> HostRouter::SourcesLocked() const {
    if (explicitSources_) {
        return sources_;
    }
    std::vector<InputSource*> out;
    const int n = SourceCount();
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (InputSource* s = SourceAt(i)) {
            out.push_back(s);
        }
    }
    return out;
}

bool HostRouter::PortConnectedLocked(int port) const noexcept {
    if (forcedOff_[port]) {
        return false;
    }
    try {
        for (InputSource* s : SourcesLocked()) {
            if (PortOf(s) == port && s->Connected()) {
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

void HostRouter::UpdateConnectionsLocked() const noexcept {
    for (int p = 0; p < kPortCount; ++p) {
        const bool connected = PortConnectedLocked(p);
        if (connected != lastConnected_[p]) {
            lastConnected_[p] = connected;
            if (connected) {
                ++epoch_[p];
            }
            FUT_LOGI(Inp, "input: port %d %s (connection #%u)", p + 1, connected ? "connected" : "disconnected",
                     epoch_[p]);
        }
        stats_[p].connected = connected;
        stats_[p].epoch = epoch_[p];
    }
}

bool HostRouter::FocusedLocked() const noexcept {
    return focusQuery_ == nullptr || focusQuery_();
}

std::uint32_t HostRouter::ConnectedPorts() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    UpdateConnectionsLocked();
    std::uint32_t mask = 0;
    for (int p = 0; p < kPortCount; ++p) {
        if (lastConnected_[p]) {
            mask |= 1u << p;
        }
    }
    return mask;
}

std::uint32_t HostRouter::PortEpoch(int port) const noexcept {
    if (port < 0 || port >= kPortCount) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    UpdateConnectionsLocked();
    return epoch_[port];
}

void HostRouter::SimulateUnplug(int port, bool unplugged) noexcept {
    if (port < 0 || port >= kPortCount) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (forcedOff_[port] == unplugged) {
        return;
    }
    forcedOff_[port] = unplugged;
    FUT_LOGI(Inp, "input: port %d simulated %s", port + 1, unplugged ? "unplug" : "replug");
    if (!unplugged && lastConnected_[port]) {
        ++epoch_[port];  // unplugged and replugged between two hotplug checks: a re-connection
    }
    if (unplugged) {
        ApplyRumbleLocked(port);
    }
}

bool HostRouter::ApplySuppress(Suppress& s, PadState& state) noexcept {
    if (s.pending) {
        s.pending = false;
        s.buttons = state.buttons;
        s.analog = 0;
        for (int i = 0; i < kAnalogCount; ++i) {
            if (state.analog[i] >= kAnalogPressThreshold) {
                s.analog = static_cast<std::uint8_t>(s.analog | (1u << i));
            }
        }
        s.axes = 0;
        for (int i = 0; i < kAxisCount; ++i) {
            if (std::abs(static_cast<int>(state.thumb[i])) > kHeldAxis) {
                s.axes = static_cast<std::uint8_t>(s.axes | (1u << i));
            }
        }
    }
    if (!s.Any()) {
        return false;
    }
    bool kept = false;
    for (int bit = 0; bit < 16; ++bit) {
        const std::uint16_t m = static_cast<std::uint16_t>(1u << bit);
        if ((s.buttons & m) == 0) {
            continue;
        }
        if ((state.buttons & m) != 0) {
            state.buttons = static_cast<std::uint16_t>(state.buttons & ~m);
            kept = true;
        } else {
            s.buttons = static_cast<std::uint16_t>(s.buttons & ~m);
        }
    }
    for (int i = 0; i < kAnalogCount; ++i) {
        const std::uint8_t m = static_cast<std::uint8_t>(1u << i);
        if ((s.analog & m) == 0) {
            continue;
        }
        if (state.analog[i] >= kAnalogPressThreshold) {
            state.analog[i] = 0;
            kept = true;
        } else {
            s.analog = static_cast<std::uint8_t>(s.analog & ~m);
        }
    }
    for (int i = 0; i < kAxisCount; ++i) {
        const std::uint8_t m = static_cast<std::uint8_t>(1u << i);
        if ((s.axes & m) == 0) {
            continue;
        }
        if (std::abs(static_cast<int>(state.thumb[i])) > kHeldAxis) {
            state.thumb[i] = 0;
            kept = true;
        } else {
            s.axes = static_cast<std::uint8_t>(s.axes & ~m);
        }
    }
    return kept;
}

void HostRouter::RefreshFocus() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const bool focused = FocusedLocked();
    if (focused == lastFocused_) {
        return;
    }
    lastFocused_ = focused;
    ++focusChanges_;
    FUT_LOGD(Inp, "input: game window %s the focus", focused ? "has" : "lost");
    for (int p = 0; p < kPortCount; ++p) {
        if (focused && !options_.backgroundInput) {
            suppressPhysical_[p].pending = true;  // held controls count again only after a release
        }
        ApplyRumbleLocked(p);
    }
}

bool HostRouter::SampleGamePort(int port, PadState& out) noexcept {
    out = {};
    if (port < 0 || port >= kPortCount) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    UpdateConnectionsLocked();
    if (!lastConnected_[port]) {
        return false;
    }
    RefreshFocus();
    const bool physicalCounts = lastFocused_ || options_.backgroundInput;
    const std::uint64_t tick = tick_[port]++;
    PortStats& st = stats_[port];
    ++st.samples;

    PadState physical;
    PadState other;
    bool retarget = false;
    try {
        for (InputSource* s : SourcesLocked()) {
            if (PortOf(s) != port) {
                continue;
            }
            PadState state;
            if (!s->Sample(port, tick, state)) {
                continue;
            }
            if (s->Kind() == SourceKind::Physical) {
                if (!physicalCounts) {
                    continue;
                }
                MergePad(physical, state);
                if (s->HasMotors() && Deliberate(state) && motorTarget_[port] != s) {
                    motorTarget_[port] = s;
                    retarget = true;
                }
            } else {
                MergePad(other, state);
            }
        }
    } catch (...) {
    }
    bool suppressed = ApplySuppress(suppressPhysical_[port], physical);
    PadState merged = physical;
    MergePad(merged, other);
    if (retarget) {
        ApplyRumbleLocked(port);
    }
    if (sampleFilter_ != nullptr) {
        sampleFilter_(port, merged);
    }
    if (InputConsumer* c = consumer_[port]) {
        ++st.divertedSamples;
        c->Consume(port, tick, merged);
        return true;  // the game sees a connected, neutral pad
    }
    suppressed = ApplySuppress(suppressAll_[port], merged) || suppressed;
    if (suppressed) {
        ++st.suppressedSamples;
    }
    if (!merged.Neutral()) {
        ++st.activeSamples;
    }
    out = merged;
    return true;
}

void HostRouter::SetSampleFilter(SampleFilter filter) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sampleFilter_ = filter;
}

void HostRouter::Divert(int port, InputConsumer* consumer) noexcept {
    if (port < 0 || port >= kPortCount) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    InputConsumer* previous = consumer_[port];
    if (previous == consumer) {
        return;
    }
    consumer_[port] = consumer;
    FUT_LOGI(Inp, "input: port %d %s%s", port + 1, consumer != nullptr ? "diverted to " : "back to the game",
             consumer != nullptr ? consumer->Name() : "");
    if (consumer == nullptr) {
        suppressAll_[port].pending = true;  // controls held at the return count only after a release
    }
    ApplyRumbleLocked(port);
}

void HostRouter::SetGameRumble(int port, const RumbleState& rumble) noexcept {
    if (port < 0 || port >= kPortCount) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    PortStats& st = stats_[port];
    ++st.rumbleRequests;
    if (NonZero(rumble)) {
        ++st.rumbleNonZero;
        st.maxRumble.left = std::max(st.maxRumble.left, rumble.left);
        st.maxRumble.right = std::max(st.maxRumble.right, rumble.right);
    }
    const RumbleState previous = requested_[port];
    if (!(previous == rumble)) {
        if (NonZero(rumble) && !loggedFirstRumble_) {
            loggedFirstRumble_ = true;
            FUT_LOGI(Inp, "input: first rumble request of the game on port %d: left %u right %u (rumble %s, strength %u %%)",
                     port + 1, rumble.left, rumble.right, options_.rumble ? "on" : "off", options_.rumbleStrength);
        } else if (NonZero(rumble) != NonZero(previous)) {
            FUT_LOGD(Inp, "input: rumble port %d %s (left %u right %u)", port + 1, NonZero(rumble) ? "on" : "off",
                     rumble.left, rumble.right);
        }
    }
    requested_[port] = rumble;
    ApplyRumbleLocked(port);
}

void HostRouter::SendLocked(InputSource* source, int port, const RumbleState& value) noexcept {
    for (SentRumble& e : sent_) {
        if (e.source == source && e.port == port) {
            if (e.value == value) {
                return;
            }
            e.value = value;
            source->SetRumble(port, value);
            return;
        }
    }
    try {
        sent_.push_back({source, port, value});
    } catch (...) {
    }
    source->SetRumble(port, value);
}

void HostRouter::ApplyRumbleLocked(int port) noexcept {
    const bool muted = !options_.rumble || !FocusedLocked() || consumer_[port] != nullptr || forcedOff_[port];
    const RumbleState scaled = muted ? RumbleState{} : Scale(requested_[port], options_.rumbleStrength);
    try {
        for (InputSource* s : SourcesLocked()) {
            if (PortOf(s) != port) {
                continue;
            }
            if (s->HasMotors()) {
                const bool target = motorTarget_[port] == nullptr || motorTarget_[port] == s;
                SendLocked(s, port, target ? scaled : RumbleState{});
            } else {
                SendLocked(s, port, requested_[port]);
            }
        }
    } catch (...) {
    }
}

void HostRouter::StopAllMotors() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
        for (InputSource* s : SourcesLocked()) {
            if (s->HasMotors()) {
                SendLocked(s, PortOf(s), RumbleState{});
            }
        }
    } catch (...) {
    }
}

HostRouter::PortStats HostRouter::Stats(int port) const noexcept {
    if (port < 0 || port >= kPortCount) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return stats_[port];
}

std::uint32_t HostRouter::FocusChanges() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return focusChanges_;
}

}  // namespace inp
