// M3d frame clock (xapi/game_timing.h FrameClock, FrameAlpha, FrameInterp) and the fixes for per-frame code
// (xapi/frame_fixes.h), analysis/loader/m3d_frameclock.md 2 and 4. The clock drives an exact model of the guest's
// fixed-step accumulator (Game::Frame 0xA2C20: cap at 6 dt 0xA2CE8, `while sec(elapsed) >= dt` with
// sec(e) = e * f32(1/QPF) 0xA2D94, elapsed -= ticks(dt), last = now - elapsed 0xA2F6A) at the QPC frequencies of
// the analysis (10 MHz, 3.58 MHz, 24 MHz, the Xbox's 733 MHz). Checked: N = 1 is the M3b lockstep clock bit for
// bit; at two frames per tick the step pattern is 1/0, alpha is exactly 1/2 in the frame that runs the tick and 1 in
// the frame between, and the displayed time G - 1 + alpha rises by exactly 1/2 per frame; events (missed vblank,
// early frame, context switch, stall) never move the displayed time backwards and the pattern comes back.
#include "doctest.h"

#include "core/guest.h"
#include "gfx/interp_frame.h"
#include "xapi/frame_fixes.h"
#include "xapi/game_timing.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr float kDt60 = 1.0f / 60.0f;  // 0x3C888889 (the NTSC dt the 60 Hz tick patches in)

// Game::Frame's accumulator with the guest's own arithmetic (timing_tests.cpp GuestAccumulator plus the
// remainder): x87 with 53-bit precision = double, ticks(s) = trunc(QPF * f32 s) (FUN_000D2B50).
struct Guest {
    std::int64_t qpf = 10'000'000;
    float dt = kDt60;
    std::int64_t last = 0;  // CApp+0xC8
    std::uint64_t ticks = 0;

    std::int64_t Step() const { return xapi::GuestTimerTicks(qpf, dt); }
    std::int64_t Cap() const { return xapi::GuestTimerTicks(qpf, dt * 6.0f); }  // fld dt; fmul 6.0f; fstp dword
    // One Game::Frame with the accumulator time `now`; returns the steps, `rest` = now - last afterwards.
    int Frame(std::int64_t now, std::int64_t* rest = nullptr) {
        const double inv = static_cast<double>(static_cast<float>(1.0 / static_cast<double>(qpf)));
        std::int64_t elapsed = now - last;
        const float cap = dt * 6.0f;
        if (static_cast<double>(cap) < static_cast<double>(elapsed) * inv) {
            elapsed = xapi::GuestTimerTicks(qpf, cap);
        }
        int steps = 0;
        for (int budget = 100; budget > 0 && !(static_cast<double>(elapsed) * inv < static_cast<double>(dt));
             --budget) {
            ++steps;
            elapsed -= Step();
        }
        last = now - elapsed;
        ticks += static_cast<std::uint64_t>(steps);
        if (rest != nullptr) {
            *rest = elapsed;
        }
        return steps;
    }
};

// Deterministic jitter in [-amplitude, +amplitude] (LCG).
struct Jitter {
    std::uint32_t state = 4711;
    std::int64_t Next(std::int64_t amplitude) {
        state = state * 1664525u + 1013904223u;
        if (amplitude <= 0) {
            return 0;
        }
        return static_cast<std::int64_t>(state % static_cast<std::uint32_t>(2 * amplitude + 1)) - amplitude;
    }
};

// One rendered frame of the simulation.
struct FrameRecord {
    std::int64_t real = 0;
    std::int64_t value = 0;  // V handed to the guest
    int steps = 0;
    std::int64_t rest = 0;
    float alpha = 1.0f;
    double shown = 0.0;  // displayed time in ticks: G - 1 + alpha
    bool reset = false, aligned = false;
    std::int64_t quanta = 0;
};

// The loader's frame sequence: the accumulator sample (FrameClock::Advance with CApp+0xC8 before the frame and the
// guest's cap), the guest's step loop, then the render sample (FrameAlpha with the clock's regime).
struct ClockSim {
    Guest guest;
    xapi::FrameClock clock;
    std::vector<FrameRecord> frames;

