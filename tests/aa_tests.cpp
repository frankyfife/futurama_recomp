// Unit tests of M3c render resolution and anti-aliasing (GFX_CONTRACT.md 12): the [render] key parsers and their
// schema defaults, the size/viewport/rectangle scaling of the GPU path, the anisotropy and alpha-to-coverage rules
// of the fixed-function pipeline, and on headless WARP devices with the debug layer: multisampled persistent back
// buffers (resolve, Xbox flip, rectangle clears), the output downsample (exact box for integer ratios,
// Catmull-Rom with anti-ringing for fractional ones), ResizeBackBuffers, GpuRenderer at an internal size,
// alpha-to-coverage on an alpha-tested edge, and the present pass into a window (1:1, upscale, downscale).
// Review M3c fixes: the quality ladder of GpuRenderer::Create (finding 2), the linear-light resolve and downsample
// of [render] linear_aa (finding 3), anisotropy only with linear magnification (finding 4) and the destination-
// alpha rule of alpha-to-coverage (finding 5).
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "gfx/ff_pipeline.h"
#include "gfx/gfx_config.h"
#include "gfx/nipc_gpu.h"
#include "gfx/rhi/d3d11/d3d11_device.h"
#include "gfx/rhi/rhi.h"
#include "platform/window.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace gfx;

