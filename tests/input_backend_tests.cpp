// Unit tests of the M4 input backends (input/xinput_source.h, input/kbm_source.h, input/kbm_window.h) with
// simulated devices, plus a smoke test of the real XInput DLL (only "connected or not": no controller is assumed).
#include "doctest.h"

#include "input/input_config.h"
#include "input/kbm_source.h"
#include "input/kbm_window.h"
#include "input/xinput_source.h"

#include <string>
#include <vector>

namespace {

// Simulated XInput (4 slots).
class FakeXInput final : public inp::XInputApi {
public:
    bool Available() const noexcept override { return true; }
    const char* DllName() const noexcept override { return "fake"; }
    std::uint32_t GetState(std::uint32_t user, std::uint32_t& packet, inp::PhysicalPad& pad) noexcept override {
        ++getCalls[user];
        if (!connected[user]) {
            return inp::kXInputErrorNotConnected;
        }
        packet = ++packets[user];
        pad = pads[user];
        return inp::kXInputErrorSuccess;
    }
    std::uint32_t SetState(std::uint32_t user, std::uint16_t left, std::uint16_t right) noexcept override {
        if (!connected[user]) {
            return inp::kXInputErrorNotConnected;
        }
        motors[user].push_back({left, right});
        return inp::kXInputErrorSuccess;
    }
    bool connected[4] = {};
    inp::PhysicalPad pads[4] = {};
    std::uint32_t packets[4] = {};
    int getCalls[4] = {};
    std::vector<inp::RumbleState> motors[4];
};

std::uint64_t g_nowMs = 1000;
std::uint64_t FakeClock() noexcept {
    return g_nowMs;
}

bool g_inMenu = false;
bool MenuQuery() noexcept {
    return g_inMenu;
}

inp::PadState SampleOnce(inp::InputSource& s) {
    inp::PadState out;
    s.Sample(0, 0, out);
    return out;
}

}  // namespace

TEST_CASE("input XInput source: probing of empty slots, connection, mapping with dead zone") {
    FakeXInput api;
    g_nowMs = 1000;
    inp::XInputPadSource pad(api, 1, &FakeClock);
    const inp::BindingTable bindings = inp::DefaultBindings();
    pad.Configure(&bindings, 10, 0);
    CHECK(std::string(pad.Name()) == "XInput controller 2");
    CHECK(pad.Kind() == inp::SourceKind::Physical);
    CHECK(pad.HasMotors());
    // Empty slot: the first query probes, later queries within 1 s do not.
    CHECK_FALSE(pad.Connected());
    CHECK(api.getCalls[1] == 1);
    inp::PadState out;
    for (int i = 0; i < 10; ++i) {
        CHECK_FALSE(pad.Connected());
        CHECK_FALSE(pad.Sample(0, static_cast<std::uint64_t>(i), out));
    }
    CHECK(api.getCalls[1] == 1);
    // Plugged in: seen at the next probe.
    api.connected[1] = true;
    api.pads[1].buttons = inp::kXiA | inp::kXiRightShoulder;
    api.pads[1].leftTrigger = 200;
    api.pads[1].lx = 2000;  // inside the 10 % dead zone
    api.pads[1].ry = -20000;
    g_nowMs += inp::XInputPadSource::kProbeIntervalMs;
    CHECK(pad.Connected());
    REQUIRE(pad.Sample(0, 0, out));
    CHECK(out.analog[inp::kAnalogA] == 255);
    CHECK(out.analog[inp::kAnalogBlack] == 255);
    CHECK(out.analog[inp::kAnalogLeftTrigger] == 200);
    CHECK(out.thumb[inp::kAxisLX] == 0);
    CHECK(out.thumb[inp::kAxisRY] == -20000);
    // A connected, sampled pad is not probed again by Connected() within 250 ms.
    const int calls = api.getCalls[1];
    CHECK(pad.Connected());
    CHECK(api.getCalls[1] == calls);
    // Unplugged: Sample notices it at once.
    api.connected[1] = false;
    CHECK_FALSE(pad.Sample(0, 1, out));
    CHECK_FALSE(pad.Connected());
    const inp::XInputPadSource::Stats st = pad.GetStats();
    CHECK(st.connects == 1);
    CHECK(st.disconnects == 1);
}

