// Display-coupled frame pacing (M3b): the pure decision logic and arithmetic of platform/frame_pacer.cpp.
// No Win32 calls and no global state, so tests/pacer_tests.cpp checks all of it headless. Owned by the
// pacing agent (M3b).
//
// Background (analysis/loader/pacing60.md, German):
//   - The game steps its logic with a fixed dt (1/60 s with [game] tick_rate = 60, timing agent) and renders
//     once per CApp::Frame without interpolation (startup.md 3.2), so the best picture is one game frame per
//     tick, each shown for the same number of display refreshes.
//   - Refresh R an integer multiple k of the target rate T (60/120/180/240 Hz, also 59.94/119.88 within
//     0.2 %, pacing60.md 3.3): flip-model swap chain with a frame-latency waitable object (maximum latency 1)
//     and Present(k) - every frame stays exactly k refreshes (measured 592 of 596 frames at 120 Hz,
//     pacing60.md 2.4 "sync:2").
//   - Otherwise (75/90/100/144/165 Hz ...): if the system allows tearing, an absolute high-resolution timer at
//     T + Present(0, ALLOW_TEARING) - on a VRR display the panel follows the 60 Hz presents. Whether VRR is
//     really active cannot be queried (pacing60.md 4.6); VrrDetector checks the DXGI frame statistics and
//     rejects the path when the visible intervals follow the refresh grid or presents share vblanks
//     (tearing without VRR). Without tearing or after a rejection: Present(1), one game frame per refresh
//     ("vsync at display rate": presented rate R >= 60 on any common display; the fixed-step loop gives
//     0 or 1 steps per frame).
//   - The pace must never depend on a blocking Present alone (pacing60.md 2.3, 4.1): a VRR panel follows the
//     foreground application and fell to 40.4 Hz while another program was in front. In the background,
//     minimized, with the display off or the session locked the pacer runs "free" on its timer at T and
//     presents only when the swap chain has a free slot (never blocks).
//
// [render] pacing (Mode) overrides the automatic choice: vsync = always vblank-coupled (Present(k) or
// Present(1), no tearing path, no background switch while the window is visible), timer = the absolute
// timer at T with non-blocking Present(0) (tearing with [render] vsync = 0), off = no wait at all
// (benchmarks; Present(0) only into free slots).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace platform::pacing {

// ---- configuration --------------------------------------------------------------------------------------------

enum class Mode : std::uint8_t { Auto, Vsync, Timer, Off };  // [render] pacing
const char* ModeName(Mode mode) noexcept;
// "auto" | "vsync" | "timer" | "off" (case-insensitive). False for anything else (*mode unchanged).
bool ParseMode(std::string_view text, Mode* mode) noexcept;

// ---- refresh rate --------------------------------------------------------------------------------------------

inline constexpr double kIntegerTolerance = 0.002;     // |R/k - T| <= 0.2 % of T counts as an integer multiple
inline constexpr std::uint32_t kMaxSyncInterval = 4;   // DXGI Present: 0..4

struct RefreshClass {
    bool known = false;      // refresh and target > 0
    bool integer = false;    // R = k * T within kIntegerTolerance (k >= 1)
    std::uint32_t k = 0;     // round(R / T) (0 if unknown or R < T / 2)
};
RefreshClass ClassifyRefresh(double refreshHz, double targetHz) noexcept;

// ---- plan selection ------------------------------------------------------------------------------------------

