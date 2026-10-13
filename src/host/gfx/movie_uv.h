// Texel addressing of the movie texture (M6 texel UVs, M6b movie packs). Pure code, unit-tested.
//
// The game's movie quad (Open 0xEE5A1..0xEE647) carries UVs in texels of the texture it created: 0..W x 0..H with
// W x H = the XMV picture (640 x 480). The NV2A addresses linear textures in texels; the renderer samples a
// normalised host texture, so a stage's coordinates are scaled after the stage transform. With a movie pack the host
// texture is larger (e.g. 1280 x 960) while the game keeps its 640 x 480 UVs: a game texel u becomes host texel
// u * hostW / W, normalised by hostW - the picture fills exactly the same screen area as the original frame.
#pragma once

#include <cstdint>

namespace gfx {

struct TexelScale {
    float sx = 1.0f;
    float sy = 1.0f;
};

// Factor from the game's texel coordinates (texture created as guestW x guestH) to normalised coordinates of a host
// texture of hostW x hostH that shows the same picture.
inline TexelScale MovieTexelScale(std::uint32_t guestW, std::uint32_t guestH, std::uint32_t hostW,
                                  std::uint32_t hostH) noexcept {
    if (guestW == 0 || guestH == 0 || hostW == 0 || hostH == 0) {
        return {};
    }
    const float toHostX = static_cast<float>(hostW) / static_cast<float>(guestW);  // game texel -> host texel
    const float toHostY = static_cast<float>(hostH) / static_cast<float>(guestH);
    return {toHostX / static_cast<float>(hostW), toHostY / static_cast<float>(hostH)};
}

}  // namespace gfx
