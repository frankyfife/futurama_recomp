// I420 -> YUY2 packing for the movie surface (M6). The original XMV library converts its decoded 4:2:0 picture into
// the YUY2 texture the game created with D3DDevice_CreateTexture2(W, H, 1, 1, 0, 0x24 = D3DFMT_YUY2, 3) and draws
// with YuvEnable (avi.md 3.1 FUN_00265B0B, 3.2); the HLE does the same, so the game's movie texture path stays.
//
// YUY2 = per 2 pixels the bytes Y0 U Y1 V (U/V shared by the pair). Chroma rows: the 4:2:0 chroma sample c lies
// between luma rows 2c and 2c+1 (MPEG-1/WMV siting), so luma row 2c takes (3*C[c] + C[c-1] + 2) / 4 and row 2c+1
// (3*C[c] + C[c+1] + 2) / 4 (edge rows repeat). Horizontally the 4:2:0 chroma already has the 4:2:2 resolution.
#pragma once

#include <cstdint>

namespace vid {

struct I420Frame;

// Writes width x height YUY2 pixels into `dst` (rows of dstPitch >= 2 * width bytes). Width and height even.
void PackI420ToYuy2(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, std::uint32_t width,
                    std::uint32_t height, std::uint8_t* dst, std::uint32_t dstPitch) noexcept;
void PackI420ToYuy2(const I420Frame& frame, std::uint8_t* dst, std::uint32_t dstPitch) noexcept;

}  // namespace vid