namespace {

struct Rgb {
    int r, g, b;
};

Rgb At(const rhi::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = image.pixels.data() + static_cast<std::size_t>(y) * image.rowPitch + x * 4;
    return {p[2], p[1], p[0]};
}

std::size_t DebugProblems(rhi::Device& device) {
    std::vector<rhi::DebugMessage> messages;
    device.DrainDebugMessages(messages);
    std::size_t problems = 0;
    for (const rhi::DebugMessage& m : messages) {
        if (m.severity >= rhi::DebugSeverity::Warning) {
            MESSAGE("debug message " << static_cast<int>(m.severity) << " #" << m.id << ": " << m.text);
            ++problems;
        }
    }
    return problems;
}

std::unique_ptr<rhi::Device> MakeDevice(std::uint32_t w, std::uint32_t h, std::uint32_t samples,
                                        std::uint32_t outW = 0, std::uint32_t outH = 0, std::uint32_t buffers = 1,
                                        void* window = nullptr, bool linearResolve = false) {
    rhi::DeviceDesc d;
    d.linearResolve = linearResolve;
    d.backend = rhi::Backend::D3D11;
    d.softwareAdapter = true;
    d.debugLayer = true;
    d.nativeWindow = window;
    d.width = w;
    d.height = h;
    d.sampleCount = samples;
    d.outputWidth = outW;
    d.outputHeight = outH;
    d.bufferCount = buffers;
    std::string error;
    auto device = rhi::CreateDevice(d, &error);
    if (device == nullptr) {
        MESSAGE("CreateDevice failed (no WARP?): " << error);
    }
    return device;
}

// Diagnostic pipeline of the backend (DiagVS + DiagColorPS / DiagTexturePS, no depth test).
struct Diag {
    rhi::ShaderHandle vs, colorPs, texturePs;
    rhi::PipelineHandle color, textured;
    bool Init(rhi::Device& device) {
        using rhi::d3d11::BuiltinShader;
        vs = device.CreateShader(rhi::ShaderStage::Vertex, rhi::d3d11::BuiltinShaderCode(BuiltinShader::DiagVS), "vs");
        colorPs = device.CreateShader(rhi::ShaderStage::Pixel,
                                      rhi::d3d11::BuiltinShaderCode(BuiltinShader::DiagColorPS), "color ps");
        texturePs = device.CreateShader(rhi::ShaderStage::Pixel,
                                        rhi::d3d11::BuiltinShaderCode(BuiltinShader::DiagTexturePS), "texture ps");
        rhi::PipelineDesc p;
        p.vertexShader = vs;
        p.layout = rhi::d3d11::DiagVertexLayout();
        p.depthStencil.depthEnable = false;
        p.raster.multisample = device.Info().sampleCount > 1;
        p.pixelShader = colorPs;
        color = device.GetPipeline(p, "diag color");
        p.pixelShader = texturePs;
        textured = device.GetPipeline(p, "diag textured");
        return color && textured;
    }
};

// Clip-space vertices of DiagVS: position, color, uv.
struct DiagVertex {
    float x, y, z, r, g, b, a, u, v;
};

void DrawVertices(rhi::Device& device, rhi::PipelineHandle pipeline, const std::vector<DiagVertex>& vertices) {
    rhi::CommandContext& ctx = device.Context();
    const auto bytes = static_cast<std::uint32_t>(vertices.size() * sizeof(DiagVertex));
    const rhi::TransientSpan span = ctx.BeginTransient(rhi::TransientKind::Vertex, bytes, 16);
    REQUIRE(span.cpu != nullptr);
    std::memcpy(span.cpu, vertices.data(), bytes);
    ctx.EndTransient();
    ctx.SetVertexBuffer(0, span.buffer, span.offset, sizeof(DiagVertex));
    ctx.SetPipeline(pipeline);
    ctx.Draw(static_cast<std::uint32_t>(vertices.size()), 0);
}

// Full-target quad with UVs 0..1.
std::vector<DiagVertex> FullQuad() {
    return {{-1, 1, 0, 1, 1, 1, 1, 0, 0},  {1, 1, 0, 1, 1, 1, 1, 1, 0}, {-1, -1, 0, 1, 1, 1, 1, 0, 1},
            {-1, -1, 0, 1, 1, 1, 1, 0, 1}, {1, 1, 0, 1, 1, 1, 1, 1, 0}, {1, -1, 0, 1, 1, 1, 1, 1, 1}};
}

// Pixels of row `y` whose luminance is strictly between `lo` and `hi` (+-8): partially covered edge pixels.
int IntermediatePixels(const rhi::Image& image, int lo, int hi) {
    int n = 0;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const int g = At(image, x, y).g;
            n += g > lo + 8 && g < hi - 8 ? 1 : 0;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("aa: [render] M3c key parsers agree with the configuration schema") {
    std::uint32_t w = 1, h = 1;
    CHECK(ParseRenderResolution("auto", &w, &h));
    CHECK((w == 0 && h == 0));
    CHECK(ParseRenderResolution("AUTO", &w, &h));
    CHECK(ParseRenderResolution("1920x1080", &w, &h));
    CHECK((w == 1920 && h == 1080));
    CHECK(ParseRenderResolution(" 640 X 480 ", &w, &h));
    CHECK((w == 640 && h == 480));
    CHECK_FALSE(ParseRenderResolution("100x100", &w, &h));  // below kConfigResolutionMin
    CHECK_FALSE(ParseRenderResolution("16385x2000", &w, &h));
    CHECK_FALSE(ParseRenderResolution("1920", &w, &h));
    CHECK_FALSE(ParseRenderResolution("native", &w, &h));
    float scale = 0.0f;
    CHECK(ParseRenderScale("1.5", &scale));
    CHECK(scale == 1.5f);
    CHECK(ParseRenderScale("2", &scale));
    CHECK(scale == 2.0f);
    CHECK(ParseRenderScale("1.25", &scale));
    CHECK_FALSE(ParseRenderScale("0.5", &scale));
    CHECK_FALSE(ParseRenderScale("2.5", &scale));
    CHECK_FALSE(ParseRenderScale("x", &scale));
    std::uint32_t n = 0;
    CHECK(ParseMsaa("off", &n));
    CHECK(n == 1);
    CHECK(ParseMsaa("OFF", &n));
    CHECK(ParseMsaa("8", &n));
    CHECK(n == 8);
    CHECK_FALSE(ParseMsaa("3", &n));
    CHECK_FALSE(ParseMsaa("16", &n));
    CHECK(ParseAnisotropy("off", &n));
    CHECK(n == 1);
    CHECK(ParseAnisotropy("16", &n));
    CHECK(n == 16);
    CHECK_FALSE(ParseAnisotropy("32", &n));

    // Every key exists in the schema; the schema defaults are the renderer's defaults; the choices match.
    for (const char* key :
         {"resolution", "render_scale", "msaa", "alpha_to_coverage", "anisotropy", "screenshot_size", "linear_aa"}) {
        INFO("[render] " << key);
        REQUIRE(core::FindConfigKey("render", key) != nullptr);
    }
    CHECK(ParseRenderResolution(core::ConfigKeyDefault("render", "resolution"), &w, &h));
    CHECK((w == 0 && h == 0));  // auto
    CHECK(ParseRenderScale(core::ConfigKeyDefault("render", "render_scale"), &scale));
    CHECK(scale == kDefaultRenderScale);
    CHECK(ParseMsaa(core::ConfigKeyDefault("render", "msaa"), &n));
    CHECK(n == kDefaultMsaa);
    CHECK(ParseAnisotropy(core::ConfigKeyDefault("render", "anisotropy"), &n));
    CHECK(n == kDefaultAnisotropy);
    bool a2c = false;
    CHECK(core::ParseConfigBool(core::ConfigKeyDefault("render", "alpha_to_coverage"), a2c));
    CHECK(a2c == kDefaultAlphaToCoverage);
    CHECK(core::ConfigKeyDefault("render", "screenshot_size") == "output");
    bool linear = true;
    CHECK(core::ParseConfigBool(core::ConfigKeyDefault("render", "linear_aa"), linear));
    CHECK(linear == kDefaultLinearAa);
    CHECK(kDefaultScreenshotSize == ScreenshotSize::Output);
    CHECK(std::string_view(core::ConfigChoiceList(*core::FindConfigKey("render", "msaa"))) == "off|2|4|8");
    CHECK(std::string_view(core::ConfigChoiceList(*core::FindConfigKey("render", "anisotropy"))) == "off|2|4|8|16");
    const std::string_view sizes = core::ConfigChoiceList(*core::FindConfigKey("render", "screenshot_size"));
    CHECK((sizes == "output|internal" || sizes == "internal|output"));

    // Settings from a configuration (the loader validated the values; invalid ones are defaults + GFX/warn).
    core::Config cfg;
    cfg.values["render.resolution"] = "1440x1080";
    cfg.values["render.render_scale"] = "1.5";
    cfg.values["render.msaa"] = "8";
    cfg.values["render.alpha_to_coverage"] = "off";
    cfg.values["render.anisotropy"] = "off";
    cfg.values["render.screenshot_size"] = "internal";
    cfg.values["render.linear_aa"] = "on";
    Settings s = SettingsFromConfig(cfg);
    CHECK(s.resolutionWidth == 1440);
    CHECK(s.resolutionHeight == 1080);
    CHECK(s.renderScale == 1.5f);
    CHECK(s.msaa == 8);
    CHECK_FALSE(s.alphaToCoverage);
    CHECK(s.anisotropy == 1);
    CHECK(s.screenshotSize == ScreenshotSize::Internal);
    CHECK(s.linearAa);
    const RenderQuality q = RenderQualityOf(s);
    CHECK(q.linearAa);
    CHECK(q.resolutionWidth == 1440);
    CHECK(q.msaa == 8);
    CHECK(q.screenshots == ScreenshotSize::Internal);
    s = SettingsFromConfig(core::Config{});
    CHECK(s.resolutionWidth == 0);
    CHECK(s.msaa == kDefaultMsaa);
    CHECK(s.anisotropy == kDefaultAnisotropy);
    CHECK(s.alphaToCoverage);
    CHECK(s.screenshotSize == ScreenshotSize::Output);
    CHECK(s.linearAa == kDefaultLinearAa);
}

TEST_CASE("aa: quality ladder of GpuRenderer::Create (review M3c finding 2)") {
    RenderQuality q;
    q.resolutionWidth = 0;  // auto
    q.renderScale = 2.0f;
    q.msaa = 8;
    q.alphaToCoverage = true;
    q.linearAa = true;
    const QualityLadder ladder = QualityFallbacks(q);
    REQUIRE(ladder.count == 6);
    const struct {
        float scale;
        std::uint32_t msaa, width;
    } expected[] = {{2.0f, 8, 0}, {1.0f, 8, 0}, {1.0f, 4, 0}, {1.0f, 2, 0}, {1.0f, 1, 0}, {1.0f, 1, 640}};
    for (std::uint32_t i = 0; i < ladder.count; ++i) {
        INFO("step " << i);
        CHECK(ladder.steps[i].renderScale == expected[i].scale);
        CHECK(ladder.steps[i].msaa == expected[i].msaa);
        CHECK(ladder.steps[i].resolutionWidth == expected[i].width);
        CHECK(ladder.steps[i].alphaToCoverage);  // the options stay (no effect without MSAA)
        CHECK(ladder.steps[i].linearAa);
    }
    CHECK(ladder.steps[5].resolutionHeight == 480);
    CHECK(DescribeQuality(ladder.steps[0]) == "render scale 2.00, MSAA 8x, resolution auto");
    CHECK(DescribeQuality(ladder.steps[5]) == "render scale 1.00, MSAA 1x, resolution 640x480");
    // Classic has nothing to reduce: one step.
    RenderQuality classic;
    classic.resolutionWidth = 640;
    classic.resolutionHeight = 480;
    CHECK(QualityFallbacks(classic).count == 1);
    // MSAA 4 at a fixed resolution: 4 -> 2 -> 1 -> 640x480.
    RenderQuality fixed;
    fixed.resolutionWidth = 2880;
    fixed.resolutionHeight = 2160;
    fixed.msaa = 4;
    CHECK(QualityFallbacks(fixed).count == 4);
}

TEST_CASE("aa: render sizes, viewport and rectangle scaling") {
    // Classic: the Create size, nothing scaled.
    RenderSizes s = ComputeRenderSizes(640, 480, 640, 480, 2880, 2160, 1.0f);
    CHECK((s.outputWidth == 640 && s.outputHeight == 480 && s.internalWidth == 640 && s.internalHeight == 480));
    s = ComputeRenderSizes(640, 480, 0, 0, 0, 0, 1.0f);  // auto without a view (headless)
    CHECK((s.outputWidth == 640 && s.internalHeight == 480));
    // Auto on the 3840x2160 monitor in borderless fullscreen: the view is 2880x2160.
    s = ComputeRenderSizes(640, 480, 0, 0, 2880, 2160, 1.0f);
    CHECK((s.outputWidth == 2880 && s.outputHeight == 2160 && s.internalWidth == 2880 && s.internalHeight == 2160));
    // The windowed view of the dev machine (225 %, window_scale 2): not exactly 4:3, kept as is.
    s = ComputeRenderSizes(640, 480, 0, 0, 2631, 1973, 1.0f);
    CHECK((s.outputWidth == 2631 && s.outputHeight == 1973));
    // A 16:9 resolution: the largest 4:3 rectangle inside it.
    s = ComputeRenderSizes(640, 480, 1920, 1080, 2631, 1973, 1.0f);
    CHECK((s.outputWidth == 1440 && s.outputHeight == 1080));
    // Supersampling on top.
    s = ComputeRenderSizes(640, 480, 1440, 1080, 0, 0, 2.0f);
    CHECK((s.outputWidth == 1440 && s.internalWidth == 2880 && s.internalHeight == 2160 && s.renderScale == 2.0f));
    s = ComputeRenderSizes(640, 480, 0, 0, 2631, 1973, 1.5f);
    CHECK((s.internalWidth == 3947 && s.internalHeight == 2960));
    // The internal size never exceeds the D3D11 texture limit.
    s = ComputeRenderSizes(640, 480, 16384, 12288, 0, 0, 2.0f);
    CHECK((s.internalWidth <= kMaxRenderDimension && s.internalHeight <= kMaxRenderDimension));
    CHECK(s.renderScale == 1.0f);
    s = ComputeRenderSizes(640, 480, 10000, 7500, 0, 0, 2.0f);
    CHECK(s.internalWidth <= kMaxRenderDimension);
    CHECK(s.renderScale == doctest::Approx(16384.0 / 10000.0).epsilon(1e-4));

    // Viewports: identity at equal sizes, exact multiples otherwise.
    const rhi::Viewport full{0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f};
    rhi::Viewport v = ScaleViewport(full, 640, 480, 640, 480);
    CHECK(std::memcmp(&v, &full, sizeof(v)) == 0);
    v = ScaleViewport(full, 640, 480, 2880, 2160);
    CHECK((v.x == 0.0f && v.y == 0.0f && v.width == 2880.0f && v.height == 2160.0f));
    v = ScaleViewport(rhi::Viewport{64.0f, 48.0f, 320.0f, 240.0f, 0.25f, 0.75f}, 640, 480, 2880, 2160);
    CHECK((v.x == 288.0f && v.y == 216.0f && v.width == 1440.0f && v.height == 1080.0f));
    CHECK((v.minDepth == 0.25f && v.maxDepth == 0.75f));
    v = ScaleViewport(full, 640, 480, 2631, 1973);
    CHECK(v.width == 2631.0f);  // computed in double: no 2630.9998
    CHECK(v.height == 1973.0f);

    // Rectangles: identity at equal sizes; adjacent guest rectangles stay adjacent.
    const rhi::Rect r{10, 20, 213, 100};
    rhi::Rect q = ScaleRect(r, 640, 480, 640, 480);
    CHECK((q.left == 10 && q.top == 20 && q.right == 213 && q.bottom == 100));
    q = ScaleRect(r, 640, 480, 2880, 2160);
    CHECK((q.left == 45 && q.top == 90 && q.right == 959 && q.bottom == 450));  // 958.5 -> 959
    const rhi::Rect next = ScaleRect(rhi::Rect{213, 20, 640, 100}, 640, 480, 2880, 2160);
    CHECK(next.left == q.right);
    CHECK(next.right == 2880);
}

TEST_CASE("aa: anisotropy and alpha-to-coverage rules of the fixed-function pipeline") {
    ni::TextureStageDesc stage;
    stage.minFilter = rhi::Filter::Linear;
    stage.magFilter = rhi::Filter::Linear;
    stage.mipFilter = rhi::MipFilter::Linear;
    CHECK(ff::MakeSamplerDesc(stage, 16, false).maxAnisotropy == 16);
    CHECK(ff::MakeSamplerDesc(stage, 8, false).maxAnisotropy == 8);
    CHECK(ff::MakeSamplerDesc(stage, 16, true).maxAnisotropy == 1);  // UI / screen-space draws
    CHECK(ff::MakeSamplerDesc(stage, 1, false) == ff::MakeSamplerDesc(stage));
    stage.mipFilter = rhi::MipFilter::Point;  // bilinear with mip maps: anisotropic too
    CHECK(ff::MakeSamplerDesc(stage, 16, false).maxAnisotropy == 16);
    stage.mipFilter = rhi::MipFilter::None;  // no mip maps (UI textures)
    CHECK(ff::MakeSamplerDesc(stage, 16, false).maxAnisotropy == 1);
    stage.mipFilter = rhi::MipFilter::Linear;
    stage.minFilter = rhi::Filter::Point;  // point-filtered textures keep their look
    CHECK(ff::MakeSamplerDesc(stage, 16, false).maxAnisotropy == 1);
    // Review M3c finding 4: MIN = LINEAR / MAG = POINT stays pixelated when magnified (D3D11's anisotropic filter
    // would magnify linearly) - the D3D8 filter is kept.
    stage.minFilter = rhi::Filter::Linear;
    stage.magFilter = rhi::Filter::Point;
    CHECK(ff::MakeSamplerDesc(stage, 16, false).maxAnisotropy == 1);
    CHECK(ff::MakeSamplerDesc(stage, 16, false) == ff::MakeSamplerDesc(stage));
    stage.magFilter = rhi::Filter::Linear;
    CHECK(ff::MakeSamplerDesc(stage, 16, false).maxAnisotropy == 16);

    // Review M3c finding 5: blends that read the destination alpha (which alpha-to-coverage changes).
    rhi::BlendDesc blend;
    CHECK_FALSE(ff::ReadsDestinationAlpha(blend));  // blending off
    blend.enable = true;
    blend.src = rhi::BlendFactor::SrcAlpha;
    blend.dst = rhi::BlendFactor::InvSrcAlpha;
    blend.srcAlpha = rhi::BlendFactor::SrcAlpha;
    blend.dstAlpha = rhi::BlendFactor::InvSrcAlpha;
    CHECK_FALSE(ff::ReadsDestinationAlpha(blend));  // the frontend's blends (4/5)
    blend.dst = rhi::BlendFactor::InvDestAlpha;
    CHECK(ff::ReadsDestinationAlpha(blend));
    blend.dst = rhi::BlendFactor::InvSrcAlpha;
    blend.src = rhi::BlendFactor::SrcAlphaSat;  // min(As, 1 - Ad)
    CHECK(ff::ReadsDestinationAlpha(blend));
    blend.src = rhi::BlendFactor::One;
    blend.dstAlpha = rhi::BlendFactor::DestAlpha;
    CHECK(ff::ReadsDestinationAlpha(blend));
    blend.enable = false;
    CHECK_FALSE(ff::ReadsDestinationAlpha(blend));

    ni::RenderStateDesc state;
    state.alphaTestEnable = true;
    state.alphaFunc = rhi::CompareFunc::Greater;
    state.alphaRef = 128;
    ff::RenderOptions options;
    options.sampleCount = 4;
    options.alphaToCoverage = true;
    CHECK(ff::UsesAlphaToCoverage(state, options));
    state.alphaFunc = rhi::CompareFunc::Less;
    CHECK(ff::UsesAlphaToCoverage(state, options));
    state.alphaFunc = rhi::CompareFunc::Equal;  // no edge to smooth
    CHECK_FALSE(ff::UsesAlphaToCoverage(state, options));
    state.alphaFunc = rhi::CompareFunc::GreaterEqual;
    state.alphaRef = 0;  // always passes
    CHECK_FALSE(ff::UsesAlphaToCoverage(state, options));
    state.alphaFunc = rhi::CompareFunc::LessEqual;
    state.alphaRef = 255;
    CHECK_FALSE(ff::UsesAlphaToCoverage(state, options));
    state.alphaFunc = rhi::CompareFunc::Greater;
    state.alphaRef = 255;  // never passes
    CHECK_FALSE(ff::UsesAlphaToCoverage(state, options));
    state.alphaRef = 0;  // "alpha > 0": the usual cut-out test
    CHECK(ff::UsesAlphaToCoverage(state, options));
    state.alphaBlendEnable = true;  // blended cut-outs keep the discard
    CHECK_FALSE(ff::UsesAlphaToCoverage(state, options));
    state.alphaBlendEnable = false;
    options.sampleCount = 1;
    CHECK_FALSE(ff::UsesAlphaToCoverage(state, options));
    options.sampleCount = 4;
    options.alphaToCoverage = false;
    CHECK_FALSE(ff::UsesAlphaToCoverage(state, options));
    state.alphaTestEnable = false;
    options.alphaToCoverage = true;
    CHECK_FALSE(ff::UsesAlphaToCoverage(state, options));
}

TEST_CASE("aa: multisampled back buffers resolve, keep the Xbox flip and take rectangle clears (WARP)") {
    auto single = MakeDevice(64, 48, 1);
    auto msaa = MakeDevice(64, 48, 4, 0, 0, 2);
    if (single == nullptr || msaa == nullptr) {
        return;
    }
    CHECK(single->Info().sampleCount == 1);
    const std::uint32_t supported =
        msaa->SupportedSampleCount(rhi::Format::BGRA8_UNorm, rhi::Format::D24_UNorm_S8_UInt, 4);
    CHECK(msaa->Info().sampleCount == supported);
    CHECK(msaa->SupportedSampleCount(rhi::Format::BGRA8_UNorm, rhi::Format::D24_UNorm_S8_UInt, 1) == 1);
    CHECK(msaa->SupportedSampleCount(rhi::Format::BGRA8_UNorm, rhi::Format::D24_UNorm_S8_UInt, 7) <= 4);
    if (supported < 4) {
        MESSAGE("WARP without 4x MSAA for BGRA8 + D24S8 - MSAA checks skipped");
        return;
    }
    // A diagonal white triangle on black: without MSAA every pixel is 0 or 255, with 4x the edge pixels are
    // partially covered.
    const std::vector<DiagVertex> triangle = {
        {-1, 1, 0, 1, 1, 1, 1, 0, 0}, {1, 1, 0, 1, 1, 1, 1, 0, 0}, {-1, -0.9f, 0, 1, 1, 1, 1, 0, 0}};
    const float black[4] = {0, 0, 0, 1};
    int intermediate[2] = {0, 0};
    rhi::Device* devices[2] = {single.get(), msaa.get()};
    for (int i = 0; i < 2; ++i) {
        rhi::Device& device = *devices[i];
        Diag diag;
        REQUIRE(diag.Init(device));
        device.Context().Clear(rhi::kClearColor | rhi::kClearDepth, black, 1.0f, 0, nullptr);
        DrawVertices(device, diag.color, triangle);
        rhi::Image image;
        REQUIRE(device.CaptureBackBuffer(image));
        CHECK((image.width == 64 && image.height == 48));
        CHECK(At(image, 2, 2).g == 255);
        CHECK(At(image, 60, 44).g == 0);
        intermediate[i] = IntermediatePixels(image, 0, 255);
        CHECK(DebugProblems(device) == 0);
    }
    CHECK(intermediate[0] == 0);
    CHECK(intermediate[1] > 40);  // one partially covered pixel per row and more
    MESSAGE("edge pixels with partial coverage: no AA " << intermediate[0] << ", 4x MSAA " << intermediate[1]);

    // Rectangle clears (ClearView for color, the depth/stencil triangle) on the multisampled targets.
    rhi::CommandContext& ctx = msaa->Context();
    const float red[4] = {1, 0, 0, 1};
    const float green[4] = {0, 1, 0, 1};
    ctx.Clear(rhi::kClearColor | rhi::kClearDepth | rhi::kClearStencil, red, 1.0f, 0, nullptr);
    const rhi::Rect rect{16, 8, 48, 40};
    ctx.Clear(rhi::kClearColor | rhi::kClearDepth | rhi::kClearStencil, green, 0.5f, 3, &rect);
    rhi::Image image;
    REQUIRE(msaa->CaptureBackBuffer(image));
    CHECK(At(image, 16, 8).g == 255);
    CHECK(At(image, 47, 39).g == 255);
    CHECK(At(image, 15, 8).r == 255);
    CHECK(At(image, 48, 39).r == 255);
    CHECK(DebugProblems(*msaa) == 0);

    // Xbox flip with multisampled buffers: frame 1 red, frame 2 green, frame 3 draws nothing -> red again.
    ctx.Clear(rhi::kClearColor, red, 1.0f, 0, nullptr);
    msaa->Present(rhi::PresentDesc{0});
    ctx.Clear(rhi::kClearColor, green, 1.0f, 0, nullptr);
    msaa->Present(rhi::PresentDesc{0});
    REQUIRE(msaa->CaptureBackBuffer(image));
    CHECK(At(image, 10, 10).r == 255);
    CHECK(At(image, 10, 10).g == 0);
    msaa->Present(rhi::PresentDesc{0});
    REQUIRE(msaa->CaptureBackBuffer(image));
    CHECK(At(image, 10, 10).g == 255);
    CHECK(DebugProblems(*msaa) == 0);
}

TEST_CASE("aa: output downsample - exact box for integer ratios, Catmull-Rom otherwise (WARP)") {
    // Random texture drawn 1:1 into the back buffer, then compared with a CPU box filter of the captured back buffer.
    auto device = MakeDevice(64, 48, 1, 32, 24);
    if (device == nullptr) {
        return;
    }
    CHECK(device->Info().outputWidth == 32);
    CHECK(device->Info().outputHeight == 24);
    Diag diag;
    REQUIRE(diag.Init(*device));
    std::vector<std::uint32_t> texels(64 * 48);
    std::mt19937 random(1234);
    for (std::uint32_t& t : texels) {
        t = (random() & 0x00FFFFFFu) | 0xFF000000u;
    }
    rhi::TextureDesc td;
    td.width = 64;
    td.height = 48;
    td.format = rhi::Format::BGRA8_UNorm;
    const rhi::SubresourceData level{texels.data(), 64 * 4, 64 * 48 * 4};
    const rhi::TextureHandle texture = device->CreateTexture(td, &level);
    REQUIRE(texture);
    rhi::SamplerDesc point;
    point.minFilter = point.magFilter = rhi::Filter::Point;
    point.addressU = point.addressV = rhi::AddressMode::Clamp;
    rhi::CommandContext& ctx = device->Context();
    ctx.SetTexture(0, texture);
    ctx.SetSampler(0, device->GetSampler(point));
    DrawVertices(*device, diag.textured, FullQuad());
    rhi::Image back, out;
    REQUIRE(device->CaptureBackBuffer(back));
    REQUIRE(device->CaptureOutput(out));
    REQUIRE((out.width == 32 && out.height == 24));
    int worst = 0;
    for (std::uint32_t y = 0; y < 24; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            const Rgb a = At(back, 2 * x, 2 * y), b = At(back, 2 * x + 1, 2 * y), c = At(back, 2 * x, 2 * y + 1),
                      d = At(back, 2 * x + 1, 2 * y + 1);
            const Rgb o = At(out, x, y);
            const double expected[3] = {(a.r + b.r + c.r + d.r) / 4.0, (a.g + b.g + c.g + d.g) / 4.0,
                                        (a.b + b.b + c.b + d.b) / 4.0};
            worst = std::max({worst, static_cast<int>(std::lround(std::fabs(o.r - expected[0]) * 2)),
                              static_cast<int>(std::lround(std::fabs(o.g - expected[1]) * 2)),
                              static_cast<int>(std::lround(std::fabs(o.b - expected[2]) * 2))});
        }
    }
    CHECK(worst <= 1);  // within half a step: the UNORM rounding of the exact average
    // The context keeps drawing into the back buffer after the mid-frame CaptureOutput.
    const float blue[4] = {0, 0, 1, 1};
    ctx.Clear(rhi::kClearColor, blue, 1.0f, 0, nullptr);
    DrawVertices(*device, diag.textured, FullQuad());
    REQUIRE(device->CaptureBackBuffer(back));
    CHECK(At(back, 5, 5).r == static_cast<int>((texels[5 * 64 + 5] >> 16) & 0xFF));
    CHECK(DebugProblems(*device) == 0);
    device->Destroy(texture);

    // Fractional ratio (1.6): a flat gray stays flat (normalized weights); an edge stays monotonic without any
    // overshoot (the anti-ringing clamp of the Catmull-Rom filter) and is at most 3 output pixels wide (review M3c
    // finding 1: the Mitchell-Netravali filter before was softer).
    auto frac = MakeDevice(64, 48, 1, 40, 30);
    REQUIRE(frac != nullptr);
    Diag fd;
    REQUIRE(fd.Init(*frac));
    const float gray[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    frac->Context().Clear(rhi::kClearColor, gray, 1.0f, 0, nullptr);
    REQUIRE(frac->CaptureOutput(out));
    REQUIRE((out.width == 40 && out.height == 30));
    for (std::uint32_t y = 0; y < 30; y += 7) {
        for (std::uint32_t x = 0; x < 40; ++x) {
            CHECK(std::abs(At(out, x, y).g - 128) <= 1);
        }
    }
    const float dark[4] = {0.25f, 0.25f, 0.25f, 1.0f};
    frac->Context().Clear(rhi::kClearColor, dark, 1.0f, 0, nullptr);
    const rhi::Rect right{32, 0, 64, 48};
    const float light[4] = {0.75f, 0.75f, 0.75f, 1.0f};
    frac->Context().Clear(rhi::kClearColor, light, 1.0f, 0, &right);
    REQUIRE(frac->CaptureOutput(out));
    int previous = 0;
    int transition = 0;
    for (std::uint32_t x = 0; x < 40; ++x) {
        const int g = At(out, x, 15).g;
        CHECK(g >= 64 - 1);   // no undershoot
        CHECK(g <= 191 + 1);  // no overshoot
        if (x > 0) {
            CHECK(g >= previous - 1);  // monotonic
        }
        if (x > 0 && (x < 17 || x > 23)) {
            CHECK(std::abs(g - previous) <= 2);  // flat away from the edge
        }
        transition += g > 64 + 8 && g < 191 - 8 ? 1 : 0;
        previous = g;
    }
    CHECK(transition <= 3);
    MESSAGE("fractional downsample 1.6: edge transition " << transition << " output pixels");
    CHECK(std::abs(At(out, 2, 15).g - 64) <= 1);
    CHECK(std::abs(At(out, 37, 15).g - 191) <= 1);
    CHECK(DebugProblems(*frac) == 0);
}

TEST_CASE("aa: [render] linear_aa resolves and downsamples linear light (WARP, review M3c finding 3)") {
    // sRGB encoding of a linear value 0..1 as an 8-bit level.
    auto encode = [](double v) {
        const double s = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
        return static_cast<int>(std::lround(s * 255.0));
    };
    // 1. Downsample 2:1 of a black/white checkerboard: gamma average 128, linear light encode(0.5) = 188.
    std::vector<std::uint32_t> checker(64 * 48);
    for (std::uint32_t y = 0; y < 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            checker[y * 64 + x] = ((x + y) & 1) != 0 ? 0xFFFFFFFFu : 0xFF000000u;
        }
    }
    rhi::TextureDesc td;
    td.width = 64;
    td.height = 48;
    td.format = rhi::Format::BGRA8_UNorm;
    const rhi::SubresourceData level{checker.data(), 64 * 4, 64 * 48 * 4};
    rhi::SamplerDesc point;
    point.minFilter = point.magFilter = rhi::Filter::Point;
    point.addressU = point.addressV = rhi::AddressMode::Clamp;
    int center[2] = {0, 0};
    for (int linear = 0; linear < 2; ++linear) {
        auto device = MakeDevice(64, 48, 1, 32, 24, 1, nullptr, linear != 0);
        if (device == nullptr) {
            return;
        }
        CHECK(device->Info().linearResolve == (linear != 0));
        Diag diag;
        REQUIRE(diag.Init(*device));
        const rhi::TextureHandle texture = device->CreateTexture(td, &level);
        REQUIRE(texture);
        device->Context().SetTexture(0, texture);
        device->Context().SetSampler(0, device->GetSampler(point));
        DrawVertices(*device, diag.textured, FullQuad());
        rhi::Image back, out;
        REQUIRE(device->CaptureBackBuffer(back));
        CHECK(At(back, 1, 0).g == 255);  // the back buffer itself is unchanged (UNORM rendering)
        CHECK(At(back, 0, 0).g == 0);
        REQUIRE(device->CaptureOutput(out));
        center[linear] = At(out, 16, 12).g;
        CHECK(DebugProblems(*device) == 0);
        device->Destroy(texture);
    }
    CHECK(std::abs(center[0] - 128) <= 1);
    CHECK(std::abs(center[1] - encode(0.5)) <= 2);
    MESSAGE("2:1 downsample of a checkerboard: gamma " << center[0] << ", linear light " << center[1]);

    // 2. MSAA resolve of a white triangle on black: a pixel with coverage c resolves to 255c in gamma and to
    //    encode(c) on linear light.
    auto gamma = MakeDevice(64, 48, 4, 0, 0, 1, nullptr, false);
    auto light = MakeDevice(64, 48, 4, 0, 0, 1, nullptr, true);
    if (gamma == nullptr || light == nullptr || gamma->Info().sampleCount < 4 || light->Info().sampleCount < 4) {
        MESSAGE("WARP without 4x MSAA - linear resolve check skipped");
        return;
    }
    const std::vector<DiagVertex> triangle = {
        {-1, 1, 0, 1, 1, 1, 1, 0, 0}, {1, 1, 0, 1, 1, 1, 1, 0, 0}, {-1, -0.9f, 0, 1, 1, 1, 1, 0, 0}};
    const float black[4] = {0, 0, 0, 1};
    rhi::Image images[2];
    rhi::Device* devices[2] = {gamma.get(), light.get()};
    for (int i = 0; i < 2; ++i) {
        Diag diag;
        REQUIRE(diag.Init(*devices[i]));
        devices[i]->Context().Clear(rhi::kClearColor | rhi::kClearDepth, black, 1.0f, 0, nullptr);
        DrawVertices(*devices[i], diag.color, triangle);
        REQUIRE(devices[i]->CaptureBackBuffer(images[i]));
        CHECK(DebugProblems(*devices[i]) == 0);
    }
    int compared = 0, worst = 0;
    for (std::uint32_t y = 0; y < 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const int g = At(images[0], x, y).g;
            const int l = At(images[1], x, y).g;
            if (g > 0 && g < 255) {
                worst = std::max(worst, std::abs(l - encode(g / 255.0)));
                ++compared;
            } else {
                CHECK(l == g);  // fully covered or empty pixels are the same
            }
        }
    }
    CHECK(compared > 40);
    CHECK(worst <= 3);
    MESSAGE("linear resolve: " << compared << " edge pixels, largest difference to encode(coverage) " << worst);
}

