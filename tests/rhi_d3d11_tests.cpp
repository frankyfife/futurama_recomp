// Unit tests of the D3D11 RHI backend (gfx/rhi/d3d11/**, GFX_CONTRACT.md 5.4) on a headless WARP device:
// resources, clears (whole / rectangle, color / depth / stencil), drawing with the backend's diagnostic
// shaders, cull mapping, strip cut, transient ring, sampler/pipeline caches, texture upload/read-back round
// trips and sampling channel order for every RHI format, constant buffers, debug-layer messages, teardown.
// Every device runs with the D3D11 debug layer when it is installed; each test ends with 0 debug errors
// unless it provokes one on purpose.
#include "doctest.h"

#include "gfx/rhi/d3d11/d3d11_device.h"
#include "gfx/rhi/d3d11/d3d11_internal.h"
#include "gfx/rhi/rhi.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace gfx::rhi;

namespace {

struct DiagVertex {
    float x, y, z;
    float r, g, b, a;
    float u, v;
};
static_assert(sizeof(DiagVertex) == d3d11::kDiagVertexStride);

std::unique_ptr<Device> MakeDevice(std::uint32_t w = 64, std::uint32_t h = 64, bool debug = true) {
    DeviceDesc d;
    d.backend = Backend::D3D11;
    d.softwareAdapter = true;
    d.debugLayer = debug;
    d.width = w;
    d.height = h;
    std::string error;
    auto device = CreateDevice(d, &error);
    if (device == nullptr) {
        MESSAGE("CreateDevice failed: " << error);
    }
    return device;
}

// Prints and counts the debug messages since the last call (0 expected unless a test provokes one).
std::size_t DebugErrors(Device& device) {
    std::vector<DebugMessage> messages;
    device.DrainDebugMessages(messages);
    std::size_t errors = 0;
    for (const DebugMessage& m : messages) {
        MESSAGE("debug message " << static_cast<int>(m.severity) << " #" << m.id << ": " << m.text);
        if (m.severity == DebugSeverity::Error || m.severity == DebugSeverity::Corruption) {
            ++errors;
        }
    }
    return errors;
}

// 0xAARRGGBB of a BGRA8 image pixel.
std::uint32_t Pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = image.pixels.data() + static_cast<std::size_t>(y) * image.rowPitch + x * 4;
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

bool Near(std::uint32_t argb, std::uint32_t expected, int tolerance = 1) {
    for (int shift = 0; shift < 32; shift += 8) {
        const int a = static_cast<int>((argb >> shift) & 0xFF);
        const int b = static_cast<int>((expected >> shift) & 0xFF);
        if (a - b > tolerance || b - a > tolerance) {
            return false;
        }
    }
    return true;
}

struct Diag {
    ShaderHandle vs, colorPs, texturePs, tintPs;

    bool Init(Device& device) {
        vs = device.CreateShader(ShaderStage::Vertex, d3d11::BuiltinShaderCode(d3d11::BuiltinShader::DiagVS), "diag vs");
        colorPs = device.CreateShader(ShaderStage::Pixel, d3d11::BuiltinShaderCode(d3d11::BuiltinShader::DiagColorPS),
                                      "diag color ps");
        texturePs = device.CreateShader(ShaderStage::Pixel,
                                        d3d11::BuiltinShaderCode(d3d11::BuiltinShader::DiagTexturePS), "diag tex ps");
        tintPs = device.CreateShader(ShaderStage::Pixel, d3d11::BuiltinShaderCode(d3d11::BuiltinShader::DiagTintPS),
                                     "diag tint ps");
        return vs && colorPs && texturePs && tintPs;
    }

    PipelineDesc Desc(ShaderHandle ps) const {
        PipelineDesc p;
        p.vertexShader = vs;
        p.pixelShader = ps;
        p.layout = d3d11::DiagVertexLayout();
        p.depthStencil.depthEnable = false;
        return p;
    }
};

// Uploads `vertices` into the transient ring and binds them to stream 0.
bool BindVertices(Device& device, const DiagVertex* vertices, std::size_t count) {
    CommandContext& ctx = device.Context();
    const auto bytes = static_cast<std::uint32_t>(count * sizeof(DiagVertex));
    const TransientSpan span = ctx.BeginTransient(TransientKind::Vertex, bytes, 16);
    if (span.cpu == nullptr) {
        return false;
    }
    std::memcpy(span.cpu, vertices, bytes);
    ctx.EndTransient();
    ctx.SetVertexBuffer(0, span.buffer, span.offset, sizeof(DiagVertex));
    return true;
}

// Full-target quad (two triangles) at depth z with color c and UVs 0..1.
std::vector<DiagVertex> Quad(float z, float r, float g, float b, float a) {
    return {{-1, 1, z, r, g, b, a, 0, 0}, {1, 1, z, r, g, b, a, 1, 0},  {-1, -1, z, r, g, b, a, 0, 1},
            {-1, -1, z, r, g, b, a, 0, 1}, {1, 1, z, r, g, b, a, 1, 0}, {1, -1, z, r, g, b, a, 1, 1}};
}

Image Capture(Device& device) {
    Image image;
    REQUIRE(device.CaptureBackBuffer(image));
    REQUIRE(image.format == Format::BGRA8_UNorm);
    return image;
}

const float kBlack[4] = {0, 0, 0, 1};

}  // namespace

