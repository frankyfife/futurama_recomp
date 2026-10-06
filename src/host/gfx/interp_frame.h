// FrameInterp: what the renderer-level interpolation (gfx/interp_history.h) needs from the frame clock per
// frame (analysis/loader/m3d_interpolation.md 3.1/6.1, m3d_frameclock.md 2.5). Owned by the interpolation
// agent; the frame clock PROVIDES the values through SetFrameInterpSource.
//
// Contract (binding for every source):
//   - `tick` = completed fixed logic steps (probe:Game::FixedStep) when the guest draws = generation G of the
//     state S[G] the guest data shows. It never decreases while a source is registered; a jump by more than 1
//     between two frames is a catch-up (the history snaps over the gap).
//   - `alpha` = weight of S[G]: the renderer shows interp(S[G-1], S[G], alpha); alpha = 1 means "the guest data
//     as is" (bit-identical to the original renderer). alpha = clamp(1 - (L - rest) / dt, 0, 1) with L = the
//     largest remainder the clock produces. With the half-tick clock at 120 Hz ("phase B") alpha is 1/2 in the
//     frames that ran a tick and exactly 1 in the frames between: the displayed time G - 1 + alpha rises by 1/2
//     per frame. (The naive reading alpha = rest / dt with "1/2 in the zero-step frame, 1 in the tick frame"
//     would step backwards every second frame - never deliver that.) Values >= 1 - 1e-3 are treated as 1.
//   - `flags`: kFrameInterpSnapAll (context switch, catch-up of >= 2 steps, 6 dt cap, first frame) -> every key
//     shows S[G]; kFrameInterpInsideStep (the guest draws from Update inside the step loop: loading and clear
//     frames, intro screens) -> pass-through, nothing recorded; kFrameInterpClockReset like SnapAll.
//   - `enabled` = the clock produces intermediate frames (frame rate above the tick rate). The renderer ANDs it
//     with [render] interpolation; with enabled = false every draw passes through unchanged (60 fps: zero change).
//   - Constant between two DisplayFrame calls of the guest main loop except for the InsideStep phase; the
//     renderer reads it at every SetCameraData and at the first draw of a frame.
//
// Until the frame clock lands the stub source returns FrameInterp{} (enabled = false: no interpolation). For
// renderer tests in the running game, the environment variable FUT_INTERP_STUB=steps selects a developer source
// that derives the values from the fixed steps of the frame (1 step -> alpha 1/2, 0 steps -> 1, >= 2 steps ->
// snapAll) - meaningful only with a frame rate of twice the tick rate and the lockstep clock ([render]
// frame_rate = 120, [game] frame_clock = lockstep, which gives the strict 1/0 step pattern).
#pragma once

#include <cstdint>

namespace gfx::interp {

inline constexpr std::uint32_t kFrameInterpSnapAll = 1u << 0;
inline constexpr std::uint32_t kFrameInterpInsideStep = 1u << 1;
inline constexpr std::uint32_t kFrameInterpClockReset = 1u << 2;

struct FrameInterp {
    std::uint64_t tick = 0;            // completed fixed steps when the guest draws (generation G)
    std::uint32_t stepsThisFrame = 0;  // fixed steps of the current CApp::Frame (diagnostics)
    float alpha = 1.0f;                // weight of S[G] (see the contract above)
    std::uint32_t flags = 0;           // kFrameInterp*
    bool enabled = false;              // the clock produces intermediate frames
};

using FrameInterpSource = FrameInterp (*)() noexcept;

// Registers the frame clock's source (nullptr = the stub / the developer source of FUT_INTERP_STUB). Thread-safe;
// called once at startup by the clock's module (or the integration).
void SetFrameInterpSource(FrameInterpSource source) noexcept;
// The values for the frame being drawn (the registered source, else the stub).
FrameInterp CurrentFrameInterp() noexcept;
// FrameInterp{} - no interpolation.
FrameInterp StubFrameInterp() noexcept;
// "clock" (a source is registered), "stub" or "steps (FUT_INTERP_STUB, developer)".
const char* FrameInterpSourceName() noexcept;

// Developer source of FUT_INTERP_STUB=steps (pure part, unit-tested): a new frame (`frame` = completed
// CApp::Frame count changed) with `steps` total fixed steps; `previous` = the values of the last frame.
FrameInterp StepsFrameInterp(const FrameInterp& previous, std::uint64_t steps) noexcept;

}  // namespace gfx::interp
