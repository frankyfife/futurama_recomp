// G3 widescreen (see widescreen.h).
#include "gfx/widescreen.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "core/guest.h"
#include "core/host_main.h"
#include "core/log.h"
#include "gfx/gfx_config.h"
#include "gfx/ni_guest_layouts.h"
#include "gfx/outline.h"
#include "platform/window.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
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
// The log starts over each time the game enters a level (anchoring off -> on), at most kMaxLogResets times: the
// front end alone used up the list before (user report 2026-10-14: no level HUD lines in the log).
constexpr std::uint32_t kMaxLoggedObjects = 96;
constexpr std::uint32_t kMaxLogResets = 16;
std::array<std::uint32_t, kMaxLoggedObjects> g_logged{};
std::uint32_t g_loggedCount = 0;
std::uint32_t g_logResets = 0;

// Exceptions to the anchoring by thirds (widescreen_math.h Classify rule 6). None needed so far: Level 1-1, 2-6, the
// pause menu, the breakdown screen, loading screens and the front end classify correctly by the rules (docs/
// g3_report.md 3). An entry: {source, guest-pixel area the draw lies within, placement}.
constexpr std::array<AnchorException, 0> kAnchorExceptions{};

// NiAVObject layout for the log (gfx/outline.h kAvWorld/kAvParent/kObjectName).
constexpr std::uint32_t kAvWorldTransform = 0x70;
constexpr std::uint32_t kAvParentOffset = 0x14;
constexpr std::uint32_t kObjectNameOffset = 0x08;

std::uint32_t g_anchorFrame = 0xFFFFFFFFu;
bool g_anchor = false;

// HUD groups of the current and the previous frame (guest main thread).
constexpr std::uint32_t kCameraPolygonArray = 0x164;  // NiCamera screen polygon NiTArray: data, +0x16C size
constexpr std::uint32_t kCameraPolygonSize = 0x16C;
constexpr std::uint32_t kMaxListedPolygons = 256;
constexpr std::uint32_t kMaxPieceVertices = 64;      // HUD sprites are quads; ribbons (404) are no pieces
constexpr std::uint32_t kMaxHudCameras = 8;
constexpr std::uint32_t kMaxGroupLogLines = 24;      // per level entered (g_logResets)
HudGroups g_hud, g_hudPrevious;
std::uint32_t g_hudFrame = 0xFFFFFFFFu;
std::uint32_t g_hudPreviousFrame = 0xFFFFFFFFu;
std::array<std::uint32_t, kMaxHudCameras> g_hudCameras{};
std::uint32_t g_hudCameraCount = 0;
std::uint64_t g_hudSignature = 0;
std::uint32_t g_groupLogLines = 0;
std::uint64_t g_groupedDraws = 0;
std::uint64_t g_movedDraws = 0;  // grouped draws whose group changed their placement
constexpr std::uint32_t kMaxMovedObjects = 48;
std::array<std::uint32_t, kMaxMovedObjects> g_moved{};
std::uint32_t g_movedCount = 0;

std::uint32_t ReadGuest32(std::uint32_t p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}

// The guest-pixel rectangle of a screen polygon (vertices x, y in 0..1 of `viewport`, y up; ni_extract.cpp
// ExtractScreenPolygon with the screen projection +0x394). False for ribbons, empty or large polygons.
bool PolygonRect(std::uint32_t polygon, const GuestRect& viewport, GuestRect* out) noexcept {
    if (polygon < 0x10000 || !guest::IsReadable(polygon, sizeof(ni::NiScreenPolygon)) ||
        outline::IsRibbonPolygon(polygon)) {
        return false;
    }
    const auto poly = guest::Read<ni::NiScreenPolygon>(polygon);
    const std::uint32_t n = poly.vertexCount;
    if (n < 3 || n > kMaxPieceVertices || poly.vertices < 0x10000 || !guest::IsReadable(poly.vertices, n * 12u)) {
        return false;
    }
    const float w = viewport.x1 - viewport.x0, h = viewport.y1 - viewport.y0;
    GuestRect r{1e30f, 1e30f, -1e30f, -1e30f};
    for (std::uint32_t i = 0; i < n; ++i) {
        const float x = viewport.x0 + guest::Read<float>(poly.vertices + i * 12u) * w;
        const float y = viewport.y0 + (1.0f - guest::Read<float>(poly.vertices + i * 12u + 4u)) * h;
        r = {std::fmin(r.x0, x), std::fmin(r.y0, y), std::fmax(r.x1, x), std::fmax(r.y1, y)};
    }
    *out = r;
    return true;
}

