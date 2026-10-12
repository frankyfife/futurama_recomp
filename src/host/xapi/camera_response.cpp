// G5: [input] camera_response (see camera_response.h).
#include "xapi/camera_response.h"

#include "core/config_schema.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "input/stick_curve.h"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace xapi::camera {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.0f;

char Lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::uint32_t ReadU32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}
std::uint8_t ReadU8(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 1)) ? guest::Read<std::uint8_t>(p) : 0;
}
float ReadF32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<float>(p) : 0.0f;
}

std::uint32_t FloatBits(float f) noexcept {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

// ---- precise mode state (guest main thread only: the camera update and the rotation run in the logic tick) ------
PreciseTurn g_turn;
std::atomic<bool> g_pending{false};       // FollowUpdate of this tick replaces the turn angle
std::atomic<float> g_pendingAngle{0.0f};  // rad, positive = game direction for a positive stick value
std::atomic<std::uint64_t> g_updates{0};
std::atomic<std::uint64_t> g_activeTicks{0};
std::atomic<std::uint64_t> g_replaced{0};
std::atomic<bool> g_loggedFirst{false};
std::atomic<bool> g_trace{false};
std::atomic<std::uint64_t> g_traceTick{0};

bool PreciseActive(const core::Config& config) {
    return ResponseFromConfig(config) == Response::Precise;
}

// FUN_00015990 follow camera update (thiscall ecx = camera controller, [esp+4] = dt, ret 4).
std::uint32_t __fastcall FollowUpdateProbe(std::uint32_t camera, std::uint32_t /*edx*/, float dt) noexcept {
    g_updates.fetch_add(1, std::memory_order_relaxed);
    float deflection = 0.0f;
    const guest::GuestAddr session = ReadU32(kSessionPtr);
    const guest::GuestAddr player = session != 0 ? ReadU32(session + kSessionPlayerOffset) : 0;
    if (player != 0 && ReadU8(player + kPlayerInputEnabledOffset) != 0) {
        deflection = ReadF32(player + kPlayerCameraStickXOffset);
        if (!std::isfinite(deflection)) {
            deflection = 0.0f;
        }
        if (ReadU8(kInvertTurnVar) != 0) {
            deflection = -deflection;  // as the game (0x17629): the option "invert" flips the turn
        }
    }
    const float step = std::isfinite(dt) && dt > 0.0f && dt < 0.25f ? dt : 0.0f;
    const float angle = g_turn.Step(deflection, step);
    const bool replace = g_turn.Active();
    g_pendingAngle.store(angle, std::memory_order_relaxed);
    g_pending.store(replace, std::memory_order_relaxed);
    if (replace && !g_loggedFirst.exchange(true)) {
        FUT_LOGI(Game, "camera: precise turn active (camera 0x%08X, rate parameter %.3f, stick %.3f)", camera,
                 static_cast<double>(ReadF32(camera + kCameraTurnRateOffset)), static_cast<double>(deflection));
    }
    const std::uint32_t result =
        core::CallGuestThiscall(core::HookTrampoline(kFollowUpdate), camera, {FloatBits(dt)});
    g_pending.store(false, std::memory_order_relaxed);
    if (replace) {
        g_activeTicks.fetch_add(1, std::memory_order_relaxed);
        // The game's own turn filter must not glide on after the precise turn: clear both stages.
        guest::Write<float>(camera + kCameraTurnVelocityOffset, 0.0f);
        const guest::GuestAddr state = ReadU32(camera + kCameraStateOffset);
        if (state != 0 && guest::IsReadable(state + kCameraStateTurnSpeedOffset, 4)) {
            guest::Write<float>(state + kCameraStateTurnSpeedOffset, 0.0f);
        }
    }
    return result;
}

// FUN_00103020 NiMatrix3::MakeYRotation (thiscall ecx = matrix, [esp+4] = angle, ret 4). 46 call sites; only the
// follow camera's (0x17B2B) is changed, and only while a precise turn is active.
std::uint32_t __fastcall MakeYRotationProbe(std::uint32_t matrix, std::uint32_t /*edx*/, float angle) noexcept {
    if (g_pending.load(std::memory_order_relaxed) && guest::Addr(_ReturnAddress()) == kFollowRotationReturn) {
        angle = -g_pendingAngle.load(std::memory_order_relaxed);  // the game passes -angle (0x17AFE fchs)
        g_replaced.fetch_add(1, std::memory_order_relaxed);
    }
    return core::CallGuestThiscall(core::HookTrampoline(kMakeYRotation), matrix, {FloatBits(angle)});
}

void LogSummary() noexcept {
    FUT_LOGI(Game, "camera summary: precise response, %llu follow-camera updates, %llu ticks with a precise turn, "
                   "%llu rotations replaced",
             static_cast<unsigned long long>(g_updates.load()), static_cast<unsigned long long>(g_activeTicks.load()),
             static_cast<unsigned long long>(g_replaced.load()));
}

}  // namespace

