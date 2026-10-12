// M3b display-coupled pacing (platform/pacer_policy.*, platform/frame_pacer.*, platform/pacer_display.*,
// gfx/gfx_config.* pacing keys): refresh classes, plan selection and hysteresis, the timer arithmetic, the
// display-limit and VRR detectors, the on-screen statistics, the waitable-slot protocol with a fake swap chain,
// the headless timer path and the configuration. Headless: no window, no D3D device (pacing60.md 6 T1-T5).
#include "doctest.h"

#include "core/config.h"
#include "gfx/gfx_config.h"
#include "platform/frame_pacer.h"
#include "platform/pacer_display.h"
#include "platform/pacer_policy.h"
#include "platform/window.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pc = platform::pacing;

namespace {

pc::PlanInput Input(double refreshHz, bool tearing = true) {
    pc::PlanInput in;
    in.mode = pc::Mode::Auto;
    in.vsync = true;
    in.targetHz = 60.0;
    in.refreshHz = refreshHz;
    in.tearingSupported = tearing;
    in.waitable = true;
    return in;
}

std::filesystem::path TestIni(const char* name, const std::string& text) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("fut_pacer_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream(file, std::ios::binary) << text;
    return file;
}

bool LoadIni(const std::filesystem::path& ini, core::Config& cfg, std::string& error, const std::wstring& extra = L"") {
    const std::wstring cmd = L"futurama.exe \"--config=" + ini.wstring() + L"\"" + extra;
    return core::LoadConfig(cmd.c_str(), L"C:\\nowhere\\futurama.exe", cfg, error);
}

}  // namespace

// ---- T1: refresh rates as QueryDisplayConfig reports them -----------------------------------------------------

TEST_CASE("pacer: refresh classes relative to the target rate (T1)") {
    struct Case {
        double refresh;
        double target;
        bool integer;
        std::uint32_t k;
    };
    const Case cases[] = {
        {120.0, 60.0, true, 2},
        {60000.0 / 1001.0, 60.0, true, 1},  // 59.94: within 0.2 %
        {119880.0 / 1000.0, 60.0, true, 2}, // 119.88
        {60.0, 60.0, true, 1},
        {180.0, 60.0, true, 3},
        {240.0, 60.0, true, 4},
        {239.76, 60.0, true, 4},
        {360.0, 60.0, true, 6},
        {143856.0 / 1000.0, 60.0, false, 2},  // 143.856
        {144.0, 60.0, false, 2},
        {165.0, 60.0, false, 3},
        {75.0, 60.0, false, 1},
        {100.0, 60.0, false, 2},
        {90.0, 60.0, false, 2},  // 1.5 rounds to 2: 45 Hz per frame is not 60
        {50.0, 60.0, false, 1},
        {100.0, 50.0, true, 2},  // PAL tick on a 100 Hz display
        {120.0, 50.0, false, 2},
        {59.0, 60.0, false, 1},  // integer GDI value for 59.94: 1.7 % off
    };
    for (const Case& c : cases) {
        CAPTURE(c.refresh);
        CAPTURE(c.target);
        const pc::RefreshClass r = pc::ClassifyRefresh(c.refresh, c.target);
        CHECK(r.known);
        CHECK(r.integer == c.integer);
        CHECK(r.k == c.k);
    }
    CHECK_FALSE(pc::ClassifyRefresh(0.0, 60.0).known);
    CHECK_FALSE(pc::ClassifyRefresh(120.0, 0.0).known);
    const pc::RefreshClass low = pc::ClassifyRefresh(24.0, 60.0);
    CHECK(low.known);
    CHECK_FALSE(low.integer);
    CHECK(low.k == 0);
}

// ---- T4: plan selection ---------------------------------------------------------------------------------------

