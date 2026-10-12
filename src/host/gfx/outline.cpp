// G1 character outlines (gfx/outline.h).
#include "gfx/outline.h"

#include "core/guest.h"
#include "core/guest_call.h"
#include "core/log.h"
#include "gfx/ni_guest_layouts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace gfx::outline {

namespace {

bool Plausible(GuestPtr address) noexcept {
    return address >= 0x10000u && address < 0x80000000u;
}
std::uint32_t R32(GuestPtr address) noexcept {
    return guest::Read<std::uint32_t>(address);
}
float Distance(const float* a, const float* b) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Shell geometry data -> body partner (technique A thickness).
struct Pairing {
    GuestPtr body = 0;                 // body NiTriShapeData (0 = no partner found)
    GuestPtr shellVertices = 0, bodyVertices = 0;
    std::uint16_t shellCount = 0, bodyCount = 0, triangles = 0;
    std::vector<std::uint16_t> map;    // shell vertex -> body vertex
};

// Pairing tolerance: the exporter moves shell vertices by 0.05 model units (g1_outlines.md 2.1); 10 % of the
// shell's bound radius (at least 0.1) accepts that and rejects different meshes with the same triangle count.
float ToleranceOf(GuestPtr shellData) noexcept {
    const float radius = guest::Read<float>(shellData + offsetof(ni::NiGeometryData, bound) + 12);
    return std::isfinite(radius) ? std::max(0.1f, 0.1f * radius) : 0.1f;
}

}  // namespace

// ---- pure helpers ----------------------------------------------------------------------------------------------

bool PairShellToBody(const float* shellPositions, std::uint32_t shellVertexCount, const std::uint16_t* shellTriangles,
                     const float* bodyPositions, std::uint32_t bodyVertexCount, const std::uint16_t* bodyTriangles,
                     std::uint32_t triangleCount, float tolerance, std::uint16_t* bodyVertexOfShellVertex,
                     float* maxDistance) noexcept {
    if (shellPositions == nullptr || bodyPositions == nullptr || shellTriangles == nullptr ||
        bodyTriangles == nullptr || bodyVertexOfShellVertex == nullptr || triangleCount == 0) {
        return false;
    }
    std::fill(bodyVertexOfShellVertex, bodyVertexOfShellVertex + shellVertexCount, std::uint16_t{0xFFFF});
    float worst = 0.0f;
    for (std::uint32_t i = 0; i < triangleCount * 3u; ++i) {
        const std::uint16_t s = shellTriangles[i], b = bodyTriangles[i];
        if (s >= shellVertexCount || b >= bodyVertexCount) {
            return false;
        }
        const float d = Distance(shellPositions + s * 3u, bodyPositions + b * 3u);
        if (!(d <= tolerance)) {
            return false;
        }
        worst = std::max(worst, d);
        if (bodyVertexOfShellVertex[s] == 0xFFFF) {
            bodyVertexOfShellVertex[s] = b;
        }
    }
    if (maxDistance != nullptr) {
        *maxDistance = worst;
    }
    return true;
}

bool ScaleShell(const float* shellPositions, const float* bodyPositions, const std::uint16_t* bodyVertexOfShellVertex,
                std::uint32_t shellVertexCount, float k, float tolerance, float* out) noexcept {
    const float f = k - 1.0f;
    for (std::uint32_t v = 0; v < shellVertexCount; ++v) {
        const float* s = shellPositions + v * 3u;
        const std::uint16_t b = bodyVertexOfShellVertex[v];
        if (b == 0xFFFF) {
            out[v * 3u + 0] = s[0];
            out[v * 3u + 1] = s[1];
            out[v * 3u + 2] = s[2];
            continue;
        }
        const float* p = bodyPositions + b * 3u;
        if (!(Distance(s, p) <= tolerance)) {
            return false;
        }
        for (int c = 0; c < 3; ++c) {
            out[v * 3u + c] = s[c] + f * (s[c] - p[c]);
        }
    }
    return true;
}

