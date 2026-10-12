// G6 first-person control (see first_person.h; analysis/loader/g6_firstperson.md).
#include "xapi/first_person.h"

#include "core/config_schema.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "gfx/freecam.h"
#include "input/input_system.h"
#include "input/kbm_source.h"
#include "input/stick_curve.h"
#include "xapi/game_select.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <string>

namespace xapi::fp {

// ---- option ---------------------------------------------------------------------------------------------------

namespace {

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view Trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

constexpr float kPi = 3.14159265358979f;

}  // namespace

bool ParseMode(std::string_view text, Mode& out) noexcept {
    const std::string_view t = Trim(text);
    if (IEquals(t, "off") || IEquals(t, "0") || IEquals(t, "false") || t.empty()) {
        out = Mode::Off;
        return true;
    }
    if (IEquals(t, "on") || IEquals(t, "1") || IEquals(t, "true")) {
        out = Mode::On;
        return true;
    }
    return false;
}

const char* ModeName(Mode value) noexcept {
    return value == Mode::On ? "on" : "off";
}

Mode ModeFromConfig(const core::Config& config) noexcept {
    try {
        const std::string text =
            config.GetString("input", "first_person", core::ConfigKeyDefault("input", "first_person"));
        Mode mode = Mode::Off;
        if (!ParseMode(text, mode)) {
            FUT_LOGW(Game, "first person: invalid [input] first_person '%s' (expected off or on) - using off",
                     text.c_str());
            return Mode::Off;
        }
        return mode;
    } catch (...) {
        return Mode::Off;
    }
}

bool HooksWanted(const core::Config& config) noexcept {
    return ModeFromConfig(config) == Mode::On;
}

// ---- pure math -------------------------------------------------------------------------------------------------

View ViewFromForward(float fx, float fy, float fz) noexcept {
    View v;
    v.yaw = std::atan2(fz, fx);
    v.pitch = -std::asin(std::clamp(fy, -1.0f, 1.0f));
    return v;
}

void ViewRotation(const View& view, float out[12]) noexcept {
    const float cy = std::cos(view.yaw), sy = std::sin(view.yaw);
    const float cp = std::cos(view.pitch), sp = std::sin(view.pitch);
    const float m[12] = {cp * cy, -sp, cp * sy, 0.0f,  // forward
                         sp * cy, cp,  sp * sy, 0.0f,  // up
                         -sy,     0.0f, cy,     0.0f};  // right
    std::memcpy(out, m, sizeof(m));
}

float ClampPitch(float pitch) noexcept {
    return std::clamp(pitch, -kPitchLimit, kPitchLimit);
}

float WrapYaw(float yaw) noexcept {
    if (!std::isfinite(yaw)) {
        return 0.0f;
    }
    yaw = std::fmod(yaw + kPi, 2.0f * kPi);
    if (yaw < 0.0f) {
        yaw += 2.0f * kPi;
    }
    return yaw - kPi;
}

void LookDeflection(float x, float y, bool gameEncoded, float& ox, float& oy) noexcept {
    ox = oy = 0.0f;
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    float dx = 0.0f;
    float dy = 0.0f;
    if (gameEncoded) {
        dx = inp::GameStickOutput(x, y);
        dy = inp::GameStickOutput(y, x);
    } else {
        const float m = std::sqrt(x * x + y * y);
        if (!(m > kStickDeadzone)) {
            return;
        }
        const float s = std::min(1.0f, (m - kStickDeadzone) / (kStickFull - kStickDeadzone));
        dx = x / m * s;
        dy = y / m * s;
    }
    const float m = std::sqrt(dx * dx + dy * dy);
    if (!(m > 0.0f)) {
        return;
    }
    const float r = std::pow(std::min(1.0f, m), kStickExponent);
    ox = dx / m * r;
    oy = dy / m * r;
}

bool MoveIntent(float x, float y, const float forward[3], const float right[3], float out[3]) noexcept {
    out[0] = out[1] = out[2] = 0.0f;
    if (!std::isfinite(x) || !std::isfinite(y) || (x == 0.0f && y == 0.0f)) {
        return false;
    }
    float f[2] = {forward[0], forward[2]};
    float r[2] = {right[0], right[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1]);
    const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    if (!(fl > 1e-4f) || !(rl > 1e-4f)) {
        return false;
    }
    f[0] /= fl;
    f[1] /= fl;
    r[0] /= rl;
    r[1] /= rl;
    const float dx = x * r[0] + y * f[0];
    const float dz = x * r[1] + y * f[1];
    const float m = std::sqrt(dx * dx + dz * dz);
    if (!(m > 1e-6f)) {
        return false;
    }
    // Unit direction times the magnitude of the intent (the original FUN_000536D0 scales its move by |intent|).
    out[0] = dx;
    out[2] = dz;
    return true;
}

// ---- runtime state ---------------------------------------------------------------------------------------------

namespace {

struct Settings {
    float lookSpeedRad = kDefaultLookSpeedDeg * kPi / 180.0f;  // full deflection, rad/s
    float mouseRadPerCount = kMouseDegPerCount * kPi / 180.0f;
    bool gameEncodedStick = false;  // [input] camera_response = precise (G5) shapes the pad's right stick
};

struct Stats {
    std::uint64_t entersUser = 0;
    std::uint64_t entersAuto = 0;
    std::uint64_t exitsUser = 0;
    std::uint64_t exitsGame = 0;
    std::uint64_t exitsSkipped = 0;
    std::uint64_t viewTicks = 0;
    std::uint64_t moveTicks = 0;
    std::uint64_t jumps = 0;
    std::uint64_t frustumSets = 0;
};

// All hooks run on the guest main thread; the atomics are read by the keyboard/mouse source and the exit reporter.
Settings g_settings;
Stats g_stats;
bool g_configured = false;
bool g_wanted = true;               // the player wants the first-person view (R3 toggles)
bool g_autoEntering = false;        // our own call of FUN_00054DA0(1) is running
guest::GuestAddr g_movePlayer = 0;  // player whose FUN_00054AE0 runs with +0xB88 cleared
std::uint32_t g_session = 0;        // counts entries; the camera hook restarts its view on a change
std::uint32_t g_viewSession = 0xFFFFFFFFu;
View g_view;
std::uint32_t g_ticksSinceGameExit = kReenterTicks;
bool g_blockedLogged = false;
std::string g_lastLevel;
std::atomic<std::uint64_t> g_lastViewMs{0};
std::atomic<std::uint64_t> g_skipStreak{0};

std::uint64_t NowMs() noexcept {
    return GetTickCount64();
}

std::uint32_t Bits(float f) noexcept {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

void LogSummary() noexcept {
    FUT_LOGI(Game,
             "first person summary: %llu entries by R3, %llu automatic, %llu exits by R3, %llu by the game, %llu "
             "automatic exits skipped, %llu view ticks, %llu move ticks, %llu jumps, %llu frustum sets",
             static_cast<unsigned long long>(g_stats.entersUser), static_cast<unsigned long long>(g_stats.entersAuto),
             static_cast<unsigned long long>(g_stats.exitsUser), static_cast<unsigned long long>(g_stats.exitsGame),
             static_cast<unsigned long long>(g_stats.exitsSkipped), static_cast<unsigned long long>(g_stats.viewTicks),
             static_cast<unsigned long long>(g_stats.moveTicks), static_cast<unsigned long long>(g_stats.jumps),
             static_cast<unsigned long long>(g_stats.frustumSets));
}

bool MouseTaken() noexcept {
    return MouseLookActive();
}

void ConfigureOnce() noexcept {
    if (g_configured) {
        return;
    }
    g_configured = true;
    const core::Config& cfg = core::GetConfig();
    auto num = [&](const char* section, const char* key, float def) {
        try {
            const std::string v = cfg.GetString(section, key, core::ConfigKeyDefault(section, key));
            char* end = nullptr;
            const float f = std::strtof(v.c_str(), &end);
            return (end != v.c_str() && std::isfinite(f) && f > 0.0f) ? f : def;
        } catch (...) {
            return def;
        }
    };
    const float lookDeg = std::clamp(num("input", "first_person_look_speed", kDefaultLookSpeedDeg), 10.0f, 1000.0f);
    const float mouse = std::clamp(num("input", "mouse_sensitivity", 1.0f), 0.05f, 20.0f);
    g_settings.lookSpeedRad = lookDeg * kPi / 180.0f;
    g_settings.mouseRadPerCount = kMouseDegPerCount * mouse * kPi / 180.0f;
    try {
        g_settings.gameEncodedStick = IEquals(Trim(cfg.GetString("input", "camera_response", "original")), "precise");
    } catch (...) {
        g_settings.gameEncodedStick = false;
    }
    inp::GlobalKeyboardMouse().SetMouseMotionQuery(&MouseTaken);
    core::AddExitReporter("first person", &LogSummary);
    FUT_LOGI(Game,
             "first person: on (experimental) - R3 switches first/third person, look speed %.0f deg/s, mouse %.3f "
             "deg/count, right stick %s",
             static_cast<double>(lookDeg), static_cast<double>(kMouseDegPerCount * mouse),
             g_settings.gameEncodedStick ? "decoded from camera_response = precise" : "own dead zone");
}

guest::GuestAddr Session() noexcept {
    return guest::Read<std::uint32_t>(kSessionPtr);
}
guest::GuestAddr SessionCamera() noexcept {
    const guest::GuestAddr s = Session();
    return s != 0 ? guest::Read<std::uint32_t>(s + kSessionCamera) : 0;
}
guest::GuestAddr SessionPlayer() noexcept {
    const guest::GuestAddr s = Session();
    return s != 0 ? guest::Read<std::uint32_t>(s + kSessionPlayer) : 0;
}

std::string LevelName() {
    const guest::GuestAddr s = Session();
    if (s == 0) {
        return {};
    }
    const guest::GuestAddr p = guest::Read<std::uint32_t>(s + 0xC);
    if (p == 0) {
        return {};
    }
    std::string name;
    for (std::uint32_t i = 0; i < 16; ++i) {
        const char c = guest::Read<char>(p + i);
        if (c == 0) {
            break;
        }
        name.push_back(c);
    }
    return name;
}

// The levels with a vehicle or a bound object keep the game's camera (same list as play_as, g4_select.md 4.3).
bool BlockedLevel() {
    const std::string level = LevelName();
    if (level != g_lastLevel) {
        g_lastLevel = level;
        g_blockedLogged = false;
    }
    const char* reason = xapi::select::PlayAsBlockedReason(level);
    if (reason != nullptr && !g_blockedLogged) {
        g_blockedLogged = true;
        FUT_LOGI(Game, "first person: %s keeps the game's camera (%s)", level.c_str(), reason);
    }
    return reason != nullptr;
}

bool CallBool(guest::GuestAddr object, std::uint32_t slot) noexcept {
    const guest::GuestAddr vt = guest::Read<std::uint32_t>(object);
    const guest::GuestAddr fn = guest::Read<std::uint32_t>(vt + slot);
    return (core::CallGuestThiscall(fn, object, {}) & 0xFFu) != 0;
}

bool Dead(guest::GuestAddr player) noexcept {
    return CallBool(player, kVfIsDead);
}

// The checks of the game's own R3 handler 0x588B0 before it enters the view.
bool GameAllowsView(guest::GuestAddr player) noexcept {
    if (CallBool(player, kVfBusyA) || CallBool(player, kVfBusyB) || CallBool(player, kVfIsDead) ||
        CallBool(player, kVfStateA) || CallBool(player, kVfStateB) || !CallBool(player, kVfOnGround)) {
        return false;
    }
    return guest::Read<std::uint8_t>(player + kPlayerStateB8E) == 0 &&
           guest::Read<std::uint32_t>(player + kPlayerRiding) == 0;
}

// Cut scene running (FUN_000D3110: byte +0x34 of the manager [0x2EB7C8]).
bool CutsceneActive() noexcept {
    const guest::GuestAddr m = guest::Read<std::uint32_t>(guest::kGameCutscenePtr);
    return m != 0 && guest::Read<std::uint8_t>(m + guest::kCutsceneActiveOffset) != 0;
}

bool InView(guest::GuestAddr player) noexcept {
    return player != 0 && guest::Read<std::uint8_t>(player + kPlayerFirstPerson) != 0;
}

// The view is the extension's (wanted, not a vehicle level) and active for `player`.
bool Extended(guest::GuestAddr player) {
    return g_wanted && InView(player) && !BlockedLevel();
}

// ---- probes ----------------------------------------------------------------------------------------------------

// FUN_00054DA0: thiscall(player, char on) ret 4.
std::uint32_t __fastcall SetFirstPersonProbe(std::uint32_t player, std::uint32_t /*edx*/, std::uint32_t on) noexcept {
    ConfigureOnce();
    const guest::GuestAddr from = guest::Addr(_ReturnAddress());
    const bool enter = (on & 0xFFu) != 0;
    if (enter) {
        ++g_session;
        if (g_autoEntering) {
            ++g_stats.entersAuto;
            FUT_LOGI(Game, "first person: on (automatic)");
        } else if (from == kToggleOnReturn) {
            g_wanted = true;
            ++g_stats.entersUser;
            FUT_LOGI(Game, "first person: on (R3)");
        } else {
            FUT_LOGI(Game, "first person: on (game, return 0x%08X)", from);
        }
    } else {
        if (from == kAutoExitReturn && Extended(player) && !Dead(player) &&
            guest::Read<std::uint32_t>(player + kPlayerRiding) == 0) {
            // In the air, attacking, hit, targeting: the extension keeps the view (the body keeps turning with it).
            ++g_stats.exitsSkipped;
            if (g_skipStreak.fetch_add(1, std::memory_order_relaxed) == 0) {
                FUT_LOGD(Game, "first person: automatic exit of FUN_00057940 skipped");
            }
            return 0;
        }
        g_skipStreak.store(0, std::memory_order_relaxed);
        if (from == kToggleOffReturn) {
            g_wanted = false;
            ++g_stats.exitsUser;
            FUT_LOGI(Game, "first person: off (R3)");
        } else if (InView(player)) {
            ++g_stats.exitsGame;
            g_ticksSinceGameExit = 0;
            FUT_LOGI(Game, "first person: off (game, return 0x%08X)", from);
        }
    }
    const std::uint32_t r = core::CallGuestThiscall(core::HookTrampoline(kSetFirstPerson), player, {on});
    if (enter && g_wanted && InView(player) && !BlockedLevel()) {
        // FUN_00054DA0 hides the HUD for every character but Fry (+0x774 != 1, FUN_000E59D0(0) on [session+0x118])
        // and switches on Bender's visor overlay (+0x7B4, FUN_00045ED0(1): a narrow green slit). The playable view
        // keeps the HUD and the full picture; leaving the view switches both back as before (FUN_00054DA0(0)).
        const guest::GuestAddr visor = guest::Read<std::uint32_t>(player + kPlayerVisor);
        if (visor != 0) {
            core::CallGuestThiscall(kVisorSet, visor, {0});
        }
        const guest::GuestAddr session = Session();
        const guest::GuestAddr hud = session != 0 ? guest::Read<std::uint32_t>(session + kSessionHud) : 0;
        if (hud != 0 && guest::Read<std::uint32_t>(player + kPlayerCharacterType) != kCharacterTypeFry) {
            core::CallGuestThiscall(kHudShow, hud, {1});
        }
    }
    return r;
}

// 0x588B0 (player vtable +0x11C): thiscall(player, float, float) ret 8 - the R3 handler, once per tick while the
// player takes input.
std::uint32_t __fastcall ViewToggleProbe(std::uint32_t player, std::uint32_t /*edx*/, std::uint32_t a,
                                         std::uint32_t b) noexcept {
    ConfigureOnce();
    if (g_ticksSinceGameExit < kReenterTicks) {
        ++g_ticksSinceGameExit;
    }
    const guest::GuestAddr cam = SessionCamera();
    if (g_wanted && player != 0 && player == SessionPlayer() && cam != 0 && !InView(player) &&
        guest::Read<std::uint32_t>(cam + kCamMode) == kCamModeFollow && g_ticksSinceGameExit >= kReenterTicks &&
        (guest::Read<std::uint16_t>(player + kPlayerPadBits) & kPadBitViewToggle) == 0 &&
        guest::Read<std::uint8_t>(player + kPlayerInputEnabled) != 0 && !CutsceneActive() && !BlockedLevel() &&
        GameAllowsView(player)) {
        g_autoEntering = true;
        core::CallGuestThiscall(kSetFirstPerson, player, {1});
        g_autoEntering = false;
    }
    return core::CallGuestThiscall(core::HookTrampoline(kPlayerViewToggle), player, {a, b});
}

// FUN_00054AE0 (player vtable +0x100): thiscall(player, float* velocity, float* rotation) ret 8.
std::uint32_t __fastcall MoveProbe(std::uint32_t player, std::uint32_t /*edx*/, std::uint32_t velocity,
                                   std::uint32_t rotation) noexcept {
    const guest::GuestAddr trampoline = core::HookTrampoline(kPlayerMove);
    if (!Extended(player)) {
        return core::CallGuestThiscall(trampoline, player, {velocity, rotation});
    }
    guest::Write<std::uint8_t>(player + kPlayerFirstPerson, 0);
    g_movePlayer = player;
    const std::uint32_t r = core::CallGuestThiscall(trampoline, player, {velocity, rotation});
    g_movePlayer = 0;
    guest::Write<std::uint8_t>(player + kPlayerFirstPerson, 1);
    ++g_stats.moveTicks;
    return r;
}

// FUN_000536D0: thiscall(player, float* out, float turn, float* rotation) ret 0xC, returns out.
std::uint32_t __fastcall MoveDirectionProbe(std::uint32_t player, std::uint32_t /*edx*/, std::uint32_t out,
                                            std::uint32_t turn, std::uint32_t rotation) noexcept {
    const guest::GuestAddr cam = SessionCamera();
    if (g_movePlayer == 0 || player != g_movePlayer || cam == 0 ||
        guest::Read<std::uint32_t>(cam + kCamMode) != kCamModeFirstPerson) {
        return core::CallGuestThiscall(core::HookTrampoline(kMoveDirection), player, {out, turn, rotation});
    }
    const float x = guest::Read<float>(player + kPlayerMoveX);
    const float y = guest::Read<float>(player + kPlayerMoveY);
    const float forward[3] = {guest::Read<float>(cam + kCamRotation), guest::Read<float>(cam + kCamRotation + 4),
                              guest::Read<float>(cam + kCamRotation + 8)};
    const float right[3] = {guest::Read<float>(cam + kCamRotation + 0x20),
                            guest::Read<float>(cam + kCamRotation + 0x24),
                            guest::Read<float>(cam + kCamRotation + 0x28)};
    float move[3] = {};
    MoveIntent(x, y, forward, right, move);
    for (int i = 0; i < 3; ++i) {
        guest::Write<float>(out + 4u * i, move[i]);
        guest::Write<float>(player + kPlayerMoveIntent + 4u * i, move[i]);
    }
    // The body keeps its rotation (FUN_00057940 turns it to the view yaw afterwards).
    std::memcpy(guest::Ptr<void>(rotation), guest::Ptr<const void>(player + kPlayerRotation), 12 * sizeof(float));
    return out;
}

// 0x50D10 (player vtable +0x104): thiscall(player) ret 0 - jump.
std::uint32_t __fastcall JumpProbe(std::uint32_t player, std::uint32_t /*edx*/) noexcept {
    const guest::GuestAddr trampoline = core::HookTrampoline(kPlayerJump);
    if (!Extended(player)) {
        return core::CallGuestThiscall(trampoline, player, {});
    }
    const bool before = guest::Read<std::uint8_t>(player + 0xB89) != 0;
    guest::Write<std::uint8_t>(player + kPlayerFirstPerson, 0);
    const std::uint32_t r = core::CallGuestThiscall(trampoline, player, {});
    guest::Write<std::uint8_t>(player + kPlayerFirstPerson, 1);
    if (!before && guest::Read<std::uint8_t>(player + 0xB89) != 0) {
        ++g_stats.jumps;  // +0xB89 = jump started (0x50E2D)
    }
    return r;
}

// FUN_00018250: thiscall(camera, float dt) ret 4 - camera mode 3.
std::uint32_t __fastcall CameraUpdateProbe(std::uint32_t cam, std::uint32_t /*edx*/, std::uint32_t dtBits) noexcept {
    ConfigureOnce();
    const guest::GuestAddr player = SessionPlayer();
    const guest::GuestAddr actor = guest::Read<std::uint32_t>(cam + kCamTarget);
    if (player == 0 || actor == 0 || !g_wanted || BlockedLevel()) {
        return core::CallGuestThiscall(core::HookTrampoline(kCameraFirstPersonUpdate), cam, {dtBits});
    }
    float dt = 0.0f;
    std::memcpy(&dt, &dtBits, sizeof(dt));
    dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;
    ++g_stats.viewTicks;

    const bool frustumDirty = guest::Read<std::uint8_t>(cam + kCamFrustumDirty) != 0;

    // View: restart from the camera's rotation at every new entry (FUN_00018050 set it from the body yaw).
    const std::uint64_t now = NowMs();
    const std::uint64_t last = g_lastViewMs.exchange(now, std::memory_order_relaxed);
    auto& kbm = inp::GlobalKeyboardMouse();
    const bool freecam = gfx::freecam::Active();
    if (g_viewSession != g_session) {
        g_viewSession = g_session;
        g_view = ViewFromForward(guest::Read<float>(cam + kCamRotation), guest::Read<float>(cam + kCamRotation + 4),
                                 guest::Read<float>(cam + kCamRotation + 8));
        g_view.pitch = ClampPitch(g_view.pitch);
        if (!freecam) {
            (void)kbm.ReadRaw(true);  // motion from before the view
        }
    } else if (now - last > 150 && !freecam) {
        (void)kbm.ReadRaw(true);  // pause, loading: drop the motion collected meanwhile
    }
    const bool invert = guest::Read<std::uint8_t>(kInvertAimVar) != 0;  // FUN_0005E020: -1 when set
    const float invertPad = invert ? -1.0f : 1.0f;
    const guest::GuestAddr pad = guest::Read<std::uint32_t>(player + kPlayerController);
    float sx = 0.0f;
    float sy = 0.0f;
    if (pad != 0) {
        LookDeflection(guest::Read<float>(pad + kPadRightStickX), guest::Read<float>(pad + kPadRightStickY),
                       g_settings.gameEncodedStick, sx, sy);
    }
    // The game: yaw += RS X, pitch += RS Y * invert (positive pitch = down; the game's default is "stick up looks
    // down"). Mouse: forward = look up, the game's option inverts it as well.
    // Zoomed weapon view (FUN_00057940 sets 45 degrees for Fry's weapon type 3 and halves the game's look input):
    // half the turn rate as well.
    const float fovDeg = guest::Read<float>(cam + kCamFovDeg);
    const float zoom = (fovDeg > 1.0f && fovDeg < 60.0f) ? 0.5f : 1.0f;
    g_view.yaw += sx * g_settings.lookSpeedRad * zoom * dt;
    g_view.pitch += sy * invertPad * g_settings.lookSpeedRad * zoom * dt;
    if (!freecam) {
        const auto raw = kbm.ReadRaw(true);
        g_view.yaw += static_cast<float>(raw.dx) * g_settings.mouseRadPerCount * zoom;
        g_view.pitch += static_cast<float>(raw.dy) * g_settings.mouseRadPerCount * zoom * invertPad;
    }
    g_view.yaw = WrapYaw(g_view.yaw);
    g_view.pitch = ClampPitch(g_view.pitch);
    float rot[12] = {};
    ViewRotation(g_view, rot);
    std::memcpy(guest::Ptr<void>(cam + kCamRotation), rot, sizeof(rot));
    guest::Write<std::uint8_t>(cam + kCamRotationDirty, 1);

    // Position: as the original (actor position + eye height of its def), then kept kWallClearance away from walls
    // in the ground plane (camera collision rays FUN_00011000), so the near plane does not cut into them.
    alignas(16) float pos[4] = {};
    const guest::GuestAddr vt = guest::Read<std::uint32_t>(actor);
    core::CallGuestThiscall(guest::Read<std::uint32_t>(vt + kVfGetPosition), actor, {guest::Addr(pos)});
    const guest::GuestAddr def = guest::Read<std::uint32_t>(actor + kPlayerDef);
    const float eye = def != 0 ? guest::Read<float>(def + kDefEyeHeight) : 0.0f;
    const float camPos[3] = {pos[0], pos[1] + eye, pos[2]};
    float hits[4] = {};
    float push[3] = {};
    {
        const float dirs[4][2] = {{rot[0], rot[2]}, {-rot[0], -rot[2]}, {rot[8], rot[10]}, {-rot[8], -rot[10]}};
        for (int i = 0; i < 4; ++i) {
            float dx = dirs[i][0];
            float dz = dirs[i][1];
            const float l = std::sqrt(dx * dx + dz * dz);
            hits[i] = kWallProbe;
            if (!(l > 1e-4f)) {
                continue;
            }
            dx /= l;
            dz /= l;
            alignas(4) float dist = kWallProbe;
            const std::uint32_t hit = core::CallGuest(
                kCameraRay, {Bits(camPos[0]), Bits(camPos[1]), Bits(camPos[2]), Bits(dx), Bits(0.0f), Bits(dz),
                             guest::Addr(&dist)});
            if (hit != 0 && std::isfinite(dist) && dist >= 0.0f && dist < kWallProbe) {
                hits[i] = dist;
                const float d = WallPush(dist);
                push[0] -= dx * d;
                push[2] -= dz * d;
            }
        }
    }
    guest::Write<float>(cam + kCamPosition, camPos[0] + push[0]);
    guest::Write<float>(cam + kCamPosition + 4, camPos[1]);
    guest::Write<float>(cam + kCamPosition + 8, camPos[2] + push[2]);
    guest::Write<std::uint8_t>(cam + kCamPositionDirty, 1);

    // Near plane: the camera apply FUN_00012210 (Session::Render) sets the frustum with near 0.25 whenever +0xA0 is
    // dirty; in the view the frustum is set here with the engine's smallest near plane (FUN_00011960 keeps
    // near >= max(0.1, far / 10000)).
    const bool frustumDirtyNow = guest::Read<std::uint8_t>(cam + kCamFrustumDirty) != 0;
    const guest::GuestAddr niCamera = guest::Read<std::uint32_t>(cam + kCamNiCamera);
    if (frustumDirtyNow && niCamera != 0) {
        const float tanH = guest::Read<float>(cam + kCamTanHalfH);
        const float tanV = guest::Read<float>(cam + kCamTanHalfV);
        alignas(4) float frustum[6] = {-tanH, tanH, tanV, -tanV, kNearPlane, guest::Read<float>(cam + kCamFar)};
        core::CallGuestThiscall(kNiCameraSetFrustum, niCamera, {guest::Addr(frustum)});
        guest::Write<std::uint8_t>(cam + kCamFrustumDirty, 0);
        ++g_stats.frustumSets;
    }
    FUT_LOGT(Game,
             "first person trace: pos %.3f %.3f %.3f eye %.3f yaw %.2f pitch %.2f fwd %.3f %.3f %.3f right %.3f %.3f "
             "move %.3f %.3f stick %.2f %.2f look %.2f %.2f walls %.2f %.2f %.2f %.2f push %.3f %.3f fov %d%d aim %.2f %.2f "
             "%.2f",
             static_cast<double>(pos[0]), static_cast<double>(pos[1]), static_cast<double>(pos[2]),
             static_cast<double>(eye), static_cast<double>(g_view.yaw * 180.0f / kPi),
             static_cast<double>(g_view.pitch * 180.0f / kPi), static_cast<double>(rot[0]),
             static_cast<double>(rot[1]), static_cast<double>(rot[2]), static_cast<double>(rot[8]),
             static_cast<double>(rot[10]), static_cast<double>(guest::Read<float>(player + kPlayerMoveIntent)),
             static_cast<double>(guest::Read<float>(player + kPlayerMoveIntent + 8)),
             static_cast<double>(guest::Read<float>(player + kPlayerMoveX)),
             static_cast<double>(guest::Read<float>(player + kPlayerMoveY)), static_cast<double>(sx),
             static_cast<double>(sy), static_cast<double>(hits[0]), static_cast<double>(hits[1]),
             static_cast<double>(hits[2]), static_cast<double>(hits[3]), static_cast<double>(push[0]),
             static_cast<double>(push[2]), frustumDirty ? 1 : 0, frustumDirtyNow ? 1 : 0,
             static_cast<double>(guest::Read<float>(player + kPlayerAimPoint)),
             static_cast<double>(guest::Read<float>(player + kPlayerAimPoint + 4)),
             static_cast<double>(guest::Read<float>(player + kPlayerAimPoint + 8)));
    return 0;
}

}  // namespace

bool MouseLookActive() noexcept {
    const std::uint64_t last = g_lastViewMs.load(std::memory_order_relaxed);
    return last != 0 && NowMs() - last <= 150;
}

}  // namespace xapi::fp

// Installed only with [input] first_person = on (core::HookDesc::condition). Prologues checked with capstone
// (g6_firstperson.md 5): whole position-independent instructions, no branch into them.
FUT_PROBE_IF(xapi::fp::kSetFirstPerson, &xapi::fp::SetFirstPersonProbe, Thiscall, "probe:Player::SetFirstPerson",
             "55 8B EC 83 E4 F0", 6, &xapi::fp::HooksWanted);
FUT_PROBE_IF(xapi::fp::kPlayerViewToggle, &xapi::fp::ViewToggleProbe, Thiscall, "probe:Player::vfViewToggle",
             "83 EC 24 56 8B F1", 6, &xapi::fp::HooksWanted);
FUT_PROBE_IF(xapi::fp::kPlayerMove, &xapi::fp::MoveProbe, Thiscall, "probe:Player::Move", "55 8B EC 83 E4 F0", 6,
             &xapi::fp::HooksWanted);
FUT_PROBE_IF(xapi::fp::kMoveDirection, &xapi::fp::MoveDirectionProbe, Thiscall, "probe:Player::MoveDirection",
             "55 8B EC 83 E4 F0", 6, &xapi::fp::HooksWanted);
FUT_PROBE_IF(xapi::fp::kPlayerJump, &xapi::fp::JumpProbe, Thiscall, "probe:Player::vfJump", "83 EC 10 56 8B F1", 6,
             &xapi::fp::HooksWanted);
FUT_PROBE_IF(xapi::fp::kCameraFirstPersonUpdate, &xapi::fp::CameraUpdateProbe, Thiscall,
             "probe:Camera::FirstPersonUpdate", "55 8B EC 83 E4 F0", 6, &xapi::fp::HooksWanted);