// One line per new grouping of a finished frame (a piece appeared or vanished, or one changed its group), capped.
void LogGroups(std::uint32_t frame) noexcept {
    std::uint64_t sig = 1469598103934665603ull;
    for (std::uint32_t i = 0; i < g_hud.PieceCount(); ++i) {
        sig = (sig ^ g_hud.PieceObject(i)) * 1099511628211ull;
        sig = (sig ^ g_hud.GroupOfPiece(i)) * 1099511628211ull;
    }
    if (sig == g_hudSignature) {
        return;
    }
    g_hudSignature = sig;
    if (g_groupLogLines >= kMaxGroupLogLines || g_hud.PieceCount() == 0) {
        return;
    }
    ++g_groupLogLines;
    char line[768];
    int used = std::snprintf(line, sizeof(line), "widescreen: frame %u HUD groups: %u pieces in %u groups", frame,
                             g_hud.PieceCount(), g_hud.GroupCount());
    for (std::uint32_t g = 0; g < g_hud.GroupCount() && used > 0 && used + 64 < static_cast<int>(sizeof(line)); ++g) {
        std::uint32_t pieces = 0;
        for (std::uint32_t i = 0; i < g_hud.PieceCount(); ++i) {
            pieces += g_hud.GroupOfPiece(i) == g ? 1u : 0u;
        }
        const GuestRect u = g_hud.GroupUnion(g);
        const int w = std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used),
                                    "%s (%.0f, %.0f)-(%.0f, %.0f) x%u -> %s", g == 0 ? ":" : ";", u.x0, u.y0, u.x1,
                                    u.y1, pieces, PlacementName(AnchorOf(u)));
        if (w <= 0) {
            break;
        }
        used += w;
    }
    FUT_LOGI(Gfx, "%s", line);
}

// The pieces of `camera`'s screen polygon list, once per frame.
void ReadCameraPolygons(std::uint32_t camera, const GuestRect& viewport) noexcept {
    for (std::uint32_t i = 0; i < g_hudCameraCount; ++i) {
        if (g_hudCameras[i] == camera) {
            return;
        }
    }
    if (g_hudCameraCount >= kMaxHudCameras) {
        return;
    }
    g_hudCameras[g_hudCameraCount++] = camera;
    const std::uint32_t array = ReadGuest32(camera + kCameraPolygonArray);
    const std::uint32_t size = ReadGuest32(camera + kCameraPolygonSize);
    if (array == 0 || size == 0 || size > kMaxListedPolygons || !guest::IsReadable(array, size * 4u)) {
        return;
    }
    bool added = false;
    for (std::uint32_t i = 0; i < size; ++i) {
        GuestRect r;
        const std::uint32_t polygon = guest::Read<std::uint32_t>(array + i * 4u);
        if (polygon != 0 && PolygonRect(polygon, viewport, &r)) {
            added = g_hud.Add(polygon, r) || added;
        }
    }
    if (added) {
        g_hud.Build();
    }
}

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
        const bool was = g_anchor;
        g_anchor = InContext("Ingame");
        if (g_anchor && !was && g_logResets < kMaxLogResets) {
            ++g_logResets;
            g_loggedCount = 0;
            g_groupLogLines = 0;
            g_hudSignature = 0;
            g_movedCount = 0;
            FUT_LOGI(Gfx, "widescreen: frame %u in a level (context Ingame) - its screen-space draws are logged anew",
                     frame);
        }
    }
    return g_anchor;
}

bool HudGroupOf(std::uint32_t camera, const GuestRect& viewport, std::uint32_t object, const GuestRect& bounds,
                std::uint32_t frame, GuestRect* group) noexcept {
    if (frame != g_hudFrame) {
        if (g_hudCameraCount != 0) {  // the groups of the finished frame
            LogGroups(g_hudFrame);
        }
        g_hudPrevious = g_hud;  // for the draws of the new frame before its first list
        g_hudPreviousFrame = g_hudFrame;
        g_hudFrame = frame;
        g_hud.Clear();
        g_hudCameraCount = 0;
    }
    if (camera != 0 && viewport.x1 > viewport.x0 && viewport.y1 > viewport.y0) {
        ReadCameraPolygons(camera, viewport);
    }
    const bool found = g_hud.GroupRect(object, bounds, group) ||
                       (g_hudCameraCount == 0 && frame == g_hudPreviousFrame + 1 &&
                        g_hudPrevious.GroupRect(object, bounds, group));
    g_groupedDraws += found ? 1u : 0u;
    return found;
}