TEST_CASE("aa: ResizeBackBuffers carries the pictures of the persistent back buffers over (WARP, MSAA)") {
    auto device = MakeDevice(64, 48, 4, 0, 0, 2);
    if (device == nullptr) {
        return;
    }
    rhi::CommandContext& ctx = device->Context();
    const float red[4] = {1, 0, 0, 1};
    const float blue[4] = {0, 0, 1, 1};
    ctx.Clear(rhi::kClearColor, red, 1.0f, 0, nullptr);
    device->Present(rhi::PresentDesc{0});
    ctx.Clear(rhi::kClearColor, blue, 1.0f, 0, nullptr);
    device->Present(rhi::PresentDesc{0});  // current = the red buffer again
    REQUIRE(device->ResizeBackBuffers(128, 96, 4, 64, 48));
    CHECK(device->Info().backBufferWidth == 128);
    CHECK(device->Info().backBufferHeight == 96);
    CHECK(device->Info().outputWidth == 64);
    CHECK(device->Info().sampleCount == std::min<std::uint32_t>(
                                            4, device->SupportedSampleCount(rhi::Format::BGRA8_UNorm,
                                                                            rhi::Format::D24_UNorm_S8_UInt, 4)));
    rhi::Image image;
    REQUIRE(device->CaptureBackBuffer(image));
    CHECK((image.width == 128 && image.height == 96));
    CHECK(At(image, 127, 95).r == 255);
    REQUIRE(device->CaptureOutput(image));
    CHECK((image.width == 64 && image.height == 48));
    CHECK(At(image, 0, 0).r == 255);
    device->Present(rhi::PresentDesc{0});
    REQUIRE(device->CaptureBackBuffer(image));
    CHECK(At(image, 64, 48).b == 255);  // the blue one
    // The context draws into the new buffers with a whole-target viewport.
    Diag diag;
    REQUIRE(diag.Init(*device));
    DrawVertices(*device, diag.color, FullQuad());
    REQUIRE(device->CaptureBackBuffer(image));
    CHECK(At(image, 127, 95).g == 255);
    CHECK(device->ResizeBackBuffers(128, 96, 4, 64, 48));  // unchanged: nothing to do
    CHECK_FALSE(device->ResizeBackBuffers(0, 96, 4, 0, 0));
    CHECK(DebugProblems(*device) == 0);
}

