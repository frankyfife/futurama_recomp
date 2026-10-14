// M3e-Rest: census of the texture pipeline's multi-map/effect inputs (docs/m8b_report.md, GFX_CONTRACT.md 16).
//
// What the game hands to the default texture pipeline beyond the base map: the other NiTexturingProperty maps
// (dark, detail, gloss, glow, bump, decals) and the NiTextureEffects of the NiDynamicEffectState (projected lights and
// shadows, environment and fog maps). ni_extract records every 3D draw that carries any of them (ExtractContext::
// census, nullptr = off; the renderer always passes its census); the exit report (GFX/info) lists the draws per role,
// the combinations per draw and the textures with their content hash (the G2 texture-dump name, gfx/texture_pack.h),
// size, NiSourceTexture file name and map/effect modes - the evidence for which paths of REN's pipeline the game uses.
// Cost: one hash-map lookup per role and draw for draws that take the multi-map path; nothing for the others.
#pragma once

#include "gfx/ni_guest_layouts.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gfx::ni {

enum class CensusRole : std::uint8_t {
    Dark,             // maps[1]
    Detail,           // maps[2]
    Gloss,            // maps[3]
    Glow,             // maps[4]
    Bump,             // maps[5]
    Decal,            // maps[6..]
    ProjLight,        // NiDynamicEffectState +0x10, plane off
    ProjLightClip,    // +0x10, plane on (+0x170)
    ProjShadow,       // +0x14, plane off
    ProjShadowClip,   // +0x14, plane on
    EnvMap,           // +0x18
    FogMap,           // +0x1C
    Count,
};
const char* CensusRoleName(CensusRole role) noexcept;

class MultiMapCensus {
public:
    // One 3D draw (not slot 54/55): `texturing` = NiTexturingProperty, `effectState` may be 0, `alphaFlags` = the
    // NiAlphaProperty flags of the draw (blend bit 0, test bit 9), `frame` = displayed-frame counter.
    void RecordDraw(GuestPtr texturing, GuestPtr effectState, std::uint32_t alphaFlags, std::uint32_t frame) noexcept;
    // Exit report (GFX/info): totals per role, the most frequent combinations and textures.
    void Log() const noexcept;
    std::uint64_t Draws(CensusRole role) const noexcept { return roleDraws_[static_cast<int>(role)]; }
    std::uint64_t MultiDraws() const noexcept { return multiDraws_; }

private:
    struct Entry {
        CensusRole role = CensusRole::Dark;
        GuestPtr texture = 0;
        std::uint32_t modes = 0;  // maps: clamp | filter << 4 | uvSet << 8 | applyMode << 12;
                                  // effects: clamp | filter << 4 | coordMode << 8 | type << 12 | on << 16
        std::uint64_t draws = 0;
        std::uint64_t hash = 0;
        bool hashed = false;
        std::uint32_t width = 0, height = 0, format = 0, bpp = 0;
        std::uint32_t firstFrame = 0;
        std::string file;  // NiSourceTexture +0x30
    };
    void Add(CensusRole role, GuestPtr texture, std::uint32_t modes, std::uint32_t frame) noexcept;

    mutable std::mutex mutex_;  // the renderer thread records, the exit reporter reads
    std::uint64_t roleDraws_[static_cast<int>(CensusRole::Count)] = {};
    std::uint64_t multiDraws_ = 0;
    std::unordered_map<std::uint64_t, Entry> entries_;           // key: texture << 32 ^ role << 24 ^ modes
    std::unordered_map<std::uint64_t, std::uint64_t> combos_;    // key: role bits | apply mode | blend/test
};

}  // namespace gfx::ni
