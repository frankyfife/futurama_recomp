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
#include "xapi/game_probes.h"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdarg>
#include <cstdio>
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
LockstepClock g_clock;
std::int64_t g_clockStep = 0;
std::int64_t g_clockQpf = 0;
std::uint64_t g_clockRenderSamples = 0;

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
    try {
        if (!g_readBackClaimed.load()) {
            NoteGameInitTiming(0);
        }
        NoteFixedStep();
    } catch (...) {
    }
    return core::CallGuestThiscall(Trampoline(guest::kGameFixedStepHelper), self, {});
}

// Replaces the timer value of a Game::Frame sample by the lockstep clock (see game_timing.h).
void OnGameFrameTimerSample(guest::GuestAddr returnAddress, guest::GuestAddr out) {
    if (g_clockMode.load() != FrameClockMode::Lockstep || out == 0 || !guest::IsReadable(out, 8)) {
        return;
    }
    const auto real = guest::Read<std::int64_t>(out);
    std::int64_t value = real;
    if (returnAddress == guest::kGameFrameClockSampleReturn) {
        // The guest's own conversion of dt (the timer initialized [0x2EB448] on its first call).
        const auto qpf = guest::Read<std::int64_t>(guest::kGameTimerFrequencyVar);
        const auto dt = std::bit_cast<float>(guest::Read<std::uint32_t>(guest::kGameFrameTimeVar));
        const std::int64_t step = GuestTimerTicks(qpf, dt);
        if (step <= 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_lock);
        value = g_clock.Advance(real, step);
        g_clockStep = step;
        g_clockQpf = qpf;
    } else {
        std::lock_guard<std::mutex> lock(g_lock);
        if (!g_clock.Started()) {
            return;
        }
        value = g_clock.Now();  // render time of this frame = its accumulator time (exact multiples of dt)
        ++g_clockRenderSamples;
    }
    guest::Write(out, value);
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

FrameClockStats GetFrameClockStats() noexcept {
    FrameClockStats s;
    s.mode = g_clockMode.load();
    std::lock_guard<std::mutex> lock(g_lock);
    s.step = g_clockStep;
    s.qpf = g_clockQpf;
    s.clock = g_clock.GetStats();
    s.renderSamples = g_clockRenderSamples;
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
                       "ticks(dt) per frame, whole-step corrections beyond 3/4 step; other callers keep QPC"
                     : "Game::Frame samples the original QPC timer");
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
