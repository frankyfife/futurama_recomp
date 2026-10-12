// Shared M4 input interface (src\INTERFACES.md section 4.27; analysis\loader\m4_input.md 8.1).
//
// First writer: the M4 test-automation agent (input script). Other modules extend it additively (shared rule,
// changelog row in src\INTERFACES.md). Header-only on purpose: the source registry and the router slot are inline
// so that every module can register without a link-time dependency on the module that implements the router.
//
// Layers (m4_input.md 8.1):
//
//   InputSource (XInput pads, keyboard/mouse, input script, ...)   one PadState per logical port and logic tick
//        |  Sample(port, tick, state) - called by the router inside XInputGetState of that port (guest main thread)
//        v
//   InputRouter  decides which sources feed the game; can divert a port to an InputConsumer (M4b freecam). A
//        |       diverted port stays connected and reports a neutral pad to the game (never 0x48F, never a removal).
//        v
//   XPP HLE (input\xpp_*.cpp): 4 logical Xbox ports -> XINPUT_STATE; XInputSetState -> rumble back to the sources.
//
// Value ranges are the Xbox ones (XINPUT_GAMEPAD): digital buttons as wButtons bits, 8 analog buttons 0..255,
// sticks -32768..32767 with +Y = up. Nothing here depends on Windows XInput.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace inp {

inline constexpr int kPortCount = 4;  // XGetDevices bits 0..3 (controller objects 0x2E6F84..0x2E6F93)

// Digital buttons: XINPUT_GAMEPAD.wButtons (the game reads only the low byte, m4_input.md 2).
inline constexpr std::uint16_t kButtonDpadUp = 0x0001;
inline constexpr std::uint16_t kButtonDpadDown = 0x0002;
inline constexpr std::uint16_t kButtonDpadLeft = 0x0004;
inline constexpr std::uint16_t kButtonDpadRight = 0x0008;
inline constexpr std::uint16_t kButtonStart = 0x0010;
inline constexpr std::uint16_t kButtonBack = 0x0020;
inline constexpr std::uint16_t kButtonLeftThumb = 0x0040;
inline constexpr std::uint16_t kButtonRightThumb = 0x0080;
inline constexpr std::uint16_t kButtonMask = 0x00FF;

// Analog buttons: index into XINPUT_GAMEPAD.bAnalogButtons[8] (m4_input.md 2 / 3.2).
enum AnalogButton : std::uint8_t {
    kAnalogA = 0,
    kAnalogB = 1,
    kAnalogX = 2,
    kAnalogY = 3,
    kAnalogBlack = 4,
    kAnalogWhite = 5,
    kAnalogLeftTrigger = 6,
    kAnalogRightTrigger = 7,
    kAnalogCount = 8,
};

// Stick axes: index into PadState::thumb (sThumbLX, sThumbLY, sThumbRX, sThumbRY).
enum StickAxis : std::uint8_t {
    kAxisLX = 0,
    kAxisLY = 1,
    kAxisRX = 2,
    kAxisRY = 3,
    kAxisCount = 4,
};

// The game's channel threshold: an analog button counts as pressed from 30/256 on (m4_input.md 3.2).
inline constexpr std::uint8_t kAnalogPressThreshold = 30;

// Virtual pad state of one logical port for one logic tick.
struct PadState {
    std::uint16_t buttons = 0;                // kButton* bits
    std::uint8_t analog[kAnalogCount] = {};   // AnalogButton order, 0..255
    std::int16_t thumb[kAxisCount] = {};      // StickAxis order, -32768..32767, +Y = up

    bool operator==(const PadState&) const = default;

    bool Neutral() const noexcept {
        return *this == PadState{};
    }
};

// Merges `from` into `into`: buttons OR'ed, analog buttons maximum, per axis the larger deflection wins
// (m4_input.md 8.1, port model "one player").
inline void MergePad(PadState& into, const PadState& from) noexcept {
    into.buttons = static_cast<std::uint16_t>(into.buttons | from.buttons);
    for (int i = 0; i < kAnalogCount; ++i) {
        into.analog[i] = std::max(into.analog[i], from.analog[i]);
    }
    for (int i = 0; i < kAxisCount; ++i) {
        if (std::abs(static_cast<int>(from.thumb[i])) > std::abs(static_cast<int>(into.thumb[i]))) {
            into.thumb[i] = from.thumb[i];
        }
    }
}

// Motor speeds the game sent with XInputSetState (XINPUT_FEEDBACK Rumble, 0..65535; left = low frequency).
struct RumbleState {
    std::uint16_t left = 0;
    std::uint16_t right = 0;
    bool operator==(const RumbleState&) const = default;
};

enum class SourceKind : std::uint8_t {
    Physical,   // XInput controller, keyboard/mouse
    Script,     // [debug] input_script (input\input_script.*)
    Synthetic,  // tests, injected devices
};

// A producer of pad states (physical backend, script player, test injector).
class InputSource {
public:
    virtual ~InputSource() = default;

