// Frame pacing and presented frame rate (frame_pacer.h).
#include "platform/frame_pacer.h"

#include "core/host_main.h"
#include "core/log.h"
#include "platform/window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>

namespace platform {

using pacing::Environment;
using pacing::FrameSchedule;
using pacing::Mode;
using pacing::Path;
using pacing::Plan;
using pacing::VrrState;

namespace {

double QpcFrequency() noexcept {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return frequency;
}

LONGLONG Qpc() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double Seconds(LONGLONG ticks) noexcept {
    return static_cast<double>(ticks) / QpcFrequency();
}

// Frame work histogram: 0.1 ms buckets up to 100 ms, the last bucket collects everything longer.
constexpr std::uint32_t kWorkBuckets = 1001;
// Frames without a free slot of the waitable object before a Present goes out anyway (a slot lost to an
// occluded or failed Present must not freeze the window; such a Present blocks at most until a vblank).
constexpr std::uint32_t kForcePresentAfter = 30;
// Plan changes logged at GFX/info (then GFX/debug; all are counted).
constexpr std::uint64_t kMaxPlanLogs = 24;
// VRR probe: no samples during the first 2 s of a probe (review M3b finding 13a); a rejected probe is repeated
// once, 10 s after the rejection, until the next display event.
constexpr double kVrrWarmupSeconds = 2.0;
constexpr double kVrrRetrySeconds = 10.0;
// Cadence guard shifts of the timer grid logged at GFX/info (then GFX/debug).
constexpr std::uint64_t kMaxCadenceLogs = 8;
// M3d just-in-time start: shorter waits are not worth a timer (the frame starts at once).
constexpr double kMinJustInTimeWaitSeconds = 0.0003;
// Present ids of the frames that started just in time (miss detection; a sample reports a present 1-2 frames later).
constexpr std::uint32_t kJitIdRing = 8;

// One line of [render] pacing_trace.
struct TraceRecord {
    LONGLONG frameStart = 0;
    LONGLONG workDone = 0;
    LONGLONG presentCall = 0;
    LONGLONG presentReturn = 0;
    std::uint32_t sync = 0;
    bool planned = false;  // PlanPresent ran this frame
    bool tearing = false;
    bool skip = false;
    bool presented = false;
    std::uint32_t presentId = 0;
    bool statsValid = false;
    std::uint32_t statPresentCount = 0;
    std::uint32_t statSyncRefresh = 0;
    LONGLONG statSyncQpc = 0;
    // M3d (columns appended after the M3b ones).
    int frameTicks = -1;  // fixed steps of the frame (NoteFrameTicks), -1 = unknown
    bool repeat = false;  // F1: not presented, the previous picture stays
    std::uint32_t refreshes = 0;  // vblank plans: refreshes the frame stays on screen (Cadence: 1 or 2)
};

struct WaitAction {
    enum Kind : std::uint8_t { None, Timer, Vblank } kind = None;
    LONGLONG until = 0;       // Timer; Vblank: first finish the timer period up to here (0 = no)
    HANDLE handle = nullptr;  // Vblank
    DWORD timeoutMs = 0;
};

struct PacerState {
    // Configuration (ApplyConfigLocked).
    bool configured = false;
    Mode mode = Mode::Auto;
    bool vsync = true;
    double targetHz = 60.0;
    std::uint32_t tickHz = 60;
    std::uint32_t legacyHz = 0;  // PaceFrame argument (tests)
    // Plan.
    Plan plan;
    pacing::PlanHysteresis hysteresis{30};
    FrameSchedule schedule;
    pacing::DisplayLimitDetector limiter;
    pacing::VrrDetector vrr;
    VrrState vrrLogged = VrrState::Unknown;
    LONGLONG vrrRetryAt = 0;      // QPC of the one repeated probe after a rejection (0 = none pending)
    std::uint32_t vrrRetries = 0;  // repeated probes since the last display event
    pacing::CadenceGuard cadence;  // timer plans on an integer refresh (review M3b finding 6)
    std::uint64_t cadenceLogs = 0;
    bool forceReplan = true;
    // Display and window state.
    DisplayTiming display;
    bool haveDisplay = false;
    HMONITOR monitor = nullptr;
    std::uint32_t epoch = 0;
    bool haveEpoch = false;
    Environment env;
    LONGLONG limitedUntil = 0;
    // Swap chain (PlanPresent).
    HANDLE waitable = nullptr;
    bool tearing = false;
    bool swapChainThisFrame = false;
    bool slotHeld = false;  // one signal of the waitable object taken and not yet used by a Present
    std::uint32_t noSlotStreak = 0;
    // Frame boundaries.
    LONGLONG paceEnd = 0;         // end of the last EndFrame/PaceFrame (start of the next frame's work)
    LONGLONG lastFrameStart = 0;  // for the interval histogram
    LONGLONG vblankNotBefore = 0; // first vblank wait after a timer plan: finish the timer period first
    // Counters.
    std::uint64_t pathFrames[pacing::kPathCount] = {};
    std::uint64_t planChanges = 0, planLogs = 0, monitorChanges = 0, displayEvents = 0;
    std::uint64_t waitTimeouts = 0, displayLimited = 0;
    std::uint64_t swapChainPresents = 0, skippedNoSlot = 0, skippedHidden = 0, forcedPresents = 0;
    std::uint64_t foreground = 0, background = 0, minimized = 0, displayOff = 0, locked = 0, limited = 0;
    pacing::IntervalHistogram intervals;
    std::uint64_t slowStarts = 0;  // frame start intervals longer than 1.5 planned periods (review M3b finding 8)
    double longestStartMs = 0.0;
    pacing::OnScreenTracker onScreen;
    // Frame work.
    std::uint32_t workHistogram[kWorkBuckets] = {};
    std::uint64_t workFrames = 0;
    double workSumMs = 0.0;
    double workLongestMs = 0.0;
    std::uint64_t workOverPeriod = 0;
    // Trace.
    std::FILE* trace = nullptr;
    bool traceTried = false;
    std::uint64_t traceFrames = 0;
    TraceRecord record;
    // M3d.
    pacing::FrameRateMode rateMode = pacing::FrameRateMode::Fixed;
    bool justInTime = true;
    pacing::CadencePattern cadencePattern;
    std::uint32_t frameSync = 0;  // refreshes the frame of this DisplayFrame stays on screen (vblank plans; 0 = none)
    bool frameTicksKnown = false;
    std::uint32_t frameTicks = 0;
    bool repeatThisFrame = false;
    pacing::FrameAdvance advance;  // for the frame that starts after EndFrame
    pacing::LatencyBudget latency;
    LONGLONG jitDelay = 0;         // QPC ticks the current frame's start was delayed (just in time)
    bool jitThisFrame = false;     // the frame being rendered started just in time
    std::uint32_t jitIds[kJitIdRing] = {};
    std::uint32_t jitIdNext = 0;
    std::uint64_t cadenceFrames[3] = {};
    std::uint64_t repeated = 0, tickless = 0, vblankFrames = 0, jitFrames = 0, jitMisses = 0;
    double jitDelaySumMs = 0.0;
    double lastBudgetMs = 0.0;
};

std::mutex g_paceMutex;  // guards g_state
PacerState g_state;

std::mutex g_statsMutex;  // guards the members below; taken before g_paceMutex when both are needed
PresentStatsAccumulator g_stats;
LONGLONG g_origin = 0;
bool g_firstPresent = true;

// ---- helpers (g_paceMutex held) ---------------------------------------------------------------------------------

void DescribePlan(const Plan& p, double refreshHz, char* out, std::size_t size) noexcept {
    const double hz = p.periodSeconds > 0.0 ? 1.0 / p.periodSeconds : 0.0;
    switch (p.path) {
    case Path::VblankLocked:
        std::snprintf(out, size, "vblank k=%u (Present(%u) on the frame-latency waitable, %.3f Hz)", p.k,
                      p.syncInterval, refreshHz > 0.0 ? refreshHz / p.k : hz);
        break;
    case Path::DisplayRate:
        std::snprintf(out, size, "display-rate (Present(1) on the frame-latency waitable, %.3f Hz)", hz);
        break;
    case Path::TearingTimer:
        std::snprintf(out, size, "tearing-timer (%.3f Hz timer, Present(0, ALLOW_TEARING))", hz);
        break;
    case Path::Timer:
        if (p.presentToSwapChain) {
            std::snprintf(out, size, "timer (%.3f Hz timer, Present(0%s) into free slots)", hz,
                          p.allowTearing ? ", ALLOW_TEARING" : "");
        } else {
            std::snprintf(out, size, "timer (%.3f Hz timer, no Present)", hz);  // minimized, display off, locked
        }
        break;
    case Path::Free:
        std::snprintf(out, size, "free (%.3f Hz timer, %s)", hz,
                      p.presentToSwapChain ? "Present(0) into free slots" : "no Present");
        break;
    case Path::Unthrottled:
        if (p.presentToSwapChain) {
            std::snprintf(out, size, "unthrottled (no wait, Present(0%s) into free slots)",
                          p.allowTearing ? ", ALLOW_TEARING" : "");
        } else {
            std::snprintf(out, size, "unthrottled (no wait, no Present)");
        }
        break;
    case Path::Headless:
        std::snprintf(out, size, "headless (%.3f Hz timer, no swap chain)", hz);
        break;
    case Path::Cadence:
        std::snprintf(out, size,
                      "cadence (Present(1)/Present(2) on the frame-latency waitable, %.3f Hz on average, %.3f "
                      "refreshes per frame)",
                      hz, p.cadenceRatio);
        break;
    default:
        std::snprintf(out, size, "%s", pacing::PathName(p.path));
        break;
    }
}

void FlushTrace() noexcept;

void OpenTraceLocked(PacerState& s, const wchar_t* path) noexcept {
    s.traceTried = true;
    if (path == nullptr || path[0] == 0) {
        return;
    }
    s.trace = _wfopen(path, L"w");
    if (s.trace == nullptr) {
        FUT_LOGW(Gfx, "pacing: [render] pacing_trace: cannot create the file - no trace");
        return;
    }
    std::setvbuf(s.trace, nullptr, _IOFBF, 1u << 16);
    std::fprintf(s.trace, "# futurama pacing trace: qpf=%.0f target_hz=%.6f tick_hz=%u mode=%s frame_rate=%s\n",
                 QpcFrequency(), s.targetHz, s.tickHz, pacing::ModeName(s.mode), pacing::FrameRateModeName(s.rateMode));
    std::fprintf(s.trace, "frame,path,sync,tearing,skip,presented,frame_start_qpc,work_done_qpc,present_call_qpc,"
                          "present_return_qpc,wait,wait_ms,timed_out,present_id,stats_valid,stat_present_count,"
                          "stat_sync_refresh,stat_sync_qpc,foreground,minimized,display_on,locked,limited,"
                          "frame_ticks,repeat,refreshes,jit_ms\n");
    FUT_LOGI(Gfx, "pacing: per-frame trace enabled ([render] pacing_trace)");
    // The exit path does not flush CRT streams of the host DLL; the null backend has no "present" reporter.
    core::AddExitReporter("pacing trace", &FlushTrace);
}

void ApplyConfigLocked(PacerState& s, const PacingConfig& c) noexcept {
    const double target = c.targetHz > 0.0 ? c.targetHz : 60.0;
    if (!s.configured || c.mode != s.mode || c.vsync != s.vsync || std::fabs(target - s.targetHz) > 1.0e-9 ||
        c.tickHz != s.tickHz || c.rateMode != s.rateMode || c.justInTime != s.justInTime) {
        s.configured = true;
        s.mode = c.mode;
        s.vsync = c.vsync;
        s.targetHz = target;
        s.tickHz = c.tickHz;
        s.rateMode = c.rateMode;
        s.justInTime = c.justInTime;
        s.forceReplan = true;
    }
    s.legacyHz = 0;
    if (!s.traceTried) {
        OpenTraceLocked(s, c.tracePath);
    }
}

void RefreshDisplayLocked(PacerState& s, HMONITOR monitor) noexcept {
    const DisplayTiming t = QueryDisplayTiming(monitor);
    const bool changed = !s.haveDisplay || t.valid != s.display.valid || t.numerator != s.display.numerator ||
                         t.denominator != s.display.denominator || std::strcmp(t.device, s.display.device) != 0;
    s.display = t;
    s.haveDisplay = true;
    s.onScreen.SetNominalRefreshHz(t.valid ? t.hz : 0.0);
    s.vrr.Stop();  // a new display (or window mode) is probed again, with one repeat after a rejection
    s.vrrLogged = VrrState::Unknown;
    s.vrrRetryAt = 0;
    s.vrrRetries = 0;
    s.cadence.Reset();
    s.forceReplan = true;
    if (!changed) {
        return;
    }
    if (t.valid) {
        FUT_LOGI(Gfx, "pacing: display %s \"%s\" %u/%u Hz = %.3f Hz (%s); refresh / target %.3f", t.device, t.name,
                 t.numerator, t.denominator, t.hz, t.source, t.hz / s.targetHz);
    } else {
        FUT_LOGI(Gfx, "pacing: display %s \"%s\": refresh rate unknown", t.device, t.name);
    }
}

void UpdateEnvironmentLocked(PacerState& s, bool swapChain, LONGLONG now) noexcept {
    Environment e;
    e.swapChain = swapChain;
    const HWND frame = MainFrameWindow();
    if (swapChain && frame != nullptr) {
        e.foreground = GetForegroundWindow() == frame;
        e.minimized = IsIconic(frame) != FALSE;
        const DisplayEnvironment de = GetDisplayEnvironment();
        e.displayOn = de.displayOn;
        e.sessionLocked = de.sessionLocked;
        const HMONITOR monitor = MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST);
        const bool monitorChanged = s.haveDisplay && monitor != s.monitor;
        const bool epochChanged = s.haveEpoch && de.epoch != s.epoch;
        if (!s.haveDisplay || monitorChanged || epochChanged) {
            s.monitorChanges += monitorChanged ? 1 : 0;
            s.displayEvents += epochChanged ? 1 : 0;
            s.monitor = monitor;
            s.epoch = de.epoch;
            s.haveEpoch = true;
            RefreshDisplayLocked(s, monitor);
        }
    } else if (swapChain && !s.haveDisplay) {
        RefreshDisplayLocked(s, nullptr);  // a swap chain without the main window (tests): primary monitor
    }
    e.displayLimited = s.mode == Mode::Auto && s.limitedUntil != 0 && now < s.limitedUntil;
    s.env = e;
}

void ApplyPlanLocked(PacerState& s, const Plan& p) noexcept {
    const Plan old = s.plan;
    if (old.path != Path::None) {
        ++s.planChanges;
    }
    // Plan changes keep the frame grid: a timer plan continues from the last frame start, the first vblank wait
    // after a timer plan begins when the timer period is over (else one frame would be short).
    s.vblankNotBefore = 0;
    if (pacing::IsTimerPath(p.path) && !pacing::IsTimerPath(old.path)) {
        if (s.lastFrameStart != 0) {
            s.schedule.Restart(s.lastFrameStart, p.periodSeconds * QpcFrequency());
        } else {
            s.schedule.Reset();
        }
    } else if (pacing::IsVblankPath(p.path) && pacing::IsTimerPath(old.path) && s.lastFrameStart != 0) {
        s.vblankNotBefore = s.lastFrameStart + static_cast<LONGLONG>(old.periodSeconds * QpcFrequency());
    }
    if (p.path == Path::TearingTimer && s.vrr.State() == VrrState::Unknown) {
        // M3d: the probe expects the plan's own period (120 Hz with [render] frame_rate = 120 | display).
        const double planHz = p.periodSeconds > 0.0 ? 1.0 / p.periodSeconds : s.targetHz;
        const auto warmup = static_cast<std::uint32_t>(std::ceil(kVrrWarmupSeconds * planHz));
        s.vrr.Start(planHz, s.display.valid ? s.display.hz : 0.0, warmup);
    } else if (p.path != Path::TearingTimer && s.vrr.State() == VrrState::Probing) {
        s.vrr.Stop();  // probe again when the path comes back
        s.vrrLogged = VrrState::Unknown;
    }
    if (p.path == Path::Cadence && (old.path != Path::Cadence || std::fabs(old.cadenceRatio - p.cadenceRatio) > 1.0e-9)) {
        s.cadencePattern.Reset(p.cadenceRatio);
    }
    s.limiter.ResetWindow();
    s.cadence.Reset();
    s.plan = p;
    char text[160];
    DescribePlan(p, s.display.valid ? s.display.hz : 0.0, text, sizeof(text));
    if (s.planLogs++ < kMaxPlanLogs) {
        FUT_LOGI(Gfx, "pacing: %s -> %s: %s", pacing::PathName(old.path), text, p.reason);
    } else {
        FUT_LOGD(Gfx, "pacing: %s -> %s: %s", pacing::PathName(old.path), text, p.reason);
    }
}

void HandleVrrLocked(PacerState& s, LONGLONG now) noexcept {
    const VrrState st = s.vrr.State();
    if (st != s.vrrLogged) {
        s.vrrLogged = st;
        if (st == VrrState::Confirmed || st == VrrState::Rejected) {
            FUT_LOGI(Gfx, "pacing: VRR %s: %s (%.0f %% of the visible intervals at the target period)%s",
                     pacing::VrrStateName(st), s.vrr.Reason(), s.vrr.MatchFraction() * 100.0,
                     st == VrrState::Rejected && s.vrrRetries == 0 ? "; probing once more in 10 s" : "");
            s.forceReplan = st == VrrState::Rejected;
            if (st == VrrState::Rejected && s.vrrRetries == 0) {
                s.vrrRetryAt = now + static_cast<LONGLONG>(kVrrRetrySeconds * QpcFrequency());
            }
        }
    }
    // One repeated probe per display event (review M3b finding 13a: a rejection right after a mode change must
    // not stand until the next event).
    if (s.vrrRetryAt != 0 && now >= s.vrrRetryAt) {
        s.vrrRetryAt = 0;
        if (s.vrr.State() == VrrState::Rejected && s.vrrRetries == 0) {
            ++s.vrrRetries;
            s.vrr.Stop();
            s.vrrLogged = VrrState::Unknown;
            s.forceReplan = true;
            FUT_LOGI(Gfx, "pacing: VRR probe repeated once (%.0f s after the rejection)", kVrrRetrySeconds);
        }
    }
}

void ReplanLocked(PacerState& s, bool immediate) noexcept {
    pacing::PlanInput in;
    in.mode = s.mode;
    in.vsync = s.vsync;
    in.targetHz = s.targetHz;
    in.refreshHz = s.display.valid ? s.display.hz : 0.0;
    in.tearingSupported = s.tearing && s.env.swapChain;
    in.waitable = s.waitable != nullptr;
    in.vrr = s.vrr.State();
    in.env = s.env;
    in.rateMode = s.rateMode;
    const Plan desired = pacing::SelectPlan(in);
    const bool force = immediate || s.forceReplan;
    s.forceReplan = false;
    if (s.hysteresis.Accept(s.plan, desired, force)) {
        ApplyPlanLocked(s, desired);
    }
}

void CountFrameLocked(PacerState& s) noexcept {
    ++s.pathFrames[static_cast<std::size_t>(s.plan.path) < pacing::kPathCount ? static_cast<std::size_t>(s.plan.path)
                                                                                : 0];
    const Environment& e = s.env;
    if (!e.swapChain) {
        return;
    }
    if (e.minimized) {
        ++s.minimized;
    } else if (!e.displayOn) {
        ++s.displayOff;
    } else if (e.sessionLocked) {
        ++s.locked;
    } else if (!e.foreground) {
        ++s.background;
    } else {
        ++s.foreground;
    }
    s.limited += e.displayLimited ? 1 : 0;
}

// Display time of the frame of this DisplayFrame on a vblank plan (its refreshes x the refresh period).
double FrameDisplaySecondsLocked(const PacerState& s) noexcept {
    if (s.frameSync != 0 && s.display.valid && s.display.hz > 0.0) {
        return static_cast<double>(s.frameSync) / s.display.hz;
    }
    return s.plan.periodSeconds;
}

WaitAction PrepareWaitLocked(PacerState& s, LONGLONG now) noexcept {
    WaitAction a;
    const Plan& p = s.plan;
    if (pacing::IsVblankPath(p.path) && s.repeatThisFrame && s.lastFrameStart != 0) {
        // M3d F1: the frame was not presented, so the waitable object releases nothing for it; the next frame
        // starts one display time after this one (same phase to its vblank as this frame had to its own).
        a.kind = WaitAction::Timer;
        a.until = s.lastFrameStart + static_cast<LONGLONG>(FrameDisplaySecondsLocked(s) * QpcFrequency());
        s.vblankNotBefore = 0;
    } else if (pacing::IsVblankPath(p.path)) {
        // Without a waitable object Present(k) itself blocked (flip queue); a slot that is already held (the
        // last Present was skipped) needs no wait either.
        if (s.waitable != nullptr && !s.slotHeld && p.presentToSwapChain) {
            a.kind = WaitAction::Vblank;
            a.handle = s.waitable;
            a.timeoutMs = pacing::VblankWaitTimeoutMs(p.periodSeconds);
            a.until = s.vblankNotBefore > now ? s.vblankNotBefore : 0;
        }
        s.vblankNotBefore = 0;
    } else if (pacing::IsTimerPath(p.path) && p.periodSeconds > 0.0) {
        const FrameSchedule::Step step = s.schedule.Next(now, p.periodSeconds * QpcFrequency());
        if (step.waitUntil > now) {
            a.kind = WaitAction::Timer;
            a.until = step.waitUntil;
        }
    }
    return a;
}

// Absolute wait on a high-resolution waitable timer (Windows 10 1803+; else a normal one, else Sleep).
void WaitUntilQpc(LONGLONG until) noexcept {
    static const HANDLE timer = [] {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (h == nullptr) {
            h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        return h;
    }();
    const LONGLONG now = Qpc();
    if (until <= now) {
        return;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER due;
    due.QuadPart = pacing::RelativeDueTime100ns(until - now, freq.QuadPart);
    if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
    } else {
        Sleep(static_cast<DWORD>((until - now) * 1000 / freq.QuadPart));
    }
}

DWORD PerformWait(const WaitAction& a) noexcept {
    switch (a.kind) {
    case WaitAction::Timer:
        WaitUntilQpc(a.until);
        return WAIT_OBJECT_0;
    case WaitAction::Vblank:
        return WaitForSingleObject(a.handle, a.timeoutMs);  // a.until: EndFrame waits for it first
    case WaitAction::None:
        break;
    }
    return WAIT_OBJECT_0;
}

void FinishWaitLocked(PacerState& s, const WaitAction& a, DWORD result, LONGLONG start, LONGLONG end) noexcept {
    if (a.kind != WaitAction::Vblank || a.handle != s.waitable) {
        return;
    }
    const bool signaled = result == WAIT_OBJECT_0;
    if (signaled) {
        s.slotHeld = true;
    } else {
        ++s.waitTimeouts;
    }
    const double seconds = Seconds(end - start);
    if (s.mode == Mode::Auto && s.limiter.AddWait(seconds, s.plan.periodSeconds, !signaled)) {
        const double hold = s.limiter.FireHoldSeconds(Seconds(end));
        s.limitedUntil = end + static_cast<LONGLONG>(hold * QpcFrequency());
        ++s.displayLimited;
        s.forceReplan = true;
        FUT_LOGI(Gfx, "pacing: vblank waits longer than 1.5 periods in 3 of 8 frames (last %.1f ms%s, period %.2f ms) "
                 "- the display holds the pace; free-running for %.0f s", seconds * 1000.0,
                 signaled ? "" : ", timed out", s.plan.periodSeconds * 1000.0, hold);
    }
}

void WriteTraceLocked(PacerState& s, const WaitAction& a, DWORD result, LONGLONG start, LONGLONG end) noexcept {
    if (s.trace == nullptr) {
        return;
    }
    const TraceRecord& r = s.record;
    const char* wait = a.kind == WaitAction::Timer ? "timer" : a.kind == WaitAction::Vblank ? "vblank" : "none";
    std::fprintf(s.trace,
                 "%llu,%s,%u,%d,%d,%d,%lld,%lld,%lld,%lld,%s,%.3f,%d,%u,%d,%u,%u,%lld,%d,%d,%d,%d,%d,%d,%d,%u,%.3f\n",
                 static_cast<unsigned long long>(++s.traceFrames), pacing::PathName(s.plan.path), r.sync,
                 r.tearing ? 1 : 0, r.skip ? 1 : 0, r.presented ? 1 : 0, static_cast<long long>(r.frameStart),
                 static_cast<long long>(r.workDone), static_cast<long long>(r.presentCall),
                 static_cast<long long>(r.presentReturn), wait, Seconds(end - start) * 1000.0,
                 a.kind == WaitAction::Vblank && result != WAIT_OBJECT_0 ? 1 : 0, r.presentId, r.statsValid ? 1 : 0,
                 r.statPresentCount, r.statSyncRefresh, static_cast<long long>(r.statSyncQpc),
                 s.env.foreground ? 1 : 0, s.env.minimized ? 1 : 0, s.env.displayOn ? 1 : 0,
                 s.env.sessionLocked ? 1 : 0, s.env.displayLimited ? 1 : 0, r.frameTicks, r.repeat ? 1 : 0,
                 r.refreshes, s.jitThisFrame ? Seconds(s.jitDelay) * 1000.0 : 0.0);
}

bool IsJustInTimePresentLocked(const PacerState& s, std::uint32_t id) noexcept {
    if (id == 0) {
        return false;
    }
    for (std::uint32_t i = 0; i < kJitIdRing; ++i) {
        if (s.jitIds[i] == id) {
            return true;
        }
    }
    return false;
}

// M3d just-in-time start (pacer_policy.h LatencyBudget): after the vblank wait of a vblank plan, the start of the
// next frame (QPC) `budget` before the vblank at which its present becomes visible; 0 = start at once. Sets the
// delay the frame clock subtracts (FrameAdvance::startDelayTicks).
LONGLONG JustInTimeLocked(PacerState& s, const WaitAction& a, DWORD result, LONGLONG end) noexcept {
    if (s.repeatThisFrame) {
        // The next frame starts on the grid of this one: the same delay as this frame (no new prediction).
        s.jitThisFrame = s.jitDelay != 0;
        return 0;
    }
    s.jitThisFrame = false;
    s.jitDelay = 0;
    if (a.kind != WaitAction::Vblank) {
        return 0;
    }
    ++s.vblankFrames;
    const Plan& p = s.plan;
    if (!s.justInTime || result != WAIT_OBJECT_0 || (p.path != Path::VblankLocked && p.path != Path::DisplayRate) ||
        !s.display.valid || !(s.display.hz > 0.0) || !p.presentToSwapChain) {
        return 0;
    }
    std::int64_t visible = 0;
    if (!s.onScreen.PredictNextVisible(1.0 / s.display.hz, &visible)) {
        return 0;
    }
    const double budget = s.latency.Seconds(Seconds(end));
    s.lastBudgetMs = budget * 1000.0;
    const auto budgetTicks = static_cast<std::int64_t>(budget * QpcFrequency());
    const auto maxWait = static_cast<std::int64_t>(p.periodSeconds * QpcFrequency());
    const std::int64_t start = pacing::JustInTimeStart(end, visible, budgetTicks, maxWait);
    if (start == 0 || Seconds(start - end) < kMinJustInTimeWaitSeconds) {
        return 0;
    }
    s.jitThisFrame = true;
    s.jitDelay = start - end;
    ++s.jitFrames;
    s.jitDelaySumMs += Seconds(s.jitDelay) * 1000.0;
    return start;
}

// The frame boundary: interval statistics, start of the next frame's work, next trace record.
void FrameStartLocked(PacerState& s, LONGLONG at) noexcept {
    if (s.lastFrameStart != 0 && at > s.lastFrameStart) {
        const double seconds = Seconds(at - s.lastFrameStart);
        s.intervals.Add(seconds, s.plan.periodSeconds);
        // Slow frames from the frame starts (review M3b finding 8): the Present return times also contain the
        // screenshot time and the work jitter of frames that still reached the screen on plan.
        if (s.plan.path != Path::Unthrottled && s.plan.periodSeconds > 0.0 && seconds > 1.5 * s.plan.periodSeconds) {
            ++s.slowStarts;
        }
        s.longestStartMs = std::max(s.longestStartMs, seconds * 1000.0);
    }
    s.lastFrameStart = at;
    s.paceEnd = at;
    s.record = TraceRecord{};
    s.record.frameStart = at;
}

void FlushTrace() noexcept {
    std::lock_guard<std::mutex> lock(g_paceMutex);
    if (g_state.trace != nullptr) {
        std::fflush(g_state.trace);
    }
}

}  // namespace

// ---- PresentStatsAccumulator -----------------------------------------------------------------------------

void PresentStatsAccumulator::Add(double t) noexcept {
    try {
        if (frames_ == 0) {
            first_ = t;
        } else {
            const double interval = t - last_;
            if (interval > longest_) {
                longest_ = interval;
            }
            if (slowThreshold_ > 0.0 && interval > slowThreshold_) {
                ++slowFrames_;
            }
        }
        const double previous = last_;
        last_ = t;
        ++frames_;
        window_.push_back(t);
        while (!window_.empty() && t - window_.front() > 1.0) {
            window_.pop_front();
        }
        if (frames_ < 2 || t - first_ < 1.0) {
            return;  // the first second has no full window
        }
        double hz;
        if (window_.size() >= 2) {
            hz = static_cast<double>(window_.size() - 1) / (t - window_.front());
        } else {
            hz = t > previous ? 1.0 / (t - previous) : 0.0;  // a gap longer than 1 s
        }
        if (!haveWindow_ || hz < slowestHz_) {
            slowestHz_ = hz;
            haveWindow_ = true;
        }
    } catch (...) {
        // std::deque allocation failure: the window statistics stay as they are.
    }
}

PresentStatistics PresentStatsAccumulator::Result() const noexcept {
    PresentStatistics s;
    s.frames = frames_;
    s.seconds = frames_ >= 2 ? last_ - first_ : 0.0;
    s.averageHz = s.seconds > 0.0 ? static_cast<double>(frames_ - 1) / s.seconds : 0.0;
    s.slowestWindowHz = haveWindow_ ? slowestHz_ : 0.0;
    s.longestFrameMs = longest_ * 1000.0;
    s.slowFrames = slowFrames_;
    return s;
}

// ---- per frame -------------------------------------------------------------------------------------------------

PresentRequest PlanPresent(const PacingConfig& config, const SwapChainInfo& swapChain) noexcept {
    std::lock_guard<std::mutex> lock(g_paceMutex);
    PacerState& s = g_state;
    ApplyConfigLocked(s, config);
    s.swapChainThisFrame = true;
    const HANDLE waitable = static_cast<HANDLE>(swapChain.frameLatencyWaitable);
    if (waitable != s.waitable || swapChain.tearingSupported != s.tearing) {
        s.waitable = waitable;
        s.tearing = swapChain.tearingSupported;
        s.slotHeld = false;
        s.noSlotStreak = 0;
        s.forceReplan = true;
    }
    if (s.forceReplan || !s.env.swapChain || s.plan.path == Path::None || s.plan.path == Path::Headless) {
        UpdateEnvironmentLocked(s, true, Qpc());
        ReplanLocked(s, true);
    }
    const Plan& p = s.plan;
    PresentRequest r;
    r.syncInterval = p.syncInterval;
    if (p.path == Path::Cadence) {
        // M3d: 1 or 2 refreshes for this frame (the flip model shows a frame for at least its sync interval).
        r.syncInterval = std::min(s.cadencePattern.Next(), pacing::kMaxSyncInterval);
    }
    s.frameSync = pacing::IsVblankPath(p.path) ? r.syncInterval : 0u;
    r.allowTearing = p.allowTearing && s.tearing && p.syncInterval == 0;
    // M3d F1 (m3d_frameclock.md 1.3/4.2): above the tick rate, a frame without a fixed step that drew nothing and
    // cleared no color would show the back buffer of two presents ago (intro legal screen / tech notice: drawn in
    // Update, so every second frame at 120 fps). Keep the previous picture instead (not at 60 fps: bit-identical).
    const bool repeat = swapChain.frameUnchanged && p.presentToSwapChain && s.frameTicksKnown && s.frameTicks == 0 &&
                        s.advance.known && s.advance.framesPerTick != 1 && s.swapChainPresents != 0;
    if (repeat) {
        r.skipSwapChain = true;
        r.repeatPrevious = true;
        s.repeatThisFrame = true;
        ++s.repeated;
    } else if (!p.presentToSwapChain) {
        r.skipSwapChain = true;
        ++s.skippedHidden;
    } else if (s.waitable != nullptr) {
        if (!s.slotHeld && WaitForSingleObject(s.waitable, 0) == WAIT_OBJECT_0) {
            s.slotHeld = true;
        }
        if (!s.slotHeld) {
            if (++s.noSlotStreak >= kForcePresentAfter) {
                s.noSlotStreak = 0;
                if (s.forcedPresents++ == 0) {
                    FUT_LOGI(Gfx, "pacing: no free swap-chain slot for %u frames - presenting without one",
                             kForcePresentAfter);
                }
            } else {
                r.skipSwapChain = true;
                ++s.skippedNoSlot;
            }
        }
    }
    s.record.planned = true;
    s.record.sync = r.syncInterval;
    s.record.tearing = r.allowTearing;
    s.record.skip = r.skipSwapChain;
    s.record.repeat = r.repeatPrevious;
    s.record.refreshes = s.frameSync;
    return r;
}

// Refreshes per frame the timer plans should show (k for a timer at the target rate on a refresh of k x the
// target, no tearing); 0 = no cadence to judge (vblank plans have their own plan value, VRR/tearing, 50 on 120 Hz).
std::uint32_t TimerCadenceLocked(const PacerState& s) noexcept {
    const Plan& p = s.plan;
    if ((p.path != Path::Timer && p.path != Path::Free) || p.allowTearing || !p.presentToSwapChain ||
        !s.display.valid || !(p.periodSeconds > 0.0)) {
        return 0;
    }
    const pacing::RefreshClass rc = pacing::ClassifyRefresh(s.display.hz, 1.0 / p.periodSeconds);
    return rc.integer ? rc.k : 0;
}

void NoteSwapChainPresent(const PresentFeedback& fb) noexcept {
    std::lock_guard<std::mutex> lock(g_paceMutex);
    PacerState& s = g_state;
    s.onScreen.SetQpcFrequency(QpcFrequency());
    const std::uint32_t cadence = TimerCadenceLocked(s);
    if (fb.swapChainPresented) {
        ++s.swapChainPresents;
        if (!fb.occluded) {
            s.slotHeld = false;  // the Present used the slot; an occluded one may not have queued a frame
        }
        s.noSlotStreak = 0;
        // M3d: the frame's own refresh count (the Cadence plan changes it per frame).
        s.onScreen.NotePresent(fb.presentId, fb.qpcCall, pacing::IsVblankPath(s.plan.path) ? s.frameSync : 0u, cadence);
        if (s.jitThisFrame) {
            s.jitIds[s.jitIdNext++ % kJitIdRing] = fb.presentId;
        }
        if (s.plan.path == Path::Cadence && s.frameSync != 0) {
            ++s.cadenceFrames[std::min<std::uint32_t>(s.frameSync, 3) - 1];
        }
    } else if (s.repeatThisFrame && s.frameSync != 0) {
        s.onScreen.ExtendLastPresent(s.frameSync);  // the previous picture stays this frame's refreshes longer
    }
    if (fb.statsValid) {
        const double interval = s.onScreen.NoteStatistics(fb.statPresentCount, fb.statSyncRefreshCount, fb.statSyncQpc);
        if (interval >= 0.0 && s.plan.path == Path::TearingTimer) {
            s.vrr.AddInterval(interval);
        }
        // M3d: a frame that started just in time and reached the screen late doubles the latency budget.
        if (interval >= 0.0) {
            const pacing::OnScreenTracker::LastPair pair = s.onScreen.Last();
            if (pair.valid && pair.expected != 0 && pair.refreshes > pair.expected &&
                IsJustInTimePresentLocked(s, fb.statPresentCount)) {
                ++s.jitMisses;
                s.latency.NoteMiss(Seconds(fb.qpcReturn));
            }
        }
        // Timer plans: move the grid by half a refresh when the presents keep missing the k-refresh cadence (a
        // present that was visible for its target period although not k fixed refreshes - VRR - counts as on time).
        const pacing::OnScreenTracker::LastPair last = s.onScreen.Last();
        if (cadence != 0 && last.valid) {
            const double refreshSeconds = 1.0 / s.display.hz;
            const bool wasStopped = s.cadence.Stopped();
            const int shift = s.cadence.AddDuration(last.refreshes, last.expected, last.seconds, refreshSeconds);
            if (shift != 0) {
                const double halfRefresh = 0.5 * refreshSeconds;
                s.schedule.Shift(static_cast<std::int64_t>(shift) *
                                 static_cast<std::int64_t>(std::llround(halfRefresh * QpcFrequency())));
                if (s.cadenceLogs++ < kMaxCadenceLogs) {
                    FUT_LOGI(Gfx, "pacing: timer cadence: %u on-screen durations within %u frames differed from %u "
                             "refreshes - timer grid shifted by %+.2f ms (half a refresh)",
                             pacing::CadenceGuard::kIrregularLimit, pacing::CadenceGuard::kWindow, cadence,
                             shift * halfRefresh * 1000.0);
                } else {
                    FUT_LOGD(Gfx, "pacing: timer cadence: timer grid shifted by %+.2f ms", shift * halfRefresh * 1000.0);
                }
            } else if (!wasStopped && s.cadence.Stopped()) {
                FUT_LOGI(Gfx, "pacing: timer cadence: still irregular after %u shifts - the guard keeps the grid until "
                         "the next plan change or display event", pacing::CadenceGuard::kMaxShifts);
            }
        }
    } else if (fb.swapChainPresented) {
        s.onScreen.NoteInvalid();
    }
    if (s.plan.path == Path::TearingTimer) {
        s.vrr.AddFrame();
    }
    TraceRecord& r = s.record;
    r.presentCall = fb.qpcCall;
    r.presentReturn = fb.qpcReturn;
    r.presented = fb.swapChainPresented;
    r.presentId = fb.presentId;
    r.statsValid = fb.statsValid;
    r.statPresentCount = fb.statPresentCount;
    r.statSyncRefresh = fb.statSyncRefreshCount;
    r.statSyncQpc = fb.statSyncQpc;
}

void EndFrame(const PacingConfig& config) noexcept {
    const LONGLONG now = Qpc();
    WaitAction action;
    {
        std::lock_guard<std::mutex> lock(g_paceMutex);
        PacerState& s = g_state;
        ApplyConfigLocked(s, config);
        const bool swapChain = s.swapChainThisFrame;
        s.swapChainThisFrame = false;
        if (!swapChain) {
            s.waitable = nullptr;
            s.slotHeld = false;
        }
        UpdateEnvironmentLocked(s, swapChain, now);
        HandleVrrLocked(s, now);
        ReplanLocked(s, false);
        CountFrameLocked(s);
        action = PrepareWaitLocked(s, now);
    }
    if (action.kind == WaitAction::Vblank && action.until != 0) {
        WaitUntilQpc(action.until);  // finish the timer period of the previous plan (not part of the vblank wait)
    }
    const LONGLONG start = Qpc();
    const DWORD result = PerformWait(action);
    const LONGLONG end = Qpc();
    LONGLONG justInTime = 0;
    {
        std::lock_guard<std::mutex> lock(g_paceMutex);
        PacerState& s = g_state;
        FinishWaitLocked(s, action, result, start, end);
        justInTime = JustInTimeLocked(s, action, result, end);
    }
    if (justInTime != 0) {
        WaitUntilQpc(justInTime);  // M3d: start the next frame shortly before its vblank (latency)
    }
    const LONGLONG begin = justInTime != 0 ? Qpc() : end;
    std::lock_guard<std::mutex> lock(g_paceMutex);
    PacerState& s = g_state;
    WriteTraceLocked(s, action, result, start, end);
    FrameStartLocked(s, begin);
    // M3d: the frame clock's advance for the frame that starts now.
    s.advance = pacing::FrameAdvanceOf(s.plan, static_cast<double>(s.tickHz), s.display.valid ? s.display.hz : 0.0,
                                       s.frameSync);
    s.advance.startDelayTicks = s.jitDelay;
    s.frameTicksKnown = false;
    s.repeatThisFrame = false;
    s.frameSync = 0;
}

void PaceFrame(std::uint32_t hz) noexcept {
    const LONGLONG now = Qpc();
    WaitAction action;
    {
        std::lock_guard<std::mutex> lock(g_paceMutex);
        PacerState& s = g_state;
        s.legacyHz = hz;
        Plan p;
        p.path = hz != 0 ? Path::Headless : Path::Unthrottled;
        p.presentToSwapChain = false;
        p.periodSeconds = hz != 0 ? 1.0 / hz : 0.0;
        p.reason = "PaceFrame";
        s.plan = p;
        CountFrameLocked(s);
        action = PrepareWaitLocked(s, now);
    }
    PerformWait(action);
    std::lock_guard<std::mutex> lock(g_paceMutex);
    FrameStartLocked(g_state, Qpc());
}

void NoteFrameWorkDone() noexcept {
    const LONGLONG now = Qpc();
    std::lock_guard<std::mutex> lock(g_paceMutex);
    PacerState& s = g_state;
    if (s.paceEnd == 0) {
        return;  // the first frame has no defined start; one sample per frame
    }
    const double ms = Seconds(now - s.paceEnd) * 1000.0;
    const auto bucket = static_cast<std::uint32_t>(ms * 10.0);
    ++s.workHistogram[bucket < kWorkBuckets ? bucket : kWorkBuckets - 1];
    ++s.workFrames;
    s.workSumMs += ms;
    s.workLongestMs = ms > s.workLongestMs ? ms : s.workLongestMs;
    if (s.plan.path != Path::Unthrottled && s.plan.periodSeconds > 0.0 && ms > s.plan.periodSeconds * 1000.0) {
        ++s.workOverPeriod;
    }
    s.latency.AddWork(ms / 1000.0);  // M3d: the just-in-time budget follows the frame work
    s.paceEnd = 0;
    s.record.workDone = now;
}

pacing::FrameAdvance CurrentFrameAdvance() noexcept {
    std::lock_guard<std::mutex> lock(g_paceMutex);
    return g_state.advance;
}

void NoteFrameTicks(std::uint32_t steps) noexcept {
    std::lock_guard<std::mutex> lock(g_paceMutex);
    PacerState& s = g_state;
    s.frameTicksKnown = true;
    s.frameTicks = steps;
    s.tickless += steps == 0 ? 1 : 0;
    s.record.frameTicks = static_cast<int>(std::min<std::uint32_t>(steps, 1000));
}

// ---- statistics ----------------------------------------------------------------------------------------------

void NotePresentedFrame() noexcept {
    const LONGLONG now = Qpc();
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        if (g_firstPresent) {
            g_firstPresent = false;
            first = true;
            g_origin = now;
        }
        double threshold = 0.0;
        {
            std::lock_guard<std::mutex> paceLock(g_paceMutex);
            const Plan& p = g_state.plan;
            if (p.path != Path::None && p.path != Path::Unthrottled && p.periodSeconds > 0.0) {
                threshold = 1.5 * p.periodSeconds;
            }
        }
        g_stats.SetSlowThreshold(threshold);
        g_stats.Add(Seconds(now - g_origin));
    }
    if (first) {
        ShowMainWindow();
        core::AddExitReporter("present", &LogPresentStatistics);
    }
}