    FrameRecord Frame(std::int64_t real, const xapi::ClockQuantum& quantum) {
        FrameRecord r;
        r.real = real;
        const std::int64_t lastPrev = guest.last;
        const xapi::FrameClock::Sample s = clock.Advance(real, guest.Step(), quantum, lastPrev, guest.Cap());
        r.value = s.value;
        r.reset = s.reset;
        r.aligned = s.aligned;
        r.quanta = s.quanta;
        r.steps = guest.Frame(s.value, &r.rest);
        r.alpha = xapi::FrameAlpha(r.rest, guest.Step(), clock.FramesPerTick(), clock.PhaseB());
        r.shown = static_cast<double>(guest.ticks) - 1.0 + static_cast<double>(r.alpha);
        frames.push_back(r);
        return r;
    }
};

xapi::ClockQuantum PerTick(std::uint32_t n) {
    xapi::ClockQuantum q;
    q.framesPerTick = n;
    return q;
}

xapi::ClockQuantum DisplayTime(double qpf, double seconds, double nominalSeconds) {
    xapi::ClockQuantum q;
    q.framesPerTick = 0;
    q.ticks = seconds * qpf;
    q.nominalTicks = nominalSeconds * qpf;
    return q;
}

// orig/retail_eu/default.xbe (or FUT_TEST_XBE), section-mapped reads; empty when the file is missing.
struct XbeImage {
    std::vector<std::uint8_t> bytes;
    struct Section {
        std::uint32_t va = 0, size = 0, raw = 0;
    };
    std::vector<Section> sections;

    static XbeImage Load() {
        XbeImage x;
        wchar_t buf[1024];
        const DWORD n = GetEnvironmentVariableW(L"FUT_TEST_XBE", buf, static_cast<DWORD>(std::size(buf)));
        const std::filesystem::path path = n > 0 && n < std::size(buf)
                                               ? std::filesystem::path(buf)
                                               : std::filesystem::path(L"D:\\FuturamaDecomp\\orig\\retail_eu\\default.xbe");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            return x;
        }
        std::ifstream in(path, std::ios::binary);
        x.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (x.bytes.size() < 0x180) {
            x.bytes.clear();
            return x;
        }
        const auto u32 = [&x](std::size_t off) {
            std::uint32_t v = 0;
            std::memcpy(&v, x.bytes.data() + off, 4);
            return v;
        };
        const std::uint32_t base = u32(0x104);
        const std::uint32_t count = u32(0x11C);
        const std::uint32_t headers = u32(0x120);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t o = headers - base + i * 0x38u;
            if (o + 0x38 > x.bytes.size()) {
                break;
            }
            x.sections.push_back(Section{u32(o + 4), u32(o + 16), u32(o + 12)});
        }
        return x;
    }
    std::vector<std::uint8_t> Read(std::uint32_t va, std::uint32_t size) const {
        for (const Section& s : sections) {
            if (va >= s.va && va + size <= s.va + s.size && s.raw + (va - s.va) + size <= bytes.size()) {
                return std::vector<std::uint8_t>(bytes.begin() + s.raw + (va - s.va),
                                                 bytes.begin() + s.raw + (va - s.va) + size);
            }
        }
        return {};
    }
};

std::vector<std::uint8_t> Hex(const char* text) {
    std::vector<std::uint8_t> out;
    for (const char* p = text; *p != '\0';) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        out.push_back(static_cast<std::uint8_t>(std::stoul(std::string(p, 2), nullptr, 16)));
        p += 2;
    }
    return out;
}

}  // namespace

// ---- guest arithmetic ------------------------------------------------------------------------------------------

