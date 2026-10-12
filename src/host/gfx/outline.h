// G1: the character outlines of the original and the [render] outline_color / outline_opacity /
// outline_thickness options (analysis/loader/g1_outlines.md, GFX_CONTRACT.md "Konturen").
//
// The game has two outline techniques; the renderer only changes them when an option differs from its default
// (gfx::OutlineSettings::Active()). At the defaults nothing here runs: no DrawItem and no guest memory changes.
//
//   A  Inverted hull "QDOutline" (every character in the levels): a NiLODNode named "QDOutline" holds one
//      shell per body mesh - the same triangles in the same order, welded vertices moved by 0.05 model units
//      (or 0) along the normal, NiStencilProperty draw mode CW (front faces culled), material emissive 0 and
//      alpha 0.75, no lights (ambient 0) -> black at 75 %. Drawn by slot 48 like any skinned NiTriShape.
//      Recognised here through the parent chain of the geometry (slot argument transform - 0x70, NiAVObject
//      parent +0x14, name +0x08). Colour/opacity: the lit result is forced to the material (emissive = colour,
//      no lights). Thickness k: the shell is paired with its body mesh (same triangle count, corners in
//      triangle order within a tolerance) and the bind-pose positions p + (k - 1) * (p - body) replace the
//      shell's source positions before FUN_0014F710 skins them; k = 0 skips the shell draws.
//   B  NiOutline (HUD head, a few menu/cockpit NIFs): CPU silhouette ribbons as 404-vertex NiScreenPolygon
//      strips with the property set of FUN_0013CE60 (texturing 0x3044BC, alpha 0x3044E8). Colour/opacity:
//      arena copy of the vertex colours. Thickness: DAT_002D7AB0 (0.003) * k, k = 0 -> DAT_002D7AB8 = 0 (off).
#pragma once

#include "gfx/gfx_config.h"
#include "gfx/ni_extract.h"

#include <cstdint>
#include <memory>

namespace gfx::outline {

using GuestPtr = ni::GuestPtr;

// Guest addresses (retail EU XBE).
inline constexpr GuestPtr kRibbonWidthGlobal = 0x002D7AB0;       // float 0.003 (read at 0x15475B / 0x15477D)
inline constexpr GuestPtr kOutlineModeGlobal = 0x002D7AB8;       // int 2 = NiOutline on, 0 = off
inline constexpr GuestPtr kOutlinePositionsGlobal = 0x0030EB40;  // world-position buffer while NiOutline::vf33 draws
inline constexpr GuestPtr kRibbonTexturingGlobal = 0x003044BC;   // NiTexturingProperty* of the ribbons
inline constexpr GuestPtr kRibbonAlphaGlobal = 0x003044E8;       // NiAlphaProperty* of the ribbons
inline constexpr float kOriginalRibbonWidth = 0.003f;
inline constexpr GuestPtr kNiTriShapeVtable = 0x002A7CE0;
inline constexpr GuestPtr kNiNodeRtti = 0x002F9190;
inline constexpr std::uint32_t kAvParent = 0x14;      // NiAVObject parent NiNode*
inline constexpr std::uint32_t kObjectName = 0x08;    // NiObjectNET name (char*, FUN_000E2EE0)
inline constexpr std::uint32_t kAvWorld = 0x70;       // NiAVObject world transform (slot 48-51 argument)
inline constexpr std::uint32_t kGeometryData = 0xC8;  // NiGeometry data
inline constexpr std::uint32_t kNodeChildren = 0xC4;  // NiNode NiTArray<NiAVObject*> base / size (+0xCC)
inline constexpr std::uint32_t kNodeChildCount = 0xCC;
inline constexpr char kOutlineNodeName[] = "QDOutline";  // compared over 10 bytes like FUN_0003F8D0

// ---- pure helpers (unit tests) -------------------------------------------------------------------------------

// Pairs the vertices of a shell with the body mesh that has the same triangles in the same order: for every
// shell vertex the body vertex of the first triangle corner that uses it (0xFFFF = unused). False if a corner pair
// is farther apart than `tolerance` or an index is out of range. `maxDistance` (optional) = largest corner
// distance.
bool PairShellToBody(const float* shellPositions, std::uint32_t shellVertexCount, const std::uint16_t* shellTriangles,
                     const float* bodyPositions, std::uint32_t bodyVertexCount, const std::uint16_t* bodyTriangles,
                     std::uint32_t triangleCount, float tolerance, std::uint16_t* bodyVertexOfShellVertex,
                     float* maxDistance) noexcept;

// out = shell + (k - 1) * (shell - body[map]) per vertex (unpaired vertices keep their position). False (out
// unspecified) if a pair lies farther apart than `tolerance` (the guest data changed).
bool ScaleShell(const float* shellPositions, const float* bodyPositions, const std::uint16_t* bodyVertexOfShellVertex,
                std::uint32_t shellVertexCount, float k, float tolerance, float* out) noexcept;

// Technique A colour/opacity on an extracted shell draw (no-op without hasColor/hasOpacity).
void ApplyShellColor(const OutlineSettings& settings, ni::DrawItem* item) noexcept;

// Technique B colour/opacity: replaces the colour stream of a ribbon draw by an arena copy (rgb = colour, alpha *
// opacity). False if there is nothing to change or the arena is full.
bool RecolorRibbon(const OutlineSettings& settings, ni::ExtractArena& arena, ni::GeometryDesc* geometry) noexcept;

// ---- guest side (renderer thread = guest main thread) ----------------------------------------------------------

// Writes DAT_002D7AB0 / DAT_002D7AB8 for the thickness option (nothing at the default). Called once by
// NiXBoxRenderer::Create (nipc_renderer.cpp).
void ApplyGuestSettings(const OutlineSettings& settings) noexcept;

// The "QDOutline" node above the geometry whose world transform is `transform` (up to 3 parents), 0 if none.
GuestPtr FindOutlineNode(GuestPtr transform) noexcept;

// True if the NiScreenPolygon property state is the ribbon property set of FUN_0013CE60.
bool IsRibbonPolygon(GuestPtr screenPolygon) noexcept;

struct Stats {
    std::uint64_t shellDraws = 0, shellsSkipped = 0, shellsRecolored = 0, shellsScaled = 0, shellsUnpaired = 0,
                  pairings = 0, ribbonDraws = 0, ribbonsRecolored = 0;
};

// Per-renderer state of technique A (pairing cache) and the counters. Create only when settings.Active().
class Outlines {
public:
    explicit Outlines(const OutlineSettings& settings);
    ~Outlines();
    Outlines(const Outlines&) = delete;
    Outlines& operator=(const Outlines&) = delete;

    const OutlineSettings& Settings() const noexcept;

    // Slot 48-51 geometry: true if it is a QDOutline shell (by the guest transform of the slot call).
    bool IsShell(GuestPtr transform) noexcept;
    // Thickness k != 1: guest address of the scaled bind-pose positions in `arena` (vertexCount float3), 0 if the
    // shell has no body partner (the draw stays unchanged).
    GuestPtr ScaledPositions(GuestPtr transform, GuestPtr data, ni::ExtractArena& arena) noexcept;
    void NoteShell(bool skipped, bool recolored) noexcept;
    void NoteRibbon(bool recolored) noexcept;
    Stats GetStats() const noexcept;
    void LogStatistics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gfx::outline