TEST_CASE("rhi d3d11: pure translations") {
    CHECK(d3d11::DxgiFormatOf(Format::BGRA8_UNorm) == DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK(d3d11::DxgiFormatOf(Format::B5G6R5_UNorm) == DXGI_FORMAT_B5G6R5_UNORM);
    CHECK(d3d11::DxgiFormatOf(Format::BC3_UNorm) == DXGI_FORMAT_BC3_UNORM);
    CHECK(d3d11::DxgiFormatOf(Format::D24_UNorm_S8_UInt) == DXGI_FORMAT_D24_UNORM_S8_UINT);
    CHECK(d3d11::DxgiFormatOf(Format::Unknown) == DXGI_FORMAT_UNKNOWN);
    for (int f = 1; f < static_cast<int>(Format::Count); ++f) {
        CHECK(d3d11::DxgiFormatOf(static_cast<Format>(f)) != DXGI_FORMAT_UNKNOWN);
    }
    CHECK(d3d11::PresentResultFromHresult(S_OK) == PresentResult::Ok);
    CHECK(d3d11::PresentResultFromHresult(DXGI_STATUS_OCCLUDED) == PresentResult::Occluded);
    CHECK(d3d11::PresentResultFromHresult(DXGI_ERROR_DEVICE_REMOVED) == PresentResult::DeviceLost);
    CHECK(d3d11::PresentResultFromHresult(DXGI_ERROR_DEVICE_RESET) == PresentResult::DeviceLost);
    CHECK(d3d11::PresentResultFromHresult(DXGI_ERROR_DEVICE_HUNG) == PresentResult::DeviceLost);
    CHECK(d3d11::PresentResultFromHresult(DXGI_ERROR_INVALID_CALL) == PresentResult::Failed);
    CHECK(d3d11::HresultText(DXGI_ERROR_DEVICE_REMOVED) == "0x887A0005 DXGI_ERROR_DEVICE_REMOVED");
    CHECK(d3d11::HresultText(0x12345678) == "0x12345678");

    // Cull mapping (GFX_CONTRACT.md 5.3): CW -> FrontCounterClockwise = TRUE + CULL_BACK.
    RasterDesc r;
    r.cull = CullMode::CW;
    CHECK(d3d11::RasterDescOf(r).CullMode == D3D11_CULL_BACK);
    CHECK(d3d11::RasterDescOf(r).FrontCounterClockwise == TRUE);
    r.cull = CullMode::CCW;
    CHECK(d3d11::RasterDescOf(r).FrontCounterClockwise == FALSE);
    r.cull = CullMode::None;
    CHECK(d3d11::RasterDescOf(r).CullMode == D3D11_CULL_NONE);
    // Alpha blend factors never use *_COLOR (D3D11 rejects them).
    BlendDesc b;
    b.srcAlpha = BlendFactor::SrcColor;
    b.dstAlpha = BlendFactor::InvDestColor;
    CHECK(d3d11::BlendDescOf(b).RenderTarget[0].SrcBlendAlpha == D3D11_BLEND_SRC_ALPHA);
    CHECK(d3d11::BlendDescOf(b).RenderTarget[0].DestBlendAlpha == D3D11_BLEND_INV_DEST_ALPHA);
    // MipFilter::None samples level 0 only.
    SamplerDesc s;
    s.minFilter = Filter::Point;
    s.magFilter = Filter::Linear;
    s.mipFilter = MipFilter::None;
    CHECK(d3d11::SamplerDescOf(s).Filter == D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT);
    CHECK(d3d11::SamplerDescOf(s).MaxLOD == 0.0f);
    s.mipFilter = MipFilter::Linear;
    s.minFilter = Filter::Linear;
    CHECK(d3d11::SamplerDescOf(s).Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR);
}

TEST_CASE("rhi d3d11: headless WARP device") {
    auto device = MakeDevice(64, 48);
    REQUIRE(device != nullptr);
    const DeviceInfo& info = device->Info();
    CHECK(info.backend == Backend::D3D11);
    CHECK(info.headless);
    CHECK(info.softwareAdapter);
    CHECK(info.backBufferWidth == 64);
    CHECK(info.backBufferHeight == 48);
    CHECK(info.backBufferFormat == Format::BGRA8_UNorm);
    CHECK(info.depthFormat == Format::D24_UNorm_S8_UInt);
    CHECK(info.apiVersion.find("D3D11 feature level") == 0);
    CHECK_FALSE(info.adapterName.empty());
    CHECK(device->NativeShaderFormat() == ShaderFormat::Dxbc);
    const DebugLayerInfo layer = device->DebugLayer();
    CHECK(layer.requested);
    if (layer.enabled) {
        CHECK(layer.available);
    } else {
        MESSAGE("D3D11 debug layer not enabled: " << layer.note);
        CHECK_FALSE(layer.note.empty());
    }
    // Required formats (GFX_CONTRACT.md 5.2); the 16-bit ones are optional.
    for (Format f : {Format::RGBA8_UNorm, Format::BGRA8_UNorm, Format::BC1_UNorm, Format::BC2_UNorm, Format::BC3_UNorm,
                     Format::A8_UNorm, Format::R8_UNorm}) {
        CHECK_MESSAGE(device->IsFormatSupported(f, kTextureSampled), GetFormatInfo(f).name);
    }
    CHECK(device->IsFormatSupported(Format::D24_UNorm_S8_UInt, kTextureDepthStencil));
    CHECK_FALSE(device->IsFormatSupported(Format::BC1_UNorm, kTextureRenderTarget));
    CHECK_FALSE(device->IsFormatSupported(Format::D32_Float, kTextureRenderTarget));
    CHECK_FALSE(device->IsFormatSupported(Format::Unknown, kTextureSampled));
    CHECK(device->Present(PresentDesc{}) == PresentResult::Ok);
    CHECK(device->Stats().presents == 1);
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: whole and rectangle color clears") {
    auto device = MakeDevice(64, 64);
    REQUIRE(device != nullptr);
    CommandContext& ctx = device->Context();
    const float blue[4] = {0, 0, 1, 1};
    const float red[4] = {1, 0, 0, 1};
    ctx.Clear(kClearColor, blue, 1.0f, 0, nullptr);
    Rect rect{10, 20, 30, 40};
    ctx.Clear(kClearColor, red, 1.0f, 0, &rect);
    Rect clipped{-5, -5, 5, 5};  // clipped to the target (D3D8 clips clear rectangles)
    ctx.Clear(kClearColor, red, 1.0f, 0, &clipped);
    Rect empty{40, 40, 40, 50};
    ctx.Clear(kClearColor, red, 1.0f, 0, &empty);
    const Image img = Capture(*device);
    CHECK(img.width == 64);
    CHECK(img.height == 64);
    CHECK(Pixel(img, 10, 20) == 0xFFFF0000u);
    CHECK(Pixel(img, 29, 39) == 0xFFFF0000u);
    CHECK(Pixel(img, 9, 20) == 0xFF0000FFu);
    CHECK(Pixel(img, 30, 39) == 0xFF0000FFu);
    CHECK(Pixel(img, 29, 40) == 0xFF0000FFu);
    CHECK(Pixel(img, 0, 0) == 0xFFFF0000u);
    CHECK(Pixel(img, 4, 4) == 0xFFFF0000u);
    CHECK(Pixel(img, 5, 5) == 0xFF0000FFu);
    CHECK(Pixel(img, 40, 40) == 0xFF0000FFu);
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: draw a triangle, cull mapping and the strip cut") {
    auto device = MakeDevice(64, 64);
    REQUIRE(device != nullptr);
    Diag diag;
    REQUIRE(diag.Init(*device));
    CommandContext& ctx = device->Context();

    // Clip-space triangle bottom-left -> top-middle -> bottom-right: clockwise on the screen (y down).
    const DiagVertex cw[3] = {{-1, -1, 0.5f, 1, 0, 0, 1, 0, 0}, {0, 1, 0.5f, 1, 0, 0, 1, 0, 0},
                              {1, -1, 0.5f, 1, 0, 0, 1, 0, 0}};
    const DiagVertex ccw[3] = {cw[0], cw[2], cw[1]};
    struct Case {
        CullMode cull;
        const DiagVertex* tri;
        bool drawn;
    };
    const Case cases[] = {{CullMode::None, cw, true},  {CullMode::None, ccw, true}, {CullMode::CW, cw, false},
                          {CullMode::CW, ccw, true},   {CullMode::CCW, cw, true},  {CullMode::CCW, ccw, false}};
    for (const Case& c : cases) {
        PipelineDesc p = diag.Desc(diag.colorPs);
        p.raster.cull = c.cull;
        const PipelineHandle pipeline = device->GetPipeline(p, "cull test");
        REQUIRE(pipeline);
        ctx.Clear(kClearColor, kBlack, 1.0f, 0, nullptr);
        ctx.SetPipeline(pipeline);
        REQUIRE(BindVertices(*device, c.tri, 3));
        ctx.Draw(3, 0);
        const Image img = Capture(*device);
        CHECK_MESSAGE(Pixel(img, 32, 40) == (c.drawn ? 0xFFFF0000u : 0xFF000000u),
                      "cull " << static_cast<int>(c.cull) << (c.tri == cw ? " cw" : " ccw"));
        CHECK(Pixel(img, 1, 1) == 0xFF000000u);  // outside the triangle
        device->Present(PresentDesc{});
    }

    // Strip cut: two separate triangles left and right; without the cut 0xFFFF the strip would add two
    // triangles bridging the middle.
    const DiagVertex strip[6] = {
        {-1, -1, 0, 0, 1, 0, 1, 0, 0},   {-1, 1, 0, 0, 1, 0, 1, 0, 0},   {-0.4f, -1, 0, 0, 1, 0, 1, 0, 0},
        {0.4f, -1, 0, 0, 1, 0, 1, 0, 0}, {1, 1, 0, 0, 1, 0, 1, 0, 0},    {1, -1, 0, 0, 1, 0, 1, 0, 0}};
    PipelineDesc p = diag.Desc(diag.colorPs);
    p.topology = PrimitiveTopology::TriangleStrip;
    const PipelineHandle stripPipeline = device->GetPipeline(p, "strip test");
    REQUIRE(stripPipeline);
    const std::uint16_t indices[7] = {0, 1, 2, kStripCut, 3, 4, 5};
    BufferDesc ib;
    ib.size = sizeof(indices);
    ib.bind = kBindIndex;
    ib.initialData = indices;
    ib.debugName = "strip indices";
    const BufferHandle indexBuffer = device->CreateBuffer(ib);
    REQUIRE(indexBuffer);
    ctx.Clear(kClearColor, kBlack, 1.0f, 0, nullptr);
    ctx.SetPipeline(stripPipeline);
    REQUIRE(BindVertices(*device, strip, 6));
    ctx.SetIndexBuffer(indexBuffer, 0);
    ctx.DrawIndexed(7, 0, 0);
    const Image img = Capture(*device);
    CHECK(Pixel(img, 3, 60) == 0xFF00FF00u);   // left triangle
    CHECK(Pixel(img, 61, 60) == 0xFF00FF00u);  // right triangle
    CHECK(Pixel(img, 32, 40) == 0xFF000000u);  // the bridge stays empty
    CHECK(device->Stats().draws == 7);
    device->Destroy(indexBuffer);
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: rectangle depth and stencil clears restore the pipeline") {
    auto device = MakeDevice(64, 64);
    REQUIRE(device != nullptr);
    Diag diag;
    REQUIRE(diag.Init(*device));
    CommandContext& ctx = device->Context();

    PipelineDesc p = diag.Desc(diag.colorPs);
    p.depthStencil.depthEnable = true;
    p.depthStencil.depthFunc = CompareFunc::LessEqual;
    p.depthStencil.depthWrite = false;
    const PipelineHandle depthTest = device->GetPipeline(p, "depth test");
    REQUIRE(depthTest);
    ctx.SetPipeline(depthTest);
    ctx.Clear(kClearColor | kClearDepth | kClearStencil, kBlack, 1.0f, 0, nullptr);
    Rect rect{16, 16, 48, 48};
    ctx.Clear(kClearDepth, kBlack, 0.25f, 0, &rect);
    const auto quad = Quad(0.5f, 1, 0, 0, 1);
    REQUIRE(BindVertices(*device, quad.data(), quad.size()));
    ctx.Draw(6, 0);  // pipeline restored after the clear draw: depth test LESSEQUAL against 1.0 / 0.25
    Image img = Capture(*device);
    CHECK(Pixel(img, 2, 2) == 0xFFFF0000u);
    CHECK(Pixel(img, 15, 30) == 0xFFFF0000u);
    CHECK(Pixel(img, 16, 16) == 0xFF000000u);
    CHECK(Pixel(img, 47, 47) == 0xFF000000u);
    CHECK(Pixel(img, 48, 47) == 0xFFFF0000u);

    // Stencil only: whole 0, rectangle 7; draw where stencil == 7.
    PipelineDesc s = diag.Desc(diag.colorPs);
    s.depthStencil.depthEnable = false;
    s.depthStencil.stencilEnable = true;
    s.depthStencil.stencilFunc = CompareFunc::Equal;
    const PipelineHandle stencilTest = device->GetPipeline(s, "stencil test");
    REQUIRE(stencilTest);
    ctx.Clear(kClearColor | kClearStencil, kBlack, 1.0f, 0, nullptr);
    Rect r2{8, 8, 24, 24};
    ctx.Clear(kClearStencil, kBlack, 0.0f, 7, &r2);
    ctx.SetPipeline(stencilTest);
    ctx.SetStencilRef(7);
    const auto green = Quad(0.5f, 0, 1, 0, 1);
    REQUIRE(BindVertices(*device, green.data(), green.size()));
    ctx.Draw(6, 0);
    img = Capture(*device);
    CHECK(Pixel(img, 8, 8) == 0xFF00FF00u);
    CHECK(Pixel(img, 23, 23) == 0xFF00FF00u);
    CHECK(Pixel(img, 24, 24) == 0xFF000000u);
    CHECK(Pixel(img, 7, 8) == 0xFF000000u);
    // The rectangle stencil clear did not touch the depth buffer (still 0.25 inside the first rectangle).
    ctx.SetStencilRef(0);
    ctx.Clear(kClearColor | kClearStencil, kBlack, 1.0f, 0, nullptr);
    ctx.SetPipeline(depthTest);
    REQUIRE(BindVertices(*device, quad.data(), quad.size()));
    ctx.Draw(6, 0);
    img = Capture(*device);
    CHECK(Pixel(img, 20, 20) == 0xFF000000u);
    CHECK(Pixel(img, 2, 2) == 0xFFFF0000u);
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: texture upload and read-back round trip for every format") {
    auto device = MakeDevice(32, 32);
    REQUIRE(device != nullptr);
    std::mt19937 rng(1234);
    for (int fi = 1; fi < static_cast<int>(Format::Count); ++fi) {
        const auto format = static_cast<Format>(fi);
        const FormatInfo& info = GetFormatInfo(format);
        CAPTURE(info.name);
        if (info.depth) {
            continue;  // below
        }
        if (!device->IsFormatSupported(format, kTextureSampled)) {
            MESSAGE("format not supported by this device: " << info.name);
            continue;
        }
        // 8x8 with a full mip chain (8, 4, 2, 1); BC levels 2x2 and 1x1 are one block each.
        TextureDesc td;
        td.width = 8;
        td.height = 8;
        td.mipLevels = 4;
        td.format = format;
        td.debugName = info.name;
        std::vector<std::vector<std::uint8_t>> data(4);
        std::vector<SubresourceData> levels(4);
        for (std::uint32_t l = 0; l < 4; ++l) {
            const std::uint32_t w = 8u >> l;
            data[l].resize(LevelSize(format, w, w));
            for (auto& b : data[l]) {
                b = static_cast<std::uint8_t>(rng());
            }
            levels[l].data = data[l].data();
            levels[l].rowPitch = RowPitch(format, w);
        }
        const TextureHandle tex = device->CreateTexture(td, levels.data());
        REQUIRE_MESSAGE(tex, device->LastError());
        for (std::uint32_t l = 0; l < 4; ++l) {
            Image img;
            REQUIRE(device->ReadTexture(tex, l, img));
            CHECK(img.format == format);
            CHECK(img.width == (8u >> l));
            CHECK(img.rowPitch == RowPitch(format, 8u >> l));
            CHECK(img.pixels == data[l]);
        }
        // UpdateTexture of level 1.
        for (auto& b : data[1]) {
            b = static_cast<std::uint8_t>(rng());
        }
        SubresourceData update;
        update.data = data[1].data();
        CHECK(device->UpdateTexture(tex, 1, update));
        Image img;
        REQUIRE(device->ReadTexture(tex, 1, img));
        CHECK(img.pixels == data[1]);
        device->Destroy(tex);
    }
    CHECK(device->Stats().textures == 0);
    CHECK(device->Stats().textureBytes == 0);

    // Depth formats: no initial data; clear through a target, read back.
    for (Format format : {Format::D24_UNorm_S8_UInt, Format::D32_Float}) {
        CAPTURE(GetFormatInfo(format).name);
        TextureDesc td;
        td.width = 32;  // = the back buffer (D3D11 requires equal view sizes)
        td.height = 32;
        td.format = format;
        td.usage = kTextureDepthStencil;
        const std::uint8_t dummy[4] = {};
        SubresourceData sd;
        sd.data = dummy;
        CHECK_FALSE(device->CreateTexture(td, &sd));  // depth takes no initial data
        const TextureHandle depth = device->CreateTexture(td, nullptr);
        REQUIRE_MESSAGE(depth, device->LastError());
        device->Context().SetRenderTargets({}, depth);
        device->Context().Clear(kClearDepth | kClearStencil, kBlack, 0.25f, 0x5A, nullptr);
        Image img;
        REQUIRE(device->ReadTexture(depth, 0, img));
        std::uint32_t v;
        std::memcpy(&v, img.pixels.data() + 4 * (5 * 32 + 7), 4);
        if (format == Format::D24_UNorm_S8_UInt) {
            CHECK((v >> 24) == 0x5Au);
            const std::uint32_t d24 = v & 0xFFFFFF;
            CHECK(d24 >= 0x3FFFFEu);
            CHECK(d24 <= 0x400000u);
        } else {
            float f;
            std::memcpy(&f, &v, 4);
            CHECK(f == 0.25f);
        }
        device->Context().SetRenderTargets({}, {});
        device->Destroy(depth);
        // Sampled depth (typeless resource + SRV + DSV).
        td.usage = kTextureDepthStencil | kTextureSampled;
        const TextureHandle sampled = device->CreateTexture(td, nullptr);
        CHECK_MESSAGE(sampled, device->LastError());
        device->Destroy(sampled);
    }
    // Rejected descriptions.
    TextureDesc bad;
    bad.width = 6;
    bad.height = 8;
    bad.format = Format::BC1_UNorm;
    CHECK_FALSE(device->CreateTexture(bad, nullptr));  // BC level 0 not a multiple of 4
    bad.width = 8;
    bad.mipLevels = 5;
    CHECK_FALSE(device->CreateTexture(bad, nullptr));  // more levels than 8x8 has
    bad.mipLevels = 1;
    bad.usage = kTextureRenderTarget;
    CHECK_FALSE(device->CreateTexture(bad, nullptr));  // BC render target
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: sampling returns the D3D8 channel order of every color format") {
    auto device = MakeDevice(16, 16);
    REQUIRE(device != nullptr);
    Diag diag;
    REQUIRE(diag.Init(*device));
    CommandContext& ctx = device->Context();
    const PipelineHandle pipeline = device->GetPipeline(diag.Desc(diag.texturePs), "sample test");
    REQUIRE(pipeline);
    SamplerDesc sd;
    sd.minFilter = sd.magFilter = Filter::Point;
    sd.addressU = sd.addressV = AddressMode::Clamp;
    const SamplerHandle sampler = device->GetSampler(sd);
    REQUIRE(sampler);

    struct Case {
        Format format;
        std::vector<std::uint8_t> texel;  // one texel (BC: one 4x4 block), repeated
        std::uint32_t expected;           // 0xAARRGGBB on the back buffer
    };
    const Case cases[] = {
        {Format::BGRA8_UNorm, {0x10, 0x20, 0x30, 0xFF}, 0xFF302010u},
        {Format::RGBA8_UNorm, {0x30, 0x20, 0x10, 0xFF}, 0xFF302010u},
        {Format::BGRX8_UNorm, {0x10, 0x20, 0x30, 0x00}, 0xFF302010u},     // X sampled as alpha 1
        {Format::B5G6R5_UNorm, {0x1F, 0xF8}, 0xFFFF00FFu},                // 0xF81F: R 31, G 0, B 31
        {Format::B5G5R5A1_UNorm, {0x00, 0xFC}, 0xFFFF0000u},              // 0xFC00: A 1, R 31
        {Format::B5G5R5A1_UNorm, {0xE0, 0x03}, 0x0000FF00u},              // 0x03E0: A 0, G 31
        {Format::B4G4R4A4_UNorm, {0xF0, 0xF0}, 0xFF00FF00u},              // 0xF0F0: A 15, R 0, G 15, B 0
        {Format::A8_UNorm, {0x80}, 0x80000000u},
        {Format::R8_UNorm, {0x80}, 0xFF800000u},
        // BC1 block: color0 = 0xF800 (red) > color1 = 0x001F (blue), all indices 0 -> red, alpha 1.
        {Format::BC1_UNorm, {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0}, 0xFFFF0000u},
        // BC2: explicit alpha 4 bits = 8 (0x88 per byte) + color block color0 = 0x001F (blue), indices 0.
        {Format::BC2_UNorm,
         {0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x1F, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00},
         0x880000FFu},
        // BC3: alpha0 = 0x40, alpha1 = 0, indices 0 -> 0x40; color block green (0x07E0), indices 0.
        {Format::BC3_UNorm,
         {0x40, 0x00, 0, 0, 0, 0, 0, 0, 0xE0, 0x07, 0x00, 0x00, 0, 0, 0, 0},
         0x4000FF00u},
    };
    for (const Case& c : cases) {
        CAPTURE(GetFormatInfo(c.format).name);
        if (!device->IsFormatSupported(c.format, kTextureSampled)) {
            MESSAGE("format not supported by this device: " << GetFormatInfo(c.format).name);
            continue;
        }
        const FormatInfo& info = GetFormatInfo(c.format);
        std::vector<std::uint8_t> data;
        const std::uint32_t units = info.compressed ? 1 : 16;  // 4x4 texels
        for (std::uint32_t i = 0; i < units; ++i) {
            data.insert(data.end(), c.texel.begin(), c.texel.end());
        }
        REQUIRE(data.size() == LevelSize(c.format, 4, 4));
        TextureDesc td;
        td.width = 4;
        td.height = 4;
        td.format = c.format;
        SubresourceData level;
        level.data = data.data();
        const TextureHandle tex = device->CreateTexture(td, &level);
        REQUIRE_MESSAGE(tex, device->LastError());
        ctx.Clear(kClearColor, kBlack, 1.0f, 0, nullptr);
        ctx.SetPipeline(pipeline);
        ctx.SetTexture(0, tex);
        ctx.SetSampler(0, sampler);
        const auto quad = Quad(0.5f, 1, 1, 1, 1);
        REQUIRE(BindVertices(*device, quad.data(), quad.size()));
        ctx.Draw(6, 0);
        const Image img = Capture(*device);
        CHECK_MESSAGE(Near(Pixel(img, 8, 8), c.expected), "got 0x" << std::hex << Pixel(img, 8, 8));
        ctx.SetTexture(0, {});
        device->Destroy(tex);
        device->Present(PresentDesc{});
    }
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: constant buffer updates") {
    auto device = MakeDevice(16, 16);
    REQUIRE(device != nullptr);
    Diag diag;
    REQUIRE(diag.Init(*device));
    CommandContext& ctx = device->Context();
    BufferDesc cb;
    cb.size = 16;
    cb.bind = kBindConstant;
    cb.debugName = "tint";
    const BufferHandle tint = device->CreateBuffer(cb);
    REQUIRE(tint);
    const PipelineHandle pipeline = device->GetPipeline(diag.Desc(diag.tintPs), "tint");
    REQUIRE(pipeline);
    const float values[2][4] = {{0.5f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    const std::uint32_t expected[2] = {0xFF80FF00u, 0xFF0000FFu};
    for (int i = 0; i < 2; ++i) {
        REQUIRE(ctx.UpdateBuffer(tint, values[i], sizeof(values[i])));
        ctx.SetConstantBuffer(kStagePixel, 0, tint);
        ctx.SetPipeline(pipeline);
        const auto quad = Quad(0.5f, 1, 1, 1, 1);
        REQUIRE(BindVertices(*device, quad.data(), quad.size()));
        ctx.Draw(6, 0);
        const Image img = Capture(*device);
        CHECK(Near(Pixel(img, 3, 3), expected[i]));
    }
    CHECK_FALSE(ctx.UpdateBuffer(tint, values, 32));  // larger than the buffer
    // Invalid descriptions.
    BufferDesc bad;
    bad.size = 12;
    bad.bind = kBindConstant;
    CHECK_FALSE(device->CreateBuffer(bad));  // not a multiple of 16
    bad.size = 16;
    bad.bind = kBindConstant | kBindVertex;
    CHECK_FALSE(device->CreateBuffer(bad));
    bad.bind = kBindVertex;
    bad.usage = BufferUsage::Static;
    CHECK_FALSE(device->CreateBuffer(bad));  // static without data
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: transient ring spans stay valid until Present") {
    auto device = MakeDevice(32, 32);
    REQUIRE(device != nullptr);
    Diag diag;
    REQUIRE(diag.Init(*device));
    CommandContext& ctx = device->Context();
    const PipelineHandle pipeline = device->GetPipeline(diag.Desc(diag.colorPs), "ring");
    REQUIRE(pipeline);

    for (int frame = 0; frame < 3; ++frame) {
        CAPTURE(frame);
        // Span A first, then more than a whole chunk (4 MiB) of other allocations, then draw with A.
        const auto quad = Quad(0.5f, 0, 0, 1, 1);
        const auto bytes = static_cast<std::uint32_t>(quad.size() * sizeof(DiagVertex));
        const TransientSpan a = ctx.BeginTransient(TransientKind::Vertex, bytes, 16);
        REQUIRE(a.cpu != nullptr);
        CHECK(a.offset % 16 == 0);
        std::memcpy(a.cpu, quad.data(), bytes);
        ctx.EndTransient();
        BufferHandle last = a.buffer;
        int chunks = 1;
        for (int i = 0; i < 9; ++i) {
            const TransientSpan s = ctx.BeginTransient(TransientKind::Vertex, 600u << 10, 256);
            REQUIRE(s.cpu != nullptr);
            CHECK(s.offset % 256 == 0);
            std::memset(s.cpu, 0x7F, s.size);
            ctx.EndTransient();
            if (s.buffer != last) {
                ++chunks;
                last = s.buffer;
            }
        }
        CHECK(chunks >= 2);
        // One allocation larger than a chunk gets a dedicated chunk.
        const TransientSpan big = ctx.BeginTransient(TransientKind::Vertex, 6u << 20, 16);
        REQUIRE(big.cpu != nullptr);
        std::memset(big.cpu, 0, big.size);
        ctx.EndTransient();
        // Index ring.
        const TransientSpan idx = ctx.BeginTransient(TransientKind::Index, 12, 4);
        REQUIRE(idx.cpu != nullptr);
        const std::uint16_t indices[6] = {0, 1, 2, 3, 4, 5};
        std::memcpy(idx.cpu, indices, sizeof(indices));
        ctx.EndTransient();

        ctx.Clear(kClearColor, kBlack, 1.0f, 0, nullptr);
        ctx.SetPipeline(pipeline);
        ctx.SetVertexBuffer(0, a.buffer, a.offset, sizeof(DiagVertex));
        ctx.SetIndexBuffer(idx.buffer, idx.offset);
        ctx.DrawIndexed(6, 0, 0);
        const Image img = Capture(*device);
        CHECK(Pixel(img, 16, 16) == 0xFF0000FFu);
        CHECK(device->Present(PresentDesc{}) == PresentResult::Ok);
    }
    // Misuse.
    const TransientSpan open = ctx.BeginTransient(TransientKind::Vertex, 64, 16);
    REQUIRE(open.cpu != nullptr);
    CHECK(ctx.BeginTransient(TransientKind::Index, 64, 4).cpu == nullptr);  // one span at a time
    ctx.EndTransient();
    CHECK(ctx.BeginTransient(TransientKind::Vertex, 64, 3).cpu == nullptr);  // alignment not a power of 2
    CHECK(ctx.BeginTransient(TransientKind::Vertex, 0, 4).cpu == nullptr);
    CHECK(device->Stats().transientBytes > (3u * 11u) << 20);
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: sampler and pipeline caches return the same handle for equal descriptors") {
    auto device = MakeDevice(16, 16);
    REQUIRE(device != nullptr);
    Diag diag;
    REQUIRE(diag.Init(*device));
    SamplerDesc a;
    SamplerDesc b = a;
    const SamplerHandle sa = device->GetSampler(a);
    REQUIRE(sa);
    CHECK(device->GetSampler(b) == sa);
    b.addressU = AddressMode::Mirror;
    const SamplerHandle sb = device->GetSampler(b);
    REQUIRE(sb);
    CHECK_FALSE(sb == sa);
    CHECK(device->GetSampler(a) == sa);
    CHECK(device->Stats().samplers == 2);

    PipelineDesc p = diag.Desc(diag.colorPs);
    const PipelineHandle pa = device->GetPipeline(p, "a");
    REQUIRE(pa);
    CHECK(device->GetPipeline(p, "again") == pa);
    PipelineDesc q = p;
    q.blend.enable = true;
    q.blend.src = BlendFactor::SrcAlpha;
    q.blend.dst = BlendFactor::InvSrcAlpha;
    const PipelineHandle pb = device->GetPipeline(q, "b");
    REQUIRE(pb);
    CHECK_FALSE(pb == pa);
    q.raster.cull = CullMode::CW;
    const PipelineHandle pc = device->GetPipeline(q, "c");
    CHECK_FALSE(pc == pb);
    CHECK(device->GetPipeline(p, "a") == pa);
    CHECK(device->Stats().pipelines == 3);
    // Invalid shader handles are rejected; a pipeline without pixel shader is allowed (depth only).
    PipelineDesc bad = p;
    bad.vertexShader = ShaderHandle{0x12345};
    CHECK_FALSE(device->GetPipeline(bad, "bad"));
    bad = p;
    bad.pixelShader = diag.vs;  // wrong stage
    CHECK_FALSE(device->GetPipeline(bad, "bad"));
    PipelineDesc depthOnly = p;
    depthOnly.pixelShader = {};
    CHECK(device->GetPipeline(depthOnly, "depth only"));
    // Draws without a valid pipeline are skipped (no D3D call -> no debug-layer error).
    device->Context().SetPipeline(PipelineHandle{999});
    device->Context().Draw(3, 0);
    CHECK(std::strlen(device->LastError()) > 0);
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: handles of destroyed objects are rejected") {
    auto device = MakeDevice(16, 16);
    REQUIRE(device != nullptr);
    TextureDesc td;
    td.width = 4;
    td.height = 4;
    td.format = Format::BGRA8_UNorm;
    const TextureHandle t1 = device->CreateTexture(td, nullptr);
    REQUIRE(t1);
    CHECK(device->Stats().textures == 1);
    CHECK(device->Stats().textureBytes == 64);
    device->Context().SetTexture(0, t1);
    device->Destroy(t1);
    CHECK(device->Stats().textures == 0);
    const TextureHandle t2 = device->CreateTexture(td, nullptr);  // reuses the slot, new generation
    REQUIRE(t2);
    CHECK_FALSE(t1 == t2);
    Image img;
    CHECK_FALSE(device->ReadTexture(t1, 0, img));
    CHECK(device->ReadTexture(t2, 0, img));
    device->Destroy(t1);  // stale: ignored
    CHECK(device->Stats().textures == 1);
    const ShaderHandle vs = device->CreateShader(ShaderStage::Vertex,
                                                 d3d11::BuiltinShaderCode(d3d11::BuiltinShader::DiagVS), "vs");
    REQUIRE(vs);
    const std::uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_FALSE(device->CreateShader(ShaderStage::Pixel, ShaderBytecode{ShaderFormat::Dxbc, garbage, 8}, "x"));
    CHECK_FALSE(device->CreateShader(ShaderStage::Pixel, ShaderBytecode{ShaderFormat::SpirV, garbage, 8}, "x"));
    device->Destroy(vs);
    device->Destroy(t2);
    CHECK(DebugErrors(*device) == 0);
}

TEST_CASE("rhi d3d11: debug-layer messages are counted, logged and drained") {
    auto device = MakeDevice(16, 16);
    REQUIRE(device != nullptr);
    const DebugLayerInfo layer = device->DebugLayer();
    if (!layer.enabled) {
        MESSAGE("debug layer not available: " << layer.note);
        CHECK_FALSE(layer.note.empty());
        return;
    }
    const d3d11::DebugLayerTotals before = d3d11::GetDebugLayerTotals();
    // Provoke a state-creation error directly on the D3D11 device (a zero-sized buffer).
    auto& backend = static_cast<d3d11::D3D11Device&>(*device);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 0;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ID3D11Buffer* buffer = nullptr;
    CHECK(FAILED(backend.D3D()->CreateBuffer(&bd, nullptr, &buffer)));
    std::vector<DebugMessage> messages;
    CHECK(device->DrainDebugMessages(messages) >= 1);
    bool sawError = false;
    for (const DebugMessage& m : messages) {
        sawError = sawError || (m.severity == DebugSeverity::Error && !m.text.empty());
    }
    CHECK(sawError);
    CHECK(device->Stats().debugErrors >= 1);
    CHECK(d3d11::GetDebugLayerTotals().errors > before.errors);
    messages.clear();
    CHECK(device->DrainDebugMessages(messages) == 0);  // drained
    CHECK(std::strcmp(d3d11::DebugLayerStateName(layer), "enabled") == 0);
}

TEST_CASE("rhi d3d11: teardown releases every object") {
    {
        auto device = MakeDevice(32, 32);
        REQUIRE(device != nullptr);
        if (!device->DebugLayer().enabled) {
            MESSAGE("debug layer not available - live objects cannot be counted");
            return;
        }
        Diag diag;
        REQUIRE(diag.Init(*device));
        CommandContext& ctx = device->Context();
        TextureDesc td;
        td.width = 8;
        td.height = 8;
        td.format = Format::BGRA8_UNorm;
        td.usage = kTextureSampled | kTextureRenderTarget;
        const TextureHandle target = device->CreateTexture(td, nullptr);
        REQUIRE(target);
        td.usage = kTextureSampled;
        const TextureHandle texture = device->CreateTexture(td, nullptr);
        REQUIRE(texture);
        BufferDesc cb;
        cb.size = 16;
        cb.bind = kBindConstant;
        const BufferHandle constants = device->CreateBuffer(cb);
        REQUIRE(constants);
        const PipelineHandle pipeline = device->GetPipeline(diag.Desc(diag.texturePs), "teardown");
        REQUIRE(pipeline);
        ctx.SetRenderTargets(target, {});
        ctx.Clear(kClearColor, kBlack, 1.0f, 0, nullptr);
        ctx.SetRenderTargets({}, {});
        ctx.SetPipeline(pipeline);
        ctx.SetTexture(0, target);
        ctx.SetSampler(0, device->GetSampler(SamplerDesc{}));
        ctx.SetConstantBuffer(kStagePixel | kStageVertex, 0, constants);
        Rect r{1, 1, 5, 5};
        ctx.Clear(kClearDepth, kBlack, 0.5f, 0, &r);  // creates the rectangle-clear objects
        const auto quad = Quad(0.5f, 1, 1, 1, 1);
        REQUIRE(BindVertices(*device, quad.data(), quad.size()));
        ctx.Draw(6, 0);
        Image img;
        REQUIRE(device->CaptureBackBuffer(img));
        CHECK(device->Present(PresentDesc{}) == PresentResult::Ok);
        device->Destroy(texture);  // the rest stays alive until the device goes
        CHECK(DebugErrors(*device) == 0);
    }
    CHECK(d3d11::LastTeardownLiveObjects() == 0);

    // Negative control: an object the caller still holds is reported.
    Microsoft::WRL::ComPtr<ID3D11Buffer> leaked;
    {
        auto device = MakeDevice(16, 16);
        REQUIRE(device != nullptr);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        REQUIRE(SUCCEEDED(static_cast<d3d11::D3D11Device&>(*device).D3D()->CreateBuffer(&bd, nullptr, &leaked)));
    }
    CHECK(d3d11::LastTeardownLiveObjects() == 1);
    leaked.Reset();
}