TEST_CASE("frameclock: guest step threshold, cap remainder and the tick halves per QPF (m3d_frameclock.md 2.2)") {
    struct Row {
        std::int64_t qpf, step, threshold, capSteps, capRest, qa, qb;
    };
    // Threshold = smallest elapsed that runs a step; capSteps/capRest = steps and remainder after the 6 dt cap:
    // ticks(f32(6 dt)) - 6 ticks(dt) = 4 at 10 MHz (6 steps), 0 at 3.58/24 MHz - there the sixth step falls one
    // tick short of the threshold, so the cap runs 5 steps and leaves a whole step below the threshold.
    const Row rows[] = {
        {10'000'000, 166'666, 166'667, 6, 4, 83'333, 83'333},
        {3'579'545, 59'659, 59'660, 5, 59'659, 29'829, 29'830},
        {24'000'000, 400'000, 400'001, 5, 400'000, 200'000, 200'000},
    };
    for (const Row& row : rows) {
        CAPTURE(row.qpf);
        Guest g;
        g.qpf = row.qpf;
        CHECK(g.Step() == row.step);
        for (const std::int64_t e : {row.threshold - 1, row.threshold}) {
            Guest t = g;
            t.last = 0;
            CHECK(t.Frame(e) == (e == row.threshold ? 1 : 0));
        }
        Guest capped = g;
        capped.last = 0;
        std::int64_t rest = -1;
        CHECK(capped.Frame(row.qpf * 10, &rest) == row.capSteps);  // a 10 s stall
        CHECK(rest == row.capRest);
        CHECK(row.capSteps * row.step + row.capRest == g.Cap());
        CHECK(xapi::FrameClock::TickQuantum(row.step, 2, 0) == row.qa);
        CHECK(xapi::FrameClock::TickQuantum(row.step, 2, 1) == row.qb);
    }
    // The Xbox's own QPF (733 333 333): 12 222 222 ticks per step.
    Guest xbox;
    xbox.qpf = 733'333'333;
    CHECK(xbox.Step() == 12'222'222);
    CHECK(xapi::FrameClock::PhaseMargin(166'666) == 162);
    CHECK(xapi::FrameClock::PhaseMargin(59'659) == 58);
    CHECK(xapi::FrameClock::PhaseMargin(10'000) == 16);  // at least 16 ticks
}

TEST_CASE("frameclock: TickQuantum splits a tick into N quanta that add up to exactly one tick") {
    for (const std::int64_t step : {166'666LL, 59'659LL, 400'000LL, 12'222'222LL, 7LL}) {
        for (std::uint32_t n = 1; n <= 5; ++n) {
            std::int64_t sum = 0;
            std::int64_t lo = step, hi = 0;
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::int64_t q = xapi::FrameClock::TickQuantum(step, n, i);
                sum += q;
                lo = std::min(lo, q);
                hi = std::max(hi, q);
            }
            CAPTURE(step);
            CAPTURE(n);
            CHECK(sum == step);
            CHECK(hi - lo <= 1);  // floor / ceil
            CHECK(xapi::FrameClock::TickQuantum(step, n, n) == xapi::FrameClock::TickQuantum(step, n, 0));
        }
    }
}

TEST_CASE("frameclock: FrameAlpha = clamp(1 - (L - rest) / step) on the 1/N grid, phase B L = step - mu") {
    constexpr std::int64_t P = 166'666;
    const std::int64_t mu = xapi::FrameClock::PhaseMargin(P);
    // Phase B: the zero-step frame (rest = L) shows S[G] as is, the tick frame (rest = q - mu) the midpoint.
    CHECK(xapi::FrameAlpha(P - mu, P, 2, true) == 1.0f);
    CHECK(xapi::FrameAlpha(P / 2 - mu, P, 2, true) == 0.5f);
    CHECK(xapi::FrameAlpha(P / 2 + 1 - mu, P, 2, true) == 0.5f);
    // Without phase B (realtime clock, display-time quanta): L = step, alpha = rest / step.
    CHECK(xapi::FrameAlpha(P / 4, P, 0, false) == doctest::Approx(0.25).epsilon(1e-5));
    CHECK(xapi::FrameAlpha(P / 4, P, 2, false) == doctest::Approx(0.25).epsilon(1e-5));  // off the grid: as is
    CHECK(xapi::FrameAlpha(P / 2 + 300, P, 2, false) == 0.5f);  // within 0.01 of 1/2: snapped
    CHECK(xapi::FrameAlpha(P - 100, P, 0, false) == 1.0f);       // >= 0.999 -> 1
    CHECK(xapi::FrameAlpha(-5, P, 0, false) == 0.0f);            // clamped
    CHECK(xapi::FrameAlpha(2 * P, P, 0, false) == 1.0f);
    CHECK(xapi::FrameAlpha(123, 0, 2, true) == 1.0f);  // no step known
}

// ---- N = 1: the M3b clock ------------------------------------------------------------------------------------------

TEST_CASE("frameclock: one frame per tick is the M3b lockstep clock bit for bit (60 fps unchanged)") {
    for (const std::int64_t qpf : {10'000'000LL, 3'579'545LL, 24'000'000LL}) {
        CAPTURE(qpf);
        Guest guest;
        guest.qpf = qpf;
        const std::int64_t step = guest.Step();
        xapi::LockstepClock m3b;
        xapi::FrameClock clock;
        Jitter jitter;
        const double period = static_cast<double>(qpf) / 60.0;
        std::int64_t real = qpf * 5;
        guest.last = real - qpf / 5;  // a stall before the first frame (cap)
        for (int k = 0; k < 6000; ++k) {
            // 60 Hz with sampling jitter, plus events: dropped frames, early frames, a slow drift, long stalls.
            double advance = period;
            if (k % 997 == 500) {
                advance *= 2.0;  // dropped frame
            } else if (k % 1301 == 700) {
                advance *= 0.3;  // early frame
            } else if (k % 2500 == 2000) {
                advance += static_cast<double>(qpf) * 0.4;  // stall (loading)
            } else if (k > 3000) {
                advance *= 1.0 / 0.999;  // 59.94 Hz display
            }
            real += static_cast<std::int64_t>(advance) + jitter.Next(qpf / 2000);
            const std::int64_t lastPrev = guest.last;
            const std::int64_t expected = m3b.Advance(real, step);
            const xapi::FrameClock::Sample s = clock.Advance(real, step, PerTick(1), lastPrev, guest.Cap());
            REQUIRE(s.value == expected);
            CHECK_FALSE(s.aligned);
            guest.Frame(s.value);
        }
        const auto& a = m3b.GetStats();
        const auto& b = clock.GetStats();
        CHECK(a.samples == b.samples);
        CHECK(a.single == b.single);
        CHECK(a.catchUps == b.catchUps);
        CHECK(a.holds == b.holds);
        CHECK(a.maxCatchUpSteps == b.maxCatchUpQuanta);
        CHECK(b.resets == 0u);
        CHECK(b.aligns == 0u);
        CHECK_FALSE(clock.PhaseB());
        CHECK(clock.FramesPerTick() == 1u);
    }
}

// ---- N = 2: the half-tick clock -------------------------------------------------------------------------------------

TEST_CASE("frameclock: 120 fps on a 60 Hz tick - 1/0 steps, alpha 1/2 and 1, displayed time +1/2 tick per frame") {
    for (const std::int64_t qpf : {10'000'000LL, 3'579'545LL, 24'000'000LL, 733'333'333LL}) {
        CAPTURE(qpf);
        ClockSim sim;
        sim.guest.qpf = qpf;
        const std::int64_t step = sim.guest.Step();
        const double period = static_cast<double>(qpf) / 120.0;
        Jitter jitter;
        const std::int64_t start = qpf * 3;
        sim.guest.last = start - qpf;  // Game::Init long before the first frame: the cap applies
        for (int k = 0; k < 4000; ++k) {
            // Sampling jitter of the accumulator probe: p99 0.09 ms, p99.9 1 ms (m3d_frameclock.md 2.3).
            const std::int64_t j = k % 100 == 37 ? jitter.Next(qpf / 1000) : jitter.Next(qpf / 20000);
            sim.Frame(start + static_cast<std::int64_t>(k * period) + j, PerTick(2));
        }
        const auto& f = sim.frames;
        CHECK(f[0].reset);
        CHECK(sim.clock.PhaseB());
        CHECK(sim.clock.GetStats().aligns <= 1u);
        // Frame 0 starts the clock (the guest caps the time since Game::Init), frame 1 aligns phase B. From then on
        // the displayed time rises by half a tick per frame (frames 1 and 2 may both run no step: alpha 1/2, 1).
        for (std::size_t i = 2; i < 4; ++i) {
            CHECK(f[i].shown - f[i - 1].shown == 0.5);
        }
        int half = 0, one = 0;
        for (std::size_t i = 4; i < f.size(); ++i) {
            CAPTURE(i);
            REQUIRE(f[i].steps + f[i - 1].steps == 1);  // strict 1/0 pattern
            REQUIRE(f[i].alpha == (f[i].steps == 1 ? 0.5f : 1.0f));
            REQUIRE(f[i].shown - f[i - 1].shown == 0.5);  // exactly half a tick per frame
            // g_frameDelta = V - V_prev = one quantum (8.333 ms at 10 MHz).
            const std::int64_t dv = f[i].value - f[i - 1].value;
            CHECK((dv == xapi::FrameClock::TickQuantum(step, 2, 0) || dv == xapi::FrameClock::TickQuantum(step, 2, 1)));
            (f[i].steps == 1 ? half : one) += 1;
        }
        CHECK(half == one);
        // The game time follows real time (within 3/4 quantum).
        const auto& st = sim.clock.GetStats();
        CHECK(static_cast<double>(st.maxPhase) <= period * 0.75 + 1.0);
        CHECK(static_cast<double>(st.minPhase) >= -period * 0.75 - 1.0);
        CHECK(st.catchUps == 0u);
        CHECK(st.holds == 0u);
    }
}

TEST_CASE("frameclock: events at 120 fps never move the displayed time backwards and the pattern comes back") {
    ClockSim sim;
    const std::int64_t qpf = sim.guest.qpf;
    const double period = static_cast<double>(qpf) / 120.0;
    const std::int64_t start = 50'000'000;
    sim.guest.last = start - 1'000'000;
    Jitter jitter;
    std::int64_t real = start;
    std::vector<std::size_t> eventFrames;
    for (int k = 0; k < 6000; ++k) {
        double advance = period;
        if (k % 840 == 300) {
            advance *= 2.0;  // a missed vblank (one refresh more)
            eventFrames.push_back(sim.frames.size());
        } else if (k % 1000 == 650) {
            advance = period * 0.42;  // a frame that starts 3.5 ms after its predecessor
            eventFrames.push_back(sim.frames.size());
        } else if (k % 2400 == 1200) {
            advance += 8.0 * period + 400'000;  // context switch: 8 black frames outside Game::Frame + 40 ms Enter
            eventFrames.push_back(sim.frames.size());
        } else if (k == 4500) {
            advance += 3'000'000;  // 300 ms stall (loading)
            eventFrames.push_back(sim.frames.size());
        }
        real += static_cast<std::int64_t>(advance);
        sim.Frame(real + jitter.Next(500), PerTick(2));
    }
    const auto& f = sim.frames;
    int snaps = 0;
    for (std::size_t i = 1; i < f.size(); ++i) {
        CAPTURE(i);
        CHECK(f[i].shown >= f[i - 1].shown);  // never backwards
        CHECK(f[i].value >= f[i - 1].value);  // the game time never runs backwards either
        snaps += f[i].steps >= 2 ? 1 : 0;
    }
    CHECK(snaps <= static_cast<int>(eventFrames.size()) * 2);
    // Within 4 frames after each event the 1/0 pattern with alpha 1/2 / 1 is back.
    for (const std::size_t e : eventFrames) {
        CAPTURE(e);
        REQUIRE(e + 12 < f.size());
        for (std::size_t i = e + 4; i < e + 12; ++i) {
            CHECK(f[i].steps + f[i + 1].steps == 1);
            CHECK(f[i].alpha == (f[i].steps == 1 ? 0.5f : 1.0f));
            CHECK(f[i + 1].shown - f[i].shown == 0.5);
        }
    }
    // Corrections only at the events (catch-ups or holds), alignments only after the cap (context switch, stall).
    const auto& st = sim.clock.GetStats();
    CHECK(st.catchUps + st.holds <= eventFrames.size() + 2);
    CHECK(st.aligns <= 4u);
}

TEST_CASE("frameclock: a regime change (plan with another frame period) is a reset and realigns phase B") {
    ClockSim sim;
    const std::int64_t start = 20'000'000;
    sim.guest.last = start - 2'000'000;
    std::int64_t real = start;
    // 60 fps (N = 1) for 100 frames, then 120 fps (N = 2), then a display-time regime (119.88 Hz) and back.
    for (int k = 0; k < 100; ++k) {
        sim.Frame(real, PerTick(1));
        real += 166'667;
    }
    CHECK_FALSE(sim.clock.PhaseB());
    const FrameRecord switched = sim.Frame(real, PerTick(2));
    CHECK(switched.reset);
    CHECK(sim.clock.GetStats().resets == 1u);
    for (int k = 0; k < 200; ++k) {
        real += 83'333;
        sim.Frame(real, PerTick(2));
    }
    CHECK(sim.clock.PhaseB());
    CHECK(sim.frames.back().steps + sim.frames[sim.frames.size() - 2].steps == 1);
    CHECK((sim.frames.back().alpha == 0.5f || sim.frames.back().alpha == 1.0f));
    const double q11988 = 1.0 / 119.88;
    real += 83'417;
    const FrameRecord display = sim.Frame(real, DisplayTime(1.0e7, q11988, q11988));
    CHECK(display.reset);
    CHECK_FALSE(sim.clock.PhaseB());
    CHECK(sim.clock.FramesPerTick() == 0u);
    for (int k = 0; k < 50; ++k) {
        real += 83'417;
        const FrameRecord r = sim.Frame(real, DisplayTime(1.0e7, q11988, q11988));
        CHECK_FALSE(r.reset);
    }
    CHECK(sim.clock.GetStats().resets == 2u);
}

TEST_CASE("frameclock: a plan change that moves the frame starts is absorbed once (review finding 11)") {
    // 120 fps; at frame 500 the pacing plan changes (free -> vblank) and every later frame starts 7 ms later on the
    // new grid (more than the 3/4 quantum margin of 6.25 ms).
    for (const bool rephase : {false, true}) {
        CAPTURE(rephase);
        ClockSim sim;
        const double period = static_cast<double>(sim.guest.qpf) / 120.0;
        const std::int64_t start = 30'000'000;
        sim.guest.last = start - 1'000'000;
        for (int k = 0; k < 1000; ++k) {
            xapi::ClockQuantum q = PerTick(2);
            q.rephase = rephase && k == 500;
            const std::int64_t shift = k >= 500 ? 70'000 : 0;
            sim.Frame(start + static_cast<std::int64_t>(k * period) + shift, q);
        }
        const auto& f = sim.frames;
        int corrections = 0;
        for (std::size_t i = 10; i < f.size(); ++i) {
            CAPTURE(i);
            CHECK(f[i].shown >= f[i - 1].shown);
            corrections += f[i].shown - f[i - 1].shown != 0.5 ? 1 : 0;
        }
        const auto& st = sim.clock.GetStats();
        if (rephase) {
            CHECK(corrections == 0);  // the displayed time keeps rising by 1/2 per frame
            CHECK(st.rephases == 1u);
            CHECK(st.catchUps + st.holds == 0u);
        } else {
            CHECK(corrections >= 1);  // the jump is caught up (and a hold follows)
            CHECK(st.catchUps >= 1u);
        }
    }
    // A jump of more than one tick is a real stall: not absorbed.
    ClockSim sim;
    const std::int64_t start = 30'000'000;
    sim.guest.last = start - 1'000'000;
    for (int k = 0; k < 200; ++k) {
        xapi::ClockQuantum q = PerTick(2);
        q.rephase = k == 100;
        sim.Frame(start + static_cast<std::int64_t>(k * 83'333.3) + (k >= 100 ? 300'000 : 0), q);
    }
    CHECK(sim.clock.GetStats().rephases == 0u);
    // N = 1 (the M3b clock) never takes an offset.
    ClockSim m3b;
    m3b.guest.last = start - 1'000'000;
    for (int k = 0; k < 200; ++k) {
        xapi::ClockQuantum q = PerTick(1);
        q.rephase = k == 100;
        m3b.Frame(start + static_cast<std::int64_t>(k * 166'666.7) + (k >= 100 ? 50'000 : 0), q);
    }
    CHECK(m3b.clock.GetStats().rephases == 0u);
}

// ---- N = 0: display-time quanta ------------------------------------------------------------------------------------

TEST_CASE("frameclock: display-time quanta follow the display exactly (119.88, 100, 75 Hz and the 144 Hz cadence)") {
    struct Case {
        double hz;
        bool cadence;  // 144 Hz cadence 1,1,2,1,1 refreshes (120 frames per second on average)
    };
    for (const Case c : {Case{119.88, false}, Case{100.0, false}, Case{75.0, false}, Case{144.0, true}}) {
        CAPTURE(c.hz);
        ClockSim sim;
        const double qpf = 1.0e7;
        const std::int64_t start = 30'000'000;
        sim.guest.last = start - 2'000'000;
        double t = static_cast<double>(start);
        const std::uint32_t pattern[] = {1, 1, 2, 1, 1};
        const double nominal = c.cadence ? 1.0 / 120.0 : 1.0 / c.hz;
        double displaySum = 0.0;
        std::int64_t firstValue = 0;
        double previousSeconds = c.cadence ? pattern[4] / c.hz : 1.0 / c.hz;
        for (int k = 0; k < 3000; ++k) {
            // The pacer's advance for frame k is the display time of frame k - 1 (FrameAdvanceOf with the refreshes
            // of the frame just presented): the time from its vblank to the vblank of frame k.
            const FrameRecord r =
                sim.Frame(static_cast<std::int64_t>(t), DisplayTime(qpf, previousSeconds, nominal));
            if (k == 0) {
                firstValue = r.value;
            } else {
                displaySum += previousSeconds * qpf;
            }
            const double seconds = c.cadence ? pattern[k % 5] / c.hz : 1.0 / c.hz;
            t += seconds * qpf;
            previousSeconds = seconds;
        }
        const auto& f = sim.frames;
        // The game time advanced by the sum of the display times (fractions carried, no drift).
        CHECK(std::fabs(static_cast<double>(f.back().value - firstValue) - displaySum) <= 1.0);
        const auto& st = sim.clock.GetStats();
        CHECK(st.catchUps == 0u);
        CHECK(st.holds == 0u);
        for (std::size_t i = 1; i < f.size(); ++i) {
            CAPTURE(i);
            CHECK(f[i].shown >= f[i - 1].shown);
            CHECK(f[i].steps <= 1);  // above the tick rate: at most one step per frame
            CHECK(f[i].alpha >= 0.0f);
            CHECK(f[i].alpha <= 1.0f);
            // The displayed time follows the game time: shown increments = this frame's quantum in ticks (alpha
            // >= 0.999 counts as 1: at most 0.001 tick off).
            const double expected = static_cast<double>(f[i].value - f[i - 1].value) / static_cast<double>(sim.guest.Step());
            CHECK(std::fabs((f[i].shown - f[i - 1].shown) - expected) <= 1.01e-3);
        }
    }
}

// ---- outside the loader ---------------------------------------------------------------------------------------------

TEST_CASE("frameclock: without the loader the renderer sees 'as is' and the fixes stay off") {
    const xapi::FrameInterp f = xapi::CurrentFrameInterp();
    CHECK_FALSE(f.enabled);
    CHECK(f.alpha == 1.0f);
    CHECK((f.flags & xapi::kFrameInterpInsideStep) != 0u);
    CHECK_FALSE(xapi::SubTickFramesActive());
    CHECK_FALSE(xapi::RenderingFrameWithoutStep());
    // The renderer's contract uses the same flag bits (gfx/interp_frame.h).
    CHECK(xapi::kFrameInterpSnapAll == gfx::interp::kFrameInterpSnapAll);
    CHECK(xapi::kFrameInterpInsideStep == gfx::interp::kFrameInterpInsideStep);
    CHECK(xapi::kFrameInterpClockReset == gfx::interp::kFrameInterpClockReset);
    const xapi::FrameClockStats s = xapi::GetFrameClockStats();
    CHECK(s.renderedFrames == 0u);
    CHECK(s.subTickFrames == 0u);
    const xapi::FrameFixStats fixes = xapi::GetFrameFixStats();
    CHECK(fixes.deathTimeCleared == 0u);
    CHECK(fixes.occluderFadeSkipped == 0u);
    CHECK(fixes.flickerKept == 0u);
}

// ---- frame fixes (m3d_frameclock.md 4.2 F2-F4) ------------------------------------------------------------------

TEST_CASE("frameclock: FlickerValue is FUN_00045EF0's signed rand() & 1") {
    CHECK(xapi::FlickerValue(0) == 0u);
    CHECK(xapi::FlickerValue(1) == 1u);
    CHECK(xapi::FlickerValue(0x7FFE) == 0u);
    CHECK(xapi::FlickerValue(0x7FFF) == 1u);
    // Negative inputs (never from rand(), but the compiler's sequence handles them): -1 & 0x80000001 -> -1.
    CHECK(xapi::FlickerValue(0xFFFFFFFFu) == 0xFFFFFFFFu);
    CHECK(xapi::FlickerValue(0x80000000u) == 0u);
}

TEST_CASE("frameclock: the fix targets are function starts with the expected bytes (skipped without the XBE)") {
    struct Target {
        guest::GuestAddr address;
        const char* bytes;  // whole function (Death::Render, FUN_00045EF0) or the stolen prologue (FUN_000CD340)
    };
    const Target targets[] = {
        // Death::Render: push esi; mov esi,ecx; mov ecx,[esi+0x3C]; test ecx,ecx; je; mov edx,[esi+0x20];
        // mov eax,[ecx]; push edx; call [eax+4]; test eax,eax; jne; mov byte [esi+0x40],1; pop esi; ret
        {0x0002BF10, "56 8B F1 8B 4E 3C 85 C9 74 11 8B 56 20 8B 01 52 FF 50 04 85 C0 75 04 C6 46 40 01 5E C3"},
        {0x000CD340, "83 EC 3C 56 8B F1"},
        // FUN_00045EF0: push esi; mov esi,ecx; call _rand; and eax,0x80000001; jns; dec; or eax,-2; inc;
        // mov [esi+0x24],eax; mov byte [esi+0xA8],1; pop esi; ret
        {0x00045EF0, "56 8B F1 E8 D4 09 12 00 25 01 00 00 80 79 05 48 83 C8 FE 40 89 46 24 C6 86 A8 00 00 00 01 5E C3"},
    };
    for (const Target& t : targets) {
        CAPTURE(t.address);
        const guest::FunctionInfo* fn = guest::FindFunctionStart(t.address);
        REQUIRE(fn != nullptr);
    }
    const XbeImage x = XbeImage::Load();
    if (x.bytes.empty()) {
        return;
    }
    for (const Target& t : targets) {
        CAPTURE(t.address);
        const std::vector<std::uint8_t> expected = Hex(t.bytes);
        CHECK(x.Read(t.address, static_cast<std::uint32_t>(expected.size())) == expected);
    }
    // The call in FUN_00045EF0 goes to the CRT's rand (0x1668CC), which the replacement calls through CallGuest.
    const std::vector<std::uint8_t> call = x.Read(0x00045EF3, 5);
    REQUIRE(call.size() == 5u);
    std::int32_t rel = 0;
    std::memcpy(&rel, call.data() + 1, 4);
    CHECK(0x00045EF8u + static_cast<std::uint32_t>(rel) == 0x001668CCu);
}