enum class Path : std::uint8_t {
    None,          // not planned yet
    VblankLocked,  // frame-latency waitable + Present(k): refresh = k * target, k = 1..4
    DisplayRate,   // frame-latency waitable + Present(1): one game frame per refresh
    TearingTimer,  // absolute timer at the target rate + Present(0, ALLOW_TEARING) (VRR path)
    Timer,         // absolute timer at the target rate + non-blocking Present(0) ([render] pacing = timer)
    Free,          // like Timer while the window is in the background, minimized, display off, locked ...
    Unthrottled,   // no wait ([render] pacing = off)
    Headless,      // no swap chain (null backend, tests): absolute timer at the target rate
    Count
};
inline constexpr std::size_t kPathCount = static_cast<std::size_t>(Path::Count);
const char* PathName(Path path) noexcept;  // "none", "vblank", "display-rate", "tearing-timer", "timer", ...
// True for the paths that wait on the frame-latency waitable object (the vblank is the clock).
bool IsVblankPath(Path path) noexcept;
// True for the paths whose clock is the absolute timer (FrameSchedule).
bool IsTimerPath(Path path) noexcept;

enum class VrrState : std::uint8_t { Unknown, Probing, Confirmed, Rejected };
const char* VrrStateName(VrrState state) noexcept;

struct Environment {
    bool swapChain = true;        // false: headless (no window / null backend)
    bool foreground = true;       // the game's frame window is the foreground window
    bool minimized = false;
    bool displayOn = true;        // GUID_CONSOLE_DISPLAY_STATE != off
    bool sessionLocked = false;   // WTS_SESSION_LOCK
    bool displayLimited = false;  // vblank waits took far longer than planned (pacer detector, auto only)
};

struct PlanInput {
    Mode mode = Mode::Auto;
    bool vsync = true;              // [render] vsync (0: auto behaves like timer, timer/off may tear)
    double targetHz = 60.0;         // game frames per second ([render] frame_rate, default [game] tick_rate)
    double refreshHz = 0.0;         // display refresh, 0 = unknown
    bool tearingSupported = false;  // DXGI_FEATURE_PRESENT_ALLOW_TEARING + swap chain flag
    bool waitable = false;          // the swap chain has a frame-latency waitable object
    VrrState vrr = VrrState::Unknown;
    Environment env;
};

struct Plan {
    Path path = Path::None;
    std::uint32_t syncInterval = 0;  // Present sync interval
    bool allowTearing = false;       // DXGI_PRESENT_ALLOW_TEARING (only with syncInterval 0)
    bool presentToSwapChain = true;  // false: minimized / display off / locked -> only rotate the back buffers
    std::uint32_t k = 0;             // VblankLocked: refreshes per frame; DisplayRate: 1
    double periodSeconds = 0.0;      // planned frame period (1/T; DisplayRate: 1/R)
    const char* reason = "";         // static text for the log
};
Plan SelectPlan(const PlanInput& in) noexcept;
// Same path, sync interval, tearing, present flag and period (within 1e-9 s).
bool SamePlan(const Plan& a, const Plan& b) noexcept;

// Hysteresis between plans (pacing60.md 4.3): a change INTO a timer-driven or presentation-less plan
// (Free, Headless, Unthrottled, Timer) and any change that the configuration or the display forces (the
// previous plan was None, or `immediate`) is accepted at once; a change OUT of Free (the window came back
// to the foreground, the display came on) needs `holdFrames` consecutive frames that want the same plan, so
// a flickering foreground state does not toggle the clock every frame. Exception (review M3b finding 5): a
// change from a plan without presents (minimized: Free with presentToSwapChain = false) to one that presents
// is accepted at once - restoring the window must not show the frozen last picture for `holdFrames` frames.
class PlanHysteresis {
public:
    explicit PlanHysteresis(std::uint32_t holdFrames = 30) noexcept : holdFrames_(holdFrames) {}
    bool Accept(const Plan& current, const Plan& desired, bool immediate) noexcept;
    void Reset() noexcept { pendingCount_ = 0; pending_ = Plan{}; }

private:
    std::uint32_t holdFrames_;
    Plan pending_;
    std::uint32_t pendingCount_ = 0;
};

// ---- timer arithmetic ------------------------------------------------------------------------------------------

