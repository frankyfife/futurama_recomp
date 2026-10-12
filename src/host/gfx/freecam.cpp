// M4b freecam (see freecam.h).
#include "gfx/freecam.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "gfx/freecam_math.h"
#include "gfx/ni_guest_layouts.h"
#include "gfx/nipc_renderer.h"
#include "input/chord_filter.h"
#include "input/input_system.h"
#include "input/kbm_source.h"
#include "xapi/game_timing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace gfx::freecam {
namespace {

// NiCamera::Click (fastcall, ecx = camera; `_text_00100000.c`, analysis/loader/renderer.md): SetCameraData (slot 47)
// with +0xA0/+0x110/+0x11C/+0x128/+0x134/+0x14C, then culling and drawing of the scene.
constexpr guest::GuestAddr kNiCameraClick = 0x00100C50;
// NiCamera::vf39 0xFFDA0 after NiAVObject::vf39: the six frustum planes (thiscall, (float frustum value, plane*),
// ret 8) in the order near, far, left, right, top, bottom, then the view data 0xFF3A0 (thiscall, no arguments).
constexpr guest::GuestAddr kPlaneNear = 0x000FEED0;
constexpr guest::GuestAddr kPlaneFar = 0x000FEF50;
constexpr guest::GuestAddr kPlaneLeft = 0x000FF000;
constexpr guest::GuestAddr kPlaneRight = 0x000FF0D0;
constexpr guest::GuestAddr kPlaneTop = 0x000FF1C0;
constexpr guest::GuestAddr kPlaneBottom = 0x000FF290;
constexpr guest::GuestAddr kViewData = 0x000FF3A0;

// NiCamera layout (ni_guest_layouts.h + vf39): world rotation rows (16-byte stride), translate, derived rows.
constexpr std::uint32_t kWorldRowDir = 0x70, kWorldRowUp = 0x80, kWorldRowRight = 0x90;
constexpr std::uint32_t kWorldTranslate = 0xA0;
constexpr std::uint32_t kPlaneSet = 0x1B0;  // pointer to 6 plane pointers
constexpr std::uint32_t kSaveWorldBegin = 0x70, kSaveWorldEnd = 0xB0;
constexpr std::uint32_t kSaveDerivedBegin = 0x110, kSaveDerivedEnd = 0x14C;  // rows copy + frustum

struct Settings {
    bool enabled = true;
    bool padChord = true;
    std::uint16_t key = 0x75;  // VK_F6
    float moveSpeed = 40.0f;  // world units per second at full deflection (level scale)
    float lookSpeedDeg = 120.0f;  // degrees per second at full stick deflection
    float mouseDegPerCount = 0.08f;
    bool invertY = false;
    bool backFaces = true;  // [freecam] back_faces: initial state of the B toggle
};

struct CameraSeen {
    guest::GuestAddr camera = 0;
    std::uint32_t target = 0;
    std::uint64_t clicks = 0;
};

struct State {
    std::recursive_mutex mutex;
    Settings settings;
    bool configured = false;
    std::array<inp::ChordFilter, inp::kPortCount> chords{};
    bool keyWasDown = false;
    bool active = false;
    bool started = false;     // the rig holds a pose (taken from the first game camera after activation)
    int port = 0;
    Rig rig;
    float fovScale = 1.0f;    // frustum extents multiplier (< 1 = zoom in)
    float sceneScale = 1.0f;  // tempo factor from the scene camera's near plane (set at the start)
    float speedFactor = 1.0f; // D-pad left/right, +/- keys (kept for the session)
    bool hideHud = false;     // Y / H
    bool backFaces = true;    // B (gamepad and key)
    std::array<bool, 256> keysBefore{};
    inp::PadState pad;        // last diverted state
    std::uint64_t ticks = 0;  // freecam ticks while active
    std::uint64_t activations = 0;
    std::array<CameraSeen, 16> seen{};
    std::uint32_t seenCount = 0;
    std::uint32_t lastFrame = 0xFFFFFFFF;
    guest::GuestAddr frameCamera = 0;  // the camera the freecam replaces in the current frame
    std::uint64_t overrides = 0;
};

State& S() noexcept {
    static State s;
    return s;
}

std::atomic<guest::GuestAddr> g_trampoline{0};
std::atomic<bool> g_hudHidden{false};
std::atomic<bool> g_backFaces{false};  // active && showBackFaces
std::atomic<guest::GuestAddr> g_sceneCamera{0};    // the scene camera the freecam replaces
std::atomic<std::uint32_t> g_currentCamera{0};      // camera of the last SetCameraData (location - 0xA0)
std::atomic<std::uint64_t> g_hudSkips{0};  // screen draws + clicks skipped with the HUD hidden

Vec3 ReadVec(guest::GuestAddr a) noexcept {
    return {guest::Read<float>(a), guest::Read<float>(a + 4), guest::Read<float>(a + 8)};
}
void WriteVec(guest::GuestAddr a, Vec3 v) noexcept {
    guest::Write<float>(a, v.x);
    guest::Write<float>(a + 4, v.y);
    guest::Write<float>(a + 8, v.z);
}

Pose ReadGamePose(guest::GuestAddr cam) noexcept {
    Pose p;
    p.direction = ReadVec(cam + kWorldRowDir);
    p.up = ReadVec(cam + kWorldRowUp);
    p.right = ReadVec(cam + kWorldRowRight);
    p.position = ReadVec(cam + kWorldTranslate);
    return p;
}

// The derived part of NiCamera::vf39 (everything after NiAVObject::vf39) from the current world pose and frustum.
void RecomputeDerived(guest::GuestAddr cam) noexcept {
    for (std::uint32_t i = 0; i < 3; ++i) {
        WriteVec(cam + ni::kNiCameraWorldDirection + i * 12, ReadVec(cam + kWorldRowDir + i * 16));
    }
    const guest::GuestAddr planes = guest::Read<std::uint32_t>(cam + kPlaneSet);
    if (planes != 0) {
        const guest::GuestAddr fr = cam + ni::kNiCameraFrustum;  // left right top bottom near far
        const struct {
            guest::GuestAddr fn;
            std::uint32_t frustumOffset;
            std::uint32_t plane;
        } calls[] = {{kPlaneNear, 0x10, 0}, {kPlaneFar, 0x14, 1},  {kPlaneLeft, 0x00, 2},
                     {kPlaneRight, 0x04, 3}, {kPlaneTop, 0x08, 4}, {kPlaneBottom, 0x0C, 5}};
        for (const auto& c : calls) {
            core::CallGuestThiscall(c.fn, cam,
                                    {guest::Read<std::uint32_t>(fr + c.frustumOffset),
                                     guest::Read<std::uint32_t>(planes + c.plane * 4)});
        }
    }
    core::CallGuestThiscall(kViewData, cam, {});
}

// The scene camera: perspective, symmetric around the view axis, full-screen port, real depth range. Excluded:
// the full-screen helper cameras (n 1, f 2 at the origin), the HUD cameras (l 0 r 0.1), the avatar portrait
// (port in the top-left corner) - measured in the front end and Level 1-1 (run/freecam logs, 2026-10-12).
bool SceneCamera(guest::GuestAddr cam) noexcept {
    const auto f = guest::Read<ni::NiFrustum>(cam + ni::kNiCameraFrustum);
    const auto port = guest::Read<ni::NiRectF>(cam + ni::kNiCameraPort);
    const bool finite = std::isfinite(f.left) && std::isfinite(f.right) && std::isfinite(f.top) &&
                        std::isfinite(f.bottom) && std::isfinite(f.nearPlane) && std::isfinite(f.farPlane);
    const bool fullPort = port.left <= 0.01f && port.right >= 0.99f && port.bottom <= 0.01f && port.top >= 0.99f;
    return finite && fullPort && f.nearPlane > 0.0f && f.farPlane >= 50.0f * f.nearPlane && f.left < 0.0f &&
           f.right > 0.0f && f.bottom < 0.0f && f.top > 0.0f;
}

void NoteCamera(State& s, guest::GuestAddr cam, std::uint32_t target) noexcept {
    for (std::uint32_t i = 0; i < s.seenCount; ++i) {
        if (s.seen[i].camera == cam && s.seen[i].target == target) {
            ++s.seen[i].clicks;
            return;
        }
    }
    if (s.seenCount < s.seen.size()) {
        s.seen[s.seenCount++] = {cam, target, 1};
    }
    const auto f = guest::Read<ni::NiFrustum>(cam + ni::kNiCameraFrustum);
    const auto port = guest::Read<ni::NiRectF>(cam + ni::kNiCameraPort);
    const Vec3 p = ReadVec(cam + kWorldTranslate);
    const Vec3 d = ReadVec(cam + kWorldRowDir);
    const Vec3 u = ReadVec(cam + kWorldRowUp);
    FUT_LOGI(Gfx,
             "freecam: camera 0x%08X target 0x%08X frustum l %.3f r %.3f t %.3f b %.3f n %.3f f %.1f port %.2f %.2f "
             "%.2f %.2f pos %.1f %.1f %.1f dir %.2f %.2f %.2f up %.2f %.2f %.2f",
             cam, target, f.left, f.right, f.top, f.bottom, f.nearPlane, f.farPlane, port.left, port.right, port.top,
             port.bottom, p.x, p.y, p.z, d.x, d.y, d.z, u.x, u.y, u.z);
}

// ---- input --------------------------------------------------------------------------------------------------------

class Consumer final : public inp::InputConsumer {
public:
    const char* Name() const noexcept override { return "freecam"; }
    void Consume(int port, std::uint64_t tick, const inp::PadState& state) noexcept override;
};
Consumer g_consumer;

void Activate(State& s, int port) noexcept {
    if (s.active) {
        return;
    }
    s.active = true;
    s.started = false;
    s.port = port;
    s.fovScale = 1.0f;
    s.hideHud = false;
    g_hudHidden.store(false, std::memory_order_relaxed);
    s.backFaces = s.settings.backFaces;
    g_backFaces.store(s.backFaces, std::memory_order_relaxed);
    s.ticks = 0;
    ++s.activations;
    inp::GlobalKeyboardMouse().SetPadEmulation(false);
    (void)inp::GlobalKeyboardMouse().ReadRaw(true);  // drop motion collected before
    inp::GlobalRouter().Divert(port, &g_consumer);
    FUT_LOGI(Gfx, "freecam: on (port %d)", port + 1);
}

void Deactivate(State& s) noexcept {
    if (!s.active) {
        return;
    }
    s.active = false;
    s.started = false;
    s.hideHud = false;
    g_hudHidden.store(false, std::memory_order_relaxed);
    g_backFaces.store(false, std::memory_order_relaxed);
    inp::GlobalRouter().Divert(s.port, nullptr);
    inp::GlobalKeyboardMouse().SetPadEmulation(true);
    FUT_LOGI(Gfx, "freecam: off after %llu ticks, %llu camera overrides, %llu HUD draws skipped",
             static_cast<unsigned long long>(s.ticks), static_cast<unsigned long long>(s.overrides),
             static_cast<unsigned long long>(g_hudSkips.load(std::memory_order_relaxed)));
}

bool Key(const inp::KeyboardMouseSource::RawSnapshot& r, std::uint16_t vk) noexcept {
    return vk < r.keys.size() && r.keys[vk];
}

void Consumer::Consume(int port, std::uint64_t tick, const inp::PadState& state) noexcept {
    (void)tick;
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (!s.active || port != s.port) {
        return;
    }
    ++s.ticks;
    const inp::PadState prev = s.pad;
    s.pad = state;
    const auto raw = inp::GlobalKeyboardMouse().ReadRaw(true);
    if (!s.started) {
        return;  // no pose yet: the next Click takes the game camera
    }
    const std::uint32_t rate = std::max<std::uint32_t>(xapi::ActiveTickRate(), 1);
    const float dt = 1.0f / static_cast<float>(rate);
    const Settings& cfg = s.settings;
    constexpr float kDead = 0.15f;
    constexpr float kDegToRad = 3.14159265f / 180.0f;

    // Gamepad: LS move, RS look, RT/LT up/down, A faster, X slower, D-pad up/down zoom, BACK = back to the game camera.
    float fwd = StickAxis(state.thumb[inp::kAxisLY], kDead);
    float strafe = StickAxis(state.thumb[inp::kAxisLX], kDead);
    float rise = (static_cast<float>(state.analog[inp::kAnalogRightTrigger]) -
                  static_cast<float>(state.analog[inp::kAnalogLeftTrigger])) / 255.0f;
    float yaw = StickAxis(state.thumb[inp::kAxisRX], kDead) * cfg.lookSpeedDeg * kDegToRad * dt;
    float pitch = StickAxis(state.thumb[inp::kAxisRY], kDead) * cfg.lookSpeedDeg * kDegToRad * dt;
    float speed = 1.0f;
    if (state.analog[inp::kAnalogA] > inp::kAnalogPressThreshold) {
        speed *= 4.0f;
    }
    if (state.analog[inp::kAnalogX] > inp::kAnalogPressThreshold) {
        speed *= 0.25f;
    }
    float zoom = 0.0f;
    if (state.buttons & inp::kButtonDpadUp) {
        zoom -= 1.0f;
    }
    if (state.buttons & inp::kButtonDpadDown) {
        zoom += 1.0f;
    }
    bool reset = (state.buttons & inp::kButtonBack) && !(prev.buttons & inp::kButtonBack);
    auto edge = [&](std::uint16_t bit) { return (state.buttons & bit) && !(prev.buttons & bit); };
    auto analogEdge = [&](int i) {
        return state.analog[i] > inp::kAnalogPressThreshold && prev.analog[i] <= inp::kAnalogPressThreshold;
    };
    auto keyEdge = [&](std::uint16_t vk) { return Key(raw, vk) && !s.keysBefore[vk]; };
    int speedSteps = (edge(inp::kButtonDpadRight) ? 1 : 0) - (edge(inp::kButtonDpadLeft) ? 1 : 0);
    speedSteps += (keyEdge(0xBB) || keyEdge(0x6B) ? 1 : 0) - (keyEdge(0xBD) || keyEdge(0x6D) ? 1 : 0);  // + / -
    if (speedSteps != 0) {
        s.speedFactor = std::clamp(s.speedFactor * std::pow(1.5f, static_cast<float>(speedSteps)), 0.02f, 50.0f);
        FUT_LOGI(Gfx, "freecam: speed x%.2f", static_cast<double>(s.speedFactor));
    }
    if (analogEdge(inp::kAnalogB) || keyEdge('B')) {
        s.backFaces = !s.backFaces;
        g_backFaces.store(s.backFaces, std::memory_order_relaxed);
        FUT_LOGI(Gfx, "freecam: back faces %s", s.backFaces ? "shown" : "culled");
    }
    if (analogEdge(inp::kAnalogY) || keyEdge('H')) {
        s.hideHud = !s.hideHud;
        g_hudHidden.store(s.hideHud, std::memory_order_relaxed);
        FUT_LOGI(Gfx, "freecam: HUD %s", s.hideHud ? "hidden" : "shown");
    }
    s.keysBefore = raw.keys;

    // Keyboard/mouse: WASD move, Q/E down/up, Shift faster, Ctrl slower, mouse look, wheel zoom, R = reset.
    fwd += (Key(raw, 'W') ? 1.0f : 0.0f) - (Key(raw, 'S') ? 1.0f : 0.0f);
    strafe += (Key(raw, 'D') ? 1.0f : 0.0f) - (Key(raw, 'A') ? 1.0f : 0.0f);
    rise += (Key(raw, 'E') ? 1.0f : 0.0f) - (Key(raw, 'Q') ? 1.0f : 0.0f);
    if (Key(raw, 0xA0) || Key(raw, 0xA1) || Key(raw, 0x10)) {  // VK_LSHIFT / VK_RSHIFT / VK_SHIFT
        speed *= 4.0f;
    }
    if (Key(raw, 0xA2) || Key(raw, 0xA3) || Key(raw, 0x11)) {  // VK_LCONTROL / VK_RCONTROL / VK_CONTROL
        speed *= 0.25f;
    }
    yaw += static_cast<float>(raw.dx) * cfg.mouseDegPerCount * kDegToRad;
    pitch -= static_cast<float>(raw.dy) * cfg.mouseDegPerCount * kDegToRad;
    zoom -= static_cast<float>(raw.wheel) * 4.0f;  // one notch = 4 ticks of the D-pad
    reset = reset || Key(raw, 'R');
    if (cfg.invertY) {
        pitch = -pitch;
    }

    fwd = std::clamp(fwd, -1.0f, 1.0f);
    strafe = std::clamp(strafe, -1.0f, 1.0f);
    rise = std::clamp(rise, -1.0f, 1.0f);
    if (reset) {
        s.started = false;  // the next Click takes the game camera again
        s.fovScale = 1.0f;
        return;
    }
    s.fovScale = std::clamp(s.fovScale * std::pow(1.02f, zoom), 0.15f, 2.5f);
    const float step = cfg.moveSpeed * s.sceneScale * s.speedFactor * speed * dt;
    Motion m;
    m.forward = fwd * step;
    m.strafe = strafe * step;
    m.rise = rise * step;
    m.yaw = yaw;
    m.pitch = pitch;
    s.rig.Step(m);
}

std::uint16_t ParseKey(const std::string& v) noexcept {
    if (v.size() >= 2 && (v[0] == 'f' || v[0] == 'F')) {
        const int n = std::atoi(v.c_str() + 1);
        if (n >= 1 && n <= 12) {
            return static_cast<std::uint16_t>(0x70 + n - 1);  // VK_F1..VK_F12
        }
    }
    return 0;
}

// ---- NiCamera::Click --------------------------------------------------------------------------------------------

std::uint32_t CallOriginal(guest::GuestAddr cam) noexcept {
    guest::GuestAddr t = g_trampoline.load(std::memory_order_relaxed);
    if (t == 0) {
        t = core::HookTrampoline(kNiCameraClick);
        if (t == 0) {
            core::Fatal("probe:NiCamera::Click at 0x%08X runs without an installed trampoline", kNiCameraClick);
        }
        g_trampoline.store(t, std::memory_order_relaxed);
    }
    return core::CallGuestThiscall(t, cam, {});
}

std::uint32_t __fastcall ClickProbe(std::uint32_t cam, std::uint32_t /*edx*/) noexcept {
    State& s = S();
    std::unique_lock<std::recursive_mutex> lock(s.mutex);
    const guest::GuestAddr renderer = guest::Read<std::uint32_t>(cam + ni::kNiCameraRenderer);
    const std::uint32_t target = gfx::RenderTargetOf(renderer);
    NoteCamera(s, cam, target);
    if (!s.active || renderer == 0 || target != 0) {
        lock.unlock();
        return CallOriginal(cam);
    }
    // One camera per displayed frame: the first scene camera of the frame. The other back-buffer cameras (HUD,
    // avatar portrait, full-screen helpers) keep the game's view.
    const std::uint32_t frame = gfx::FramesDisplayed();
    if (frame != s.lastFrame) {
        s.lastFrame = frame;
        s.frameCamera = 0;
        // The game never clears the back buffer: its levels always fill the screen. The freecam can look out of
        // a level into the void, where old pixels (and a hidden HUD) would stay - clear before the frame's first
        // back-buffer camera (measured in Level 1-1, run/freecam 2026-10-12).
        gfx::ClearBackBuffer(renderer, 0xFF000000u);  // black void (the game's background colour is dark green)
    }
    if (s.frameCamera == 0 && SceneCamera(cam)) {
        s.frameCamera = cam;
    }
    if (s.frameCamera != cam) {
        lock.unlock();
        return CallOriginal(cam);
    }
    if (!s.started) {
        s.rig.Start(ReadGamePose(cam));
        s.started = true;
        // Scene scale (heuristic): the front end set is about 3 units deep, a level hundreds; their near planes are
        // 0.1 and 0.25. The tempo follows (near / 0.25)^2: front end x0.16, levels x1.
        const float n = guest::Read<ni::NiFrustum>(cam + ni::kNiCameraFrustum).nearPlane / 0.25f;
        s.sceneScale = std::clamp(n * n, 0.01f, 16.0f);
        FUT_LOGI(Gfx, "freecam: starts at the game camera 0x%08X (world up %.0f %.0f %.0f, handedness %+.0f)", cam,
                 s.rig.WorldUp().x, s.rig.WorldUp().y, s.rig.WorldUp().z, s.rig.Handedness());
    }
    const Pose p = s.rig.Current();
    const float fov = s.fovScale;
    ++s.overrides;
    lock.unlock();

    std::uint8_t savedWorld[kSaveWorldEnd - kSaveWorldBegin];
    std::uint8_t savedDerived[kSaveDerivedEnd - kSaveDerivedBegin];
    std::memcpy(savedWorld, guest::Ptr<const void>(cam + kSaveWorldBegin), sizeof(savedWorld));
    std::memcpy(savedDerived, guest::Ptr<const void>(cam + kSaveDerivedBegin), sizeof(savedDerived));

    WriteVec(cam + kWorldRowDir, p.direction);
    WriteVec(cam + kWorldRowUp, p.up);
    WriteVec(cam + kWorldRowRight, p.right);
    WriteVec(cam + kWorldTranslate, p.position);
    if (fov != 1.0f) {
        auto f = guest::Read<ni::NiFrustum>(cam + ni::kNiCameraFrustum);
        f.left *= fov;
        f.right *= fov;
        f.top *= fov;
        f.bottom *= fov;
        guest::Write(cam + ni::kNiCameraFrustum, f);
    }
    RecomputeDerived(cam);
    g_sceneCamera.store(cam, std::memory_order_relaxed);
    const std::uint32_t result = CallOriginal(cam);

    std::memcpy(guest::Ptr<void>(cam + kSaveWorldBegin), savedWorld, sizeof(savedWorld));
    std::memcpy(guest::Ptr<void>(cam + kSaveDerivedBegin), savedDerived, sizeof(savedDerived));
    RecomputeDerived(cam);
    return result;
}

void LogSummary() noexcept {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    FUT_LOGI(Gfx, "freecam summary: %llu activations, %llu camera overrides, %u cameras seen",
             static_cast<unsigned long long>(s.activations), static_cast<unsigned long long>(s.overrides),
             s.seenCount);
    for (std::uint32_t i = 0; i < s.seenCount; ++i) {
        FUT_LOGI(Gfx, "freecam: camera 0x%08X target 0x%08X clicked %llu times", s.seen[i].camera, s.seen[i].target,
                 static_cast<unsigned long long>(s.seen[i].clicks));
    }
}

}  // namespace