TEST_CASE("aa: GpuRenderer renders the guest's 640x480 world at the internal size (headless WARP)") {
    GpuRenderer::CreateParams params;
    params.width = 64;  // guest size (stands in for 640x480)
    params.height = 48;
    params.debugLayer = true;
    params.softwareAdapter = true;
    params.quality.resolutionWidth = 128;
    params.quality.resolutionHeight = 96;
    params.quality.renderScale = 2.0f;
    params.quality.msaa = 4;
    params.quality.alphaToCoverage = true;
    params.quality.anisotropy = 16;
    std::string error;
    std::unique_ptr<GpuRenderer> gpu = GpuRenderer::Create(params, &error);
    if (gpu == nullptr) {
        MESSAGE("GpuRenderer::Create failed (no WARP?): " << error);
        return;
    }
    CHECK(gpu->Sizes().outputWidth == 128);
    CHECK(gpu->Sizes().internalWidth == 256);
    CHECK(gpu->Sizes().internalHeight == 192);
    rhi::Device& device = gpu->Device();
    CHECK(device.Info().backBufferWidth == 256);
    CHECK(device.Info().outputWidth == 128);
    // ClearBuffer with the NiRect of the top-right quadrant (y up) in guest pixels -> internal pixels.
    gpu->ClearBuffer(nullptr, 1 | 2 | 4, 0xFF102030u, 1.0f, 0);
    const ni::NiRectF topRight{0.5f, 1.0f, 1.0f, 0.5f};
    gpu->ClearBuffer(&topRight, 1, 0xFF00FF00u, 1.0f, 0);
    rhi::Image image;
    REQUIRE(device.CaptureBackBuffer(image));
    REQUIRE((image.width == 256 && image.height == 192));
    CHECK(At(image, 128, 0).g == 255);
    CHECK(At(image, 255, 95).g == 255);
    CHECK(At(image, 127, 0).g == 0x20);
    CHECK(At(image, 255, 96).g == 0x20);
    REQUIRE(device.CaptureOutput(image));
    REQUIRE((image.width == 128 && image.height == 96));
    CHECK(At(image, 64, 0).g == 255);
    CHECK(At(image, 63, 0).g == 0x20);  // the box filter keeps the edge exactly
    CHECK(At(image, 127, 47).g == 255);
    CHECK(At(image, 127, 48).g == 0x20);
    // A fixed resolution does not follow the view.
    CHECK_FALSE(gpu->ResizeForView(200, 150));
    CHECK(DebugProblems(device) == 0);

    // Resolution auto (headless: the guest size) follows a view size at once with ResizeForView.
    params.quality.resolutionWidth = params.quality.resolutionHeight = 0;
    params.quality.renderScale = 1.0f;
    gpu = GpuRenderer::Create(params, &error);
    REQUIRE(gpu != nullptr);
    CHECK(gpu->Sizes().internalWidth == 64);
    CHECK(gpu->ResizeForView(200, 150));
    CHECK(gpu->Sizes().outputWidth == 200);
    CHECK(gpu->Sizes().internalHeight == 150);
    CHECK_FALSE(gpu->ResizeForView(200, 150));  // unchanged
    gpu->ClearBuffer(&topRight, 1, 0xFFFF0000u, 1.0f, 0);
    REQUIRE(gpu->Device().CaptureBackBuffer(image));
    REQUIRE((image.width == 200 && image.height == 150));
    CHECK(At(image, 100, 0).r == 255);
    CHECK(At(image, 199, 74).r == 255);
    CHECK(At(image, 99, 0).r != 255);
    CHECK(At(image, 199, 75).r != 255);
    CHECK(DebugProblems(gpu->Device()) == 0);
}

