// M3d frame rates up to 120 Hz (platform/pacer_policy.h FrameRateMode, ChooseFrameRate, the Cadence plan,
// FrameAdvanceOf, LatencyBudget, JustInTimeStart, OnScreenTracker prediction; gfx/gfx_config.h ParseFrameRate and
// the [render] frame_rate / low_latency keys; platform/window.h ViewLayout), analysis/loader/m3d_frameclock.md 3.
// Headless: no window, no D3D device.
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "gfx/gfx_config.h"
#include "platform/frame_pacer.h"
#include "platform/pacer_policy.h"
#include "platform/window.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pc = platform::pacing;

namespace {

pc::PlanInput M3dInput(pc::FrameRateMode mode, double refreshHz, bool tearing) {
    pc::PlanInput in;
    in.mode = pc::Mode::Auto;
    in.vsync = true;
    in.targetHz = pc::kMaxFrameRate;
    in.refreshHz = refreshHz;
    in.tearingSupported = tearing;
    in.waitable = true;
    in.rateMode = mode;
    return in;
}

std::filesystem::path M3dIni(const char* name, const std::string& text) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("fut_pacer_m3d_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream(file, std::ios::binary) << text;
    return file;
}

bool LoadM3dIni(const std::filesystem::path& ini, core::Config& cfg, std::string& error, const std::wstring& extra = L"") {
    const std::wstring cmd = L"futurama.exe \"--config=" + ini.wstring() + L"\"" + extra;
    return core::LoadConfig(cmd.c_str(), L"C:\\nowhere\\futurama.exe", cfg, error);
}

}  // namespace

// ---- frame rate choice ---------------------------------------------------------------------------------------------

TEST_CASE("pacer m3d: the frame rate a refresh allows (120 = one frame per refresh up to 120, display = even rate)") {
    struct Row {
        double refresh;
        double max120Hz;
        std::uint32_t max120K;
        double displayHz;
        std::uint32_t displayK;
    };
    const Row rows[] = {
        {60.0, 60.0, 1, 60.0, 1},             // a 60 Hz display stays at 60 (no interpolation)
        {59.94, 59.94, 1, 59.94, 1},
        {75.0, 75.0, 1, 75.0, 1},
        {100.0, 100.0, 1, 100.0, 1},
        {119.88, 119.88, 1, 119.88, 1},
        {120.0, 120.0, 1, 120.0, 1},          // every vblank
        {120.2, 120.2, 1, 120.2, 1},          // within 0.2 %: still one frame per refresh
        {144.0, 120.0, 0, 72.0, 2},           // 120: cadence or VRR timer; display: 72 evenly
        {165.0, 120.0, 0, 82.5, 2},
        {180.0, 120.0, 0, 90.0, 2},
        {240.0, 120.0, 2, 120.0, 2},          // every second vblank
        {360.0, 120.0, 3, 120.0, 3},
        {480.0, 120.0, 4, 120.0, 4},
        {500.0, 120.0, 0, 120.0, 0},          // above 4 refreshes per frame: no even divisor
        {0.0, 120.0, 0, 120.0, 0},            // unknown refresh: the cap
    };
    for (const Row& r : rows) {
        CAPTURE(r.refresh);
        const pc::RateChoice a = pc::ChooseFrameRate(pc::FrameRateMode::Max120, r.refresh, pc::kMaxFrameRate);
        CHECK(a.frameHz == doctest::Approx(r.max120Hz));
        CHECK(a.k == r.max120K);
        const pc::RateChoice d = pc::ChooseFrameRate(pc::FrameRateMode::Display, r.refresh, pc::kMaxFrameRate);
        CHECK(d.frameHz == doctest::Approx(r.displayHz));
        CHECK(d.k == r.displayK);
        CHECK(a.frameHz <= pc::kMaxFrameRate * 1.002);
        CHECK(d.frameHz <= pc::kMaxFrameRate * 1.002);
    }
    CHECK(std::string(pc::FrameRateModeName(pc::FrameRateMode::Fixed)) == "fixed");
    CHECK(std::string(pc::FrameRateModeName(pc::FrameRateMode::Max120)) == "120");
    CHECK(std::string(pc::FrameRateModeName(pc::FrameRateMode::Display)) == "display");
    CHECK(std::string(pc::PathName(pc::Path::Cadence)) == "cadence");
    CHECK(pc::IsVblankPath(pc::Path::Cadence));
    CHECK_FALSE(pc::IsTimerPath(pc::Path::Cadence));
}