void Configure() noexcept {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (s.configured) {
        return;
    }
    s.configured = true;
    const core::Config& cfg = core::GetConfig();
    auto str = [&](const char* key) {
        return cfg.GetString("freecam", key, core::ConfigKeyDefault("freecam", key));
    };
    auto num = [&](const char* key, float def) {
        const std::string v = str(key);
        char* end = nullptr;
        const float f = std::strtof(v.c_str(), &end);
        return (end != v.c_str() && std::isfinite(f)) ? f : def;
    };
    Settings& st = s.settings;
    st.enabled = cfg.GetBool("freecam", "enabled", true);
    st.padChord = str("toggle_pad") != "off";
    st.key = ParseKey(str("toggle_key"));
    st.moveSpeed = num("move_speed", st.moveSpeed);
    st.lookSpeedDeg = num("look_speed", st.lookSpeedDeg);
    st.mouseDegPerCount = 0.08f * num("mouse_sensitivity", 1.0f);
    st.invertY = cfg.GetBool("freecam", "invert_y", false);
    st.backFaces = cfg.GetBool("freecam", "back_faces", true);
    core::AddExitReporter("freecam", &LogSummary);
    FUT_LOGI(Gfx, "freecam: %s, toggle %s%s%s, move speed %.0f, look speed %.0f deg/s", st.enabled ? "ready" : "off",
             st.padChord ? "L3+R3" : "", st.padChord && st.key != 0 ? " / " : "",
             st.key != 0 ? str("toggle_key").c_str() : "", static_cast<double>(st.moveSpeed),
             static_cast<double>(st.lookSpeedDeg));
}

