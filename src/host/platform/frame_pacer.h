// Frame pacing and presented frame rate (GFX_CONTRACT.md 8). Owned by the backend agent.
//
// The Xbox game runs its logic in fixed 1/50 s steps (PAL, CApp+0xE8 = 0) and D3DDevice_Swap waits for the
// vertical blank (Create gets PresentationInterval ONE). On the PC, Present runs with VSync ([render]
// vsync = 1, usually 60 Hz) and the 50 Hz limiter ([render] frame_rate = 50) keeps the guest at its
// native rate. M2 had the limiter inside gfx/nipc_renderer.cpp (PaceFrame); the integration moves the
// call site to this API (same algorithm: waitable high-resolution timer, a new schedule when more than one
// frame late).
//
// Exit criterion 3 of M3 ("presented frame rate >= 50") is read from the lines LogPresentStatistics logs
// (GFX/info). The first NotePresentedFrame registers it as the exit reporter "present" (core::AddExitReporter)
// and shows the main window (platform::ShowMainWindow - "visible from the first Present"):
//   "present: N frames in T s (average A Hz, slowest 1 s window W Hz, longest frame L ms)"
//   "present: pacing P Hz, R schedule resets (more than one frame late), S frames longer than 1.5 periods"
// average = (N - 1) / T over first..last presented frame; slowest 1 s window = the lowest rate
// (intervals / span) over the frames of any trailing 1 s window after the first second (a window with a
// single frame - a gap longer than 1 s - counts as 1 / gap); longest frame = the longest interval.
#pragma once

#include <cstdint>
#include <deque>

namespace platform {

// Waits for the next 1/hz slot (hz = 0: returns at once). Call once per displayed frame, after Present.
void PaceFrame(std::uint32_t hz) noexcept;

// Records one presented frame (QPC time stamp) for the statistics below. Guest main thread.
void NotePresentedFrame() noexcept;

struct PresentStatistics {
    std::uint64_t frames = 0;
    double seconds = 0.0;          // first to last presented frame
    double averageHz = 0.0;
    double slowestWindowHz = 0.0;  // lowest rate of any trailing 1 s window (0 = run shorter than 1 s)
    double longestFrameMs = 0.0;
    // Pacer details (second log line).
    std::uint32_t pacingHz = 0;          // last PaceFrame argument
    std::uint64_t scheduleResets = 0;    // PaceFrame started a new schedule (more than one frame late)
    std::uint64_t slowFrames = 0;        // intervals longer than 1.5 pacing periods (0 without pacing)
};
PresentStatistics GetPresentStatistics() noexcept;  // thread-safe
void LogPresentStatistics() noexcept;               // thread-safe (exit reporter)

// Tests: forget the pacing schedule and the statistics.
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
