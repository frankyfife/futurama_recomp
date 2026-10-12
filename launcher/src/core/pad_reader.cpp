#include "core/pad_reader.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fl {
namespace {

#ifdef _WIN32
// XInput 1.4 structures (xinput.h), declared here so the launcher does not link xinput.lib.
struct XGamepad {
    WORD buttons;
    BYTE leftTrigger;
    BYTE rightTrigger;
    SHORT thumbLX;
    SHORT thumbLY;
    SHORT thumbRX;
    SHORT thumbRY;
};
struct XState {
    DWORD packetNumber;
    XGamepad gamepad;
};
struct XVibration {
    WORD leftMotorSpeed;
    WORD rightMotorSpeed;
};
using GetStateFn = DWORD(WINAPI*)(DWORD, XState*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XVibration*);

class XInputReader final : public PadReader {
public:
    XInputReader() {
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            module_ = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module_ != nullptr) {
                name_ = QString::fromWCharArray(name);
                break;
            }
        }
        if (module_ != nullptr) {
            getState_ = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(module_, "XInputGetState")));
            setState_ = reinterpret_cast<SetStateFn>(reinterpret_cast<void*>(GetProcAddress(module_, "XInputSetState")));
        }
    }
    ~XInputReader() override {
        // Never leave a motor running (the test button stops it after a moment, but the window may close first).
        for (int slot = 0; slot < kPadSlots; ++slot) {
            if (rumbling_[static_cast<size_t>(slot)]) {
                setRumble(slot, 0.0, 0.0);
            }
        }
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }
    XInputReader(const XInputReader&) = delete;
    XInputReader& operator=(const XInputReader&) = delete;

    bool available() const override { return getState_ != nullptr; }
    QString backendName() const override { return name_; }

    PadState read(int slot) override {
        PadState out;
        if (getState_ == nullptr || slot < 0 || slot >= kPadSlots) {
            return out;
        }
        XState s{};
        if (getState_(static_cast<DWORD>(slot), &s) != ERROR_SUCCESS) {
            return out;
        }
        out.connected = true;
        out.packet = s.packetNumber;
        out.buttons = s.gamepad.buttons;
        out.leftTrigger = s.gamepad.leftTrigger;
        out.rightTrigger = s.gamepad.rightTrigger;
        out.lx = s.gamepad.thumbLX;
        out.ly = s.gamepad.thumbLY;
        out.rx = s.gamepad.thumbRX;
        out.ry = s.gamepad.thumbRY;
        return out;
    }

    bool setRumble(int slot, double left, double right) override {
        if (setState_ == nullptr || slot < 0 || slot >= kPadSlots) {
            return false;
        }
        XVibration v{static_cast<WORD>(std::clamp(left, 0.0, 1.0) * 65535.0 + 0.5),
                     static_cast<WORD>(std::clamp(right, 0.0, 1.0) * 65535.0 + 0.5)};
        const bool ok = setState_(static_cast<DWORD>(slot), &v) == ERROR_SUCCESS;
        rumbling_[static_cast<size_t>(slot)] = ok && (v.leftMotorSpeed != 0 || v.rightMotorSpeed != 0);
        return ok;
    }

private:
    HMODULE module_ = nullptr;
    QString name_;
    GetStateFn getState_ = nullptr;
    SetStateFn setState_ = nullptr;
    std::array<bool, kPadSlots> rumbling_{};
};
#endif

PadReaderFactory& Factory() {
    static PadReaderFactory factory;
    return factory;
}

}  // namespace

std::unique_ptr<PadReader> CreateXInputReader() {
#ifdef _WIN32
    return std::make_unique<XInputReader>();
#else
    return std::make_unique<SimulatedPadReader>();
#endif
}

PadState SimulatedPadReader::read(int slot) {
    if (slot < 0 || slot >= kPadSlots) {
        return {};
    }
    ++reads_[static_cast<size_t>(slot)];
    return states_[static_cast<size_t>(slot)];
}

bool SimulatedPadReader::setRumble(int slot, double left, double right) {
    if (slot < 0 || slot >= kPadSlots || !states_[static_cast<size_t>(slot)].connected) {
        return false;
    }
    Rumble& r = rumble_[static_cast<size_t>(slot)];
    r.left = left;
    r.right = right;
    ++r.calls;
    return true;
}

void SimulatedPadReader::setState(int slot, const PadState& state) {
    if (slot >= 0 && slot < kPadSlots) {
        states_[static_cast<size_t>(slot)] = state;
    }
}

SimulatedPadReader::Rumble SimulatedPadReader::rumble(int slot) const {
    return slot >= 0 && slot < kPadSlots ? rumble_[static_cast<size_t>(slot)] : Rumble{};
}

int SimulatedPadReader::readCount(int slot) const {
    return slot >= 0 && slot < kPadSlots ? reads_[static_cast<size_t>(slot)] : 0;
}

void SetPadReaderFactory(PadReaderFactory factory) {
    Factory() = std::move(factory);
}

std::unique_ptr<PadReader> CreatePadReader() {
    if (Factory()) {
        return Factory()();
    }
    return CreateXInputReader();
}

}  // namespace fl
