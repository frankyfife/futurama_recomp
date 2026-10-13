// G3 widescreen (see widescreen.h).
#include "gfx/widescreen.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "core/guest.h"
#include "core/host_main.h"
#include "core/log.h"
#include "gfx/gfx_config.h"
#include "gfx/ni_guest_layouts.h"
#include "platform/window.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

namespace gfx::widescreen {
namespace {

std::string Normalized(std::string_view text) {
    std::string s;
    for (char c : text) {
        if (c != ' ' && c != '\t') {
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return s;
}

// Widened cameras whose Click is running (nested Clicks of other cameras happen: HUD cameras inside the scene's).
struct Widened {
    std::uint32_t camera = 0;
    float factor = 1.0f;
};
constexpr std::uint32_t kMaxNested = 4;
std::array<Widened, kMaxNested> g_widened{};  // guest main thread only (Click and slot 47 run there)
std::uint32_t g_widenedCount = 0;
std::atomic<std::uint64_t> g_widenedClicks{0};

// Statistics (guest main thread).
std::uint64_t g_placements[4] = {};
std::uint64_t g_unwidenedQuads = 0;
constexpr std::uint32_t kMaxLoggedObjects = 64;
std::array<std::uint32_t, kMaxLoggedObjects> g_logged{};
std::uint32_t g_loggedCount = 0;

// Exceptions to the anchoring by thirds (widescreen_math.h Classify rule 6). None needed so far: Level 1-1, 2-6, the
// pause menu, the breakdown screen, loading screens and the front end classify correctly by the rules (docs/
// g3_report.md 3). An entry: {source, guest-pixel area the draw lies within, placement}.
constexpr std::array<AnchorException, 0> kAnchorExceptions{};

std::uint32_t g_anchorFrame = 0xFFFFFFFFu;
bool g_anchor = false;

// The game's current context is `want` ("Ingame", "Frontend"; NUL included in the size).
template <std::size_t N>
bool InContext(const char (&want)[N]) noexcept {
    // [[g_pApp] + 0xBC] = current context, [context + 4] = its name (input/input_system.cpp GameInFrontendMenu).
    auto read = [](std::uint32_t p) -> std::uint32_t {
        return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
    };
    const std::uint32_t app = read(guest::kGameAppPtr);
    const std::uint32_t context = app != 0 ? read(app + guest::kCAppCurrentContextOffset) : 0;
    const std::uint32_t name = context != 0 ? read(context + guest::kGameContextNameOffset) : 0;
    if (name == 0 || !guest::IsReadable(name, N)) {
        return false;
    }
    return std::memcmp(guest::Ptr<const char>(name), want, N) == 0;
}

void MonitorSize(const std::string& monitor, std::uint32_t* w, std::uint32_t* h) noexcept {
    // Physical pixels (the boot thread may be DPI unaware: 3840x2160 at 225 % would read as 1707x960).
    const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const platform::MonitorChoice choice = platform::SelectMonitor(monitor.empty() ? nullptr : monitor.c_str());
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (choice.monitor != nullptr && GetMonitorInfoW(choice.monitor, &mi)) {
        *w = static_cast<std::uint32_t>(mi.rcMonitor.right - mi.rcMonitor.left);
        *h = static_cast<std::uint32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    if (previous != nullptr) {
        SetThreadDpiAwarenessContext(previous);
    }
}

Display Resolve() noexcept {
    Display d;
    std::string text = "original";
    try {
        text = core::GetConfig().GetString("render", "aspect", std::string(core::ConfigKeyDefault("render", "aspect")));
    } catch (...) {
    }
    if (!ParseAspect(text, &d.mode)) {
        FUT_LOGW(Gfx, "[render] aspect = '%s' is not original, 16:9, 16:10, 21:9 or monitor - using original",
                 text.c_str());
        d.mode = AspectMode::Original;
    }
    if (d.mode == AspectMode::Original) {
        return d;
    }
    const Settings& s = GetSettings();
    std::uint32_t mw = 0, mh = 0;
    if (d.mode == AspectMode::Monitor && (s.resolutionWidth == 0 || s.resolutionHeight == 0)) {
        MonitorSize(s.monitor, &mw, &mh);
    }
    AspectOf(d.mode, s.resolutionWidth, s.resolutionHeight, mw, mh, &d.aspectW, &d.aspectH);
    d.factor = HorPlusFactor(d.aspectW, d.aspectH);
    // Narrower than 4:3 (a portrait monitor) or within 1 % of it: nothing to widen.
    d.active = d.factor > 1.01f;
    if (!d.active) {
        FUT_LOGI(Gfx, "widescreen: [render] aspect = %s gives %u:%u - no wider than 4:3, original picture",
                 AspectModeName(d.mode), d.aspectW, d.aspectH);
        d.aspectW = d.aspectH = 0;
        d.factor = 1.0f;
        return d;
    }
    FUT_LOGI(Gfx, "widescreen: [render] aspect = %s -> %u:%u, Hor+ factor %.4f (vertical field of view kept), HUD "
             "anchored at the edges in levels", AspectModeName(d.mode), d.aspectW, d.aspectH,
             static_cast<double>(d.factor));
    core::AddExitReporter("widescreen", &LogSummary);
    return d;
}

}  // namespace

bool ParseAspect(std::string_view text, AspectMode* mode) noexcept {
    std::string s;
    try {
        s = Normalized(text);
    } catch (...) {
        return false;
    }
    static constexpr struct {
        const char* word;
        AspectMode mode;
    } kWords[] = {{"original", AspectMode::Original},
                  {"16:9", AspectMode::Wide16x9},
                  {"16:10", AspectMode::Wide16x10},
                  {"21:9", AspectMode::Wide21x9},
                  {"monitor", AspectMode::Monitor}};
    for (const auto& w : kWords) {
        if (s == w.word) {
            *mode = w.mode;
            return true;
        }
    }
    return false;
}

const char* AspectModeName(AspectMode mode) noexcept {
    switch (mode) {
    case AspectMode::Original:
        return "original";
    case AspectMode::Wide16x9:
        return "16:9";
    case AspectMode::Wide16x10:
        return "16:10";
    case AspectMode::Wide21x9:
        return "21:9";
    case AspectMode::Monitor:
        return "monitor";
    }
    return "?";
}

void AspectOf(AspectMode mode, std::uint32_t resolutionW, std::uint32_t resolutionH, std::uint32_t monitorW,
              std::uint32_t monitorH, std::uint32_t* aspectW, std::uint32_t* aspectH) noexcept {
    std::uint32_t w = 0, h = 0;
    switch (mode) {
    case AspectMode::Original:
        break;
    case AspectMode::Wide16x9:
        w = 16, h = 9;
        break;
    case AspectMode::Wide16x10:
        w = 16, h = 10;
        break;
    case AspectMode::Wide21x9:
        w = 64, h = 27;  // "21:9" panels are 2560x1080 (64:27) or 3440x1440 (43:18); 64:27 is the common one
        break;
    case AspectMode::Monitor:
        if (resolutionW != 0 && resolutionH != 0) {
            w = resolutionW, h = resolutionH;
        } else if (monitorW != 0 && monitorH != 0) {
            w = monitorW, h = monitorH;
        }
        break;
    }
    // A pixel size reduced (1920x1080 -> 16:9) for the log; the ratio stays exact.
    std::uint32_t g = 1;
    if (mode == AspectMode::Monitor && w != 0 && h != 0) {
        std::uint32_t a = w, b = h;
        while (b != 0) {
            const std::uint32_t t = a % b;
            a = b;
            b = t;
        }
        g = a;
    }
    *aspectW = w / g;
    *aspectH = h / g;
}

const Display& GetDisplay() noexcept {
    static const Display d = Resolve();
    return d;
}

float FrustumFactor(std::uint32_t camera) noexcept {
    const Display& d = GetDisplay();
    if (!d.active || camera == 0) {
        return 1.0f;
    }
    // The front end stays 4:3 with bars: its 3D set parks the panels of the other menus just outside the 4:3 view
    // (16:9 run: the edge of the options panel shows at the left). Levels, the pause menu and cut scenes get Hor+.
    if (InContext("Frontend")) {
        return 1.0f;
    }
    const auto f = guest::Read<ni::NiFrustum>(camera + ni::kNiCameraFrustum);
    const auto port = guest::Read<ni::NiRectF>(camera + ni::kNiCameraPort);
    return WidenableCamera(f, port) ? d.factor : 1.0f;
}

void BeginWidened(std::uint32_t camera, float factor) noexcept {
    if (g_widenedCount < kMaxNested) {
        g_widened[g_widenedCount] = {camera, factor};
    }
    ++g_widenedCount;
    g_widenedClicks.fetch_add(1, std::memory_order_relaxed);
}

void EndWidened(std::uint32_t camera) noexcept {
    (void)camera;
    if (g_widenedCount > 0) {
        --g_widenedCount;
    }
}

float WidenedFactor(std::uint32_t camera) noexcept {
    const std::uint32_t n = g_widenedCount < kMaxNested ? g_widenedCount : kMaxNested;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (g_widened[i].camera == camera) {
            return g_widened[i].factor;
        }
    }
    return 1.0f;
}

std::span<const AnchorException> AnchorExceptions() noexcept {
    return kAnchorExceptions;
}

bool AnchoringAllowed(std::uint32_t frame) noexcept {
    if (frame != g_anchorFrame) {
        g_anchorFrame = frame;
        g_anchor = InContext("Ingame");
    }
    return g_anchor;
}

void NoteDraw(const DrawFacts& facts, Placement placement, std::uint32_t object, std::uint32_t frame) noexcept {
    ++g_placements[static_cast<std::uint32_t>(placement) & 3u];
    if (facts.source == DrawSource::Clear || (facts.camera == DrawFacts::Camera::Widened &&
                                              facts.source == DrawSource::Geometry)) {
        return;  // the 3D scene and the clears are not logged one by one
    }
    for (std::uint32_t i = 0; i < g_loggedCount; ++i) {
        if (g_logged[i] == object) {
            return;
        }
    }
    if (g_loggedCount >= kMaxLoggedObjects) {
        return;
    }
    g_logged[g_loggedCount++] = object;
    static constexpr const char* kSource[] = {"geometry", "screen polygon", "screen texture", "clear"};
    static constexpr const char* kCamera[] = {"hor+", "port", "full width"};
    FUT_LOGI(Gfx,
             "widescreen: frame %u %s 0x%08X (camera %s%s%s, %s) bounds (%.0f, %.0f)-(%.0f, %.0f)%s -> %s", frame,
             kSource[static_cast<std::uint32_t>(facts.source) & 3u], object,
             kCamera[static_cast<std::uint32_t>(facts.camera) % 3u], facts.hudCamera ? ", hud" : "",
             facts.ribbon ? ", ribbon" : "", facts.textured ? "textured" : "untextured", facts.bounds.x0,
             facts.bounds.y0, facts.bounds.x1, facts.bounds.y1, facts.anchoring ? ", level" : "",
             PlacementName(placement));
}

void NoteUnwidenedQuad() noexcept {
    ++g_unwidenedQuads;
}

void LogSummary() noexcept {
    FUT_LOGI(Gfx,
             "widescreen: %u:%u, %llu widened camera clicks; placements stretch %llu, center %llu, left %llu, right "
             "%llu; %llu full-screen quads drawn unwidened",
             GetDisplay().aspectW, GetDisplay().aspectH,
             static_cast<unsigned long long>(g_widenedClicks.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_placements[0]), static_cast<unsigned long long>(g_placements[1]),
             static_cast<unsigned long long>(g_placements[2]), static_cast<unsigned long long>(g_placements[3]),
             static_cast<unsigned long long>(g_unwidenedQuads));
}

}  // namespace gfx::widescreen
