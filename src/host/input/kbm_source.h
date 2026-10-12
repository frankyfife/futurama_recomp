// Keyboard and mouse as one virtual Xbox controller on port 1 (analysis\loader\m4_input.md 8.1, 8.3, 8.4, 9.2).
//
// Events arrive from the game window's UI thread (Raw Input, input/kbm_window.h) or from tests; Sample() turns them
// into one PadState per logic tick through the kbm_* bindings (input/bindings.h):
// - keys / mouse buttons held at the sample, or pressed and released since the previous sample (a tap shorter than a
//   tick is reported for exactly one tick: m4_input.md 8.3, several ticks may run per frame);
// - mouse wheel: every notch presses for 2 ticks, then releases for at least 1 tick (queued, at most 3 per direction);
// - mouse movement is accumulated until the sample: counts * mouse_sensitivity / kMouseCountsPerFullDeflection is the
//   stick deflection the game should see (input/stick_curve.h inverse curve, bindings.cpp);
// - menu escape: while the game is in the Frontend context, a key bound to START presses BACK instead (decided when
//   the key goes down; m4_input.md 9.2 "Esc kontextabhängig"); Ingame it stays START (pause).
// ReleaseAll() (focus loss) drops every held key, button, queued notch and movement.
#pragma once

#include "input/bindings.h"
#include "input/input_api.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace inp {

// Raw wheel units of one notch (WHEEL_DELTA).
inline constexpr int kWheelDelta = 120;

class KeyboardMouseSource final : public InputSource {
public:
    using MenuQuery = bool (*)() noexcept;  // true while START should act as BACK (frontend menus)

    // `bindings` must outlive the source.
    void Configure(const BindingTable* bindings, float mouseSensitivity, bool menuEscape) noexcept;
    void SetMenuQuery(MenuQuery query) noexcept;
    // false: the source reports "not connected" and ignores events.
    void SetEnabled(bool enabled) noexcept;
    // M4b: false = Sample feeds no pad state (the freecam reads keys and mouse through ReadRaw instead); events are
    // still tracked. Motion and taps collected meanwhile are dropped, so returning to the game does not jump.
    void SetPadEmulation(bool on) noexcept;
    // G6: while `query` returns true, mouse motion is not mapped to the bound controls (it is dropped from the pad
    // emulation; ReadRaw still reports it) - the first-person view (xapi/first_person.h) reads the raw counts.
    // nullptr = always mapped. The query runs outside the source's lock.
    using MouseMotionQuery = bool (*)() noexcept;
    void SetMouseMotionQuery(MouseMotionQuery query) noexcept;

    // ---- events (any thread) -----------------------------------------------------------------------------
    void KeyEvent(std::uint16_t vk, bool down) noexcept;  // vk 1..255, left/right modifiers (VK_LSHIFT ...)
    void MouseButtonEvent(int button, bool down) noexcept;  // 0 left, 1 right, 2 middle, 3 x1, 4 x2
    void WheelEvent(int notches) noexcept;                  // > 0 = away from the user (wheel_up)
    void WheelDeltaEvent(int delta) noexcept;               // raw wheel units (kWheelDelta = one notch), summed
    void MouseMoveEvent(int dx, int dy) noexcept;           // relative counts, +y = towards the user
    void ReleaseAll() noexcept;
    bool KeyDown(std::uint16_t vk) const noexcept;

    // ---- InputSource -------------------------------------------------------------------------------------
    const char* Name() const noexcept override { return "keyboard/mouse"; }
    SourceKind Kind() const noexcept override { return SourceKind::Physical; }
    int DefaultPort() const noexcept override { return 0; }
    bool Connected() const noexcept override;
    bool Sample(int port, std::uint64_t tick, PadState& out) noexcept override;

    // Raw view for later consumers (M4b freecam: WASD + mouse delta without the pad emulation). Movement is
    // accumulated separately from Sample; `consumeMotion` resets it.
    struct RawSnapshot {
        std::array<bool, 256> keys{};
        std::uint8_t mouseButtons = 0;
        std::int64_t dx = 0, dy = 0;
        std::int32_t wheel = 0;
    };
    RawSnapshot ReadRaw(bool consumeMotion) noexcept;

    struct Stats {
        std::uint64_t keyEvents = 0;
        std::uint64_t mouseEvents = 0;
        std::uint64_t samples = 0;
        std::uint64_t activeSamples = 0;  // samples with a non-neutral pad
        std::uint32_t releases = 0;       // ReleaseAll calls
        std::uint32_t menuEscapes = 0;    // START presses turned into BACK
    };
    Stats GetStats() const noexcept;

private:
    void WheelNotchesLocked(int notches) noexcept;

    struct WheelLatch {
        int pending = 0;
        int pressLeft = 0;
        bool needRelease = false;
        bool Step() noexcept;  // pressed this tick?
    };

    mutable std::mutex mutex_;
    const BindingTable* bindings_ = nullptr;
    float sensitivity_ = 1.0f;
    bool menuEscape_ = true;
    MenuQuery menuQuery_ = nullptr;
    bool enabled_ = true;
    bool padEmulation_ = true;
    std::atomic<MouseMotionQuery> mouseMotionQuery_{nullptr};  // G6

    std::array<bool, 256> down_{};
    std::array<bool, 256> tapped_{};
    std::uint8_t buttonsDown_ = 0;
    std::uint8_t buttonsTapped_ = 0;
    WheelLatch wheel_[2];
    std::int64_t dx_ = 0, dy_ = 0;          // since the last Sample
    std::int64_t rawDx_ = 0, rawDy_ = 0;    // since the last ReadRaw(consume)
    std::int32_t rawWheel_ = 0;
    int wheelRemainder_ = 0;  // WheelDeltaEvent: part of a notch not yet queued (sign = direction)
    bool startWasDown_ = false;
    bool startAsBack_ = false;
    Stats stats_{};
};

// Raw Input keyboard record -> virtual-key code of the binding names (0 = ignore): VK_SHIFT/CONTROL/MENU become
// the left/right codes (make code 0x36 = right shift, E0 prefix = right ctrl/alt), 0xFF (fake keys of escape
// sequences) is dropped. `flags` = RAWKEYBOARD::Flags (RI_KEY_E0 = 2).
std::uint16_t NormalizeRawKey(std::uint16_t vkey, std::uint16_t makeCode, std::uint16_t flags) noexcept;

}  // namespace inp