// Absolute frame schedule at a fractional period in QPC ticks: deadline n = anchor + round(n * period), so
// 60 Hz at QPF 10 MHz holds 60.000 Hz exactly (166 666.67 ticks per frame; the M2 pacer used the integer
// period 166 666 = 60.0002 Hz). A frame that ends more than one period after its deadline starts a new
// schedule at that moment ("schedule reset", the lost time is not made up - the game's own fixed-step
// accumulator does that); a frame that is late by less than one period does not wait and keeps the grid.
class FrameSchedule {
public:
    struct Step {
        std::int64_t waitUntil = 0;  // QPC; <= now means no wait
        bool started = false;        // the first frame or a new period started the schedule (no wait)
        bool reset = false;          // more than one period late: new schedule (counted)
        bool late = false;           // after the deadline but within one period: no wait, grid kept
    };
    // Next frame boundary after a frame that ended at `now`. A different period restarts the schedule.
    Step Next(std::int64_t now, double periodTicks) noexcept;
    void Reset() noexcept { started_ = false; }
    // Starts the schedule on the grid of an earlier frame start `anchor` (a change from a vblank-coupled plan to
    // a timer plan keeps the frame period instead of starting the first timer frame at once).
    void Restart(std::int64_t anchor, double periodTicks) noexcept;
    // Moves every later deadline by `ticks` (CadenceGuard: half a refresh). No effect before the schedule started.
    void Shift(std::int64_t ticks) noexcept {
        if (started_) {
            anchor_ += ticks;
        }
    }
    std::uint64_t Resets() const noexcept { return resets_; }
    double Period() const noexcept { return period_; }

private:
    bool started_ = false;
    std::int64_t anchor_ = 0;
    std::uint64_t index_ = 0;
    double period_ = 0.0;
    std::uint64_t resets_ = 0;
};

// Relative due time for SetWaitableTimer (100 ns units, negative = relative) of `ticks` QPC ticks at
// frequency `qpf`; 0 for ticks <= 0.
std::int64_t RelativeDueTime100ns(std::int64_t ticks, std::int64_t qpf) noexcept;
// Timeout of a vblank wait: 2 planned periods (pacing60.md 4.1 rule 2), rounded up, 4..250 ms.
std::uint32_t VblankWaitTimeoutMs(double periodSeconds) noexcept;

// ---- cadence guard of the timer plans --------------------------------------------------------------------------

// Timer plans (Timer, Free without tearing) on a display whose refresh is k x the target (60 Hz on 120 Hz) are
// not phase-locked to the vblank: the absolute timer starts every frame at the same phase of the refresh grid,
// and a present that reaches the compositor close to its latch point flips between k - 1 and k + 1 refreshes
// with the timer and work jitter (review M3b finding 6; measured with pacing = timer at 60 Hz on the 120 Hz
// machine: "1:406 2:1934 3:571" in one run, 872 of 884 regular in another - the phase is random per run). The
// effective latch point relative to the reported vblank differs between builds and loads (the present pass has
// to finish on the GPU), so the guard does not aim at a fixed phase: it watches the on-screen duration of
// consecutive presents and asks for a shift of the timer grid by half a refresh - the point farthest from the
// latch - when kIrregularLimit of kWindow durations were not k refreshes. Real hitches (a few per minute) stay
// below the limit. After a shift the next kHoldoff durations (frames still in flight) are not judged.
// Phase-5 hardening (measured on the same machine once the windowed swap chain ran in independent flip with VRR:
// every 60 Hz present stayed ONE refresh of ~16.7 ms, the guard shifted 50 times in 30 s and the timer fell to
// 59.62 Hz): a duration that missed k refreshes but whose visible interval is within half a refresh of k refresh
// periods counts as regular - the panel followed the presents ("followed"); the shift direction alternates
// (+1/2, -1/2, ... refresh: the same phase modulo the refresh, but the grid never drifts, so the average rate
// stays exact); after kMaxShifts shifts without a clean window in between the guard stops until Reset().
class CadenceGuard {
public:
    static constexpr std::uint32_t kWindow = 60;
    static constexpr std::uint32_t kIrregularLimit = 6;
    static constexpr std::uint32_t kHoldoff = 30;
    static constexpr std::uint32_t kMaxShifts = 4;
    // One on-screen duration in refreshes of a present whose plan expected `expected` refreshes (0 = not judged:
    // vblank plans, tearing, refresh not an integer multiple), with its visible interval and the nominal refresh
    // period in seconds (negative / 0 = unknown: judged by the refresh count alone). Returns the shift of the
    // timer grid in half refreshes to apply now: +1 or -1 (the window starts over), 0 = none.
    int AddDuration(std::uint32_t refreshes, std::uint32_t expected, double visibleSeconds = -1.0,
                    double refreshSeconds = 0.0) noexcept;
    // Plan change or display event: judging starts over (the shift and follow counters stay).
    void Reset() noexcept {
        samples_ = irregular_ = holdoff_ = streak_ = 0;
        stopped_ = false;
    }
    std::uint64_t Shifts() const noexcept { return shifts_; }
    std::uint64_t Judged() const noexcept { return judged_; }
    std::uint64_t Followed() const noexcept { return followed_; }  // missed k refreshes but on time (VRR)
    bool Stopped() const noexcept { return stopped_; }             // kMaxShifts without a clean window

private:
    std::uint32_t samples_ = 0, irregular_ = 0, holdoff_ = 0, streak_ = 0;
    bool stopped_ = false;
    std::uint64_t shifts_ = 0, judged_ = 0, followed_ = 0;
};