PresentStatistics GetPresentStatistics() noexcept {
    PresentStatistics r;
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        r = g_stats.Result();
    }
    std::lock_guard<std::mutex> lock(g_paceMutex);
    const PacerState& s = g_state;
    // Slow frames from the frame starts (review M3b finding 8); the Present-based count stays available.
    r.presentSlowFrames = r.slowFrames;
    r.slowFrames = s.slowStarts;
    r.longestStartMs = s.longestStartMs;
    r.cadenceShifts = s.cadence.Shifts();
    r.cadenceJudged = s.cadence.Judged();
    r.cadenceFollowed = s.cadence.Followed();
    r.cadenceStopped = s.cadence.Stopped();
    r.vrrRetries = s.vrrRetries;
    // 0 = unthrottled (pacing = off / frame_rate = 0), as PaceFrame(0) in M2/M3. M3d modes: the plan's own rate.
    const double effectiveHz =
        s.rateMode != pacing::FrameRateMode::Fixed && s.plan.nominalHz > 0.0 ? s.plan.nominalHz : s.targetHz;
    r.pacingHz = s.legacyHz != 0                                  ? s.legacyHz
                 : !s.configured || s.mode == Mode::Off           ? 0u
                                                                  : static_cast<std::uint32_t>(std::lround(effectiveHz));
    r.timerResets = s.schedule.Resets();
    r.waitTimeouts = s.waitTimeouts;
    r.scheduleResets = r.timerResets + r.waitTimeouts;
    r.workFrames = s.workFrames;
    r.workAverageMs = s.workFrames != 0 ? s.workSumMs / static_cast<double>(s.workFrames) : 0.0;
    r.workLongestMs = s.workLongestMs;
    r.workOverPeriod = s.workOverPeriod;
    // Upper edge of the bucket in which 99 % of the frames are reached.
    const std::uint64_t target = (s.workFrames * 99 + 99) / 100;
    std::uint64_t seen = 0;
    for (std::uint32_t b = 0; b < kWorkBuckets && s.workFrames != 0; ++b) {
        seen += s.workHistogram[b];
        if (seen >= target) {
            r.workP99Ms = b + 1 < kWorkBuckets ? (b + 1) / 10.0 : s.workLongestMs;
            break;
        }
    }
    r.mode = s.mode;
    r.targetHz = effectiveHz;
    r.tickHz = s.tickHz;
    r.plan = s.plan;
    std::copy(std::begin(s.pathFrames), std::end(s.pathFrames), std::begin(r.pathFrames));
    r.planChanges = s.planChanges;
    r.display = s.display;
    r.tearingSupported = s.tearing;
    r.frameLatencyWaitable = s.waitable != nullptr;
    r.vrr = s.vrr.State();
    r.vrrReason = s.vrr.Reason();
    r.monitorChanges = s.monitorChanges;
    r.displayEvents = s.displayEvents;
    r.displayLimited = s.displayLimited;
    r.swapChainPresents = s.swapChainPresents;
    r.skippedNoSlot = s.skippedNoSlot;
    r.skippedHidden = s.skippedHidden;
    r.forcedPresents = s.forcedPresents;
    for (std::size_t b = 0; b < pacing::IntervalHistogram::kBuckets; ++b) {
        r.intervalBuckets[b] = s.intervals.Count(b);
    }
    r.onPeriodPercent = s.intervals.OnPeriodPercent();
    r.onScreen = s.onScreen.Get();
    r.foregroundFrames = s.foreground;
    r.backgroundFrames = s.background;
    r.minimizedFrames = s.minimized;
    r.displayOffFrames = s.displayOff;
    r.lockedFrames = s.locked;
    r.limitedFrames = s.limited;
    // M3d.
    r.rateMode = s.rateMode;
    r.advance = s.advance;
    std::copy(std::begin(s.cadenceFrames), std::end(s.cadenceFrames), std::begin(r.cadenceFrames));
    r.repeatedFrames = s.repeated;
    r.ticklessFrames = s.tickless;
    r.justInTime = s.justInTime;
    r.vblankFrames = s.vblankFrames;
    r.justInTimeFrames = s.jitFrames;
    r.justInTimeDelayMs = s.jitFrames != 0 ? s.jitDelaySumMs / static_cast<double>(s.jitFrames) : 0.0;
    r.latencyBudgetMs = s.lastBudgetMs;
    r.justInTimeMisses = s.jitMisses;
    return r;
}

