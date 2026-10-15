// G3 widescreen: the pure part (tests/widescreen_tests.cpp) - Hor+ frustum, guest-pixel -> internal-pixel placement of
// one draw and the classification rules that pick the placement. gfx/widescreen.h has the stateful part (settings,
// the NiCamera::Click side, the game context).
//
// Model: the guest keeps its 640x480 raster (slots 21/22, viewports, pre-transformed pixels). With [render] aspect !=
// original the back buffer has the display aspect (e.g. 16:9) and every draw is mapped from guest pixels into it with
// one of four placements:
//   Stretch  guest 0..640 -> the whole width (3D of a Hor+ camera whose frustum is widened by the same factor, full-
//            screen fades)
//   Center   guest 0..640 -> the centred 4:3 area (menus, texts, loading screens, movies; the rest are bars)
//   Left     guest 0..640 -> a 4:3 area at the left edge (HUD elements of the left third)
//   Right    guest 0..640 -> a 4:3 area at the right edge (HUD elements of the right third)
// The vertical mapping is the same for all four (guest 480 lines -> the back buffer height).
#pragma once

#include "gfx/ni_guest_layouts.h"
#include "gfx/rhi/rhi.h"

#include <cmath>
#include <cstdint>
#include <span>

namespace gfx::widescreen {

enum class Placement : std::uint8_t { Stretch, Center, Left, Right };
inline const char* PlacementName(Placement p) noexcept {
    switch (p) {
    case Placement::Stretch:
        return "stretch";
    case Placement::Center:
        return "center";
    case Placement::Left:
        return "left";
    case Placement::Right:
        return "right";
    }
    return "?";
}

inline constexpr float kGuestWidth = 640.0f;
inline constexpr float kGuestHeight = 480.0f;

// Hor+ factor: how much wider the display is than the guest's 4:3 (1 = no change). aspectW/aspectH = display aspect
// (0 -> 1).
inline float HorPlusFactor(std::uint32_t aspectW, std::uint32_t aspectH, float guestW = kGuestWidth,
                           float guestH = kGuestHeight) noexcept {
    if (aspectW == 0 || aspectH == 0 || guestW <= 0.0f || guestH <= 0.0f) {
        return 1.0f;
    }
    const float k = (static_cast<float>(aspectW) / static_cast<float>(aspectH)) / (guestW / guestH);
    return std::isfinite(k) && k > 0.0f ? k : 1.0f;
}

// Hor+: the horizontal extents (left/right at the near plane) times k, the vertical ones unchanged - the vertical
// field of view stays, the horizontal one grows. Works for off-centre frusta too (both edges scale around the axis).
inline void WidenFrustum(ni::NiFrustum* f, float k) noexcept {
    f->left *= k;
    f->right *= k;
}

// The scene cameras that get Hor+: perspective with a real depth range, the view axis inside the frustum, and a port
// over the whole width (the height may be letterboxed). Not widened: the full-screen helper cameras (n 1, f 2), the
// HUD cameras (l 0, r 0.1: the axis on the frustum edge), the avatar camera (port in the left third) - measured in
// the front end, the pause menu and Level 1-1 (docs/g3_report.md).
inline bool WidenableCamera(const ni::NiFrustum& f, const ni::NiRectF& port) noexcept {
    const bool finite = std::isfinite(f.left) && std::isfinite(f.right) && std::isfinite(f.top) &&
                        std::isfinite(f.bottom) && std::isfinite(f.nearPlane) && std::isfinite(f.farPlane);
    const bool fullWidth = port.left <= 0.01f && port.right >= 0.99f && port.top > port.bottom;
    return finite && fullWidth && f.nearPlane > 0.0f && f.farPlane >= 50.0f * f.nearPlane && f.left < 0.0f &&
           f.right > 0.0f && f.bottom < 0.0f && f.top > 0.0f;
}

// Guest pixel x -> internal pixel x: x * scaleX + offsetX; y -> y * scaleY.
struct Mapping {
    double scaleX = 1.0, offsetX = 0.0, scaleY = 1.0;
};
inline Mapping MappingOf(Placement p, std::uint32_t guestW, std::uint32_t guestH, std::uint32_t internalW,
                         std::uint32_t internalH) noexcept {
    Mapping m;
    if (guestW == 0 || guestH == 0) {
        return m;
    }
    m.scaleY = static_cast<double>(internalH) / guestH;
    if (p == Placement::Stretch) {
        m.scaleX = static_cast<double>(internalW) / guestW;
        return m;
    }
    m.scaleX = m.scaleY;  // square guest pixels: the 4:3 area is internalH * 4/3 wide
    const double area = guestW * m.scaleX;
    const double free = static_cast<double>(internalW) - area;
    m.offsetX = p == Placement::Left ? 0.0 : p == Placement::Right ? free : std::floor(free / 2.0 + 0.5);
    return m;
}

inline rhi::Viewport PlaceViewport(const rhi::Viewport& v, const Mapping& m) noexcept {
    rhi::Viewport r = v;
    r.x = static_cast<float>(v.x * m.scaleX + m.offsetX);
    r.y = static_cast<float>(v.y * m.scaleY);
    r.width = static_cast<float>(v.width * m.scaleX);
    r.height = static_cast<float>(v.height * m.scaleY);
    return r;
}

// Edges rounded to the nearest internal pixel (rectangles that tile stay tiled).
inline rhi::Rect PlaceRect(const rhi::Rect& rect, const Mapping& m) noexcept {
    auto x = [&](std::int32_t v) { return static_cast<std::int32_t>(std::floor(v * m.scaleX + m.offsetX + 0.5)); };
    auto y = [&](std::int32_t v) { return static_cast<std::int32_t>(std::floor(v * m.scaleY + 0.5)); };
    rhi::Rect r;
    r.left = x(rect.left);
    r.right = x(rect.right);
    r.top = y(rect.top);
    r.bottom = y(rect.bottom);
    return r;
}

// ---- classification ------------------------------------------------------------------------------------------------

// A rectangle in the guest raster (pixels, x0 < x1, y0 < y1).
struct GuestRect {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// True if `r` covers the whole 640x480 raster (a fade, a backdrop, a loading picture); `slack` pixels may be missing
// at each edge.
inline bool CoversScreen(const GuestRect& r, float slack = 8.0f) noexcept {
    return r.x0 <= slack && r.y0 <= slack && r.x1 >= kGuestWidth - slack && r.y1 >= kGuestHeight - slack;
}

// Edge anchoring of a HUD rectangle by its centre: left third -> Left, right third -> Right, else Center.
inline Placement AnchorOf(const GuestRect& r) noexcept {
    const float cx = 0.5f * (r.x0 + r.x1);
    if (cx < kGuestWidth / 3.0f) {
        return Placement::Left;
    }
    if (cx > kGuestWidth * 2.0f / 3.0f) {
        return Placement::Right;
    }
    return Placement::Center;
}

// ---- HUD groups (2026-10-14) ----------------------------------------------------------------------------------------
// A HUD element of the game is several screen polygons (the 4-1 swamp timer: clock, bar, progress fill and finish
// flag, each its own NiScreenPolygon at a fixed guest-pixel position, FUN_000E5B70) plus the digits of a HUD camera.
// Anchored one by one, the pieces of an element that straddles the thirds were torn apart (clock and fill at the
// left edge, bar in the centre, flag at the right edge). The pieces of one frame are therefore grouped by proximity:
// two pieces belong to one group if their rectangles are at most kHudGroupGap guest pixels apart in x and in y
// (transitively). A group is anchored as a whole by the centre of its union, so it keeps its 4:3 layout.
//   - Gap: the pieces of one element are 4 px apart (4-1: clock 156..188, bar 192..448, flag 452..484); separate
//     elements are 36 px and more apart (Nibbler counter 56..120 and the clock; 1-1, 4-1 measured).
//   - Pieces wider than half the raster (banners, letterbox bars) join no group: they would pull the edge elements
//     next to them into the centre.
//   - Full-screen rectangles (fades) and empty ones are no pieces.
inline constexpr float kHudGroupGap = 16.0f;
inline constexpr float kHudPieceMaxWidth = kGuestWidth / 2.0f;

// Largest of the horizontal and vertical distance between two rectangles (0 or negative when they overlap).
inline float RectDistance(const GuestRect& a, const GuestRect& b) noexcept {
    const float dx = std::fmax(a.x0 - b.x1, b.x0 - a.x1);
    const float dy = std::fmax(a.y0 - b.y1, b.y0 - a.y1);
    return std::fmax(dx, dy);
}
inline GuestRect RectUnion(const GuestRect& a, const GuestRect& b) noexcept {
    return {std::fmin(a.x0, b.x0), std::fmin(a.y0, b.y0), std::fmax(a.x1, b.x1), std::fmax(a.y1, b.y1)};
}
// Can `r` be a piece of a HUD group (finite, not empty, not full-screen, not a banner)?
inline bool HudPiece(const GuestRect& r) noexcept {
    const bool finite = std::isfinite(r.x0) && std::isfinite(r.y0) && std::isfinite(r.x1) && std::isfinite(r.y1);
    return finite && r.x1 > r.x0 && r.y1 > r.y0 && !CoversScreen(r) && r.x1 - r.x0 <= kHudPieceMaxWidth;
}

// The HUD pieces of one frame and their groups (fixed capacity, no allocation; the renderer fills it from the screen
// polygon lists of the cameras, gfx/widescreen.cpp).
class HudGroups {
public:
    static constexpr std::uint32_t kMaxPieces = 64;