// ---- display-limited detector -------------------------------------------------------------------------------

// Vblank waits that take more than 1.5 planned periods (or time out) in 3 of the last 8 frames mean that the
// display - not the game - holds the pace (a VRR panel at its floor, a display that went off without a
// notification). The pacer then runs Free for a hold time that doubles with every repeat (1, 2, 4, 8 s) and
// shrinks back to 1 s after 10 s without a detection.
class DisplayLimitDetector {
public:
    // One vblank wait: its duration and whether it timed out. Returns true when the detector fires.
    bool AddWait(double waitSeconds, double periodSeconds, bool timedOut) noexcept;
    // Hold time for the detection that just fired (seconds); `nowSeconds` is any monotonic clock.
    double FireHoldSeconds(double nowSeconds) noexcept;
    void ResetWindow() noexcept { history_ = 0; count_ = 0; }

private:
    std::uint8_t history_ = 0;  // bit i = frame i ago was slow
    std::uint32_t count_ = 0;   // frames seen (up to 8)
    double hold_ = 0.0;
    double lastFire_ = -1.0e9;
};

// ---- VRR detection from DXGI frame statistics -------------------------------------------------------------------

// Probing state of the TearingTimer path (pacing60.md 4.6): `kSamples` visible intervals of consecutive presents.
//   Confirmed: >= 90 % of the intervals within 1/T +- 1.5 ms and more than 0.3 ms away from every multiple of
//              the refresh period (the panel follows the presents)
//   Rejected:  > 10 % of the presents share their vblank with the previous one (tearing without VRR), or the
//              intervals follow the refresh grid (fixed refresh, composed window), or fewer than kSamples
//              intervals within kMaxFrames frames after the warm-up (no frame statistics: occluded, other adapter)
// Warm-up (review M3b finding 13a): the first `warmupFrames` frames of a probe take no samples - the change into
// independent flip after showing the window or Alt+Enter took 1.4-1.9 s in pacing60.md 2.4; the pacer starts a
// probe with 2 s of warm-up and repeats a rejected probe once (frame_pacer.cpp).
class VrrDetector {
public:
    static constexpr std::uint32_t kSamples = 60;
    static constexpr std::uint32_t kMaxFrames = 240;
    void Start(double targetHz, double refreshHz, std::uint32_t warmupFrames = 0) noexcept;
    void Stop() noexcept { state_ = VrrState::Unknown; }
    // One visible interval between two consecutive presents (seconds, 0 = same vblank).
    void AddInterval(double seconds) noexcept;
    // One game frame of the probe (with or without a usable interval).
    void AddFrame() noexcept;
    VrrState State() const noexcept { return state_; }
    const char* Reason() const noexcept { return reason_; }
    double MatchFraction() const noexcept { return samples_ != 0 ? static_cast<double>(match_) / samples_ : 0.0; }
    bool WarmingUp() const noexcept { return state_ == VrrState::Probing && frames_ < warmup_; }

private:
    void Evaluate() noexcept;
    VrrState state_ = VrrState::Unknown;
    double target_ = 60.0;
    double refresh_ = 0.0;
    std::uint32_t samples_ = 0, match_ = 0, shared_ = 0, frames_ = 0, warmup_ = 0;
    const char* reason_ = "";
};

