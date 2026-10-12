// Unit tests of the M4 input router and the XPP HLE with simulated devices (input/host_router.h, input/xpp_hle.h;
// analysis/loader/m4_input.md 2 "Vertrag für die HLE", 8.1). No real controller is involved.
#include "doctest.h"

#include "core/guest.h"
#include "input/host_router.h"
#include "input/xpp_hle.h"

#include <cstring>
#include <vector>

namespace {

class FakeSource final : public inp::InputSource {
public:
    FakeSource(const char* name, inp::SourceKind kind, int port, bool motors = false)
        : name_(name), kind_(kind), port_(port), motors_(motors) {}
    const char* Name() const noexcept override { return name_; }
    inp::SourceKind Kind() const noexcept override { return kind_; }
    int DefaultPort() const noexcept override { return port_; }
    bool Connected() const noexcept override { return connected; }
    bool Sample(int port, std::uint64_t tick, inp::PadState& out) noexcept override {
        ++samples;
        lastPort = port;
        ticks.push_back(tick);
        if (!contribute) {
            return false;
        }
        out = state;
        return true;
    }
    void SetRumble(int port, const inp::RumbleState& r) noexcept override {
        (void)port;
        rumble = r;
        ++rumbleCalls;
    }
    bool HasMotors() const noexcept override { return motors_; }

    inp::PadState state;
    bool connected = true;
    bool contribute = true;
    int samples = 0;
    int lastPort = -1;
    std::vector<std::uint64_t> ticks;
    inp::RumbleState rumble;
    int rumbleCalls = 0;

private:
    const char* name_;
    inp::SourceKind kind_;
    int port_;
    bool motors_;
};

class FakeConsumer final : public inp::InputConsumer {
public:
    const char* Name() const noexcept override { return "freecam (test)"; }
    void Consume(int port, std::uint64_t tick, const inp::PadState& state) noexcept override {
        lastPort = port;
        lastTick = tick;
        last = state;
        ++count;
    }
    int lastPort = -1;
    std::uint64_t lastTick = 0;
    inp::PadState last;
    int count = 0;
};

bool g_focus = true;
bool TestFocus() noexcept {
    return g_focus;
}

inp::HostRouter* g_router = nullptr;
inp::InputRouter* TestRouter() noexcept {
    return g_router;
}

inp::PadState Pad(std::uint16_t buttons, std::uint8_t a = 0, std::int16_t lx = 0) {
    inp::PadState s;
    s.buttons = buttons;
    s.analog[inp::kAnalogA] = a;
    s.thumb[inp::kAxisLX] = lx;
    return s;
}

}  // namespace

TEST_CASE("input router: one player - sources of port 1 are merged (buttons OR, analog max, larger deflection)") {
    FakeSource kbm("kbm", inp::SourceKind::Physical, 0);
    FakeSource pad("pad", inp::SourceKind::Physical, 0, true);
    FakeSource script("script", inp::SourceKind::Script, 0);
    inp::HostRouter r;
    r.UseSources({&kbm, &pad, &script});
    kbm.state = Pad(inp::kButtonStart, 100, -20000);
    pad.state = Pad(inp::kButtonDpadUp, 255, 10000);
    pad.state.thumb[inp::kAxisRY] = -3000;
    script.state = Pad(inp::kButtonBack, 30, 25000);
    CHECK(r.ConnectedPorts() == 1u);
    inp::PadState out;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.buttons == (inp::kButtonStart | inp::kButtonDpadUp | inp::kButtonBack));
    CHECK(out.analog[inp::kAnalogA] == 255);
    CHECK(out.thumb[inp::kAxisLX] == 25000);
    CHECK(out.thumb[inp::kAxisRY] == -3000);
    REQUIRE(r.SampleGamePort(0, out));
    // Every source is sampled once per port sample, with the port's tick count.
    CHECK(kbm.ticks == std::vector<std::uint64_t>{0, 1});
    CHECK(script.ticks == std::vector<std::uint64_t>{0, 1});
    // Other ports have no device.
    CHECK_FALSE(r.SampleGamePort(1, out));
    CHECK(out.Neutral());
    CHECK(kbm.samples == 2);
    // A source without a contribution (Sample false) counts as neutral.
    kbm.contribute = pad.contribute = script.contribute = false;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.Neutral());
    CHECK(r.Stats(0).samples == 3);
}

