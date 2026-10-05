// XPP / XInput stubs for M2-M3 (design E7, 2.1 "XPP ... M2-M3: keine Geraete, XGetDevices = 0", 6 "M2";
// avi.md 4.1-4.3). No controller is ever connected: the game's controller objects stay unopened, so
// XInputGetState/XInputSetState are not reached (controller update 0x9C3D0 returns for handle 0).
//
// Returned values (evidence in input/xpp_stubs.cpp):
//   XInitDevices       no-op
//   XGetDevices        0 (no device on any port)
//   XGetDeviceChanges  FALSE, *pdwInsertions = *pdwRemovals = 0
//   XInputOpen         NULL
//   XInputClose        no-op
//   XInputGetState     ERROR_DEVICE_NOT_CONNECTED (0x48F), state zeroed
//   XInputSetState     ERROR_DEVICE_NOT_CONNECTED, Header.dwStatus set to it synchronously (never 0x3E5)
#pragma once

#include <cstddef>
#include <cstdint>

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
    std::uint32_t hEvent;    // guest HANDLE (0 in the game: ctor 0x9C112 zeroes the header)
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
inline constexpr std::uint32_t kErrorDeviceNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
inline constexpr std::uint32_t kErrorIoPending = 0x3E5;           // ERROR_IO_PENDING

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