// ---- statistics ---------------------------------------------------------------------------------------------

// Frame intervals relative to the planned period of their frame: buckets [0, .5), [.5, .85), [.85, .95),
// [.95, 1.05), [1.05, 1.5), [1.5, 2.5), [2.5, 4), [4, inf) periods.
class IntervalHistogram {
public:
    static constexpr std::size_t kBuckets = 8;
    static constexpr double kEdges[kBuckets - 1] = {0.5, 0.85, 0.95, 1.05, 1.5, 2.5, 4.0};
    void Add(double seconds, double periodSeconds) noexcept;
    std::uint64_t Count(std::size_t bucket) const noexcept { return bucket < kBuckets ? counts_[bucket] : 0; }
    std::uint64_t Total() const noexcept { return total_; }
    double OnPeriodPercent() const noexcept;  // bucket [.95, 1.05) in percent of all
    void Reset() noexcept;

private:
    std::uint64_t counts_[kBuckets] = {};
    std::uint64_t total_ = 0;
};

// What reached the screen, from the DXGI frame statistics after each Present (pacing60.md 5).
// NotePresent records every swap-chain Present (its id from GetLastPresentCount, the QPC of the call, the
// planned number of refreshes - 0 = not vblank-coupled - and the nominal number for timer plans on an integer
// refresh, 0 = none). NoteStatistics takes one GetFrameStatistics sample (PresentCount of the newest visible
// present, SyncRefreshCount and SyncQPCTime of the vblank in which it became visible). Two samples of
// consecutive present ids give the on-screen duration of the older one in refreshes and the visible interval;
// a sample also gives the latency present call -> visible.
// Review M3b finding 9: one sample per frame misses presents that became visible between two samples. Such
// jumps (d >= 2 presents in one sample step) are counted separately with the refreshes they took (`jumps`,
// `jumpPresents`, `jumpsIrregular` = dr != d * expected), so stutter of the timer plans is not hidden. The
// measured refresh is a least-squares fit of SyncQPCTime over SyncRefreshCount across ALL samples, pooled over
// segments with their own offset: a new segment starts when the counter stands still or goes back, after a gap
// of more than 1 s, or when the count differs from what the elapsed time allows at the nominal refresh by half a
// refresh or more. The counter sometimes lags its time stamp by one refresh, for a single sample or for good
// (measured), and the old sum over d == 1 pairs read 119.66-119.98 Hz on a 120.001 Hz display.
class OnScreenTracker {
public:
    static constexpr std::size_t kDurationBuckets = 6;  // 0, 1, 2, 3, 4, 5+ refreshes
    static constexpr std::size_t kLatencyBuckets = 1001;  // 0.1 ms up to 100 ms
    void SetQpcFrequency(double qpf) noexcept { qpf_ = qpf > 0.0 ? qpf : 1.0; }
    // Nominal refresh of the display (segment breaks of the refresh fit; 0 = unknown).
    void SetNominalRefreshHz(double hz) noexcept { nominalHz_ = hz > 0.0 ? hz : 0.0; }
    void NotePresent(std::uint32_t presentId, std::int64_t qpc, std::uint32_t plannedRefreshes,
                     std::uint32_t nominalRefreshes = 0) noexcept;
    // Returns the visible interval (seconds) if this sample follows the previous one by exactly one present,
    // else a negative value.
    double NoteStatistics(std::uint32_t presentCount, std::uint32_t syncRefreshCount, std::int64_t syncQpc) noexcept;
    void NoteInvalid() noexcept { ++invalid_; }
    void Reset() noexcept;