TEST_CASE("input router: connected ports, epochs, simulated unplug (removal for ERRCTRLINGAME)") {
    FakeSource kbm("kbm", inp::SourceKind::Physical, 0);
    FakeSource pad2("pad 2", inp::SourceKind::Physical, 2, true);
    pad2.connected = false;
    inp::HostRouter r;
    r.UseSources({&kbm, &pad2});
    CHECK(r.ConnectedPorts() == 0x1u);
    CHECK(r.PortEpoch(0) == 1);
    CHECK(r.PortEpoch(2) == 0);
    pad2.connected = true;
    CHECK(r.ConnectedPorts() == 0x5u);
    CHECK(r.PortEpoch(2) == 1);
    pad2.connected = false;
    CHECK(r.ConnectedPorts() == 0x1u);
    pad2.connected = true;
    CHECK(r.ConnectedPorts() == 0x5u);
    CHECK(r.PortEpoch(2) == 2);
    // Keyboard/mouse keeps port 1 connected: unplugging the only controller there removes nothing.
    FakeSource pad0("pad 0", inp::SourceKind::Physical, 0, true);
    r.UseSources({&kbm, &pad0, &pad2});
    pad0.connected = false;
    CHECK((r.ConnectedPorts() & 1u) == 1u);
    // Simulated unplug of port 1.
    r.SimulateUnplug(0, true);
    CHECK((r.ConnectedPorts() & 1u) == 0u);
    inp::PadState out;
    CHECK_FALSE(r.SampleGamePort(0, out));
    r.SimulateUnplug(0, false);
    CHECK((r.ConnectedPorts() & 1u) == 1u);
    CHECK(r.PortEpoch(0) == 2);
    // Unplug + replug between two checks: the epoch still changes (the XPP HLE reports removal + insertion).
    r.SimulateUnplug(0, true);
    r.SimulateUnplug(0, false);
    CHECK((r.ConnectedPorts() & 1u) == 1u);
    CHECK(r.PortEpoch(0) == 3);
}

TEST_CASE("input router: physical input needs the focus; held controls count again only after a release") {
    FakeSource kbm("kbm", inp::SourceKind::Physical, 0);
    FakeSource script("script", inp::SourceKind::Script, 0);
    inp::HostRouter r;
    r.UseSources({&kbm, &script});
    g_focus = true;
    r.SetFocusQuery(&TestFocus);
    kbm.state = Pad(inp::kButtonStart, 200, 30000);
    script.state = Pad(inp::kButtonBack);
    inp::PadState out;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.buttons == (inp::kButtonStart | inp::kButtonBack));
    g_focus = false;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.buttons == inp::kButtonBack);  // the script does not depend on the focus
    CHECK(out.analog[inp::kAnalogA] == 0);
    g_focus = true;  // keys still held while the focus comes back
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.buttons == inp::kButtonBack);
    CHECK(out.analog[inp::kAnalogA] == 0);
    CHECK(out.thumb[inp::kAxisLX] == 0);
    CHECK(r.Stats(0).suppressedSamples >= 1);
    kbm.state = Pad(0, 200, 30000);  // START released, A and the stick still held
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.analog[inp::kAnalogA] == 0);
    kbm.state = Pad(inp::kButtonStart, 0, 30000);  // A released, START pressed again: counts
    REQUIRE(r.SampleGamePort(0, out));
    CHECK((out.buttons & inp::kButtonStart) != 0);
    CHECK(out.thumb[inp::kAxisLX] == 0);
    kbm.state = Pad(inp::kButtonStart, 200, 30000);  // new presses count
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.analog[inp::kAnalogA] == 200);
    CHECK(out.thumb[inp::kAxisLX] == 0);  // the stick was never released
    kbm.state = Pad(inp::kButtonStart, 200, 1000);
    REQUIRE(r.SampleGamePort(0, out));
    kbm.state = Pad(inp::kButtonStart, 200, 30000);
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.thumb[inp::kAxisLX] == 30000);
    CHECK(r.FocusChanges() == 2);

    // background_input = 1: physical sources keep working without the focus.
    r.Configure({true, 100, true});
    g_focus = false;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK((out.buttons & inp::kButtonStart) != 0);
    g_focus = true;
}

