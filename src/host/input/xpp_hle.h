// XPP / XInput HLE (M4; design E7, analysis\loader\m4_input.md 2 "Vertrag für die HLE", avi.md 4). Replaces the
// M2-M3 stubs (input/xpp_stubs.*): the 7 XPP entry points now report the logical ports of the input router
// (input_api.h InputRouter, implementation input/host_router.h) to the game.
//
//   XInitDevices       starts the input layer (inp::StartInput: [input] settings, XInput, keyboard/mouse), no-op otherwise
//   XGetDevices        Gamepad: bit i = port i connected (InputRouter::ConnectedPorts); MemoryUnit: 0
//   XGetDeviceChanges  Gamepad: insertions/removals since the last call (or XGetDevices); a port that was re-connected
//                      in between (PortEpoch changed) reports BOTH bits; TRUE iff a mask is non-zero; MU: FALSE
//   XInputOpen         a non-NULL guest handle per port (never NULL for port 0..3 of the gamepad type: NULL would leave
//                      the port "connected" without input, m4_input.md 2)
//   XInputClose        closes the handle, the port's motors go to 0
//   XInputGetState     samples the port once (InputRouter::SampleGamePort, once per tick) -> XINPUT_STATE with a
//                      packet number that changes with the state; a port without device: 0x48F + zeroed state; a
//                      diverted port: 0 + neutral state; invalid/closed handle: 0x57
//   XInputSetState     motor speeds -> InputRouter::SetGameRumble; Header.dwStatus is set SYNCHRONOUSLY to the result
//                      (0, 0x48F or 0x57 - never left at ERROR_IO_PENDING 0x3E5, which would stop the game's rumble
//                      for good: it sends only while dwStatus != 0x3E5, 0x9C229). hEvent is never used (the game
//                      never writes it, m4_input.md 2: the feedback block comes from the game pool, not zeroed).
#pragma once

#include "input/input_api.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace inp {

#pragma pack(push, 1)
// XINPUT_GAMEPAD / XINPUT_STATE (XDK xbox.h; offsets used by the controller update 0x9C3D0: wButtons
// at +4, bAnalogButtons[8] at +6, thumbs at +0x0E..+0x15; avi.md 4.2 channel table).
struct XInputGamepad {
    std::uint16_t wButtons;
    std::uint8_t bAnalogButtons[8];
    std::int16_t sThumbLX;
    std::int16_t sThumbLY;
    std::int16_t sThumbRX;
    std::int16_t sThumbRY;
};
struct XInputState {
    std::uint32_t dwPacketNumber;
    XInputGamepad Gamepad;
};
// XINPUT_FEEDBACK (controller object +0x154: header 0x42 bytes, motor speeds at +0x42/+0x44 = object
// +0x196/+0x198, avi.md 4.2).
struct XInputFeedback {
    std::uint32_t dwStatus;  // the game sends only while != ERROR_IO_PENDING (0x9C229)
    std::uint32_t hEvent;    // guest HANDLE; never written by the game (m4_input.md 2) - ignored
    std::uint8_t reserved[58];
    std::uint16_t wLeftMotorSpeed;
    std::uint16_t wRightMotorSpeed;
};
#pragma pack(pop)
static_assert(sizeof(XInputGamepad) == 0x12);
static_assert(sizeof(XInputState) == 0x16);
static_assert(offsetof(XInputState, Gamepad) == 4 && offsetof(XInputGamepad, sThumbLX) == 0x0A);
static_assert(sizeof(XInputFeedback) == 0x46 && offsetof(XInputFeedback, wLeftMotorSpeed) == 0x42);

inline constexpr std::uint32_t kErrorSuccess = 0;
inline constexpr std::uint32_t kErrorInvalidParameter = 0x57;     // ERROR_INVALID_PARAMETER (bad handle)
inline constexpr std::uint32_t kErrorDeviceNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
inline constexpr std::uint32_t kErrorIoPending = 0x3E5;           // ERROR_IO_PENDING

// Copies a pad state into the guest XINPUT_GAMEPAD.
void ToXInputGamepad(const PadState& pad, XInputGamepad& out) noexcept;

// Hotplug bookkeeping of XGetDevices / XGetDeviceChanges (pure; tests).
class HotplugTracker {
public:
    // XGetDevices: the reported mask becomes the known state.
    void Reset(std::uint32_t mask, const std::uint32_t (&epochs)[kPortCount]) noexcept;
    // XGetDeviceChanges: differences to the known state; a port connected before and now whose epoch changed is
    // reported as removal + insertion. Returns true iff a mask is non-zero.
    bool Changes(std::uint32_t mask, const std::uint32_t (&epochs)[kPortCount], std::uint32_t& insertions,
                 std::uint32_t& removals) noexcept;
    std::uint32_t ReportedMask() const noexcept { return reported_; }

private:
    std::uint32_t reported_ = 0;
    std::uint32_t epochs_[kPortCount] = {};
};

// The logic of the 7 entries on top of an InputRouter (the hooks use one global instance with inp::GetRouter();
// tests create their own). Handles are guest-visible dummy objects (xdk::AllocGuestObject, below 0x80000000).
class XppDevices {
public:
    using RouterGetter = InputRouter* (*)() noexcept;
    explicit XppDevices(RouterGetter getter = &GetRouter) noexcept : getter_(getter) {}

    std::uint32_t GetDevices() noexcept;
    bool GetDeviceChanges(std::uint32_t& insertions, std::uint32_t& removals) noexcept;
    void* Open(std::uint32_t port) noexcept;
    void Close(void* handle) noexcept;
    std::uint32_t GetState(void* handle, XInputState* state) noexcept;
    std::uint32_t SetState(void* handle, XInputFeedback* feedback) noexcept;

    int PortOfHandle(const void* handle) const noexcept;  // -1 if not a handle of this instance
    bool IsOpen(int port) const noexcept;

    struct Stats {
        std::uint64_t getStateCalls = 0;
        std::uint64_t getStateFailures = 0;  // non-zero results
        std::uint64_t setStateCalls = 0;
        std::uint32_t opens = 0;
        std::uint32_t closes = 0;
        std::uint32_t insertions = 0;  // ports reported as inserted
        std::uint32_t removals = 0;
    };
    Stats GetStats() const noexcept;

private:
    static void Epochs(InputRouter& router, std::uint32_t (&epochs)[kPortCount]) noexcept;
    void* HandleLocked(int port) noexcept;

    RouterGetter getter_;
    mutable std::mutex mutex_;
    HotplugTracker tracker_;
    void* handles_[kPortCount] = {};
    bool open_[kPortCount] = {};
    std::uint32_t packet_[kPortCount] = {};
    PadState last_[kPortCount] = {};
    Stats stats_{};
};

// The instance behind the hooks.
XppDevices& GlobalXpp() noexcept;

void __stdcall XInitDevicesHle(std::uint32_t preallocTypeCount, const void* preallocTypes) noexcept;
std::uint32_t __stdcall XGetDevicesHle(const void* deviceType) noexcept;
std::int32_t __stdcall XGetDeviceChangesHle(const void* deviceType, std::uint32_t* insertions,
                                            std::uint32_t* removals) noexcept;
void* __stdcall XInputOpenHle(const void* deviceType, std::uint32_t port, std::uint32_t slot,
                              const void* pollingParameters) noexcept;
void __stdcall XInputCloseHle(void* device) noexcept;
std::uint32_t __stdcall XInputGetStateHle(void* device, XInputState* state) noexcept;
std::uint32_t __stdcall XInputSetStateHle(void* device, XInputFeedback* feedback) noexcept;

}  // namespace inp
