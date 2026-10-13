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

TEST_CASE("nipc_gpu: ClearBuffer clears the whole target, NiRect rectangles (y up) and texture targets") {
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

    // M8: texture targets (render to texture) get their own color target; the back buffer stays untouched.
    constexpr ni::GuestPtr kData = 0x00123450;  // only a key here
    gpu->SetTextureTarget(kData, 32, 32);
    gpu->ClearBuffer(nullptr, 1, 0xFF0000FFu, 1.0f, 0);
    gpu->ClearBuffer(&topRight, 1, 0xFFFF0000u, 1.0f, 0);
    gpu->SetBackBufferTarget();
    REQUIRE(device.CaptureBackBuffer(image));
    CHECK(Pixel(image, 0, 47) == 0xFF102030u);
    const rhi::TextureHandle rendered = gpu->RenderedTexture(kData, 32, 32);
    REQUIRE(rendered);
    CHECK(rendered != gpu->BlackTexture());
    rhi::Image texture;
    REQUIRE(device.ReadTexture(rendered, 0, texture));
    CHECK(texture.width == 32);
    CHECK(Pixel(texture, 0, 31) == 0xFF0000FFu);
    CHECK(Pixel(texture, 16, 0) == 0xFFFF0000u);  // NiRect in the texture's own pixels
    CHECK(Pixel(texture, 15, 0) == 0xFF0000FFu);
    gpu->ClearBuffer(nullptr, 1, 0xFF0000FFu, 1.0f, 0);
    REQUIRE(device.CaptureBackBuffer(image));
    CHECK(Pixel(image, 0, 47) == 0xFF0000FFu);
    gpu->ForgetRenderedTexture(kData);

    CHECK(DebugErrors(device) == 0);
}

TEST_CASE("nipc_gpu: render to texture at the render resolution with MSAA is resolved before sampling") {
    GpuRenderer::CreateParams params;
    params.window = nullptr;
    params.width = 64;
    params.height = 48;
    params.debugLayer = true;
    params.softwareAdapter = true;
    params.quality.resolutionWidth = 128;  // internal factor 2
    params.quality.resolutionHeight = 96;
    params.quality.msaa = 4;
    std::string error;
    std::unique_ptr<GpuRenderer> gpu = GpuRenderer::Create(params, &error);
    if (gpu == nullptr) {
        MESSAGE("GpuRenderer::Create failed (no WARP?): " << error);
        return;
    }
    rhi::Device& device = gpu->Device();
    constexpr ni::GuestPtr kData = 0x00123460;
    gpu->SetTextureTarget(kData, 16, 16);
    gpu->ClearBuffer(nullptr, 1 | 4, 0xFF00FF00u, 1.0f, 0);
    const ni::NiRectF left{0.0f, 0.5f, 1.0f, 0.0f};
    gpu->ClearBuffer(&left, 1, 0xFFFFFFFFu, 1.0f, 0);
    gpu->SetBackBufferTarget();
    const rhi::TextureHandle rendered = gpu->RenderedTexture(kData, 16, 16);
    rhi::Image texture;
    REQUIRE(device.ReadTexture(rendered, 0, texture));
    CHECK(texture.width == 32);  // 16 guest pixels x 2
    CHECK(texture.height == 32);
    CHECK(Pixel(texture, 0, 0) == 0xFFFFFFFFu);
    CHECK(Pixel(texture, 15, 31) == 0xFFFFFFFFu);
    CHECK(Pixel(texture, 16, 0) == 0xFF00FF00u);
    CHECK(Pixel(texture, 31, 31) == 0xFF00FF00u);
    CHECK(DebugErrors(device) == 0);
}

