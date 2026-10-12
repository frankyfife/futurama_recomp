// D3D8 fixed-function emulation (ff_pipeline.h, GFX_CONTRACT.md 6). Owned by the shader agent.
//
// Per draw: feature key -> shader permutations + rhi pipeline (hash-cached by the backend), guest vertex
// arrays -> transient ring (zero-stride default streams for absent attributes), constants b0..b3
// (b0 only when the camera or the fog values change, b2/b3 only when their contents change), textures
// and samplers of the four stages (a 1x1 white texture on every unused stage), draw.
#include "gfx/ff_pipeline.h"

#include "core/log.h"
#include "gfx/freecam.h"

#include "ff_ps_flat.h"  // gen_shaders/ (fut_add_shader in src/host/CMakeLists.txt)
#include "ff_ps_smooth.h"
#include "ff_stub_ps.h"
#include "ff_stub_vs.h"
#include "ff_vs_lit.h"
#include "ff_vs_pretransformed.h"
#include "ff_vs_transformed.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace gfx::ff {

namespace {

static_assert(FF_FEATURE_VS_COMPILE_MASK == (FF_FEATURE_PRETRANSFORMED | FF_FEATURE_LIGHTING));
static_assert(FF_FEATURE_PS_COMPILE_MASK == FF_FEATURE_FLAT_SHADE);
static_assert(FF_FEATURE_COMPILE_MASK == (FF_FEATURE_VS_COMPILE_MASK | FF_FEATURE_PS_COMPILE_MASK));
// rhi::CompareFunc is passed to the shader as FF_CMP_* (alpha test).
static_assert(static_cast<int>(rhi::CompareFunc::Never) == FF_CMP_NEVER &&
              static_cast<int>(rhi::CompareFunc::LessEqual) == FF_CMP_LESSEQUAL &&
              static_cast<int>(rhi::CompareFunc::GreaterEqual) == FF_CMP_GREATEREQUAL &&
              static_cast<int>(rhi::CompareFunc::Always) == FF_CMP_ALWAYS);

float4 ToFloat4(const Float4& v) noexcept {
    return {v.x, v.y, v.z, v.w};
}

float4x4 ToFloat4x4(const Matrix4& m) noexcept {
    float4x4 r{};
    std::memcpy(r.m, m.m, sizeof(r.m));
    return r;
}

float Saturate(float v) noexcept {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// D3D8 has one set of blend factors for color and alpha; on the alpha channel a color factor acts with its
// alpha component (SRCCOLOR -> source alpha ...), and SRCALPHASAT is (f, f, f, 1) -> ONE. D3D11 rejects
// *_COLOR factors for the alpha channel, so they are mapped to these exact equivalents.
rhi::BlendFactor AlphaChannelFactor(rhi::BlendFactor factor) noexcept {
    switch (factor) {
        case rhi::BlendFactor::SrcColor:
            return rhi::BlendFactor::SrcAlpha;
        case rhi::BlendFactor::InvSrcColor:
            return rhi::BlendFactor::InvSrcAlpha;
        case rhi::BlendFactor::DestColor:
            return rhi::BlendFactor::DestAlpha;
        case rhi::BlendFactor::InvDestColor:
            return rhi::BlendFactor::InvDestAlpha;
        case rhi::BlendFactor::SrcAlphaSat:
            return rhi::BlendFactor::One;
        default:
            return factor;
    }
}

// Default-stream buffer (zero-stride bindings for absent attributes, GFX_CONTRACT.md 6.2).
constexpr std::uint32_t kDefaultNormalOffset = 0;   // float3 (0, 0, 0)
constexpr std::uint32_t kDefaultColorOffset = 16;   // float4 (1, 1, 1, 1): D3D8 diffuse of a vertex without one
constexpr std::uint32_t kDefaultUvOffset = 32;      // float2 (0, 0)
constexpr float kDefaultStreamData[12] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

constexpr std::uint32_t kPositionSize = 12;
constexpr std::uint32_t kNormalSize = 12;
constexpr std::uint32_t kColorSize = 16;
constexpr std::uint32_t kUvSize = 8;

rhi::ShaderBytecode Dxbc(const BYTE* data, std::size_t size) noexcept {
    rhi::ShaderBytecode code;
    code.format = rhi::ShaderFormat::Dxbc;
    code.data = data;
    code.size = size;
    return code;
}

}  // namespace

// ---- pure translations ---------------------------------------------------------------------------------------

std::uint32_t ActiveStageCount(const ni::RenderStateDesc& state) noexcept {
    std::uint32_t count = 0;
    while (count < ni::kMaxTextureStages && state.stages[count].colorOp != FF_TOP_DISABLE) {
        ++count;
    }
    return count;
}

FeatureKey MakeFeatureKey(const ni::DrawItem& item) noexcept {
    const ni::GeometryDesc& geometry = item.geometry;
    const ni::RenderStateDesc& state = item.state;
    FeatureKey key = 0;
    if (geometry.pretransformed) {
        key |= FF_FEATURE_PRETRANSFORMED;  // D3DFVF_XYZRHW: no transform, no lighting
    } else {
        if (item.useScreenCamera) {
            key |= FF_FEATURE_SCREEN_CAMERA;
        }
        if (state.lightingEnable) {
            key |= FF_FEATURE_LIGHTING;
        }
        if (geometry.normals.data != nullptr) {
            key |= FF_FEATURE_HAS_NORMAL;
        }
        if (state.normalizeNormals) {
            key |= FF_FEATURE_NORMALIZE;
        }
    }
    if (geometry.colors.data != nullptr) {
        key |= FF_FEATURE_HAS_COLOR;
    }
    if (state.specularEnable) {
        key |= FF_FEATURE_SPECULAR;
    }
    if (state.alphaTestEnable) {
        key |= FF_FEATURE_ALPHA_TEST;
    }
    if (state.fogEnable) {
        key |= FF_FEATURE_FOG;
    }
    if (state.shadeMode == ni::ShadeMode::Flat) {
        key |= FF_FEATURE_FLAT_SHADE;
    }
    // D3DRS_LOCALVIEWER: REN never writes it (the shadow 0x244870 is only read by the Xbox D3D lighting
    // setup FUN_0023DEA0), so ni_extract reports the device default TRUE.
    if (state.localViewer) {
        key |= FF_FEATURE_LOCAL_VIEWER;
    }
    key |= (ActiveStageCount(state) << FF_FEATURE_STAGE_SHIFT) & FF_FEATURE_STAGE_MASK;
    key |= (std::min<std::uint32_t>(geometry.uvSetCount, FF_MAX_UV_SETS) << FF_FEATURE_UV_SHIFT) & FF_FEATURE_UV_MASK;
    return key;
}

std::uint32_t VertexPermutation(FeatureKey key) noexcept {
    if ((key & FF_FEATURE_PRETRANSFORMED) != 0) {
        return 0;
    }
    return (key & FF_FEATURE_LIGHTING) != 0 ? 2 : 1;
}

std::uint32_t PixelPermutation(FeatureKey key) noexcept {
    return (key & FF_FEATURE_FLAT_SHADE) != 0 ? 1 : 0;
}

rhi::ShaderBytecode VertexShaderBytecode(FeatureKey key) noexcept {
    switch (VertexPermutation(key)) {
        case 0:
            return Dxbc(g_ff_vs_pretransformed, sizeof(g_ff_vs_pretransformed));
        case 1:
            return Dxbc(g_ff_vs_transformed, sizeof(g_ff_vs_transformed));
        default:
            return Dxbc(g_ff_vs_lit, sizeof(g_ff_vs_lit));
    }
}

rhi::ShaderBytecode PixelShaderBytecode(FeatureKey key) noexcept {
    return PixelPermutation(key) == 1 ? Dxbc(g_ff_ps_flat, sizeof(g_ff_ps_flat))
                                      : Dxbc(g_ff_ps_smooth, sizeof(g_ff_ps_smooth));
}

rhi::ShaderBytecode StubShaderBytecode(rhi::ShaderStage stage) noexcept {
    return stage == rhi::ShaderStage::Vertex ? Dxbc(g_ff_stub_vs, sizeof(g_ff_stub_vs))
                                             : Dxbc(g_ff_stub_ps, sizeof(g_ff_stub_ps));
}

rhi::BlendDesc MakeBlendDesc(const ni::RenderStateDesc& state) noexcept {
    rhi::BlendDesc desc;  // disabled: ONE/ZERO (canonical, so equal states hash equally)
    if (state.alphaBlendEnable) {
        desc.enable = true;
        desc.src = state.srcBlend;
        desc.dst = state.dstBlend;
        desc.op = rhi::BlendOp::Add;  // D3DRS_BLENDOP: REN never sets it (default ADD)
        desc.srcAlpha = AlphaChannelFactor(state.srcBlend);
        desc.dstAlpha = AlphaChannelFactor(state.dstBlend);
        desc.opAlpha = rhi::BlendOp::Add;
    }
    desc.writeMask = 0xF;  // D3DRS_COLORWRITEENABLE: never set by REN
    return desc;
}

rhi::DepthStencilDesc MakeDepthStencilDesc(const ni::RenderStateDesc& state) noexcept {
    rhi::DepthStencilDesc desc;
    // FUN_00159B60 keeps D3DRS_ZENABLE on and switches ZFUNC (LESSEQUAL / ALWAYS) and ZWRITEENABLE.
    desc.depthEnable = true;
    desc.depthWrite = state.zWriteEnable;
    desc.depthFunc = state.zFunc;
    if (state.stencilEnable) {
        desc.stencilEnable = true;
        desc.stencilReadMask = state.stencilMask;  // D3DRS_STENCILMASK (FUN_001599E0)
        desc.stencilWriteMask = 0xFF;              // D3DRS_STENCILWRITEMASK: never set by REN (default all)
        desc.stencilFunc = state.stencilFunc;
        desc.stencilFail = state.stencilFail;
        desc.stencilDepthFail = state.stencilZFail;
        desc.stencilPass = state.stencilPass;
    }
    return desc;
}

rhi::RasterDesc MakeRasterDesc(const ni::RenderStateDesc& state) noexcept {
    rhi::RasterDesc desc;
    desc.cull = state.cullMode;  // D3D8 meaning (FUN_001599E0 table, left-right swap applied by ni_extract)
    // M4b: the freecam can look at level walls from outside, where only their back faces exist. The normal back-face
    // cull (CW, draw mode 0) is dropped then; CCW (the QDOutline shells, NiStencilProperty DRAW_CW) stays, so the
    // outlines are unchanged. Left-right swap (mirrors the table) is not considered - the game never sets it.
    if (desc.cull == rhi::CullMode::CW && freecam::ShowBackFaces()) {
        desc.cull = rhi::CullMode::None;
    }
    desc.fill = state.fillMode;
    desc.scissorEnable = false;
    desc.depthClip = true;
    return desc;
}

rhi::SamplerDesc MakeSamplerDesc(const ni::TextureStageDesc& stage) noexcept {
    rhi::SamplerDesc desc;
    desc.minFilter = stage.minFilter;
    desc.magFilter = stage.magFilter;
    desc.mipFilter = stage.mipFilter;  // None: level 0 only (D3DTEXF_NONE)
    desc.addressU = stage.addressU;
    desc.addressV = stage.addressV;
    desc.addressW = rhi::AddressMode::Wrap;
    desc.maxAnisotropy = 1;
    desc.mipLodBias = 0.0f;  // D3DTSS_MIPMAPLODBIAS: REN never sets it
    desc.maxLod = stage.mipFilter == rhi::MipFilter::None ? 0.0f : 1000.0f;
    return desc;
}

// M3c: anisotropic filtering only where it changes a 3D texture's minification (ff_pipeline.h RenderOptions):
// mip-mapped (bilinear or trilinear) with linear minification and not a screen-space draw. D3D11's anisotropic
// filter interpolates between mip levels, so a bilinear (mip point) texture becomes trilinear. It also magnifies
// linearly: a stage with MAG = POINT keeps its D3D8 filter (review M3c finding 4).
rhi::SamplerDesc MakeSamplerDesc(const ni::TextureStageDesc& stage, std::uint32_t maxAnisotropy,
                                 bool screenSpace) noexcept {
    rhi::SamplerDesc desc = MakeSamplerDesc(stage);
    if (maxAnisotropy > 1 && !screenSpace && stage.mipFilter != rhi::MipFilter::None &&
        stage.minFilter == rhi::Filter::Linear && stage.magFilter == rhi::Filter::Linear) {
        desc.maxAnisotropy = static_cast<std::uint8_t>(std::min<std::uint32_t>(maxAnisotropy, 16));
    }
    return desc;
}

bool UsesSharpNearest(const ni::TextureStageDesc& stage, const RenderOptions& options) noexcept {
    return options.sharpNearest && stage.texture && stage.minFilter == rhi::Filter::Point &&
           stage.magFilter == rhi::Filter::Point;
}

bool UsesAlphaToCoverage(const ni::RenderStateDesc& state, const RenderOptions& options) noexcept {
    if (options.sampleCount <= 1 || !options.alphaToCoverage || !state.alphaTestEnable || state.alphaBlendEnable) {
        return false;
    }
    // Only the threshold comparisons have an edge to smooth; a test that always passes (>= 0, <= 255) or never
    // passes (> 255, < 0) keeps the plain D3D8 behaviour.
    const std::uint32_t ref = state.alphaRef;
    switch (state.alphaFunc) {
    case rhi::CompareFunc::Greater:
        return ref < 255;
    case rhi::CompareFunc::GreaterEqual:
        return ref > 0;
    case rhi::CompareFunc::Less:
        return ref > 0;
    case rhi::CompareFunc::LessEqual:
        return ref < 255;
    default:
        return false;
    }
}

bool ReadsDestinationAlpha(const rhi::BlendDesc& blend) noexcept {
    if (!blend.enable) {
        return false;
    }
    auto reads = [](rhi::BlendFactor f) noexcept {
        return f == rhi::BlendFactor::DestAlpha || f == rhi::BlendFactor::InvDestAlpha ||
               f == rhi::BlendFactor::SrcAlphaSat;  // min(As, 1 - Ad)
    };
    return reads(blend.src) || reads(blend.dst) || reads(blend.srcAlpha) || reads(blend.dstAlpha);
}

rhi::VertexLayoutDesc MakeVertexLayout() noexcept {
    rhi::VertexLayoutDesc layout;
    auto add = [&layout](rhi::VertexSemantic semantic, std::uint8_t index, rhi::VertexFormat format,
                         std::uint8_t stream, std::uint16_t stride) noexcept {
        rhi::VertexAttribute& a = layout.attributes[layout.attributeCount++];
        a.semantic = semantic;
        a.semanticIndex = index;
        a.format = format;
        a.stream = stream;
        a.offset = 0;
        layout.streamStrides[stream] = stride;
    };
    add(rhi::VertexSemantic::Position, 0, rhi::VertexFormat::Float3, FF_STREAM_POSITION, kPositionSize);
    add(rhi::VertexSemantic::Normal, 0, rhi::VertexFormat::Float3, FF_STREAM_NORMAL, kNormalSize);
    add(rhi::VertexSemantic::Color, 0, rhi::VertexFormat::Float4, FF_STREAM_COLOR, kColorSize);
    for (std::uint8_t i = 0; i < FF_MAX_UV_SETS; ++i) {
        add(rhi::VertexSemantic::TexCoord, i, rhi::VertexFormat::Float2, static_cast<std::uint8_t>(FF_STREAM_UV0 + i),
            kUvSize);
    }
    return layout;
}

void FillFrameConstants(const ni::CameraDesc& camera, std::uint32_t targetWidth, std::uint32_t targetHeight,
                        FFFrameConstants* out) noexcept {
    if (out == nullptr) {
        return;
    }
    FFFrameConstants c{};
    c.view = ToFloat4x4(camera.view);
    c.projection = ToFloat4x4(camera.projection);
    c.viewProjection = ToFloat4x4(Multiply(camera.view, camera.projection));
    c.screenProjection = ToFloat4x4(camera.screenProjection);
    // Pre-transformed pixels -> NDC of the current viewport (the integration sets CameraDesc::viewport on
    // the context): ndc.x = 2 (x + 0.5 - vx) / vw - 1, ndc.y = 1 - 2 (y + 0.5 - vy) / vh.
    float vx = camera.viewport.x;
    float vy = camera.viewport.y;
    float vw = camera.viewport.width;
    float vh = camera.viewport.height;
    if (vw <= 0.0f || vh <= 0.0f) {
        vx = 0.0f;
        vy = 0.0f;
        vw = static_cast<float>(targetWidth != 0 ? targetWidth : 1);
        vh = static_cast<float>(targetHeight != 0 ? targetHeight : 1);
    }
    c.pretransform = {2.0f / vw, -2.0f / vh, -1.0f - 2.0f * vx / vw, 1.0f + 2.0f * vy / vh};
    c.fogColor = {0.0f, 0.0f, 0.0f, 1.0f};
    c.fogParams = {0.0f, 1.0f, 1.0f, 1.0f};
    const float w = static_cast<float>(targetWidth);
    const float h = static_cast<float>(targetHeight);
    c.targetSize = {w, h, w > 0.0f ? 1.0f / w : 0.0f, h > 0.0f ? 1.0f / h : 0.0f};
    *out = c;
}

void FillFogConstants(const ni::RenderStateDesc& state, FFFrameConstants* out) noexcept {
    if (out == nullptr) {
        return;
    }
    out->fogColor = {Saturate(state.fogColor.x), Saturate(state.fogColor.y), Saturate(state.fogColor.z), 1.0f};
    const float range = state.fogEnd - state.fogStart;
    out->fogParams = {state.fogStart, state.fogEnd, state.fogDensity, range != 0.0f ? 1.0f / range : 0.0f};
}

void FillDrawConstants(const ni::DrawItem& item, const ni::CameraDesc& camera, FFDrawConstants* out) noexcept {
    if (out == nullptr) {
        return;
    }
    const ni::RenderStateDesc& state = item.state;
    const FeatureKey key = MakeFeatureKey(item);
    FFDrawConstants c{};
    Matrix4 worldView = item.world;
    Matrix4 worldViewProjection = IdentityMatrix();
    if ((key & FF_FEATURE_PRETRANSFORMED) != 0) {
        worldView = IdentityMatrix();
    } else if (item.useScreenCamera) {
        // Slot 54 (FUN_00149F30): view = identity, projection = this+0x394.
        worldViewProjection = Multiply(item.world, camera.screenProjection);
    } else {
        worldView = Multiply(item.world, camera.view);
        worldViewProjection = Multiply(worldView, camera.projection);
    }
    // Normals: inverse transpose of world * view (upper 3x3); a singular matrix gives identity.
    Matrix4 inverse;
    Inverse(worldView, &inverse);
    Matrix4 normalMatrix = Transpose(inverse);
    for (int i = 0; i < 3; ++i) {
        normalMatrix.m[i][3] = 0.0f;
        normalMatrix.m[3][i] = 0.0f;
    }
    normalMatrix.m[3][3] = 1.0f;
    c.world = ToFloat4x4(item.world);
    c.worldView = ToFloat4x4(worldView);
    c.worldViewProjection = ToFloat4x4(worldViewProjection);
    c.normalMatrix = ToFloat4x4(normalMatrix);

    const ni::MaterialDesc& m = state.material;
    c.materialDiffuse = ToFloat4(m.diffuse);
    c.materialAmbient = ToFloat4(m.ambient);
    c.materialSpecular = ToFloat4(m.specular);
    c.materialEmissive = ToFloat4(m.emissive);
    c.materialPower = {m.power, 0.0f, 0.0f, 0.0f};
    // D3DRS_AMBIENT (FUN_0015BA40 packs the sum into a D3DCOLOR; the port clamps instead of wrapping).
    const Float4& ambient = item.lights.ambient;
    c.globalAmbient = {Saturate(ambient.x), Saturate(ambient.y), Saturate(ambient.z), 1.0f};
    c.textureFactor = ToFloat4(state.textureFactor);
    const bool lit = (key & FF_FEATURE_LIGHTING) != 0;
    c.features = {key, lit ? std::min<std::uint32_t>(item.lights.count, FF_MAX_LIGHTS) : 0u,
                  std::min<std::uint32_t>(item.geometry.uvSetCount, FF_MAX_UV_SETS), ActiveStageCount(state)};
    c.colorSources = {state.diffuseSource, state.ambientSource, state.specularSource, state.emissiveSource};
    c.alphaTest = {state.alphaTestEnable ? 1u : 0u, static_cast<std::uint32_t>(state.alphaFunc), state.alphaRef, 0u};
    c.fog = {state.fogEnable ? static_cast<std::uint32_t>(state.fogTableMode) : static_cast<std::uint32_t>(FF_FOG_NONE),
             state.rangeFogEnable ? 1u : 0u, 0u, 0u};
    *out = c;
}

void FillLightConstantsWithView(const ni::LightSetDesc& lights, const Matrix4& view, FFLightConstants* out) noexcept {
    if (out == nullptr) {
        return;
    }
    FFLightConstants c{};
    const std::uint32_t count = std::min<std::uint32_t>(lights.count, FF_MAX_LIGHTS);
    for (std::uint32_t i = 0; i < count; ++i) {
        const ni::LightDesc& in = lights.lights[i];
        FFLight& l = c.lights[i];
        l.diffuse = ToFloat4(in.diffuse);
        l.specular = ToFloat4(in.specular);
        l.ambient = ToFloat4(in.ambient);
        const Float3 position = TransformPoint(in.position, view);
        // D3DLIGHT8::Direction is where the light points; the shader lights with -direction.
        const Float3 direction = Normalize(TransformVector(in.direction, view));
        // FUN_0015B6A0 gives point/spot lights Range 0x5F7FFFFF (~1.8e19); directional lights have none.
        const float range = in.type == FF_LIGHT_DIRECTIONAL ? 0.0f : in.range;
        l.position = {position.x, position.y, position.z, range};
        l.direction = {direction.x, direction.y, direction.z, in.falloff};
        l.attenuation = {in.attenuation0, in.attenuation1, in.attenuation2, static_cast<float>(in.type)};
        // Spot cone: theta = inner, phi = outer full angles (radians).
        const float cosHalfTheta = std::cos(in.theta * 0.5f);
        const float cosHalfPhi = std::cos(in.phi * 0.5f);
        const float span = cosHalfTheta - cosHalfPhi;
        l.spot = {cosHalfTheta, cosHalfPhi, span > 0.0f ? 1.0f / span : 0.0f, 0.0f};
    }
    *out = c;
}

void FillLightConstants(const ni::LightSetDesc& lights, const ni::CameraDesc& camera, FFLightConstants* out) noexcept {
    FillLightConstantsWithView(lights, camera.view, out);
}

void FillStageConstants(const ni::RenderStateDesc& state, FFStageConstants* out) noexcept {
    if (out == nullptr) {
        return;
    }
    FFStageConstants c{};
    const std::uint32_t active = ActiveStageCount(state);
    for (std::uint32_t i = 0; i < FF_MAX_STAGES; ++i) {
        const ni::TextureStageDesc& s = state.stages[i];
        FFStage& d = c.stages[i];
        if (i < active) {
            d.color = {s.colorOp, s.colorArg1, s.colorArg2, s.colorArg0};
            d.alpha = {s.alphaOp, s.alphaArg1, s.alphaArg2, s.alphaArg0};
            d.coord = {s.texCoordIndex, s.transformFlags, s.resultArg, s.texture ? FF_STAGE_TEXTURE : 0u};
            d.transform = ToFloat4x4(s.transform);
        } else {
            d.color = {FF_TOP_DISABLE, FF_TA_TEXTURE, FF_TA_CURRENT, FF_TA_CURRENT};
            d.alpha = {FF_TOP_DISABLE, FF_TA_TEXTURE, FF_TA_CURRENT, FF_TA_CURRENT};
            d.coord = {i, FF_TTFF_DISABLE, FF_TA_CURRENT, 0u};
            d.transform = ToFloat4x4(IdentityMatrix());
        }
    }
    *out = c;
}

const char* DrawResultName(DrawResult result) noexcept {
    switch (result) {
        case DrawResult::Drawn:
            return "Drawn";
        case DrawResult::Skipped:
            return "Skipped";
        case DrawResult::Failed:
            return "Failed";
    }
    return "?";
}

// ---- FixedFunctionPipeline -------------------------------------------------------------------------------------

struct FixedFunctionPipeline::Impl {
    rhi::Device* device = nullptr;
    rhi::ShaderHandle vertexShaders[kVertexPermutations];
    rhi::ShaderHandle pixelShaders[kPixelPermutations];
    bool vertexShaderUsed[kVertexPermutations] = {};
    bool pixelShaderUsed[kPixelPermutations] = {};
    rhi::BufferHandle frameBuffer, drawBuffer, lightBuffer, stageBuffer, defaultStreams;
    rhi::TextureHandle whiteTexture;
    rhi::SamplerHandle defaultSampler;
    rhi::VertexLayoutDesc layout;

