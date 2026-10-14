// Unit tests of the fixed-function pipeline translations (gfx/ff_pipeline.*, GFX_CONTRACT.md 6.6):
// feature keys, rhi descriptors, constant-buffer packing, the Xbox texture-op translation, the embedded
// shader bytecode and FixedFunctionPipeline::Draw against a recording mock device (no GPU).
// Rendered pixel checks on WARP: tests/ff_render_tests.cpp.
#include "doctest.h"

#include "gfx/ff_pipeline.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace {

using namespace gfx;
using gfx::ff::FeatureKey;

bool Near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

ni::DrawItem MakeTriangleItem() {
    static const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    static const std::uint16_t indices[3] = {0, 1, 2};
    ni::DrawItem item;
    item.kind = ni::DrawKind::Shape;
    item.geometry.vertexCount = 3;
    item.geometry.positions = {positions, 12};
    item.geometry.indices = indices;
    item.geometry.indexCount = 3;
    item.geometry.primitiveCount = 1;
    item.state.lightingEnable = false;
    return item;
}

// ---- recording mock device ----------------------------------------------------------------------------------

struct MockContext final : rhi::CommandContext {
    struct VertexBinding {
        rhi::BufferHandle buffer;
        std::uint32_t offset = 0;
        std::uint32_t stride = 0;
    };
    VertexBinding vertexBuffers[rhi::kMaxVertexStreams];
    rhi::BufferHandle indexBuffer;
    std::uint32_t indexOffset = 0;
    rhi::BufferHandle constantBuffers[2][4];  // [vertex, pixel][slot]
    rhi::TextureHandle textures[4];
    rhi::SamplerHandle samplers[4];
    rhi::PipelineHandle pipeline;
    std::uint8_t stencilRef = 0;
    std::uint32_t draws = 0, drawIndexed = 0, lastCount = 0;
    std::map<std::uint32_t, std::vector<std::uint8_t>> bufferContents;  // UpdateBuffer data per buffer id
    std::map<std::uint32_t, std::uint32_t> updateCounts;
    std::vector<std::uint8_t> ring[2];  // vertex, index
    rhi::BufferHandle ringBuffer[2] = {{900}, {901}};
    std::uint32_t ringUsed[2] = {};
    bool open = false;
    bool failTransient = false;

    void SetRenderTargets(rhi::TextureHandle, rhi::TextureHandle) noexcept override {}
    void SetViewport(const rhi::Viewport&) noexcept override {}
    void SetScissor(const rhi::Rect&) noexcept override {}
    void Clear(std::uint8_t, const float[4], float, std::uint8_t, const rhi::Rect*) noexcept override {}
    void SetPipeline(rhi::PipelineHandle p) noexcept override { pipeline = p; }
    void SetStencilRef(std::uint8_t ref) noexcept override { stencilRef = ref; }
    void SetBlendFactor(const float[4]) noexcept override {}
    void SetVertexBuffer(std::uint32_t stream, rhi::BufferHandle buffer, std::uint32_t offset,
                         std::uint32_t stride) noexcept override {
        vertexBuffers[stream] = {buffer, offset, stride};
    }
    void SetIndexBuffer(rhi::BufferHandle buffer, std::uint32_t offset) noexcept override {
        indexBuffer = buffer;
        indexOffset = offset;
    }
    void SetConstantBuffer(std::uint8_t stages, std::uint32_t slot, rhi::BufferHandle buffer) noexcept override {
        if (stages & rhi::kStageVertex) constantBuffers[0][slot] = buffer;
        if (stages & rhi::kStagePixel) constantBuffers[1][slot] = buffer;
    }
    void SetTexture(std::uint32_t slot, rhi::TextureHandle texture) noexcept override { textures[slot] = texture; }
    void SetSampler(std::uint32_t slot, rhi::SamplerHandle sampler) noexcept override { samplers[slot] = sampler; }
    void Draw(std::uint32_t count, std::uint32_t) noexcept override {
        ++draws;
        lastCount = count;
    }
    void DrawIndexed(std::uint32_t count, std::uint32_t, std::int32_t) noexcept override {
        ++drawIndexed;
        lastCount = count;
    }
    bool UpdateBuffer(rhi::BufferHandle buffer, const void* data, std::uint32_t size) noexcept override {
        auto& v = bufferContents[buffer.id];
        v.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
        ++updateCounts[buffer.id];
        return true;
    }
    rhi::TransientSpan BeginTransient(rhi::TransientKind kind, std::uint32_t size, std::uint32_t alignment) noexcept override {
        CHECK_FALSE(open);  // (CHECK: REQUIRE would throw out of a noexcept function)
        if (failTransient) return {};
        const int k = kind == rhi::TransientKind::Vertex ? 0 : 1;
        std::uint32_t offset = (ringUsed[k] + alignment - 1) / alignment * alignment;
        ring[k].resize(offset + size);
        ringUsed[k] = offset + size;
        open = true;
        return {ringBuffer[k], offset, size, ring[k].data() + offset};
    }
    void EndTransient() noexcept override {
        CHECK(open);
        open = false;
    }

    template <class T>
    const T* Constants(rhi::BufferHandle buffer) const {
        auto it = bufferContents.find(buffer.id);
        return it != bufferContents.end() && it->second.size() == sizeof(T) ? reinterpret_cast<const T*>(it->second.data())
                                                                            : nullptr;
    }
    const std::uint8_t* Ring(const VertexBinding& b) const {
        return b.buffer == ringBuffer[0] ? ring[0].data() + b.offset : nullptr;
    }
};

struct MockDevice final : rhi::Device {
    MockContext context;
    rhi::DeviceInfo info;
    std::uint32_t nextId = 1;
    std::vector<rhi::BufferDesc> buffers;
    std::vector<std::vector<std::uint8_t>> bufferInit;
    std::vector<rhi::TextureDesc> textures;
    std::vector<rhi::ShaderStage> shaders;
    std::vector<rhi::ShaderBytecode> shaderCode;
    std::vector<rhi::SamplerDesc> samplers;
    std::vector<rhi::PipelineDesc> pipelines;
    std::uint32_t destroyed = 0;
    bool failPipelines = false;

