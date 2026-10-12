// Game logic tick rate (M3b; see game_timing.h for the patch table, the evidence and what stays unchanged).
//
// Sources: analysis/loader/timing60.md (2.1 instruction listing, 2.2 constant pool, 2.3 patch table, 2.4
// .data initial values, 3 readers of the globals, 4.10 tick-counted cosmetics, 6 fixed-step loop) with the
// capstone listings in analysis/loader/scripts/timing_patch.out; the instruction bytes below were checked
// again against orig/retail_eu/default.xbe (tests/timing_tests.cpp compares them with the file when present).
#include "xapi/game_timing.h"

#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "core/selftest.h"
#include "core/xbe_loader.h"
#include "gfx/interp_frame.h"
#include "platform/frame_pacer.h"
#include "xapi/frame_fixes.h"
#include "xapi/game_probes.h"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace xapi {
namespace {

// ---- float values (IEEE-754 single bits as the XBE stores them) -----------------------------------------
constexpr std::uint32_t kF32Fps50 = 0x42480000;        // 50.0f
constexpr std::uint32_t kF32Fps60 = 0x42700000;        // 60.0f
constexpr std::uint32_t kF32Dt50 = 0x3CA3D70A;         // 0.02f
constexpr std::uint32_t kF32Dt60 = 0x3C888889;         // 1/60 = 0.016666668f
constexpr std::uint32_t kF32FrameTimePal = 0x3D214285;   // 1/25.4 (written only, timing60.md 3)
constexpr std::uint32_t kF32FrameTimeNtsc = 0x3D064B8A;  // 1/30.5
static_assert(std::bit_cast<float>(kF32Fps50) == 50.0f);
static_assert(std::bit_cast<float>(kF32Fps60) == 60.0f);
static_assert(std::bit_cast<std::uint32_t>(0.02f) == kF32Dt50);
static_assert(std::bit_cast<std::uint32_t>(1.0f / 60.0f) == kF32Dt60);
static_assert(std::bit_cast<std::uint32_t>(1.0f / 25.4f) == kF32FrameTimePal);
static_assert(std::bit_cast<std::uint32_t>(1.0f / 30.5f) == kF32FrameTimeNtsc);
constexpr std::uint32_t kMovieTagPal = 0x004C4150;  // "PAL\0" as the dword immediate of 0xA2744

// ---- x86 encodings of the rows -------------------------------------------------------------------------
struct Encoded {
    std::uint8_t length;
    std::array<std::uint8_t, kMaxPatchBytes> bytes;
};
constexpr std::uint8_t B(std::uint32_t v, int i) {
    return static_cast<std::uint8_t>(v >> (8 * i));
}
// fld dword ptr [abs32] = D9 05 abs32
constexpr Encoded FldM32(guest::GuestAddr src) {
    return {6, {0xD9, 0x05, B(src, 0), B(src, 1), B(src, 2), B(src, 3)}};
}
// fst dword ptr [abs32] = D9 15 abs32
constexpr Encoded FstM32(guest::GuestAddr dst) {
    return {6, {0xD9, 0x15, B(dst, 0), B(dst, 1), B(dst, 2), B(dst, 3)}};
}
// fstp dword ptr [abs32] = D9 1D abs32
constexpr Encoded FstpM32(guest::GuestAddr dst) {
    return {6, {0xD9, 0x1D, B(dst, 0), B(dst, 1), B(dst, 2), B(dst, 3)}};
}
// mov dword ptr [abs32], imm32 = C7 05 abs32 imm32
constexpr Encoded MovM32Imm32(guest::GuestAddr dst, std::uint32_t imm) {
    return {10, {0xC7, 0x05, B(dst, 0), B(dst, 1), B(dst, 2), B(dst, 3), B(imm, 0), B(imm, 1), B(imm, 2),
                 B(imm, 3)}};
}
// mov byte ptr [abs32], bl = 88 1D abs32
constexpr Encoded MovM8Bl(guest::GuestAddr dst) {
    return {6, {0x88, 0x1D, B(dst, 0), B(dst, 1), B(dst, 2), B(dst, 3)}};
}
// little-endian dword (.data / .rdata value)
constexpr Encoded Dword(std::uint32_t v) {
    return {4, {B(v, 0), B(v, 1), B(v, 2), B(v, 3)}};
}
constexpr Encoded Raw(std::uint8_t length, std::array<std::uint8_t, kMaxPatchBytes> bytes) {
    return {length, bytes};
}

constexpr TimingPatch Patch(const char* name, guest::GuestAddr address, Encoded original, Encoded patched,
                            TimingPatchKind kind) {
    // Mismatched lengths make the row invalid (length 0), which ValidateTimingPatchTable reports.
    return TimingPatch{name,
                       address,
                       original.length == patched.length ? original.length : std::uint8_t{0},
                       original.bytes,
                       patched.bytes,
                       kind};
}
constexpr TimingPatch Reference(const char* name, guest::GuestAddr address, Encoded bytes) {
    return Patch(name, address, bytes, bytes, TimingPatchKind::Reference);
}

using TimingPatchKind::Code;
using TimingPatchKind::Data;

// tick_rate = 60: the PAL branch of Game::Init gets the NTSC values (timing60.md 2.3), the .data initial
// values follow (2.4). Sorted by address.
constexpr TimingPatch kPatches60[] = {
    Patch("Game::Init PAL fps source: fld [50.0f] -> fld [60.0f] (-> 0x2E7A60, fps 0x2D6744)",
          guest::kGameInitPalFpsLoad, FldM32(guest::kRdataFloat50), FldM32(guest::kRdataFloat60), Code),
    Patch("Game::Init PAL fps copy [0x2E7B6C]: 50.0f -> 60.0f", guest::kGameInitPalFpsCopyStore,
          MovM32Imm32(guest::kGameFpsCopy2Var, kF32Fps50), MovM32Imm32(guest::kGameFpsCopy2Var, kF32Fps60),
          Code),
    Patch("Game::Init PAL dt clamp [0x2E7A5C]: 0.02f -> 1/60", guest::kGameInitPalDtMaxStore,
          MovM32Imm32(guest::kGameDtMaxVar, kF32Dt50), MovM32Imm32(guest::kGameDtMaxVar, kF32Dt60), Code),
    Patch("Game::Init PAL dt source: fld [0.02f] -> fld [1/60] (-> 0x2E7A58, dt 0x2D6748)",
          guest::kGameInitPalDtLoad, FldM32(guest::kRdataFloat0p02), FldM32(guest::kRdataFloatOneSixtieth),
          Code),
    Patch("Game::Init PAL [0x2E7B68]: 1/25.4 -> 1/30.5 (written only)", guest::kGameInitPalFrameTimeStore,
          MovM32Imm32(guest::kGameVideoFrameTimeVar, kF32FrameTimePal),
          MovM32Imm32(guest::kGameVideoFrameTimeVar, kF32FrameTimeNtsc), Code),
    Patch(".data fps initial value 0x2D6744: 50.0f -> 60.0f (key repeat period of FUN_000E4AA0)",
          guest::kGameFpsVar, Dword(kF32Fps50), Dword(kF32Fps60), Data),
    Patch(".data dt initial value 0x2D6748: 0.02f -> 1/60", guest::kGameFrameTimeVar, Dword(kF32Dt50),
          Dword(kF32Dt60), Data),
};

// Unchanged code and constants the patch relies on (timing60.md 2.1/2.2).
constexpr TimingPatch kReferences[] = {
    Reference("Game::Init: mov eax,[esi+0xE8] (CApp video standard)", guest::kGameInitVideoStandardBranch,
              Raw(6, {0x8B, 0x86, 0xE8, 0x00, 0x00, 0x00})),
    Reference("Game::Init: mov ebx,1; cmp eax,ebx; jne 0xA2714 (PAL branch)",
              guest::kGameInitVideoStandardBranch + 6, Raw(9, {0xBB, 0x01, 0x00, 0x00, 0x00, 0x3B, 0xC3, 0x75, 0x48})),
    Reference("Game::Init NTSC: fld [60.0f]", guest::kGameInitNtscFpsLoad, FldM32(guest::kRdataFloat60)),
    Reference("Game::Init NTSC: fld [1/60]", guest::kGameInitNtscDtLoad, FldM32(guest::kRdataFloatOneSixtieth)),
    Reference("Game::Init NTSC: mov [0x2E7B6C], 60.0f", guest::kGameInitNtscFpsCopyStore,
              MovM32Imm32(guest::kGameFpsCopy2Var, kF32Fps60)),
    Reference("Game::Init NTSC: mov [0x2E7A5C], 1/60", guest::kGameInitNtscDtMaxStore,
              MovM32Imm32(guest::kGameDtMaxVar, kF32Dt60)),
    Reference("Game::Init NTSC: mov [0x2E7B68], 1/30.5", guest::kGameInitNtscFrameTimeStore,
              MovM32Imm32(guest::kGameVideoFrameTimeVar, kF32FrameTimeNtsc)),
    Reference("Game::Init PAL: fst [0x2E7A60] (fps copy, unchanged)", guest::kGameInitPalFpsCopy1Store,
              FstM32(guest::kGameFpsCopy1Var)),
    Reference("Game::Init PAL: mov [0x2E7B70], \"PAL\" (movie tag, kept)", guest::kGameInitPalMovieTagStore,
              MovM32Imm32(guest::kGameMovieTagVar, kMovieTagPal)),
    Reference("Game::Init tail: fst [0x2E7A58] (dt clamp)", guest::kGameInitTimeBaseTail,
              FstM32(guest::kGameDtMinVar)),
    Reference("Game::Init tail: mov [0x2D674C], bl (fixed step = 1)", guest::kGameInitTimeBaseTail + 6,
              MovM8Bl(guest::kGameFixedStepVar)),
    Reference("Game::Init tail: fxch st(1); mov ecx,esi", guest::kGameInitTimeBaseTail + 12,
              Raw(4, {0xD9, 0xC9, 0x8B, 0xCE})),
    Reference("Game::Init tail: fstp [0x2D6744] (fps)", guest::kGameInitTimeBaseTail + 16,
              FstpM32(guest::kGameFpsVar)),
    Reference("Game::Init tail: fstp [0x2D6748] (dt)", guest::kGameInitTimeBaseTail + 22,
              FstpM32(guest::kGameFrameTimeVar)),
    Reference(".rdata 60.0f (fps operand target)", guest::kRdataFloat60, Dword(kF32Fps60)),
    Reference(".rdata 1/60 (dt operand target)", guest::kRdataFloatOneSixtieth, Dword(kF32Dt60)),
};

std::string Hex(const std::uint8_t* bytes, std::uint32_t n) {
    std::string out;
    char buf[4];
    for (std::uint32_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", bytes[i]);
        out += buf;
    }
    return out;
}

std::string Printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

bool Overlaps(guest::GuestAddr a, std::uint32_t an, guest::GuestAddr b, std::uint32_t bn) noexcept {
    return a < b + bn && b < a + an;
}

const char* KindName(TimingPatchKind kind) noexcept {
    switch (kind) {
    case TimingPatchKind::Code:
        return "code";
    case TimingPatchKind::Data:
        return "data";
    case TimingPatchKind::Reference:
        return "reference";
    }
    return "?";
}

// Operand range [begin, end) a Code row may change: imm32 of `mov m32, imm32`, abs32 of `fld m32`.
bool CodeOperandRange(const TimingPatch& p, std::uint32_t& begin, std::uint32_t& end) noexcept {
    if (p.length == 10 && p.original[0] == 0xC7 && p.original[1] == 0x05) {
        begin = 6;
        end = 10;
        return true;
    }
    if (p.length == 6 && p.original[0] == 0xD9 && p.original[1] == 0x05) {
        begin = 2;
        end = 6;
        return true;
    }
    return false;
}

// Image access of the loader: the mapped XBE (guest addresses are host addresses) and the file bytes.
class GuestImageMemory final : public PatchMemory {
public:
    bool ReadCurrent(guest::GuestAddr va, void* out, std::uint32_t size) noexcept override {
        if (!guest::IsReadable(va, size)) {
            return false;
        }
        std::memcpy(out, guest::Ptr<const void>(va), size);
        return true;
    }
    bool ReadOriginal(guest::GuestAddr va, void* out, std::uint32_t size) noexcept override {
        return core::ReadOriginalXbeBytes(va, out, size);
    }
    bool Write(guest::GuestAddr va, const void* data, std::uint32_t size) noexcept override {
        return guest::WriteBytes(va, data, size);
    }
};

// ---- process state ------------------------------------------------------------------------------------
std::atomic<std::uint32_t> g_activeRate{0};
std::atomic<bool> g_patchesApplied{false};
std::atomic<bool> g_readBackClaimed{false};
std::atomic<FrameClockMode> g_clockMode{FrameClockMode::Realtime};

std::mutex g_lock;  // read-back, step and clock statistics; never held across a guest call
GameTimingReadBack g_readBack;
FrameClock g_clock;  // M3d: the M3b LockstepClock generalised to quanta (N = 1 is the M3b clock)
std::int64_t g_clockStep = 0;
std::int64_t g_clockQpf = 0;
std::uint64_t g_clockRenderSamples = 0;
std::uint64_t g_clockStartDelays = 0;
std::uint64_t g_clockFrameStarts = 0;       // samples fed with the pacer's frame start (review finding 11)
std::int64_t g_clockLastFrameStart = 0;     // FrameAdvance::frameStartQpc of the last sample (staleness check)
std::uint32_t g_clockPlanSerial = 0;        // FrameAdvance::planSerial of the last sample
std::uint64_t g_regimeLogs = 0;
std::uint64_t g_alignLogs = 0;
constexpr std::uint64_t kMaxClockLogs = 8;

// M3d: fixed steps so far (probe:Game::FixedStep; atomic: the renderer reads it per draw).
std::atomic<std::uint64_t> g_fixedSteps{0};

// M3d: the frame being rendered (accumulator sample -> render sample -> end of CApp::Frame). Guest main thread;
// the lock guards the statistics read by the exit reporter and keeps the per-draw reads consistent.
enum class FramePhase : std::uint8_t { Outside, Loop, Render };
struct InterpState {
    FramePhase phase = FramePhase::Outside;
    std::uint64_t appFrame = 0;          // CApp::Frame number (observer frames + 1) of the render phase
    std::uint64_t stepsAtSample = 0;     // g_fixedSteps at the accumulator sample
    std::uint32_t switchesAtSample = 0;  // context switches at the accumulator sample
    std::int64_t accNow = 0;             // the accumulator time handed to the guest
    std::int64_t step = 0;               // ticks(dt)
    bool capThisFrame = false;           // the guest caps the elapsed time at 6 dt in this frame
    bool resetThisFrame = false;         // first frame or a regime change of the clock
    bool subTick = false;                // the clock produces frames between ticks
    std::uint32_t framesPerTick = 1;
    bool phaseB = false;
    std::uint32_t maxSteps = 1;          // steps a frame of the regime may run without being a catch-up
    bool haveRegime = false;             // realtime mode: regime of the last sample (reset detection)
    std::uint32_t regimeN = 1;
    double regimeNominal = 0.0;
    std::uint32_t pairTicks = 1;
    std::uint64_t renderFrames = 0;
    FrameInterp published;
    // Statistics.
    std::uint64_t subTickFrames = 0, alphaHalf = 0, alphaOne = 0, alphaOther = 0;
    std::uint64_t snapSwitch = 0, snapCatchUp = 0, snapCap = 0, snapReset = 0;
};
std::mutex g_interpLock;
InterpState g_interp;

struct StepState {
    StepHistogram histogram;
    std::uint64_t steps = 0;
    std::int64_t firstQpc = 0;
    std::int64_t lastQpc = 0;
    std::uint64_t lastStepFrame = 0;  // CApp::Frame number of the last step
    std::int64_t frameFirstQpc = 0;   // QPC of the first step of that frame
    std::uint32_t stalls = 0;
    std::int64_t stallTicks = 0;
};
StepState g_steps;

std::int64_t Qpc() noexcept {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
std::int64_t Qpf() noexcept {
    static const std::int64_t f = [] {
        LARGE_INTEGER v{};
        QueryPerformanceFrequency(&v);
        return v.QuadPart > 0 ? v.QuadPart : 1;
    }();
    return f;
}

// dt in seconds for the statistics: the read-back value, else the active (or default) tick rate.
double StatsDt() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_readBack.valid && g_readBack.dt > 0.0f) {
            return static_cast<double>(g_readBack.dt);
        }
    }
    const std::uint32_t rate = g_activeRate.load();
    return 1.0 / static_cast<double>(rate != 0 ? rate : kDefaultTickRate);
}