TEST_CASE("pacer: plan selection (T4)") {
    SUBCASE("integer multiple -> vblank, Present(k)") {
        for (const double hz : {60.0, 120.0, 180.0, 240.0, 60000.0 / 1001.0, 119.88}) {
            CAPTURE(hz);
            const pc::Plan p = pc::SelectPlan(Input(hz));
            CHECK(p.path == pc::Path::VblankLocked);
            CHECK(p.syncInterval == static_cast<std::uint32_t>(std::lround(hz / 60.0)));
            CHECK(p.k == p.syncInterval);
            CHECK_FALSE(p.allowTearing);
            CHECK(p.presentToSwapChain);
            CHECK(p.periodSeconds == doctest::Approx(1.0 / 60.0));
        }
    }
    SUBCASE("not a multiple, tearing supported -> tearing timer at the target rate") {
        for (const double hz : {75.0, 90.0, 100.0, 144.0, 165.0, 50.0, 360.0}) {
            CAPTURE(hz);
            const pc::Plan p = pc::SelectPlan(Input(hz, true));
            CHECK(p.path == pc::Path::TearingTimer);
            CHECK(p.syncInterval == 0);
            CHECK(p.allowTearing);
            CHECK(p.periodSeconds == doctest::Approx(1.0 / 60.0));
        }
        pc::PlanInput unknown = Input(0.0, true);
        CHECK(pc::SelectPlan(unknown).path == pc::Path::TearingTimer);
    }
    SUBCASE("not a multiple without tearing, or VRR rejected -> one frame per refresh") {
        pc::Plan p = pc::SelectPlan(Input(144.0, false));
        CHECK(p.path == pc::Path::DisplayRate);
        CHECK(p.syncInterval == 1);
        CHECK_FALSE(p.allowTearing);
        CHECK(p.periodSeconds == doctest::Approx(1.0 / 144.0));
        pc::PlanInput rejected = Input(165.0, true);
        rejected.vrr = pc::VrrState::Rejected;
        p = pc::SelectPlan(rejected);
        CHECK(p.path == pc::Path::DisplayRate);
        CHECK(p.periodSeconds == doctest::Approx(1.0 / 165.0));
        pc::PlanInput confirmed = Input(165.0, true);
        confirmed.vrr = pc::VrrState::Confirmed;
        CHECK(pc::SelectPlan(confirmed).path == pc::Path::TearingTimer);
        // 50 Hz display, no tearing: 50 frames per second is all it can show.
        p = pc::SelectPlan(Input(50.0, false));
        CHECK(p.path == pc::Path::DisplayRate);
        CHECK(p.periodSeconds == doctest::Approx(0.02));
        // Unknown refresh without tearing: Present(1), period of the target.
        p = pc::SelectPlan(Input(0.0, false));
        CHECK(p.path == pc::Path::DisplayRate);
        CHECK(p.periodSeconds == doctest::Approx(1.0 / 60.0));
    }
    SUBCASE("background, minimized, display off, locked, display-limited -> free (auto)") {
        pc::PlanInput in = Input(120.0);
        in.env.foreground = false;
        pc::Plan p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Free);
        CHECK(p.presentToSwapChain);
        CHECK(p.syncInterval == 0);
        CHECK_FALSE(p.allowTearing);
        CHECK(p.periodSeconds == doctest::Approx(1.0 / 60.0));
        in = Input(120.0);
        in.env.displayLimited = true;
        CHECK(pc::SelectPlan(in).path == pc::Path::Free);
        for (int which = 0; which < 3; ++which) {
            in = Input(120.0);
            in.env.minimized = which == 0;
            in.env.displayOn = which != 1;
            in.env.sessionLocked = which == 2;
            p = pc::SelectPlan(in);
            CHECK(p.path == pc::Path::Free);
            CHECK_FALSE(p.presentToSwapChain);  // nothing to show: only rotate the back buffers
        }
    }
    SUBCASE("[render] pacing = vsync: always vblank-coupled while something is shown") {
        pc::PlanInput in = Input(120.0);
        in.mode = pc::Mode::Vsync;
        CHECK(pc::SelectPlan(in).path == pc::Path::VblankLocked);
        in.refreshHz = 144.0;  // tearing would be possible, vsync wins
        pc::Plan p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::DisplayRate);
        CHECK(p.syncInterval == 1);
        in.env.foreground = false;  // the background does not matter
        CHECK(pc::SelectPlan(in).path == pc::Path::DisplayRate);
        in.env.displayLimited = true;  // the detector only acts in auto
        CHECK(pc::SelectPlan(in).path == pc::Path::DisplayRate);
        in.env.minimized = true;  // no vblank to couple to
        CHECK(pc::SelectPlan(in).path == pc::Path::Free);
    }
    SUBCASE("[render] pacing = timer / off, [render] vsync = 0") {
        pc::PlanInput in = Input(120.0);
        in.mode = pc::Mode::Timer;
        pc::Plan p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Timer);
        CHECK(p.syncInterval == 0);
        CHECK_FALSE(p.allowTearing);  // vsync = 1: no tearing
        in.vsync = false;
        CHECK(pc::SelectPlan(in).allowTearing);
        in.env.minimized = true;
        p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Timer);
        CHECK_FALSE(p.presentToSwapChain);
        in = Input(120.0);
        in.mode = pc::Mode::Off;
        p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Unthrottled);
        CHECK(p.syncInterval == 0);
        in = Input(120.0);
        in.vsync = false;  // auto + vsync 0 = timer with tearing (the M3 meaning of vsync = 0)
        p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Timer);
        CHECK(p.allowTearing);
        in.tearingSupported = false;
        CHECK_FALSE(pc::SelectPlan(in).allowTearing);
    }
    SUBCASE("no swap chain -> headless timer (off: unthrottled)") {
        pc::PlanInput in = Input(120.0);
        in.env.swapChain = false;
        pc::Plan p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Headless);
        CHECK_FALSE(p.presentToSwapChain);
        CHECK(p.periodSeconds == doctest::Approx(1.0 / 60.0));
        in.mode = pc::Mode::Off;
        CHECK(pc::SelectPlan(in).path == pc::Path::Unthrottled);
        in.mode = pc::Mode::Auto;
        in.targetHz = 50.0;
        CHECK(pc::SelectPlan(in).periodSeconds == doctest::Approx(0.02));
    }
    SUBCASE("PAL tick (target 50)") {
        pc::PlanInput in = Input(100.0);
        in.targetHz = 50.0;
        pc::Plan p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::VblankLocked);
        CHECK(p.k == 2);
        in.refreshHz = 120.0;
        CHECK(pc::SelectPlan(in).path == pc::Path::TearingTimer);
        in.tearingSupported = false;
        p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::DisplayRate);
        CHECK(p.periodSeconds == doctest::Approx(1.0 / 120.0));
    }
}

TEST_CASE("pacer: hysteresis - into free at once, out of free after 30 frames") {
    pc::PlanHysteresis h(30);
    pc::Plan none;
    pc::Plan vblank = pc::SelectPlan(Input(120.0));
    pc::PlanInput bg = Input(120.0);
    bg.env.foreground = false;
    pc::Plan free = pc::SelectPlan(bg);
    CHECK(h.Accept(none, vblank, false));         // first plan
    CHECK_FALSE(h.Accept(vblank, vblank, false)); // same plan: nothing to do
    CHECK(h.Accept(vblank, free, false));         // background: at once
    for (int i = 1; i < 30; ++i) {
        CHECK_FALSE(h.Accept(free, vblank, false));
    }
    CHECK(h.Accept(free, vblank, false));  // 30th frame
    // An interruption restarts the count.
    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(h.Accept(free, vblank, false));
    }
    CHECK_FALSE(h.Accept(free, free, false));
    for (int i = 1; i < 30; ++i) {
        CHECK_FALSE(h.Accept(free, vblank, false));
    }
    CHECK(h.Accept(free, vblank, false));
    // A forced replan (display change, configuration) is immediate; free -> hidden free is immediate too.
    CHECK(h.Accept(free, vblank, true));
    pc::PlanInput min = bg;
    min.env.minimized = true;
    const pc::Plan hidden = pc::SelectPlan(min);
    CHECK(hidden.path == pc::Path::Free);
    CHECK_FALSE(hidden.presentToSwapChain);
    CHECK(h.Accept(free, hidden, false));
    // Restored from minimized (review M3b finding 5): presenting resumes at once - into vblank as well as into
    // a free plan that presents (restored behind another window).
    CHECK(h.Accept(hidden, vblank, false));
    CHECK(h.Accept(hidden, free, false));
    // ... while a visible free plan still waits 30 frames for vblank.
    CHECK_FALSE(h.Accept(free, vblank, false));
    // Timer mode minimized -> restored: the timer plan that presents again is taken at once as well.
    pc::PlanInput timerIn = Input(120.0);
    timerIn.mode = pc::Mode::Timer;
    const pc::Plan timerShown = pc::SelectPlan(timerIn);
    timerIn.env.minimized = true;
    const pc::Plan timerHidden = pc::SelectPlan(timerIn);
    CHECK(timerHidden.path == pc::Path::Timer);
    CHECK_FALSE(timerHidden.presentToSwapChain);
    CHECK(h.Accept(timerShown, timerHidden, false));
    CHECK(h.Accept(timerHidden, timerShown, false));
    // Between two non-free plans (VRR rejected): immediate.
    pc::Plan tearing = pc::SelectPlan(Input(144.0, true));
    pc::Plan rate = pc::SelectPlan(Input(144.0, false));
    CHECK(h.Accept(tearing, rate, false));
}

