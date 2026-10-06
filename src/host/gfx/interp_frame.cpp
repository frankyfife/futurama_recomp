// FrameInterp source registration, stub and the FUT_INTERP_STUB developer source (see interp_frame.h).
#include "gfx/interp_frame.h"

#include "xapi/game_probes.h"
#include "xapi/game_timing.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gfx::interp {
namespace {

std::atomic<FrameInterpSource> g_source{nullptr};

// FUT_INTERP_STUB=steps (read once).
bool StepsStubRequested() noexcept {
    static const bool requested = [] {
        char value[16] = {};
        std::size_t length = 0;
        if (getenv_s(&length, value, sizeof(value), "FUT_INTERP_STUB") != 0 || length == 0) {
            return false;
        }
        return _stricmp(value, "steps") == 0;
    }();
    return requested;
}

struct StepsState {
    std::mutex lock;
    bool started = false;
    std::uint64_t frame = ~0ull;  // completed CApp::Frame count the cached value belongs to
    FrameInterp value;
};
StepsState g_steps;

FrameInterp StepsSource() noexcept {
    try {
        const std::uint64_t frame = xapi::GetGameObserverStats().frames;
        std::lock_guard<std::mutex> guard(g_steps.lock);
        if (g_steps.started && frame == g_steps.frame) {
            return g_steps.value;
        }
        const std::uint64_t steps = xapi::GetGameTickStats().steps;
        if (!g_steps.started) {
            g_steps.value.tick = steps;
            g_steps.value.alpha = 1.0f;
            g_steps.value.flags = kFrameInterpSnapAll;
            g_steps.value.enabled = true;
            g_steps.started = true;
        } else {
            g_steps.value = StepsFrameInterp(g_steps.value, steps);
        }
        g_steps.frame = frame;
        return g_steps.value;
    } catch (...) {
        return FrameInterp{};
    }
}

}  // namespace

FrameInterp StepsFrameInterp(const FrameInterp& previous, std::uint64_t steps) noexcept {
    FrameInterp f;
    f.enabled = true;
    f.tick = steps;
    const std::uint64_t delta = steps >= previous.tick ? steps - previous.tick : 0;
    f.stepsThisFrame = static_cast<std::uint32_t>(delta > 0xFFFFu ? 0xFFFFu : delta);
    if (steps < previous.tick) {
        f.flags = kFrameInterpClockReset;
        f.alpha = 1.0f;
    } else if (delta == 1) {
        f.alpha = 0.5f;  // phase B: a tick frame shows the middle of the last two states
    } else if (delta == 0) {
        f.alpha = 1.0f;  // the frame between two ticks shows S[G]
    } else {
        f.alpha = 1.0f;
        f.flags = kFrameInterpSnapAll;
    }
    return f;
}

void SetFrameInterpSource(FrameInterpSource source) noexcept {
    g_source.store(source);
}

FrameInterp StubFrameInterp() noexcept {
    return FrameInterp{};
}

FrameInterp CurrentFrameInterp() noexcept {
    if (const FrameInterpSource source = g_source.load()) {
        return source();
    }
    if (StepsStubRequested()) {
        return StepsSource();
    }
    return StubFrameInterp();
}

const char* FrameInterpSourceName() noexcept {
    if (g_source.load() != nullptr) {
        return "clock";
    }
    return StepsStubRequested() ? "steps (FUT_INTERP_STUB, developer)" : "stub";
}

}  // namespace gfx::interp