TEST_CASE("pacer m3d: cadence pattern - 1,1,2,1,1 on 144 Hz, 2,1 on 180 Hz, exact on average") {
    pc::CadencePattern p;
    p.Reset(144.0 / 120.0);
    std::vector<std::uint32_t> first;
    for (int i = 0; i < 10; ++i) {
        first.push_back(p.Next());
    }
    // s_n = round((n + 1) * 1.2) - round(n * 1.2): 1, 1, 2, 1, 1 (round half to even in floor(x + 0.5) form).
    std::uint32_t sum = 0;
    for (const std::uint32_t s : first) {
        CHECK((s == 1u || s == 2u));
        sum += s;
    }
    CHECK(sum == 12u);  // 10 frames = 12 refreshes = 1/12 s at 144 Hz: 120 frames per second
    p.Reset(180.0 / 120.0);
    std::uint32_t a = p.Next(), b = p.Next();
    CHECK(a + b == 3u);
    CHECK(a != b);
    // A long run stays exact (no drift of the accumulator).
    p.Reset(165.0 / 120.0);
    std::uint64_t total = 0;
    const std::uint64_t frames = 1'200'000;
    for (std::uint64_t i = 0; i < frames; ++i) {
        const std::uint32_t s = p.Next();
        REQUIRE((s == 1u || s == 2u));
        total += s;
    }
    CHECK(static_cast<double>(total) == doctest::Approx(static_cast<double>(frames) * 165.0 / 120.0).epsilon(1e-9));
    // Invalid ratios: one refresh per frame.
    p.Reset(0.5);
    CHECK(p.Next() == 1u);
    CHECK(p.Ratio() == 0.0);
}

