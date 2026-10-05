// Frame pacing and presented frame rate (frame_pacer.h).
#include "platform/frame_pacer.h"

#include "core/host_main.h"
#include "core/log.h"
#include "platform/window.h"

#include <algorithm>
#include <iterator>
#include <mutex>

namespace platform {
namespace {

std::mutex g_paceMutex;
LONGLONG g_nextFrame = 0;
std::uint32_t g_pacingHz = 0;
std::uint64_t g_scheduleResets = 0;
LONGLONG g_paceEnd = 0;  // QPC when the last PaceFrame returned (start of the next frame's work)

// Frame work histogram: 0.1 ms buckets up to 100 ms, the last bucket collects everything longer.
constexpr std::uint32_t kWorkBuckets = 1001;
std::uint32_t g_workHistogram[kWorkBuckets] = {};
std::uint64_t g_workFrames = 0;
double g_workSumMs = 0.0;
double g_workLongestMs = 0.0;
std::uint64_t g_workOverPeriod = 0;

std::mutex g_statsMutex;
PresentStatsAccumulator g_stats;
LONGLONG g_origin = 0;
bool g_firstPresent = true;

double QpcFrequency() noexcept {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return frequency;
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

// ---- pacing --------------------------------------------------------------------------------------------------

// Waits for the next 1/hz s slot like D3DDevice_Swap waits for the vertical blank (PresentationInterval
// ONE from FUN_000D26A0). Falls back to a new schedule when the game is more than one frame late (the M2
// algorithm of gfx/nipc_renderer.cpp, unchanged).
namespace {

void PaceFrameLocked(std::uint32_t hz) noexcept {
    static HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const LONGLONG period = freq.QuadPart / hz;
    g_pacingHz = hz;
    if (g_nextFrame == 0 || now.QuadPart - g_nextFrame > period) {
        if (g_nextFrame != 0) {
            ++g_scheduleResets;
        }
        g_nextFrame = now.QuadPart + period;
        return;
    }
    if (now.QuadPart < g_nextFrame) {
        const LONGLONG ticks = g_nextFrame - now.QuadPart;
        LARGE_INTEGER due;
        due.QuadPart = -(ticks * 10000000 / freq.QuadPart);
        if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(static_cast<DWORD>(ticks * 1000 / freq.QuadPart));
        }
    }
    g_nextFrame += period;
}

}  // namespace

void PaceFrame(std::uint32_t hz) noexcept {
    std::lock_guard<std::mutex> lock(g_paceMutex);
    if (hz != 0) {
        PaceFrameLocked(hz);
    }
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    g_paceEnd = end.QuadPart;
}

void NoteFrameWorkDone() noexcept {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard<std::mutex> lock(g_paceMutex);
    if (g_paceEnd == 0) {
        return;  // the first frame has no defined start; one sample per frame
    }
    const double ms = static_cast<double>(now.QuadPart - g_paceEnd) * 1000.0 / QpcFrequency();
    const auto bucket = static_cast<std::uint32_t>(ms * 10.0);
    ++g_workHistogram[bucket < kWorkBuckets ? bucket : kWorkBuckets - 1];
    ++g_workFrames;
    g_workSumMs += ms;
    g_workLongestMs = ms > g_workLongestMs ? ms : g_workLongestMs;
    if (g_pacingHz != 0 && ms > 1000.0 / g_pacingHz) {
        ++g_workOverPeriod;
    }
    g_paceEnd = 0;
}

// ---- statistics ----------------------------------------------------------------------------------------------

void NotePresentedFrame() noexcept {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        if (g_firstPresent) {
            g_firstPresent = false;
            first = true;
            g_origin = now.QuadPart;
        }
        std::uint32_t hz;
        {
            std::lock_guard<std::mutex> paceLock(g_paceMutex);
            hz = g_pacingHz;
        }
        g_stats.SetSlowThreshold(hz != 0 ? 1.5 / hz : 0.0);
        g_stats.Add(static_cast<double>(now.QuadPart - g_origin) / QpcFrequency());
    }
    if (first) {
        ShowMainWindow();
        core::AddExitReporter("present", &LogPresentStatistics);
    }
}

PresentStatistics GetPresentStatistics() noexcept {
    PresentStatistics s;
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        s = g_stats.Result();
    }
    std::lock_guard<std::mutex> lock(g_paceMutex);
    s.pacingHz = g_pacingHz;
    s.scheduleResets = g_scheduleResets;
    s.workFrames = g_workFrames;
    s.workAverageMs = g_workFrames != 0 ? g_workSumMs / static_cast<double>(g_workFrames) : 0.0;
    s.workLongestMs = g_workLongestMs;
    s.workOverPeriod = g_workOverPeriod;
    // Upper edge of the bucket in which 99 % of the frames are reached.
    const std::uint64_t target = (g_workFrames * 99 + 99) / 100;
    std::uint64_t seen = 0;
    for (std::uint32_t b = 0; b < kWorkBuckets && g_workFrames != 0; ++b) {
        seen += g_workHistogram[b];
        if (seen >= target) {
            s.workP99Ms = b + 1 < kWorkBuckets ? (b + 1) / 10.0 : g_workLongestMs;
            break;
        }
    }
    return s;
}

void LogPresentStatistics() noexcept {
    const PresentStatistics s = GetPresentStatistics();
    FUT_LOGI(Gfx, "present: %llu frames in %.2f s (average %.2f Hz, slowest 1 s window %.2f Hz, longest frame %.1f ms)",
             static_cast<unsigned long long>(s.frames), s.seconds, s.averageHz, s.slowestWindowHz, s.longestFrameMs);
    FUT_LOGI(Gfx, "present: pacing %u Hz, %llu schedule resets (more than one frame late), %llu frames longer than "
             "1.5 periods", s.pacingHz, static_cast<unsigned long long>(s.scheduleResets),
             static_cast<unsigned long long>(s.slowFrames));
    FUT_LOGI(Gfx, "present: frame work %llu frames: average %.2f ms, 99th percentile %.1f ms, longest %.1f ms (%llu "
             "frames longer than one pacing period) -> sustainable %.0f Hz",
             static_cast<unsigned long long>(s.workFrames), s.workAverageMs, s.workP99Ms, s.workLongestMs,
             static_cast<unsigned long long>(s.workOverPeriod), s.workAverageMs > 0.0 ? 1000.0 / s.workAverageMs : 0.0);
}

void ResetFramePacer() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_paceMutex);
        g_nextFrame = 0;
        g_pacingHz = 0;
        g_scheduleResets = 0;
        g_paceEnd = 0;
        std::fill(std::begin(g_workHistogram), std::end(g_workHistogram), 0u);
        g_workFrames = 0;
        g_workSumMs = 0.0;
        g_workLongestMs = 0.0;
        g_workOverPeriod = 0;
    }
    std::lock_guard<std::mutex> lock(g_statsMutex);
    g_stats = PresentStatsAccumulator{};
    g_firstPresent = true;
    g_origin = 0;
}

}  // namespace platform