TEST_CASE("aa: alpha-to-coverage turns an alpha-tested edge into partial coverage (ff pipeline, WARP)") {
    // Two horizontal half-transparent edges: an 8x1 alpha ramp stretched over the target, alpha test GREATER 127.
    // With discard every resolved pixel is either the quad or the background; with alpha-to-coverage (4x MSAA)
    // the pixels at the threshold are partially covered and the 50 % line stays where the test cuts.
    int edgeColumn[2] = {-1, -1};
    int intermediate[2] = {0, 0};
    for (int a2c = 0; a2c < 2; ++a2c) {
        auto device = MakeDevice(64, 64, 4);
        if (device == nullptr) {
            return;
        }
        if (device->Info().sampleCount < 4) {
            MESSAGE("WARP without 4x MSAA - alpha-to-coverage check skipped");
            return;
        }
        ff::FixedFunctionPipeline pipeline;
        std::string error;
        REQUIRE_MESSAGE(pipeline.Init(*device, &error), error);
        ff::RenderOptions options;
        options.sampleCount = 4;
        options.alphaToCoverage = a2c != 0;
        pipeline.SetRenderOptions(options);
        ni::CameraDesc camera;
        camera.view = camera.projection = camera.screenProjection = IdentityMatrix();
        camera.viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        rhi::CommandContext& ctx = device->Context();
        ctx.SetRenderTargets({}, {});
        ctx.SetViewport(camera.viewport);
        pipeline.SetCamera(camera, 64, 64);
        const float black[4] = {0, 0, 0, 1};
        ctx.Clear(rhi::kClearColor | rhi::kClearDepth | rhi::kClearStencil, black, 1.0f, 0, nullptr);
        // Texture: white, alpha 0, 36, 73, ... 255 (8 texels), bilinear, clamped.
        std::vector<std::uint32_t> ramp(8);
        for (std::uint32_t i = 0; i < 8; ++i) {
            ramp[i] = 0x00FFFFFFu | (static_cast<std::uint32_t>(i * 255 / 7) << 24);
        }
        rhi::TextureDesc td;
        td.width = 8;
        td.height = 1;
        td.format = rhi::Format::BGRA8_UNorm;
        const rhi::SubresourceData level{ramp.data(), 32, 32};
        const rhi::TextureHandle texture = device->CreateTexture(td, &level);
        REQUIRE(texture);
        // Slightly rotated quad so the cut line crosses pixels at sub-pixel positions.
        const float positions[12] = {-1.0f, 1.0f, 0.5f, 1.0f, 1.1f, 0.5f, -1.0f, -1.0f, 0.5f, 1.0f, -0.9f, 0.5f};
        const float uvs[8] = {0, 0, 1, 0, 0, 1, 1, 1};
        ni::DrawItem item;
        item.geometry.vertexCount = 4;
        item.geometry.positions = {positions, 12};
        item.geometry.uvs[0] = {uvs, 8};
        item.geometry.uvSetCount = 1;
        item.geometry.topology = ni::Topology::TriangleStrip;
        item.state.cullMode = rhi::CullMode::None;
        item.state.zFunc = rhi::CompareFunc::Always;
        item.state.zWriteEnable = false;
        ni::TextureStageDesc& s = item.state.stages[0];
        s.colorOp = FF_TOP_SELECTARG1;
        s.colorArg1 = FF_TA_TEXTURE;
        s.alphaOp = FF_TOP_SELECTARG1;
        s.alphaArg1 = FF_TA_TEXTURE;
        s.texture = texture;
        s.addressU = s.addressV = rhi::AddressMode::Clamp;
        s.minFilter = s.magFilter = rhi::Filter::Linear;
        s.mipFilter = rhi::MipFilter::None;
        item.state.alphaTestEnable = true;
        item.state.alphaFunc = rhi::CompareFunc::Greater;
        item.state.alphaRef = 127;
        REQUIRE(pipeline.Draw(item) == ff::DrawResult::Drawn);
        CHECK(pipeline.Stats().alphaToCoverageDraws == static_cast<std::uint64_t>(a2c));
        CHECK(pipeline.Stats().alphaTestDiscardDraws == static_cast<std::uint64_t>(1 - a2c));
        rhi::Image image;
        REQUIRE(device->CaptureBackBuffer(image));
        CHECK(At(image, 2, 32).g == 0);     // alpha ~0: cut away
        CHECK(At(image, 61, 32).g == 255);  // alpha ~1: drawn
        intermediate[a2c] = IntermediatePixels(image, 0, 255);
        for (int x = 0; x < 64; ++x) {
            if (At(image, static_cast<std::uint32_t>(x), 32).g >= 128) {
                edgeColumn[a2c] = x;
                break;
            }
        }
        CHECK(DebugProblems(*device) == 0);
        pipeline.Shutdown();
        device->Destroy(texture);
    }
    MESSAGE("partially covered pixels: discard " << intermediate[0] << ", alpha to coverage " << intermediate[1]
                                                 << "; 50 % column " << edgeColumn[0] << " / " << edgeColumn[1]);
    CHECK(intermediate[0] <= 70);       // only the slanted geometric edges of the quad itself
    CHECK(intermediate[1] > intermediate[0] + 30);  // + the alpha-tested cut line
    CHECK(std::abs(edgeColumn[0] - edgeColumn[1]) <= 1);  // the cut stays where the alpha test puts it
}

