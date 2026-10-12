// XInput 1.4 backend: one InputSource per XInput user index 0..3 (analysis\loader\m4_input.md 8.2).
//
// - XInput is loaded at run time (xinput1_4.dll, Windows 8+; fallback xinput9_1_0.dll with the same GetState/
//   SetState). Without it every slot reads "not connected".
// - Sampling: once per logic tick inside the game's XInputGetState of the source's port (InputRouter). A slot that
//   is not connected is probed at most every kProbeIntervalMs (XInputGetState on empty slots is reported to be slow -
//   general knowledge, not measured here); a connected slot that the router did not sample for kStaleIntervalMs is
//   probed again by Connected().
// - Mapping: the pad_* bindings (input/bindings.h; default identity with LB = WHITE, RB = BLACK, analog triggers ->
//   the analog LT/RT buttons 0..255), radial dead zone [input] pad_deadzone.
// - Rumble: the router's motor speeds go to XInputSetState (left = low-frequency motor as on the Xbox). Sent only on a
//   change; re-sent after a reconnection; StopMotors() at exit/crash.
#pragma once

#include "input/bindings.h"
#include "input/input_api.h"

#include <cstdint>
#include <mutex>

namespace inp {

inline constexpr std::uint32_t kXInputUsers = 4;
inline constexpr std::uint32_t kXInputErrorSuccess = 0;
inline constexpr std::uint32_t kXInputErrorNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED

// Access to XInput (the system DLL or a test double).
class XInputApi {
public:
    virtual ~XInputApi() = default;
    virtual bool Available() const noexcept = 0;
    virtual const char* DllName() const noexcept = 0;
    // XInputGetState: ERROR_SUCCESS (0) with packet and pad, or an error (0x48F not connected).
    virtual std::uint32_t GetState(std::uint32_t user, std::uint32_t& packet, PhysicalPad& pad) noexcept = 0;
    // XInputSetState with XINPUT_VIBRATION {left, right}.
    virtual std::uint32_t SetState(std::uint32_t user, std::uint16_t left, std::uint16_t right) noexcept = 0;
};
// The process-wide XInput (loads the DLL on first use; thread-safe).
XInputApi& SystemXInput() noexcept;

// G5: optional process-wide shaping of every controller's mapped state (after bindings and pad_deadzone), e.g. the
// right-stick dead zone of [input] camera_response = precise (input_system.cpp). nullptr = none (default).
using PadShaper = void (*)(int port, PadState& state) noexcept;
void SetPadShaper(PadShaper shaper) noexcept;

// Milliseconds of a monotonic clock (tests inject their own).
using MillisecondClock = std::uint64_t (*)() noexcept;

class XInputPadSource final : public InputSource {
public:
    static constexpr std::uint64_t kProbeIntervalMs = 1000;  // disconnected slot
    static constexpr std::uint64_t kStaleIntervalMs = 250;   // connected slot not sampled for this long

    XInputPadSource(XInputApi& api, std::uint32_t user, MillisecondClock clock = nullptr) noexcept;

    // Bindings must outlive the source (the input system's settings); port = the logical port it feeds.
    void Configure(const BindingTable* bindings, std::uint32_t deadzonePercent, int port) noexcept;

    const char* Name() const noexcept override { return name_; }
    SourceKind Kind() const noexcept override { return SourceKind::Physical; }
    int DefaultPort() const noexcept override { return port_; }
    bool Connected() const noexcept override;
    bool Sample(int port, std::uint64_t tick, PadState& out) noexcept override;
    void SetRumble(int port, const RumbleState& rumble) noexcept override;
    bool HasMotors() const noexcept override { return true; }

    std::uint32_t User() const noexcept { return user_; }
    // Motors off now (exit, crash, focus loss handled by the router).
    void StopMotors() noexcept;
    // The last physical state read (live view, tests).
    PhysicalPad LastPhysical() const noexcept;

    struct Stats {
        std::uint32_t connects = 0;
        std::uint32_t disconnects = 0;
        std::uint64_t samples = 0;
        std::uint64_t probes = 0;           // GetState calls outside Sample (Connected)
        std::uint64_t rumbleCommands = 0;   // XInputSetState calls
        RumbleState maxRumble;              // largest motor speeds sent
    };
    Stats GetStats() const noexcept;

private:
    // Reads the slot; updates the connection state. Caller holds mutex_.
    bool QueryLocked(PhysicalPad& pad, std::uint64_t now) const noexcept;
    void SendRumbleLocked() const noexcept;
    std::uint64_t Now() const noexcept;

    XInputApi& api_;
    const std::uint32_t user_;
    MillisecondClock clock_;
    char name_[32] = {};
    const BindingTable* bindings_ = nullptr;
    std::uint32_t deadzone_ = 0;
    int port_ = 0;

    mutable std::mutex mutex_;
    mutable bool connected_ = false;
    mutable bool everQueried_ = false;
    mutable std::uint64_t lastQueryMs_ = 0;
    mutable PhysicalPad last_{};
    mutable RumbleState requested_{};
    mutable RumbleState sent_{};
    mutable Stats stats_{};
};

}  // namespace inp