void ApplyShellColor(const OutlineSettings& settings, ni::DrawItem* item) noexcept {
    if (item == nullptr || (!settings.hasColor && !settings.hasOpacity)) {
        return;
    }
    ni::RenderStateDesc& s = item->state;
    if (settings.hasColor) {
        // Output = emissive + ambient * (global ambient + lights) + diffuse * lights: with ambient = diffuse = 0
        // and no lights the lit colour is exactly the emissive colour; alpha = the material diffuse alpha.
        s.lightingEnable = true;
        s.diffuseSource = s.ambientSource = s.emissiveSource = FF_MCS_MATERIAL;
        s.specularEnable = false;
        s.material.emissive = {settings.color[0], settings.color[1], settings.color[2], s.material.emissive.w};
        s.material.ambient = {0.0f, 0.0f, 0.0f, s.material.ambient.w};
        s.material.diffuse = {0.0f, 0.0f, 0.0f, s.material.diffuse.w};
        item->lights.count = 0;
        item->lights.ambient = {0.0f, 0.0f, 0.0f, 1.0f};
    }
    if (settings.hasOpacity) {
        s.material.diffuse.w = settings.opacity;
        if (!settings.hasColor) {
            // The original shells have no vertex colours, so the diffuse alpha comes from the material (NV2A
            // COLOR1 fallback); keep it that way for shells that might have them.
            s.diffuseSource = FF_MCS_MATERIAL;
        }
    }
}

bool RecolorRibbon(const OutlineSettings& settings, ni::ExtractArena& arena, ni::GeometryDesc* geometry) noexcept {
    if (geometry == nullptr || geometry->colors.data == nullptr || geometry->vertexCount == 0 ||
        (!settings.hasColor && !settings.hasOpacity)) {
        return false;
    }
    const std::uint32_t n = geometry->vertexCount;
    auto* out = arena.AllocateArray<float>(static_cast<std::size_t>(n) * 4u);
    if (out == nullptr) {
        return false;
    }
    const auto* src = static_cast<const std::uint8_t*>(geometry->colors.data);
    for (std::uint32_t i = 0; i < n; ++i) {
        float c[4];
        std::memcpy(c, src + static_cast<std::size_t>(i) * geometry->colors.stride, sizeof(c));
        if (settings.hasColor) {
            c[0] = settings.color[0];
            c[1] = settings.color[1];
            c[2] = settings.color[2];
        }
        if (settings.hasOpacity) {
            c[3] *= settings.opacity;
        }
        std::memcpy(out + i * 4u, c, sizeof(c));
    }
    geometry->colors = {out, 16};
    return true;
}

// ---- guest side ----------------------------------------------------------------------------------------------

void ApplyGuestSettings(const OutlineSettings& settings) noexcept {
    if (settings.thickness == kDefaultOutlineThickness) {
        return;  // default: the guest keeps its own values
    }
    if (settings.thickness <= 0.0f) {
        *guest::Ptr<std::int32_t>(kOutlineModeGlobal) = 0;  // NiOutline::vf33 -> plain NiNode::vf33
        FUT_LOGI(Gfx, "outlines: [render] outline_thickness = 0 - outlines off (DAT_002D7AB8 = 0, shells skipped)");
        return;
    }
    *guest::Ptr<float>(kRibbonWidthGlobal) = kOriginalRibbonWidth * settings.thickness;
    FUT_LOGI(Gfx, "outlines: [render] outline_thickness = %.2f - ribbon half width DAT_002D7AB0 = %.5f",
             static_cast<double>(settings.thickness), static_cast<double>(kOriginalRibbonWidth * settings.thickness));
}

GuestPtr FindOutlineNode(GuestPtr transform) noexcept {
    if (!Plausible(transform) || transform < kAvWorld) {
        return 0;
    }
    GuestPtr object = transform - kAvWorld;
    for (int depth = 0; depth < 3; ++depth) {
        const GuestPtr parent = R32(object + kAvParent);
        if (!Plausible(parent)) {
            return 0;
        }
        const GuestPtr name = R32(parent + kObjectName);
        if (Plausible(name) && std::memcmp(guest::Ptr<const char>(name), kOutlineNodeName, sizeof(kOutlineNodeName)) == 0) {
            return parent;
        }
        object = parent;
    }
    return 0;
}

bool IsRibbonPolygon(GuestPtr screenPolygon) noexcept {
    if (!Plausible(screenPolygon)) {
        return false;
    }
    const GuestPtr ps = R32(screenPolygon + offsetof(ni::NiScreenPolygon, propertyState));
    const GuestPtr texturing = R32(kRibbonTexturingGlobal), alpha = R32(kRibbonAlphaGlobal);
    if (!Plausible(ps) || !Plausible(texturing) || !Plausible(alpha)) {
        return false;
    }
    const auto* state = guest::Ptr<const ni::NiPropertyState>(ps);
    return state->texturing == texturing && state->alpha == alpha;
}

struct Outlines::Impl {
    OutlineSettings settings;
    Stats stats;
    std::unordered_map<GuestPtr, Pairing> pairings;  // by shell NiTriShapeData*
    std::uint32_t loggedUnpaired = 0;