TEST_CASE("pacer m3d: plan selection for [render] frame_rate = 120 | display (m3d_frameclock.md 3.2)") {
    using M = pc::FrameRateMode;
    SUBCASE("120 Hz: vblank k = 1 (every refresh) in both modes, VRR or not") {
        for (const M mode : {M::Max120, M::Display}) {
            for (const bool tearing : {false, true}) {
                const pc::Plan p = pc::SelectPlan(M3dInput(mode, 120.0, tearing));
                CHECK(p.path == pc::Path::VblankLocked);
                CHECK(p.syncInterval == 1u);
                CHECK(p.k == 1u);
                CHECK(p.nominalHz == doctest::Approx(120.0));
                CHECK(p.periodSeconds == doctest::Approx(1.0 / 120.0));
                CHECK_FALSE(p.allowTearing);
            }
        }
    }
    SUBCASE("60 Hz: one frame per refresh at 60 (the clock stays at one tick per frame)") {
        const pc::Plan p = pc::SelectPlan(M3dInput(M::Max120, 60.0, true));
        CHECK(p.path == pc::Path::VblankLocked);
        CHECK(p.k == 1u);
        CHECK(p.nominalHz == doctest::Approx(60.0));
        CHECK(pc::FrameAdvanceOf(p, 60.0, 60.0, 0).framesPerTick == 1u);
    }
    SUBCASE("240 Hz: vblank k = 2") {
        const pc::Plan p = pc::SelectPlan(M3dInput(M::Display, 240.0, false));
        CHECK(p.path == pc::Path::VblankLocked);
        CHECK(p.k == 2u);
        CHECK(p.nominalHz == doctest::Approx(120.0));
    }
    SUBCASE("144/165 Hz with tearing: the VRR timer at 120") {
        for (const M mode : {M::Max120, M::Display}) {
            for (const double hz : {144.0, 165.0}) {
                const pc::Plan p = pc::SelectPlan(M3dInput(mode, hz, true));
                CHECK(p.path == pc::Path::TearingTimer);
                CHECK(p.allowTearing);
                CHECK(p.nominalHz == doctest::Approx(120.0));
                CHECK(p.periodSeconds == doctest::Approx(1.0 / 120.0));
            }
        }
    }
    SUBCASE("144 Hz without VRR: 120 = cadence 1/2 refreshes, display = 72 evenly") {
        pc::PlanInput in = M3dInput(M::Max120, 144.0, false);
        pc::Plan p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Cadence);
        CHECK(p.cadenceRatio == doctest::Approx(1.2));
        CHECK(p.nominalHz == doctest::Approx(120.0));
        in.tearingSupported = true;
        in.vrr = pc::VrrState::Rejected;
        CHECK(pc::SelectPlan(in).path == pc::Path::Cadence);
        in.rateMode = M::Display;
        p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::VblankLocked);
        CHECK(p.k == 2u);
        CHECK(p.nominalHz == doctest::Approx(72.0));
        CHECK(p.periodSeconds == doctest::Approx(2.0 / 144.0));
    }
    SUBCASE("pacing = vsync: even divisor, else cadence; timer, vsync = 0, off, headless") {
        pc::PlanInput in = M3dInput(M::Display, 165.0, true);
        in.mode = pc::Mode::Vsync;
        CHECK(pc::SelectPlan(in).path == pc::Path::VblankLocked);
        in.rateMode = M::Max120;
        CHECK(pc::SelectPlan(in).path == pc::Path::Cadence);
        in.mode = pc::Mode::Timer;
        pc::Plan p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Timer);
        CHECK(p.nominalHz == doctest::Approx(120.0));
        in.mode = pc::Mode::Auto;
        in.vsync = false;
        CHECK(pc::SelectPlan(in).path == pc::Path::Timer);
        in.vsync = true;
        in.mode = pc::Mode::Off;
        CHECK(pc::SelectPlan(in).path == pc::Path::Unthrottled);
        in.mode = pc::Mode::Auto;
        in.env.swapChain = false;
        p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Headless);
        CHECK(p.nominalHz == doctest::Approx(120.0));
    }
    SUBCASE("background and minimized: free at the plan's even rate") {
        pc::PlanInput in = M3dInput(M::Display, 120.0, true);
        in.env.foreground = false;
        pc::Plan p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Free);
        CHECK(p.presentToSwapChain);
        CHECK(p.nominalHz == doctest::Approx(120.0));
        CHECK(pc::FrameAdvanceOf(p, 60.0, 120.0, 0).framesPerTick == 2u);  // the clock keeps its regime
        in.env.foreground = true;
        in.env.minimized = true;
        p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::Free);
        CHECK_FALSE(p.presentToSwapChain);
        in = M3dInput(M::Display, 144.0, false);
        in.env.foreground = false;
        CHECK(pc::SelectPlan(in).nominalHz == doctest::Approx(72.0));
    }
    SUBCASE("fixed rates keep the M3b plans and name their rate") {
        pc::PlanInput in = M3dInput(M::Fixed, 120.0, true);
        in.targetHz = 60.0;
        const pc::Plan p = pc::SelectPlan(in);
        CHECK(p.path == pc::Path::VblankLocked);
        CHECK(p.k == 2u);
        CHECK(p.nominalHz == doctest::Approx(60.0));
        CHECK(pc::FrameAdvanceOf(p, 60.0, 120.0, 0).framesPerTick == 1u);  // 60 fps: the M3b clock
        pc::Plan q = p;
        q.cadenceRatio = 1.2;
        CHECK_FALSE(pc::SamePlan(p, q));
        CHECK(pc::SamePlan(p, p));
    }
}

