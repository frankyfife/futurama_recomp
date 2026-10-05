// D3D8 fixed-function emulation on top of the RHI (GFX_CONTRACT.md 6). Owned by the shader agent.
//
// Input: ni::DrawItem / ni::CameraDesc (what NiXBoxRenderer wrote into D3D8, gfx/ni_extract.h).
// Output: rhi pipeline state (hash-cached), constant buffers (gfx/shaders/ff_shared.h), vertex streams
// (transient ring uploads of the guest arrays, zero-stride defaults for absent attributes) and the draw.
// Shaders: HLSL in gfx/shaders/ff_ubershader.hlsl, compiled at build time by fxc (src/host/CMakeLists.txt,
// fut_add_shader) into gen_shaders/ff_vs_*.h / ff_ps_*.h (`g_<name>` DXBC arrays); permutations by
// FeatureKey & FF_FEATURE_VS_COMPILE_MASK (pretransformed / transformed / transformed + lit) and
// FF_FEATURE_PS_COMPILE_MASK (smooth / flat), the rest is uniform branching on FFDrawConstants::features.
//
// D3D8 semantics reproduced (reference: the D3D8 fixed-function specification plus the states REN sets,
// renderer.md 5.4/5.5; differences are listed in GFX_CONTRACT.md 11 and the result report):
//   - transform: clip = float4(pos, 1) * world * view * projection; screen camera (slot 54): world *
//     screenProjection with view = identity; pre-transformed (slot 55): render-target pixels -> NDC of the
//     current viewport (= CameraDesc::viewport) with +0.5 (D3D8 pixel centers on integers).
//   - lighting (D3DRS_LIGHTING, per vertex, view space): emissive + ambient * (D3DRS_AMBIENT + sum of light
//     ambients) + diffuse * sum(N.L * light diffuse), specular sum(pow(N.H, Power) * light specular) with
//     D3DRS_SPECULARENABLE; material color sources D3DMCS_* (COLOR1 = vertex color if present, else the
//     material; COLOR2 = material, there is no specular stream); point/spot/directional lights with
//     attenuation 1 / (a0 + a1 d + a2 d^2), Range, the D3D spot cone (theta/phi/falloff); normals through
//     the inverse transpose of world * view, normalized with D3DRS_NORMALIZENORMALS (FUN_00159200);
//     D3DRS_LOCALVIEWER (RenderStateDesc::localViewer; REN never writes it). Without lighting the
//     vertex color (or white) is the diffuse color and the specular color is 0.
//   - texture stages: COLOROP/ALPHAOP/args/RESULTARG per stage (FF_TOP_* = PC D3D8 numbering; ni_extract
//     translates REN's Xbox codes, ff_shared.h FF_XBOX_TOP_*), every D3D8 op except the bump-env
//     perturbation and PREMODULATE (P2), arg modifiers COMPLEMENT/ALPHAREPLICATE; texture coordinates from
//     TEXCOORDINDEX (set + CAMERASPACE* generation) and the texture transform (TTFF COUNTn / PROJECTED).
//   - D3DRS_SPECULARENABLE adds the specular color after the stages; alpha test (D3DRS_ALPHAFUNC against
//     ALPHAREF on the 8-bit alpha); table fog LINEAR/EXP/EXP2 on view z (radial distance with
//     RANGEFOGENABLE), color D3DRS_FOGCOLOR; flat shading (first vertex = D3D11 provoking vertex).
//   - NiTexturingProperty apply modes reach the shader only as stage states (ni_extract reproduces them).
//     REN's base map (stage 0, default pipeline vf2 gap code: `push 2` at 0x1643E9 = table row 2, apply
//     mode != REPLACE stored at stage+0x90 at 0x164445, then FUN_001629C0 via 0x1650AA): REPLACE ->
//     SELECTARG1(TEXTURE) for color and alpha (and FUN_0015BF80 turns lighting off); DECAL, MODULATE and
//     the HILIGHT modes -> MODULATE(TEXTURE, DIFFUSE) for color and alpha (row 2 of the base table at
//     pipeline manager +0x108, FUN_0015A120); no texture -> SELECTARG2(DIFFUSE). Further maps use the second
//     table (+0x1F8, args TEXTURE/CURRENT): row 0 ADD (glow), 1 Xbox 0xE = BLENDTEXTUREALPHA (decal),
//     2 MODULATE (dark), 3 MODULATE2X (detail), 8 BUMPENVMAP(LUMINANCE) by caps. All of these are covered
//     by the ubershader and by tests/ff_render_tests.cpp.
//   - render states -> rhi descriptors: alpha blend (D3D8 has no separate alpha factors: the alpha channel
//     uses the alpha equivalents of color factors, SRCALPHASAT -> ONE), z test/write, stencil (write mask
//     0xFF: REN never sets D3DRS_STENCILWRITEMASK), cull mode (D3D8 meaning, rhi::CullMode), fill mode.
#pragma once

#include "gfx/ni_extract.h"
#include "gfx/rhi/rhi.h"
#include "gfx/shaders/ff_shared.h"

#include <cstdint>
#include <memory>
#include <string>