std::uint32_t ReadGuestU32(guest::GuestAddr a) noexcept {
    return guest::IsReadable(a, 4) ? guest::Read<std::uint32_t>(a) : 0;
}

TimingGlobals ReadTimingGlobals(guest::GuestAddr app) noexcept {
    TimingGlobals v;
    v.fps = ReadGuestU32(guest::kGameFpsVar);
    v.dt = ReadGuestU32(guest::kGameFrameTimeVar);
    v.dtMin = ReadGuestU32(guest::kGameDtMinVar);
    v.dtMax = ReadGuestU32(guest::kGameDtMaxVar);
    v.fpsCopy1 = ReadGuestU32(guest::kGameFpsCopy1Var);
    v.fpsCopy2 = ReadGuestU32(guest::kGameFpsCopy2Var);
    v.frameTime = ReadGuestU32(guest::kGameVideoFrameTimeVar);
    v.fixedStep = guest::IsReadable(guest::kGameFixedStepVar, 1) ? guest::Read<std::uint8_t>(guest::kGameFixedStepVar)
                                                                  : 0xFFu;
    if (app == 0) {
        app = ReadGuestU32(guest::kGameGamePtr);
    }
    v.videoStandard = app != 0 ? ReadGuestU32(app + guest::kCAppVideoStandardOffset) : 0xFFFFFFFFu;
    if (guest::IsReadable(guest::kGameMovieTagVar, static_cast<std::uint32_t>(v.movieTag.size()))) {
        std::memcpy(v.movieTag.data(), guest::Ptr<const void>(guest::kGameMovieTagVar), v.movieTag.size());
    }
    v.movieTag.back() = '\0';
    return v;
}

