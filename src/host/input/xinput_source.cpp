// XInput 1.4 backend (see xinput_source.h).
#include "input/xinput_source.h"

#include "core/log.h"

#include <Xinput.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace inp {
namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

// xinput1_4.dll ships with Windows 8 and later (Windows SDK: XInput 1.4); xinput9_1_0.dll is the older system
// variant with the same GetState/SetState. Loaded at run time so that a missing DLL only means "no controllers".
class SystemXInputApi final : public XInputApi {
public:
    SystemXInputApi() noexcept {
        static constexpr const wchar_t* kDlls[] = {L"xinput1_4.dll", L"xinput9_1_0.dll"};
        static constexpr const char* kNames[] = {"xinput1_4.dll", "xinput9_1_0.dll"};
        for (int i = 0; i < 2; ++i) {
            HMODULE m = LoadLibraryExW(kDlls[i], nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (m == nullptr) {
                continue;
            }
            getState_ = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
            setState_ = reinterpret_cast<SetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputSetState")));
            if (getState_ != nullptr && setState_ != nullptr) {
                dllName_ = kNames[i];
                return;
            }
            getState_ = nullptr;
            setState_ = nullptr;
            FreeLibrary(m);
        }
    }
    bool Available() const noexcept override { return getState_ != nullptr; }
    const char* DllName() const noexcept override { return dllName_; }
    std::uint32_t GetState(std::uint32_t user, std::uint32_t& packet, PhysicalPad& pad) noexcept override {
        if (getState_ == nullptr) {
            return kXInputErrorNotConnected;
        }
        XINPUT_STATE s{};
        const DWORD r = getState_(user, &s);
        if (r != ERROR_SUCCESS) {
            return r;
        }
        packet = s.dwPacketNumber;
        pad.buttons = s.Gamepad.wButtons;
        pad.leftTrigger = s.Gamepad.bLeftTrigger;
        pad.rightTrigger = s.Gamepad.bRightTrigger;
        pad.lx = s.Gamepad.sThumbLX;
        pad.ly = s.Gamepad.sThumbLY;
        pad.rx = s.Gamepad.sThumbRX;
        pad.ry = s.Gamepad.sThumbRY;
        return ERROR_SUCCESS;
    }
    std::uint32_t SetState(std::uint32_t user, std::uint16_t left, std::uint16_t right) noexcept override {
        if (setState_ == nullptr) {
            return kXInputErrorNotConnected;
        }
        XINPUT_VIBRATION v{left, right};
        return setState_(user, &v);
    }

private:
    GetStateFn getState_ = nullptr;
    SetStateFn setState_ = nullptr;
    const char* dllName_ = "none";
};

std::uint64_t TickCountClock() noexcept {
    return GetTickCount64();
}

}  // namespace

namespace {
std::atomic<PadShaper> g_padShaper{nullptr};  // G5
}  // namespace

void SetPadShaper(PadShaper shaper) noexcept {
    g_padShaper.store(shaper, std::memory_order_release);
}

XInputApi& SystemXInput() noexcept {
    static SystemXInputApi api;  // thread-safe static initialization
    return api;
}

XInputPadSource::XInputPadSource(XInputApi& api, std::uint32_t user, MillisecondClock clock) noexcept
    : api_(api), user_(user), clock_(clock != nullptr ? clock : &TickCountClock) {
    std::snprintf(name_, sizeof(name_), "XInput controller %u", user + 1);
}

void XInputPadSource::Configure(const BindingTable* bindings, std::uint32_t deadzonePercent, int port) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    bindings_ = bindings;
    deadzone_ = deadzonePercent;
    port_ = std::clamp(port, 0, kPortCount - 1);
}

std::uint64_t XInputPadSource::Now() const noexcept {
    return clock_();
}

bool XInputPadSource::QueryLocked(PhysicalPad& pad, std::uint64_t now) const noexcept {
    std::uint32_t packet = 0;
    PhysicalPad p{};
    const std::uint32_t r = api_.GetState(user_, packet, p);
    lastQueryMs_ = now;
    everQueried_ = true;
    const bool connected = r == kXInputErrorSuccess;
    if (connected != connected_) {
        connected_ = connected;
        if (connected) {
            ++stats_.connects;
            FUT_LOGI(Inp, "input: %s connected (XInput user index %u)", name_, user_);
            sent_ = {};  // a fresh device has its motors off; re-send what the game wants
            SendRumbleLocked();
        } else {
            ++stats_.disconnects;
            FUT_LOGI(Inp, "input: %s disconnected (XInputGetState 0x%X)", name_, r);
            sent_ = {};
        }
    }
    last_ = connected ? p : PhysicalPad{};
    pad = last_;
    return connected;
}

bool XInputPadSource::Connected() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = Now();
    const std::uint64_t interval = connected_ ? kStaleIntervalMs : kProbeIntervalMs;
    if (!everQueried_ || now - lastQueryMs_ >= interval) {
        ++stats_.probes;
        PhysicalPad ignored;
        QueryLocked(ignored, now);
    }
    return connected_;
}

bool XInputPadSource::Sample(int port, std::uint64_t tick, PadState& out) noexcept {
    (void)port;
    (void)tick;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = Now();
    // A slot that is not connected is only probed at the probe interval (also from Sample).
    if (!connected_ && everQueried_ && now - lastQueryMs_ < kProbeIntervalMs) {
        return false;
    }
    ++stats_.samples;
    PhysicalPad pad;
    if (!QueryLocked(pad, now)) {
        return false;
    }
    out = MapPad(ApplyStickDeadzone(pad, deadzone_), bindings_ != nullptr ? *bindings_ : DefaultBindings());
    if (const PadShaper shaper = g_padShaper.load(std::memory_order_acquire)) {
        shaper(port, out);  // G5
    }
    return true;
}

void XInputPadSource::SendRumbleLocked() const noexcept {
    if (!connected_ || requested_ == sent_) {
        return;
    }
    ++stats_.rumbleCommands;
    stats_.maxRumble.left = std::max(stats_.maxRumble.left, requested_.left);
    stats_.maxRumble.right = std::max(stats_.maxRumble.right, requested_.right);
    const std::uint32_t r = api_.SetState(user_, requested_.left, requested_.right);
    if (r == kXInputErrorSuccess) {
        sent_ = requested_;
    }
    FUT_LOGT(Inp, "input: %s rumble %u/%u -> 0x%X", name_, requested_.left, requested_.right, r);
}

void XInputPadSource::SetRumble(int port, const RumbleState& rumble) noexcept {
    (void)port;
    std::lock_guard<std::mutex> lock(mutex_);
    requested_ = rumble;
    SendRumbleLocked();
}

void XInputPadSource::StopMotors() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    requested_ = {};
    if (connected_ && !(sent_ == RumbleState{})) {
        api_.SetState(user_, 0, 0);
        sent_ = {};
    }
}

PhysicalPad XInputPadSource::LastPhysical() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

XInputPadSource::Stats XInputPadSource::GetStats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace inp
