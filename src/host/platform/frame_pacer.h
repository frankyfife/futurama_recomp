// Frame pacing and presented frame rate (GFX_CONTRACT.md 8; M3b display-coupled pacing, analysis/loader/
// pacing60.md). Owned by the pacing agent (M3b; before: backend agent).
//
// The game runs its logic in fixed steps of dt = 1/[game] tick_rate (60 Hz by default since M3b: the NTSC
// timing values with PAL content, xapi/game_timing.h; 50 Hz = original PAL) and renders once per CApp::Frame
// without interpolation. The pacer decides per displayed frame how the picture reaches the display and when
// the next frame starts (pure policy: platform/pacer_policy.h; refresh detection: platform/pacer_display.h):
//
//   GpuRenderer::DisplayFrame (gfx/nipc_gpu.cpp, guest main thread = render thread)
//     NoteFrameWorkDone()                      end of the frame's own work (statistics)
//     PlanPresent(config, swapChain)           -> PresentRequest {sync interval, tearing, skip}; takes a free
//                                                 slot of the frame-latency waitable object without waiting
//     rhi::Device::Present(...)
//     NoteSwapChainPresent(feedback)           present id + DXGI frame statistics (what reached the screen)
//     NotePresentedFrame()                     QPC after Present: the "present:" rate statistics
//   OnFrameDisplayed (gfx/nipc_renderer.cpp, both backends)
//     EndFrame(config)                         replans if needed, then waits: vblank paths on the waitable
//                                              object (timeout 2 periods), timer paths on an absolute
//                                              high-resolution waitable timer; without a swap chain this
//                                              frame (null backend) the headless timer at the target rate
//
// Plans (pacing::Path, chosen by pacing::SelectPlan from [render] pacing, the refresh of the window's monitor,
// tearing support, the VRR probe and the window state): vblank k = Present(k) when refresh = k * target
// (k 1..4, 59.94/119.88 included); tearing-timer = timer + Present(0, ALLOW_TEARING) otherwise (VRR path,
// probed with the frame statistics); display-rate = Present(1), one game frame per refresh, when tearing is
// not available or VRR was rejected; free = timer + non-blocking Present(0) while the window is in the
// background, minimized, the display is off, the session is locked or the display holds the pace;
// timer/unthrottled = [render] pacing = timer/off; headless = null backend. Changes out of free need 30
// frames (hysteresis) unless presentation resumes (restored from minimized: at once); the window's monitor and
// the display epoch (platform/window.h) are polled every frame. Timer and free plans on a refresh of k x the
// target are watched by pacing::CadenceGuard (half-refresh shift of the timer grid, alternating in sign, when the
// on-screen durations keep missing k; a present that stayed visible for its target period - a VRR panel following
// the presents - counts as on time); the VRR probe starts with 2 s of warm-up and a rejection is probed once more
// after 10 s.
//
// Exit reporter "present" (registered by the first NotePresentedFrame, which also shows the main window -
// "visible from the first Present"), GFX/info. The first three lines keep their M3 format (tools/m3_check.py):
//   "present: N frames in T s (average A Hz, slowest 1 s window W Hz, longest frame L ms)"
//   "present: pacing P Hz, R schedule resets (more than one frame late), S frames longer than 1.5 periods"
//   "present: frame work N frames: average A ms, 99th percentile P ms, longest L ms (W frames longer than one
//    pacing period) -> sustainable S Hz"
// P = the target rate; R = timer re-anchors + vblank waits that timed out; S = frame START intervals longer than
// 1.5 planned periods (review M3b finding 8: the Present return times also contain the screenshot time and the
// work jitter of frames that still reached the screen on plan; their count follows in parentheses). M3b adds
// (formats in frame_pacer.cpp LogPresentStatistics):
//   "present: mode ..., plan ...; frames per plan: ...; N plan changes"
//   "present: display ... Hz (source), tearing ..., frame-latency waitable ..., VRR ..."
//   "present: frame start intervals (planned period): <buckets> -> X % on period; longest L ms"
//   "present: swap chain: N presents, S skipped (...), F forced; T vblank waits timed out, D display-limited ...;
//    timer cadence: J durations judged (F followed by the panel), H half-refresh shifts[ (guard stopped)];
//    V repeated VRR probes"
//   "present: on screen (DXGI frame statistics): ... refreshes per frame ...; J jumps over P presents (I off the
//    cadence); ... on plan ...; measured refresh ... Hz (fit over N samples, S segments); present to visible
//    median ... ms, 99th percentile ... ms"
//   "present: environment: foreground N frames (P %), background ..., minimized ..., display off ...,
//    session locked ..., display-limited ..."
// average = (N - 1) / T over first..last presented frame (QPC after Present); slowest 1 s window = the lowest
// rate over the frames of any trailing 1 s window after the first second (a window with a single frame - a gap
// longer than 1 s - counts as 1 / gap); longest frame = the longest interval. These three are Present-based: a
// screenshot frame (about 10 ms between NoteFrameWorkDone and Present) shows up in "longest frame" although it
// reached the screen on time; the on-screen line tells what the display showed. "frame work" = time from the end
// of the previous EndFrame/PaceFrame to NoteFrameWorkDone (guest logic + slot calls + D3D11 submission).
// [render] pacing_trace = <file> writes one CSV line per frame (columns in frame_pacer.cpp).
//
// Threading: PlanPresent, NoteSwapChainPresent, EndFrame, PaceFrame, NoteFrameWorkDone, NotePresentedFrame run
// on the render thread; GetPresentStatistics / LogPresentStatistics on any thread. No lock is held while waiting.
//
// M3d (frame rates up to 120 Hz; analysis/loader/m3d_frameclock.md 3; the frame-clock/pacing agent):
//   - PacingConfig::rateMode ([render] frame_rate = 120 | display): the plan's frame rate comes from the refresh
//     (pacing::SelectPlan, pacer_policy.h), e.g. vblank k = 1 at 120 fps on 120 Hz, the VRR timer at 120 on
//     144/165 Hz, the Cadence plan (Present(1)/Present(2) per frame) or an even R / k without VRR.
//   - CurrentFrameAdvance(): what the game's frame clock (xapi/game_timing.h FrameClock) advances for the next frame
//     (frames per tick, display time, the just-in-time delay). The clock reports the fixed steps of each rendered
//     frame back with NoteFrameTicks.
//   - Repeated picture (m3d_frameclock.md 1.3 F1): a frame that ran no fixed step and drew nothing (SwapChainInfo::
//     frameUnchanged; the intro draws its legal screen and tech notice in Update) is not presented when the frame rate
//     is above the tick rate - the previous picture stays one frame longer instead of the back buffer of two presents
//     ago (the tech notice flickered at 120 fps). Vblank plans then keep the frame grid on the timer.
//   - Just-in-time start (M3c review finding 6, PacingConfig::justInTime = [render] low_latency): on the vblank plans
//     the next frame starts pacing::LatencyBudget before the vblank at which it becomes visible (predicted from the
//     frame statistics) instead of right after the previous vblank: at 60 fps on 120 Hz about 12 ms less latency.
//   Exit line "present: frame rate ..." (LogPresentStatistics) after the M3b lines.
#pragma once