bool ParseResponse(std::string_view text, Response& out) noexcept {
    if (IEquals(text, "original")) {
        out = Response::Original;
        return true;
    }
    if (IEquals(text, "precise")) {
        out = Response::Precise;
        return true;
    }
    return false;
}

const char* ResponseName(Response value) noexcept {
    return value == Response::Precise ? "precise" : "original";
}

Response ResponseFromConfig(const core::Config& config) noexcept {
    try {
        const std::string text =
            config.GetString("input", "camera_response", core::ConfigKeyDefault("input", "camera_response"));
        Response value = Response::Original;
        if (!text.empty() && !ParseResponse(text, value)) {
            FUT_LOGW(Game, "camera: invalid [input] camera_response '%s' (expected original or precise) - using "
                           "original",
                     text.c_str());
            return Response::Original;
        }
        return value;
    } catch (...) {
        return Response::Original;
    }
}

float PreciseTurn::Step(float deflection, float dt) noexcept {
    const float d = std::isfinite(deflection) ? std::clamp(deflection, -1.0f, 1.0f) : 0.0f;
    const float target = d * kPreciseMaxYawDegPerSecond * kDegToRad;
    if (!(dt > 0.0f)) {
        return 0.0f;
    }
    const float alpha = 1.0f - std::exp(-dt / kPreciseSmoothingSeconds);
    rate += (target - rate) * alpha;
    if (target == 0.0f && std::fabs(rate) < kPreciseStopRadPerSecond) {
        rate = 0.0f;
    }
    return rate * dt;
}

float OriginalSteadyTurnRate(float deflection, float rateParam, float subStepsPerSecond) noexcept {
    // FUN_00015990 0x17608..0x179C4 (g5_camera.md 2.2): u, then the fixed points of both sub-step filters.
    const float d = std::fabs(deflection);
    float u = d * (d < 0.9f ? 0.25f : 1.0f) * std::fabs(rateParam);
    if (u < 0.01f) {
        u = 0.0f;
    }
    const float v = std::min(u * 0.91f / (1.0f - 0.91f), 2.0f * std::fabs(rateParam));  // v = (u + v) * 0.91
    const float w = v * 0.001f * 0.97f / (1.0f - 0.97f);                                // w = (v*0.001 + w)*0.97
    const float perTickLimit = 0.1f * subStepsPerSecond / 2.0f;  // +-0.1 rad per tick, 2 sub-steps per tick
    return std::min(w * 0.01f * subStepsPerSecond, perTickLimit);
}