    // Duration of the pair the last NoteStatistics completed (d == 1): refreshes, the visible interval in seconds
    // and what its present expected (planned, else nominal; 0 = nothing expected). valid = false when the last
    // sample completed no pair.
    struct LastPair {
        bool valid = false;
        std::uint32_t refreshes = 0;
        std::uint32_t expected = 0;
        double seconds = 0.0;
    };
    LastPair Last() const noexcept { return last_; }

    struct Result {
        std::uint64_t presents = 0;      // NotePresent calls
        std::uint64_t samples = 0;       // distinct visible presents seen
        std::uint64_t pairs = 0;         // consecutive pairs (durations known)
        std::uint64_t invalid = 0;       // GetFrameStatistics failed (disjoint, occluded, unsupported)
        std::uint64_t durations[kDurationBuckets] = {};
        std::uint64_t planned = 0;       // pairs with a vblank-coupled plan (plannedRefreshes > 0)
        std::uint64_t onPlan = 0;        // ... whose duration equalled the plan
        std::uint64_t late = 0;          // ... that stayed longer than planned
        std::uint64_t early = 0;         // ... that stayed shorter than planned
        std::uint64_t jumps = 0;         // sample steps over d >= 2 presents (durations unknown)
        std::uint64_t jumpPresents = 0;  // sum of d over those steps
        std::uint64_t jumpsIrregular = 0;  // ... with an expectation and dr != d * expected
        double measuredHz = 0.0;         // least-squares fit over all samples (0 = unknown)
        std::uint64_t refreshSamples = 0;  // samples in the fit
        std::uint64_t refreshSegments = 0; // segments of the fit
        double latencyMedianMs = 0.0;    // present call -> visible
        double latencyP99Ms = 0.0;
        std::uint64_t latencySamples = 0;
    };
    Result Get() const noexcept;

private:
    struct Entry {
        std::uint32_t id = 0;
        std::int64_t qpc = 0;
        std::uint32_t planned = 0;
        std::uint32_t nominal = 0;
        bool used = false;
    };
    // One segment of the refresh fit (Welford's online covariance; x = refreshes, y = QPC ticks, both relative
    // to the first sample of the segment).
    struct Segment {
        std::uint64_t n = 0;
        std::uint32_t baseRefresh = 0;
        std::int64_t baseQpc = 0;
        double meanX = 0.0, meanY = 0.0, sxx = 0.0, sxy = 0.0;
        void Add(std::uint32_t refresh, std::int64_t qpc) noexcept;
    };
    static constexpr std::size_t kRing = 32;
    const Entry* Find(std::uint32_t id) const noexcept;
    void CloseSegment() noexcept;
    double qpf_ = 1.0e7;
    double nominalHz_ = 0.0;
    Entry ring_[kRing] = {};
    std::size_t next_ = 0;
    bool haveLast_ = false;
    std::uint32_t lastCount_ = 0;
    std::uint32_t lastRefresh_ = 0;
    std::int64_t lastQpc_ = 0;
    LastPair last_;
    std::uint64_t presents_ = 0, samples_ = 0, pairs_ = 0, invalid_ = 0;
    std::uint64_t durations_[kDurationBuckets] = {};
    std::uint64_t planned_ = 0, onPlan_ = 0, late_ = 0, early_ = 0;
    std::uint64_t jumps_ = 0, jumpPresents_ = 0, jumpsIrregular_ = 0;
    Segment segment_;
    double pooledSxx_ = 0.0, pooledSxy_ = 0.0;
    std::uint64_t fitSamples_ = 0, fitSegments_ = 0;
    std::uint32_t latency_[kLatencyBuckets] = {};
    std::uint64_t latencySamples_ = 0;
};

}  // namespace platform::pacing
