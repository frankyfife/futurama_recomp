// Game logic tick rate (M3b): 60 Hz with PAL content, or the original 50 Hz PAL timing.
//
// The EU XBE always takes the PAL branch of Game::Init 0xA26A0 (CApp+0xE8 = 0: PAL archives, language
// selection, *-PAL.xmv movies, PAL splash/legal images). That branch sets fps 50 and the fixed time step
// dt = 0.02 s; the NTSC branch of the same block sets fps 60 and dt = 1/60 (analysis/loader/timing60.md 2).
// [game] tick_rate = 60 (default) patches the PAL branch operand by operand to the NTSC values, so the game
// runs the NTSC time base with all PAL content: CApp+0xE8 stays 0 and the movie tag stays "PAL".
//
// Patch table (timing60.md 2.3/2.4; every row checks its whole original instruction or value against the
// mapped image AND the XBE file before any byte is written; only operand bytes change):
//   0xA2714 fld [0x2955AC] (50.0f)              -> fld [0x2942CC] (60.0f, the NTSC branch's constant)
//   0xA271A mov [0x2E7B6C], 50.0f               -> 60.0f
//   0xA272A mov [0x2E7A5C], 0.02f               -> 1/60 (0x3C888889)
//   0xA2734 fld [0x2942C0] (0.02f)              -> fld [0x29E390] (1/60, the NTSC branch's constant)
//   0xA273A mov [0x2E7B68], 1/25.4 (0x3D214285) -> 1/30.5 (0x3D064B8A; written only, exact NTSC state)
//   .data 0x2D6744 fps initial value 50.0f      -> 60.0f: the pause/input ctor FUN_000E4AA0 reads it BEFORE
//                                                  Game::Init; key repeat period trunc(fps * 0.25) = 15 ticks
//                                                  = 0.25 s instead of 12 ticks (timing60.md 4.7)
//   .data 0x2D6748 dt initial value 0.02f       -> 1/60 (consistency; nobody reads dt before Game::Init)
// The shared .rdata constants are never patched (50.0f has 9 code users, 0.02f has 13; timing60.md 2.2).
// Unchanged and verified as references: the branch 0xA26BD, the five NTSC instructions (their operands are
// the patch targets), the PAL movie tag store 0xA2744 and the common tail 0xA274E.
//
// Deliberately NOT changed (timing60.md 4.10, 5, 12): everything the game advances once per fixed step without
// dt runs at 60 Hz exactly like in the NTSC version, whose code and level scripts are identical (3112/3116 game
// functions, all 19 timing functions, 21/23 scripts):
//   UI/camera cosmetics: frontend fade 50 ticks (1.00 s -> 0.83 s), frontend input lock 30 ticks, menu
//     debounce / "new game" delay 5 ticks, overlay fade FUN_000A8490 (0.03 per tick), camera shake, per-tick
//     smoothing, the camera spring's fixed 0.01 s sub-steps (FUN_00015990: 2 per tick at 50 and 60 Hz).
//   Gameplay (review M3b findings 1-3; NTSC parity accepted, timing60.md 12): rigid-body damping x0.98 / x0.9
//     per tick (FUN_000AFA30: pushable objects, seesaws, mines - about 17 % shorter coasting), the Chicken
//     Walker's aim/turn step of a fixed 0.02 s per update (FUN_000445A0 with +0x64 from 0x43EEC: 20 % faster),
//     level-script loops with short waits (every loop iteration ends in a compiler-inserted Wait(0): -17 % to
//     +11 % per iteration, cumulative over the iterations).
// Counted in seconds through dt or QPC (unchanged): position/velocity integration, timers, NetImmerse animation,
// sound fades, script waits, intro/legal/death screens, the 60 s attract idle.
//
// Runtime evidence (GAME/info):
//   "game timing: 60 Hz (NTSC timing values, PAL content) - ..."      HostMain stage ApplyGameTiming
//   "game timing: effective after Game::Init: tick 60 Hz, fps 60 (0x42700000), dt 0.016666668 s ..."
//                                                                    (probe:Game::Init read-back)
//   "game clock: lockstep - [game] frame_clock = lockstep; ..."       HostMain stage ApplyGameTiming
//   "game ticks: N fixed steps in W s (R steps/s ...), steps per CApp::Frame ..."   exit reporter
//   "game clock summary: lockstep, N accumulator samples: +1 step x ..., catch-up x ..., hold x ..."
// probe:Game::FixedStep (FUN_0002C890, called once per fixed step at 0xA2DD3) counts the steps;
// probe:Game::Timer (FUN_000D2B90) feeds Game::Frame the lockstep clock ([game] frame_clock, section below).
#pragma once