TEST_CASE("nipc_gpu: render-to-texture size follows the render resolution") {
    CHECK(RenderTargetDimension(128, 480, 480) == 128);
    CHECK(RenderTargetDimension(128, 480, 960) == 256);
    CHECK(RenderTargetDimension(128, 480, 2160) == 576);  // factor 4.5
    CHECK(RenderTargetDimension(256, 480, 4320) == 2304);
    CHECK(RenderTargetDimension(1024, 480, 4320) == kMaxRenderTargetDimension);
    CHECK(RenderTargetDimension(128, 480, 240) == 128);   // never below the guest size
    CHECK(RenderTargetDimension(0, 480, 960) == 0);
}

TEST_CASE("nipc_gpu: G3 bars - black only for centred frames, a depth clear or stretched content leaves the sides") {
    // Fog fix (user report 2026-10-13, Level1-2): in a level the game clears only depth/stencil and the Hor+ scene
    // covers the width; pixels the scene leaves empty (beyond the far plane) must keep the previous picture at the
    // sides as in the centre instead of showing the black bar clear.
    auto make = [](std::unique_ptr<GpuRenderer>& gpu) {
        GpuRenderer::CreateParams params;
        params.window = nullptr;
        params.width = 640;  // the guest screen (widescreen::CoversScreen tests 640 x 480)
        params.height = 480;
        params.debugLayer = true;
        params.softwareAdapter = true;
        params.aspectWidth = 16;
        params.aspectHeight = 9;
        std::string error;
        gpu = GpuRenderer::Create(params, &error);
        if (gpu == nullptr) {
            MESSAGE("GpuRenderer::Create failed (no WARP?): " << error);
        }
        return gpu != nullptr;
    };
    const ni::NiRectF full{0.0f, 1.0f, 1.0f, 0.0f};
    const ni::NiRectF middle{0.25f, 0.75f, 0.75f, 0.25f};  // inside the centred 4:3 area, not covering
    rhi::Image image;

    std::unique_ptr<GpuRenderer> gpu;
    if (!make(gpu)) {
        return;
    }
    const std::uint32_t w = gpu->Sizes().internalWidth;
    const std::uint32_t h = gpu->Sizes().internalHeight;
    REQUIRE(w * 9 == h * 16);
    gpu->ClearBuffer(nullptr, 1 | 2 | 4, 0xFF00FF00u, 1.0f, 0);  // whole target (no placement): the "old picture"
    gpu->ClearBuffer(&full, 2 | 4, 0xFF000000u, 1.0f, 0);         // the level's depth/stencil clear
    REQUIRE(gpu->Device().CaptureBackBuffer(image));
    CHECK(Pixel(image, 0, h / 2) == 0xFF00FF00u);  // side kept
    CHECK(Pixel(image, w - 1, h / 2) == 0xFF00FF00u);
    gpu->ClearBuffer(&middle, 1, 0xFF0000FFu, 1.0f, 0);  // centred content: now the bars are black
    REQUIRE(gpu->Device().CaptureBackBuffer(image));
    CHECK(Pixel(image, 0, h / 2) == 0xFF000000u);
    CHECK(Pixel(image, w - 1, h / 2) == 0xFF000000u);
    CHECK(Pixel(image, w / 2, h / 2) == 0xFF0000FFu);
    CHECK(DebugErrors(gpu->Device()) == 0);
    gpu.reset();

    // A full-screen colour clear is stretched over the width: it fills the sides itself, no black bars first and
    // none after it in that frame.
    if (!make(gpu)) {
        return;
    }
    gpu->ClearBuffer(&full, 1 | 2 | 4, 0xFF217219u, 1.0f, 0);
    gpu->ClearBuffer(&middle, 1, 0xFF0000FFu, 1.0f, 0);
    REQUIRE(gpu->Device().CaptureBackBuffer(image));
    CHECK(Pixel(image, 0, h / 2) == 0xFF217219u);
    CHECK(Pixel(image, gpu->Sizes().internalWidth - 1, 0) == 0xFF217219u);
    CHECK(DebugErrors(gpu->Device()) == 0);
}