#include "platform/pacer_display.h"
#include "platform/pacer_policy.h"

#include <cstdint>
#include <deque>

namespace platform {

// ---- configuration (gfx::Settings -> gfx/nipc_gpu.cpp, gfx/nipc_renderer.cpp) ----------------------------------

struct PacingConfig {
    pacing::Mode mode = pacing::Mode::Auto;  // [render] pacing
    bool vsync = true;                       // [render] vsync
    double targetHz = 60.0;                  // game frames per second: [render] frame_rate, default the tick rate
                                             // (M3d modes: the cap, pacing::kMaxFrameRate)
    std::uint32_t tickHz = 60;               // [game] tick_rate (log; M3d: the frame clock's frames per tick)
    const wchar_t* tracePath = nullptr;      // [render] pacing_trace (nullptr or "" = off); must stay valid
    pacing::FrameRateMode rateMode = pacing::FrameRateMode::Fixed;  // M3d: [render] frame_rate = 120 | display
    bool justInTime = true;                  // M3d: [render] low_latency (just-in-time frame start)
};

// ---- per frame -----------------------------------------------------------------------------------------------

struct SwapChainInfo {
    void* frameLatencyWaitable = nullptr;  // rhi::Device::FrameLatencyWaitable (HANDLE)
    bool tearingSupported = false;         // rhi::DeviceInfo::tearingSupported
    // M3d: the frame drew nothing and cleared no color (its back buffer holds the picture of two presents ago); the
    // pacer may then keep the previous picture (PresentRequest::repeatPrevious).
    bool frameUnchanged = false;
};
struct PresentRequest {
    std::uint32_t syncInterval = 0;
    bool allowTearing = false;
    bool skipSwapChain = false;
    // M3d: skipSwapChain because the frame repeats the previous picture (F1); the back buffers rotate as for any
    // skipped frame.
    bool repeatPrevious = false;
};
// Before Present (render thread). Never waits.
PresentRequest PlanPresent(const PacingConfig& config, const SwapChainInfo& swapChain) noexcept;

struct PresentFeedback {
    std::int64_t qpcCall = 0;         // QPC right before / after rhi::Device::Present
    std::int64_t qpcReturn = 0;
    bool swapChainPresented = false;  // rhi::PresentTiming (copied by the caller)
    bool occluded = false;
    std::uint32_t presentId = 0;
    bool statsValid = false;
    std::uint32_t statPresentCount = 0;
    std::uint32_t statSyncRefreshCount = 0;
    std::int64_t statSyncQpc = 0;
};
// After Present (render thread).
void NoteSwapChainPresent(const PresentFeedback& feedback) noexcept;

// After the frame (render thread): replan and wait for the start of the next frame.
void EndFrame(const PacingConfig& config) noexcept;

// M2/M3 entry kept for tests: waits for the next 1/hz s slot on the absolute timer (hz = 0: returns at once).
// The game itself uses EndFrame.
void PaceFrame(std::uint32_t hz) noexcept;

// End of the frame's own work (before screenshots and Present; GpuRenderer::DisplayFrame). Guest main thread.
void NoteFrameWorkDone() noexcept;

// Records one presented frame (QPC time stamp) for the statistics below. Guest main thread.
void NotePresentedFrame() noexcept;

// M3d: the advance of the game's frame clock for the frame that starts now (planned by the last EndFrame;
// FrameAdvance::known = false before the first frame). Render thread (the guest main thread).
pacing::FrameAdvance CurrentFrameAdvance() noexcept;
// M3d: fixed steps the frame being rendered ran (the frame clock at its render sample). Frames that never report
// (context-switch black frames, movie ends) count as unknown. Render thread.
void NoteFrameTicks(std::uint32_t steps) noexcept;

struct PresentStatistics {
    std::uint64_t frames = 0;
    double seconds = 0.0;          // first to last presented frame
    double averageHz = 0.0;
    double slowestWindowHz = 0.0;  // lowest rate of any trailing 1 s window (0 = run shorter than 1 s)
    double longestFrameMs = 0.0;
    // Pacer details (second log line).
    std::uint32_t pacingHz = 0;          // target rate (rounded; PaceFrame: its argument)
    std::uint64_t scheduleResets = 0;    // timer re-anchors (more than one period late) + vblank wait timeouts
    std::uint64_t slowFrames = 0;        // frame start intervals longer than 1.5 planned periods (0 without pacing;
                                         // PresentStatsAccumulator::Result: Present intervals)
    std::uint64_t presentSlowFrames = 0; // Present intervals longer than 1.5 planned periods (incl. screenshots)
    double longestStartMs = 0.0;         // longest frame start interval
    // Frame work (third line; NoteFrameWorkDone).
    std::uint64_t workFrames = 0;
    double workAverageMs = 0.0;
    double workP99Ms = 0.0;              // 99th percentile (0.1 ms buckets up to 100 ms)
    double workLongestMs = 0.0;
    std::uint64_t workOverPeriod = 0;    // frames whose work alone exceeded one planned period
    // M3b display-coupled pacing.
    pacing::Mode mode = pacing::Mode::Auto;
    double targetHz = 0.0;
    std::uint32_t tickHz = 0;
    pacing::Plan plan;                                  // current plan
    std::uint64_t pathFrames[pacing::kPathCount] = {};  // EndFrame calls per plan path
    std::uint64_t planChanges = 0;
    DisplayTiming display;                              // of the window's monitor (valid = false: unknown)
    bool tearingSupported = false;
    bool frameLatencyWaitable = false;
    pacing::VrrState vrr = pacing::VrrState::Unknown;
    const char* vrrReason = "";
    std::uint64_t monitorChanges = 0;
    std::uint64_t displayEvents = 0;                    // display epoch changes seen (window.h)
    std::uint64_t timerResets = 0;
    std::uint64_t waitTimeouts = 0;                     // vblank waits that timed out
    std::uint64_t displayLimited = 0;                   // display-limited detections
    std::uint64_t swapChainPresents = 0;
    std::uint64_t skippedNoSlot = 0;                    // no free slot of the waitable object
    std::uint64_t skippedHidden = 0;                    // minimized / display off / locked
    std::uint64_t forcedPresents = 0;                   // presented without a slot after a long starvation
    std::uint64_t cadenceJudged = 0;                    // timer-plan durations judged by pacing::CadenceGuard
    std::uint64_t cadenceFollowed = 0;                  // ... off the refresh count but on time (VRR panel)
    std::uint64_t cadenceShifts = 0;                    // half-refresh shifts of the timer grid (alternating sign)
    bool cadenceStopped = false;                        // the guard stopped (kMaxShifts without a clean window)
    std::uint32_t vrrRetries = 0;                       // repeated VRR probes since the last display event
    std::uint64_t intervalBuckets[pacing::IntervalHistogram::kBuckets] = {};  // frame start intervals
    double onPeriodPercent = 0.0;
    pacing::OnScreenTracker::Result onScreen;
    std::uint64_t foregroundFrames = 0, backgroundFrames = 0, minimizedFrames = 0, displayOffFrames = 0,
                  lockedFrames = 0, limitedFrames = 0;
    // M3d.
    pacing::FrameRateMode rateMode = pacing::FrameRateMode::Fixed;
    pacing::FrameAdvance advance;                 // of the current plan (frames per tick, display time)
    std::uint64_t cadenceFrames[3] = {};          // Cadence plan: frames shown 1, 2, 3+ refreshes
    std::uint64_t repeatedFrames = 0;             // F1: frames without a tick and without a draw, not presented
    std::uint64_t ticklessFrames = 0;             // rendered frames that ran no fixed step (NoteFrameTicks 0)
    bool justInTime = false;                      // [render] low_latency
    std::uint64_t vblankFrames = 0;               // frames that waited on the frame-latency waitable
    std::uint64_t justInTimeFrames = 0;           // ... and then started just in time
    double justInTimeDelayMs = 0.0;               // average delay of those frames
    double latencyBudgetMs = 0.0;                 // the last budget
    std::uint64_t justInTimeMisses = 0;           // just-in-time frames that reached the screen late
};
PresentStatistics GetPresentStatistics() noexcept;  // thread-safe
void LogPresentStatistics() noexcept;               // thread-safe (exit reporter)

// Tests: forget the plan, the schedule and the statistics.
void ResetFramePacer() noexcept;

// The statistics computation (pure, tests): feed frame times in seconds (non-decreasing).
class PresentStatsAccumulator {
public:
    void Add(double seconds) noexcept;
    void SetSlowThreshold(double seconds) noexcept { slowThreshold_ = seconds; }
    PresentStatistics Result() const noexcept;

private:
    std::uint64_t frames_ = 0;
    double first_ = 0.0;
    double last_ = 0.0;
    double longest_ = 0.0;
    double slowestHz_ = 0.0;
    bool haveWindow_ = false;
    double slowThreshold_ = 0.0;  // 0 = do not count slow frames
    std::uint64_t slowFrames_ = 0;
    std::deque<double> window_;   // frame times of the trailing 1 s
};

}  // namespace platform