    ni::CameraDesc camera;
    std::uint32_t targetWidth = 640;
    std::uint32_t targetHeight = 480;
    RenderOptions options;  // M3c: MSAA / alpha-to-coverage / anisotropy of the target
    FFFrameConstants frame{};
    bool frameDirty = true;
    FFLightConstants lastLights{};
    bool lightsValid = false;
    FFStageConstants lastStages{};
    bool stagesValid = false;

    rhi::PipelineDesc lastPipelineDesc;
    rhi::PipelineHandle lastPipeline;
    std::unordered_set<std::uint32_t> seenPipelines;  // statistics: distinct pipeline handles

    PipelineStats stats;
    const char* note = "";
    bool loggedPipelineFailure = false;
    bool loggedUploadFailure = false;

    void Release() noexcept {
        if (device == nullptr) {
            return;
        }
        for (rhi::ShaderHandle& s : vertexShaders) {
            if (s) {
                device->Destroy(s);
            }
            s = {};
        }
        for (rhi::ShaderHandle& s : pixelShaders) {
            if (s) {
                device->Destroy(s);
            }
            s = {};
        }
        for (rhi::BufferHandle* b : {&frameBuffer, &drawBuffer, &lightBuffer, &stageBuffer, &defaultStreams}) {
            if (*b) {
                device->Destroy(*b);
            }
            *b = {};
        }
        if (whiteTexture) {
            device->Destroy(whiteTexture);
        }
        whiteTexture = {};
        device = nullptr;
    }

