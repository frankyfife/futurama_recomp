// M3e-Rest: census of the texture pipeline's multi-map/effect inputs (multimap_census.h).
#include "gfx/multimap_census.h"

#include "core/log.h"
#include "core/guest.h"
#include "gfx/texture_pack.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace gfx::ni {
namespace {

bool Plausible(GuestPtr address) noexcept {
    return address >= 0x10000u && address < 0x80000000u;
}
// Plain reads like ni_extract (pointers checked with Plausible); guest::IsReadable only where a first sight is
// recorded (file names, image hashes).
std::uint32_t R32(GuestPtr address) noexcept {
    return Plausible(address) ? guest::Read<std::uint32_t>(address) : 0u;
}
std::uint8_t R8(GuestPtr address) noexcept {
    return Plausible(address) ? guest::Read<std::uint8_t>(address) : 0u;
}
std::string GuestCString(GuestPtr address) {
    std::string s;
    for (std::uint32_t i = 0; i < 96 && Plausible(address + i) && guest::IsReadable(address + i, 1); ++i) {
        const char c = static_cast<char>(guest::Read<std::uint8_t>(address + i));
        if (c == '\0') {
            break;
        }
        s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    return s;
}

// NiTextureEffect fields (FUN_00163F10, FUN_00164200).
constexpr std::uint32_t kEffectOn = 0xC0;
constexpr std::uint32_t kEffectTexture = 0x15C;
constexpr std::uint32_t kEffectFilter = 0x160;
constexpr std::uint32_t kEffectClamp = 0x164;
constexpr std::uint32_t kEffectCoordMode = 0x16C;
constexpr std::uint32_t kSourceTextureFile = 0x30;
constexpr std::uint32_t kSourceTexturePixels = 0x34;
constexpr std::uint32_t kTextureDataPixelData = 0x5C;  // NiPCTextureData::pixelData (gfx/nipc_renderer.h)

std::uint32_t EffectModes(GuestPtr effect) noexcept {
    return (R32(effect + kEffectClamp) & 0xFu) | ((R32(effect + kEffectFilter) & 0xFu) << 4) |
           ((R32(effect + kEffectCoordMode) & 0xFu) << 8) | ((R32(effect + kNiTextureEffectType) & 0xFu) << 12) |
           (R8(effect + kEffectOn) != 0 ? 0x10000u : 0u);
}

}  // namespace

const char* CensusRoleName(CensusRole role) noexcept {
    switch (role) {
    case CensusRole::Dark:
        return "dark";
    case CensusRole::Detail:
        return "detail";
    case CensusRole::Gloss:
        return "gloss";
    case CensusRole::Glow:
        return "glow";
    case CensusRole::Bump:
        return "bump";
    case CensusRole::Decal:
        return "decal";
    case CensusRole::ProjLight:
        return "projected light";
    case CensusRole::ProjLightClip:
        return "projected light (clipped)";
    case CensusRole::ProjShadow:
        return "projected shadow";
    case CensusRole::ProjShadowClip:
        return "projected shadow (clipped)";
    case CensusRole::EnvMap:
        return "environment map";
    case CensusRole::FogMap:
        return "fog map";
    default:
        return "?";
    }
}

void MultiMapCensus::Add(CensusRole role, GuestPtr texture, std::uint32_t modes, std::uint32_t frame) noexcept {
    ++roleDraws_[static_cast<int>(role)];
    const std::uint64_t key = (static_cast<std::uint64_t>(texture) << 32) ^
                              (static_cast<std::uint64_t>(role) << 24) ^ (modes & 0xFFFFFFu);
    Entry& e = entries_[key];
    if (e.draws++ != 0) {
        // The image may be uploaded only by the stage of the first draw (after RecordDraw): look it up again.
        if (!e.hashed && e.draws <= 4 && texpack::UploadedHash(texture, &e.hash)) {
            e.hashed = true;
        }
        return;
    }
    e.role = role;
    e.texture = texture;
    e.modes = modes;
    e.firstFrame = frame;
    if (Plausible(texture) && R32(texture) == kNiRenderedTextureVtable) {
        e.file = "<rendered texture>";
        e.width = R32(texture + kNiRenderedTextureWidth);
        e.height = R32(texture + kNiRenderedTextureHeight);
        return;
    }
    if (!Plausible(texture) || R32(texture) != kNiSourceTextureVtable) {
        return;
    }
    e.file = GuestCString(R32(texture + kSourceTextureFile));
    texpack::ImageInfo info;
    std::uint64_t hash = 0;
    // The image: NiSourceTexture +0x34, or (pixels released after the upload) the renderer data's NiPixelData +0x5C.
    GuestPtr pixels = R32(texture + kSourceTexturePixels);
    const GuestPtr rendererData = R32(texture + kNiTextureRendererData);
    if (!Plausible(pixels) && Plausible(rendererData)) {
        pixels = R32(rendererData + kTextureDataPixelData);
    }
    if (texpack::HashGuestPixelData(pixels, &hash, &info)) {
        e.hashed = true;
        e.hash = hash;
        e.width = info.width;
        e.height = info.height;
        e.format = info.format;
        e.bpp = info.bitsPerPixel;
    } else {
        e.hashed = texpack::UploadedHash(texture, &e.hash);
        const GuestPtr data = R32(texture + kNiTextureRendererData);
        if (Plausible(data)) {
            e.width = R32(data + kTextureDataWidth);
            e.height = R32(data + kTextureDataHeight);
        }
    }
}

void MultiMapCensus::RecordDraw(GuestPtr texturing, GuestPtr effectState, std::uint32_t alphaFlags,
                                std::uint32_t frame) noexcept {
    std::uint32_t roles = 0;
    std::uint32_t applyMode = 0;
    const NiTexturingProperty* t = Plausible(texturing) ? guest::Ptr<const NiTexturingProperty>(texturing) : nullptr;
    const NiDynamicEffectState* es =
        Plausible(effectState) ? guest::Ptr<const NiDynamicEffectState>(effectState) : nullptr;
    const bool maps = t != nullptr && t->hasNonBaseMaps != 0 && Plausible(t->maps);
    const bool effects = es != nullptr && (Plausible(es->projectedLights) || Plausible(es->projectedShadows) ||
                                           Plausible(es->environmentMap) || Plausible(es->fogMap));
    if (!maps && !effects) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (maps) {
        applyMode = t->applyMode & 7u;
        const std::uint32_t size = std::min<std::uint32_t>(t->mapsSize, 16);
        for (std::uint32_t i = 1; i < size; ++i) {
            const GuestPtr map = R32(t->maps + 4 * i);
            if (!Plausible(map)) {
                continue;
            }
            const NiTexturingMap* m = guest::Ptr<const NiTexturingMap>(map);
            const CensusRole role = i >= kMapDecal ? CensusRole::Decal : static_cast<CensusRole>(i - 1);
            const std::uint32_t modes = (m->clamp & 0xFu) | ((m->filter & 0xFu) << 4) | ((m->uvSet & 0xFu) << 8) |
                                        (applyMode << 12);
            Add(role, m->texture, modes, frame);
            roles |= 1u << static_cast<int>(role);
        }
    }
    if (effects) {
        const GuestPtr lists[2] = {es->projectedLights, es->projectedShadows};
        for (int l = 0; l < 2; ++l) {
            int guard = 0;
            for (GuestPtr node = lists[l]; Plausible(node) && guard < 64; ++guard) {
                const GuestPtr item = R32(node + 4);
                node = R32(node);
                if (!Plausible(item)) {
                    continue;
                }
                const bool clipped = R8(item + kNiTextureEffectClipped) != 0;
                const CensusRole role = l == 0 ? (clipped ? CensusRole::ProjLightClip : CensusRole::ProjLight)
                                               : (clipped ? CensusRole::ProjShadowClip : CensusRole::ProjShadow);
                Add(role, R32(item + kEffectTexture), EffectModes(item), frame);
                roles |= 1u << static_cast<int>(role);
            }
        }
        if (Plausible(es->environmentMap)) {
            Add(CensusRole::EnvMap, R32(es->environmentMap + kEffectTexture), EffectModes(es->environmentMap), frame);
            roles |= 1u << static_cast<int>(CensusRole::EnvMap);
        }
        if (Plausible(es->fogMap)) {
            Add(CensusRole::FogMap, R32(es->fogMap + kEffectTexture), EffectModes(es->fogMap), frame);
            roles |= 1u << static_cast<int>(CensusRole::FogMap);
        }
    }
    ++multiDraws_;
    const std::uint64_t combo = roles | (static_cast<std::uint64_t>(applyMode) << 16) |
                                ((alphaFlags & 1u) != 0 ? 1ull << 20 : 0) |
                                (((alphaFlags >> 9) & 1u) != 0 ? 1ull << 21 : 0);
    ++combos_[combo];
}

void MultiMapCensus::Log() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string totals;
    for (int r = 0; r < static_cast<int>(CensusRole::Count); ++r) {
        if (roleDraws_[r] != 0) {
            totals += " ";
            totals += CensusRoleName(static_cast<CensusRole>(r));
            totals += " " + std::to_string(roleDraws_[r]) + ",";
        }
    }
    if (!totals.empty()) {
        totals.pop_back();
    }
    FUT_LOGI(Gfx, "multi-map census: %llu draws with other maps or texture effects;%s",
             static_cast<unsigned long long>(multiDraws_), totals.empty() ? " none" : totals.c_str());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> combos(combos_.begin(), combos_.end());
    std::sort(combos.begin(), combos.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < combos.size() && i < 24; ++i) {
        std::string roles;
        for (int r = 0; r < static_cast<int>(CensusRole::Count); ++r) {
            if ((combos[i].first >> r) & 1u) {
                roles += roles.empty() ? "" : " + ";
                roles += CensusRoleName(static_cast<CensusRole>(r));
            }
        }
        FUT_LOGI(Gfx, "multi-map census: combination %s, apply mode %u%s%s: %llu draws", roles.c_str(),
                 static_cast<unsigned>((combos[i].first >> 16) & 7u), (combos[i].first >> 20) & 1u ? ", blended" : "",
                 (combos[i].first >> 21) & 1u ? ", alpha-tested" : "", static_cast<unsigned long long>(combos[i].second));
    }
    std::vector<const Entry*> list;
    list.reserve(entries_.size());
    for (const auto& kv : entries_) {
        list.push_back(&kv.second);
    }
    std::sort(list.begin(), list.end(), [](const Entry* a, const Entry* b) {
        return a->role != b->role ? a->role < b->role : a->draws > b->draws;
    });
    std::size_t shown = 0;
    for (const Entry* e : list) {
        if (shown++ == 96) {
            FUT_LOGI(Gfx, "multi-map census: ... %zu more texture entries", list.size() - 96);
            break;
        }
        const bool effect = e->role >= CensusRole::ProjLight;
        char modes[96];
        if (effect) {
            std::snprintf(modes, sizeof(modes), "type %u, coord mode %u, clamp %u, filter %u%s", (e->modes >> 12) & 0xFu,
                          (e->modes >> 8) & 0xFu, e->modes & 0xFu, (e->modes >> 4) & 0xFu,
                          (e->modes & 0x10000u) != 0 ? "" : ", off");
        } else {
            std::snprintf(modes, sizeof(modes), "clamp %u, filter %u, uv set %u, apply mode %u", e->modes & 0xFu,
                          (e->modes >> 4) & 0xFu, (e->modes >> 8) & 0xFu, (e->modes >> 12) & 0xFu);
        }
        FUT_LOGI(Gfx, "multi-map census: %s texture 0x%08X %s %ux%u %s file '%s' (%s): %llu draws from frame %u",
                 CensusRoleName(e->role), e->texture, e->hashed ? texpack::HashName(e->hash).c_str() : "-",
                 e->width, e->height, e->bpp != 0 ? texpack::SourceFormatName(e->format, e->bpp) : "?", e->file.c_str(),
                 modes, static_cast<unsigned long long>(e->draws), e->firstFrame);
    }
}

}  // namespace gfx::ni