#include "core/config.h"
#include "core/guest.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace xapi {

inline constexpr std::uint32_t kTickRateNtsc = 60;  // default
inline constexpr std::uint32_t kTickRatePal = 50;
inline constexpr std::uint32_t kDefaultTickRate = kTickRateNtsc;

// "60" -> 60, "50" -> 50 (surrounding blanks ignored), anything else 0. Same values as the choice list of
// [game] tick_rate in core/config.cpp ("60|50"), which rejects other values with exit code 1.
std::uint32_t ParseTickRate(std::string_view value) noexcept;
// [game] tick_rate of `config` (default 60); 0 if the value is invalid (LoadConfig already refused it).
std::uint32_t ConfiguredTickRate(const core::Config& config) noexcept;
// Tick rate the stage ApplyGameTiming established (60 or 50), 0 before the stage ran.
std::uint32_t ActiveTickRate() noexcept;

// ---- patch table ----------------------------------------------------------------------------------------

inline constexpr std::uint32_t kMaxPatchBytes = 10;  // longest row: mov dword ptr [abs32], imm32

enum class TimingPatchKind : std::uint8_t {
    Code,       // instruction in .text: whole instruction compared, operand bytes replaced
    Data,       // initial value in .data (file-backed part): dword compared and replaced
    Reference,  // instruction or constant that must hold `original` and stays unchanged (patched == original)
};

struct TimingPatch {
    const char* name;
    guest::GuestAddr address;
    std::uint8_t length;  // 1..kMaxPatchBytes
    std::array<std::uint8_t, kMaxPatchBytes> original;
    std::array<std::uint8_t, kMaxPatchBytes> patched;
    TimingPatchKind kind;
};

// Rows that change bytes for `tickRate`: 60 -> 5 Code + 2 Data rows (sorted by address), 50 -> empty.
std::span<const TimingPatch> TimingPatches(std::uint32_t tickRate) noexcept;
// Unchanged rows that must match before any patch is written (branch, NTSC instructions, .rdata targets,
// PAL tag store, common tail).
std::span<const TimingPatch> TimingReferenceRows() noexcept;
// Rows the stage wrote into the mapped image (empty before the stage and at 50 Hz). MappingMatchesFile
// (core/selftest_m1.cpp) excludes them like the fs and hook patches.
std::span<const TimingPatch> AppliedTimingPatches() noexcept;

// Structural checks of a table (no memory access): length 1..10, Code/Data rows change at least one
// byte, Reference rows none; a Code row keeps its opcode bytes (the first 2) and changes only its operand
// (Code: abs32 of `fld m32` or imm32 of `mov m32, imm32`); rows do not overlap each other or an fs patch
// site. False with a description in `error`.
bool ValidateTimingPatchTable(std::span<const TimingPatch> patches, std::span<const TimingPatch> references,
                              std::string& error);

// Memory access of the patch engine (guest image in the loader, fake memories in the unit tests).
class PatchMemory {
public:
    virtual ~PatchMemory() = default;
    // Current bytes of the image; false if not readable.
    virtual bool ReadCurrent(guest::GuestAddr va, void* out, std::uint32_t size) noexcept = 0;
    // Bytes of the XBE file at va (core::ReadOriginalXbeBytes); false if not available.
    virtual bool ReadOriginal(guest::GuestAddr va, void* out, std::uint32_t size) noexcept = 0;
    virtual bool Write(guest::GuestAddr va, const void* data, std::uint32_t size) noexcept = 0;
};