    // One vertex stream: guest array (or arena) -> transient ring, tightly packed; absent -> zero-stride default.
    bool BindStream(rhi::CommandContext& ctx, std::uint32_t stream, const ni::VertexStreamView& view,
                    std::uint32_t count, std::uint32_t elementSize, std::uint32_t defaultOffset) noexcept {
        if (view.data == nullptr) {
            ctx.SetVertexBuffer(stream, defaultStreams, defaultOffset, 0);
            return true;
        }
        const std::uint32_t bytes = count * elementSize;
        const rhi::TransientSpan span = ctx.BeginTransient(rhi::TransientKind::Vertex, bytes, 16);
        if (span.cpu == nullptr) {
            return false;  // nothing is open after a failed BeginTransient
        }
        const auto* src = static_cast<const std::uint8_t*>(view.data);
        auto* dst = static_cast<std::uint8_t*>(span.cpu);
        if (view.stride == elementSize || view.stride == 0) {
            if (view.stride == 0) {
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::memcpy(dst + static_cast<std::size_t>(i) * elementSize, src, elementSize);
                }
            } else {
                std::memcpy(dst, src, bytes);
            }
        } else {
            for (std::uint32_t i = 0; i < count; ++i) {
                std::memcpy(dst + static_cast<std::size_t>(i) * elementSize,
                            src + static_cast<std::size_t>(i) * view.stride, elementSize);
            }
        }
        ctx.EndTransient();
        ctx.SetVertexBuffer(stream, span.buffer, span.offset, elementSize);
        stats.uploadBytes += bytes;
        return true;
    }