TEST_CASE("pacer m3d: frames per tick and the clock's advance per plan") {
    CHECK(pc::FramesPerTick(120.0, 60.0) == 2u);
    CHECK(pc::FramesPerTick(60.0, 60.0) == 1u);
    CHECK(pc::FramesPerTick(240.0, 60.0) == 4u);
    CHECK(pc::FramesPerTick(120.002, 60.0) == 2u);   // the measured TV (within 100 ppm)
    CHECK(pc::FramesPerTick(119.88, 60.0) == 0u);    // 1000 ppm off: display time
    // At or below the tick rate (+ 0.2 %): the M3b clock without interpolation (review finding 8) - display time
    // quanta there would give 2-step frames that the renderer can only snap.
    CHECK(pc::FramesPerTick(59.94, 60.0) == 1u);
    CHECK(pc::FramesPerTick(59.95, 60.0) == 1u);
    CHECK(pc::FramesPerTick(60.02, 60.0) == 1u);
    CHECK(pc::FramesPerTick(50.0, 60.0) == 1u);
    CHECK(pc::FramesPerTick(60.2, 60.0) == 0u);    // above: display time, interpolated
    CHECK(pc::FramesPerTick(72.0, 60.0) == 0u);
    CHECK(pc::FramesPerTick(100.0, 50.0) == 2u);     // PAL logic on 100 Hz
    CHECK(pc::FramesPerTick(0.0, 60.0) == 0u);
    CHECK(pc::FramesPerTick(120.0, 0.0) == 0u);

    pc::Plan none;
    CHECK_FALSE(pc::FrameAdvanceOf(none, 60.0, 120.0, 0).known);

    pc::Plan vblank;
    vblank.path = pc::Path::VblankLocked;
    vblank.k = 1;
    vblank.syncInterval = 1;
    vblank.nominalHz = 120.0;
    vblank.periodSeconds = 1.0 / 120.0;
    pc::FrameAdvance a = pc::FrameAdvanceOf(vblank, 60.0, 120.0, 1);
    CHECK(a.known);
    CHECK(a.framesPerTick == 2u);
    CHECK(a.seconds == doctest::Approx(1.0 / 120.0));
    CHECK(a.nominalSeconds == doctest::Approx(1.0 / 120.0));
    CHECK_FALSE(a.variable);
    CHECK(a.startDelayTicks == 0);

    pc::Plan display119 = vblank;
    display119.nominalHz = 119.88;
    display119.periodSeconds = 1.0 / 119.88;
    a = pc::FrameAdvanceOf(display119, 60.0, 119.88, 1);
    CHECK(a.framesPerTick == 0u);
    CHECK(a.seconds == doctest::Approx(1.0 / 119.88));

    // frame_rate = 120 | display on a 59.94 Hz or 50 Hz monitor: one frame per refresh, the M3b clock (finding 8).
    for (const double hz : {59.94, 50.0}) {
        CAPTURE(hz);
        const pc::RateChoice c = pc::ChooseFrameRate(pc::FrameRateMode::Display, hz, pc::kMaxFrameRate);
        CHECK(c.k == 1u);
        pc::Plan low = vblank;
        low.nominalHz = c.frameHz;
        low.periodSeconds = 1.0 / hz;
        a = pc::FrameAdvanceOf(low, 60.0, hz, 1);
        CHECK(a.framesPerTick == 1u);
    }

    pc::Plan cadence;
    cadence.path = pc::Path::Cadence;
    cadence.syncInterval = 1;
    cadence.nominalHz = 120.0;
    cadence.periodSeconds = 1.0 / 120.0;
    cadence.cadenceRatio = 1.2;
    a = pc::FrameAdvanceOf(cadence, 60.0, 144.0, 2);
    CHECK(a.framesPerTick == 0u);  // never a fixed fraction: the content follows each frame's display time
    CHECK(a.variable);
    CHECK(a.seconds == doctest::Approx(2.0 / 144.0));
    CHECK(a.nominalSeconds == doctest::Approx(1.0 / 120.0));
    a = pc::FrameAdvanceOf(cadence, 60.0, 144.0, 1);
    CHECK(a.seconds == doctest::Approx(1.0 / 144.0));

    pc::Plan timer;
    timer.path = pc::Path::TearingTimer;
    timer.nominalHz = 120.0;
    timer.periodSeconds = 1.0 / 120.0;
    a = pc::FrameAdvanceOf(timer, 60.0, 144.0, 0);
    CHECK(a.framesPerTick == 2u);
    CHECK(a.seconds == doctest::Approx(1.0 / 120.0));
}