TEST_CASE("input XInput source: rumble only on change, re-sent after a reconnection, stopped at exit") {
    FakeXInput api;
    g_nowMs = 5000;
    inp::XInputPadSource pad(api, 0, &FakeClock);
    pad.Configure(nullptr, 0, 0);
    api.connected[0] = true;
    CHECK(pad.Connected());
    pad.SetRumble(0, {1000, 2000});
    pad.SetRumble(0, {1000, 2000});
    REQUIRE(api.motors[0].size() == 1);
    CHECK(api.motors[0][0] == inp::RumbleState{1000, 2000});
    api.connected[0] = false;
    inp::PadState out;
    CHECK_FALSE(pad.Sample(0, 0, out));
    pad.SetRumble(0, {3000, 0});  // stored while unplugged
    CHECK(api.motors[0].size() == 1);
    api.connected[0] = true;
    g_nowMs += inp::XInputPadSource::kProbeIntervalMs;
    CHECK(pad.Connected());
    REQUIRE(api.motors[0].size() == 2);
    CHECK(api.motors[0][1] == inp::RumbleState{3000, 0});
    pad.StopMotors();
    REQUIRE(api.motors[0].size() == 3);
    CHECK(api.motors[0][2] == inp::RumbleState{});
    CHECK(pad.GetStats().maxRumble == inp::RumbleState{3000, 2000});
}

TEST_CASE("input XInput: the real xinput1_4.dll loads; slots read connected or not connected") {
    inp::XInputApi& api = inp::SystemXInput();
    CHECK(api.Available());
    CHECK(std::string(api.DllName()) == "xinput1_4.dll");
    int connected = 0;
    for (std::uint32_t u = 0; u < 4; ++u) {
        std::uint32_t packet = 0;
        inp::PhysicalPad pad;
        const std::uint32_t r = api.GetState(u, packet, pad);
        CHECK((r == inp::kXInputErrorSuccess || r == inp::kXInputErrorNotConnected));
        connected += r == inp::kXInputErrorSuccess ? 1 : 0;
    }
    // A slot index beyond the 4 users is rejected by XInput itself.
    std::uint32_t packet = 0;
    inp::PhysicalPad pad;
    CHECK(api.GetState(4, packet, pad) != inp::kXInputErrorSuccess);
    MESSAGE("real XInput controllers connected during the test: " << connected
            << " (input and rumble of real hardware are not tested here)");
}

TEST_CASE("input keyboard/mouse: taps shorter than a tick last one tick, held keys stay, focus loss releases all") {
    inp::KeyboardMouseSource kbm;
    const inp::BindingTable bindings = inp::DefaultBindings();
    kbm.Configure(&bindings, 1.0f, false);
    CHECK(kbm.Connected());
    CHECK(kbm.Kind() == inp::SourceKind::Physical);
    CHECK(SampleOnce(kbm).Neutral());
    // Enter pressed and released between two samples: A for exactly one tick (m4_input.md 8.3).
    kbm.KeyEvent(0x0D, true);
    kbm.KeyEvent(0x0D, false);
    CHECK(SampleOnce(kbm).analog[inp::kAnalogA] == 255);
    CHECK(SampleOnce(kbm).analog[inp::kAnalogA] == 0);
    // Held W: LY up in every tick; key repeat does not matter.
    kbm.KeyEvent('W', true);
    kbm.KeyEvent('W', true);
    CHECK(SampleOnce(kbm).thumb[inp::kAxisLY] == 32767);
    CHECK(SampleOnce(kbm).thumb[inp::kAxisLY] == 32767);
    // Mouse button tap.
    kbm.MouseButtonEvent(1, true);
    kbm.MouseButtonEvent(1, false);
    CHECK(SampleOnce(kbm).analog[inp::kAnalogX] == 255);
    CHECK(SampleOnce(kbm).analog[inp::kAnalogX] == 0);
    // Focus loss.
    kbm.MouseButtonEvent(0, true);
    kbm.MouseMoveEvent(100, 0);
    kbm.ReleaseAll();
    const inp::PadState s = SampleOnce(kbm);
    CHECK(s.Neutral());
    CHECK_FALSE(kbm.KeyDown('W'));
    // Disabled: not connected, events ignored.
    kbm.SetEnabled(false);
    CHECK_FALSE(kbm.Connected());
    kbm.KeyEvent('W', true);
    inp::PadState out;
    CHECK_FALSE(kbm.Sample(0, 0, out));
    kbm.SetEnabled(true);
    CHECK_FALSE(kbm.KeyDown('W'));
    const inp::KeyboardMouseSource::Stats st = kbm.GetStats();
    CHECK(st.keyEvents == 4);
    CHECK(st.releases >= 2);
}

