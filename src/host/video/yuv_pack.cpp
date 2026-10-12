// I420 -> YUY2 packing (see yuv_pack.h).
#include "video/yuv_pack.h"

#include "video/wmv_decoder.h"

#include <cstddef>

namespace vid {

void PackI420ToYuy2(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, std::uint32_t width,
                    std::uint32_t height, std::uint8_t* dst, std::uint32_t dstPitch) noexcept {
    const std::uint32_t cw = width / 2, ch = height / 2;
    if (cw == 0 || ch == 0) {
        return;
    }
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t c = row / 2;
        // Neighbour chroma row: above for even luma rows, below for odd ones (clamped at the edges).
        const std::uint32_t n = (row & 1u) == 0 ? (c > 0 ? c - 1 : 0) : (c + 1 < ch ? c + 1 : ch - 1);
        const std::uint8_t* yr = y + static_cast<std::size_t>(row) * width;
        const std::uint8_t* u0 = u + static_cast<std::size_t>(c) * cw;
        const std::uint8_t* u1 = u + static_cast<std::size_t>(n) * cw;
        const std::uint8_t* v0 = v + static_cast<std::size_t>(c) * cw;
        const std::uint8_t* v1 = v + static_cast<std::size_t>(n) * cw;
        std::uint8_t* o = dst + static_cast<std::size_t>(row) * dstPitch;
        for (std::uint32_t x = 0; x < cw; ++x, o += 4) {
            o[0] = yr[2 * x];
            o[1] = static_cast<std::uint8_t>((3u * u0[x] + u1[x] + 2u) >> 2);
            o[2] = yr[2 * x + 1];
            o[3] = static_cast<std::uint8_t>((3u * v0[x] + v1[x] + 2u) >> 2);
        }
    }
}

void PackI420ToYuy2(const I420Frame& frame, std::uint8_t* dst, std::uint32_t dstPitch) noexcept {
    if (frame.data.size() < static_cast<std::size_t>(frame.width) * frame.height * 3 / 2) {
        return;
    }
    PackI420ToYuy2(frame.Y(), frame.U(), frame.V(), frame.width, frame.height, dst, dstPitch);
}

}  // namespace vid