// ---- just-in-time start ------------------------------------------------------------------------------------------

TEST_CASE("pacer m3d: latency budget = max(4 ms, p99 work + margin), doubled after a miss, relaxed after 2 s") {
    pc::LatencyBudget b;
    CHECK(b.Seconds(0.0) == doctest::Approx(pc::LatencyBudget::kMinSeconds));  // nothing measured yet
    for (int i = 0; i < 300; ++i) {
        b.AddWork(0.0003);  // 0.3 ms work per frame
    }
    CHECK(b.Seconds(0.0) == doctest::Approx(pc::LatencyBudget::kMinSeconds));  // 0.3 + 3.5 < 4 ms
    for (int i = 0; i < 300; ++i) {
        b.AddWork(0.0008);  // 0.8 ms work per frame (the measured RelWithDebInfo median)
    }
    CHECK(b.Seconds(0.0) == doctest::Approx(0.0008 + pc::LatencyBudget::kMarginSeconds));
    for (int i = 0; i < 300; ++i) {
        b.AddWork(i % 50 == 0 ? 0.004 : 0.001);  // 2 % of the frames take 4 ms
    }
    CHECK(b.Seconds(0.0) == doctest::Approx(0.004 + pc::LatencyBudget::kMarginSeconds));
    const double base = b.Seconds(10.0);
    b.NoteMiss(10.0);
    CHECK(b.Factor() == 2u);
    CHECK(b.Seconds(10.5) == doctest::Approx(2.0 * base));
    b.NoteMiss(11.0);
    b.NoteMiss(11.1);
    CHECK(b.Factor() == pc::LatencyBudget::kMaxFactor);  // capped at 4x
    CHECK(b.Misses() == 3u);
    CHECK(b.Seconds(11.2) == doctest::Approx(4.0 * base));
    CHECK(b.Seconds(13.2) == doctest::Approx(2.0 * base));  // 2 s without a miss: halved
    CHECK(b.Seconds(13.3) == doctest::Approx(2.0 * base));
    CHECK(b.Seconds(15.3) == doctest::Approx(base));
    b.AddWork(-1.0);  // ignored
    b.Reset();
    CHECK(b.Misses() == 3u);  // the count survives a reset (statistics)
    CHECK(b.Factor() == 1u);
    CHECK(b.Seconds(0.0) == doctest::Approx(pc::LatencyBudget::kMinSeconds));
}

TEST_CASE("pacer m3d: a frame that needs a lead between B and 4B is late a few times, not every relax interval") {
    // Review finding 9: 60 fps for 10 minutes, 1 ms CPU work (B = 4.5 ms); the screen needs `lead` before the vblank
    // (GPU time or composition the budget does not measure). Without the backoff a lead of 5-8 ms gave a late frame
    // every 2 s (29.4 per minute). Now: a few misses, then the factor holds.
    for (const double lead : {0.003, 0.005, 0.006, 0.008, 0.012}) {
        CAPTURE(lead);
        pc::LatencyBudget b;
        for (int i = 0; i < 300; ++i) {
            b.AddWork(0.001);
        }
        std::uint64_t late = 0;
        for (int frame = 0; frame < 60 * 600; ++frame) {
            const double t = frame / 60.0;
            if (b.Seconds(t) < lead) {
                ++late;
                b.NoteMiss(t + 1.0 / 60.0);
            }
        }
        if (lead < 0.0045) {
            CHECK(late == 0u);
        } else {
            CHECK(late <= 2u + pc::LatencyBudget::kMaxRelaxFailures);
            CHECK(b.Held());
            CHECK(b.Seconds(600.0) >= lead);
        }
        // A plan change starts the relax schedule over (the factor stays until a relax step succeeds).
        b.NotePlanChange();
        CHECK_FALSE(b.Held());
        CHECK(b.RelaxFailures() == 0u);
    }
}