TEST_CASE("input keyboard/mouse: wheel notches press for 2 ticks with a release between them") {
    inp::KeyboardMouseSource kbm;
    kbm.Configure(nullptr, 1.0f, false);
    kbm.WheelEvent(2);  // two notches up -> BLACK
    std::vector<int> seq;
    for (int i = 0; i < 7; ++i) {
        seq.push_back(SampleOnce(kbm).analog[inp::kAnalogBlack] != 0 ? 1 : 0);
    }
    CHECK(seq == std::vector<int>{1, 1, 0, 1, 1, 0, 0});
    kbm.WheelEvent(-10);  // capped at 3 queued notches -> WHITE
    int presses = 0;
    bool prev = false;
    for (int i = 0; i < 20; ++i) {
        const bool down = SampleOnce(kbm).analog[inp::kAnalogWhite] != 0;
        presses += down && !prev ? 1 : 0;
        prev = down;
    }
    CHECK(presses == 3);
}

TEST_CASE("input keyboard/mouse: high-resolution wheel fractions add up to whole notches") {
    // Review M4 finding 3: one detent of a 15-unit wheel = 8 records; it must press BLACK once, not 3 times.
    inp::KeyboardMouseSource kbm;
    kbm.Configure(nullptr, 1.0f, false);
    for (int i = 0; i < 8; ++i) {
        kbm.WheelDeltaEvent(15);
    }
    int presses = 0;
    bool prev = false;
    for (int i = 0; i < 30; ++i) {
        const bool down = SampleOnce(kbm).analog[inp::kAnalogBlack] != 0;
        presses += down && !prev ? 1 : 0;
        prev = down;
    }
    CHECK(presses == 1);
    // Half a detent does nothing; a change of direction drops the partial notch.
    kbm.WheelDeltaEvent(60);
    kbm.WheelDeltaEvent(-60);
    kbm.WheelDeltaEvent(-30);
    for (int i = 0; i < 6; ++i) {
        const inp::PadState s = SampleOnce(kbm);
        CHECK(s.analog[inp::kAnalogBlack] == 0);
        CHECK(s.analog[inp::kAnalogWhite] == 0);
    }
    kbm.WheelDeltaEvent(-30);  // -120 in total since the direction change -> one WHITE notch
    CHECK(SampleOnce(kbm).analog[inp::kAnalogWhite] != 0);
    // Classic wheels: one record of 2 * WHEEL_DELTA = two notches.
    kbm.ReleaseAll();
    kbm.WheelDeltaEvent(2 * inp::kWheelDelta);
    std::vector<int> seq;
    for (int i = 0; i < 7; ++i) {
        seq.push_back(SampleOnce(kbm).analog[inp::kAnalogBlack] != 0 ? 1 : 0);
    }
    CHECK(seq == std::vector<int>{1, 1, 0, 1, 1, 0, 0});
}

TEST_CASE("input keyboard/mouse: mouse movement accumulates until the sample (sensitivity, inverse curve)") {
    inp::KeyboardMouseSource kbm;
    kbm.Configure(nullptr, 2.0f, false);
    // 6 counts at sensitivity 2: wish 6 * 2 / 24 = 0.5 to the right, split over three events.
    kbm.MouseMoveEvent(2, 0);
    kbm.MouseMoveEvent(2, 0);
    kbm.MouseMoveEvent(2, 0);
    const inp::PadState s = SampleOnce(kbm);
    CHECK(s.thumb[inp::kAxisRX] > 0);
    CHECK(s.thumb[inp::kAxisRY] == 0);
    // Raw value for a deflection of 0.5 with dead zone 0.4: 0.4 + 0.5 * 0.6 / 1.3.
    CHECK(static_cast<float>(s.thumb[inp::kAxisRX]) / 32768.0f == doctest::Approx(0.4f + 0.5f * 0.6f / 1.3f).epsilon(0.001));
    CHECK(SampleOnce(kbm).thumb[inp::kAxisRX] == 0);  // consumed
    // Mouse up (negative y) = stick up.
    kbm.MouseMoveEvent(0, -100);
    CHECK(SampleOnce(kbm).thumb[inp::kAxisRY] > 28000);
    // The raw view keeps its own accumulator for later consumers (freecam).
    const inp::KeyboardMouseSource::RawSnapshot raw = kbm.ReadRaw(true);
    CHECK(raw.dx == 6);
    CHECK(raw.dy == -100);
    CHECK(kbm.ReadRaw(false).dx == 0);
}