    DrawResult Fail(const char* what) noexcept {
        note = what;
        ++stats.failed;
        return DrawResult::Failed;
    }

    DrawResult Skip(const char* what) noexcept {
        note = what;
        ++stats.skipped;
        return DrawResult::Skipped;
    }

    DrawResult Draw(const ni::DrawItem& item) noexcept;
};

DrawResult FixedFunctionPipeline::Impl::Draw(const ni::DrawItem& item) noexcept {
    const ni::GeometryDesc& geometry = item.geometry;
    const ni::RenderStateDesc& state = item.state;
    note = "";
    if (geometry.vertexCount == 0 || geometry.positions.data == nullptr) {
        return Skip("no vertices");
    }
    const bool indexed = geometry.indices != nullptr;
    const std::uint32_t elementCount = indexed ? geometry.indexCount : geometry.vertexCount;
    if (elementCount < 3) {
        return Skip(indexed ? "fewer than 3 indices" : "fewer than 3 vertices");
    }

    rhi::CommandContext& ctx = device->Context();
    const FeatureKey key = MakeFeatureKey(item);
    const std::uint32_t vsIndex = VertexPermutation(key);
    const std::uint32_t psIndex = PixelPermutation(key);
    if (!vertexShaderUsed[vsIndex]) {
        vertexShaderUsed[vsIndex] = true;
        ++stats.permutations;
    }
    if (!pixelShaderUsed[psIndex]) {
        pixelShaderUsed[psIndex] = true;
        ++stats.permutations;
    }

    // Pipeline state (one-entry memo in front of the backend's hash cache).
    rhi::PipelineDesc desc;
    desc.vertexShader = vertexShaders[vsIndex];
    desc.pixelShader = pixelShaders[psIndex];
    desc.layout = layout;
    desc.blend = MakeBlendDesc(state);
    desc.depthStencil = MakeDepthStencilDesc(state);
    desc.raster = MakeRasterDesc(state);
    // M3c: MSAA target and alpha-to-coverage for alpha-tested materials (RenderOptions, ff_pipeline.h).
    // Review M3c finding 5: a blend that reads the destination alpha would see the coverage alpha of earlier
    // alpha-to-coverage draws instead of the texel alpha - from then on the D3D8 discard is used again.
    if (ReadsDestinationAlpha(desc.blend)) {
        ++stats.destAlphaBlendDraws;
        if (!stats.alphaToCoverageSuspended && options.alphaToCoverage && options.sampleCount > 1) {
            stats.alphaToCoverageSuspended = true;
            FUT_LOGW(Gfx, "ff pipeline: a draw blends with the destination alpha (src %u, dst %u, src alpha %u, dst alpha "
                     "%u) - alpha to coverage suspended for the rest of the session (it changes the target's alpha)",
                     static_cast<unsigned>(desc.blend.src), static_cast<unsigned>(desc.blend.dst),
                     static_cast<unsigned>(desc.blend.srcAlpha), static_cast<unsigned>(desc.blend.dstAlpha));
        }
    }
    const bool alphaToCoverage = !stats.alphaToCoverageSuspended && UsesAlphaToCoverage(state, options);
    desc.blend.alphaToCoverage = alphaToCoverage;
    desc.raster.multisample = options.sampleCount > 1;
    desc.topology = geometry.topology == ni::Topology::TriangleStrip ? rhi::PrimitiveTopology::TriangleStrip
                                                                     : rhi::PrimitiveTopology::TriangleList;
    const rhi::DeviceInfo& info = device->Info();
    if (info.backBufferFormat != rhi::Format::Unknown) {
        desc.colorFormat = info.backBufferFormat;
    }
    if (info.depthFormat != rhi::Format::Unknown) {
        desc.depthFormat = info.depthFormat;
    }
    if (!lastPipeline || !(desc == lastPipelineDesc)) {
        const rhi::PipelineHandle pipeline = device->GetPipeline(desc, "ff");
        if (!pipeline) {
            if (!loggedPipelineFailure) {
                loggedPipelineFailure = true;
                FUT_LOGE(Gfx, "ff pipeline: GetPipeline failed (key 0x%08X): %s", key, device->LastError());
            }
            return Fail("pipeline creation failed");
        }
        try {
            if (seenPipelines.insert(pipeline.id).second) {
                ++stats.pipelineStates;
            }
        } catch (...) {  // statistics only
        }
        lastPipeline = pipeline;
        lastPipelineDesc = desc;
    }

    // Vertex streams. UV sets are uploaded only when an active stage reads them (PASSTHRU coordinates).
    const std::uint32_t activeStages = ActiveStageCount(state);
    bool uvNeeded[FF_MAX_UV_SETS] = {};
    for (std::uint32_t i = 0; i < activeStages; ++i) {
        const std::uint32_t index = state.stages[i].texCoordIndex;
        if ((index & FF_TCI_MASK) == FF_TCI_PASSTHRU && (index & 0xFFFFu) < FF_MAX_UV_SETS) {
            uvNeeded[index & 0xFFFFu] = true;
        }
    }
    const std::uint32_t count = geometry.vertexCount;
    const bool pretransformed = (key & FF_FEATURE_PRETRANSFORMED) != 0;
    const ni::VertexStreamView noStream{};
    bool ok = BindStream(ctx, FF_STREAM_POSITION, geometry.positions, count, kPositionSize, kDefaultNormalOffset);
    ok = ok && BindStream(ctx, FF_STREAM_NORMAL, pretransformed ? noStream : geometry.normals, count, kNormalSize,
                          kDefaultNormalOffset);
    ok = ok && BindStream(ctx, FF_STREAM_COLOR, geometry.colors, count, kColorSize, kDefaultColorOffset);
    for (std::uint32_t set = 0; set < FF_MAX_UV_SETS && ok; ++set) {
        const bool present = set < geometry.uvSetCount && uvNeeded[set];
        ok = BindStream(ctx, FF_STREAM_UV0 + set, present ? geometry.uvs[set] : noStream, count, kUvSize,
                        kDefaultUvOffset);
    }
    if (ok && indexed) {
        const std::uint32_t bytes = geometry.indexCount * 2u;
        const rhi::TransientSpan span = ctx.BeginTransient(rhi::TransientKind::Index, bytes, 16);
        if (span.cpu == nullptr) {
            ok = false;
        } else {
            std::memcpy(span.cpu, geometry.indices, bytes);
            ctx.EndTransient();
            ctx.SetIndexBuffer(span.buffer, span.offset);
            stats.uploadBytes += bytes;
        }
    }
    if (!ok) {
        if (!loggedUploadFailure) {
            loggedUploadFailure = true;
            FUT_LOGE(Gfx, "ff pipeline: transient ring allocation failed (%u vertices, %u indices): %s", count,
                     geometry.indexCount, device->LastError());
        }
        return Fail("transient ring allocation failed");
    }

    // Constants.
    if ((key & FF_FEATURE_FOG) != 0) {
        FFFrameConstants fog = frame;
        FillFogConstants(state, &fog);
        if (std::memcmp(&fog.fogColor, &frame.fogColor, sizeof(float4)) != 0 ||
            std::memcmp(&fog.fogParams, &frame.fogParams, sizeof(float4)) != 0) {
            frame.fogColor = fog.fogColor;
            frame.fogParams = fog.fogParams;
            frameDirty = true;
        }
    }
    if (frameDirty) {
        if (!ctx.UpdateBuffer(frameBuffer, &frame, sizeof(frame))) {
            return Fail("frame constant upload failed");
        }
        frameDirty = false;
        ++stats.frameUploads;
    }
    FFDrawConstants draw;
    FillDrawConstants(item, camera, &draw);
    if (alphaToCoverage) {
        draw.alphaTest.w = 1u;  // the shader writes the sharpened coverage alpha instead of discarding
    }
    if (!ctx.UpdateBuffer(drawBuffer, &draw, sizeof(draw))) {
        return Fail("draw constant upload failed");
    }
    if (draw.features.y != 0) {
        FFLightConstants lights;
        FillLightConstantsWithView(item.lights, item.useScreenCamera ? IdentityMatrix() : camera.view, &lights);
        if (!lightsValid || std::memcmp(&lights, &lastLights, sizeof(lights)) != 0) {
            if (!ctx.UpdateBuffer(lightBuffer, &lights, sizeof(lights))) {
                return Fail("light constant upload failed");
            }
            lastLights = lights;
            lightsValid = true;
            ++stats.lightUploads;
        }
    }
    FFStageConstants stages;
    FillStageConstants(state, &stages);
    for (std::uint32_t i = 0; i < activeStages && i < FF_MAX_STAGES; ++i) {
        if (UsesSharpNearest(state.stages[i], options)) {  // M3c: anti-aliased nearest (RenderOptions)
            stages.stages[i].coord.w |= FF_STAGE_SHARP_NEAREST;
        }
    }
    if (!stagesValid || std::memcmp(&stages, &lastStages, sizeof(stages)) != 0) {
        if (!ctx.UpdateBuffer(stageBuffer, &stages, sizeof(stages))) {
            return Fail("stage constant upload failed");
        }
        lastStages = stages;
        stagesValid = true;
        ++stats.stageUploads;
    }
    constexpr std::uint8_t kBothStages = rhi::kStageVertex | rhi::kStagePixel;
    ctx.SetConstantBuffer(kBothStages, FF_CB_FRAME, frameBuffer);
    ctx.SetConstantBuffer(kBothStages, FF_CB_DRAW, drawBuffer);
    ctx.SetConstantBuffer(rhi::kStageVertex, FF_CB_LIGHTS, lightBuffer);
    ctx.SetConstantBuffer(kBothStages, FF_CB_STAGES, stageBuffer);

    // Textures and samplers of the four stages (white texture + default sampler where nothing is bound).
    const bool screenSpace = pretransformed || item.useScreenCamera;  // slots 55/54: UI and intro screens
    for (std::uint32_t i = 0; i < FF_MAX_STAGES; ++i) {
        rhi::TextureHandle texture = whiteTexture;
        rhi::SamplerHandle sampler = defaultSampler;
        if (i < activeStages) {
            const ni::TextureStageDesc& stage = state.stages[i];
            if (stage.texture) {
                texture = stage.texture;
            } else if (stage.niTexture != 0) {
                ++stats.missingTextures;
            }
            rhi::SamplerDesc samplerDesc = MakeSamplerDesc(stage, options.maxAnisotropy, screenSpace);
            if (samplerDesc.maxAnisotropy > 1) {
                ++stats.anisotropicStages;
            }
            if (UsesSharpNearest(stage, options)) {  // M3c: the shader keeps the texels flat, the sampler blends seams
                samplerDesc.minFilter = rhi::Filter::Linear;
                samplerDesc.magFilter = rhi::Filter::Linear;
                ++stats.sharpNearestStages;
            }
            const rhi::SamplerHandle s = device->GetSampler(samplerDesc);
            if (s) {
                sampler = s;
            }
        }
        ctx.SetTexture(i, texture);
        ctx.SetSampler(i, sampler);
    }

    ctx.SetPipeline(lastPipeline);
    ctx.SetStencilRef(state.stencilRef);
    if (indexed) {
        ctx.DrawIndexed(geometry.indexCount, 0, 0);
    } else {
        ctx.Draw(geometry.vertexCount, 0);
    }
    ++stats.draws;
    stats.vertices += geometry.vertexCount;
    if ((key & FF_FEATURE_LIGHTING) != 0) {
        ++stats.litDraws;
    }
    if (pretransformed) {
        ++stats.pretransformedDraws;
    }
    if (state.alphaTestEnable) {
        ++(alphaToCoverage ? stats.alphaToCoverageDraws : stats.alphaTestDiscardDraws);
    }
    return DrawResult::Drawn;
}