TEST_CASE("pacer m3d: just-in-time start lies between now and now + max wait, else at once") {
    // 120 Hz in 100 ns ticks; the next present becomes visible at 1 000 000 + 2 refreshes.
    const std::int64_t visible = 1'000'000 + 166'667;
    const std::int64_t budget = 40'000;  // 4 ms
    CHECK(pc::JustInTimeStart(1'000'000, visible, budget, 166'667) == visible - budget);
    CHECK(pc::JustInTimeStart(visible - budget, visible, budget, 166'667) == 0);      // already there
    CHECK(pc::JustInTimeStart(visible - 10'000, visible, budget, 166'667) == 0);      // past the start
    CHECK(pc::JustInTimeStart(1'000'000, visible + 500'000, budget, 166'667) == 0);  // too far (bad prediction)
    CHECK(pc::JustInTimeStart(1'000'000, 0, budget, 166'667) == 0);
    CHECK(pc::JustInTimeStart(1'000'000, visible, 0, 166'667) == 0);
}

TEST_CASE("pacer m3d: the on-screen tracker predicts the vblank of the next present (k = 2 and k = 1)") {
    pc::OnScreenTracker t;
    t.SetQpcFrequency(1.0e7);
    const double refresh = 1.0e7 / 120.0;
    std::int64_t predicted = 0;
    CHECK_FALSE(t.PredictNextVisible(1.0 / 120.0, &predicted));  // no sample yet
    // Present(2): present n is issued right after present n-1 became visible; the statistics after present n
    // report n-1 as the newest visible present.
    std::uint32_t refreshCount = 100;
    std::int64_t vblank = 50'000'000;
    t.NotePresent(1, vblank - 70'000, 2);
    t.NoteStatistics(1, refreshCount, vblank);
    for (std::uint32_t id = 2; id <= 20; ++id) {
        t.NotePresent(id, vblank + 10'000, 2);
        REQUIRE(t.PredictNextVisible(1.0 / 120.0, &predicted));
        // present id + 1 becomes visible after id-1 (now) and id (2 refreshes each).
        CHECK(std::llabs(predicted - (vblank + std::llround(4.0 * refresh))) <= 1);
        vblank += std::llround(2.0 * refresh);
        refreshCount += 2;
        t.NoteStatistics(id, refreshCount, vblank);
    }
    // A repeated picture (M3d F1) is presented like any other frame: one more present of 2 refreshes.
    t.NotePresent(21, vblank + 10'000, 2);
    t.NotePresent(22, vblank + 12'000, 2);
    REQUIRE(t.PredictNextVisible(1.0 / 120.0, &predicted));
    CHECK(std::llabs(predicted - (vblank + std::llround(6.0 * refresh))) <= 1);
    // A present without a planned refresh count (timer plan) breaks the chain.
    t.NotePresent(23, vblank + 20'000, 0);
    CHECK_FALSE(t.PredictNextVisible(1.0 / 120.0, &predicted));
}

// ---- repeated picture (F1) --------------------------------------------------------------------------------------

TEST_CASE("pacer m3d: above the tick rate a frame without a step that drew nothing keeps the previous picture") {
    // Fixed developer rates on the timer plan (independent of the monitor): 120 = two frames per tick, 60 = one.
    for (const double target : {120.0, 60.0}) {
        CAPTURE(target);
        platform::ResetFramePacer();
        platform::PacingConfig c;
        c.mode = pc::Mode::Timer;
        c.targetHz = target;
        c.tickHz = 60;
        platform::SwapChainInfo chain;
        std::uint32_t id = 0, repeats = 0;
        for (int frame = 0; frame < 12; ++frame) {
            // Like the intro at 120 fps: the tick frame draws (in Update), the frame between draws nothing.
            platform::NoteFrameTicks(frame % 2 == 0 ? 1u : 0u);
            chain.frameUnchanged = frame % 2 == 1;
            const platform::PresentRequest r = platform::PlanPresent(c, chain);
            // A repeated picture is presented like every frame (the device shows the previous buffer again), so
            // the vblank plans' waitable keeps one signal per Present (verify phase: skipping it broke the plan).
            CHECK_FALSE(r.skipSwapChain);
            CHECK((!r.repeatPrevious || chain.frameUnchanged));
            repeats += r.repeatPrevious ? 1u : 0u;
            platform::PresentFeedback fb;
            fb.swapChainPresented = !r.skipSwapChain;
            fb.presentId = r.skipSwapChain ? id : ++id;
            platform::NoteSwapChainPresent(fb);
            platform::EndFrame(c);
            const pc::FrameAdvance a = platform::CurrentFrameAdvance();
            CHECK(a.known);
            CHECK(a.framesPerTick == (target > 60.0 ? 2u : 1u));
        }
        const platform::PresentStatistics s = platform::GetPresentStatistics();
        if (target > 60.0) {
            CHECK(repeats == 6u);  // every frame between two ticks (the first one follows a presented frame)
            CHECK(s.repeatedFrames == 6u);
        } else {
            CHECK(repeats == 0u);  // 60 fps: bit-identical to M3b, the back buffer is presented as it is
            CHECK(s.repeatedFrames == 0u);
        }
        CHECK(s.ticklessFrames == 6u);
    }
    platform::ResetFramePacer();
}

// ---- configuration --------------------------------------------------------------------------------------------------

TEST_CASE("pacer m3d: ParseFrameRate accepts exactly the schema choices of [render] frame_rate") {
    std::uint32_t hz = 0;
    pc::FrameRateMode mode = pc::FrameRateMode::Fixed;
    REQUIRE(gfx::ParseFrameRate("60", &hz, &mode));
    CHECK(hz == 60u);
    CHECK(mode == pc::FrameRateMode::Fixed);
    REQUIRE(gfx::ParseFrameRate(" 120 ", &hz, &mode));
    CHECK(hz == 120u);
    CHECK(mode == pc::FrameRateMode::Max120);
    REQUIRE(gfx::ParseFrameRate("Display", &hz, &mode));
    CHECK(hz == 120u);
    CHECK(mode == pc::FrameRateMode::Display);
    REQUIRE(gfx::ParseFrameRate("0", &hz, &mode));
    CHECK(hz == 0u);
    CHECK(mode == pc::FrameRateMode::Fixed);
    REQUIRE(gfx::ParseFrameRate("75", &hz, &mode));
    CHECK(mode == pc::FrameRateMode::Fixed);
    CHECK_FALSE(gfx::ParseFrameRate("monitor", &hz, &mode));
    CHECK_FALSE(gfx::ParseFrameRate("", &hz, &mode));
    CHECK_FALSE(gfx::ParseFrameRate("-1", &hz, &mode));
    // Every value the schema accepts parses, and the visible choices are 60, 120 and display.
    const core::ConfigKeyDef* key = core::FindConfigKey("render", "frame_rate");
    REQUIRE(key != nullptr);
    std::vector<std::string> visible;
    for (const core::ConfigChoice& c : key->choices) {
        const std::string value = c.value;
        if (value.find("..") != std::string::npos) {
            continue;
        }
        if (!c.hidden) {
            visible.push_back(value);
        }
        CAPTURE(value);
        std::string error;
        CHECK(core::ValidateConfigValue(*key, value, error));
        CHECK(gfx::ParseFrameRate(value, &hz, &mode));
        CHECK(std::strlen(c.label.de) > 0);
        CHECK(std::strlen(c.label.en) > 0);
    }
    CHECK(visible == std::vector<std::string>{"60", "120", "display"});
    for (const char* value : {"0", "50", "144", "1000", "DISPLAY"}) {
        CAPTURE(value);
        std::string error;
        CHECK(core::ValidateConfigValue(*key, value, error));
        CHECK(gfx::ParseFrameRate(value, &hz, &mode));
    }
    for (const char* value : {"monitor", "1001", "-5", "1.5"}) {
        CAPTURE(value);
        std::string error;
        CHECK_FALSE(core::ValidateConfigValue(*key, value, error));
    }
    const core::ConfigKeyDef* low = core::FindConfigKey("render", "low_latency");
    REQUIRE(low != nullptr);
    CHECK(std::string(low->defaultValue) == "on");
}

TEST_CASE("pacer m3d: [render] frame_rate = 120 | display and low_latency reach the pacing configuration") {
    core::Config cfg;
    std::string error;
    const auto plain = M3dIni("plain.ini", "[debug]\nselftest = 0\n");
    REQUIRE_MESSAGE(LoadM3dIni(plain, cfg, error), error);
    gfx::Settings s = gfx::SettingsFromConfig(cfg);
    CHECK(s.frameRateMode == pc::FrameRateMode::Fixed);  // unset: the tick rate (60 fps, M3b)
    CHECK(s.frameRate == 60u);
    CHECK(s.lowLatency);
    platform::PacingConfig c = gfx::PacingConfigOf(s);
    CHECK(c.rateMode == pc::FrameRateMode::Fixed);
    CHECK(c.justInTime);

    REQUIRE_MESSAGE(LoadM3dIni(plain, cfg, error, L" --set=render.frame_rate=120 --set=render.low_latency=0"), error);
    s = gfx::SettingsFromConfig(cfg);
    CHECK(s.frameRateMode == pc::FrameRateMode::Max120);
    CHECK(s.frameRate == 120u);
    CHECK_FALSE(s.lowLatency);
    c = gfx::PacingConfigOf(s);
    CHECK(c.rateMode == pc::FrameRateMode::Max120);
    CHECK(c.targetHz == doctest::Approx(120.0));
    CHECK(c.tickHz == 60u);
    CHECK(c.mode == pc::Mode::Auto);
    CHECK_FALSE(c.justInTime);

    REQUIRE_MESSAGE(LoadM3dIni(plain, cfg, error, L" --set=render.frame_rate=display --set=game.tick_rate=50"), error);
    s = gfx::SettingsFromConfig(cfg);
    CHECK(s.frameRateMode == pc::FrameRateMode::Display);
    c = gfx::PacingConfigOf(s);
    CHECK(c.rateMode == pc::FrameRateMode::Display);
    CHECK(c.tickHz == 50u);

    REQUIRE_MESSAGE(LoadM3dIni(plain, cfg, error, L" --set=render.frame_rate=60"), error);
    s = gfx::SettingsFromConfig(cfg);
    CHECK(s.frameRateMode == pc::FrameRateMode::Fixed);
    CHECK(s.frameRate == 60u);

    CHECK_FALSE(LoadM3dIni(plain, cfg, error, L" --set=render.frame_rate=monitor"));
    CHECK(error.find("frame_rate") != std::string::npos);
}

// ---- fullscreen view -------------------------------------------------------------------------------------------------

TEST_CASE("pacer m3d: the view covers the whole monitor in fullscreen (independent flip, VRR) and keeps 4:3 in a window") {
    platform::ViewRect r = platform::ViewLayout(3840, 2160, true);
    CHECK(r.x == 0);
    CHECK(r.y == 0);
    CHECK(r.width == 3840u);
    CHECK(r.height == 2160u);
    r = platform::ViewLayout(3840, 2160, false);
    CHECK(r.width == 2880u);
    CHECK(r.height == 2160u);
    CHECK(r.x == 480);
    r = platform::ViewLayout(1280, 960, false);
    CHECK(r.width == 1280u);
    CHECK(r.height == 960u);
}
