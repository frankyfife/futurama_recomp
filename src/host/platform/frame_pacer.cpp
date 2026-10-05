// Frame pacing and presented frame rate (frame_pacer.h).
#include "platform/frame_pacer.h"

#include "core/host_main.h"
#include "core/log.h"
#include "platform/window.h"

#include <mutex>

namespace platform {
namespace {

std::mutex g_paceMutex;
LONGLONG g_nextFrame = 0;
std::uint32_t g_pacingHz = 0;
std::uint64_t g_scheduleResets = 0;

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
void PaceFrame(std::uint32_t hz) noexcept {
    if (hz == 0) {
        return;
    }
    static HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::lock_guard<std::mutex> lock(g_paceMutex);
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
    return s;
}

void LogPresentStatistics() noexcept {
    const PresentStatistics s = GetPresentStatistics();
    FUT_LOGI(Gfx, "present: %llu frames in %.2f s (average %.2f Hz, slowest 1 s window %.2f Hz, longest frame %.1f ms)",
             static_cast<unsigned long long>(s.frames), s.seconds, s.averageHz, s.slowestWindowHz, s.longestFrameMs);
    FUT_LOGI(Gfx, "present: pacing %u Hz, %llu schedule resets (more than one frame late), %llu frames longer than "
             "1.5 periods", s.pacingHz, static_cast<unsigned long long>(s.scheduleResets),
             static_cast<unsigned long long>(s.slowFrames));
}

void ResetFramePacer() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_paceMutex);
        g_nextFrame = 0;
        g_pacingHz = 0;
        g_scheduleResets = 0;
    }
    std::lock_guard<std::mutex> lock(g_statsMutex);
    g_stats = PresentStatsAccumulator{};
    g_firstPresent = true;
    g_origin = 0;
}

}  // namespace platform