void SampleFilter(int port, inp::PadState& merged) noexcept {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (!s.settings.enabled || port < 0 || port >= inp::kPortCount) {
        return;
    }
    bool toggle = false;
    if (s.settings.padChord && s.chords[port].Filter(merged)) {
        toggle = true;
    }
    if (s.settings.key != 0 && port == (s.active ? s.port : 0)) {
        const bool down = inp::GlobalKeyboardMouse().KeyDown(s.settings.key);
        toggle = toggle || (down && !s.keyWasDown);
        s.keyWasDown = down;
    }
    if (!toggle) {
        return;
    }
    if (s.active) {
        Deactivate(s);
    } else {
        Activate(s, port);
    }
}

bool HudHidden() noexcept {
    return g_hudHidden.load(std::memory_order_relaxed);
}

void NoteCameraData(std::uint32_t location) noexcept {
    g_currentCamera.store(location != 0 ? location - ni::kNiCameraWorldLocation : 0, std::memory_order_relaxed);
}

bool SuppressOverlayDraw(std::uint32_t renderTarget, bool geometry) noexcept {
    if (!g_hudHidden.load(std::memory_order_relaxed) || renderTarget != 0) {
        return false;
    }
    if (geometry &&
        g_currentCamera.load(std::memory_order_relaxed) == g_sceneCamera.load(std::memory_order_relaxed)) {
        return false;
    }
    g_hudSkips.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ShowBackFaces() noexcept {
    return g_backFaces.load(std::memory_order_relaxed);
}

bool Active() noexcept {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    return s.active;
}

void SetActive(bool on, int port) noexcept {
    State& s = S();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (on) {
        Activate(s, port);
    } else {
        Deactivate(s);
    }
}

}  // namespace gfx::freecam

// NiCamera::Click prologue: mov eax, fs:[0] (6 bytes, fs offset 0 is not an fs patch site; position independent).
FUT_PROBE(gfx::freecam::kNiCameraClick, &gfx::freecam::ClickProbe, Thiscall, "probe:NiCamera::Click",
          "64 A1 00 00 00 00", 6);