const char* TimingDescription(std::uint32_t rate) noexcept {
    return rate == kTickRateNtsc ? "NTSC timing values, PAL content" : "original PAL timing values";
}

guest::GuestAddr Trampoline(guest::GuestAddr address) noexcept {
    const guest::GuestAddr t = core::HookTrampoline(address);
    if (t == 0) {
        core::Fatal("probe at 0x%08X runs without an installed trampoline", address);
    }
    return t;
}

void NoteFixedStep() {
    const GameObserverStats observer = GetGameObserverStats();
    const std::uint64_t frame = observer.frames + 1;  // CApp::Frame in progress (counted after it returns)
    const std::int64_t now = Qpc();
    const double dt = StatsDt();
    const auto capTicks = static_cast<std::int64_t>(6.0 * dt * static_cast<double>(Qpf()));
    std::lock_guard<std::mutex> lock(g_lock);
    StepState& s = g_steps;
    if (s.steps == 0) {
        s.firstQpc = now;
    }
    if (frame != s.lastStepFrame) {
        // Game::Frame caps the time since its last sample at 6 dt (timing60.md 6): a longer gap between two
        // stepping frames is game time the guest drops on purpose.
        if (s.lastStepFrame != 0 && now - s.frameFirstQpc > capTicks) {
            ++s.stalls;
            s.stallTicks += now - s.frameFirstQpc - capTicks;
        }
        s.lastStepFrame = frame;
        s.frameFirstQpc = now;
    }
    s.histogram.OnStep(frame);
    ++s.steps;
    s.lastQpc = now;
}

// ---- probe ---------------------------------------------------------------------------------------------

// FUN_0002C890 - thiscall (ecx = [0x2E7B7C]), no stack args, plain ret (0x2C8E5/0x2C8FF/0x2C908/0x2C91F),
// al = 1. Game::Frame 0xA2C20 calls it exactly once per fixed time step at 0xA2DD3 (after CApp vf10 at
// 0xA2DCA, before cur->Update() at 0xA2DE0); no other caller (guest.h kGameFixedStepHelper). The probe
// counts the steps (observables 8.2/8.3 of timing60.md: steps per second, steps per CApp::Frame) and, if
// probe:Game::Init is disabled, does the timing read-back at the first step. It does not read eax/edx
// before writing them (push ebp; mov ebp,[0x2E6FA4]; ...; mov esi,ecx). Declared __fastcall with an unused
// edx (same layout and purge as thiscall without stack args).
std::uint32_t __fastcall FixedStepProbe(std::uint32_t self, std::uint32_t /*edx*/) noexcept {
    g_fixedSteps.fetch_add(1);  // M3d: generation of the tick state (the step's Update runs after this call)
    try {
        if (!g_readBackClaimed.load()) {
            NoteGameInitTiming(0);
        }
        NoteFixedStep();
    } catch (...) {
    }
    return core::CallGuestThiscall(Trampoline(guest::kGameFixedStepHelper), self, {});
}

// CApp+0xC8 = `last` of the fixed-step accumulator (Game::Frame stores now - remainder at 0xA2F6A; startup.md 3.2).
constexpr std::uint32_t kCAppLastTimeOffset = 0xC8;

std::int64_t ReadAppLast() noexcept {
    const guest::GuestAddr app = ReadGuestU32(guest::kGameGamePtr);
    if (app == 0 || !guest::IsReadable(app + kCAppLastTimeOffset, 8)) {
        return 0;
    }
    return guest::Read<std::int64_t>(app + kCAppLastTimeOffset);
}

// Accumulator sample (return 0xA2C6F): the frame clock and the start of the frame's loop phase.
void AccumulatorSample(std::int64_t real, guest::GuestAddr out, bool lockstep) {
    // The guest's own conversion of dt (the timer initialized [0x2EB448] on its first call).
    const auto qpf = guest::Read<std::int64_t>(guest::kGameTimerFrequencyVar);
    const auto dt = std::bit_cast<float>(guest::Read<std::uint32_t>(guest::kGameFrameTimeVar));
    const std::int64_t step = GuestTimerTicks(qpf, dt);
    const std::int64_t cap = GuestTimerTicks(qpf, dt * 6.0f);  // fld dt; fmul 6.0f (0xA2CE8)
    const platform::pacing::FrameAdvance advance = platform::CurrentFrameAdvance();
    const std::int64_t lastPrev = ReadAppLast();
    ClockQuantum quantum;  // unknown plan (the first frame): M3b, one tick per frame
    if (advance.known) {
        quantum.framesPerTick = advance.framesPerTick;
        quantum.ticks = advance.seconds * static_cast<double>(qpf);
        quantum.nominalTicks = advance.nominalSeconds * static_cast<double>(qpf);
        if (quantum.framesPerTick == 0 && !(quantum.ticks >= 1.0)) {
            quantum.framesPerTick = 1;
        }
    }
    std::int64_t value = real;
    FrameClock::Sample sample;
    bool logRegime = false, logAlign = false, firstSample = false;
    if (lockstep && step > 0) {
        std::lock_guard<std::mutex> lock(g_lock);
        const bool wasStarted = g_clock.Started();
        firstSample = !wasStarted;
        // Review finding 11: the sub-tick regimes compare the pacer's frame start (the guest samples its timer only
        // after the streaming pumps) - when it is fresh (EndFrame ran for this frame) and plausible.
        std::int64_t clockReal = real;
        const bool subTickRegime = advance.known && quantum.framesPerTick != 1;
        if (subTickRegime && advance.frameStartQpc > 0 && advance.frameStartQpc != g_clockLastFrameStart &&
            advance.frameStartQpc <= real && real - advance.frameStartQpc < qpf) {
            clockReal = advance.frameStartQpc;
            ++g_clockFrameStarts;
        }
        g_clockLastFrameStart = advance.frameStartQpc;
        quantum.rephase = subTickRegime && wasStarted && advance.planSerial != g_clockPlanSerial;
        g_clockPlanSerial = advance.planSerial;
        sample = g_clock.Advance(clockReal - advance.startDelayTicks, step, quantum, lastPrev, cap);
        value = sample.value;
        g_clockStep = step;
        g_clockQpf = qpf;
        g_clockStartDelays += advance.startDelayTicks != 0 ? 1 : 0;
        logRegime = sample.reset && (g_regimeLogs++ < kMaxClockLogs || !wasStarted);
        logAlign = sample.aligned && g_alignLogs++ < kMaxClockLogs;
    }
    if (lockstep) {
        guest::Write(out, value);
    }
    const GameObserverStats observer = GetGameObserverStats();
    {
        std::lock_guard<std::mutex> lock(g_interpLock);
        InterpState& st = g_interp;
        st.phase = FramePhase::Loop;
        st.stepsAtSample = g_fixedSteps.load();
        st.switchesAtSample = observer.contextSwitches;
        st.accNow = value;
        st.step = step;
        st.capThisFrame = lastPrev != 0 && cap > 0 && value - lastPrev > cap;
        st.framesPerTick = quantum.framesPerTick;
        st.subTick = advance.known && quantum.framesPerTick != 1 && step > 0;
        st.phaseB = lockstep && g_clock.PhaseB();
        // Steps a frame of the regime may run without being a catch-up: 1 at or above the tick rate, else
        // ceil(tick rate / frame rate) (e.g. 2 on a 50 Hz display with the 60 Hz tick).
        st.maxSteps = 1;
        if (quantum.framesPerTick == 0 && dt > 0.0f && advance.nominalSeconds > 0.0) {
            const double perFrame = advance.nominalSeconds / static_cast<double>(dt);
            st.maxSteps = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(perFrame - 1.0e-6)));
        }
        if (lockstep) {
            st.resetThisFrame = sample.reset;
        } else {
            const double nominal = quantum.framesPerTick == 0 ? quantum.nominalTicks : 0.0;
            st.resetThisFrame = !st.haveRegime || st.regimeN != quantum.framesPerTick ||
                                std::fabs(st.regimeNominal - nominal) > 1.0e-6 * std::max(1.0, nominal);
            st.haveRegime = true;
            st.regimeN = quantum.framesPerTick;
            st.regimeNominal = nominal;
        }
    }
    if (logRegime) {
        if (quantum.framesPerTick >= 2) {
            FUT_LOGI(Game, "game clock: %u frames per tick (quanta %lld/%lld ticks of ticks(dt) = %lld; phase B: alpha "
                     "1/%u in the frame that runs the tick) - %s", quantum.framesPerTick,
                     static_cast<long long>(FrameClock::TickQuantum(step, quantum.framesPerTick, 0)),
                     static_cast<long long>(FrameClock::TickQuantum(step, quantum.framesPerTick, 1)),
                     static_cast<long long>(step), quantum.framesPerTick,
                     firstSample ? "clock started" : "frame period changed");
        } else if (quantum.framesPerTick == 1) {
            FUT_LOGI(Game, "game clock: 1 frame per tick (+ticks(dt) = %lld per frame, M3b) - %s",
                     static_cast<long long>(step), advance.known ? "plan known" : "no plan yet");
        } else {
            FUT_LOGI(Game, "game clock: display time per frame (%.1f ticks, nominal %.1f = %.3f Hz; %.3f frames per "
                     "tick) - interpolated", quantum.ticks, quantum.nominalTicks,
                     quantum.nominalTicks > 0.0 ? static_cast<double>(qpf) / quantum.nominalTicks : 0.0,
                     quantum.nominalTicks > 0.0 ? static_cast<double>(step) / quantum.nominalTicks : 0.0);
        }
    }
    if (logAlign) {
        FUT_LOGD(Game, "game clock: remainder aligned to phase B (mu %lld ticks)",
                 static_cast<long long>(FrameClock::PhaseMargin(step)));
    }
}

