// XPP / XInput HLE (M4, see xpp_hle.h). Callers are the game's input code (m4_input.md 2/3, avi.md 4.2): init
// FUN_0009C2A0, hotplug FUN_0009C140 (per logic TICK from CApp::vf10 0xD25D0, and from the disc error loop 0xF3004),
// rumble FUN_0009C220, controller update 0x9C3D0 (vtable 0x2A14F0[0], once per tick and open port). "ctrl" =
// controller object (0x19C bytes, ctor 0x9BF50; +0x138 disconnected flag, +0x14C port, +0x150 XInput handle, +0x154
// XINPUT_FEEDBACK).
#include "input/xpp_hle.h"

#include "audio/xdk_entries.h"
#include "core/guest.h"
#include "core/log.h"
#include "input/input_system.h"

#include <cstring>

namespace inp {
namespace {

std::uint32_t A(const volatile void* p) noexcept {
    return guest::Addr(p);
}

bool IsGamepad(const void* type) noexcept {
    return A(type) == guest::kXppDeviceTypeGamepad;
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

void ToXInputGamepad(const PadState& pad, XInputGamepad& out) noexcept {
    out.wButtons = static_cast<std::uint16_t>(pad.buttons & kButtonMask);
    std::memcpy(out.bAnalogButtons, pad.analog, sizeof(out.bAnalogButtons));
    out.sThumbLX = pad.thumb[kAxisLX];
    out.sThumbLY = pad.thumb[kAxisLY];
    out.sThumbRX = pad.thumb[kAxisRX];
    out.sThumbRY = pad.thumb[kAxisRY];
}

// ---- HotplugTracker --------------------------------------------------------------------------------------------

void HotplugTracker::Reset(std::uint32_t mask, const std::uint32_t (&epochs)[kPortCount]) noexcept {
    reported_ = mask & ((1u << kPortCount) - 1);
    for (int p = 0; p < kPortCount; ++p) {
        epochs_[p] = epochs[p];
    }
}

bool HotplugTracker::Changes(std::uint32_t mask, const std::uint32_t (&epochs)[kPortCount],
                             std::uint32_t& insertions, std::uint32_t& removals) noexcept {
    insertions = 0;
    removals = 0;
    mask &= (1u << kPortCount) - 1;
    for (int p = 0; p < kPortCount; ++p) {
        const std::uint32_t bit = 1u << p;
        const bool now = (mask & bit) != 0;
        const bool was = (reported_ & bit) != 0;
        const bool replugged = now && was && epochs[p] != epochs_[p];
        if ((was && !now) || replugged) {
            removals |= bit;
        }
        if ((now && !was) || replugged) {
            insertions |= bit;
        }
        epochs_[p] = epochs[p];
    }
    reported_ = mask;
    return insertions != 0 || removals != 0;
}

// ---- XppDevices ------------------------------------------------------------------------------------------------

void XppDevices::Epochs(InputRouter& router, std::uint32_t (&epochs)[kPortCount]) noexcept {
    for (int p = 0; p < kPortCount; ++p) {
        epochs[p] = router.PortEpoch(p);
    }
}

// The router (and through it the sources, e.g. the input script's loader) is queried before mutex_ is taken: a
// source may end the process, and the exit reporter LogInputSummary takes mutex_ (GetStats).
std::uint32_t XppDevices::GetDevices() noexcept {
    InputRouter* router = getter_();
    std::uint32_t epochs[kPortCount] = {};
    std::uint32_t mask = 0;
    if (router != nullptr) {
        mask = router->ConnectedPorts() & ((1u << kPortCount) - 1);
        Epochs(*router, epochs);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    tracker_.Reset(mask, epochs);
    return mask;
}

bool XppDevices::GetDeviceChanges(std::uint32_t& insertions, std::uint32_t& removals) noexcept {
    InputRouter* router = getter_();
    std::uint32_t epochs[kPortCount] = {};
    std::uint32_t mask = 0;
    if (router != nullptr) {
        mask = router->ConnectedPorts();
        Epochs(*router, epochs);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool changed = tracker_.Changes(mask, epochs, insertions, removals);
    for (int p = 0; p < kPortCount; ++p) {
        stats_.insertions += (insertions >> p) & 1u;
        stats_.removals += (removals >> p) & 1u;
    }
    if (changed) {
        FUT_LOGI(Inp, "input: XGetDeviceChanges reports insertions 0x%X, removals 0x%X (ports connected now 0x%X)",
                 insertions, removals, mask);
    }
    return changed;
}

void* XppDevices::HandleLocked(int port) noexcept {
    if (handles_[port] == nullptr) {
        handles_[port] = xdk::AllocGuestObject(16);
        if (handles_[port] == nullptr) {
            // ASSUMPTION: the game only stores the handle and compares it with 0 (0x9C3DC, 0x9C192, 0x9C1C4,
            // 0x9C360); a non-zero tag below the XBE image is enough when the dummy-object pool is exhausted.
            handles_[port] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x0000F000u + 0x10u * port));
            FUT_LOGW(Inp, "input: XDK dummy-object pool full - port %d uses a tag handle", port + 1);
        }
    }
    return handles_[port];
}

int XppDevices::PortOfHandle(const void* handle) const noexcept {
    if (handle == nullptr) {
        return -1;
    }
    for (int p = 0; p < kPortCount; ++p) {
        if (handles_[p] == handle) {
            return p;
        }
    }
    return -1;
}

bool XppDevices::IsOpen(int port) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return port >= 0 && port < kPortCount && open_[port];
}

void* XppDevices::Open(std::uint32_t port) noexcept {
    if (port >= static_cast<std::uint32_t>(kPortCount)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const int p = static_cast<int>(port);
    void* h = HandleLocked(p);
    open_[p] = true;
    last_[p] = {};
    ++stats_.opens;
    return h;
}

void XppDevices::Close(void* handle) noexcept {
    InputRouter* router = getter_();
    int port = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        port = PortOfHandle(handle);
        if (port < 0 || !open_[port]) {
            return;
        }
        open_[port] = false;
        ++stats_.closes;
    }
    if (router != nullptr) {
        router->SetGameRumble(port, RumbleState{});  // a closed device does not keep vibrating
    }
}

std::uint32_t XppDevices::GetState(void* handle, XInputState* state) noexcept {
    InputRouter* router = getter_();
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.getStateCalls;
    if (state == nullptr) {
        ++stats_.getStateFailures;
        return kErrorInvalidParameter;
    }
    std::memset(state, 0, sizeof(*state));
    const int port = PortOfHandle(handle);
    if (port < 0 || !open_[port]) {
        ++stats_.getStateFailures;
        return kErrorInvalidParameter;
    }
    PadState pad;
    if (router == nullptr || !router->SampleGamePort(port, pad)) {
        ++stats_.getStateFailures;
        return kErrorDeviceNotConnected;
    }
    if (!(pad == last_[port])) {
        last_[port] = pad;
        ++packet_[port];
    }
    state->dwPacketNumber = packet_[port];
    ToXInputGamepad(pad, state->Gamepad);
    return kErrorSuccess;
}

std::uint32_t XppDevices::SetState(void* handle, XInputFeedback* feedback) noexcept {
    if (feedback == nullptr) {
        return kErrorInvalidParameter;
    }
    InputRouter* router = getter_();
    int port = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.setStateCalls;
        port = PortOfHandle(handle);
        if (port < 0 || !open_[port]) {
            feedback->dwStatus = kErrorInvalidParameter;
            return kErrorInvalidParameter;
        }
    }
    if (router == nullptr || (router->ConnectedPorts() & (1u << port)) == 0) {
        feedback->dwStatus = kErrorDeviceNotConnected;
        return kErrorDeviceNotConnected;
    }
    router->SetGameRumble(port, RumbleState{feedback->wLeftMotorSpeed, feedback->wRightMotorSpeed});
    feedback->dwStatus = kErrorSuccess;  // synchronous completion (never ERROR_IO_PENDING)
    return kErrorSuccess;
}

XppDevices::Stats XppDevices::GetStats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

XppDevices& GlobalXpp() noexcept {
    static XppDevices devices(&GetRouter);
    return devices;
}

// ---- the 7 hooks ---------------------------------------------------------------------------------------------------

// XPP::XInitDevices 0x28CFE1 (5-byte jmp -> XAPILIB::XInitDevices 0x28C3B5) - stdcall (DWORD
// dwPreallocTypeCount, PXDEVICE_PREALLOC_TYPE), void, ret 8. Only caller: init 0x9C2D3 (FUN_0009C2A0, once from
// FUN_000D26A0) with (1, {&XDEVICE_TYPE_GAMEPAD, 4}); eax is not read (next: xor ebx,ebx). The original starts the
// USB/MU driver stack (MU_Init 0x28C09C only from here, avi.md 4.1) - not needed. Starts the host input layer.
void __stdcall XInitDevicesHle(std::uint32_t preallocTypeCount, const void* preallocTypes) noexcept {
    if (xdk::detail::InPurgeProbe()) {
        return;
    }
    StartInput();
    XDK_TRACE_CALL(XInitDevices, "(%u, types=0x%08X) -> input layer started", preallocTypeCount, A(preallocTypes));
}
XDK_HOOK(XInitDevices, &XInitDevicesHle);

// XPP::XGetDevices 0x28CFE6 - stdcall (PXPP_DEVICE_TYPE), DWORD port bitmask, ret 4. Init 0x9C328 (Gamepad ->
// ebp = mask; XInputOpen for every set bit 0..3, 0x9C340..0x9C3A1) and 0x9C334 (MemoryUnit, result discarded).
// Gamepad: the router's connected ports; the mask becomes the known state for XGetDeviceChanges. MU: 0.
std::uint32_t __stdcall XGetDevicesHle(const void* deviceType) noexcept {
    std::uint32_t mask = 0;
    if (!xdk::detail::InPurgeProbe() && IsGamepad(deviceType)) {
        StartInput();
        mask = GlobalXpp().GetDevices();
    }
    XDK_TRACE_CALL(XGetDevices, "(%s 0x%08X) -> 0x%X", DeviceTypeName(deviceType), A(deviceType), mask);
    return mask;
}
XDK_HOOK(XGetDevices, &XGetDevicesHle);

// XPP::XGetDeviceChanges 0x28D008 - stdcall (PXPP_DEVICE_TYPE, PDWORD pdwInsertions, PDWORD pdwRemovals), BOOL,
// ret 12. Only caller: hotplug 0x9C15D per tick with zeroed locals; `test eax,eax; je 0x9C210` skips everything for
// FALSE. TRUE: per port first the removal (close handle, reset values, ctrl+0x138 = 1), then the insertion (close an
// old handle, XInputOpen, reset, ctrl+0x138 = 0) - both bits = reopen (m4_input.md 2).
std::int32_t __stdcall XGetDeviceChangesHle(const void* deviceType, std::uint32_t* insertions,
                                            std::uint32_t* removals) noexcept {
    std::uint32_t ins = 0;
    std::uint32_t rem = 0;
    bool changed = false;
    if (!xdk::detail::InPurgeProbe() && IsGamepad(deviceType)) {
        StartInput();
        changed = GlobalXpp().GetDeviceChanges(ins, rem);
    }
    if (insertions != nullptr) {
        *insertions = ins;
    }
    if (removals != nullptr) {
        *removals = rem;
    }
    XDK_TRACE_CALL(XGetDeviceChanges, "(%s 0x%08X, 0x%08X, 0x%08X) -> %s (ins 0x%X, rem 0x%X)",
                   DeviceTypeName(deviceType), A(deviceType), A(insertions), A(removals), changed ? "TRUE" : "FALSE",
                   ins, rem);
    return changed ? TRUE : FALSE;
}
XDK_HOOK(XGetDeviceChanges, &XGetDeviceChangesHle);

// XPP::XInputOpen 0x28CE5C - stdcall (PXPP_DEVICE_TYPE, DWORD dwPort, DWORD dwSlot,
// PXINPUT_POLLING_PARAMETERS), HANDLE, ret 16. Callers 0x9C1E9 (insertion) and 0x9C383 (init), port from the table
// 0x2D65A0 = {0,1,2,3}, slot 0, NULL; both store eax at ctrl+0x150. NULL would leave the port "connected" (+0x138 = 0)
// without input, so every gamepad port 0..3 gets a handle (a guest dummy object, unique per port).
// ASSUMPTION: GetLastError is not set for a failure (the game never reads it after XInputOpen).
void* __stdcall XInputOpenHle(const void* deviceType, std::uint32_t port, std::uint32_t slot,
                              const void* pollingParameters) noexcept {
    void* handle = nullptr;
    if (!xdk::detail::InPurgeProbe() && IsGamepad(deviceType)) {
        StartInput();
        handle = GlobalXpp().Open(port);
    }
    XDK_TRACE_CALL(XInputOpen, "(%s 0x%08X, port=%u, slot=%u, poll=0x%08X) -> 0x%08X", DeviceTypeName(deviceType),
                   A(deviceType), port, slot, A(pollingParameters), A(handle));
    return handle;
}
XDK_HOOK(XInputOpen, &XInputOpenHle);

// XPP::XInputClose 0x28CEB2 - stdcall (HANDLE), void, ret 4. Callers 0x9C197/0x9C1C9/0x9C363, only for a non-zero
// ctrl+0x150. Closes the port's handle; its motors go to 0.
void __stdcall XInputCloseHle(void* device) noexcept {
    if (!xdk::detail::InPurgeProbe()) {
        GlobalXpp().Close(device);
    }
    XDK_TRACE_CALL(XInputClose, "(0x%08X)", A(device));
}
XDK_HOOK(XInputClose, &XInputCloseHle);

// XPP::XInputGetState 0x28CEBE - stdcall (HANDLE, PXINPUT_STATE), DWORD, ret 8. Only caller: controller update
// 0x9C3E6, only for ctrl+0x150 != 0 (0x9C3DC), buffer on the stack; `test eax,eax; je 0x9C3FD` - any non-zero result
// only resets the controller (FUN_0009BB50) without reading the state, the port stays "connected" (removals come only
// from XGetDeviceChanges). The original returns 0, 0x48F or 0x57 (avi.md 4.1). Called once per tick and open port:
// the router samples its sources here (m4_input.md 8.3).
std::uint32_t __stdcall XInputGetStateHle(void* device, XInputState* state) noexcept {
    std::uint32_t r = kErrorInvalidParameter;
    if (xdk::detail::InPurgeProbe()) {
        if (state != nullptr) {
            std::memset(state, 0, sizeof(*state));
        }
    } else {
        r = GlobalXpp().GetState(device, state);
    }
    XDK_TRACE_CALL(XInputGetState, "(0x%08X, state=0x%08X) -> 0x%X", A(device), A(state), r);
    return r;
}
XDK_HOOK(XInputGetState, &XInputGetStateHle);

// XPP::XInputSetState 0x28CF2A - stdcall (HANDLE, PXINPUT_FEEDBACK), DWORD, ret 8. Only caller: rumble 0x9C28C
// (FUN_0009C220 from update 0x9C996, only with an open handle and the dirty flag ctrl+0x148), result ignored (next:
// mov byte [esi+0x148],0). The game sends only while feedback.dwStatus != ERROR_IO_PENDING (0x9C229) and never resets
// it itself (ctor 0x9C112), so the status is completed synchronously. hEvent is never written by the game (search for
// `0x158]` in 0x9B000-0x9D000: no hit; m4_input.md 2) and is ignored. Motor speeds 0..65535 (left = low frequency).
std::uint32_t __stdcall XInputSetStateHle(void* device, XInputFeedback* feedback) noexcept {
    std::uint32_t r = kErrorInvalidParameter;
    if (xdk::detail::InPurgeProbe()) {
        if (feedback != nullptr) {
            feedback->dwStatus = r;
        }
    } else {
        r = GlobalXpp().SetState(device, feedback);
    }
    XDK_TRACE_CALL(XInputSetState, "(0x%08X, feedback=0x%08X motors=%u/%u) -> 0x%X, dwStatus=0x%X", A(device),
                   A(feedback), feedback != nullptr ? feedback->wLeftMotorSpeed : 0u,
                   feedback != nullptr ? feedback->wRightMotorSpeed : 0u, r, feedback != nullptr ? feedback->dwStatus : 0u);
    return r;
}
XDK_HOOK(XInputSetState, &XInputSetStateHle);

}  // namespace inp
