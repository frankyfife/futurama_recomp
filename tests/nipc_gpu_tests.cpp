// Unit tests of the GPU path of NiPCRenderer (gfx/nipc_gpu.h, M3 integration): names, the half-pixel
// shift of transformed geometry (design 8.3 B19) and ClearBuffer (slot 17 semantics: NiRect with y up,
// whole target, empty rectangle, texture targets) on a headless WARP device with the debug layer.
#include "doctest.h"

#include "gfx/nipc_gpu.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace gfx;

// 0xAARRGGBB of a BGRA8 image pixel.
std::uint32_t Pixel(const rhi::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = image.pixels.data() + static_cast<std::size_t>(y) * image.rowPitch + x * 4;
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::size_t DebugErrors(rhi::Device& device) {
    std::vector<rhi::DebugMessage> messages;
    device.DrainDebugMessages(messages);
    std::size_t errors = 0;
    for (const rhi::DebugMessage& m : messages) {
        MESSAGE("debug message " << static_cast<int>(m.severity) << " #" << m.id << ": " << m.text);
        if (m.severity == rhi::DebugSeverity::Error || m.severity == rhi::DebugSeverity::Corruption) {
            ++errors;
        }
    }
    return errors;
}

// Screen position (pixels, y down) of a clip-space point in a viewport of w x h.
void ToScreen(const Float4& clip, float w, float h, float* x, float* y) {
    *x = (clip.x / clip.w + 1.0f) * 0.5f * w;
    *y = (1.0f - clip.y / clip.w) * 0.5f * h;
}

Float4 Mul(const Float4& v, const Matrix4& m) {
    Float4 r;
    r.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + v.w * m.m[3][0];
    r.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + v.w * m.m[3][1];
    r.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + v.w * m.m[3][2];
    r.w = v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + v.w * m.m[3][3];
    return r;
}

}  // namespace

TEST_CASE("nipc_gpu: draw outcome and kind names") {
    CHECK(std::string(DrawOutcomeName(DrawOutcome::Drawn)) == "Drawn");
    CHECK(std::string(DrawOutcomeName(DrawOutcome::NotInScene)) == "NotInScene");
    CHECK(std::string(DrawOutcomeName(DrawOutcome::TextureTarget)) == "TextureTarget");
    CHECK(std::string(DrawOutcomeName(DrawOutcome::Count)) == "?");
    CHECK(std::string(DrawKindName(ni::DrawKind::Shape)) == "Shape");
    CHECK(std::string(DrawKindName(ni::DrawKind::ScreenTexture)) == "ScreenTexture");
    CHECK(kDrawOutcomeCount == 8);
}

TEST_CASE("nipc_gpu: the half-pixel shift moves transformed vertices +0.5 pixel right and down") {
    // Perspective projection as SetCameraData 0x1469E0 builds it (frustum l/r/t/b at distance 1, n, f).
    const float l = -0.6f, r = 0.4f, t = 0.45f, b = -0.3f, n = 0.5f, f = 200.0f;
    Matrix4 p;
    p.m[0][0] = 2.0f / (r - l);
    p.m[2][0] = -(r + l) / (r - l);
    p.m[1][1] = 2.0f / (t - b);
    p.m[2][1] = -(t + b) / (t - b);
    p.m[2][2] = f / (f - n);
    p.m[3][2] = -n * f / (f - n);
    p.m[2][3] = 1.0f;
    Matrix4 shifted = p;
    HalfPixelShift(&shifted, 640.0f, 480.0f);
    for (const Float4 v : {Float4{0.1f, -0.2f, 3.0f, 1.0f}, Float4{-5.0f, 4.0f, 50.0f, 1.0f},
                           Float4{0.0f, 0.0f, 0.5f, 1.0f}}) {
        const Float4 a = Mul(v, p);
        const Float4 c = Mul(v, shifted);
        float ax, ay, cx, cy;
        ToScreen(a, 640.0f, 480.0f, &ax, &ay);
        ToScreen(c, 640.0f, 480.0f, &cx, &cy);
        CHECK(cx - ax == doctest::Approx(0.5f).epsilon(1e-3));
        CHECK(cy - ay == doctest::Approx(0.5f).epsilon(1e-3));
        CHECK(c.z == a.z);  // depth and w unchanged
        CHECK(c.w == a.w);
    }
    // Degenerate viewports leave the matrix alone.
    Matrix4 same = p;
    HalfPixelShift(&same, 0.0f, 480.0f);
    CHECK(std::memcmp(&same, &p, sizeof(p)) == 0);
}

TEST_CASE("nipc_gpu: ClearBuffer clears the whole target, NiRect rectangles (y up) and skips texture targets") {
    GpuRenderer::CreateParams params;
    params.window = nullptr;  // headless back buffer
    params.width = 64;
    params.height = 48;
    params.debugLayer = true;
    params.softwareAdapter = true;  // WARP
    std::string error;
    std::unique_ptr<GpuRenderer> gpu = GpuRenderer::Create(params, &error);
    if (gpu == nullptr) {
        MESSAGE("GpuRenderer::Create failed (no WARP?): " << error);
        return;
    }
    rhi::Device& device = gpu->Device();
    CHECK(device.Info().headless);
    CHECK(gpu->DefaultTexture());
    CHECK(gpu->BlackTexture());
    rhi::Image image;

    // mode 1 = color (plus depth 4 and stencil 2), NULL rect = whole target.
    gpu->ClearBuffer(nullptr, 1 | 2 | 4, 0xFF102030u, 1.0f, 0);
    REQUIRE(device.CaptureBackBuffer(image));
    CHECK(Pixel(image, 0, 0) == 0xFF102030u);
    CHECK(Pixel(image, 63, 47) == 0xFF102030u);

    // NiRect {left 0.5, right 1, top 1, bottom 0.5} with y up = the top-right quadrant (0x1464C2..0x14652B).
    const ni::NiRectF topRight{0.5f, 1.0f, 1.0f, 0.5f};
    gpu->ClearBuffer(&topRight, 1, 0xFF00FF00u, 1.0f, 0);
    REQUIRE(device.CaptureBackBuffer(image));
    CHECK(Pixel(image, 32, 0) == 0xFF00FF00u);
    CHECK(Pixel(image, 63, 23) == 0xFF00FF00u);
    CHECK(Pixel(image, 31, 0) == 0xFF102030u);
    CHECK(Pixel(image, 63, 24) == 0xFF102030u);
    CHECK(Pixel(image, 0, 47) == 0xFF102030u);

    // An empty rectangle clears nothing (D3DDevice_Clear with an empty D3DRECT).
    const ni::NiRectF empty{0.25f, 0.25f, 1.0f, 0.0f};
    gpu->ClearBuffer(&empty, 1, 0xFFFF0000u, 1.0f, 0);
    REQUIRE(device.CaptureBackBuffer(image));
    CHECK(Pixel(image, 16, 10) == 0xFF102030u);

    // Texture targets (render to texture, P2): clears are counted no-ops until SetRenderTarget(NULL).
    gpu->SetTextureTarget(32, 32);
    gpu->ClearBuffer(nullptr, 1, 0xFF0000FFu, 1.0f, 0);
    gpu->SetBackBufferTarget();
    REQUIRE(device.CaptureBackBuffer(image));
    CHECK(Pixel(image, 0, 47) == 0xFF102030u);
    gpu->ClearBuffer(nullptr, 1, 0xFF0000FFu, 1.0f, 0);
    REQUIRE(device.CaptureBackBuffer(image));
    CHECK(Pixel(image, 0, 47) == 0xFF0000FFu);

    CHECK(DebugErrors(device) == 0);
}