TEST_CASE("aa: point-filtered textures magnified by 4.5 keep equal texels (anti-aliased nearest, WARP)") {
    // A 4x1 texture black / white / black / white over an 18-pixel wide target = 4.5 pixels per texel (the 640x480
    // intro screens at 2880x2160). Nearest gives columns of 4 and 5 pixels; anti-aliased nearest gives 4 flat
    // pixels per texel plus one 50 % seam pixel on the borders that fall on a pixel center, so every texel keeps
    // its area of 4.5 pixels.
    for (int sharp = 0; sharp < 2; ++sharp) {
        auto device = MakeDevice(18, 4, 1);
        if (device == nullptr) {
            return;
        }
        ff::FixedFunctionPipeline pipeline;
        std::string error;
        REQUIRE_MESSAGE(pipeline.Init(*device, &error), error);
        ff::RenderOptions options;
        options.sharpNearest = sharp != 0;
        pipeline.SetRenderOptions(options);
        ni::CameraDesc camera;
        camera.view = camera.projection = camera.screenProjection = IdentityMatrix();
        camera.viewport = {0.0f, 0.0f, 18.0f, 4.0f, 0.0f, 1.0f};
        rhi::CommandContext& ctx = device->Context();
        ctx.SetRenderTargets({}, {});
        ctx.SetViewport(camera.viewport);
        pipeline.SetCamera(camera, 18, 4);
        const std::uint32_t texels[4] = {0xFF000000u, 0xFFFFFFFFu, 0xFF000000u, 0xFFFFFFFFu};
        rhi::TextureDesc td;
        td.width = 4;
        td.height = 1;
        td.format = rhi::Format::BGRA8_UNorm;
        const rhi::SubresourceData level{texels, 16, 16};
        const rhi::TextureHandle texture = device->CreateTexture(td, &level);
        REQUIRE(texture);
        const float positions[12] = {-1, 1, 0.5f, 1, 1, 0.5f, -1, -1, 0.5f, 1, -1, 0.5f};
        const float uvs[8] = {0, 0, 1, 0, 0, 1, 1, 1};
        ni::DrawItem item;
        item.geometry.vertexCount = 4;
        item.geometry.positions = {positions, 12};
        item.geometry.uvs[0] = {uvs, 8};
        item.geometry.uvSetCount = 1;
        item.geometry.topology = ni::Topology::TriangleStrip;
        item.state.cullMode = rhi::CullMode::None;
        item.state.zFunc = rhi::CompareFunc::Always;
        ni::TextureStageDesc& s = item.state.stages[0];
        s.colorOp = FF_TOP_SELECTARG1;
        s.colorArg1 = FF_TA_TEXTURE;
        s.alphaOp = FF_TOP_SELECTARG1;
        s.alphaArg1 = FF_TA_TEXTURE;
        s.texture = texture;
        s.addressU = s.addressV = rhi::AddressMode::Clamp;
        s.minFilter = s.magFilter = rhi::Filter::Point;
        s.mipFilter = rhi::MipFilter::None;
        REQUIRE(pipeline.Draw(item) == ff::DrawResult::Drawn);
        CHECK(pipeline.Stats().sharpNearestStages == static_cast<std::uint64_t>(sharp));
        rhi::Image image;
        REQUIRE(device->CaptureBackBuffer(image));
        std::vector<int> row(18);
        for (std::uint32_t x = 0; x < 18; ++x) {
            row[x] = At(image, x, 2).g;
        }
        INFO("row " << row[0] << " " << row[1] << " " << row[2] << " " << row[3] << " " << row[4] << " " << row[5]
                    << " ... " << row[13] << " " << row[14]);
        for (std::uint32_t x : {0u, 1u, 2u, 3u, 9u, 10u, 11u, 12u}) {
            CHECK(row[x] == 0);
        }
        for (std::uint32_t x : {5u, 6u, 7u, 8u, 14u, 15u, 16u, 17u}) {
            CHECK(row[x] == 255);
        }
        if (sharp != 0) {
            CHECK(std::abs(row[4] - 128) <= 2);  // texel border 4.5 on the pixel center: half and half
            CHECK(std::abs(row[13] - 128) <= 2);
            const int sum = std::accumulate(row.begin(), row.end(), 0);
            CHECK(std::abs(sum - 9 * 255) <= 4);  // two white texels of 4.5 pixels each
        } else {
            CHECK((row[4] == 0 || row[4] == 255));  // nearest: the border pixel goes to one texel
        }
        CHECK(DebugProblems(*device) == 0);
        pipeline.Shutdown();
        device->Destroy(texture);
    }
}