// Render sample (return 0xA2FBB): alpha and the interpolation values of the frame (game_timing.h FrameInterp).
void RenderSample() {
    const GameObserverStats observer = GetGameObserverStats();
    const std::int64_t lastAfter = ReadAppLast();
    const std::uint64_t ticks = g_fixedSteps.load();
    std::uint32_t steps = 0;
    {
        std::lock_guard<std::mutex> lock(g_interpLock);
        InterpState& st = g_interp;
        steps = static_cast<std::uint32_t>(std::min<std::uint64_t>(ticks - st.stepsAtSample, 1000));
        const bool switched = observer.contextSwitches != st.switchesAtSample;
        const bool catchUp = steps > st.maxSteps;
        FrameInterp f;
        f.tick = ticks;
        f.stepsThisFrame = steps;
        f.enabled = st.subTick;
        f.frame = ++st.renderFrames;
        if (steps != 0) {
            st.pairTicks = steps;
        }
        f.pairTicks = st.pairTicks;
        if (st.subTick && st.step > 0 && lastAfter != 0) {
            f.alpha = FrameAlpha(st.accNow - lastAfter, st.step, st.framesPerTick, st.phaseB);
        }
        if (st.resetThisFrame) {
            f.flags |= kFrameInterpClockReset | kFrameInterpSnapAll;
        }
        if (switched) {
            f.flags |= kFrameInterpSnapAll;
        }
        if (catchUp || st.capThisFrame) {
            // Interpolated over the pair of the last `pairTicks` ticks (review finding 10), not snapped.
            f.flags |= kFrameInterpCatchUp;
        }
        if (st.subTick) {
            ++st.subTickFrames;
            st.alphaHalf += f.alpha == 0.5f ? 1 : 0;
            st.alphaOne += f.alpha == 1.0f ? 1 : 0;
            st.alphaOther += f.alpha != 0.5f && f.alpha != 1.0f ? 1 : 0;
            st.snapSwitch += switched ? 1 : 0;
            st.snapCatchUp += catchUp ? 1 : 0;
            st.snapCap += st.capThisFrame ? 1 : 0;
            st.snapReset += st.resetThisFrame ? 1 : 0;
        }
        st.published = f;
        st.phase = FramePhase::Render;
        st.appFrame = observer.frames + 1;  // the CApp::Frame in progress (counted when it returns)
    }
    platform::NoteFrameTicks(steps);  // F1: frames without a step that draw nothing keep the previous picture
}

// Replaces the timer value of a Game::Frame sample by the frame clock (see game_timing.h) and runs the M3d
// interpolation bookkeeping (both clock modes).
void OnGameFrameTimerSample(guest::GuestAddr returnAddress, guest::GuestAddr out) {
    if (out == 0 || !guest::IsReadable(out, 8)) {
        return;
    }
    const bool lockstep = g_clockMode.load() == FrameClockMode::Lockstep;
    const auto real = guest::Read<std::int64_t>(out);
    if (returnAddress == guest::kGameFrameClockSampleReturn) {
        AccumulatorSample(real, out, lockstep);
        return;
    }
    if (lockstep) {
        std::int64_t value = real;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_clock.Started()) {
                value = g_clock.Now();  // render time of this frame = its accumulator time (multiples of a quantum)
                ++g_clockRenderSamples;
            }
        }
        guest::Write(out, value);
    }
    if (returnAddress == guest::kGameFrameRenderSampleReturn) {
        RenderSample();
    }
}

// The values for the renderer's interpolation (gfx/interp_frame.h source; same flag bits).
static_assert(kFrameInterpSnapAll == gfx::interp::kFrameInterpSnapAll);
static_assert(kFrameInterpInsideStep == gfx::interp::kFrameInterpInsideStep);
static_assert(kFrameInterpClockReset == gfx::interp::kFrameInterpClockReset);
static_assert(kFrameInterpCatchUp == gfx::interp::kFrameInterpCatchUp);
gfx::interp::FrameInterp RendererFrameInterp() noexcept {
    const FrameInterp f = CurrentFrameInterp();
    gfx::interp::FrameInterp r;
    r.tick = f.tick;
    r.stepsThisFrame = f.stepsThisFrame;
    r.pairTicks = f.pairTicks;
    r.alpha = f.alpha;
    r.flags = f.flags;
    r.enabled = f.enabled;
    return r;
}

// FUN_000D2B90 - cdecl void(int64* out), plain ret 0xD2BE6, eax = out (guest.h kGameTimer). The probe calls
// the original (QPF initialization on the first call, *out = QPC) and, for the three samples of Game::Frame
// (return addresses kGameFrameClockSampleReturn / kGameFrameRenderInitSampleReturn /
// kGameFrameRenderSampleReturn), replaces *out by the lockstep clock ([game] frame_clock = lockstep). All other
// callers get the original value. The original reads only its stack argument (sub esp,8; mov ecx,[0x2EB44C];
// xor eax,eax ...), so the host frame in between is transparent; the return address is the guest's because
// the hook jmp pushes nothing (same as probe:Game::ContextSwitch).
std::uint32_t __cdecl GameTimerProbe(std::uint32_t out) noexcept {
    const std::uint32_t eax = core::CallGuest(Trampoline(guest::kGameTimer), {out});
    const guest::GuestAddr ret = guest::Addr(_ReturnAddress());
    if (ret == guest::kGameFrameClockSampleReturn || ret == guest::kGameFrameRenderInitSampleReturn ||
        ret == guest::kGameFrameRenderSampleReturn) {
        try {
            OnGameFrameTimerSample(ret, out);
        } catch (...) {
        }
    }
    return eax;
}

}  // namespace

// ---- frame clock ------------------------------------------------------------------------------------------

bool ParseFrameClock(std::string_view value, FrameClockMode& mode) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    auto equalsNoCase = [&](std::string_view word) {
        if (value.size() != word.size()) {
            return false;
        }
        for (std::size_t i = 0; i < word.size(); ++i) {
            const char c = value[i] >= 'A' && value[i] <= 'Z' ? static_cast<char>(value[i] - 'A' + 'a') : value[i];
            if (c != word[i]) {
                return false;
            }
        }
        return true;
    };
    if (equalsNoCase("lockstep")) {
        mode = FrameClockMode::Lockstep;
        return true;
    }
    if (equalsNoCase("realtime")) {
        mode = FrameClockMode::Realtime;
        return true;
    }
    return false;
}

bool ConfiguredFrameClock(const core::Config& config, FrameClockMode& mode) noexcept {
    try {
        if (!config.Has("game", "frame_clock")) {
            mode = FrameClockMode::Lockstep;
            return true;
        }
        return ParseFrameClock(config.GetString("game", "frame_clock"), mode);
    } catch (...) {
        return false;
    }
}

const char* FrameClockName(FrameClockMode mode) noexcept {
    return mode == FrameClockMode::Lockstep ? "lockstep" : "realtime";
}

FrameClockMode ActiveFrameClock() noexcept {
    return g_clockMode.load();
}

std::int64_t GuestTimerTicks(std::int64_t qpf, float seconds) noexcept {
    if (qpf <= 0 || !(seconds > 0.0f)) {
        return 0;
    }
    // fild qword [QPF]; fmul dword [s]; _ftol (truncation) - FUN_000D2B50. Double carries the product of a
    // 64-bit integer below 2^53 and a float exactly enough for the truncation (no integer within 1e-6).
    return static_cast<std::int64_t>(static_cast<double>(qpf) * static_cast<double>(seconds));
}