    void Clear() noexcept {
        count_ = 0;
        groups_ = 0;
    }
    // Adds a piece (`object` = the guest object, e.g. the NiScreenPolygon). Ignored: no HudPiece, a known object, a
    // full table. Call Build() after a batch of Add()s.
    bool Add(std::uint32_t object, const GuestRect& r) noexcept {
        if (!HudPiece(r) || count_ >= kMaxPieces) {
            return false;
        }
        for (std::uint32_t i = 0; i < count_; ++i) {
            if (pieces_[i].object == object) {
                return false;
            }
        }
        pieces_[count_++] = {object, r, 0};
        return true;
    }
    // Single-linkage grouping of all pieces: within `gap` of any piece of a group -> that group.
    void Build(float gap = kHudGroupGap) noexcept {
        std::uint8_t root[kMaxPieces];
        for (std::uint32_t i = 0; i < count_; ++i) {
            root[i] = static_cast<std::uint8_t>(i);
        }
        auto find = [&root](std::uint32_t i) {
            while (root[i] != i) {
                root[i] = root[root[i]];
                i = root[i];
            }
            return i;
        };
        for (std::uint32_t i = 0; i < count_; ++i) {
            for (std::uint32_t j = i + 1; j < count_; ++j) {
                if (RectDistance(pieces_[i].rect, pieces_[j].rect) <= gap) {
                    const std::uint32_t a = find(i), b = find(j);
                    if (a != b) {
                        root[a > b ? a : b] = static_cast<std::uint8_t>(a > b ? b : a);
                    }
                }
            }
        }
        groups_ = 0;
        std::uint8_t groupOfRoot[kMaxPieces];
        for (std::uint32_t i = 0; i < count_; ++i) {
            const std::uint32_t r = find(i);
            if (r == i) {
                groupOfRoot[i] = static_cast<std::uint8_t>(groups_);
                unions_[groups_++] = pieces_[i].rect;
            }
        }
        for (std::uint32_t i = 0; i < count_; ++i) {
            const std::uint8_t g = groupOfRoot[find(i)];
            pieces_[i].group = g;
            unions_[g] = RectUnion(unions_[g], pieces_[i].rect);
        }
    }
    std::uint32_t PieceCount() const noexcept { return count_; }
    std::uint32_t GroupCount() const noexcept { return groups_; }
    // Group of piece `i` (Build() first) and the union of group `g`.
    std::uint32_t GroupOfPiece(std::uint32_t i) const noexcept { return i < count_ ? pieces_[i].group : 0; }
    std::uint32_t PieceObject(std::uint32_t i) const noexcept { return i < count_ ? pieces_[i].object : 0; }
    GuestRect GroupUnion(std::uint32_t g) const noexcept { return g < groups_ ? unions_[g] : GuestRect{}; }
    // The rectangle a draw is anchored by: the union of its group with `r` - the group of the piece `object`, else
    // the nearest group within `gap` of `r` (a draw that is no piece, e.g. the digits of a HUD camera next to their
    // icon). False if there is none (the draw is anchored by `r` alone).
    bool GroupRect(std::uint32_t object, const GuestRect& r, GuestRect* out, float gap = kHudGroupGap) const noexcept {
        std::uint32_t best = groups_;
        if (object != 0) {
            for (std::uint32_t i = 0; i < count_; ++i) {
                if (pieces_[i].object == object) {
                    best = pieces_[i].group;
                    break;
                }
            }
        }
        if (best == groups_ && HudPiece(r)) {
            float nearest = gap;
            for (std::uint32_t g = 0; g < groups_; ++g) {
                const float d = RectDistance(unions_[g], r);
                if (d <= nearest) {
                    nearest = d;
                    best = g;
                }
            }
        }
        if (best == groups_) {
            return false;
        }
        *out = RectUnion(unions_[best], r);
        return true;
    }

private:
    struct Piece {
        std::uint32_t object = 0;
        GuestRect rect;
        std::uint8_t group = 0;
    };
    Piece pieces_[kMaxPieces] = {};
    GuestRect unions_[kMaxPieces] = {};
    std::uint32_t count_ = 0;
    std::uint32_t groups_ = 0;
};

// Exceptions to the anchoring rule: a draw of `source` whose rectangle lies within `area` (guest pixels) gets
// `placement`. Empty = no exceptions (Level 1-1, the pause menu, loading screens and the front end need none; the
// table is the place for elements found later, docs/g3_report.md).
enum class DrawSource : std::uint8_t { Geometry, ScreenPolygon, ScreenTexture, Clear };
struct AnchorException {
    DrawSource source;
    GuestRect area;
    Placement placement;
};

// What the renderer knows about one back-buffer draw.
struct DrawFacts {
    DrawSource source = DrawSource::Geometry;
    // The camera data in effect: a Hor+ camera (widened frustum), a camera whose port does not span the width
    // (classified as a whole by its port), or a full-width camera that is not widened (HUD cameras, helpers).
    enum class Camera : std::uint8_t { Widened, Port, FullWidth } camera = Camera::FullWidth;
    bool hudCamera = false;    // FullWidth: an off-axis camera (l >= 0, b >= 0) like the HUD digit cameras
    GuestRect port;            // Port: the camera's viewport in guest pixels
    bool ribbon = false;       // NiOutline ribbon polygon (G1 technique B: screen polygons computed from 3D)
    bool hasBounds = false;    // `bounds` valid (at least one vertex in front of the camera)
    GuestRect bounds;          // screen bounding box in guest pixels
    bool textured = false;     // stage 0 has a texture
    bool anchoring = false;    // HUD anchoring allowed (the game is in a level: context "Ingame")
    bool grouped = false;      // `group` valid: the draw belongs to a HUD group (HudGroups::GroupRect)
    GuestRect group;           // the union of its group (with `bounds`); rule 6 anchors by it instead of `bounds`
};

// The placement rules (docs/g3_report.md 2.3):
//  1. Hor+ camera: its geometry and outline ribbons -> Stretch.
//  2. Port camera (avatar portrait): the port decides for all its draws (anchored by the port's centre).
//  3. Geometry of a full-width camera that is neither Hor+ nor a HUD camera (l >= 0, b >= 0) -> Center: a 3D scene
//     without Hor+ (the front end) stays one undistorted 4:3 picture.
//  4. Anything covering the whole raster: untextured (fades) -> Stretch, textured (loading pictures, movies) -> Center.
//  5. Screen textures (slot 55: loading screens, tiles of one picture) -> Center.
//  6. Exceptions table (by the draw's own rectangle), then - only in a level - the third of the raster the centre
//     of the draw's HUD group lies in (2026-10-14; without a group the draw's own centre).
//  7. Otherwise Center.
inline Placement Classify(const DrawFacts& d, std::span<const AnchorException> exceptions = {}) noexcept {
    if (d.camera == DrawFacts::Camera::Widened && (d.source == DrawSource::Geometry || d.ribbon)) {
        return Placement::Stretch;
    }
    if (d.camera == DrawFacts::Camera::Port && (d.source == DrawSource::Geometry || d.ribbon)) {
        return d.anchoring ? AnchorOf(d.port) : Placement::Center;
    }
    if (d.source == DrawSource::Geometry && d.camera == DrawFacts::Camera::FullWidth && !d.hudCamera) {
        return Placement::Center;  // 3D of a camera without Hor+ (the front end): one undistorted 4:3 picture
    }
    if (!d.hasBounds) {
        return Placement::Center;
    }
    if (CoversScreen(d.bounds)) {
        return d.textured ? Placement::Center : Placement::Stretch;
    }
    if (d.source == DrawSource::ScreenTexture) {
        return Placement::Center;
    }
    for (const AnchorException& e : exceptions) {
        if (e.source == d.source && d.bounds.x0 >= e.area.x0 && d.bounds.x1 <= e.area.x1 &&
            d.bounds.y0 >= e.area.y0 && d.bounds.y1 <= e.area.y1) {
            return e.placement;
        }
    }
    if (!d.anchoring) {
        return Placement::Center;
    }
    return AnchorOf(d.grouped ? d.group : d.bounds);
}

}  // namespace gfx::widescreen