TEST_CASE("input router: a diverted port stays connected and neutral; the consumer gets the input") {
    FakeSource kbm("kbm", inp::SourceKind::Physical, 0);
    FakeSource pad("pad", inp::SourceKind::Physical, 0, true);
    inp::HostRouter r;
    r.UseSources({&kbm, &pad});
    FakeConsumer cam;
    kbm.state = Pad(inp::kButtonDpadUp, 255, 32767);
    r.SetGameRumble(0, {1000, 2000});
    CHECK(pad.rumble == inp::RumbleState{1000, 2000});
    r.Divert(0, &cam);
    CHECK(pad.rumble == inp::RumbleState{});  // muted while diverted
    inp::PadState out;
    REQUIRE(r.SampleGamePort(0, out));  // connected
    CHECK(out.Neutral());
    CHECK(cam.count == 1);
    CHECK(cam.last.buttons == inp::kButtonDpadUp);
    CHECK(cam.last.thumb[inp::kAxisLX] == 32767);
    CHECK(r.ConnectedPorts() == 1u);
    // Back to the game while D-pad up and A are still held: no false press edge.
    r.Divert(0, nullptr);
    CHECK(pad.rumble == inp::RumbleState{1000, 2000});
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.Neutral());
    kbm.state = Pad(0, 255, 32767);  // D-pad released
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.Neutral());
    kbm.state = Pad(inp::kButtonDpadUp, 0, 0);  // pressed again; A and the stick released
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.buttons == inp::kButtonDpadUp);
    kbm.state = Pad(inp::kButtonDpadUp, 255, 32767);
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.analog[inp::kAnalogA] == 255);
    CHECK(out.thumb[inp::kAxisLX] == 32767);
    CHECK(r.Stats(0).divertedSamples == 1);
    // A consumer may end the diversion from inside Consume (recursive lock).
    class SelfEnding final : public inp::InputConsumer {
    public:
        explicit SelfEnding(inp::HostRouter& r) : router(r) {}
        const char* Name() const noexcept override { return "self-ending"; }
        void Consume(int port, std::uint64_t, const inp::PadState&) noexcept override { router.Divert(port, nullptr); }
        inp::HostRouter& router;
    } ending(r);
    r.Divert(0, &ending);
    REQUIRE(r.SampleGamePort(0, out));
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(r.Stats(0).divertedSamples == 2);
}

TEST_CASE("input router: rumble - strength, on/off, focus, only the most recently used controller") {
    FakeSource kbm("kbm", inp::SourceKind::Physical, 0);
    FakeSource pad1("pad 1", inp::SourceKind::Physical, 0, true);
    FakeSource pad2("pad 2", inp::SourceKind::Physical, 0, true);
    FakeSource script("script", inp::SourceKind::Script, 0);
    inp::HostRouter r;
    r.UseSources({&kbm, &pad1, &pad2, &script});
    g_focus = true;
    r.SetFocusQuery(&TestFocus);
    r.Configure({true, 50, false});
    // Before any controller was used every controller of the port vibrates.
    r.SetGameRumble(0, {65535, 32768});
    CHECK(pad1.rumble == inp::RumbleState{32767, 16384});
    CHECK(pad2.rumble == inp::RumbleState{32767, 16384});
    CHECK(script.rumble == inp::RumbleState{65535, 32768});  // without motors: the game's values
    CHECK(kbm.rumble == inp::RumbleState{65535, 32768});
    // pad 2 is used: only it vibrates.
    pad2.state = Pad(inp::kButtonStart);
    inp::PadState out;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(pad1.rumble == inp::RumbleState{});
    CHECK(pad2.rumble == inp::RumbleState{32767, 16384});
    // Unchanged requests are not sent again.
    const int calls = pad2.rumbleCalls;
    r.SetGameRumble(0, {65535, 32768});
    CHECK(pad2.rumbleCalls == calls);
    // Focus lost: motors off; back: on again.
    g_focus = false;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(pad2.rumble == inp::RumbleState{});
    CHECK(script.rumble == inp::RumbleState{65535, 32768});
    g_focus = true;
    r.RefreshFocus();
    CHECK(pad2.rumble == inp::RumbleState{32767, 16384});
    // rumble = 0.
    r.Configure({false, 100, false});
    CHECK(pad2.rumble == inp::RumbleState{});
    r.Configure({true, 100, false});
    CHECK(pad2.rumble == inp::RumbleState{65535, 32768});
    r.StopAllMotors();
    CHECK(pad2.rumble == inp::RumbleState{});
    r.SetGameRumble(0, {0, 0});
    r.SetGameRumble(0, {100, 0});
    CHECK(pad2.rumble == inp::RumbleState{100, 0});
    const inp::HostRouter::PortStats st = r.Stats(0);
    CHECK(st.rumbleRequests == 4);
    CHECK(st.rumbleNonZero == 3);
    CHECK(st.maxRumble == inp::RumbleState{65535, 32768});
}