void NoteDraw(const DrawFacts& facts, Placement placement, std::uint32_t object, std::uint32_t frame,
              std::uint32_t camera, std::uint32_t transform) noexcept {
    ++g_placements[static_cast<std::uint32_t>(placement) & 3u];
    if (facts.source == DrawSource::Clear || (facts.camera == DrawFacts::Camera::Widened &&
                                              facts.source == DrawSource::Geometry)) {
        return;  // the 3D scene and the clears are not logged one by one
    }
    // A HUD draw its group placed differently than its own rectangle would have (the G3 tearing, 2026-10-14).
    if (facts.grouped && facts.anchoring && placement == AnchorOf(facts.group) && placement != AnchorOf(facts.bounds)) {
        ++g_movedDraws;
        bool known = false;
        for (std::uint32_t i = 0; i < g_movedCount && !known; ++i) {
            known = g_moved[i] == object;
        }
        if (!known && g_movedCount < kMaxMovedObjects) {
            g_moved[g_movedCount++] = object;
            FUT_LOGI(Gfx,
                     "widescreen: frame %u %s 0x%08X bounds (%.0f, %.0f)-(%.0f, %.0f) alone -> %s, with its HUD group "
                     "(%.0f, %.0f)-(%.0f, %.0f) -> %s",
                     frame, facts.source == DrawSource::ScreenPolygon ? "screen polygon" : "geometry", object,
                     facts.bounds.x0, facts.bounds.y0, facts.bounds.x1, facts.bounds.y1,
                     PlacementName(AnchorOf(facts.bounds)), facts.group.x0, facts.group.y0, facts.group.x1,
                     facts.group.y1, PlacementName(placement));
        }
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
    // A geometry draw: its NiAVObject (transform - 0x70) and up to three parents by name.
    char nodes[160];
    nodes[0] = 0;
    std::size_t used = 0;
    std::uint32_t av = transform > kAvWorldTransform ? transform - kAvWorldTransform : 0;
    for (int depth = 0; av != 0 && depth < 4 && used + 40 < sizeof(nodes); ++depth) {
        const std::uint32_t name = ReadGuest32(av + kObjectNameOffset);
        char text[24] = "-";
        if (name != 0 && guest::IsReadable(name, 1)) {
            std::size_t n = 0;
            while (n + 1 < sizeof(text) && guest::IsReadable(name + n, 1) && guest::Read<char>(name + n) != 0) {
                text[n] = guest::Read<char>(name + n);
                ++n;
            }
            text[n] = 0;
        }
        const int w = std::snprintf(nodes + used, sizeof(nodes) - used, "%s0x%08X '%s'", depth == 0 ? ", node " : " < ",
                                    av, text);
        if (w <= 0) {
            break;
        }
        used += static_cast<std::size_t>(w);
        av = ReadGuest32(av + kAvParentOffset);
    }
    // A screen polygon: its index in the camera's list (NiCamera::Click draws the list).
    if (facts.source == DrawSource::ScreenPolygon && camera != 0 && used + 32 < sizeof(nodes)) {
        const std::uint32_t array = ReadGuest32(camera + kCameraPolygonArray);
        const std::uint32_t size = ReadGuest32(camera + kCameraPolygonSize);
        std::uint32_t index = 0xFFFFFFFFu;
        for (std::uint32_t i = 0; array != 0 && i < size && i < kMaxListedPolygons; ++i) {
            if (ReadGuest32(array + 4 * i) == object) {
                index = i;
                break;
            }
        }
        const int w = std::snprintf(nodes + used, sizeof(nodes) - used, ", list %d/%u",
                                    index == 0xFFFFFFFFu ? -1 : static_cast<int>(index), size);
        if (w > 0) {
            used += static_cast<std::size_t>(w);
        }
    }
    FUT_LOGI(Gfx,
             "widescreen: frame %u %s 0x%08X (camera 0x%08X %s%s%s, %s) bounds (%.0f, %.0f)-(%.0f, %.0f)%s -> %s%s",
             frame, kSource[static_cast<std::uint32_t>(facts.source) & 3u], object, camera,
             kCamera[static_cast<std::uint32_t>(facts.camera) % 3u], facts.hudCamera ? ", hud" : "",
             facts.ribbon ? ", ribbon" : "", facts.textured ? "textured" : "untextured", facts.bounds.x0,
             facts.bounds.y0, facts.bounds.x1, facts.bounds.y1, facts.anchoring ? ", level" : "",
             PlacementName(placement), nodes);
}

void NoteUnwidenedQuad() noexcept {
    ++g_unwidenedQuads;
}

void LogSummary() noexcept {
    FUT_LOGI(Gfx,
             "widescreen: %u:%u, %llu widened camera clicks; placements stretch %llu, center %llu, left %llu, right "
             "%llu (%llu HUD draws anchored by their group, %llu of them placed differently than alone); %llu "
             "full-screen quads drawn unwidened",
             GetDisplay().aspectW, GetDisplay().aspectH,
             static_cast<unsigned long long>(g_widenedClicks.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(g_placements[0]), static_cast<unsigned long long>(g_placements[1]),
             static_cast<unsigned long long>(g_placements[2]), static_cast<unsigned long long>(g_placements[3]),
             static_cast<unsigned long long>(g_groupedDraws), static_cast<unsigned long long>(g_movedDraws),
             static_cast<unsigned long long>(g_unwidenedQuads));
}

}  // namespace gfx::widescreen