namespace gfx::ff {

using FeatureKey = std::uint32_t;  // FF_FEATURE_* bits

// Pure translations (unit-testable without a device, tests/ff_*.cpp).
FeatureKey MakeFeatureKey(const ni::DrawItem& item) noexcept;
rhi::BlendDesc MakeBlendDesc(const ni::RenderStateDesc& state) noexcept;
rhi::DepthStencilDesc MakeDepthStencilDesc(const ni::RenderStateDesc& state) noexcept;
rhi::RasterDesc MakeRasterDesc(const ni::RenderStateDesc& state) noexcept;
rhi::SamplerDesc MakeSamplerDesc(const ni::TextureStageDesc& stage) noexcept;
// The vertex layout of every ff draw (streams FF_STREAM_*, ff_shared.h).
rhi::VertexLayoutDesc MakeVertexLayout() noexcept;
// View/projection/screen matrices and the pre-transform of `camera` (fog fields reset: color 0, params
// (0, 1, 1, 1); FillFogConstants sets them per draw).
void FillFrameConstants(const ni::CameraDesc& camera, std::uint32_t targetWidth, std::uint32_t targetHeight,
                        FFFrameConstants* out) noexcept;
void FillDrawConstants(const ni::DrawItem& item, const ni::CameraDesc& camera, FFDrawConstants* out) noexcept;
// Lights to view space (D3D8 lights are given in world space and lit in camera space).
void FillLightConstants(const ni::LightSetDesc& lights, const ni::CameraDesc& camera, FFLightConstants* out) noexcept;
void FillStageConstants(const ni::RenderStateDesc& state, FFStageConstants* out) noexcept;

// ---- added by the shader agent (M3 phase 2) --------------------------------------------------------------

// Fog color and parameters of one draw into the frame constants (D3DRS_FOGCOLOR, FOGSTART, FOGEND,
// FOGDENSITY as FUN_00159700/FUN_00159900 set them): fogColor = (rgb, 1), fogParams = (start, end,
// density, 1 / (end - start) or 0 when start == end). The pipeline re-uploads b0 when they change.
void FillFogConstants(const ni::RenderStateDesc& state, FFFrameConstants* out) noexcept;
// Lights with an explicit view matrix (identity for the screen camera of slot 54).
void FillLightConstantsWithView(const ni::LightSetDesc& lights, const Matrix4& view, FFLightConstants* out) noexcept;

// Number of stages the D3D8 cascade evaluates: stages before the first colorOp == FF_TOP_DISABLE (0..4).
std::uint32_t ActiveStageCount(const ni::RenderStateDesc& state) noexcept;
// Permutation index of a feature key: vertex shader 0 pretransformed, 1 transformed, 2 transformed + lit;
// pixel shader 0 smooth, 1 flat.
std::uint32_t VertexPermutation(FeatureKey key) noexcept;
std::uint32_t PixelPermutation(FeatureKey key) noexcept;
inline constexpr std::uint32_t kVertexPermutations = 3;
inline constexpr std::uint32_t kPixelPermutations = 2;
// Embedded DXBC (build-time fxc output) of the permutation selected by `key`.
rhi::ShaderBytecode VertexShaderBytecode(FeatureKey key) noexcept;
rhi::ShaderBytecode PixelShaderBytecode(FeatureKey key) noexcept;
// The phase-1 stub shaders (gfx/shaders/ff_stub.hlsl) for RHI tests that cannot include gen_shaders/.
rhi::ShaderBytecode StubShaderBytecode(rhi::ShaderStage stage) noexcept;

struct PipelineStats {
    std::uint64_t draws = 0;
    std::uint64_t skipped = 0;        // DrawResult::Skipped
    std::uint64_t failed = 0;
    std::uint64_t vertices = 0;
    std::uint64_t uploadBytes = 0;
    std::uint32_t permutations = 0;   // shader permutations in use
    std::uint32_t pipelineStates = 0; // distinct rhi pipelines requested
    // added by the shader agent (M3 phase 2)
    std::uint64_t litDraws = 0;           // draws with D3DRS_LIGHTING
    std::uint64_t pretransformedDraws = 0;
    std::uint64_t frameUploads = 0;       // b0 uploads (SetCamera, fog changes)
    std::uint64_t lightUploads = 0;       // b2 uploads (skipped when the light set is unchanged)
    std::uint64_t stageUploads = 0;       // b3 uploads (skipped when the stage setup is unchanged)
    std::uint64_t missingTextures = 0;    // active stages whose NiTexture had no host texture (white bound)
};

enum class DrawResult : std::uint8_t {
    Drawn,
    Skipped,  // nothing to draw (0 vertices/indices) or a stage texture is missing and the item asks to skip
    Failed,   // device error (logged once by the pipeline)
};
const char* DrawResultName(DrawResult result) noexcept;  // "Drawn", "Skipped", "Failed"

class FixedFunctionPipeline {
public:
    FixedFunctionPipeline() noexcept;
    ~FixedFunctionPipeline();
    FixedFunctionPipeline(const FixedFunctionPipeline&) = delete;
    FixedFunctionPipeline& operator=(const FixedFunctionPipeline&) = delete;

    // Creates shaders (embedded DXBC), constant buffers and the default streams on `device`.
    bool Init(rhi::Device& device, std::string* error) noexcept;
    void Shutdown() noexcept;
    bool IsReady() const noexcept;

    // After SetCameraData (slot 47) and after every render-target change (slot 15).
    void SetCamera(const ni::CameraDesc& camera, std::uint32_t targetWidth, std::uint32_t targetHeight) noexcept;
    // Translates and draws one item on device.Context(). Texture handles must be resolved already.
    DrawResult Draw(const ni::DrawItem& item) noexcept;

    PipelineStats Stats() const noexcept;
    // Why the last Draw was Skipped/Failed ("" after Drawn): static text, valid until the next Draw.
    const char* LastDrawNote() const noexcept;
    // Forget the cached constant-buffer contents and bindings (e.g. after something else used the context).
    void InvalidateState() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gfx::ff