// ---- timer arithmetic ------------------------------------------------------------------------------------------

TEST_CASE("pacer: the frame schedule holds 60.000 Hz at QPF 10 MHz (fractional period)") {
    pc::FrameSchedule s;
    const double period = 1.0e7 / 60.0;  // 166 666.67 ticks
    std::int64_t now = 1'000'000;
    pc::FrameSchedule::Step step = s.Next(now, period);
    CHECK(step.started);
    CHECK(step.waitUntil == now);
    const std::int64_t anchor = now;
    for (int i = 1; i <= 600; ++i) {
        now += 1000;  // short frames: always wait
        step = s.Next(now, period);
        CHECK_FALSE(step.reset);
        CHECK_FALSE(step.late);
        CHECK(step.waitUntil == anchor + std::llround(i * period));
        now = step.waitUntil;
    }
    CHECK(now - anchor == 100'000'000);  // 600 frames = exactly 10 s (the M2 integer period lost 4 ticks/s)
    CHECK(s.Resets() == 0);
    // Late by less than one period: no wait, the grid is kept.
    now += static_cast<std::int64_t>(period) + 50'000;
    step = s.Next(now, period);
    CHECK(step.late);
    CHECK_FALSE(step.reset);
    CHECK(step.waitUntil == anchor + std::llround(601 * period));
    step = s.Next(now + 10, period);
    CHECK(step.waitUntil == anchor + std::llround(602 * period));
    // More than one period late: a new schedule from now (counted).
    now = anchor + std::llround(602 * period) + 400'000;
    step = s.Next(now, period);
    CHECK(step.reset);
    CHECK(step.waitUntil == now);
    CHECK(s.Resets() == 1);
    step = s.Next(now + 5, period);
    CHECK(step.waitUntil == now + std::llround(period));
    // A new period restarts without counting a reset.
    step = s.Next(now + 100, 200'000.0);
    CHECK(step.started);
    CHECK(s.Resets() == 1);
    s.Reset();
    CHECK(s.Next(5, period).started);
    // Restart on an earlier frame start (vblank plan -> timer plan): the first timer frame keeps the period.
    s.Restart(1'000'000, period);
    step = s.Next(1'000'000 + 20'000, period);
    CHECK_FALSE(step.started);
    CHECK_FALSE(step.reset);
    CHECK(step.waitUntil == 1'000'000 + std::llround(period));
    s.Restart(1'000'000, period);
    step = s.Next(1'000'000 + 3 * std::llround(period), period);  // a long frame: new schedule as usual
    CHECK(step.reset);
}

TEST_CASE("pacer: the frame schedule stays exact over a long run (re-anchoring)") {
    pc::FrameSchedule s;
    const double period = 1.0e7 / 60.0;
    std::int64_t now = 0;
    s.Next(now, period);
    const int frames = 60 * 60 * 25;  // 25 min: crosses the re-anchoring interval (2^20 frames)
    for (int i = 1; i <= frames; ++i) {
        now = s.Next(now + 10, period).waitUntil;
    }
    CHECK(std::llabs(now - std::llround(frames * period)) <= 1);
    CHECK(s.Resets() == 0);
}

TEST_CASE("pacer: shifting the frame schedule moves every later deadline") {
    pc::FrameSchedule s;
    const double period = 1.0e7 / 60.0;
    s.Shift(1000);  // not started: no effect
    pc::FrameSchedule::Step step = s.Next(0, period);
    CHECK(step.started);
    step = s.Next(10, period);
    CHECK(step.waitUntil == std::llround(period));
    s.Shift(41'667);  // half a 120 Hz refresh at QPF 10 MHz
    step = s.Next(step.waitUntil + 10, period);
    CHECK(step.waitUntil == std::llround(2 * period) + 41'667);
    CHECK_FALSE(step.late);
    step = s.Next(step.waitUntil + 10, period);
    CHECK(step.waitUntil == std::llround(3 * period) + 41'667);
    CHECK(s.Resets() == 0);
}

TEST_CASE("pacer: cadence guard of the timer plans (review M3b finding 6)") {
    pc::CadenceGuard g;
    // Nothing expected (vblank plans, tearing, 50 Hz on 120 Hz): never judged.
    for (int i = 0; i < 200; ++i) {
        CHECK_FALSE(g.AddDuration(i % 3, 0));
    }
    CHECK(g.Judged() == 0);
    // Regular 2-refresh cadence with a few real hitches: no shift.
    for (int i = 0; i < 600; ++i) {
        CHECK_FALSE(g.AddDuration(i % 120 == 7 ? 3 : 2, 2));
    }
    CHECK(g.Shifts() == 0);
    // Presents at the latch: 1/3 flips. The sixth irregular duration of a window shifts once ...
    int fired = 0;
    for (int i = 0; i < 6; ++i) {
        fired += g.AddDuration(i % 2 == 0 ? 1 : 3, 2) ? 1 : 0;
    }
    CHECK(fired == 1);
    CHECK(g.Shifts() == 1);
    // ... then the frames still in flight are not judged (hold-off) ...
    for (std::uint32_t i = 0; i < pc::CadenceGuard::kHoldoff; ++i) {
        CHECK_FALSE(g.AddDuration(3, 2));
    }
    // ... and a regular cadence after the shift stays put.
    for (int i = 0; i < 300; ++i) {
        CHECK_FALSE(g.AddDuration(2, 2));
    }
    CHECK(g.Shifts() == 1);
    // Irregular durations spread over several windows (5 per 60) never fire.
    g.Reset();
    for (int i = 0; i < 600; ++i) {
        CHECK_FALSE(g.AddDuration(i % 12 == 0 ? 1 : 2, 2));
    }
    CHECK(g.Shifts() == 1);
}

TEST_CASE("pacer: cadence guard - VRR panel, alternating shifts, stop after kMaxShifts (phase 5)") {
    const double refresh = 1.0 / 120.0;
    SUBCASE("a panel that follows the 60 Hz presents (one refresh of 16.7 ms each) is on time") {
        // Measured with pacing = timer once the window ran in independent flip with VRR: refreshes per frame
        // "1:1773 2:15"; judged by the count alone the guard shifted 50 times in 30 s (59.62 Hz).
        pc::CadenceGuard g;
        for (int i = 0; i < 3600; ++i) {
            const double visible = 1.0 / 60.0 + ((i % 7) - 3) * 0.0004;  // +-1.2 ms present jitter
            CHECK(g.AddDuration(1, 2, visible, refresh) == 0);
        }
        CHECK(g.Shifts() == 0);
        CHECK(g.Followed() == 3600);
        CHECK(g.Judged() == 3600);
    }
    SUBCASE("fixed refresh: 1 and 3 refreshes are off by a whole refresh and still count") {
        pc::CadenceGuard g;
        int fired = 0;
        for (int i = 0; i < 6; ++i) {
            const std::uint32_t r = i % 2 == 0 ? 1u : 3u;
            fired += g.AddDuration(r, 2, static_cast<double>(r) * refresh, refresh) != 0 ? 1 : 0;
        }
        CHECK(fired == 1);
        CHECK(g.Followed() == 0);
    }
    SUBCASE("shifts alternate in sign and stop after kMaxShifts without a clean window") {
        pc::CadenceGuard g;
        std::vector<int> shifts;
        for (int i = 0; i < 2000; ++i) {
            const int s = g.AddDuration(i % 2 == 0 ? 1 : 3, 2);  // never gets better
            if (s != 0) {
                shifts.push_back(s);
            }
        }
        CHECK(shifts == std::vector<int>{1, -1, 1, -1});
        CHECK(g.Shifts() == pc::CadenceGuard::kMaxShifts);
        CHECK(g.Stopped());
        // A plan change starts over.
        g.Reset();
        CHECK_FALSE(g.Stopped());
        int fired = 0;
        for (int i = 0; i < 6; ++i) {
            fired += g.AddDuration(3, 2) != 0 ? 1 : 0;
        }
        CHECK(fired == 1);
        CHECK(g.Shifts() == pc::CadenceGuard::kMaxShifts + 1);
    }
    SUBCASE("a clean window after a shift resets the streak") {
        pc::CadenceGuard g;
        for (int round = 0; round < 10; ++round) {
            int fired = 0;
            for (int i = 0; i < 6; ++i) {
                fired += g.AddDuration(3, 2) != 0 ? 1 : 0;
            }
            CHECK(fired == 1);
            for (std::uint32_t i = 0; i < pc::CadenceGuard::kHoldoff + pc::CadenceGuard::kWindow; ++i) {
                CHECK(g.AddDuration(2, 2) == 0);
            }
        }
        CHECK(g.Shifts() == 10);
        CHECK_FALSE(g.Stopped());
    }
}

TEST_CASE("pacer: relative due time and vblank wait timeout") {
    CHECK(pc::RelativeDueTime100ns(166'667, 10'000'000) == -166'667);
    CHECK(pc::RelativeDueTime100ns(3'000'000'000LL, 3'000'000'000LL) == -10'000'000);  // 1 s at 3 GHz
    CHECK(pc::RelativeDueTime100ns(1, 3'000'000'000LL) == -1);  // never 0 for a positive wait
    CHECK(pc::RelativeDueTime100ns(0, 10'000'000) == 0);
    CHECK(pc::RelativeDueTime100ns(-5, 10'000'000) == 0);
    CHECK(pc::RelativeDueTime100ns(5, 0) == 0);
    CHECK(pc::VblankWaitTimeoutMs(1.0 / 60.0) == 34);
    CHECK(pc::VblankWaitTimeoutMs(1.0 / 144.0) == 14);
    CHECK(pc::VblankWaitTimeoutMs(0.0005) == 4);
    CHECK(pc::VblankWaitTimeoutMs(1.0) == 250);
}

// ---- detectors -----------------------------------------------------------------------------------------------

TEST_CASE("pacer: display-limit detector (3 of 8 slow vblank waits)") {
    pc::DisplayLimitDetector d;
    const double period = 1.0 / 60.0;
    for (int i = 0; i < 100; ++i) {
        CHECK_FALSE(d.AddWait(0.0135, period, false));  // normal: period minus work
    }
    CHECK_FALSE(d.AddWait(0.040, period, false));
    CHECK_FALSE(d.AddWait(0.010, period, false));
    CHECK_FALSE(d.AddWait(0.040, period, false));
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(d.AddWait(0.010, period, false));
    }
    CHECK_FALSE(d.AddWait(0.040, period, false));  // the first slow one fell out of the 8-frame window
    CHECK(d.AddWait(0.0, period, true));            // a timeout counts as slow: 3 of the last 8
    CHECK_FALSE(d.AddWait(0.040, period, false));   // the window starts empty after a detection
    // Hold times double while detections repeat, back to 1 s after a quiet phase.
    CHECK(d.FireHoldSeconds(100.0) == doctest::Approx(1.0));
    CHECK(d.FireHoldSeconds(102.0) == doctest::Approx(2.0));
    CHECK(d.FireHoldSeconds(105.0) == doctest::Approx(4.0));
    CHECK(d.FireHoldSeconds(110.0) == doctest::Approx(8.0));
    CHECK(d.FireHoldSeconds(119.0) == doctest::Approx(8.0));
    CHECK(d.FireHoldSeconds(200.0) == doctest::Approx(1.0));
    // M3d: after entering a vblank plan the first kWarmupWaits waits are not judged (the transient of a composed
    // window at 120 fps: 16, 29, 19 ms waits right after the switch), slow waits after that still are.
    const double period120 = 1.0 / 120.0;
    d.StartWarmup();
    for (std::uint32_t i = 0; i < pc::DisplayLimitDetector::kWarmupWaits; ++i) {
        CHECK_FALSE(d.AddWait(i % 2 == 0 ? 0.029 : 0.0, period120, i % 2 != 0));
    }
    CHECK_FALSE(d.AddWait(0.020, period120, false));
    CHECK_FALSE(d.AddWait(0.020, period120, false));
    CHECK(d.AddWait(0.020, period120, false));
}

TEST_CASE("pacer: VRR detector on synthetic frame statistics (T5)") {
    SUBCASE("VRR panel follows the 60 Hz presents (144 Hz display)") {
        pc::VrrDetector v;
        v.Start(60.0, 144.0);
        CHECK(v.State() == pc::VrrState::Probing);
        for (std::uint32_t i = 0; i < pc::VrrDetector::kSamples; ++i) {
            v.AddFrame();
            v.AddInterval(1.0 / 60.0 + (static_cast<int>(i % 5) - 2) * 0.0002);
        }
        CHECK(v.State() == pc::VrrState::Confirmed);
        CHECK(v.MatchFraction() == doctest::Approx(1.0));
    }
    SUBCASE("fixed 144 Hz refresh, composed: intervals on the 2/3-refresh grid") {
        pc::VrrDetector v;
        v.Start(60.0, 144.0);
        for (std::uint32_t i = 0; i < pc::VrrDetector::kSamples; ++i) {
            v.AddFrame();
            v.AddInterval((i % 5 == 1 || i % 5 == 3 ? 3.0 : 2.0) / 144.0);
        }
        CHECK(v.State() == pc::VrrState::Rejected);
    }
    SUBCASE("VRR floor 40.4 Hz (another application drives the panel)") {
        pc::VrrDetector v;
        v.Start(60.0, 120.0);
        for (std::uint32_t i = 0; i < pc::VrrDetector::kSamples; ++i) {
            v.AddInterval(0.02474);
        }
        CHECK(v.State() == pc::VrrState::Rejected);
    }
    SUBCASE("tearing without VRR: presents share vblanks") {
        pc::VrrDetector v;
        v.Start(60.0, 165.0);
        for (std::uint32_t i = 0; i < pc::VrrDetector::kSamples; ++i) {
            v.AddInterval(i % 4 == 0 ? 0.0 : 1.0 / 60.0);
        }
        CHECK(v.State() == pc::VrrState::Rejected);
        CHECK(std::string(v.Reason()).find("share") != std::string::npos);
    }
    SUBCASE("no frame statistics (occluded, other adapter)") {
        pc::VrrDetector v;
        v.Start(60.0, 144.0);
        for (std::uint32_t i = 0; i + 1 < pc::VrrDetector::kMaxFrames; ++i) {
            v.AddFrame();
        }
        CHECK(v.State() == pc::VrrState::Probing);
        v.AddFrame();
        CHECK(v.State() == pc::VrrState::Rejected);
    }
    SUBCASE("only the probing state takes samples") {
        pc::VrrDetector v;
        v.AddInterval(1.0 / 60.0);
        v.AddFrame();
        CHECK(v.State() == pc::VrrState::Unknown);
        v.Start(60.0, 144.0);
        v.Stop();
        CHECK(v.State() == pc::VrrState::Unknown);
    }
    SUBCASE("warm-up: the change into independent flip is not judged (review M3b finding 13a)") {
        pc::VrrDetector v;
        v.Start(60.0, 144.0, 120);
        CHECK(v.WarmingUp());
        // 2 s of composed presentation on the refresh grid right after the mode change ...
        for (int i = 0; i < 120; ++i) {
            v.AddInterval((i % 5 == 1 || i % 5 == 3 ? 3.0 : 2.0) / 144.0);
            v.AddFrame();
        }
        CHECK_FALSE(v.WarmingUp());
        CHECK(v.State() == pc::VrrState::Probing);
        // ... then the panel follows the presents.
        for (std::uint32_t i = 0; i < pc::VrrDetector::kSamples; ++i) {
            v.AddInterval(1.0 / 60.0);
            v.AddFrame();
        }
        CHECK(v.State() == pc::VrrState::Confirmed);
        // The frame limit for missing statistics counts after the warm-up.
        pc::VrrDetector w;
        w.Start(60.0, 144.0, 120);
        for (std::uint32_t i = 0; i + 1 < 120 + pc::VrrDetector::kMaxFrames; ++i) {
            w.AddFrame();
        }
        CHECK(w.State() == pc::VrrState::Probing);
        w.AddFrame();
        CHECK(w.State() == pc::VrrState::Rejected);
    }
}

// ---- statistics ---------------------------------------------------------------------------------------------

TEST_CASE("pacer: interval histogram relative to the planned period") {
    pc::IntervalHistogram h;
    const double p = 1.0 / 60.0;
    h.Add(0.001, p);       // < 0.5
    h.Add(0.6 * p, p);     // 0.5-0.85
    h.Add(0.9 * p, p);     // 0.85-0.95
    for (int i = 0; i < 96; ++i) {
        h.Add(p, p);       // 0.95-1.05
    }
    h.Add(1.2 * p, p);     // 1.05-1.5
    h.Add(2.0 * p, p);     // 1.5-2.5
    h.Add(3.0 * p, p);     // 2.5-4
    h.Add(10.0 * p, p);    // >= 4
    CHECK(h.Total() == 103);
    const std::uint64_t expected[] = {1, 1, 1, 96, 1, 1, 1, 1};
    for (std::size_t b = 0; b < pc::IntervalHistogram::kBuckets; ++b) {
        CAPTURE(b);
        CHECK(h.Count(b) == expected[b]);
    }
    CHECK(h.OnPeriodPercent() == doctest::Approx(100.0 * 96 / 103));
    h.Reset();
    CHECK(h.Total() == 0);
    CHECK(h.OnPeriodPercent() == 0.0);
}

TEST_CASE("pacer: on-screen tracker - durations in refreshes, latency, measured refresh") {
    pc::OnScreenTracker t;
    t.SetQpcFrequency(1.0e7);
    const std::int64_t refresh = 83'333;  // 120 Hz in 100 ns ticks
    std::int64_t vblank = 10'000'000;
    std::uint32_t refreshCount = 1000;
    // Present(2) at 60 Hz: frame n is presented 3 ms after the vblank and visible 2 refreshes after the previous.
    for (std::uint32_t id = 1; id <= 100; ++id) {
        const std::int64_t call = vblank + 30'000;
        t.NotePresent(id, call, 2);
        vblank += 2 * refresh;
        refreshCount += id == 50 ? 3 : 2;  // one frame stays 3 refreshes (late)
        if (id == 50) {
            vblank += refresh;
        }
        const double interval = t.NoteStatistics(id, refreshCount, vblank);
        if (id == 1) {
            CHECK(interval < 0.0);  // no previous sample
        } else {
            CHECK(interval > 0.0);
        }
        CHECK(t.NoteStatistics(id, refreshCount, vblank) < 0.0);  // same sample again: nothing new
    }
    pc::OnScreenTracker::Result r = t.Get();
    CHECK(r.presents == 100);
    CHECK(r.samples == 100);
    CHECK(r.pairs == 99);
    CHECK(r.durations[2] == 98);
    CHECK(r.durations[3] == 1);
    CHECK(r.planned == 99);
    CHECK(r.onPlan == 98);
    CHECK(r.late == 1);
    CHECK(r.early == 0);
    CHECK(r.measuredHz == doctest::Approx(120.0).epsilon(0.001));
    CHECK(r.latencySamples == 100);
    CHECK(r.latencyMedianMs == doctest::Approx(13.7).epsilon(0.02));  // 2 refreshes - 3 ms
    // A gap (presents 101/102 not sampled one by one) gives no pair but keeps the measurement.
    t.NotePresent(101, vblank + 30'000, 2);
    t.NotePresent(102, vblank + 30'000 + 2 * refresh, 2);
    vblank += 4 * refresh;
    refreshCount += 4;
    CHECK(t.NoteStatistics(102, refreshCount, vblank) < 0.0);
    r = t.Get();
    CHECK(r.pairs == 99);
    CHECK(r.samples == 101);
    // A new swap chain (count goes back) starts over without a bogus pair.
    t.NotePresent(1, vblank + 10, 1);
    CHECK(t.NoteStatistics(1, refreshCount + 1, vblank + refresh) < 0.0);
    t.NoteInvalid();
    CHECK(t.Get().invalid == 1);
    t.Reset();
    CHECK(t.Get().samples == 0);
}

TEST_CASE("pacer: on-screen tracker - jumps, refresh fit and the last pair (review M3b finding 9)") {
    pc::OnScreenTracker t;
    t.SetQpcFrequency(1.0e7);
    t.SetNominalRefreshHz(120.0);
    const double refresh = 1.0e7 / 120.001;  // 83 332.64 ticks
    const auto at = [&](std::uint32_t n) { return 5'000'000 + std::llround(n * refresh); };
    // A 60 Hz timer plan (nominal 2 refreshes) sampled once per frame. Every 10th sample sees two presents at
    // once (d = 2): 2 + 3 refreshes (irregular) or 2 + 2 (regular). Some samples' refresh count is one off its
    // time stamp, as measured on the 120 Hz machine (never a jump sample or the one before it, so the jump
    // counts stay exact).
    std::uint32_t id = 0, n = 1000;
    std::uint64_t jumps = 0, irregular = 0;
    std::uint64_t pairsSeen = 0;
    for (int i = 0; i < 2000; ++i) {
        const bool jump = i % 10 == 9;
        const bool late = jump && i % 20 == 19;
        t.NotePresent(++id, at(n) - 30'000, 0, 2);
        if (jump) {
            n += 2;
            t.NotePresent(++id, at(n) - 30'000, 0, 2);
        }
        n += late ? 3 : 2;
        const bool offByOne = i % 7 == 3 && i % 10 >= 1 && i % 10 <= 7;
        const std::uint32_t count = n + (offByOne ? 1u : 0u);
        t.NoteStatistics(id, count, at(n));
        const pc::OnScreenTracker::LastPair last = t.Last();
        if (i > 0 && !jump) {
            CHECK(last.valid);
            CHECK(last.expected == 2);
            ++pairsSeen;
        } else {
            CHECK_FALSE(last.valid);
        }
        jumps += i > 0 && jump ? 1 : 0;
        irregular += i > 0 && late ? 1 : 0;
    }
    pc::OnScreenTracker::Result r = t.Get();
    CHECK(r.pairs == pairsSeen);
    CHECK(r.jumps == jumps);
    CHECK(r.jumpPresents == 2 * jumps);
    CHECK(r.jumpsIrregular == irregular);
    CHECK(r.planned == 0);  // a timer plan is not vblank-coupled
    CHECK(r.refreshSegments > 1);  // every off-by-one sample ends a segment
    CHECK(r.refreshSamples == 2000);
    CHECK(r.measuredHz == doctest::Approx(120.001).epsilon(0.00002));
    const std::uint64_t segmentsBefore = r.refreshSegments;
    // A frozen counter across a minimized phase (3 s later, counter almost unchanged) and a garbage jump of the
    // counter start new segments instead of bending the fit.
    std::uint32_t c = n + 1;  // the counter did not advance with the time
    n += 360;
    const auto sample = [&](std::uint32_t refreshes, std::uint32_t counterStep) {
        t.NotePresent(++id, at(n + refreshes) - 30'000, 0, 2);
        n += refreshes;
        c += counterStep;
        t.NoteStatistics(id, c, at(n));
    };
    sample(0, 0);  // after the gap: segment 2
    for (int i = 0; i < 20; ++i) {
        sample(2, 2);
    }
    sample(2, 5'906'734);  // garbage jump: segment 3
    for (int i = 0; i < 100; ++i) {
        sample(2, 2);
    }
    r = t.Get();
    CHECK(r.refreshSegments == segmentsBefore + 2);
    CHECK(r.measuredHz == doctest::Approx(120.001).epsilon(0.00002));
    // A counter that lags its time stamp by one refresh for good (measured 4 times in 60 s) ends a segment; the
    // fit keeps the true rate where the counter alone reads 119.8 Hz.
    t.Reset();
    t.SetNominalRefreshHz(120.0);
    n = 0;
    c = 0;
    for (int i = 0; i < 900; ++i) {
        sample(2, i % 225 == 100 ? 1 : 2);  // 4 lost counts in 15 s
    }
    r = t.Get();
    CHECK(r.refreshSegments == 5);
    CHECK(r.measuredHz == doctest::Approx(120.001).epsilon(0.00002));
    // Without a nominal refresh only a gap of more than 1 s breaks a segment.
    t.Reset();
    t.SetNominalRefreshHz(0.0);
    t.NoteStatistics(1, 100, at(100));
    t.NoteStatistics(2, 102, at(102));
    t.NoteStatistics(3, 104, at(104));
    t.NoteStatistics(4, 106, at(106) + 20'000'000);
    t.NoteStatistics(5, 108, at(108) + 20'000'000);
    r = t.Get();
    CHECK(r.refreshSegments == 2);
    CHECK(r.measuredHz == doctest::Approx(120.001).epsilon(0.00002));
}

// ---- names ------------------------------------------------------------------------------------------------------

TEST_CASE("pacer: mode and path names") {
    pc::Mode m = pc::Mode::Off;
    CHECK(pc::ParseMode("auto", &m));
    CHECK(m == pc::Mode::Auto);
    CHECK(pc::ParseMode("VSync", &m));
    CHECK(m == pc::Mode::Vsync);
    CHECK(pc::ParseMode("timer", &m));
    CHECK(m == pc::Mode::Timer);
    CHECK(pc::ParseMode("OFF", &m));
    CHECK(m == pc::Mode::Off);
    CHECK_FALSE(pc::ParseMode("legacy", &m));
    CHECK_FALSE(pc::ParseMode("", &m));
    CHECK_FALSE(pc::ParseMode("automatic", &m));
    CHECK(m == pc::Mode::Off);
    for (const pc::Mode mode : {pc::Mode::Auto, pc::Mode::Vsync, pc::Mode::Timer, pc::Mode::Off}) {
        pc::Mode back = pc::Mode::Auto;
        CHECK(pc::ParseMode(pc::ModeName(mode), &back));
        CHECK(back == mode);
    }
    for (std::size_t p = 0; p < pc::kPathCount; ++p) {
        CHECK(std::string(pc::PathName(static_cast<pc::Path>(p))) != "?");
    }
    CHECK(pc::IsVblankPath(pc::Path::VblankLocked));
    CHECK(pc::IsVblankPath(pc::Path::DisplayRate));
    CHECK_FALSE(pc::IsVblankPath(pc::Path::Free));
    CHECK(pc::IsTimerPath(pc::Path::Free));
    CHECK(pc::IsTimerPath(pc::Path::Headless));
    CHECK(pc::IsTimerPath(pc::Path::TearingTimer));
    CHECK_FALSE(pc::IsTimerPath(pc::Path::Unthrottled));
}

// ---- configuration -------------------------------------------------------------------------------------------

TEST_CASE("pacer: [render] pacing / frame_rate / pacing_trace and the tick rate") {
    core::Config cfg;
    std::string error;
    const auto plain = TestIni("plain.ini", "[debug]\nselftest = 0\n");
    REQUIRE_MESSAGE(LoadIni(plain, cfg, error), error);
    gfx::Settings s = gfx::SettingsFromConfig(cfg);
    CHECK(s.tickRate == 60);
    CHECK(s.frameRate == 60);  // no 50 Hz default any more
    CHECK(s.pacing == pc::Mode::Auto);
    CHECK(s.pacingTrace.empty());
    platform::PacingConfig c = gfx::PacingConfigOf(s);
    CHECK(c.mode == pc::Mode::Auto);
    CHECK(c.targetHz == doctest::Approx(60.0));
    CHECK(c.tickHz == 60);
    CHECK(c.vsync);
    CHECK(c.tracePath == nullptr);

    REQUIRE_MESSAGE(LoadIni(plain, cfg, error, L" --set=game.tick_rate=50"), error);
    s = gfx::SettingsFromConfig(cfg);
    CHECK(s.tickRate == 50);
    CHECK(s.frameRate == 50);  // follows the tick rate
    CHECK(gfx::PacingConfigOf(s).targetHz == doctest::Approx(50.0));

    for (const char* mode : {"auto", "vsync", "timer", "off", "VSYNC"}) {
        CAPTURE(mode);
        const std::wstring arg = L" --set=render.pacing=" + std::wstring(mode, mode + std::strlen(mode));
        REQUIRE_MESSAGE(LoadIni(plain, cfg, error, arg), error);
        s = gfx::SettingsFromConfig(cfg);
        pc::Mode expected = pc::Mode::Auto;
        REQUIRE(pc::ParseMode(mode, &expected));
        CHECK(s.pacing == expected);
    }
    for (const wchar_t* bad : {L" --set=render.pacing=legacy", L" --set=render.pacing=", L" --set=render.pacing=1"}) {
        CAPTURE(core::WideToUtf8(bad));
        CHECK_FALSE(LoadIni(plain, cfg, error, bad));
        CHECK(error.find("pacing") != std::string::npos);
    }
    // --set keys and values are trimmed and unquoted like INI values (review M3b finding 10: " timer" passed the
    // choice check and was then ignored by the reader).
    REQUIRE_MESSAGE(LoadIni(plain, cfg, error, L" \"--set=render.pacing= timer \""), error);
    CHECK(gfx::SettingsFromConfig(cfg).pacing == pc::Mode::Timer);
    REQUIRE_MESSAGE(LoadIni(plain, cfg, error, L" \"--set= render.pacing =vsync\""), error);
    CHECK(gfx::SettingsFromConfig(cfg).pacing == pc::Mode::Vsync);
    REQUIRE_MESSAGE(LoadIni(plain, cfg, error, L" --set=render.pacing=\\\"off\\\""), error);
    CHECK(gfx::SettingsFromConfig(cfg).pacing == pc::Mode::Off);
    CHECK_FALSE(LoadIni(plain, cfg, error, L" \"--set= =timer\""));
    CHECK(error.find("--set expects") != std::string::npos);

    // frame_rate 0 keeps its M2 meaning (unthrottled) as long as pacing is auto.
    REQUIRE_MESSAGE(LoadIni(plain, cfg, error, L" --set=render.frame_rate=0"), error);
    s = gfx::SettingsFromConfig(cfg);
    CHECK(s.frameRate == 0);
    c = gfx::PacingConfigOf(s);
    CHECK(c.mode == pc::Mode::Off);
    CHECK(c.targetHz == doctest::Approx(60.0));
    REQUIRE_MESSAGE(LoadIni(plain, cfg, error, L" --set=render.frame_rate=0 --set=render.pacing=timer"), error);
    CHECK(gfx::PacingConfigOf(gfx::SettingsFromConfig(cfg)).mode == pc::Mode::Timer);
    // An explicit frame rate wins (with a warning when it differs from the tick rate).
    REQUIRE_MESSAGE(LoadIni(plain, cfg, error, L" --set=render.frame_rate=120"), error);
    s = gfx::SettingsFromConfig(cfg);
    CHECK(s.frameRate == 120);
    CHECK(gfx::PacingConfigOf(s).targetHz == doctest::Approx(120.0));

    const auto traced = TestIni("trace.ini", "[render]\npacing = vsync\npacing_trace = pacing.csv\n");
    REQUIRE_MESSAGE(LoadIni(traced, cfg, error), error);
    s = gfx::SettingsFromConfig(cfg);
    CHECK(s.pacing == pc::Mode::Vsync);
    CHECK(s.pacingTrace == traced.parent_path() / "pacing.csv");  // relative to the INI
    c = gfx::PacingConfigOf(s);
    REQUIRE(c.tracePath != nullptr);
    CHECK(std::filesystem::path(c.tracePath) == s.pacingTrace);
}

// ---- display ------------------------------------------------------------------------------------------------------

TEST_CASE("pacer: refresh rate of the primary monitor (QueryDisplayConfig)") {
    const platform::DisplayTiming t = platform::QueryDisplayTiming(nullptr);
    MESSAGE("primary display " << t.device << " \"" << t.name << "\": " << t.numerator << "/" << t.denominator
                               << " Hz = " << t.hz << " Hz (" << std::string(t.source) << ")");
    if (!t.valid) {
        return;  // a machine without an active display (CI service session)
    }
    CHECK(t.hz > 20.0);
    CHECK(t.hz < 1000.0);
    CHECK(t.denominator != 0);
    CHECK(t.hz == doctest::Approx(static_cast<double>(t.numerator) / t.denominator));
    CHECK(std::string(t.source) != "none");
    if (std::string(t.source) == "gdi" && std::string(t.device).rfind("\\\\.\\DISPLAY", 0) != 0) {
        // No active display output (TV off, remote session): GDI reports Windows' placeholder "WinDisc" at 60 Hz.
        MESSAGE("no active display output - GDI placeholder " << t.device);
        return;
    }
    CHECK(std::string(t.device).rfind("\\\\.\\DISPLAY", 0) == 0);
}

// ---- runtime pacer ------------------------------------------------------------------------------------------------

TEST_CASE("pacer: headless EndFrame paces at the target rate (null backend)") {
    platform::ResetFramePacer();
    platform::PacingConfig c;
    c.targetHz = 60.0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 31; ++i) {  // the first frame starts the schedule, 30 periods follow
        platform::EndFrame(c);
        platform::NotePresentedFrame();
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed > 0.45);
    CHECK(elapsed < 1.5);
    const platform::PresentStatistics s = platform::GetPresentStatistics();
    CHECK(s.frames == 31);
    CHECK(s.pacingHz == 60);
    CHECK(s.plan.path == pc::Path::Headless);
    CHECK(s.pathFrames[static_cast<std::size_t>(pc::Path::Headless)] == 31);
    CHECK(s.averageHz > 50.0);
    CHECK(s.averageHz < 65.0);
    CHECK(s.swapChainPresents == 0);
    std::uint64_t intervals = 0;
    for (const std::uint64_t b : s.intervalBuckets) {
        intervals += b;
    }
    CHECK(intervals == 30);
    platform::LogPresentStatistics();
    // off: no wait at all
    c.mode = pc::Mode::Off;
    const auto offStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        platform::EndFrame(c);
    }
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - offStart).count() < 0.3);
    CHECK(platform::GetPresentStatistics().plan.path == pc::Path::Unthrottled);
    platform::ResetFramePacer();
    CHECK(platform::GetPresentStatistics().frames == 0);
}

// The protocol of the frame-latency waitable object, with a semaphore standing in for DXGI's (the "display"
// releases it when the frame is shown): one signal is taken per swap-chain Present, by EndFrame's wait or by
// PlanPresent's zero-timeout poll; without a signal the Present is skipped (never blocks), and after 30
// starved frames one goes out anyway.
TEST_CASE("pacer: frame-latency waitable slots with a fake swap chain") {
    platform::ResetFramePacer();
    const HANDLE latency = CreateSemaphoreW(nullptr, 1, 1, nullptr);
    REQUIRE(latency != nullptr);
    platform::PacingConfig c;
    c.mode = pc::Mode::Vsync;  // vblank-coupled whatever the primary monitor's refresh is
    platform::SwapChainInfo chain;
    chain.frameLatencyWaitable = latency;
    std::uint32_t id = 0;
    for (int frame = 0; frame < 20; ++frame) {
        const platform::PresentRequest r = platform::PlanPresent(c, chain);
        CHECK_FALSE(r.skipSwapChain);
        CHECK(r.syncInterval >= 1);
        CHECK_FALSE(r.allowTearing);
        platform::PresentFeedback fb;
        fb.swapChainPresented = true;
        fb.presentId = ++id;
        platform::NoteSwapChainPresent(fb);
        ReleaseSemaphore(latency, 1, nullptr);  // the frame is on screen: the slot comes back
        platform::EndFrame(c);                  // takes it
    }
    platform::PresentStatistics s = platform::GetPresentStatistics();
    CHECK(pc::IsVblankPath(s.plan.path));
    CHECK(s.frameLatencyWaitable);
    CHECK(s.swapChainPresents == 20);
    CHECK(s.skippedNoSlot == 0);
    CHECK(s.waitTimeouts == 0);
    // The display stops releasing slots: the slot taken last is used by the first Present, then EndFrame
    // times out (2 periods) and PlanPresent skips; the 30th starved frame presents anyway.
    std::uint32_t skipped = 0, presented = 0;
    for (int frame = 0; frame < 31; ++frame) {
        const platform::PresentRequest r = platform::PlanPresent(c, chain);
        platform::PresentFeedback fb;
        fb.swapChainPresented = !r.skipSwapChain;
        fb.presentId = r.skipSwapChain ? id : ++id;
        platform::NoteSwapChainPresent(fb);
        skipped += r.skipSwapChain ? 1 : 0;
        presented += r.skipSwapChain ? 0 : 1;
        platform::EndFrame(c);
    }
    s = platform::GetPresentStatistics();
    CHECK(presented == 2);
    CHECK(skipped == 29);
    CHECK(s.forcedPresents == 1);
    CHECK(s.skippedNoSlot == 29);
    CHECK(s.waitTimeouts == 31);
    CHECK(s.scheduleResets >= 31);
    platform::LogPresentStatistics();
    platform::ResetFramePacer();
    CloseHandle(latency);
}

TEST_CASE("pacer: display epoch from the window module") {
    const platform::DisplayEnvironment before = platform::GetDisplayEnvironment();
    platform::NoteDisplayChange();
    const platform::DisplayEnvironment after = platform::GetDisplayEnvironment();
    CHECK(after.epoch == before.epoch + 1);
    CHECK(after.displayOn);
    CHECK_FALSE(after.sessionLocked);
}