std::int64_t LockstepClock::Advance(std::int64_t real, std::int64_t step) noexcept {
    ++stats_.samples;
    if (!started_ || step <= 0) {
        started_ = true;
        virtual_ = real;
        return virtual_;
    }
    const std::int64_t margin = step * 3 / 4;
    const std::int64_t d = real - virtual_;
    std::int64_t n = 1;
    if (d - step > margin) {
        n = (d - margin + step - 1) / step;  // smallest n with d - n * step <= margin
    } else if (d - step < -margin) {
        n = d + margin < 0 ? 0 : (d + margin) / step;  // largest n with d - n * step >= -margin
    }
    virtual_ += n * step;
    if (n == 1) {
        ++stats_.single;
    } else if (n == 0) {
        ++stats_.holds;
    } else {
        ++stats_.catchUps;
        stats_.maxCatchUpSteps = std::max(stats_.maxCatchUpSteps, n);
    }
    const std::int64_t phase = real - virtual_;
    stats_.minPhase = std::min(stats_.minPhase, phase);
    stats_.maxPhase = std::max(stats_.maxPhase, phase);
    return virtual_;
}

// ---- M3d: frame clock in quanta -----------------------------------------------------------------------------------

std::int64_t FrameClock::PhaseMargin(std::int64_t step) noexcept {
    return std::max<std::int64_t>(16, step / 1024);
}

std::int64_t FrameClock::TickQuantum(std::int64_t step, std::uint32_t n, std::uint32_t i) noexcept {
    if (n <= 1) {
        return step;
    }
    i %= n;
    const auto a = static_cast<std::int64_t>(i);
    const auto nn = static_cast<std::int64_t>(n);
    return (a + 1) * step / nn - a * step / nn;
}

std::int64_t FrameClock::PeekQuantum(std::int64_t step) const noexcept {
    if (regimeN_ >= 1) {
        return TickQuantum(step, regimeN_, phase_);
    }
    const auto q = static_cast<std::int64_t>(std::floor(fraction_ + ticks_));
    return q >= 1 ? q : 1;
}

void FrameClock::Consume(std::int64_t step) noexcept {
    if (regimeN_ >= 2) {
        phase_ = (phase_ + 1) % regimeN_;
    } else if (regimeN_ == 0) {
        fraction_ += ticks_ - static_cast<double>(PeekQuantum(step));
        if (fraction_ < 0.0 || fraction_ >= 1.0) {
            fraction_ -= std::floor(fraction_);
        }
    }
}

FrameClock::Sample FrameClock::Advance(std::int64_t realIn, std::int64_t step, const ClockQuantum& quantum,
                                       std::int64_t lastPrev, std::int64_t capTicks) noexcept {
    Sample out;
    ++stats_.samples;
    std::int64_t real = realIn;
    std::uint32_t n = quantum.framesPerTick;
    if (n == 0 && !(quantum.ticks >= 1.0)) {
        n = 1;  // no usable display time: one tick per frame (M3b)
    }
    const double nominal =
        n == 0 ? (quantum.nominalTicks >= 1.0 ? quantum.nominalTicks : quantum.ticks) : 0.0;
    if (!started_ || step <= 0) {
        // First sample (M3b: the virtual time starts at real time). The remainder is aligned in the next sample.
        started_ = true;
        virtual_ = real;
        offset_ = 0;
        regimeN_ = n;
        regimeStep_ = step;
        regimeNominal_ = nominal;
        phase_ = 0;
        fraction_ = 0.0;
        ticks_ = quantum.ticks;
        alignPending_ = n >= 2;
        phaseB_ = false;
        out.value = real;
        out.reset = true;
        return out;
    }
    if (n != regimeN_ || step != regimeStep_ ||
        (n == 0 && std::fabs(nominal - regimeNominal_) > 1.0e-6 * std::max(1.0, nominal))) {
        // New regime (plan with another frame period, other tick rate): the clock stays continuous, the remainder is
        // aligned again (N >= 2) and the frame is reported as a reset (the renderer snaps).
        ++stats_.resets;
        out.reset = true;
        regimeN_ = n;
        regimeStep_ = step;
        regimeNominal_ = nominal;
        phase_ = 0;
        fraction_ = 0.0;
        alignPending_ = n >= 2;
        phaseB_ = false;
        if (n == 1) {
            offset_ = 0;  // the M3b clock has no offset
        }
    }
    ticks_ = quantum.ticks;
    const std::int64_t previous = virtual_;
    // Margin 3/4 of the nominal quantum (N = 1: step * 3 / 4 exactly as M3b).
    const std::int64_t nominalQuantum = n >= 1 ? step / static_cast<std::int64_t>(n) : std::llround(nominal);
    const std::int64_t margin = n == 1 ? step * 3 / 4 : nominalQuantum * 3 / 4;
    const std::int64_t q0 = PeekQuantum(step);
    if (quantum.rephase && n != 1) {
        // A pacing plan change moved the frame starts to another grid (review finding 11): absorb the jump (at most
        // one tick) so that this frame advances by exactly one quantum.
        const std::int64_t jump = real - offset_ - virtual_ - q0;
        if (jump != 0 && jump >= -step && jump <= step) {
            offset_ += jump;
            ++stats_.rephases;
            out.rephased = true;
        }
    }
    real -= offset_;
    const std::int64_t d = real - virtual_;
    std::int64_t count = 0;
    if (d - q0 > margin) {
        // Catch up: the smallest number of quanta that brings real - V to <= margin.
        if (n == 1) {
            count = (d - margin + step - 1) / step;
            virtual_ += count * step;
        } else {
            if (n >= 2) {
                const std::int64_t cycles = (d - margin) / step - 1;  // whole ticks first (N quanta = ticks(dt))
                if (cycles > 0) {
                    virtual_ += cycles * step;
                    count += cycles * static_cast<std::int64_t>(n);
                }
            } else if (nominalQuantum > 0) {
                const std::int64_t bulk = (d - margin) / nominalQuantum - 2;
                if (bulk > 0) {
                    const double total = fraction_ + static_cast<double>(bulk) * ticks_;
                    const double whole = std::floor(total);
                    virtual_ += static_cast<std::int64_t>(whole);
                    fraction_ = total - whole;
                    count += bulk;
                }
            }
            while (real - virtual_ > margin && count < 100'000'000) {
                virtual_ += PeekQuantum(step);
                Consume(step);
                ++count;
            }
        }
    } else if (d - q0 >= -margin) {
        virtual_ += q0;
        Consume(step);
        count = 1;
    }
    // else: hold (more than 3/4 quantum ahead of real time; never behind the previous frame)
    out.quanta = count;
    if (count == 1) {
        ++stats_.single;
    } else if (count == 0) {
        ++stats_.holds;
    } else {
        ++stats_.catchUps;
        stats_.maxCatchUpQuanta = std::max(stats_.maxCatchUpQuanta, count);
    }
    // Phase B (N >= 2): the guest's remainder r = V - CApp+0xC8 after its step loop is congruent to V - lastPrev
    // modulo ticks(dt) and so modulo the quantum. Move V once so that r = ticks(dt) - mu (mod quantum): the largest
    // remainder of the pattern is then ticks(dt) - mu (m3d_frameclock.md 2.3). Candidates delta + k * quantum (k -1, 0,
    // 1): the one closest to real time that does not move V behind the previous frame.
    // An elapsed time outside (0, 5 ticks) (the guest's 6 dt cap applies in this frame, or an unknown CApp+0xC8)
    // keeps the alignment pending for the next frame.
    if (alignPending_ && n >= 2 && lastPrev != 0) {
        const std::int64_t el = virtual_ - lastPrev;
        if (el > 0 && el < 5 * step) {
            alignPending_ = false;
            const double q = static_cast<double>(step) / static_cast<double>(n);
            const std::int64_t mu = PhaseMargin(step);
            const double phi = std::fmod(static_cast<double>(step - mu), q);
            double delta = std::fmod(phi - static_cast<double>(el), q);
            if (delta > q / 2.0) {
                delta -= q;
            } else if (delta <= -q / 2.0) {
                delta += q;
            }
            const std::int64_t base = std::llround(delta);
            if (std::llabs(base) > mu / 2) {
                bool found = false;
                std::int64_t best = 0;
                for (int k = -1; k <= 1; ++k) {
                    const std::int64_t c = base + std::llround(static_cast<double>(k) * q);
                    if (virtual_ + c < previous) {
                        continue;
                    }
                    if (!found || std::llabs(real - (virtual_ + c)) < std::llabs(real - (virtual_ + best))) {
                        best = c;
                        found = true;
                    }
                }
                if (found) {
                    virtual_ += best;
                    ++stats_.aligns;
                    out.aligned = true;
                }
            }
            phaseB_ = true;
        }
    }
    // The guest caps the elapsed time at 6 dt (0xA2CE8..0xA2D1F): its remainder restarts near the step boundary, so
    // the next frame aligns again.
    if (capTicks > 0 && lastPrev != 0 && virtual_ - lastPrev > capTicks) {
        alignPending_ = n >= 2;
        phaseB_ = false;
    }
    const std::int64_t phase = real - virtual_;
    stats_.minPhase = std::min(stats_.minPhase, phase);
    stats_.maxPhase = std::max(stats_.maxPhase, phase);
    out.value = virtual_;
    return out;
}