    MockDevice() {
        info.backend = rhi::Backend::D3D11;
        info.backBufferFormat = rhi::Format::BGRA8_UNorm;
        info.depthFormat = rhi::Format::D24_UNorm_S8_UInt;
    }
    const rhi::DeviceInfo& Info() const noexcept override { return info; }
    rhi::CommandContext& Context() noexcept override { return context; }
    rhi::ShaderFormat NativeShaderFormat() const noexcept override { return rhi::ShaderFormat::Dxbc; }
    bool IsFormatSupported(rhi::Format, std::uint8_t) const noexcept override { return true; }
    rhi::BufferHandle CreateBuffer(const rhi::BufferDesc& desc) noexcept override {
        buffers.push_back(desc);
        const auto* p = static_cast<const std::uint8_t*>(desc.initialData);
        bufferInit.emplace_back(p != nullptr ? std::vector<std::uint8_t>(p, p + desc.size) : std::vector<std::uint8_t>{});
        return {static_cast<std::uint32_t>(100 + buffers.size())};
    }
    rhi::TextureHandle CreateTexture(const rhi::TextureDesc& desc, const rhi::SubresourceData*) noexcept override {
        textures.push_back(desc);
        return {static_cast<std::uint32_t>(200 + textures.size())};
    }
    bool UpdateTexture(rhi::TextureHandle, std::uint32_t, const rhi::SubresourceData&) noexcept override { return true; }
    rhi::ShaderHandle CreateShader(rhi::ShaderStage stage, const rhi::ShaderBytecode& code, const char*) noexcept override {
        shaders.push_back(stage);
        shaderCode.push_back(code);
        return {static_cast<std::uint32_t>(300 + shaders.size())};
    }
    void Destroy(rhi::BufferHandle) noexcept override { ++destroyed; }
    void Destroy(rhi::TextureHandle) noexcept override { ++destroyed; }
    void Destroy(rhi::ShaderHandle) noexcept override { ++destroyed; }
    rhi::SamplerHandle GetSampler(const rhi::SamplerDesc& desc) noexcept override {
        for (std::size_t i = 0; i < samplers.size(); ++i) {
            if (samplers[i] == desc) return {static_cast<std::uint32_t>(400 + i + 1)};
        }
        samplers.push_back(desc);
        return {static_cast<std::uint32_t>(400 + samplers.size())};
    }
    rhi::PipelineHandle GetPipeline(const rhi::PipelineDesc& desc, const char*) noexcept override {
        if (failPipelines) return {};
        for (std::size_t i = 0; i < pipelines.size(); ++i) {
            if (pipelines[i] == desc) return {static_cast<std::uint32_t>(500 + i + 1)};
        }
        pipelines.push_back(desc);
        return {static_cast<std::uint32_t>(500 + pipelines.size())};
    }
    rhi::PresentResult Present(const rhi::PresentDesc&) noexcept override { return rhi::PresentResult::Ok; }
    bool CaptureBackBuffer(rhi::Image&) noexcept override { return false; }
    bool ReadTexture(rhi::TextureHandle, std::uint32_t, rhi::Image&) noexcept override { return false; }
    rhi::DebugLayerInfo DebugLayer() const noexcept override { return {}; }
    std::size_t DrainDebugMessages(std::vector<rhi::DebugMessage>&) noexcept override { return 0; }
    rhi::DeviceStats Stats() const noexcept override { return {}; }
    const char* LastError() const noexcept override { return "mock"; }
};

ni::CameraDesc IdentityCamera() {
    ni::CameraDesc camera;
    camera.view = IdentityMatrix();
    camera.projection = IdentityMatrix();
    camera.screenProjection = IdentityMatrix();
    camera.viewport = {0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f};
    return camera;
}

}  // namespace

// ---- pure translations -------------------------------------------------------------------------------------

TEST_CASE("ff: Xbox and PC texture op codes (ff_shared.h)") {
    // Evidence: FUN_0023FDB0 / 0x23FFA4 (op - 12 = blend alpha source), REN's decal row 0xE (FUN_0015A120).
    // The renumbered ops; the translation itself is ni::TextureOpFromXbox (extract agent).
    CHECK(FF_XBOX_TOP_BLENDTEXTUREALPHA == 14);
    CHECK(FF_TOP_BLENDTEXTUREALPHA == 13);
    CHECK(FF_XBOX_TOP_BLENDCURRENTALPHA == 13);
    CHECK(FF_TOP_BLENDCURRENTALPHA == 16);
    CHECK(FF_XBOX_TOP_BUMPENVMAP == 0x19);
    CHECK(FF_TOP_BUMPENVMAP == 22);
    CHECK(FF_XBOX_TOP_DOTPRODUCT3 == 22);
    CHECK(FF_TOP_DOTPRODUCT3 == 24);
}