TEST_CASE("aa: GPU frame timing over many frames without debug-layer warnings (headless WARP)") {
    // More frames than the timer ring has slots (6), presents without a swap chain: the queries must be read
    // before they are restarted (debug-layer warning #408 otherwise) and frames must get timed.
    auto device = MakeDevice(64, 48, 4);
    if (device == nullptr) {
        return;
    }
    const float color[4] = {0.5f, 0.25f, 0.0f, 1.0f};
    for (int frame = 0; frame < 40; ++frame) {
        device->Context().Clear(rhi::kClearColor | rhi::kClearDepth, color, 1.0f, 0, nullptr);
        device->Present(rhi::PresentDesc{0});
    }
    const rhi::DeviceStats stats = device->Stats();
    MESSAGE("gpu frames timed " << stats.gpuFramesTimed << ", average " << stats.gpuFrameMsAverage << " ms, p99 "
                                << stats.gpuFrameMsP99 << " ms");
    CHECK(stats.gpuFramesTimed > 0);
    CHECK(stats.gpuFrameMsAverage >= 0.0);
    CHECK(stats.gpuFrameMsP99 <= stats.gpuFrameMsMax + 0.1);
    CHECK(DebugProblems(*device) == 0);
}

TEST_CASE("aa: present pass into a window - 1:1, upscale and downsample (WARP)") {
    REQUIRE(platform::MainWindow() == nullptr);
    platform::WindowOptions options;
    options.scale = 1;
    options.title = L"fut_tests aa window";
    const char* error = nullptr;
    REQUIRE_MESSAGE(platform::CreateMainWindow(options, &error), (error != nullptr ? error : ""));
    std::uint32_t vw = 0, vh = 0;
    platform::MainWindowClientSize(&vw, &vh);
    REQUIRE(vw > 0);
    // Output = view (1:1), smaller (upscale), larger with an integer ratio and a fractional one (downsample).
    const std::uint32_t sizes[][2] = {{vw, vh}, {vw / 2, vh / 2}, {vw * 2, vh * 2}, {vw * 3 / 2, vh * 3 / 2}};
    for (const auto& size : sizes) {
        for (int variant = 0; variant < 3; ++variant) {
            // Linear and integer scaling; linear scaling once more with [render] linear_aa (the sRGB view of the
            // swap chain buffer for the downsample in the present pass, review M3c finding 3).
            const rhi::PresentScaling scaling = variant == 1 ? rhi::PresentScaling::Integer : rhi::PresentScaling::Linear;
            const bool linearResolve = variant == 2;
            INFO("output " << size[0] << "x" << size[1] << ", view " << vw << "x" << vh << ", scaling "
                           << rhi::PresentScalingName(scaling) << ", linear resolve " << linearResolve);
            rhi::DeviceDesc d;
            d.backend = rhi::Backend::D3D11;
            d.softwareAdapter = true;
            d.debugLayer = true;
            d.nativeWindow = platform::MainWindow();
            d.width = size[0];
            d.height = size[1];
            d.sampleCount = 4;
            d.scaling = scaling;
            d.linearResolve = linearResolve;
            std::string reason;
            auto device = rhi::CreateDevice(d, &reason);
            REQUIRE_MESSAGE(device != nullptr, reason);
            const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
            for (int n = 0; n < 2; ++n) {
                device->Context().Clear(rhi::kClearColor | rhi::kClearDepth, color, 1.0f, 0, nullptr);
                const rhi::PresentResult r = device->Present(rhi::PresentDesc{0});
                CHECK((r == rhi::PresentResult::Ok || r == rhi::PresentResult::Occluded));
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    DispatchMessageW(&msg);
                }
            }
            CHECK(DebugProblems(*device) == 0);
        }
    }
    platform::DestroyMainWindow();
}