bool ReadCameraSample(CameraSample& out) noexcept {
    out = {};
    const guest::GuestAddr session = ReadU32(kSessionPtr);
    const guest::GuestAddr camera = session != 0 ? ReadU32(session + kSessionCameraOffset) : 0;
    if (camera == 0 || !guest::IsReadable(camera, kCameraHeightRateOffset + 4)) {
        return false;
    }
    out.mode = ReadU32(camera + kCameraModeOffset);
    float look[3];
    for (int i = 0; i < 3; ++i) {
        out.position[i] = ReadF32(camera + kCameraPositionOffset + 4 * i);
        look[i] = ReadF32(camera + kCameraLookAtOffset + 4 * i);
    }
    out.yawDeg = std::atan2(out.position[0] - look[0], out.position[2] - look[2]) / kDegToRad;
    out.turnVelocity = ReadF32(camera + kCameraTurnVelocityOffset);
    const guest::GuestAddr state = ReadU32(camera + kCameraStateOffset);
    out.turnSpeed = state != 0 ? ReadF32(state + kCameraStateTurnSpeedOffset) : 0.0f;
    out.rateParam = ReadF32(camera + kCameraTurnRateOffset);
    out.heightRateParam = ReadF32(camera + kCameraHeightRateOffset);
    const guest::GuestAddr player = ReadU32(session + kSessionPlayerOffset);
    if (player != 0) {
        out.stickX = ReadF32(player + kPlayerCameraStickXOffset);
        out.controllable = ReadU8(player + kPlayerInputEnabledOffset) != 0;
    }
    out.valid = std::isfinite(out.yawDeg);
    return out.valid;
}

void SetTrace(bool on) noexcept {
    g_trace.store(on, std::memory_order_relaxed);
    FUT_LOGI(Game, "camera trace %s", on ? "on" : "off");
}

void TraceTick() noexcept {
    if (!g_trace.load(std::memory_order_relaxed)) {
        return;
    }
    const std::uint64_t tick = g_traceTick.fetch_add(1, std::memory_order_relaxed);
    CameraSample s;
    if (!ReadCameraSample(s)) {
        FUT_LOGI(Game, "camera trace: %llu no camera", static_cast<unsigned long long>(tick));
        return;
    }
    FUT_LOGI(Game,
             "camera trace: %llu mode %u yaw %.3f pos %.3f %.3f %.3f v %.5f w %.6f rate %.4f hrate %.4f stick %.4f "
             "ctrl %d",
             static_cast<unsigned long long>(tick), s.mode, static_cast<double>(s.yawDeg),
             static_cast<double>(s.position[0]), static_cast<double>(s.position[1]),
             static_cast<double>(s.position[2]), static_cast<double>(s.turnVelocity),
             static_cast<double>(s.turnSpeed), static_cast<double>(s.rateParam),
             static_cast<double>(s.heightRateParam), static_cast<double>(s.stickX), s.controllable ? 1 : 0);
}

void LogConfiguration() noexcept {
    const Response r = ResponseFromConfig(core::GetConfig());
    if (r == Response::Precise) {
        FUT_LOGI(Game, "camera: response precise - right stick turns the follow camera directly (%.0f deg/s at full "
                       "deflection, smoothing %.0f ms), pad right stick dead zone %.0f %%",
                 static_cast<double>(kPreciseMaxYawDegPerSecond),
                 static_cast<double>(kPreciseSmoothingSeconds * 1000.0f),
                 static_cast<double>(inp::kPreciseStickDeadzone * 100.0f));
        core::AddExitReporter("camera", &LogSummary);
    } else {
        FUT_LOGI(Game, "camera: response original (the game's camera filter, no hooks)");
    }
}

}  // namespace xapi::camera

// Installed only with [input] camera_response = precise (core::HookDesc::condition). Prologues checked with capstone
// (g5_camera.md 3): whole, position-independent instructions, no branch into them (the only references are calls
// to the function start: 0x15990 from 0x19CCB, 0x103020 from 46 call sites).
// FUN_00015990: push ebp; mov ebp, esp; and esp, 0xFFFFFFF0.
FUT_PROBE_IF(xapi::camera::kFollowUpdate, &xapi::camera::FollowUpdateProbe, Thiscall, "probe:Camera::FollowUpdate",
             "55 8B EC 83 E4 F0", 6, &xapi::camera::PreciseActive);
// FUN_00103020: fld dword ptr [esp+4]; xor eax, eax.
FUT_PROBE_IF(xapi::camera::kMakeYRotation, &xapi::camera::MakeYRotationProbe, Thiscall,
             "probe:NiMatrix3::MakeYRotation", "D9 44 24 04 33 C0", 6, &xapi::camera::PreciseActive);