FixedFunctionPipeline::FixedFunctionPipeline() noexcept = default;

FixedFunctionPipeline::~FixedFunctionPipeline() {
    Shutdown();
}

bool FixedFunctionPipeline::Init(rhi::Device& device, std::string* error) noexcept {
    Shutdown();
    auto fail = [&](const char* what) noexcept {
        if (error != nullptr) {
            try {
                *error = std::string("fixed-function pipeline: ") + what + ": " + device.LastError();
            } catch (...) {
            }
        }
        if (impl_ != nullptr) {
            impl_->Release();
            impl_.reset();
        }
        return false;
    };
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
        return fail("out of memory");
    }
    Impl& d = *impl_;
    d.device = &device;
    static const char* const kVertexNames[kVertexPermutations] = {"ff_vs_pretransformed", "ff_vs_transformed",
                                                                  "ff_vs_lit"};
    static const FeatureKey kVertexKeys[kVertexPermutations] = {FF_FEATURE_PRETRANSFORMED, 0, FF_FEATURE_LIGHTING};
    for (std::uint32_t i = 0; i < kVertexPermutations; ++i) {
        d.vertexShaders[i] = device.CreateShader(rhi::ShaderStage::Vertex, VertexShaderBytecode(kVertexKeys[i]),
                                                 kVertexNames[i]);
        if (!d.vertexShaders[i]) {
            return fail("vertex shader creation failed");
        }
    }
    static const char* const kPixelNames[kPixelPermutations] = {"ff_ps_smooth", "ff_ps_flat"};
    static const FeatureKey kPixelKeys[kPixelPermutations] = {0, FF_FEATURE_FLAT_SHADE};
    for (std::uint32_t i = 0; i < kPixelPermutations; ++i) {
        d.pixelShaders[i] = device.CreateShader(rhi::ShaderStage::Pixel, PixelShaderBytecode(kPixelKeys[i]),
                                                kPixelNames[i]);
        if (!d.pixelShaders[i]) {
            return fail("pixel shader creation failed");
        }
    }
    auto constantBuffer = [&device](std::uint32_t size, const char* name) noexcept {
        rhi::BufferDesc desc;
        desc.size = size;
        desc.bind = rhi::kBindConstant;
        desc.usage = rhi::BufferUsage::Dynamic;
        desc.debugName = name;
        return device.CreateBuffer(desc);
    };
    d.frameBuffer = constantBuffer(sizeof(FFFrameConstants), "ff b0 frame");
    d.drawBuffer = constantBuffer(sizeof(FFDrawConstants), "ff b1 draw");
    d.lightBuffer = constantBuffer(sizeof(FFLightConstants), "ff b2 lights");
    d.stageBuffer = constantBuffer(sizeof(FFStageConstants), "ff b3 stages");
    if (!d.frameBuffer || !d.drawBuffer || !d.lightBuffer || !d.stageBuffer) {
        return fail("constant buffer creation failed");
    }
    rhi::BufferDesc streams;
    streams.size = sizeof(kDefaultStreamData);
    streams.bind = rhi::kBindVertex;
    streams.usage = rhi::BufferUsage::Static;
    streams.initialData = kDefaultStreamData;
    streams.debugName = "ff default streams";
    d.defaultStreams = device.CreateBuffer(streams);
    if (!d.defaultStreams) {
        return fail("default stream buffer creation failed");
    }
    static const std::uint32_t kWhite = 0xFFFFFFFFu;
    rhi::TextureDesc white;
    white.width = 1;
    white.height = 1;
    white.mipLevels = 1;
    white.format = rhi::Format::BGRA8_UNorm;
    white.usage = rhi::kTextureSampled;
    white.debugName = "ff white";
    rhi::SubresourceData level;
    level.data = &kWhite;
    level.rowPitch = 4;
    level.slicePitch = 4;
    d.whiteTexture = device.CreateTexture(white, &level);
    if (!d.whiteTexture) {
        return fail("white texture creation failed");
    }
    rhi::SamplerDesc pointClamp;
    pointClamp.minFilter = rhi::Filter::Point;
    pointClamp.magFilter = rhi::Filter::Point;
    pointClamp.addressU = pointClamp.addressV = pointClamp.addressW = rhi::AddressMode::Clamp;
    pointClamp.maxLod = 0.0f;
    d.defaultSampler = device.GetSampler(pointClamp);
    if (!d.defaultSampler) {
        return fail("default sampler creation failed");
    }
    d.layout = MakeVertexLayout();
    FillFrameConstants(d.camera, d.targetWidth, d.targetHeight, &d.frame);
    d.frameDirty = true;
    return true;
}