// Validates the table, then checks every reference and patch row against the current image and the XBE
// file (`original` must equal both, so a wrong XBE or a second application is refused), then writes all
// patch rows and reads them back. Nothing is written unless every check passed. False with `error`.
bool ApplyTimingPatches(std::span<const TimingPatch> patches, std::span<const TimingPatch> references,
                        PatchMemory& memory, std::string& error);

// HostMain stage "ApplyGameTiming" (after ApplyFsPatches, before InstallHooks): reads [game] tick_rate,
// applies TimingPatches(rate) to the mapped XBE, registers the exit reporter "game timing" and logs the
// active timing once. False (exit code 2) if a check fails.
bool StageApplyGameTiming(const core::Config& config);

// ---- read-back after Game::Init ------------------------------------------------------------------------

// Raw values of the time base globals (floats as their IEEE bits).
struct TimingGlobals {
    std::uint32_t fps = 0;          // kGameFpsVar
    std::uint32_t dt = 0;           // kGameFrameTimeVar
    std::uint32_t dtMin = 0;        // kGameDtMinVar
    std::uint32_t dtMax = 0;        // kGameDtMaxVar
    std::uint32_t fpsCopy1 = 0;     // kGameFpsCopy1Var
    std::uint32_t fpsCopy2 = 0;     // kGameFpsCopy2Var
    std::uint32_t frameTime = 0;    // kGameVideoFrameTimeVar
    std::uint32_t fixedStep = 0;    // kGameFixedStepVar (u8)
    std::uint32_t videoStandard = 0;  // CApp+0xE8
    std::array<char, 8> movieTag{};   // kGameMovieTagVar, NUL-terminated copy
};

// State Game::Init must produce for `tickRate` (60: NTSC timing values with the "PAL" tag; 50: PAL).
TimingGlobals ExpectedTimingGlobals(std::uint32_t tickRate) noexcept;
// Comma-separated names of the fields that differ ("" if equal), e.g. "fps, dt".
std::string DiffTimingGlobals(const TimingGlobals& actual, const TimingGlobals& expected);

struct GameTimingReadBack {
    bool valid = false;  // read once after Game::Init
    std::uint32_t tickRate = 0;  // ActiveTickRate() at that time
    TimingGlobals values;
    bool matchesExpected = false;
    float fps = 0.0f;  // values.fps / values.dt as floats
    float dt = 0.0f;
};
GameTimingReadBack GetGameTimingReadBack() noexcept;

// Called by probe:Game::Init right after the original returned (xapi/game_probes.cpp): reads the globals
// (CApp = `app`), compares them with ExpectedTimingGlobals(ActiveTickRate()) and logs the result once
// (GAME/info, GAME/error on a mismatch). Without that probe the first fixed step does it.
void NoteGameInitTiming(guest::GuestAddr app) noexcept;

// ---- fixed steps per frame (probe:Game::FixedStep) -------------------------------------------------------

// Histogram of fixed steps per CApp::Frame, built from the step probe alone: a step knows the number of
// the CApp::Frame in progress (completed frames + 1); frames between two stepping frames had 0 steps.
class StepHistogram {
public:
    static constexpr std::uint32_t kBuckets = 4;  // 0, 1, 2, 3+ steps

    // A fixed step ran during CApp::Frame number `frame` (1-based, non-decreasing).
    void OnStep(std::uint64_t frame) noexcept;
    // Histogram over frames first-stepping-frame .. `completedFrames` (frames after the last stepping frame
    // count as 0 steps). Does not change the state.
    std::array<std::uint64_t, kBuckets> Snapshot(std::uint64_t completedFrames) const noexcept;
    std::uint32_t MaxStepsPerFrame() const noexcept { return maxSteps_; }

private:
    std::array<std::uint64_t, kBuckets> closed_{};  // frames before currentFrame_
    std::uint64_t currentFrame_ = 0;                // frame of the last step, 0 = none yet
    std::uint32_t stepsInCurrent_ = 0;
    std::uint32_t maxSteps_ = 0;
};