void LogPresentStatistics() noexcept {
    using ull = unsigned long long;
    const PresentStatistics s = GetPresentStatistics();
    FUT_LOGI(Gfx, "present: %llu frames in %.2f s (average %.2f Hz, slowest 1 s window %.2f Hz, longest frame %.1f ms)",
             static_cast<ull>(s.frames), s.seconds, s.averageHz, s.slowestWindowHz, s.longestFrameMs);
    FUT_LOGI(Gfx, "present: pacing %u Hz, %llu schedule resets (more than one frame late), %llu frames longer than "
             "1.5 periods (frame starts; %llu Present intervals incl. screenshots)", s.pacingHz,
             static_cast<ull>(s.scheduleResets), static_cast<ull>(s.slowFrames), static_cast<ull>(s.presentSlowFrames));
    FUT_LOGI(Gfx, "present: frame work %llu frames: average %.2f ms, 99th percentile %.1f ms, longest %.1f ms (%llu "
             "frames longer than one pacing period) -> sustainable %.0f Hz",
             static_cast<ull>(s.workFrames), s.workAverageMs, s.workP99Ms, s.workLongestMs,
             static_cast<ull>(s.workOverPeriod), s.workAverageMs > 0.0 ? 1000.0 / s.workAverageMs : 0.0);
    // M3b lines.
    char plan[160];
    DescribePlan(s.plan, s.display.valid ? s.display.hz : 0.0, plan, sizeof(plan));
    char perPath[256] = {};
    std::size_t used = 0;
    for (std::size_t p = 0; p < pacing::kPathCount; ++p) {
        if (s.pathFrames[p] == 0 || used >= sizeof(perPath)) {
            continue;
        }
        const int n = std::snprintf(perPath + used, sizeof(perPath) - used, "%s%s %llu", used != 0 ? ", " : "",
                                    pacing::PathName(static_cast<Path>(p)), static_cast<ull>(s.pathFrames[p]));
        used += n > 0 ? static_cast<std::size_t>(n) : 0;
    }
    FUT_LOGI(Gfx, "present: mode %s (tick %u Hz, target %.3f Hz), plan %s; frames per plan: %s; %llu plan changes",
             pacing::ModeName(s.mode), s.tickHz, s.targetHz, plan, used != 0 ? perPath : "none",
             static_cast<ull>(s.planChanges));
    if (s.display.valid) {
        FUT_LOGI(Gfx, "present: display %s \"%s\" %u/%u Hz = %.3f Hz (%s), tearing %s, frame-latency waitable %s, VRR "
                 "%s%s%s; %llu monitor changes, %llu display events",
                 s.display.device, s.display.name, s.display.numerator, s.display.denominator, s.display.hz,
                 s.display.source, s.tearingSupported ? "supported" : "not supported",
                 s.frameLatencyWaitable ? "yes" : "no", pacing::VrrStateName(s.vrr), s.vrrReason[0] != 0 ? ": " : "",
                 s.vrrReason, static_cast<ull>(s.monitorChanges), static_cast<ull>(s.displayEvents));
    } else {
        FUT_LOGI(Gfx, "present: display %s, refresh rate unknown, tearing %s, frame-latency waitable %s, VRR %s",
                 s.display.device[0] != 0 ? s.display.device : "none", s.tearingSupported ? "supported" : "not supported",
                 s.frameLatencyWaitable ? "yes" : "no", pacing::VrrStateName(s.vrr));
    }
    const std::uint64_t* b = s.intervalBuckets;
    FUT_LOGI(Gfx, "present: frame start intervals (planned period): <0.5: %llu, 0.5-0.85: %llu, 0.85-0.95: %llu, "
             "0.95-1.05: %llu, 1.05-1.5: %llu, 1.5-2.5: %llu, 2.5-4: %llu, >=4: %llu -> %.2f %% on period; longest "
             "%.1f ms",
             static_cast<ull>(b[0]), static_cast<ull>(b[1]), static_cast<ull>(b[2]), static_cast<ull>(b[3]),
             static_cast<ull>(b[4]), static_cast<ull>(b[5]), static_cast<ull>(b[6]), static_cast<ull>(b[7]),
             s.onPeriodPercent, s.longestStartMs);
    FUT_LOGI(Gfx, "present: swap chain: %llu presents, %llu skipped (%llu without a free slot, %llu hidden), %llu "
             "forced; %llu vblank waits timed out, %llu timer re-anchors, %llu display-limited detections; timer "
             "cadence: %llu durations judged (%llu followed by the panel), %llu half-refresh shifts%s; %u repeated VRR "
             "probes",
             static_cast<ull>(s.swapChainPresents), static_cast<ull>(s.skippedNoSlot + s.skippedHidden),
             static_cast<ull>(s.skippedNoSlot), static_cast<ull>(s.skippedHidden), static_cast<ull>(s.forcedPresents),
             static_cast<ull>(s.waitTimeouts), static_cast<ull>(s.timerResets), static_cast<ull>(s.displayLimited),
             static_cast<ull>(s.cadenceJudged), static_cast<ull>(s.cadenceFollowed), static_cast<ull>(s.cadenceShifts),
             s.cadenceStopped ? " (guard stopped)" : "", s.vrrRetries);
    const pacing::OnScreenTracker::Result& o = s.onScreen;
    if (o.samples != 0) {
        FUT_LOGI(Gfx, "present: on screen (DXGI frame statistics): %llu visible presents, %llu consecutive pairs, "
                 "%llu invalid samples; refreshes per frame 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu; %llu jumps over "
                 "%llu presents (%llu off the cadence); vblank-coupled %llu: %llu on plan (%.2f %%), %llu late, %llu "
                 "early; measured refresh %.3f Hz (fit over %llu samples, %llu segments); present to visible median "
                 "%.1f ms, 99th percentile %.1f ms",
                 static_cast<ull>(o.samples), static_cast<ull>(o.pairs), static_cast<ull>(o.invalid),
                 static_cast<ull>(o.durations[0]), static_cast<ull>(o.durations[1]), static_cast<ull>(o.durations[2]),
                 static_cast<ull>(o.durations[3]), static_cast<ull>(o.durations[4]), static_cast<ull>(o.durations[5]),
                 static_cast<ull>(o.jumps), static_cast<ull>(o.jumpPresents), static_cast<ull>(o.jumpsIrregular),
                 static_cast<ull>(o.planned), static_cast<ull>(o.onPlan),
                 o.planned != 0 ? 100.0 * static_cast<double>(o.onPlan) / static_cast<double>(o.planned) : 0.0,
                 static_cast<ull>(o.late), static_cast<ull>(o.early), o.measuredHz, static_cast<ull>(o.refreshSamples),
                 static_cast<ull>(o.refreshSegments), o.latencyMedianMs, o.latencyP99Ms);
    } else {
        FUT_LOGI(Gfx, "present: on screen (DXGI frame statistics): no samples (%llu invalid; headless, occluded or "
                 "never presented)", static_cast<ull>(o.invalid));
    }
    const std::uint64_t windowFrames =
        s.foregroundFrames + s.backgroundFrames + s.minimizedFrames + s.displayOffFrames + s.lockedFrames;
    FUT_LOGI(Gfx, "present: environment: foreground %llu frames (%.1f %%), background %llu, minimized %llu, display "
             "off %llu, session locked %llu, display-limited %llu",
             static_cast<ull>(s.foregroundFrames),
             windowFrames != 0 ? 100.0 * static_cast<double>(s.foregroundFrames) / static_cast<double>(windowFrames)
                               : 0.0,
             static_cast<ull>(s.backgroundFrames), static_cast<ull>(s.minimizedFrames),
             static_cast<ull>(s.displayOffFrames), static_cast<ull>(s.lockedFrames), static_cast<ull>(s.limitedFrames));
    // M3d line (formats documented in frame_pacer.h).
    char perTick[64];
    if (!s.advance.known) {
        std::snprintf(perTick, sizeof(perTick), "no plan");
    } else if (s.advance.framesPerTick != 0) {
        std::snprintf(perTick, sizeof(perTick), "%u frame%s per tick", s.advance.framesPerTick,
                      s.advance.framesPerTick == 1 ? "" : "s");
    } else {
        std::snprintf(perTick, sizeof(perTick), "%.3f frames per tick (clock in display time)",
                      s.tickHz != 0 && s.advance.nominalSeconds > 0.0
                          ? 1.0 / (s.advance.nominalSeconds * static_cast<double>(s.tickHz))
                          : 0.0);
    }
    FUT_LOGI(Gfx, "present: frame rate %s (plan %.3f Hz, %s); cadence frames 1:%llu 2:%llu 3+:%llu; repeated pictures "
             "%llu (of %llu frames without a fixed step); just-in-time start %s: %llu of %llu vblank frames (average "
             "delay %.1f ms, budget %.1f ms, %llu late)",
             pacing::FrameRateModeName(s.rateMode), s.targetHz, perTick, static_cast<ull>(s.cadenceFrames[0]),
             static_cast<ull>(s.cadenceFrames[1]), static_cast<ull>(s.cadenceFrames[2]),
             static_cast<ull>(s.repeatedFrames), static_cast<ull>(s.ticklessFrames), s.justInTime ? "on" : "off",
             static_cast<ull>(s.justInTimeFrames), static_cast<ull>(s.vblankFrames), s.justInTimeDelayMs,
             s.latencyBudgetMs, static_cast<ull>(s.justInTimeMisses));
    FlushTrace();
}

void ResetFramePacer() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_paceMutex);
        if (g_state.trace != nullptr) {
            std::fclose(g_state.trace);
        }
        g_state = PacerState{};
    }
    std::lock_guard<std::mutex> lock(g_statsMutex);
    g_stats = PresentStatsAccumulator{};
    g_firstPresent = true;
    g_origin = 0;
}

}  // namespace platform