float FrameAlpha(std::int64_t rest, std::int64_t step, std::uint32_t framesPerTick, bool phaseB) noexcept {
    if (step <= 0) {
        return 1.0f;
    }
    const std::int64_t largest = step - (phaseB ? FrameClock::PhaseMargin(step) : 0);
    double a = 1.0 - static_cast<double>(largest - rest) / static_cast<double>(step);
    a = std::clamp(a, 0.0, 1.0);
    if (framesPerTick >= 2) {
        const double n = static_cast<double>(framesPerTick);
        const double j = std::floor(a * n + 0.5);
        if (std::fabs(a - j / n) < 0.01) {
            a = j / n;  // the exact 1/N grid (phase B: 1/2 and 1)
        }
    }
    if (a >= 0.999) {
        a = 1.0;
    }
    return static_cast<float>(a);
}

FrameInterp CurrentFrameInterp() noexcept {
    const std::uint64_t frames = GetGameObserverStats().frames;
    std::lock_guard<std::mutex> lock(g_interpLock);
    const InterpState& st = g_interp;
    if (st.phase == FramePhase::Render && frames < st.appFrame) {
        return st.published;
    }
    // Inside the step loop (draws from Update) or outside Game::Frame: the guest data as is.
    FrameInterp f;
    f.tick = g_fixedSteps.load();
    f.stepsThisFrame = st.phase == FramePhase::Loop ? static_cast<std::uint32_t>(f.tick - st.stepsAtSample) : 0u;
    f.pairTicks = st.pairTicks;
    f.flags = kFrameInterpInsideStep;
    f.enabled = st.subTick;
    f.frame = st.renderFrames;
    return f;
}

bool SubTickFramesActive() noexcept {
    std::lock_guard<std::mutex> lock(g_interpLock);
    return g_interp.subTick;
}

bool RenderingFrameWithoutStep() noexcept {
    const std::uint64_t frames = GetGameObserverStats().frames;
    std::lock_guard<std::mutex> lock(g_interpLock);
    const InterpState& st = g_interp;
    return st.subTick && st.phase == FramePhase::Render && frames < st.appFrame && st.published.stepsThisFrame == 0;
}

FrameClockStats GetFrameClockStats() noexcept {
    FrameClockStats s;
    s.mode = g_clockMode.load();
    {
        std::lock_guard<std::mutex> lock(g_lock);
        s.step = g_clockStep;
        s.qpf = g_clockQpf;
        const FrameClock::Stats& c = g_clock.GetStats();
        s.clock.samples = c.samples;
        s.clock.single = c.single;
        s.clock.catchUps = c.catchUps;
        s.clock.holds = c.holds;
        s.clock.maxCatchUpSteps = c.maxCatchUpQuanta;
        s.clock.minPhase = c.minPhase;
        s.clock.maxPhase = c.maxPhase;
        s.renderSamples = g_clockRenderSamples;
        s.framesPerTick = g_clock.FramesPerTick();
        s.resets = c.resets;
        s.aligns = c.aligns;
        s.startDelays = g_clockStartDelays;
        s.frameStartSamples = g_clockFrameStarts;
        s.rephases = c.rephases;
    }
    std::lock_guard<std::mutex> lock(g_interpLock);
    const InterpState& st = g_interp;
    s.renderedFrames = st.renderFrames;
    s.subTickFrames = st.subTickFrames;
    s.alphaHalf = st.alphaHalf;
    s.alphaOne = st.alphaOne;
    s.alphaOther = st.alphaOther;
    s.snapSwitch = st.snapSwitch;
    s.snapCatchUp = st.snapCatchUp;
    s.snapCap = st.snapCap;
    s.snapReset = st.snapReset;
    return s;
}

// ---- configuration --------------------------------------------------------------------------------------

std::uint32_t ParseTickRate(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    if (value == "60") {
        return kTickRateNtsc;
    }
    if (value == "50") {
        return kTickRatePal;
    }
    return 0;
}

std::uint32_t ConfiguredTickRate(const core::Config& config) noexcept {
    try {
        if (!config.Has("game", "tick_rate")) {
            return kDefaultTickRate;
        }
        return ParseTickRate(config.GetString("game", "tick_rate"));
    } catch (...) {
        return 0;
    }
}

std::uint32_t ActiveTickRate() noexcept {
    return g_activeRate.load();
}

// ---- patch table ----------------------------------------------------------------------------------------

std::span<const TimingPatch> TimingPatches(std::uint32_t tickRate) noexcept {
    if (tickRate == kTickRateNtsc) {
        return kPatches60;
    }
    return {};
}

std::span<const TimingPatch> TimingReferenceRows() noexcept {
    return kReferences;
}

std::span<const TimingPatch> AppliedTimingPatches() noexcept {
    return g_patchesApplied.load() ? TimingPatches(kTickRateNtsc) : std::span<const TimingPatch>{};
}