struct GameTickStats {
    std::uint64_t steps = 0;           // probe:Game::FixedStep calls
    std::uint64_t frames = 0;          // completed CApp::Frame calls (game observer probe)
    double wallSeconds = 0.0;          // first to last step (host QPC)
    double gameSeconds = 0.0;          // steps * dt (read-back dt, else 1 / tick rate)
    double stepsPerSecond = 0.0;       // (steps - 1) / wallSeconds
    std::uint32_t stalls = 0;          // gaps > 6 dt between two stepping frames (the guest caps them)
    double stallSeconds = 0.0;         // sum of (gap - 6 dt) over those gaps, approximate
    double steadyStepsPerSecond = 0.0;  // (steps - 1) / (wallSeconds - stallSeconds)
    std::array<std::uint64_t, StepHistogram::kBuckets> stepsPerFrame{};  // 0, 1, 2, 3+
    std::uint32_t maxStepsPerFrame = 0;
};
GameTickStats GetGameTickStats() noexcept;
// Logs "game ticks: ..." and "game clock: ..." (GAME/info). Exit reporter "game timing".
void LogGameTickSummary() noexcept;

// ---- frame clock of Game::Frame (probe:Game::Timer) ---------------------------------------------------------
//
// Game::Frame 0xA2C20 samples the game timer FUN_000D2B90 (QPC) AFTER variable work (streaming pumps, fiber
// switches) and runs floor-like `while sec(elapsed) >= dt` steps without interpolation. Each displayed frame
// therefore gets 1 step only while its sample keeps a stable phase to the step boundary; after every context
// switch or stall >= 6 dt the cap leaves the remainder right at the boundary (4 ticks at QPF 10 MHz), and the
// sampling jitter then gives 0- and 2-step frames (timing60.md 6, pacing60.md 1.4/2.6). Measured on the 120 Hz
// development machine with the M3 timer pacer at 60 Hz: 584 frames with 0 and 587 with 2 steps of 5968 (9.8 %
// each), i.e. a repeated picture followed by a skipped tick about every 10th frame.
// With the M3b pacer (Present(2) on the 120 Hz display) still 82 / 76 of 5968 frames; with lockstep 6 / 6
// (stall catch-ups and the hold that follows them), 99.75 % of the frames exactly one step.
//
// [game] frame_clock = lockstep (default) replaces the three timer samples of Game::Frame (return addresses
// 0xA2C6F accumulator "now", 0xA2FA6 render-time init, 0xA2FBB render time -> g_frameDelta 0x2E7B88) by a
// virtual time V that advances by exactly ticks(dt) = trunc(QPF * dt) (the guest's own conversion) per frame,
// so a frame at the tick rate always yields exactly one step and the remainder never moves. V follows real
// time in whole steps: when the real sample is more than 3/4 step ahead of V + ticks(dt) (late or dropped
// frames, stalls) V catches up by as many steps as needed, when it is more than 3/4 step behind (frames faster
// than the tick rate) V holds. The phase real - V therefore stays within -3/4..+3/4 step (12.5 ms at 60 Hz);
// a correction moves it to the opposite side with 1/2 step margin, so jitter cannot flip-flop. Every other
// caller of FUN_000D2B90 (intro/legal/death screen times, the 2.5 s limit 0xD3167, streaming) keeps real QPC.
// realtime = the original samples (the probe only passes them through).
enum class FrameClockMode : std::uint8_t { Lockstep, Realtime };
// "lockstep" / "realtime" (case-insensitive, surrounding blanks ignored); false otherwise.
bool ParseFrameClock(std::string_view value, FrameClockMode& mode) noexcept;
// [game] frame_clock (default lockstep); false if the value is invalid (LoadConfig already refused it).
bool ConfiguredFrameClock(const core::Config& config, FrameClockMode& mode) noexcept;
const char* FrameClockName(FrameClockMode mode) noexcept;
// Mode the stage established (Realtime before the stage ran).
FrameClockMode ActiveFrameClock() noexcept;