TEST_CASE("aa: full-screen 2D covers the upscaled multisampled 4:3 area to its edges (edge seam, WARP)") {
    // User report 2026-10-15 (2880x2160 -> 2880x1620 at 16:9, MSAA 8x): movies, fades and letterbox bars showed an
    // old picture in the first two rows and columns of the centred 4:3 area. The guest's full-screen quads start on
    // integer D3D8 pixels; with the B19 shift of 0.5 guest pixels they started 0.5 * scale device pixels inside the
    // area. gfx::PixelCenterShift keeps 0.5 only at scale 1 with one sample. Scale 3.375 like the user's 1620 lines:
    // guest 64 x 48 -> a 216 x 162 area at x = 36 of a 288 x 162 (16:9) target.
    struct Case {
        std::uint32_t w, h, samples, offsetX;
    };
    static const std::uint16_t kIndices[6] = {0, 1, 2, 2, 1, 3};
    auto quad = [](float qx0, float qx1) {
        return std::vector<float>{qx0, 0.0f, 0.5f, qx1, 0.0f, 0.5f, qx0, 48.0f, 0.5f, qx1, 48.0f, 0.5f};
    };
    auto item = [](const std::vector<float>& positions, const float* colors, bool pretransformed) {
        ni::DrawItem it;
        it.geometry.vertexCount = 4;
        it.geometry.positions = {positions.data(), 12};
        it.geometry.colors = {colors, 16};
        it.geometry.indices = kIndices;
        it.geometry.indexCount = 6;
        it.geometry.pretransformed = pretransformed;
        it.state.zFunc = rhi::CompareFunc::Always;
        it.state.zWriteEnable = false;
        it.state.cullMode = rhi::CullMode::None;
        it.state.lightingEnable = false;  // vertex colours (the game's screen polygons are unlit)
        return it;
    };
    const float green[16] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    const float half[16] = {0, 0.5f, 0, 1, 0, 0.5f, 0, 1, 0, 0.5f, 0, 1, 0, 0.5f, 0, 1};
    const std::vector<float> ndc = {-1, 1, 0.5f, 1, 1, 0.5f, -1, -1, 0.5f, 1, -1, 0.5f};
    for (const Case cs : {Case{288, 162, 8, 36}, Case{288, 162, 1, 36}, Case{64, 48, 1, 0}, Case{64, 48, 4, 0}}) {
        CAPTURE(cs.w);
        CAPTURE(cs.samples);
        auto device = MakeDevice(cs.w, cs.h, cs.samples);
        if (device == nullptr) {
            return;
        }
        const std::uint32_t samples = device->Info().sampleCount;
        const double scale = static_cast<double>(cs.h) / 48.0;
        const float rule = PixelCenterShift(scale, samples);
        CHECK(rule == (scale == 1.0 && samples == 1 ? 0.5f : 0.0f));
        ff::FixedFunctionPipeline pipeline;
        std::string error;
        REQUIRE_MESSAGE(pipeline.Init(*device, &error), error);
        ff::RenderOptions options;
        options.sampleCount = samples;
        pipeline.SetRenderOptions(options);
        rhi::CommandContext& ctx = device->Context();
        ctx.SetRenderTargets({}, {});
        const auto areaW = static_cast<std::uint32_t>(64 * scale);
        const std::uint32_t x0 = cs.offsetX, x1 = cs.offsetX + areaW;
        const std::vector<float> fullQuad = quad(0.0f, 64.0f);
        for (const float shift : {0.5f, rule}) {
            CAPTURE(shift);
            ni::CameraDesc camera;
            camera.view = camera.projection = camera.screenProjection = IdentityMatrix();
            camera.viewport = {0.0f, 0.0f, 64.0f, 48.0f, 0.0f, 1.0f};
            HalfPixelShift(&camera.projection, 64.0f, 48.0f, shift, shift);
            pipeline.SetCamera(camera, 64, 48, shift, shift);
            ctx.SetViewport({static_cast<float>(x0), 0.0f, static_cast<float>(areaW), static_cast<float>(cs.h), 0, 1});
            const bool exact = shift == rule;
            // Pre-transformed (slot 55, NiScreenTexture) and transformed over the viewport (the movie NiScreenPolygon
            // of the slot-54 screen camera, fades).
            for (int kind = 0; kind < 2; ++kind) {
                CAPTURE(kind);
                const float red[4] = {1, 0, 0, 1};  // "the picture of two presents ago"
                ctx.Clear(rhi::kClearColor | rhi::kClearDepth | rhi::kClearStencil, red, 1.0f, 0, nullptr);
                REQUIRE(pipeline.Draw(item(kind == 0 ? fullQuad : ndc, green, kind == 0)) == ff::DrawResult::Drawn);
                rhi::Image image;
                REQUIRE(device->CaptureBackBuffer(image));
                auto off = [&image](std::uint32_t x, std::uint32_t y) {
                    return At(image, x, y).r != 0 || At(image, x, y).g != 255 ? 1 : 0;
                };
                int uncovered = 0;
                for (std::uint32_t y = 0; y < cs.h; ++y) {
                    uncovered += off(x0, y) + off(x0 + 1, y) + off(x1 - 2, y) + off(x1 - 1, y);
                }
                for (std::uint32_t x = x0; x < x1; ++x) {
                    uncovered += off(x, 0) + off(x, 1) + off(x, cs.h - 2) + off(x, cs.h - 1);
                }
                if (exact) {
                    CHECK(uncovered == 0);
                } else {
                    CHECK(uncovered > 0);  // the old shift on an upscaled or multisampled target: the seam
                }
                if (x0 > 0) {  // nothing spills into the bars
                    CHECK(At(image, x0 - 1, cs.h / 2).r == 255);
                    CHECK(At(image, x1, cs.h / 2).r == 255);
                }
            }
            if (!exact) {
                continue;
            }
            // Tiled 2D pieces (HUD frames, letterbox bars beside the movie): additive halves meet without a gap or a
            // double-covered column, also where the shared edge (guest 21 -> 70.875 device pixels) splits a pixel.
            const float black[4] = {0, 0, 0, 1};
            ctx.Clear(rhi::kClearColor | rhi::kClearDepth | rhi::kClearStencil, black, 1.0f, 0, nullptr);
            for (const auto& [a, b] : {std::pair{0.0f, 21.0f}, std::pair{21.0f, 64.0f}}) {
                const std::vector<float> positions = quad(a, b);
                ni::DrawItem tile = item(positions, half, true);
                tile.state.alphaBlendEnable = true;
                tile.state.srcBlend = rhi::BlendFactor::One;
                tile.state.dstBlend = rhi::BlendFactor::One;
                REQUIRE(pipeline.Draw(tile) == ff::DrawResult::Drawn);
            }
            rhi::Image image;
            REQUIRE(device->CaptureBackBuffer(image));
            int wrong = 0;
            for (std::uint32_t y = 0; y < cs.h; y += 7) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    wrong += std::abs(At(image, x, y).g - 128) > 2 ? 1 : 0;
                }
            }
            CHECK(wrong == 0);
        }
        CHECK(DebugProblems(*device) == 0);
        pipeline.Shutdown();
    }
}