bool ValidateTimingPatchTable(std::span<const TimingPatch> patches, std::span<const TimingPatch> references,
                              std::string& error) {
    std::vector<const TimingPatch*> all;
    for (const TimingPatch& p : patches) {
        all.push_back(&p);
    }
    for (const TimingPatch& p : references) {
        all.push_back(&p);
    }
    for (std::size_t index = 0; index < all.size(); ++index) {
        const TimingPatch& p = *all[index];
        const bool isPatchRow = p.kind != TimingPatchKind::Reference;
        const std::string where = Printf("row 0x%08X (%s, %s)", p.address, KindName(p.kind), p.name);
        if (p.length == 0 || p.length > kMaxPatchBytes) {
            error = where + Printf(": length %u (original and patched encodings must have the same length 1..%u)",
                                   p.length, kMaxPatchBytes);
            return false;
        }
        const bool inPatches = index < patches.size();
        if (inPatches != isPatchRow) {
            error = where + ": kind does not fit its table (patch rows Code/Data, references Reference)";
            return false;
        }
        std::uint32_t changed = 0;
        for (std::uint32_t i = 0; i < p.length; ++i) {
            changed += p.original[i] != p.patched[i] ? 1u : 0u;
        }
        for (std::uint32_t i = p.length; i < kMaxPatchBytes; ++i) {
            if (p.original[i] != 0 || p.patched[i] != 0) {
                error = where + ": bytes after the row length must be zero";
                return false;
            }
        }
        if (!isPatchRow) {
            if (changed != 0) {
                error = where + ": a reference row must not change bytes";
                return false;
            }
        } else if (changed == 0) {
            error = where + ": the row changes no byte";
            return false;
        }
        if (p.kind == TimingPatchKind::Code) {
            std::uint32_t begin = 0, end = 0;
            if (!CodeOperandRange(p, begin, end)) {
                error = where + ": unsupported instruction (only fld m32 and mov m32,imm32 operands are patched)";
                return false;
            }
            for (std::uint32_t i = 0; i < p.length; ++i) {
                if ((i < begin || i >= end) && p.original[i] != p.patched[i]) {
                    error = where + Printf(": byte %u outside the operand changes", i);
                    return false;
                }
            }
        }
        if (p.kind == TimingPatchKind::Data && p.length != 4) {
            error = where + ": a data row is one dword";
            return false;
        }
        for (const auto& site : guest::FsPatchSites()) {
            if (Overlaps(site.address, site.length, p.address, p.length)) {
                error = where + Printf(": overlaps the fs patch site 0x%08X", site.address);
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            if (Overlaps(all[i]->address, all[i]->length, all[j]->address, all[j]->length)) {
                error = Printf("rows 0x%08X (%s) and 0x%08X (%s) overlap", all[i]->address, all[i]->name,
                               all[j]->address, all[j]->name);
                return false;
            }
        }
    }
    return true;
}

bool ApplyTimingPatches(std::span<const TimingPatch> patches, std::span<const TimingPatch> references,
                        PatchMemory& memory, std::string& error) {
    if (!ValidateTimingPatchTable(patches, references, error)) {
        return false;
    }
    // 1. Every row (references first) must hold its original bytes in the file and in the image.
    auto check = [&](const TimingPatch& p) {
        std::uint8_t current[kMaxPatchBytes] = {};
        std::uint8_t file[kMaxPatchBytes] = {};
        const std::string where = Printf("0x%08X (%s)", p.address, p.name);
        if (!memory.ReadOriginal(p.address, file, p.length)) {
            error = where + ": XBE file bytes are not available (XBE not loaded or address outside the image)";
            return false;
        }
        if (std::memcmp(file, p.original.data(), p.length) != 0) {
            error = where + ": the XBE file holds " + Hex(file, p.length) + ", expected " +
                    Hex(p.original.data(), p.length) + " (different XBE build)";
            return false;
        }
        if (!memory.ReadCurrent(p.address, current, p.length)) {
            error = where + ": not readable in the mapped image";
            return false;
        }
        if (std::memcmp(current, p.original.data(), p.length) != 0) {
            const bool already = p.kind != TimingPatchKind::Reference &&
                                 std::memcmp(current, p.patched.data(), p.length) == 0;
            error = where + ": the image holds " + Hex(current, p.length) + ", expected " +
                    Hex(p.original.data(), p.length) +
                    (already ? " (already patched)" : " (changed by another patch or hook)");
            return false;
        }
        return true;
    };
    for (const TimingPatch& p : references) {
        if (!check(p)) {
            return false;
        }
    }
    for (const TimingPatch& p : patches) {
        if (!check(p)) {
            return false;
        }
    }
    // 2. Write, 3. read back.
    for (const TimingPatch& p : patches) {
        if (!memory.Write(p.address, p.patched.data(), p.length)) {
            error = Printf("0x%08X (%s): write failed", p.address, p.name);
            return false;
        }
    }
    for (const TimingPatch& p : patches) {
        std::uint8_t current[kMaxPatchBytes] = {};
        if (!memory.ReadCurrent(p.address, current, p.length) ||
            std::memcmp(current, p.patched.data(), p.length) != 0) {
            error = Printf("0x%08X (%s): read-back after the write differs", p.address, p.name);
            return false;
        }
    }
    return true;
}

bool StageApplyGameTiming(const core::Config& config) {
    try {
        const std::uint32_t rate = ConfiguredTickRate(config);
        if (rate == 0) {
            FUT_LOGE(Game, "game timing: invalid [game] tick_rate '%s' (expected 60 or 50)",
                     config.GetString("game", "tick_rate").c_str());
            return false;
        }
        FrameClockMode clock = FrameClockMode::Lockstep;
        if (!ConfiguredFrameClock(config, clock)) {
            FUT_LOGE(Game, "game timing: invalid [game] frame_clock '%s' (expected lockstep or realtime)",
                     config.GetString("game", "frame_clock").c_str());
            return false;
        }
        core::AddExitReporter("game timing", &LogGameTickSummary);
        RegisterFrameFixReporter();  // M3d: "frame fixes: ..." (xapi/frame_fixes.h)
        const auto patches = TimingPatches(rate);
        if (!patches.empty()) {
            GuestImageMemory memory;
            std::string error;
            if (!ApplyTimingPatches(patches, TimingReferenceRows(), memory, error)) {
                FUT_LOGE(Game, "game timing: %u Hz patch refused, nothing written: %s", rate, error.c_str());
                return false;
            }
            g_patchesApplied.store(true);
            std::uint32_t code = 0, data = 0;
            for (const TimingPatch& p : patches) {
                (p.kind == TimingPatchKind::Code ? code : data) += 1;
                FUT_LOGD(Game, "game timing patch 0x%08X %s: %s -> %s", p.address, p.name,
                         Hex(p.original.data(), p.length).c_str(), Hex(p.patched.data(), p.length).c_str());
            }
            FUT_LOGI(Game,
                     "game timing: %u Hz (%s) - [game] tick_rate = %u; %zu patches applied: %u operands in the "
                     "PAL branch of Game::Init 0x%08X, %u .data initial values (fps 0x%08X, dt 0x%08X); "
                     "%zu reference rows verified; CApp+0xE8 and the \"PAL\" movie tag unchanged",
                     rate, TimingDescription(rate), rate, patches.size(), code, guest::kGameInit, data,
                     guest::kGameFpsVar, guest::kGameFrameTimeVar, TimingReferenceRows().size());
        } else {
            FUT_LOGI(Game,
                     "game timing: %u Hz (%s) - [game] tick_rate = %u; no patches, Game::Init 0x%08X keeps fps "
                     "50 and the fixed time step 0.02 s",
                     rate, TimingDescription(rate), rate, guest::kGameInit);
        }
        g_activeRate.store(rate);
        g_clockMode.store(clock);
        FUT_LOGI(Game,
                 "game clock: %s - [game] frame_clock = %s; %s", FrameClockName(clock), FrameClockName(clock),
                 clock == FrameClockMode::Lockstep
                     ? "the timer samples of Game::Frame (returns 0x000A2C6F/0x000A2FA6/0x000A2FBB) advance by "
                       "ticks(dt) per frame, whole-step corrections beyond 3/4 step; other callers keep QPC; M3d: "
                       "above the tick rate by the frame's share of a tick (half a tick at 120 fps)"
                     : "Game::Frame samples the original QPC timer");
        // M3d: the renderer's interpolation reads the frame's alpha and tick from this module.
        gfx::interp::SetFrameInterpSource(&RendererFrameInterp);
        return true;
    } catch (const std::exception& e) {
        FUT_LOGE(Game, "game timing: exception %s", e.what());
    } catch (...) {
        FUT_LOGE(Game, "game timing: unknown exception");
    }
    return false;
}

// ---- read-back ------------------------------------------------------------------------------------------

TimingGlobals ExpectedTimingGlobals(std::uint32_t tickRate) noexcept {
    const bool ntsc = tickRate == kTickRateNtsc;
    TimingGlobals v;
    v.fps = ntsc ? kF32Fps60 : kF32Fps50;
    v.dt = ntsc ? kF32Dt60 : kF32Dt50;
    v.dtMin = v.dt;
    v.dtMax = v.dt;
    v.fpsCopy1 = v.fps;
    v.fpsCopy2 = v.fps;
    v.frameTime = ntsc ? kF32FrameTimeNtsc : kF32FrameTimePal;
    v.fixedStep = 1;
    v.videoStandard = 0;  // PAL content in both modes
    v.movieTag = {'P', 'A', 'L', '\0', '\0', '\0', '\0', '\0'};
    return v;
}

std::string DiffTimingGlobals(const TimingGlobals& actual, const TimingGlobals& expected) {
    std::string out;
    auto add = [&](bool differs, const char* name) {
        if (differs) {
            out += (out.empty() ? "" : ", ");
            out += name;
        }
    };
    add(actual.fps != expected.fps, "fps");
    add(actual.dt != expected.dt, "dt");
    add(actual.dtMin != expected.dtMin, "dt min");
    add(actual.dtMax != expected.dtMax, "dt max");
    add(actual.fpsCopy1 != expected.fpsCopy1, "fps copy 0x2E7A60");
    add(actual.fpsCopy2 != expected.fpsCopy2, "fps copy 0x2E7B6C");
    add(actual.frameTime != expected.frameTime, "0x2E7B68");
    add(actual.fixedStep != expected.fixedStep, "fixed step");
    add(actual.videoStandard != expected.videoStandard, "CApp+0xE8");
    add(std::strncmp(actual.movieTag.data(), expected.movieTag.data(), actual.movieTag.size()) != 0, "movie tag");
    return out;
}

GameTimingReadBack GetGameTimingReadBack() noexcept {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_readBack;
}

void NoteGameInitTiming(guest::GuestAddr app) noexcept {
    if (g_readBackClaimed.exchange(true)) {
        return;
    }
    try {
        const std::uint32_t active = g_activeRate.load();
        const std::uint32_t rate = active != 0 ? active : kTickRatePal;  // no stage: the image is unpatched
        const TimingGlobals v = ReadTimingGlobals(app);
        const TimingGlobals expected = ExpectedTimingGlobals(rate);
        const std::string diff = DiffTimingGlobals(v, expected);
        GameTimingReadBack r;
        r.valid = true;
        r.tickRate = rate;
        r.values = v;
        r.matchesExpected = diff.empty();
        r.fps = std::bit_cast<float>(v.fps);
        r.dt = std::bit_cast<float>(v.dt);
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_readBack = r;
        }
        const std::string text = Printf(
            "game timing: effective after Game::Init: tick %u Hz, fps %.0f (0x%08X), dt %.9g s (0x%08X), fixed "
            "step %u, dt clamps 0x%08X/0x%08X, fps copies 0x%08X/0x%08X, [0x%08X] 0x%08X, CApp+0xE8 = %u, movie "
            "tag \"%s\"",
            rate, static_cast<double>(r.fps), v.fps, static_cast<double>(r.dt), v.dt, v.fixedStep, v.dtMin,
            v.dtMax, v.fpsCopy1, v.fpsCopy2, guest::kGameVideoFrameTimeVar, v.frameTime, v.videoStandard,
            v.movieTag.data());
        if (r.matchesExpected) {
            FUT_LOGI(Game, "%s - ok (%s)", text.c_str(), TimingDescription(rate));
        } else {
            FUT_LOGE(Game, "%s - MISMATCH in %s (expected the %u Hz values: fps 0x%08X, dt 0x%08X, CApp+0xE8 0, "
                           "tag \"PAL\")",
                     text.c_str(), diff.c_str(), rate, expected.fps, expected.dt);
        }
    } catch (...) {
    }
}