// Guest conversion seconds -> timer ticks (FUN_000D2B50 / _ftol: trunc(QPF * s)), e.g. 166666 for dt = 1/60
// at QPF 10 MHz. 0 for non-positive input.
std::int64_t GuestTimerTicks(std::int64_t qpf, float seconds) noexcept;

class LockstepClock {
public:
    struct Stats {
        std::uint64_t samples = 0;     // Advance calls
        std::uint64_t single = 0;      // advanced by exactly one step
        std::uint64_t catchUps = 0;    // advanced by 2 or more steps
        std::uint64_t holds = 0;       // did not advance
        std::int64_t maxCatchUpSteps = 0;
        std::int64_t minPhase = 0;     // real - virtual after the sample, in ticks
        std::int64_t maxPhase = 0;
    };

    // Accumulator sample of Game::Frame: `real` = the timer value the guest just read, `step` = ticks(dt) > 0.
    // Returns the virtual time (the first sample returns `real`).
    std::int64_t Advance(std::int64_t real, std::int64_t step) noexcept;
    bool Started() const noexcept { return started_; }
    std::int64_t Now() const noexcept { return virtual_; }
    const Stats& GetStats() const noexcept { return stats_; }

private:
    bool started_ = false;
    std::int64_t virtual_ = 0;
    Stats stats_;
};

// ---- M3d: frame clock in quanta (half-tick clock; analysis/loader/m3d_frameclock.md 2) -------------------------
//
// The M3b clock advances ticks(dt) per frame. M3d generalises it to a quantum per frame that the frame pacer chooses
// (platform::CurrentFrameAdvance, platform/pacer_policy.h FrameAdvance):
//   - N frames per tick (the plan's rate is N x the tick rate within 100 ppm): quanta of ticks(dt) / N - for N = 2
//     alternately floor and ceil, so two quanta are exactly ticks(dt). N = 1 is the M3b clock bit for bit (unit test).
//   - otherwise (75/100/119.88 Hz, the cadence plan, 72 fps on 144 Hz): the display time of the frame in QPC ticks
//     with a fractional accumulator, so the game time follows the display exactly.
// Corrections as in M3b: catch up by whole quanta when real time is more than 3/4 quantum ahead, hold one frame
// when it is more than 3/4 quantum behind (never behind the previous frame). With N >= 2 the guest's remainder is
// aligned once ("phase B", m3d_frameclock.md 2.3) so that the largest remainder of the pattern is ticks(dt) - mu
// (mu = max(16, ticks(dt) / 1024)): alpha is then exactly 1/N, 2/N, ..., 1 - for N = 2 1/2 in the frame that runs
// the tick and 1 in the frame between (a tick's input shows half its effect in its own frame). Alignment happens
// in the second frame, after the guest's 6 dt cap and after a change of the regime (frames per tick, nominal period
// or ticks(dt)); a regime change is reported as a clock reset.
// Review finding 11: in the sub-tick regimes (N != 1) the input `real` is the pacer's frame start instead of the
// guest's timer sample (which follows the streaming pumps), and a pacing plan change (`rephase`: free <-> vblank moves
// the frame starts to another grid) is absorbed once: the clock adds an offset so that this frame advances by exactly
// one quantum instead of holding or catching up on the jump (at most one tick; larger jumps are real stalls). N = 1
// keeps the M3b clock bit for bit (no offset).
struct ClockQuantum {
    std::uint32_t framesPerTick = 1;  // N >= 1: quanta of ticks(dt) / N; 0 = `ticks`
    double ticks = 0.0;               // N == 0: QPC ticks of this frame's display time (fractions accumulate)
    double nominalTicks = 0.0;        // N == 0: QPC ticks of the regime's nominal period (margin and regime identity)
    bool rephase = false;             // N != 1: the pacing plan changed before this frame (see above)
};