TEST_CASE("input router: rumble - resting stick noise does not make a pad the most recently used one") {
    // Review M4 finding 2: real pads at rest report small non-zero stick values (pad_deadzone 0 by default).
    FakeSource padA("pad A", inp::SourceKind::Physical, 0, true);
    FakeSource padB("pad B", inp::SourceKind::Physical, 0, true);
    inp::HostRouter r;
    r.UseSources({&padA, &padB});
    g_focus = true;
    r.SetFocusQuery(&TestFocus);
    r.Configure({true, 100, false});
    padA.state = Pad(0, 255, 1200);  // the player (A pressed)
    padB.state = Pad(0, 0, 0);
    padB.state.thumb[inp::kAxisLY] = -900;  // resting noise
    padB.state.thumb[inp::kAxisRX] = 3000;
    padB.state.analog[inp::kAnalogLeftTrigger] = 10;  // below the channel threshold 30
    inp::PadState out;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(r.SampleGamePort(0, out));
    }
    r.SetGameRumble(0, {40000, 40000});
    CHECK(padA.rumble == inp::RumbleState{40000, 40000});
    CHECK(padB.rumble == inp::RumbleState{});
    // pad A now rests too: it stays the target, and no rumble command is repeated per tick.
    padA.state = Pad(0, 0, 800);
    const int callsA = padA.rumbleCalls;
    const int callsB = padB.rumbleCalls;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(r.SampleGamePort(0, out));
    }
    CHECK(padA.rumble == inp::RumbleState{40000, 40000});
    CHECK(padB.rumble == inp::RumbleState{});
    CHECK(padA.rumbleCalls == callsA);
    CHECK(padB.rumbleCalls == callsB);
    // A deliberate stick deflection on pad B (beyond 0.2) moves the target.
    padB.state.thumb[inp::kAxisLY] = -20000;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(padA.rumble == inp::RumbleState{});
    CHECK(padB.rumble == inp::RumbleState{40000, 40000});
}

TEST_CASE("input router: per_port - a controller on port n is sampled and rumbled only there") {
    FakeSource kbm("kbm", inp::SourceKind::Physical, 0);
    FakeSource pad1("pad 1", inp::SourceKind::Physical, 1, true);
    inp::HostRouter r;
    r.UseSources({&kbm, &pad1});
    pad1.state = Pad(inp::kButtonBack);
    CHECK(r.ConnectedPorts() == 0x3u);
    inp::PadState out;
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(out.Neutral());
    CHECK(pad1.samples == 0);
    REQUIRE(r.SampleGamePort(1, out));
    CHECK(out.buttons == inp::kButtonBack);
    CHECK(pad1.lastPort == 1);
    r.SetGameRumble(0, {500, 500});
    CHECK(pad1.rumble == inp::RumbleState{});
    r.SetGameRumble(1, {600, 700});
    CHECK(pad1.rumble == inp::RumbleState{600, 700});
}

// ---- XPP HLE ------------------------------------------------------------------------------------------------------

TEST_CASE("XPP HLE: hotplug tracker - insertions, removals, re-connection between two checks") {
    inp::HotplugTracker t;
    const std::uint32_t e0[4] = {1, 0, 0, 0};
    t.Reset(0x1, e0);
    std::uint32_t ins = 0, rem = 0;
    CHECK_FALSE(t.Changes(0x1, e0, ins, rem));
    const std::uint32_t e1[4] = {1, 0, 1, 0};
    CHECK(t.Changes(0x5, e1, ins, rem));
    CHECK(ins == 0x4);
    CHECK(rem == 0);
    CHECK_FALSE(t.Changes(0x5, e1, ins, rem));
    CHECK(t.Changes(0x1, e1, ins, rem));
    CHECK(ins == 0);
    CHECK(rem == 0x4);
    // Port 1 unplugged and replugged in between: both bits (the game closes and reopens it, m4_input.md 2).
    const std::uint32_t e2[4] = {2, 0, 1, 0};
    CHECK(t.Changes(0x1, e2, ins, rem));
    CHECK(ins == 0x1);
    CHECK(rem == 0x1);
    CHECK(t.ReportedMask() == 0x1);
    // Bits above port 4 are ignored.
    CHECK_FALSE(t.Changes(0x11, e2, ins, rem));
}