// ---- steps per frame ------------------------------------------------------------------------------------

void StepHistogram::OnStep(std::uint64_t frame) noexcept {
    if (frame > currentFrame_) {
        if (currentFrame_ != 0) {
            closed_[std::min<std::uint32_t>(stepsInCurrent_, kBuckets - 1)] += 1;
            closed_[0] += frame - currentFrame_ - 1;  // frames in between ran no step
        }
        currentFrame_ = frame;
        stepsInCurrent_ = 0;
    }
    ++stepsInCurrent_;
    maxSteps_ = std::max(maxSteps_, stepsInCurrent_);
}

std::array<std::uint64_t, StepHistogram::kBuckets> StepHistogram::Snapshot(
    std::uint64_t completedFrames) const noexcept {
    std::array<std::uint64_t, kBuckets> h = closed_;
    if (currentFrame_ != 0 && completedFrames >= currentFrame_) {
        h[std::min<std::uint32_t>(stepsInCurrent_, kBuckets - 1)] += 1;
        h[0] += completedFrames - currentFrame_;
    }
    return h;
}

GameTickStats GetGameTickStats() noexcept {
    GameTickStats t;
    const std::uint64_t completed = GetGameObserverStats().frames;
    const double dt = StatsDt();
    std::lock_guard<std::mutex> lock(g_lock);
    const StepState& s = g_steps;
    t.steps = s.steps;
    t.frames = completed;
    const double qpf = static_cast<double>(Qpf());
    t.wallSeconds = s.steps > 1 ? static_cast<double>(s.lastQpc - s.firstQpc) / qpf : 0.0;
    t.gameSeconds = static_cast<double>(s.steps) * dt;
    t.stepsPerSecond = t.wallSeconds > 0.0 ? static_cast<double>(s.steps - 1) / t.wallSeconds : 0.0;
    t.stalls = s.stalls;
    t.stallSeconds = static_cast<double>(s.stallTicks) / qpf;
    const double steadyWall = t.wallSeconds - t.stallSeconds;
    t.steadyStepsPerSecond = steadyWall > 0.0 && s.steps > 1 ? static_cast<double>(s.steps - 1) / steadyWall : 0.0;
    t.stepsPerFrame = s.histogram.Snapshot(completed);
    t.maxStepsPerFrame = s.histogram.MaxStepsPerFrame();
    return t;
}

void LogGameTickSummary() noexcept {
    try {
        const GameTickStats t = GetGameTickStats();
        const std::uint32_t rate = g_activeRate.load();
        FUT_LOGI(Game,
                 "game ticks: %llu fixed steps in %.3f s (%.3f steps/s at tick rate %u Hz; game time %.3f s; %u "
                 "stalls > 6 dt, about %.3f s dropped by the guest's cap: steady %.3f steps/s), steps per "
                 "CApp::Frame over %llu frames: 0 x %llu, 1 x %llu, 2 x %llu, 3+ x %llu (max %u)",
                 static_cast<unsigned long long>(t.steps), t.wallSeconds, t.stepsPerSecond, rate, t.gameSeconds,
                 t.stalls, t.stallSeconds, t.steadyStepsPerSecond, static_cast<unsigned long long>(t.frames),
                 static_cast<unsigned long long>(t.stepsPerFrame[0]),
                 static_cast<unsigned long long>(t.stepsPerFrame[1]),
                 static_cast<unsigned long long>(t.stepsPerFrame[2]),
                 static_cast<unsigned long long>(t.stepsPerFrame[3]), t.maxStepsPerFrame);
        const FrameClockStats c = GetFrameClockStats();
        const double tickMs = c.qpf > 0 ? 1000.0 / static_cast<double>(c.qpf) : 0.0;
        FUT_LOGI(Game,
                 "game clock summary: %s, %llu accumulator samples: +1 step x %llu, catch-up x %llu (max +%lld steps), "
                 "hold x %llu; phase real - virtual %.3f..%.3f ms; %llu render samples; ticks(dt) = %lld at QPF "
                 "%lld",
                 FrameClockName(c.mode), static_cast<unsigned long long>(c.clock.samples),
                 static_cast<unsigned long long>(c.clock.single), static_cast<unsigned long long>(c.clock.catchUps),
                 static_cast<long long>(c.clock.maxCatchUpSteps), static_cast<unsigned long long>(c.clock.holds),
                 static_cast<double>(c.clock.minPhase) * tickMs, static_cast<double>(c.clock.maxPhase) * tickMs,
                 static_cast<unsigned long long>(c.renderSamples), static_cast<long long>(c.step),
                 static_cast<long long>(c.qpf));
        // M3d (frame rates above the tick rate; quanta instead of steps in the line above when N != 1).
        char regime[48];
        if (c.framesPerTick != 0) {
            std::snprintf(regime, sizeof(regime), "%u frame%s per tick", c.framesPerTick, c.framesPerTick == 1 ? "" : "s");
        } else {
            std::snprintf(regime, sizeof(regime), "display time per frame");
        }
        FUT_LOGI(Game,
                 "game clock M3d: %s; %llu regime changes, %llu phase B alignments, %llu samples after a just-in-time "
                 "start; %llu rendered frames, %llu between ticks enabled: alpha 1/2 x %llu, 1 x %llu, other x %llu; "
                 "snap-all: context switch %llu, clock reset %llu; interpolated over the step pair: catch-up %llu, 6 dt "
                 "cap %llu; clock input: frame start x %llu, plan-change rephase x %llu",
                 regime, static_cast<unsigned long long>(c.resets), static_cast<unsigned long long>(c.aligns),
                 static_cast<unsigned long long>(c.startDelays), static_cast<unsigned long long>(c.renderedFrames),
                 static_cast<unsigned long long>(c.subTickFrames), static_cast<unsigned long long>(c.alphaHalf),
                 static_cast<unsigned long long>(c.alphaOne), static_cast<unsigned long long>(c.alphaOther),
                 static_cast<unsigned long long>(c.snapSwitch), static_cast<unsigned long long>(c.snapReset),
                 static_cast<unsigned long long>(c.snapCatchUp), static_cast<unsigned long long>(c.snapCap),
                 static_cast<unsigned long long>(c.frameStartSamples), static_cast<unsigned long long>(c.rephases));
    } catch (...) {
    }
}

}  // namespace xapi

// ---- self-test ---------------------------------------------------------------------------------------------

// After the stage: every row of the 60 Hz table holds its patched bytes (tick rate 60) or its original bytes
// (50), every reference row is unchanged, and no installed hook overlaps a row.
FUT_SELFTEST(GameTimingPatched, Mapped) {
    const std::uint32_t rate = xapi::ActiveTickRate();
    if (rate == 0) {
        detail = "stage ApplyGameTiming did not run";
        return false;
    }
    std::uint32_t good = 0, total = 0;
    auto check = [&](const xapi::TimingPatch& p, bool patched) {
        ++total;
        const auto& expected = patched ? p.patched : p.original;
        if (std::memcmp(guest::Ptr<const void>(p.address), expected.data(), p.length) != 0) {
            if (detail.empty()) {
                char buf[192];
                std::snprintf(buf, sizeof(buf), "0x%08X (%s) does not hold its %s bytes", p.address, p.name,
                              patched ? "patched" : "original");
                detail = buf;
            }
            return;
        }
        for (const core::InstalledHook& h : core::InstalledHookList()) {
            const std::uint32_t n = h.trampolineBytes != 0 ? h.trampolineBytes : 5u;
            if (h.address < p.address + p.length && p.address < h.address + n) {
                if (detail.empty()) {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf), "hook %s at 0x%08X overlaps 0x%08X (%s)", h.name, h.address,
                                  p.address, p.name);
                    detail = buf;
                }
                return;
            }
        }
        ++good;
    };
    for (const xapi::TimingPatch& p : xapi::TimingPatches(xapi::kTickRateNtsc)) {
        check(p, rate == xapi::kTickRateNtsc);
    }
    for (const xapi::TimingPatch& p : xapi::TimingReferenceRows()) {
        check(p, false);
    }
    const std::size_t applied = xapi::AppliedTimingPatches().size();
    const std::size_t expectedApplied = xapi::TimingPatches(rate).size();
    if (detail.empty()) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%u Hz: %u/%u rows as expected (%zu patched, %zu references), no hook overlap",
                      rate, good, total, applied, xapi::TimingReferenceRows().size());
        detail = buf;
    }
    return good == total && applied == expectedApplied;
}

FUT_PROBE(guest::kGameFixedStepHelper, &xapi::FixedStepProbe, Thiscall, "probe:Game::FixedStep",
          "55 8B 2D A4 6F 2E 00", 7);
FUT_PROBE(guest::kGameTimer, &xapi::GameTimerProbe, Cdecl, "probe:Game::Timer", "83 EC 08 8B 0D 4C B4 2E 00", 9);