    static bool IsKindOfNiNode(GuestPtr object) noexcept {
        const GuestPtr vtable = R32(object);
        if (!Plausible(vtable)) {
            return false;
        }
        std::uint32_t rtti = core::CallGuestThiscall(R32(vtable + 4), object);  // vf1 GetRTTI
        for (std::uint32_t depth = 0; Plausible(rtti) && depth < 64; ++depth) {
            if (rtti == kNiNodeRtti) {
                return true;
            }
            rtti = R32(rtti + 8);
        }
        return false;
    }

    // Body mesh of `shellData` below the parent of the QDOutline node (outside the QDOutline subtree).
    bool Pair(GuestPtr outlineNode, GuestPtr shellData, Pairing* out) noexcept {
        ++stats.pairings;
        const GuestPtr root = R32(outlineNode + kAvParent);
        if (!Plausible(root)) {
            return false;
        }
        const auto* shell = guest::Ptr<const ni::NiTriShapeData>(shellData);
        const std::uint32_t shellCount = shell->base.base.vertexCount, triangles = shell->base.triangleCount;
        if (shellCount == 0 || triangles == 0 || !Plausible(shell->base.base.vertices) || !Plausible(shell->triList) ||
            shell->triListLength < triangles * 3u) {
            return false;
        }
        const float tolerance = ToleranceOf(shellData);
        std::vector<std::uint16_t> map(shellCount), best;
        GuestPtr bestBody = 0;
        float bestDistance = 0.0f;
        GuestPtr stack[64];
        std::uint32_t top = 0, visited = 0;
        stack[top++] = root;
        while (top > 0 && visited < 4096) {
            const GuestPtr node = stack[--top];
            const GuestPtr children = R32(node + kNodeChildren);
            const std::uint32_t count = R32(node + kNodeChildCount);
            if (!Plausible(children) || count > 1024) {
                continue;
            }
            for (std::uint32_t i = 0; i < count && visited < 4096; ++i, ++visited) {
                const GuestPtr child = R32(children + i * 4u);
                if (!Plausible(child) || child == outlineNode) {
                    continue;
                }
                if (R32(child) == kNiTriShapeVtable) {
                    const GuestPtr data = R32(child + kGeometryData);
                    if (!Plausible(data) || data == shellData || R32(data) != ni::kNiTriShapeDataVtable) {
                        continue;
                    }
                    const auto* body = guest::Ptr<const ni::NiTriShapeData>(data);
                    if (body->base.triangleCount != triangles || !Plausible(body->base.base.vertices) ||
                        !Plausible(body->triList) || body->triListLength < triangles * 3u) {
                        continue;
                    }
                    float distance = 0.0f;
                    if (PairShellToBody(guest::Ptr<const float>(shell->base.base.vertices), shellCount,
                                        guest::Ptr<const std::uint16_t>(shell->triList),
                                        guest::Ptr<const float>(body->base.base.vertices), body->base.base.vertexCount,
                                        guest::Ptr<const std::uint16_t>(body->triList), triangles, tolerance,
                                        map.data(), &distance) &&
                        (bestBody == 0 || distance < bestDistance)) {
                        bestBody = data;
                        bestDistance = distance;
                        best = map;
                    }
                } else if (top < 64 && IsKindOfNiNode(child)) {
                    stack[top++] = child;
                }
            }
        }
        if (bestBody == 0) {
            return false;
        }
        const auto* body = guest::Ptr<const ni::NiTriShapeData>(bestBody);
        out->body = bestBody;
        out->shellVertices = shell->base.base.vertices;
        out->bodyVertices = body->base.base.vertices;
        out->shellCount = static_cast<std::uint16_t>(shellCount);
        out->bodyCount = body->base.base.vertexCount;
        out->triangles = static_cast<std::uint16_t>(triangles);
        out->map = std::move(best);
        return true;
    }

    // Is the cached pairing still the guest's current data?
    static bool Current(GuestPtr shellData, const Pairing& p) noexcept {
        const auto* shell = guest::Ptr<const ni::NiTriShapeData>(shellData);
        if (shell->base.base.vertexCount != p.shellCount || shell->base.base.vertices != p.shellVertices ||
            shell->base.triangleCount != p.triangles) {
            return false;
        }
        if (p.body == 0) {
            return true;  // remembered "no partner"
        }
        const auto* body = guest::Ptr<const ni::NiTriShapeData>(p.body);
        return R32(p.body) == ni::kNiTriShapeDataVtable && body->base.base.vertexCount == p.bodyCount &&
               body->base.base.vertices == p.bodyVertices && body->base.triangleCount == p.triangles;
    }
};