void FixedFunctionPipeline::Shutdown() noexcept {
    if (impl_ != nullptr) {
        impl_->Release();
        impl_.reset();
    }
}

bool FixedFunctionPipeline::IsReady() const noexcept {
    return impl_ != nullptr && impl_->device != nullptr;
}

void FixedFunctionPipeline::SetCamera(const ni::CameraDesc& camera, std::uint32_t targetWidth,
                                      std::uint32_t targetHeight) noexcept {
    if (impl_ == nullptr) {
        return;
    }
    Impl& d = *impl_;
    d.camera = camera;
    d.targetWidth = targetWidth;
    d.targetHeight = targetHeight;
    const float4 fogColor = d.frame.fogColor;
    const float4 fogParams = d.frame.fogParams;
    FillFrameConstants(camera, targetWidth, targetHeight, &d.frame);
    d.frame.fogColor = fogColor;  // fog values belong to the draws (FillFogConstants)
    d.frame.fogParams = fogParams;
    d.frameDirty = true;
    d.lightsValid = false;  // view-space lights depend on the camera
}

void FixedFunctionPipeline::SetRenderOptions(const RenderOptions& options) noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->options = options;
    impl_->lastPipeline = {};  // blend/raster descriptors depend on the options
}

RenderOptions FixedFunctionPipeline::GetRenderOptions() const noexcept {
    return impl_ != nullptr ? impl_->options : RenderOptions{};
}

DrawResult FixedFunctionPipeline::Draw(const ni::DrawItem& item) noexcept {
    if (impl_ == nullptr || impl_->device == nullptr) {
        return DrawResult::Failed;
    }
    return impl_->Draw(item);
}

PipelineStats FixedFunctionPipeline::Stats() const noexcept {
    return impl_ != nullptr ? impl_->stats : PipelineStats{};
}

const char* FixedFunctionPipeline::LastDrawNote() const noexcept {
    return impl_ != nullptr ? impl_->note : "pipeline not initialized";
}

void FixedFunctionPipeline::InvalidateState() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->frameDirty = true;
    impl_->lightsValid = false;
    impl_->stagesValid = false;
    impl_->lastPipeline = {};
}

}  // namespace gfx::ff