TEST_CASE("ff: feature key") {
    ni::DrawItem item = MakeTriangleItem();
    FeatureKey key = ff::MakeFeatureKey(item);
    CHECK((key & FF_FEATURE_PRETRANSFORMED) == 0);
    CHECK((key & FF_FEATURE_LIGHTING) == 0);
    CHECK((key & FF_FEATURE_HAS_COLOR) == 0);
    CHECK((key & FF_FEATURE_LOCAL_VIEWER) != 0);  // RenderStateDesc::localViewer default TRUE
    CHECK(((key & FF_FEATURE_STAGE_MASK) >> FF_FEATURE_STAGE_SHIFT) == 0);
    item.state.localViewer = false;
    CHECK((ff::MakeFeatureKey(item) & FF_FEATURE_LOCAL_VIEWER) == 0);
    item.state.localViewer = true;
    CHECK(ff::VertexPermutation(key) == 1);
    CHECK(ff::PixelPermutation(key) == 0);

    static const float normals[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    static const float colors[12] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    item.geometry.normals = {normals, 12};
    item.geometry.colors = {colors, 16};
    item.geometry.uvSetCount = 2;
    item.state.lightingEnable = true;
    item.state.normalizeNormals = true;
    item.state.specularEnable = true;
    item.state.alphaTestEnable = true;
    item.state.fogEnable = true;
    item.state.shadeMode = ni::ShadeMode::Flat;
    item.state.stages[0].colorOp = FF_TOP_MODULATE;
    item.state.stages[1].colorOp = FF_TOP_ADD;
    item.state.stages[2].colorOp = FF_TOP_DISABLE;
    item.state.stages[3].colorOp = FF_TOP_MODULATE;  // after a DISABLE: not part of the cascade
    item.useScreenCamera = true;
    key = ff::MakeFeatureKey(item);
    for (std::uint32_t bit : {FF_FEATURE_SCREEN_CAMERA, FF_FEATURE_LIGHTING, FF_FEATURE_HAS_NORMAL, FF_FEATURE_HAS_COLOR,
                              FF_FEATURE_SPECULAR, FF_FEATURE_NORMALIZE, FF_FEATURE_ALPHA_TEST, FF_FEATURE_FOG,
                              FF_FEATURE_FLAT_SHADE}) {
        CHECK((key & bit) == bit);
    }
    CHECK(((key & FF_FEATURE_STAGE_MASK) >> FF_FEATURE_STAGE_SHIFT) == 2);
    CHECK(((key & FF_FEATURE_UV_MASK) >> FF_FEATURE_UV_SHIFT) == 2);
    CHECK(ff::ActiveStageCount(item.state) == 2);
    CHECK(ff::VertexPermutation(key) == 2);
    CHECK(ff::PixelPermutation(key) == 1);

    // Pre-transformed (slot 55): never lit, no normals.
    item.geometry.pretransformed = true;
    key = ff::MakeFeatureKey(item);
    CHECK((key & FF_FEATURE_PRETRANSFORMED) != 0);
    CHECK((key & (FF_FEATURE_LIGHTING | FF_FEATURE_HAS_NORMAL | FF_FEATURE_NORMALIZE | FF_FEATURE_SCREEN_CAMERA)) == 0);
    CHECK(ff::VertexPermutation(key) == 0);
}

TEST_CASE("ff: embedded shader bytecode exists for every permutation") {
    for (FeatureKey key : {FeatureKey{FF_FEATURE_PRETRANSFORMED}, FeatureKey{0}, FeatureKey{FF_FEATURE_LIGHTING}}) {
        const rhi::ShaderBytecode code = ff::VertexShaderBytecode(key);
        REQUIRE(code.data != nullptr);
        REQUIRE(code.size > 32);
        CHECK(std::memcmp(code.data, "DXBC", 4) == 0);
        CHECK(code.format == rhi::ShaderFormat::Dxbc);
    }
    CHECK(ff::VertexShaderBytecode(FF_FEATURE_PRETRANSFORMED).data != ff::VertexShaderBytecode(0).data);
    CHECK(ff::VertexShaderBytecode(FF_FEATURE_LIGHTING).data != ff::VertexShaderBytecode(0).data);
    // LIGHTING is ignored for pre-transformed vertices.
    CHECK(ff::VertexShaderBytecode(FF_FEATURE_PRETRANSFORMED | FF_FEATURE_LIGHTING).data ==
          ff::VertexShaderBytecode(FF_FEATURE_PRETRANSFORMED).data);
    for (FeatureKey key : {FeatureKey{0}, FeatureKey{FF_FEATURE_FLAT_SHADE}}) {
        const rhi::ShaderBytecode code = ff::PixelShaderBytecode(key);
        REQUIRE(code.size > 32);
        CHECK(std::memcmp(code.data, "DXBC", 4) == 0);
    }
    CHECK(ff::PixelShaderBytecode(FF_FEATURE_FLAT_SHADE).data != ff::PixelShaderBytecode(0).data);
    CHECK(std::memcmp(ff::StubShaderBytecode(rhi::ShaderStage::Vertex).data, "DXBC", 4) == 0);
    CHECK(std::memcmp(ff::StubShaderBytecode(rhi::ShaderStage::Pixel).data, "DXBC", 4) == 0);
}

TEST_CASE("ff: blend descriptor") {
    ni::RenderStateDesc state;
    rhi::BlendDesc blend = ff::MakeBlendDesc(state);
    CHECK_FALSE(blend.enable);
    CHECK(blend == rhi::BlendDesc{});  // canonical when disabled
    state.alphaBlendEnable = true;
    state.srcBlend = rhi::BlendFactor::SrcAlpha;
    state.dstBlend = rhi::BlendFactor::InvSrcAlpha;
    blend = ff::MakeBlendDesc(state);
    CHECK(blend.enable);
    CHECK(blend.src == rhi::BlendFactor::SrcAlpha);
    CHECK(blend.dst == rhi::BlendFactor::InvSrcAlpha);
    CHECK(blend.srcAlpha == rhi::BlendFactor::SrcAlpha);
    CHECK(blend.dstAlpha == rhi::BlendFactor::InvSrcAlpha);
    CHECK(blend.op == rhi::BlendOp::Add);
    CHECK(blend.writeMask == 0xF);
    // Color factors act with their alpha component on the alpha channel (D3D11 forbids *_COLOR there).
    state.srcBlend = rhi::BlendFactor::DestColor;
    state.dstBlend = rhi::BlendFactor::InvSrcColor;
    blend = ff::MakeBlendDesc(state);
    CHECK(blend.src == rhi::BlendFactor::DestColor);
    CHECK(blend.srcAlpha == rhi::BlendFactor::DestAlpha);
    CHECK(blend.dstAlpha == rhi::BlendFactor::InvSrcAlpha);
    state.srcBlend = rhi::BlendFactor::SrcAlphaSat;
    state.dstBlend = rhi::BlendFactor::InvDestColor;
    blend = ff::MakeBlendDesc(state);
    CHECK(blend.srcAlpha == rhi::BlendFactor::One);
    CHECK(blend.dstAlpha == rhi::BlendFactor::InvDestAlpha);
}

TEST_CASE("ff: depth/stencil and raster descriptors") {
    ni::RenderStateDesc state;
    rhi::DepthStencilDesc ds = ff::MakeDepthStencilDesc(state);
    CHECK(ds.depthEnable);
    CHECK(ds.depthWrite);
    CHECK(ds.depthFunc == rhi::CompareFunc::LessEqual);
    CHECK_FALSE(ds.stencilEnable);
    // NiZBufferProperty test off = ZFUNC ALWAYS with z still enabled (FUN_00159B60).
    state.zFunc = rhi::CompareFunc::Always;
    state.zWriteEnable = false;
    state.stencilEnable = true;
    state.stencilFunc = rhi::CompareFunc::Equal;
    state.stencilMask = 0x0F;
    state.stencilFail = rhi::StencilOp::Zero;
    state.stencilZFail = rhi::StencilOp::IncrSat;
    state.stencilPass = rhi::StencilOp::Replace;
    ds = ff::MakeDepthStencilDesc(state);
    CHECK(ds.depthEnable);
    CHECK_FALSE(ds.depthWrite);
    CHECK(ds.depthFunc == rhi::CompareFunc::Always);
    CHECK(ds.stencilEnable);
    CHECK(ds.stencilFunc == rhi::CompareFunc::Equal);
    CHECK(ds.stencilReadMask == 0x0F);
    CHECK(ds.stencilWriteMask == 0xFF);
    CHECK(ds.stencilFail == rhi::StencilOp::Zero);
    CHECK(ds.stencilDepthFail == rhi::StencilOp::IncrSat);
    CHECK(ds.stencilPass == rhi::StencilOp::Replace);

    rhi::RasterDesc raster = ff::MakeRasterDesc(ni::RenderStateDesc{});
    CHECK(raster.cull == rhi::CullMode::CW);  // NiStencilProperty draw mode 0 (FUN_001599E0)
    CHECK(raster.fill == rhi::FillMode::Solid);
    CHECK(raster.depthClip);
    state.cullMode = rhi::CullMode::None;
    state.fillMode = rhi::FillMode::Wireframe;
    raster = ff::MakeRasterDesc(state);
    CHECK(raster.cull == rhi::CullMode::None);
    CHECK(raster.fill == rhi::FillMode::Wireframe);
}

TEST_CASE("ff: sampler descriptor and vertex layout") {
    ni::TextureStageDesc stage;
    stage.minFilter = rhi::Filter::Point;
    stage.magFilter = rhi::Filter::Linear;
    stage.mipFilter = rhi::MipFilter::None;
    stage.addressU = rhi::AddressMode::Clamp;
    stage.addressV = rhi::AddressMode::Wrap;
    rhi::SamplerDesc s = ff::MakeSamplerDesc(stage);
    CHECK(s.minFilter == rhi::Filter::Point);
    CHECK(s.magFilter == rhi::Filter::Linear);
    CHECK(s.mipFilter == rhi::MipFilter::None);
    CHECK(s.maxLod == 0.0f);
    CHECK(s.addressU == rhi::AddressMode::Clamp);
    CHECK(s.addressV == rhi::AddressMode::Wrap);
    stage.mipFilter = rhi::MipFilter::Linear;
    CHECK(ff::MakeSamplerDesc(stage).maxLod > 100.0f);

    const rhi::VertexLayoutDesc layout = ff::MakeVertexLayout();
    REQUIRE(layout.attributeCount == FF_STREAM_COUNT);
    CHECK(layout.attributes[0].semantic == rhi::VertexSemantic::Position);
    CHECK(layout.attributes[0].format == rhi::VertexFormat::Float3);
    CHECK(layout.attributes[1].semantic == rhi::VertexSemantic::Normal);
    CHECK(layout.attributes[2].semantic == rhi::VertexSemantic::Color);
    CHECK(layout.attributes[2].format == rhi::VertexFormat::Float4);
    for (int i = 0; i < 4; ++i) {
        const rhi::VertexAttribute& a = layout.attributes[3 + i];
        CHECK(a.semantic == rhi::VertexSemantic::TexCoord);
        CHECK(a.semanticIndex == i);
        CHECK(a.stream == FF_STREAM_UV0 + i);
        CHECK(a.format == rhi::VertexFormat::Float2);
        CHECK(layout.streamStrides[a.stream] == 8);
    }
    CHECK(layout.streamStrides[FF_STREAM_POSITION] == 12);
    CHECK(layout.streamStrides[FF_STREAM_COLOR] == 16);
}

TEST_CASE("ff: constant buffer packing (C++ offsets = HLSL cbuffer packing)") {
    using ff::FFDrawConstants;
    using ff::FFFrameConstants;
    using ff::FFLight;
    using ff::FFStage;
    // HLSL packs float4/uint4/float4x4 on 16-byte registers in declaration order.
    CHECK(offsetof(FFFrameConstants, pretransform) == 256);
    CHECK(offsetof(FFFrameConstants, fogColor) == 272);
    CHECK(offsetof(FFFrameConstants, fogParams) == 288);
    CHECK(offsetof(FFFrameConstants, targetSize) == 304);
    CHECK(offsetof(FFFrameConstants, pixelCenter) == 320);
    CHECK(sizeof(FFFrameConstants) == 336);
    CHECK(offsetof(FFDrawConstants, materialDiffuse) == 256);
    CHECK(offsetof(FFDrawConstants, features) == 368);
    CHECK(offsetof(FFDrawConstants, fog) == 416);
    CHECK(sizeof(FFDrawConstants) == 432);
    CHECK(offsetof(FFLight, position) == 48);
    CHECK(offsetof(FFLight, spot) == 96);
    CHECK(offsetof(FFStage, coord) == 32);
    CHECK(offsetof(FFStage, transform) == 48);
    CHECK(sizeof(ff::FFLightConstants) == 8 * 112);
    CHECK(sizeof(ff::FFStageConstants) == 4 * 112);
}

TEST_CASE("ff: frame constants and the pre-transform of D3D8 pixels") {
    ni::CameraDesc camera = IdentityCamera();
    camera.view.m[3][2] = 5.0f;
    camera.projection.m[0][0] = 2.0f;
    ff::FFFrameConstants c;
    ff::FillFrameConstants(camera, 640, 480, &c);
    CHECK(c.view.m[3][2] == 5.0f);
    CHECK(c.projection.m[0][0] == 2.0f);
    CHECK(c.viewProjection.m[3][2] == 5.0f);
    CHECK(c.viewProjection.m[0][0] == 2.0f);
    CHECK(c.targetSize.x == 640.0f);
    CHECK(Near(c.targetSize.w, 1.0f / 480.0f));
    // A D3D8 pixel edge at x - 0.5 (FUN_0014A050) lands on the D3D11 pixel edge x.
    auto ndc = [&c](float x, float y) {
        return std::pair<float, float>{(x + 0.5f) * c.pretransform.x + c.pretransform.z,
                                       (y + 0.5f) * c.pretransform.y + c.pretransform.w};
    };
    CHECK(Near(ndc(-0.5f, -0.5f).first, -1.0f));
    CHECK(Near(ndc(-0.5f, -0.5f).second, 1.0f));
    CHECK(Near(ndc(639.5f, 479.5f).first, 1.0f));
    CHECK(Near(ndc(639.5f, 479.5f).second, -1.0f));
    // Sub-viewport: pixels stay absolute render-target coordinates.
    camera.viewport = {100.0f, 50.0f, 200.0f, 100.0f, 0.0f, 1.0f};
    ff::FillFrameConstants(camera, 640, 480, &c);
    CHECK(Near(ndc(99.5f, 49.5f).first, -1.0f));
    CHECK(Near(ndc(99.5f, 49.5f).second, 1.0f));
    CHECK(Near(ndc(299.5f, 149.5f).first, 1.0f));
    CHECK(Near(ndc(299.5f, 149.5f).second, -1.0f));
    // The B19 pixel-center shift: 0.5 by default, the given one on texture targets (gfx::TargetPixelCenter).
    CHECK(c.pixelCenter.x == 0.5f);
    CHECK(c.pixelCenter.y == 0.5f);
    ff::FillFrameConstants(camera, 640, 480, &c, 0.0f, 0.25f);
    CHECK(c.pixelCenter.x == 0.0f);
    CHECK(c.pixelCenter.y == 0.25f);
    ff::FillFrameConstants(camera, 640, 480, &c);

    ni::RenderStateDesc state;
    state.fogColor = {0.25f, 0.5f, 2.0f};
    state.fogStart = 10.0f;
    state.fogEnd = 30.0f;
    state.fogDensity = 0.5f;
    ff::FillFogConstants(state, &c);
    CHECK(c.fogColor.x == 0.25f);
    CHECK(c.fogColor.z == 1.0f);  // saturated
    CHECK(c.fogParams.x == 10.0f);
    CHECK(c.fogParams.y == 30.0f);
    CHECK(c.fogParams.z == 0.5f);
    CHECK(Near(c.fogParams.w, 1.0f / 20.0f));
    state.fogEnd = 10.0f;
    ff::FillFogConstants(state, &c);
    CHECK(c.fogParams.w == 0.0f);
}

TEST_CASE("ff: draw constants (matrices, material, states)") {
    ni::CameraDesc camera = IdentityCamera();
    camera.view.m[3][0] = 1.0f;          // view translation
    camera.projection.m[1][1] = 3.0f;
    camera.screenProjection.m[0][0] = 2.0f;
    ni::DrawItem item = MakeTriangleItem();
    item.world.m[0][0] = 2.0f;           // non-uniform scale
    item.world.m[3][1] = 4.0f;
    item.state.lightingEnable = true;
    item.state.material.diffuse = {0.5f, 0.25f, 1.0f, 0.75f};
    item.state.material.power = 8.0f;
    item.state.diffuseSource = FF_MCS_COLOR1;
    item.state.alphaTestEnable = true;
    item.state.alphaFunc = rhi::CompareFunc::Greater;
    item.state.alphaRef = 0x80;
    item.state.fogEnable = true;
    item.state.fogTableMode = FF_FOG_LINEAR;
    item.state.rangeFogEnable = true;
    item.lights.count = 3;
    item.lights.ambient = {0.5f, 1.5f, -1.0f, 1.0f};
    item.geometry.uvSetCount = 6;

    ff::FFDrawConstants c;
    ff::FillDrawConstants(item, camera, &c);
    CHECK(c.world.m[0][0] == 2.0f);
    CHECK(c.worldView.m[3][0] == 1.0f);  // world * view
    CHECK(c.worldView.m[3][1] == 4.0f);
    CHECK(c.worldViewProjection.m[3][1] == 12.0f);
    // normal matrix = inverse transpose of world*view (upper 3x3): x scale 2 -> 0.5, no translation.
    CHECK(Near(c.normalMatrix.m[0][0], 0.5f));
    CHECK(c.normalMatrix.m[3][0] == 0.0f);
    CHECK(c.normalMatrix.m[0][3] == 0.0f);
    CHECK(c.materialDiffuse.w == 0.75f);
    CHECK(c.materialPower.x == 8.0f);
    CHECK(c.globalAmbient.x == 0.5f);
    CHECK(c.globalAmbient.y == 1.0f);  // clamped (the original wraps)
    CHECK(c.globalAmbient.z == 0.0f);
    CHECK(c.features.x == ff::MakeFeatureKey(item));
    CHECK(c.features.y == 3);
    CHECK(c.features.z == 4);  // at most 4 UV sets
    CHECK(c.colorSources.x == FF_MCS_COLOR1);
    CHECK(c.alphaTest.x == 1);
    CHECK(c.alphaTest.y == FF_CMP_GREATER);
    CHECK(c.alphaTest.z == 0x80);
    CHECK(c.fog.x == FF_FOG_LINEAR);
    CHECK(c.fog.y == 1);

    // Lighting off: no lights are counted.
    item.state.lightingEnable = false;
    ff::FillDrawConstants(item, camera, &c);
    CHECK(c.features.y == 0);
    // Screen camera (slot 54): view = identity, projection = screenProjection.
    item.useScreenCamera = true;
    ff::FillDrawConstants(item, camera, &c);
    CHECK(c.worldView.m[3][0] == 0.0f);
    CHECK(c.worldViewProjection.m[0][0] == 4.0f);
    // Fog disabled -> mode NONE.
    item.state.fogEnable = false;
    ff::FillDrawConstants(item, camera, &c);
    CHECK(c.fog.x == FF_FOG_NONE);
}

TEST_CASE("ff: alpha-blended draws discard fully transparent fragments (G2 menu text boxes)") {
    ni::CameraDesc camera = IdentityCamera();
    ni::DrawItem item = MakeTriangleItem();
    item.state.alphaBlendEnable = true;
    item.state.srcBlend = rhi::BlendFactor::SrcAlpha;
    item.state.dstBlend = rhi::BlendFactor::InvSrcAlpha;
    item.state.alphaTestEnable = false;
    item.state.zWriteEnable = true;
    CHECK(ff::DiscardsTransparentFragments(item.state));
    ff::FFDrawConstants c;
    ff::FillDrawConstants(item, camera, &c);
    CHECK(c.alphaTest.x == 1);
    CHECK(c.alphaTest.y == FF_CMP_GREATER);
    CHECK(c.alphaTest.z == 0);
    CHECK(c.alphaTest.w == 0);
    item.state.dstBlend = rhi::BlendFactor::One;  // additive with source alpha: alpha 0 adds nothing either
    CHECK(ff::DiscardsTransparentFragments(item.state));
    // Not where an alpha-0 fragment can change the colour, where nothing writes depth or the game tests alpha.
    item.state.srcBlend = rhi::BlendFactor::One;
    CHECK_FALSE(ff::DiscardsTransparentFragments(item.state));
    item.state.srcBlend = rhi::BlendFactor::SrcAlpha;
    item.state.dstBlend = rhi::BlendFactor::SrcColor;
    CHECK_FALSE(ff::DiscardsTransparentFragments(item.state));
    item.state.dstBlend = rhi::BlendFactor::InvSrcAlpha;
    item.state.zWriteEnable = false;
    CHECK_FALSE(ff::DiscardsTransparentFragments(item.state));
    ff::FillDrawConstants(item, camera, &c);
    CHECK(c.alphaTest.x == 0);
    item.state.zWriteEnable = true;
    item.state.alphaBlendEnable = false;
    CHECK_FALSE(ff::DiscardsTransparentFragments(item.state));
    item.state.alphaBlendEnable = true;
    item.state.alphaTestEnable = true;
    item.state.alphaFunc = rhi::CompareFunc::GreaterEqual;
    item.state.alphaRef = 0x40;
    CHECK_FALSE(ff::DiscardsTransparentFragments(item.state));
    ff::FillDrawConstants(item, camera, &c);
    CHECK(c.alphaTest.y == FF_CMP_GREATEREQUAL);
    CHECK(c.alphaTest.z == 0x40);
}

TEST_CASE("ff: lights are transformed to view space") {
    ni::CameraDesc camera = IdentityCamera();
    // View: 90 degrees about y (x -> -z, z -> x) and a translation.
    camera.view = {};
    camera.view.m[0][2] = -1.0f;
    camera.view.m[1][1] = 1.0f;
    camera.view.m[2][0] = 1.0f;
    camera.view.m[3][3] = 1.0f;
    camera.view.m[3][1] = -2.0f;
    ni::LightSetDesc lights;
    lights.count = 2;
    lights.lights[0].type = FF_LIGHT_DIRECTIONAL;
    lights.lights[0].direction = {2.0f, 0.0f, 0.0f};
    lights.lights[0].diffuse = {1.0f, 0.5f, 0.25f, 1.0f};
    lights.lights[1].type = FF_LIGHT_SPOT;
    lights.lights[1].position = {1.0f, 3.0f, 0.0f};
    lights.lights[1].direction = {0.0f, 0.0f, 1.0f};
    lights.lights[1].range = 1.0e19f;
    lights.lights[1].attenuation0 = 1.0f;
    lights.lights[1].attenuation2 = 0.5f;
    lights.lights[1].theta = 1.0f;
    lights.lights[1].phi = 2.0f;
    lights.lights[1].falloff = 3.0f;
    ff::FFLightConstants c;
    ff::FillLightConstants(lights, camera, &c);
    CHECK(Near(c.lights[0].direction.z, -1.0f));  // normalized, rotated
    CHECK(Near(c.lights[0].direction.x, 0.0f));
    CHECK(c.lights[0].attenuation.w == static_cast<float>(FF_LIGHT_DIRECTIONAL));
    CHECK(c.lights[0].diffuse.y == 0.5f);
    CHECK(Near(c.lights[1].position.x, 0.0f));
    CHECK(Near(c.lights[1].position.y, 1.0f));
    CHECK(Near(c.lights[1].position.z, -1.0f));
    CHECK(c.lights[1].position.w == 1.0e19f);
    CHECK(Near(c.lights[1].direction.x, 1.0f));
    CHECK(c.lights[1].direction.w == 3.0f);
    CHECK(c.lights[1].attenuation.x == 1.0f);
    CHECK(c.lights[1].attenuation.z == 0.5f);
    CHECK(Near(c.lights[1].spot.x, std::cos(0.5f)));
    CHECK(Near(c.lights[1].spot.y, std::cos(1.0f)));
    CHECK(Near(c.lights[1].spot.z, 1.0f / (std::cos(0.5f) - std::cos(1.0f)), 1e-3f));
    // theta == phi (FUN_0015B6A0 sets both to the same angle): hard cone, no division by zero.
    lights.lights[1].phi = 1.0f;
    ff::FillLightConstants(lights, camera, &c);
    CHECK(c.lights[1].spot.z == 0.0f);
    CHECK(c.lights[2].attenuation.w == 0.0f);  // unused entries stay zero
}

TEST_CASE("ff: stage constants stop at the first disabled stage") {
    ni::RenderStateDesc state;
    state.stages[0].colorOp = FF_TOP_MODULATE;
    state.stages[0].colorArg1 = FF_TA_TEXTURE;
    state.stages[0].colorArg2 = FF_TA_DIFFUSE;
    state.stages[0].alphaOp = FF_TOP_SELECTARG1;
    state.stages[0].texCoordIndex = 1;
    state.stages[0].transformFlags = FF_TTFF_COUNT2;
    state.stages[0].texture = {7};
    state.stages[0].transform.m[2][0] = 0.5f;
    state.stages[1].colorOp = FF_TOP_DISABLE;
    state.stages[2].colorOp = FF_TOP_ADD;
    ff::FFStageConstants c;
    ff::FillStageConstants(state, &c);
    CHECK(c.stages[0].color.x == FF_TOP_MODULATE);
    CHECK(c.stages[0].color.y == FF_TA_TEXTURE);
    CHECK(c.stages[0].color.z == FF_TA_DIFFUSE);
    CHECK(c.stages[0].alpha.x == FF_TOP_SELECTARG1);
    CHECK(c.stages[0].coord.x == 1);
    CHECK(c.stages[0].coord.y == FF_TTFF_COUNT2);
    CHECK(c.stages[0].coord.z == FF_TA_CURRENT);
    CHECK(c.stages[0].coord.w == 1);
    CHECK(c.stages[0].transform.m[2][0] == 0.5f);
    CHECK(c.stages[1].color.x == FF_TOP_DISABLE);
    CHECK(c.stages[2].color.x == FF_TOP_DISABLE);  // after the cascade ended
    CHECK(c.stages[3].transform.m[3][3] == 1.0f);
}

// ---- FixedFunctionPipeline on the mock device -----------------------------------------------------------------

TEST_CASE("ff pipeline: Init creates the permutations and resources") {
    MockDevice device;
    ff::FixedFunctionPipeline pipeline;
    CHECK_FALSE(pipeline.IsReady());
    CHECK(pipeline.Draw(MakeTriangleItem()) == ff::DrawResult::Failed);
    std::string error;
    REQUIRE(pipeline.Init(device, &error));
    CHECK(pipeline.IsReady());
    REQUIRE(device.shaders.size() == ff::kVertexPermutations + ff::kPixelPermutations);
    CHECK(device.shaders[0] == rhi::ShaderStage::Vertex);
    CHECK(device.shaders[3] == rhi::ShaderStage::Pixel);
    REQUIRE(device.buffers.size() == 5);  // b0..b3 + default streams
    CHECK(device.buffers[0].size == sizeof(ff::FFFrameConstants));
    CHECK(device.buffers[0].bind == rhi::kBindConstant);
    CHECK(device.buffers[0].usage == rhi::BufferUsage::Dynamic);
    CHECK(device.buffers[4].bind == rhi::kBindVertex);
    REQUIRE(device.bufferInit[4].size() == 48);
    const float* defaults = reinterpret_cast<const float*>(device.bufferInit[4].data());
    CHECK(defaults[0] == 0.0f);  // normal
    CHECK(defaults[4] == 1.0f);  // color white
    CHECK(defaults[7] == 1.0f);
    CHECK(defaults[8] == 0.0f);  // uv
    REQUIRE(device.textures.size() == 1);
    CHECK(device.textures[0].width == 1);
    CHECK(device.textures[0].format == rhi::Format::BGRA8_UNorm);
    pipeline.Shutdown();
    CHECK(device.destroyed == 5 + 5 + 1);
    CHECK_FALSE(pipeline.IsReady());
}

TEST_CASE("ff pipeline: Draw binds streams, constants, textures and draws") {
    MockDevice device;
    ff::FixedFunctionPipeline pipeline;
    REQUIRE(pipeline.Init(device, nullptr));
    ni::CameraDesc camera = IdentityCamera();
    pipeline.SetCamera(camera, 640, 480);

    // Positions with a stride of 20 (gathered), colors tight, two UV sets of which stage 0 reads set 1.
    const float positions[15] = {0, 0, 0, 9, 9, 1, 0, 0, 9, 9, 0, 1, 0, 9, 9};
    const float colors[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    const float uv0[6] = {5, 5, 5, 5, 5, 5};
    const float uv1[6] = {0, 0, 1, 0, 0, 1};
    const std::uint16_t indices[3] = {0, 2, 1};
    ni::DrawItem item = MakeTriangleItem();
    item.geometry.positions = {positions, 20};
    item.geometry.colors = {colors, 16};
    item.geometry.uvs[0] = {uv0, 8};
    item.geometry.uvs[1] = {uv1, 8};
    item.geometry.uvSetCount = 2;
    item.geometry.indices = indices;
    item.state.stages[0].colorOp = FF_TOP_MODULATE;
    item.state.stages[0].texCoordIndex = 1;
    item.state.stages[0].texture = {77};
    item.state.stages[0].niTexture = 0x1234;
    item.state.stages[0].addressU = rhi::AddressMode::Clamp;
    item.state.stencilRef = 9;

    REQUIRE(pipeline.Draw(item) == ff::DrawResult::Drawn);
    MockContext& ctx = device.context;
    CHECK(ctx.drawIndexed == 1);
    CHECK(ctx.lastCount == 3);
    CHECK(ctx.stencilRef == 9);
    REQUIRE(device.pipelines.size() == 1);
    const rhi::PipelineDesc& p = device.pipelines[0];
    CHECK(p.topology == rhi::PrimitiveTopology::TriangleList);
    CHECK(p.layout == ff::MakeVertexLayout());
    CHECK(p.raster.cull == rhi::CullMode::CW);
    CHECK(ctx.pipeline.id != 0);

    // Position stream: gathered to stride 12.
    const auto& pos = ctx.vertexBuffers[FF_STREAM_POSITION];
    CHECK(pos.stride == 12);
    const float* uploaded = reinterpret_cast<const float*>(ctx.Ring(pos));
    REQUIRE(uploaded != nullptr);
    CHECK(uploaded[3] == 1.0f);
    CHECK(uploaded[7] == 1.0f);
    CHECK(uploaded[8] == 0.0f);
    // Normals absent -> default buffer, stride 0; color uploaded; uv1 uploaded, uv0 (unused) default.
    const rhi::BufferHandle defaults{105};
    CHECK(ctx.vertexBuffers[FF_STREAM_NORMAL].buffer == defaults);
    CHECK(ctx.vertexBuffers[FF_STREAM_NORMAL].stride == 0);
    CHECK(ctx.vertexBuffers[FF_STREAM_COLOR].stride == 16);
    CHECK(reinterpret_cast<const float*>(ctx.Ring(ctx.vertexBuffers[FF_STREAM_COLOR]))[5] == 1.0f);
    CHECK(ctx.vertexBuffers[FF_STREAM_UV0].buffer == defaults);
    CHECK(ctx.vertexBuffers[FF_STREAM_UV0].offset == 32);
    CHECK(ctx.vertexBuffers[FF_STREAM_UV0 + 1].stride == 8);
    CHECK(reinterpret_cast<const float*>(ctx.Ring(ctx.vertexBuffers[FF_STREAM_UV0 + 1]))[2] == 1.0f);
    CHECK(ctx.vertexBuffers[FF_STREAM_UV0 + 2].buffer == defaults);
    // Indices.
    CHECK(ctx.indexBuffer == ctx.ringBuffer[1]);
    CHECK(reinterpret_cast<const std::uint16_t*>(ctx.ring[1].data() + ctx.indexOffset)[1] == 2);
    // Constants bound to both stages (lights: vertex shader only) with the expected contents.
    for (int slot = 0; slot < 4; ++slot) {
        CHECK(ctx.constantBuffers[0][slot].id == 101u + slot);
    }
    CHECK(ctx.constantBuffers[1][FF_CB_DRAW].id == 102u);
    CHECK(ctx.constantBuffers[1][FF_CB_LIGHTS].id == 0u);
    const auto* draw = ctx.Constants<ff::FFDrawConstants>({102});
    REQUIRE(draw != nullptr);
    CHECK(draw->features.x == ff::MakeFeatureKey(item));
    const auto* stages = ctx.Constants<ff::FFStageConstants>({104});
    REQUIRE(stages != nullptr);
    CHECK(stages->stages[0].color.x == FF_TOP_MODULATE);
    CHECK(stages->stages[0].coord.x == 1);
    CHECK(stages->stages[0].coord.w == 1);
    CHECK(ctx.Constants<ff::FFFrameConstants>({101}) != nullptr);
    // Textures: stage 0 = the item's texture with its sampler, 1..3 = white with the default sampler.
    CHECK(ctx.textures[0].id == 77u);
    CHECK(ctx.textures[1].id == 201u);
    CHECK(ctx.textures[3].id == 201u);
    REQUIRE(ctx.samplers[0].id != 0);
    CHECK(device.samplers[ctx.samplers[0].id - 401].addressU == rhi::AddressMode::Clamp);
    CHECK(device.samplers[ctx.samplers[1].id - 401].minFilter == rhi::Filter::Point);

    ff::PipelineStats stats = pipeline.Stats();
    CHECK(stats.draws == 1);
    CHECK(stats.vertices == 3);
    CHECK(stats.pipelineStates == 1);
    CHECK(stats.permutations == 2);
    CHECK(stats.frameUploads == 1);
    CHECK(stats.stageUploads == 1);
    CHECK(stats.uploadBytes == 3 * 12 + 3 * 16 + 3 * 8 + 6);

    // Same state again: b0/b3 are not re-uploaded, the pipeline memo is reused.
    REQUIRE(pipeline.Draw(item) == ff::DrawResult::Drawn);
    stats = pipeline.Stats();
    CHECK(stats.frameUploads == 1);
    CHECK(stats.stageUploads == 1);
    CHECK(device.pipelines.size() == 1);
    CHECK(ctx.updateCounts[102] == 2);
    // A fog change re-uploads b0 with the new parameters.
    item.state.fogEnable = true;
    item.state.fogTableMode = FF_FOG_LINEAR;
    item.state.fogStart = 2.0f;
    item.state.fogEnd = 4.0f;
    REQUIRE(pipeline.Draw(item) == ff::DrawResult::Drawn);
    CHECK(pipeline.Stats().frameUploads == 2);
    const auto* frame = ctx.Constants<ff::FFFrameConstants>({101});
    REQUIRE(frame != nullptr);
    CHECK(frame->fogParams.x == 2.0f);
    CHECK(frame->fogParams.w == 0.5f);
    // A camera change keeps the fog values.
    pipeline.SetCamera(camera, 640, 480);
    REQUIRE(pipeline.Draw(item) == ff::DrawResult::Drawn);
    CHECK(pipeline.Stats().frameUploads == 3);
    frame = ctx.Constants<ff::FFFrameConstants>({101});
    CHECK(frame->fogParams.y == 4.0f);
}

TEST_CASE("ff pipeline: lights, strips, missing textures, skips and failures") {
    MockDevice device;
    ff::FixedFunctionPipeline pipeline;
    REQUIRE(pipeline.Init(device, nullptr));
    pipeline.SetCamera(IdentityCamera(), 640, 480);
    MockContext& ctx = device.context;

    ni::DrawItem item = MakeTriangleItem();
    item.state.lightingEnable = true;
    item.lights.count = 1;
    item.lights.lights[0].type = FF_LIGHT_DIRECTIONAL;
    item.lights.lights[0].direction = {0, 0, 1};
    REQUIRE(pipeline.Draw(item) == ff::DrawResult::Drawn);
    CHECK(pipeline.Stats().lightUploads == 1);
    CHECK(pipeline.Stats().litDraws == 1);
    CHECK(ctx.constantBuffers[0][FF_CB_LIGHTS].id == 103u);
    REQUIRE(pipeline.Draw(item) == ff::DrawResult::Drawn);
    CHECK(pipeline.Stats().lightUploads == 1);  // unchanged light set
    item.lights.lights[0].direction = {0, 1, 0};
    REQUIRE(pipeline.Draw(item) == ff::DrawResult::Drawn);
    CHECK(pipeline.Stats().lightUploads == 2);

    // Non-indexed strip, stage with an unresolved NiTexture -> white texture, counted.
    static const float strip[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    ni::DrawItem s = MakeTriangleItem();
    s.geometry.positions = {strip, 12};
    s.geometry.vertexCount = 4;
    s.geometry.indices = nullptr;
    s.geometry.indexCount = 0;
    s.geometry.topology = ni::Topology::TriangleStrip;
    s.state.stages[0].colorOp = FF_TOP_SELECTARG1;
    s.state.stages[0].niTexture = 0x5000;
    REQUIRE(pipeline.Draw(s) == ff::DrawResult::Drawn);
    CHECK(ctx.draws == 1);
    CHECK(ctx.lastCount == 4);
    CHECK(device.pipelines.back().topology == rhi::PrimitiveTopology::TriangleStrip);
    CHECK(ctx.textures[0].id == 201u);
    CHECK(pipeline.Stats().missingTextures == 1);

    // Skips.
    ni::DrawItem empty = MakeTriangleItem();
    empty.geometry.vertexCount = 0;
    CHECK(pipeline.Draw(empty) == ff::DrawResult::Skipped);
    CHECK(std::strcmp(pipeline.LastDrawNote(), "no vertices") == 0);
    empty = MakeTriangleItem();
    empty.geometry.indexCount = 2;
    CHECK(pipeline.Draw(empty) == ff::DrawResult::Skipped);
    CHECK(pipeline.Stats().skipped == 2);

    // Failures: transient ring and pipeline creation.
    ctx.failTransient = true;
    CHECK(pipeline.Draw(MakeTriangleItem()) == ff::DrawResult::Failed);
    CHECK(std::strcmp(pipeline.LastDrawNote(), "transient ring allocation failed") == 0);
    CHECK_FALSE(ctx.open);
    ctx.failTransient = false;
    device.failPipelines = true;
    ni::DrawItem other = MakeTriangleItem();
    other.state.cullMode = rhi::CullMode::None;  // a new pipeline state
    CHECK(pipeline.Draw(other) == ff::DrawResult::Failed);
    CHECK(pipeline.Stats().failed == 2);
    CHECK(std::strcmp(ff::DrawResultName(ff::DrawResult::Skipped), "Skipped") == 0);
}
