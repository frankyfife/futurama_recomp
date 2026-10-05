// XPP / XInput stubs (M2-M3): no controller is connected (see xpp_stubs.h). Callers are the game's input
// code (avi.md 4.2): init FUN_0009C2A0, hotplug FUN_0009C140 (per frame from 0xD25D3/0xF3004), rumble
// FUN_0009C220, controller update 0x9C3D0 (vtable 0x2A14F0[0]). "ctrl" = controller object (0x19C bytes,
// ctor 0x9BF50; +0x14C port, +0x150 XInput handle, +0x154 XINPUT_FEEDBACK).
#include "input/xpp_stubs.h"

#include "audio/xdk_entries.h"
#include "core/guest.h"
#include "core/log.h"

#include <atomic>
#include <cstring>

namespace inp {
namespace {

std::uint32_t A(const volatile void* p) noexcept {
    return guest::Addr(p);
}

const char* DeviceTypeName(const void* type) noexcept {
    switch (A(type)) {
    case guest::kXppDeviceTypeGamepad:
        return "Gamepad";
    case guest::kXppDeviceTypeMemoryUnit:
        return "MemoryUnit";
    default:
        return "?";
    }
}

}  // namespace

// XPP::XInitDevices 0x28CFE1 (5-byte jmp -> XAPILIB::XInitDevices 0x28C3B5) - stdcall (DWORD
// dwPreallocTypeCount, PXDEVICE_PREALLOC_TYPE), void, ret 8. Only caller: init 0x9C2D3 with
// (1, {&XDEVICE_TYPE_GAMEPAD, 4}); eax is not read (next: xor ebx,ebx). The original starts the USB/MU
// driver stack (MU_Init 0x28C09C only from here, avi.md 4.1) - skipped. No-op.
void __stdcall XInitDevicesHle(std::uint32_t preallocTypeCount, const void* preallocTypes) noexcept {
    static std::atomic<bool> logged{false};
    if (!xdk::detail::InPurgeProbe() && !logged.exchange(true)) {
        FUT_LOGI(Inp, "XPP: stub input - no controllers connected until M4");
    }
    XDK_TRACE_CALL(XInitDevices, "(%u, types=0x%08X) -> no-op", preallocTypeCount, A(preallocTypes));
}
XDK_HOOK(XInitDevices, &XInitDevicesHle);

// XPP::XGetDevices 0x28CFE6 - stdcall (PXPP_DEVICE_TYPE), DWORD port bitmask, ret 4. Init 0x9C328
// (Gamepad -> ebp = mask; XInputOpen for every set bit, 0x9C340..0x9C3A1) and 0x9C334 (MemoryUnit,
// result discarded). 0 = nothing connected: no controller is opened.
std::uint32_t __stdcall XGetDevicesHle(const void* deviceType) noexcept {
    XDK_TRACE_CALL(XGetDevices, "(%s 0x%08X) -> 0", DeviceTypeName(deviceType), A(deviceType));
    return 0;
}
XDK_HOOK(XGetDevices, &XGetDevicesHle);

// XPP::XGetDeviceChanges 0x28D008 - stdcall (PXPP_DEVICE_TYPE, PDWORD pdwInsertions, PDWORD pdwRemovals),
// BOOL, ret 12. Only caller: hotplug 0x9C15D per frame with zeroed locals; `test eax,eax; je 0x9C210`
// skips everything for FALSE. FALSE, both masks 0.
std::int32_t __stdcall XGetDeviceChangesHle(const void* deviceType, std::uint32_t* insertions,
                                            std::uint32_t* removals) noexcept {
    if (insertions != nullptr) {
        *insertions = 0;
    }
    if (removals != nullptr) {
        *removals = 0;
    }
    XDK_TRACE_CALL(XGetDeviceChanges, "(%s 0x%08X, 0x%08X, 0x%08X) -> FALSE", DeviceTypeName(deviceType),
                   A(deviceType), A(insertions), A(removals));
    return FALSE;
}
XDK_HOOK(XGetDeviceChanges, &XGetDeviceChangesHle);

// XPP::XInputOpen 0x28CE5C - stdcall (PXPP_DEVICE_TYPE, DWORD dwPort, DWORD dwSlot,
// PXINPUT_POLLING_PARAMETERS), HANDLE, ret 16. Callers 0x9C1E9 (insertion) and 0x9C383 (init), both
// store eax at ctrl+0x150. Not reached while XGetDevices/XGetDeviceChanges report nothing; if called,
// NULL = failure (XDK), and every later use of ctrl+0x150 tests it (update 0x9C3DC, close 0x9C192/
// 0x9C1C4/0x9C360). ASSUMPTION: GetLastError is not set (the game never reads it after XInputOpen).
void* __stdcall XInputOpenHle(const void* deviceType, std::uint32_t port, std::uint32_t slot,
                              const void* pollingParameters) noexcept {
    XDK_TRACE_CALL(XInputOpen, "(%s 0x%08X, port=%u, slot=%u, poll=0x%08X) -> NULL",
                   DeviceTypeName(deviceType), A(deviceType), port, slot, A(pollingParameters));
    return nullptr;
}
XDK_HOOK(XInputOpen, &XInputOpenHle);

// XPP::XInputClose 0x28CEB2 - stdcall (HANDLE), void, ret 4. Callers 0x9C197/0x9C1C9/0x9C363, only for a
// non-zero ctrl+0x150. No-op.
void __stdcall XInputCloseHle(void* device) noexcept {
    XDK_TRACE_CALL(XInputClose, "(0x%08X) -> no-op", A(device));
}
XDK_HOOK(XInputClose, &XInputCloseHle);

// XPP::XInputGetState 0x28CEBE - stdcall (HANDLE, PXINPUT_STATE), DWORD, ret 8. Only caller: controller
// update 0x9C3E6, only for ctrl+0x150 != 0 (0x9C3DC); `test eax,eax; je 0x9C3FD` - any non-zero result
// resets the controller (FUN_0009BB50) without reading the state. The original returns 0, 0x48F or 0x57
// (avi.md 4.1). ERROR_DEVICE_NOT_CONNECTED, state zeroed (22 bytes, the XINPUT_STATE size).
std::uint32_t __stdcall XInputGetStateHle(void* device, XInputState* state) noexcept {
    if (state != nullptr) {
        std::memset(state, 0, sizeof(*state));
    }
    XDK_TRACE_CALL(XInputGetState, "(0x%08X, state=0x%08X) -> 0x48F (not connected)", A(device), A(state));
    return kErrorDeviceNotConnected;
}
XDK_HOOK(XInputGetState, &XInputGetStateHle);

// XPP::XInputSetState 0x28CF2A - stdcall (HANDLE, PXINPUT_FEEDBACK), DWORD, ret 8. Only caller: rumble
// 0x9C28C (from update 0x9C996, i.e. only with an open handle), result ignored (next: mov byte
// [esi+0x148],0). The game sends only while feedback.dwStatus != ERROR_IO_PENDING (0x9C229) and never
// resets it itself (ctor 0x9C112), so the status is completed synchronously (avi.md 4.3: never leave
// 0x3E5). ERROR_DEVICE_NOT_CONNECTED as result and status; hEvent (0 in the game) is not signalled.
std::uint32_t __stdcall XInputSetStateHle(void* device, XInputFeedback* feedback) noexcept {
    if (feedback != nullptr) {
        feedback->dwStatus = kErrorDeviceNotConnected;
    }
    XDK_TRACE_CALL(
        XInputSetState, "(0x%08X, feedback=0x%08X motors=%u/%u event=0x%08X) -> 0x48F, dwStatus=0x48F",
        A(device), A(feedback), feedback != nullptr ? feedback->wLeftMotorSpeed : 0u,
        feedback != nullptr ? feedback->wRightMotorSpeed : 0u, feedback != nullptr ? feedback->hEvent : 0u);
    return kErrorDeviceNotConnected;
}
XDK_HOOK(XInputSetState, &XInputSetStateHle);

}  // namespace inp