class FrameClock {
public:
    struct Stats {
        std::uint64_t samples = 0;    // Advance calls
        std::uint64_t single = 0;     // advanced by exactly one quantum
        std::uint64_t catchUps = 0;   // advanced by 2 or more quanta
        std::uint64_t holds = 0;      // did not advance
        std::int64_t maxCatchUpQuanta = 0;
        std::int64_t minPhase = 0;    // real - virtual after the sample, in ticks (excluding the first sample)
        std::int64_t maxPhase = 0;
        std::uint64_t resets = 0;     // regime changes after the first sample
        std::uint64_t aligns = 0;     // phase B corrections
        std::uint64_t rephases = 0;   // plan changes absorbed (ClockQuantum::rephase)
    };
    struct Sample {
        std::int64_t value = 0;   // the virtual time V handed to the guest
        bool reset = false;       // first sample or a regime change
        bool aligned = false;     // the remainder was moved to phase B in this sample
        bool rephased = false;    // a plan change's phase jump was absorbed in this sample
        std::int64_t quanta = 0;  // quanta added: 0 hold, 1 normal, >= 2 catch-up
    };
    // Accumulator sample of Game::Frame. `real` = the guest's timer value (minus the pacer's deliberate start delay),
    // `step` = ticks(dt) > 0, `lastPrev` = CApp+0xC8 before this frame (0 = unknown: no alignment), `capTicks` =
    // the guest's cap ticks(6 dt) (0 = unknown). The first sample returns `real`.
    Sample Advance(std::int64_t real, std::int64_t step, const ClockQuantum& quantum, std::int64_t lastPrev,
                   std::int64_t capTicks) noexcept;
    bool Started() const noexcept { return started_; }
    std::int64_t Now() const noexcept { return virtual_; }
    // The remainder pattern is aligned to phase B (N >= 2, no cap or reset since the alignment).
    bool PhaseB() const noexcept { return phaseB_; }
    std::uint32_t FramesPerTick() const noexcept { return regimeN_; }
    const Stats& GetStats() const noexcept { return stats_; }
    // mu of phase B: max(16, ticks(dt) / 1024) ticks below the guest's step threshold.
    static std::int64_t PhaseMargin(std::int64_t step) noexcept;
    // Quantum i (0 .. N-1) of a tick split into N: floor((i + 1) * step / N) - floor(i * step / N).
    static std::int64_t TickQuantum(std::int64_t step, std::uint32_t n, std::uint32_t i) noexcept;

private:
    std::int64_t PeekQuantum(std::int64_t step) const noexcept;
    void Consume(std::int64_t step) noexcept;
    bool started_ = false;
    std::int64_t virtual_ = 0;
    std::int64_t offset_ = 0;    // subtracted from `real` (rephase; always 0 with N = 1)
    // Regime.
    std::uint32_t regimeN_ = 1;
    std::int64_t regimeStep_ = 0;
    double regimeNominal_ = 0.0;
    // Quantum state.
    std::uint32_t phase_ = 0;    // N >= 2: index of the next quantum
    double ticks_ = 0.0;         // N == 0: this frame's quantum
    double fraction_ = 0.0;      // N == 0: accumulated fraction of a tick
    bool alignPending_ = false;
    bool phaseB_ = false;
    Stats stats_;
};

// alpha of the frame being rendered (m3d_frameclock.md 2.5, m3d_interpolation.md 6.1): the weight of the newest
// tick state, alpha = clamp(1 - (L - rest) / step, 0, 1) with rest = the guest's remainder (accumulator time - CApp+0xC8
// after the step loop) and L = the largest remainder the clock produces: ticks(dt) - mu in phase B, else ticks(dt).
// With N >= 2 a value within 0.01 of a multiple of 1/N is that multiple (phase B: exactly 1/2 and 1); >= 0.999 is 1.
float FrameAlpha(std::int64_t rest, std::int64_t step, std::uint32_t framesPerTick, bool phaseB) noexcept;