    virtual const char* Name() const noexcept = 0;
    virtual SourceKind Kind() const noexcept = 0;
    // Logical port this source feeds by default (0 = player 1, shown as "port 1" by the game). The router may
    // place it elsewhere (pad_mode per_port).
    virtual int DefaultPort() const noexcept {
        return 0;
    }
    // True while the source provides a device (pad plugged in, keyboard present, script loaded). A port is
    // connected while at least one source routed to it is connected. Any thread; must be cheap.
    virtual bool Connected() const noexcept = 0;
    // Called by the router exactly once per logic tick for every port this source feeds, inside the game's
    // XInputGetState of that port (guest main thread; m4_input.md 8.3: sample per tick, not per frame).
    // `tick` counts the router's samples of that port (0, 1, 2, ...). Writes the state of this tick to `out`
    // (which the caller zeroed); false = no contribution this tick (treated as neutral).
    virtual bool Sample(int port, std::uint64_t tick, PadState& out) noexcept = 0;
    // Motor speeds the game requested for a port this source feeds (XInputSetState). Default: ignored.
    virtual void SetRumble(int port, const RumbleState& rumble) noexcept {
        (void)port;
        (void)rumble;
    }
    // (Added by the input agent, additive.) True for a source with vibration motors (an XInput controller). The
    // router sends the game's rumble of a port, scaled by [input] rumble / rumble_strength and muted while the
    // window has no focus or the port is diverted, only to the most recently used such source of the port (0 to
    // the others: the game rumbles only its active pad, m4_input.md 6). Sources without motors (keyboard/mouse,
    // script) get the game's unscaled values. Default: no motors.
    virtual bool HasMotors() const noexcept {
        return false;
    }
};

// A taker of pad states the router diverted away from the game (M4b freecam, debug tools).
class InputConsumer {
public:
    virtual ~InputConsumer() = default;
    virtual const char* Name() const noexcept = 0;
    // The merged state of the diverted port for this tick (guest main thread, inside XInputGetState).
    virtual void Consume(int port, std::uint64_t tick, const PadState& state) noexcept = 0;
};

// The router (implemented by the input agent; registered with SetRouter before the guest runs).
class InputRouter {
public:
    virtual ~InputRouter() = default;
    // Ports that are connected (bit i = port i), what XGetDevices / XGetDeviceChanges report.
    virtual std::uint32_t ConnectedPorts() const noexcept = 0;
    // The game's view of `port` for this tick: samples every source routed to the port once and merges them.
    // False = the port has no device (XInputGetState then fails). A diverted port returns true + neutral.
    virtual bool SampleGamePort(int port, PadState& out) noexcept = 0;
    // XInputSetState of the game for `port`: forwarded to the port's sources (scaled by [input] rumble_*).
    virtual void SetGameRumble(int port, const RumbleState& rumble) noexcept = 0;
    // Diverts `port` to `consumer` (nullptr = back to the game). While diverted the game sees a connected,
    // neutral pad; on return, controls still held are passed on only after they were released once
    // (m4_input.md 8.1: no false press edge).
    virtual void Divert(int port, InputConsumer* consumer) noexcept = 0;

    // ---- added by the input agent (additive; defaults keep other implementations valid) ----
    // Connection generation of `port`: +1 every time the port becomes connected. The XPP HLE reports a removal AND
    // an insertion when it changed between two XGetDeviceChanges calls although the port is connected at both
    // (unplug + replug in between, m4_input.md 2 HLE contract item 3). Default 0 = not tracked.
    virtual std::uint32_t PortEpoch(int port) const noexcept {
        (void)port;
        return 0;
    }
    // Test / diagnosis (scripted runs, m4_input.md 8.5): treats `port` as unplugged while `unplugged` is true.
    // XGetDeviceChanges then reports the removal; in-game the game pauses with ERRCTRLINGAME ("connect the
    // controller to port N"). Unplug + replug between two hotplug checks counts as a re-connection (PortEpoch).
    virtual void SimulateUnplug(int port, bool unplugged) noexcept {
        (void)port;
        (void)unplugged;
    }
};

// ---- registry (header-only) ---------------------------------------------------------------------------

inline constexpr int kMaxSources = 16;

namespace detail {
struct SourceRegistry {
    std::atomic<InputSource*> sources[kMaxSources] = {};
    std::atomic<int> count{0};
};
inline SourceRegistry& Registry() noexcept {
    static SourceRegistry registry;
    return registry;
}
inline std::atomic<InputRouter*>& RouterSlot() noexcept {
    static std::atomic<InputRouter*> router{nullptr};
    return router;
}
}  // namespace detail

// Registers a source (static lifetime; normally a namespace-scope object registering itself in its constructor
// or a HostMain stage before the guest starts). Idempotent; false if the table is full.
inline bool RegisterSource(InputSource* source) noexcept {
    detail::SourceRegistry& r = detail::Registry();
    const int n = r.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (r.sources[i].load(std::memory_order_acquire) == source) {
            return true;
        }
    }
    const int slot = r.count.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kMaxSources) {
        r.count.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    r.sources[slot].store(source, std::memory_order_release);
    return true;
}
inline int SourceCount() noexcept {
    return std::min(detail::Registry().count.load(std::memory_order_acquire), kMaxSources);
}
// nullptr for an index out of range (or a slot being filled concurrently).
inline InputSource* SourceAt(int index) noexcept {
    if (index < 0 || index >= SourceCount()) {
        return nullptr;
    }
    return detail::Registry().sources[index].load(std::memory_order_acquire);
}

inline void SetRouter(InputRouter* router) noexcept {
    detail::RouterSlot().store(router, std::memory_order_release);
}
// nullptr until the input layer installed its router.
inline InputRouter* GetRouter() noexcept {
    return detail::RouterSlot().load(std::memory_order_acquire);
}

}  // namespace inp