TEST_CASE("input keyboard/mouse: menu escape - START becomes BACK while the frontend is active") {
    inp::KeyboardMouseSource kbm;
    kbm.Configure(nullptr, 1.0f, true);
    kbm.SetMenuQuery(&MenuQuery);
    g_inMenu = true;
    kbm.KeyEvent(0x1B, true);
    CHECK(SampleOnce(kbm).buttons == inp::kButtonBack);
    g_inMenu = false;  // decided when the key went down
    CHECK(SampleOnce(kbm).buttons == inp::kButtonBack);
    kbm.KeyEvent(0x1B, false);
    CHECK(SampleOnce(kbm).Neutral());
    kbm.KeyEvent(0x1B, true);  // in-game: START (pause)
    CHECK(SampleOnce(kbm).buttons == inp::kButtonStart);
    kbm.KeyEvent(0x1B, false);
    SampleOnce(kbm);
    // Option off: always START.
    kbm.Configure(nullptr, 1.0f, false);
    g_inMenu = true;
    kbm.KeyEvent(0x1B, true);
    CHECK(SampleOnce(kbm).buttons == inp::kButtonStart);
    g_inMenu = false;
    CHECK(kbm.GetStats().menuEscapes == 1);
}

TEST_CASE("input keyboard/mouse: Raw Input key normalization (left/right modifiers, fake keys)") {
    constexpr std::uint16_t kE0 = 2;
    CHECK(inp::NormalizeRawKey(0x10, 0x2A, 0) == 0xA0);    // left shift
    CHECK(inp::NormalizeRawKey(0x10, 0x36, 0) == 0xA1);    // right shift
    CHECK(inp::NormalizeRawKey(0x11, 0x1D, 0) == 0xA2);    // left ctrl
    CHECK(inp::NormalizeRawKey(0x11, 0x1D, kE0) == 0xA3);  // right ctrl
    CHECK(inp::NormalizeRawKey(0x12, 0x38, 0) == 0xA4);    // left alt
    CHECK(inp::NormalizeRawKey(0x12, 0x38, kE0) == 0xA5);  // right alt (AltGr)
    CHECK(inp::NormalizeRawKey(0x0D, 0x1C, kE0) == 0x0D);  // numpad Enter = enter
    CHECK(inp::NormalizeRawKey(0xFF, 0x2A, kE0) == 0);     // fake key of an escape sequence
    CHECK(inp::NormalizeRawKey('W', 0x11, 0) == 'W');
    // The generic modifier codes follow their left/right keys.
    inp::KeyboardMouseSource kbm;
    inp::BindingTable b = inp::DefaultBindings();
    std::string error;
    REQUIRE(inp::ParseBindingList("key:vk_10", inp::BindDevice::KeyboardMouse,
                                  b.kbm[static_cast<int>(inp::Control::Y)], error));
    kbm.Configure(&b, 1.0f, false);
    kbm.KeyEvent(0xA1, true);
    CHECK(SampleOnce(kbm).analog[inp::kAnalogY] == 255);
}

TEST_CASE("input window bridge: Raw Input registration on the window's thread, focus loss releases the keys") {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FutInputBridgeTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"input test", WS_OVERLAPPEDWINDOW, 0, 0, 200, 200, nullptr,
                                nullptr, wc.hInstance, nullptr);
    REQUIRE(hwnd != nullptr);
    inp::KeyboardMouseSource kbm;
    kbm.Configure(nullptr, 1.0f, false);
    REQUIRE(inp::AttachKeyboardMouse(hwnd, &kbm, inp::KbmWindowOptions{true}));
    CHECK_FALSE(inp::AttachKeyboardMouse(hwnd, &kbm, inp::KbmWindowOptions{true}));  // once per window
    MSG msg;
    for (int i = 0; i < 50 && !inp::KeyboardMouseAttached(); ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&msg);
        }
        Sleep(1);
    }
    CHECK(inp::KeyboardMouseAttached());
    kbm.KeyEvent('W', true);
    const std::uint32_t releasesBefore = inp::GetKbmWindowStats().focusReleases;
    SendMessageW(hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
    CHECK_FALSE(kbm.KeyDown('W'));
    CHECK(inp::GetKbmWindowStats().focusReleases == releasesBefore + 1);
    CHECK_FALSE(inp::MouseCaptured());
    DestroyWindow(hwnd);
    CHECK_FALSE(inp::KeyboardMouseAttached());
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