TEST_CASE("XPP HLE: devices, handles, states with packet numbers, synchronous rumble status") {
    FakeSource kbm("kbm", inp::SourceKind::Physical, 0);
    FakeSource pad2("pad 2", inp::SourceKind::Physical, 2, true);
    pad2.connected = false;
    inp::HostRouter router;
    router.UseSources({&kbm, &pad2});
    g_router = &router;
    inp::XppDevices xpp(&TestRouter);

    CHECK(xpp.GetDevices() == 0x1u);
    std::uint32_t ins = 0xFF, rem = 0xFF;
    CHECK_FALSE(xpp.GetDeviceChanges(ins, rem));
    CHECK(ins == 0);
    CHECK(rem == 0);

    void* h0 = xpp.Open(0);
    void* h1 = xpp.Open(1);  // a port without device still gets a handle (never NULL, m4_input.md 2 item 4)
    REQUIRE(h0 != nullptr);
    REQUIRE(h1 != nullptr);
    CHECK(h0 != h1);
    CHECK(xpp.Open(4) == nullptr);
    CHECK(xpp.PortOfHandle(h1) == 1);
    CHECK(guest::Addr(h0) < 0x80000000u);

    inp::XInputState st;
    std::memset(&st, 0xCD, sizeof(st));
    kbm.state = Pad(inp::kButtonStart | inp::kButtonRightThumb, 77, -1234);
    kbm.state.analog[inp::kAnalogRightTrigger] = 255;
    kbm.state.thumb[inp::kAxisRY] = 32767;
    REQUIRE(xpp.GetState(h0, &st) == inp::kErrorSuccess);
    CHECK(st.Gamepad.wButtons == (inp::kButtonStart | inp::kButtonRightThumb));
    CHECK(st.Gamepad.bAnalogButtons[inp::kAnalogA] == 77);
    CHECK(st.Gamepad.bAnalogButtons[inp::kAnalogRightTrigger] == 255);
    CHECK(st.Gamepad.sThumbLX == -1234);
    CHECK(st.Gamepad.sThumbRY == 32767);
    const std::uint32_t packet = st.dwPacketNumber;
    REQUIRE(xpp.GetState(h0, &st) == inp::kErrorSuccess);
    CHECK(st.dwPacketNumber == packet);  // unchanged state, same packet number
    kbm.state = Pad(0);
    REQUIRE(xpp.GetState(h0, &st) == inp::kErrorSuccess);
    CHECK(st.dwPacketNumber == packet + 1);
    CHECK(st.Gamepad.wButtons == 0);

    // Port 2 has no device yet: 0x48F and a zeroed state (the game then only resets the controller).
    std::memset(&st, 0xCD, sizeof(st));
    CHECK(xpp.GetState(h1, &st) == inp::kErrorDeviceNotConnected);
    CHECK(st.dwPacketNumber == 0);
    CHECK(st.Gamepad.sThumbRY == 0);
    // Invalid handle / no buffer.
    CHECK(xpp.GetState(nullptr, &st) == inp::kErrorInvalidParameter);
    CHECK(xpp.GetState(h0, nullptr) == inp::kErrorInvalidParameter);

    // Rumble: dwStatus is completed synchronously, never ERROR_IO_PENDING.
    inp::XInputFeedback fb{};
    fb.dwStatus = inp::kErrorIoPending;
    fb.hEvent = 0xDEADBEEF;  // garbage from the game pool: never used
    fb.wLeftMotorSpeed = 40000;
    fb.wRightMotorSpeed = 20000;
    FakeSource pad0("pad 0", inp::SourceKind::Physical, 0, true);
    router.UseSources({&kbm, &pad0, &pad2});
    CHECK(xpp.SetState(h0, &fb) == inp::kErrorSuccess);
    CHECK(fb.dwStatus == inp::kErrorSuccess);
    CHECK(pad0.rumble == inp::RumbleState{40000, 20000});
    fb.dwStatus = inp::kErrorIoPending;
    CHECK(xpp.SetState(h1, &fb) == inp::kErrorDeviceNotConnected);
    CHECK(fb.dwStatus == inp::kErrorDeviceNotConnected);
    fb.dwStatus = inp::kErrorIoPending;
    CHECK(xpp.SetState(nullptr, &fb) == inp::kErrorInvalidParameter);
    CHECK(fb.dwStatus == inp::kErrorInvalidParameter);
    CHECK(xpp.SetState(h0, nullptr) == inp::kErrorInvalidParameter);

    // Hotplug of the controller on port 3 (per_port).
    pad2.connected = true;
    CHECK(xpp.GetDeviceChanges(ins, rem));
    CHECK(ins == 0x4);
    CHECK(rem == 0);
    void* h2 = xpp.Open(2);
    pad2.state = Pad(inp::kButtonBack);
    REQUIRE(xpp.GetState(h2, &st) == inp::kErrorSuccess);
    CHECK(st.Gamepad.wButtons == inp::kButtonBack);
    pad2.connected = false;
    CHECK(xpp.GetDeviceChanges(ins, rem));
    CHECK(ins == 0);
    CHECK(rem == 0x4);
    CHECK(xpp.GetState(h2, &st) == inp::kErrorDeviceNotConnected);  // until the game closes it

    // Close: the port's motors stop; a closed handle is invalid.
    xpp.Close(h0);
    CHECK(pad0.rumble == inp::RumbleState{});
    CHECK_FALSE(xpp.IsOpen(0));
    CHECK(xpp.GetState(h0, &st) == inp::kErrorInvalidParameter);
    // Reopening returns the same handle.
    CHECK(xpp.Open(0) == h0);

    const inp::XppDevices::Stats s = xpp.GetStats();
    CHECK(s.insertions == 1);
    CHECK(s.removals == 1);
    CHECK(s.opens == 4);
    CHECK(s.closes == 1);
    g_router = nullptr;
}