// ---- M3d: interpolation values for the renderer -----------------------------------------------------------------
//
// Published at the render sample of Game::Frame (return 0xA2FBB, after `last` 0xA2F6A, before cur->Render() 0xA3014)
// and valid for every draw until CApp::Frame returns (pause and debug overlays included). Inside the step loop
// (draws from Update: intro screens, the eight cleared frames of a context switch) and outside Game::Frame (the movie
// before it) the values say "as is" (alpha 1, kFrameInterpInsideStep). The flags have the bit values of the renderer's
// contract (gfx/interp_frame.h); the stage registers CurrentFrameInterp there.
// A catch-up (more steps than the regime allows) and the guest's 6 dt cap are no snap-all since the M3d review (finding
// 10): the renderer interpolates over the pair of the last k = pairTicks ticks, interp(S[G-k], S[G], (k - 1 + alpha) /
// k), so the displayed time G - 1 + alpha neither stands still nor goes back after the correction.
inline constexpr std::uint32_t kFrameInterpSnapAll = 1u << 0;     // context switch, clock reset
inline constexpr std::uint32_t kFrameInterpInsideStep = 1u << 1;  // drawn from Update or outside Game::Frame
inline constexpr std::uint32_t kFrameInterpClockReset = 1u << 2;  // first frame or a regime change of the clock
inline constexpr std::uint32_t kFrameInterpCatchUp = 1u << 3;     // catch-up or 6 dt cap (diagnostics, see above)

struct FrameInterp {
    std::uint64_t tick = 0;            // completed fixed steps (probe:Game::FixedStep) = generation G
    std::uint32_t stepsThisFrame = 0;  // fixed steps of the current CApp::Frame
    std::uint32_t pairTicks = 1;       // ticks between the two newest rendered states (steps of the last tick frame)
    float alpha = 1.0f;                // weight of S[G]: display = interp(S[G-1], S[G], alpha) (k ticks: see above)
    std::uint32_t flags = 0;
    bool enabled = false;              // the clock produces frames between ticks (frame rate above the tick rate)
    std::uint64_t frame = 0;           // render samples so far (changes once per rendered frame)
};
FrameInterp CurrentFrameInterp() noexcept;
// The fixes for code that runs per frame instead of per tick (xapi/frame_fixes.cpp, m3d_frameclock.md 4.2): true while
// the clock produces frames between ticks (never at 60 fps on a 60 Hz tick: bit-identical).
bool SubTickFramesActive() noexcept;
// True while a frame that ran no fixed step renders (sub-tick mode only).
bool RenderingFrameWithoutStep() noexcept;

struct FrameClockStats {
    FrameClockMode mode = FrameClockMode::Realtime;
    std::int64_t step = 0;  // last ticks(dt)
    std::int64_t qpf = 0;   // guest timer frequency [0x2EB448]
    LockstepClock::Stats clock;
    std::uint64_t renderSamples = 0;  // samples at 0xA2FA6 / 0xA2FBB answered with the virtual time
    // M3d.
    std::uint32_t framesPerTick = 1;  // regime of the last accumulator sample (0 = display time)
    std::uint64_t resets = 0, aligns = 0;
    std::uint64_t renderedFrames = 0;               // render samples at 0xA2FBB
    std::uint64_t subTickFrames = 0;                // ... in the sub-tick mode
    std::uint64_t alphaHalf = 0, alphaOne = 0, alphaOther = 0;  // alpha of the sub-tick frames
    // Clock events of the sub-tick frames: snap-all (context switch, reset) and pair interpolation (catch-up, cap).
    std::uint64_t snapSwitch = 0, snapCatchUp = 0, snapCap = 0, snapReset = 0;
    std::uint64_t frameStartSamples = 0;            // accumulator samples fed with the pacer's frame start (finding 11)
    std::uint64_t rephases = 0;                     // plan changes whose start phase jump the clock absorbed
    std::uint64_t startDelays = 0;                  // samples corrected by the pacer's just-in-time delay
};
FrameClockStats GetFrameClockStats() noexcept;

}  // namespace xapi