Outlines::Outlines(const OutlineSettings& settings) : impl_(std::make_unique<Impl>()) {
    impl_->settings = settings;
}
Outlines::~Outlines() = default;

const OutlineSettings& Outlines::Settings() const noexcept {
    return impl_->settings;
}

bool Outlines::IsShell(GuestPtr transform) noexcept {
    return FindOutlineNode(transform) != 0;
}

GuestPtr Outlines::ScaledPositions(GuestPtr transform, GuestPtr data, ni::ExtractArena& arena) noexcept {
    Impl& d = *impl_;
    const GuestPtr outlineNode = FindOutlineNode(transform);
    if (outlineNode == 0 || !Plausible(data) || R32(data) != ni::kNiTriShapeDataVtable) {
        return 0;
    }
    try {
        auto it = d.pairings.find(data);
        if (it == d.pairings.end() || !Impl::Current(data, it->second)) {
            Pairing p;
            const auto* shell = guest::Ptr<const ni::NiTriShapeData>(data);
            if (!d.Pair(outlineNode, data, &p)) {
                p = Pairing{};
                p.shellVertices = shell->base.base.vertices;
                p.shellCount = shell->base.base.vertexCount;
                p.triangles = shell->base.triangleCount;
                if (d.loggedUnpaired++ < 8) {
                    FUT_LOGI(Gfx, "outlines: shell data 0x%08X (%u vertices, %u triangles) has no body mesh - drawn "
                             "with its original thickness", data, p.shellCount, p.triangles);
                }
            }
            it = d.pairings.insert_or_assign(data, std::move(p)).first;
        }
        Pairing& p = it->second;
        if (p.body == 0) {
            ++d.stats.shellsUnpaired;
            return 0;
        }
        auto* out = arena.AllocateArray<float>(static_cast<std::size_t>(p.shellCount) * 3u);
        if (out == nullptr) {
            return 0;
        }
        if (!ScaleShell(guest::Ptr<const float>(p.shellVertices), guest::Ptr<const float>(p.bodyVertices), p.map.data(),
                        p.shellCount, d.settings.thickness, ToleranceOf(data), out)) {
            d.pairings.erase(it);  // the guest data changed: pair again on the next draw
            return 0;
        }
        ++d.stats.shellsScaled;
        return guest::Addr(out);
    } catch (...) {
        return 0;  // std::bad_alloc: keep the original shell
    }
}

void Outlines::NoteShell(bool skipped, bool recolored) noexcept {
    ++impl_->stats.shellDraws;
    impl_->stats.shellsSkipped += skipped ? 1 : 0;
    impl_->stats.shellsRecolored += recolored ? 1 : 0;
}

void Outlines::NoteRibbon(bool recolored) noexcept {
    ++impl_->stats.ribbonDraws;
    impl_->stats.ribbonsRecolored += recolored ? 1 : 0;
}

Stats Outlines::GetStats() const noexcept {
    return impl_->stats;
}

void Outlines::LogStatistics() const noexcept {
    const Stats& s = impl_->stats;
    const OutlineSettings& o = impl_->settings;
    FUT_LOGI(Gfx,
             "outlines: colour %s%02X%02X%02X, opacity %s%.2f, thickness %.2f; shells %llu draws (%llu skipped, %llu "
             "recoloured, %llu scaled, %llu without body mesh; %llu pairings, %zu cached), ribbons %llu draws (%llu "
             "recoloured)",
             o.hasColor ? "" : "original/", static_cast<unsigned>(std::lround(o.color[0] * 255.0f)),
             static_cast<unsigned>(std::lround(o.color[1] * 255.0f)),
             static_cast<unsigned>(std::lround(o.color[2] * 255.0f)), o.hasOpacity ? "" : "original/",
             static_cast<double>(o.opacity), static_cast<double>(o.thickness),
             static_cast<unsigned long long>(s.shellDraws), static_cast<unsigned long long>(s.shellsSkipped),
             static_cast<unsigned long long>(s.shellsRecolored), static_cast<unsigned long long>(s.shellsScaled),
             static_cast<unsigned long long>(s.shellsUnpaired), static_cast<unsigned long long>(s.pairings),
             impl_->pairings.size(), static_cast<unsigned long long>(s.ribbonDraws),
             static_cast<unsigned long long>(s.ribbonsRecolored));
}

}  // namespace gfx::outline