TEST_CASE("XPP HLE: game view of a simulated unplug - removal, then insertion; diverted port keeps working") {
    FakeSource kbm("kbm", inp::SourceKind::Physical, 0);
    inp::HostRouter router;
    router.UseSources({&kbm});
    g_router = &router;
    inp::XppDevices xpp(&TestRouter);
    REQUIRE(xpp.GetDevices() == 0x1u);
    void* h = xpp.Open(0);
    std::uint32_t ins = 0, rem = 0;
    // The game's hotplug check runs every tick: unplugged for one tick -> removal (ctrl+0x138 = 1: in-game pause with
    // ERRCTRLINGAME), replugged -> insertion.
    router.SimulateUnplug(0, true);
    CHECK(xpp.GetDeviceChanges(ins, rem));
    CHECK(rem == 0x1);
    CHECK(ins == 0);
    inp::XInputState st;
    CHECK(xpp.GetState(h, &st) == inp::kErrorDeviceNotConnected);
    router.SimulateUnplug(0, false);
    CHECK(xpp.GetDeviceChanges(ins, rem));
    CHECK(ins == 0x1);
    CHECK(rem == 0);
    // Unplug + replug within one tick: both bits in one call (close + reopen, no pause).
    router.SimulateUnplug(0, true);
    router.SimulateUnplug(0, false);
    CHECK(xpp.GetDeviceChanges(ins, rem));
    CHECK(ins == 0x1);
    CHECK(rem == 0x1);
    // Diverted (freecam): success + neutral state, no removal.
    FakeConsumer cam;
    router.Divert(0, &cam);
    kbm.state = Pad(inp::kButtonStart);
    CHECK(xpp.GetState(h, &st) == inp::kErrorSuccess);
    CHECK(st.Gamepad.wButtons == 0);
    CHECK_FALSE(xpp.GetDeviceChanges(ins, rem));
    CHECK(cam.last.buttons == inp::kButtonStart);
    router.Divert(0, nullptr);
    // Without a router (none installed): no devices, not connected.
    g_router = nullptr;
    CHECK(xpp.GetDevices() == 0);
    CHECK(xpp.GetState(h, &st) == inp::kErrorDeviceNotConnected);
}
