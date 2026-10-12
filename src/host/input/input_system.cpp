// The PC port's input layer: wiring (see input_system.h).
#include "input/input_system.h"

#include "core/config.h"
#include "core/crash.h"
#include "core/guest.h"
#include "core/host_main.h"
#include "core/log.h"
#include "gfx/freecam.h"
#include "input/input_script.h"
#include "input/kbm_window.h"
#include "input/level_skip.h"
#include "input/stick_curve.h"
#include "input/xpp_hle.h"
#include "platform/window.h"
#include "xapi/camera_response.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace inp {
namespace {

std::mutex g_startLock;
std::atomic<bool> g_started{false};

InputSettings& Settings() noexcept {
    static InputSettings settings;
    return settings;
}

// Installs the router before the guest runs (DLL load; input_api.h "registered with SetRouter before the guest runs").
[[maybe_unused]] const bool g_routerInstalled = [] {
    SetRouter(&GlobalRouter());
    return true;
}();

std::uint32_t ReadGuestU32(guest::GuestAddr p) noexcept {
    return (p != 0 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}

// G5 (precise camera response): the right stick of a controller with a small radial dead zone and a linear curve
// (stick_curve.h PreciseStickOutput), encoded so that the game's own curve yields it. Not while the port is diverted
// (the freecam reads the raw stick).
void PreciseRightStick(int port, PadState& state) noexcept {
    if (GlobalRouter().Diverted(port)) {
        return;
    }
    PreciseStickToGameThumbs(state.thumb[kAxisRX], state.thumb[kAxisRY]);
}

void StopMotorsCrashReporter() noexcept {
    for (std::uint32_t i = 0; i < kXInputUsers; ++i) {
        GlobalXInputPad(i).StopMotors();
    }
}

}  // namespace

HostRouter& GlobalRouter() noexcept {
    static HostRouter router;
    return router;
}

KeyboardMouseSource& GlobalKeyboardMouse() noexcept {
    static KeyboardMouseSource source;
    return source;
}

XInputPadSource& GlobalXInputPad(std::uint32_t user) noexcept {
    static XInputPadSource pads[kXInputUsers] = {
        XInputPadSource(SystemXInput(), 0), XInputPadSource(SystemXInput(), 1),
        XInputPadSource(SystemXInput(), 2), XInputPadSource(SystemXInput(), 3)};
    return pads[user < kXInputUsers ? user : 0];
}

const InputSettings& ActiveInputSettings() noexcept {
    return Settings();
}

bool InputStarted() noexcept {
    return g_started.load(std::memory_order_acquire);
}

bool GameWindowFocused() noexcept {
    const HWND frame = platform::MainFrameWindow();
    return frame == nullptr || GetForegroundWindow() == frame;
}

bool GameInFrontendMenu() noexcept {
    // [[g_pApp] + 0xBC] = current context, [context + 4] = its name (xapi/game_probes.cpp ContextName).
    const std::uint32_t app = ReadGuestU32(guest::kGameAppPtr);
    const std::uint32_t context = app != 0 ? ReadGuestU32(app + guest::kCAppCurrentContextOffset) : 0;
    const std::uint32_t name = context != 0 ? ReadGuestU32(context + guest::kGameContextNameOffset) : 0;
    static constexpr char kFrontend[] = "Frontend";
    if (name == 0 || !guest::IsReadable(name, sizeof(kFrontend))) {
        return false;
    }
    return std::memcmp(guest::Ptr<const char>(name), kFrontend, sizeof(kFrontend)) == 0;
}

void StartInput() noexcept {
    if (g_started.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_startLock);
    if (g_started.load(std::memory_order_relaxed)) {
        return;
    }
    FUT_HLE_TRY {
        std::vector<std::string> warnings;
        InputSettings& s = Settings();
        s = ReadInputSettings(core::GetConfig(), warnings);
        for (const std::string& w : warnings) {
            FUT_LOGW(Inp, "input: %s", w.c_str());
        }

        HostRouter& router = GlobalRouter();
        router.Configure({s.rumble, s.rumbleStrength, s.backgroundInput});
        router.SetFocusQuery(&GameWindowFocused);
        gfx::freecam::Configure();  // M4b: L3+R3 / key toggle, held-back chord buttons
        router.SetSampleFilter(&gfx::freecam::SampleFilter);
        levelskip::Configure();  // G7: A A B B A A B B A A in a level -> regular level end
        router.AddSampleFilter(&levelskip::SampleFilter);
        // G5: [input] camera_response = precise - smaller dead zone of the controllers' right stick.
        xapi::camera::LogConfiguration();
        SetPadShaper(xapi::camera::ResponseFromConfig(core::GetConfig()) == xapi::camera::Response::Precise
                         ? &PreciseRightStick
                         : nullptr);

        XInputApi& xinput = SystemXInput();
        for (std::uint32_t i = 0; i < kXInputUsers; ++i) {
            XInputPadSource& pad = GlobalXInputPad(i);
            pad.Configure(&s.bindings, s.padDeadzone, s.padMode == PadMode::PerPort ? static_cast<int>(i) : 0);
            RegisterSource(&pad);
        }
        KeyboardMouseSource& kbm = GlobalKeyboardMouse();
        kbm.Configure(&s.bindings, s.mouseSensitivity, s.menuEscape);
        kbm.SetMenuQuery(&GameInFrontendMenu);
        RegisterSource(&kbm);

        const HWND frame = platform::MainFrameWindow();
        const bool window = frame != nullptr && AttachKeyboardMouse(frame, &kbm, KbmWindowOptions{s.mouseCapture});

        core::AddExitReporter("input", &LogInputSummary);
        core::AddCrashReporter("input", &StopMotorsCrashReporter);

        std::uint32_t pads = 0;
        for (std::uint32_t i = 0; i < kXInputUsers; ++i) {
            pads += GlobalXInputPad(i).Connected() ? 1u : 0u;
        }
        FUT_LOGI(Inp,
                 "input: XPP HLE active - pad mode %s, XInput %s (%u of 4 controllers connected), keyboard/mouse %s, "
                 "rumble %s %u %%, mouse sensitivity %.2f, pad dead zone %u %%, menu escape %s, %d input sources",
                 PadModeName(s.padMode), xinput.Available() ? xinput.DllName() : "not available", pads,
                 window ? "on the game window (Raw Input)" : "without window (no keyboard/mouse events)",
                 s.rumble ? "on" : "off", s.rumbleStrength, static_cast<double>(s.mouseSensitivity), s.padDeadzone,
                 s.menuEscape ? "on" : "off", SourceCount());
        if (core::LogEnabled(core::Channel::Inp, core::Level::Debug)) {
            for (int c = 0; c < kControlCount; ++c) {
                const ControlInfo& info = Info(static_cast<Control>(c));
                FUT_LOGD(Inp, "input: %-10s pad [%s] kbm [%s]", info.id,
                         FormatBindingList(s.bindings.pad[c]).c_str(), FormatBindingList(s.bindings.kbm[c]).c_str());
            }
        }
        // Review M4 finding 1: load the script here, outside every XPP/router lock. Loaded lazily inside the first
        // XGetDevices, a syntax error ended the process under XppDevices::mutex_, and the exit reporter
        // LogInputSummary (GetStats) locked it again: std::terminate (0xC0000409) instead of exit code 1.
        script::PreloadScript();
        g_started.store(true, std::memory_order_release);
    }
    FUT_HLE_CATCH("inp::StartInput")
}

void LogInputSummary() noexcept {
    try {
        HostRouter& router = GlobalRouter();
        router.StopAllMotors();
        const XppDevices::Stats x = GlobalXpp().GetStats();
        std::string ports;
        for (int p = 0; p < kPortCount; ++p) {
            const HostRouter::PortStats st = router.Stats(p);
            if (st.samples == 0 && st.epoch == 0 && st.rumbleRequests == 0) {
                continue;
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "%sport %d: %llu ticks (%llu with input, %llu diverted, %llu held back), %u connection(s), "
                          "rumble %llu requests (%llu non-zero, max %u/%u)",
                          ports.empty() ? "" : "; ", p + 1, static_cast<unsigned long long>(st.samples),
                          static_cast<unsigned long long>(st.activeSamples),
                          static_cast<unsigned long long>(st.divertedSamples),
                          static_cast<unsigned long long>(st.suppressedSamples), st.epoch,
                          static_cast<unsigned long long>(st.rumbleRequests),
                          static_cast<unsigned long long>(st.rumbleNonZero), st.maxRumble.left, st.maxRumble.right);
            ports += buf;
        }
        std::string pads;
        for (std::uint32_t i = 0; i < kXInputUsers; ++i) {
            const XInputPadSource::Stats st = GlobalXInputPad(i).GetStats();
            if (st.connects == 0) {
                continue;
            }
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%scontroller %u: %u connect(s), %u disconnect(s), %u rumble commands (max %u/%u)",
                          pads.empty() ? "" : "; ", i + 1, st.connects, st.disconnects,
                          static_cast<unsigned>(st.rumbleCommands), st.maxRumble.left, st.maxRumble.right);
            pads += buf;
        }
        const KeyboardMouseSource::Stats k = GlobalKeyboardMouse().GetStats();
        const KbmWindowStats w = GetKbmWindowStats();
        FUT_LOGI(Inp,
                 "input summary: XInputGetState %llu (%llu failed), XInputSetState %llu, opens %u, closes %u, "
                 "insertions %u, removals %u; %s; %s; keyboard %llu / mouse %llu events (raw %llu/%llu, %llu mouse "
                 "records ignored without capture, %u captures, %u focus releases, %u menu escapes), focus changes %u",
                 static_cast<unsigned long long>(x.getStateCalls), static_cast<unsigned long long>(x.getStateFailures),
                 static_cast<unsigned long long>(x.setStateCalls), x.opens, x.closes, x.insertions, x.removals,
                 ports.empty() ? "no port sampled" : ports.c_str(), pads.empty() ? "no controller seen" : pads.c_str(),
                 static_cast<unsigned long long>(k.keyEvents), static_cast<unsigned long long>(k.mouseEvents),
                 static_cast<unsigned long long>(w.rawKeyboard), static_cast<unsigned long long>(w.rawMouse),
                 static_cast<unsigned long long>(w.ignoredMouse), w.captures, w.focusReleases, k.menuEscapes,
                 router.FocusChanges());
    } catch (...) {
    }
}

}  // namespace inp
