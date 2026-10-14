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
// (0, 1, 1, 1); FillFogConstants sets them per draw). `pixelCenterX/Y` = the B19 pixel-center shift in viewport
// pixels the projections carry (FFFrameConstants::pixelCenter: pre-transformed quads and the 1/16-pixel snap).
void FillFrameConstants(const ni::CameraDesc& camera, std::uint32_t targetWidth, std::uint32_t targetHeight,
                        FFFrameConstants* out, float pixelCenterX = 0.5f, float pixelCenterY = 0.5f) noexcept;
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

// ---- M3c (render resolution and anti-aliasing; added by the renderer agent) ----------------------------------
//
// Options of the target the pipeline draws into ([render] msaa / alpha_to_coverage / anisotropy). The default
// is the M3 behaviour (single sample, no A2C, no anisotropic filtering) - the "classic" configuration renders
// bit-identically to M3.
//   - sampleCount > 1: the raster state is multisampled (rhi::RasterDesc::multisample).
//   - alphaToCoverage (only with sampleCount > 1): alpha-tested materials that do not blend use
//     alpha-to-coverage instead of the D3D8 alpha test's discard. The shader turns the tested alpha into a
//     coverage value around the test threshold, sharpened over one pixel (alpha - threshold) / fwidth(alpha)
//     + 0.5, so cut-out edges (grates, plants, fences) get MSAA coverage instead of a stair step without
//     becoming blurry or thinner/thicker (the 50 % coverage line lies exactly where the alpha test switches).
//     Alpha-tested materials WITH blending keep the discard (their blend already softens the edge).
//     Review M3c finding 5: alpha-to-coverage writes the coverage alpha (inside mostly 1.0) into the target's
//     alpha channel where the D3D8 alpha test leaves the texel alpha. Nothing visible changes unless a later draw
//     blends with the destination alpha (DESTALPHA, INVDESTALPHA, SRCALPHASAT); the first such draw suspends
//     alpha-to-coverage for the rest of the session (GFX warning, PipelineStats::alphaToCoverageSuspended).
//   - maxAnisotropy > 1: anisotropic filtering for mip-mapped textures with linear minification AND
//     magnification (bilinear or trilinear) on transformed (3D) geometry; point-filtered (also MAG = POINT, which
//     D3D11's anisotropic filter would magnify linearly - review M3c finding 4), non-mip-mapped textures and the
//     screen-space draws of slots 54/55 (UI, intro screens) keep their D3D8 filter.
//   - sharpNearest (the target is larger than the guest's 640x480): textures with point minification and
//     magnification are sampled as "anti-aliased nearest" (FF_STAGE_SHARP_NEAREST in ff_shared.h: bilinear
//     sampler, coordinates on the texel centers except one pixel at each texel border), so a point-filtered
//     640x480 image keeps texels of equal size at a non-integer scale (4.5 at 2880x2160) instead of alternating
//     4- and 5-pixel columns, and box-downscaled it is the original again.
struct RenderOptions {
    std::uint32_t sampleCount = 1;
    bool alphaToCoverage = false;
    std::uint32_t maxAnisotropy = 1;  // 1 = off, 2..16
    bool sharpNearest = false;
    friend bool operator==(const RenderOptions&, const RenderOptions&) = default;
};
// True if `stage` is sampled as anti-aliased nearest under `options` (rule above; a texture must be bound).
bool UsesSharpNearest(const ni::TextureStageDesc& stage, const RenderOptions& options) noexcept;
// The sampler of a stage with anisotropic filtering where the rule above allows it (`screenSpace` = draw of
// slot 54/55). maxAnisotropy <= 1 gives MakeSamplerDesc(stage).
rhi::SamplerDesc MakeSamplerDesc(const ni::TextureStageDesc& stage, std::uint32_t maxAnisotropy,
                                 bool screenSpace) noexcept;
// True if `state` is drawn with alpha-to-coverage under `options` (rule above).
bool UsesAlphaToCoverage(const ni::RenderStateDesc& state, const RenderOptions& options) noexcept;
// G2 fix (menu text boxes): true if `state` blends with SRCALPHA / INVSRCALPHA or SRCALPHA / ONE, has no alpha test
// and writes depth. A fragment with alpha 0 then changes no colour, but under D3D8 rules it still writes depth, so
// the transparent part of every glyph quad of the menu text hid the pulsing highlight drawn after it (darker
// rectangles behind the selected item, with and without texture pack). The Xbox shows no such rectangles (xemu
// reference frames of the main menu), so these draws discard alpha-0 fragments (alpha test GREATER 0, depth
// unchanged there; colour identical).
bool DiscardsTransparentFragments(const ni::RenderStateDesc& state) noexcept;
// Review M3c finding 5: true if blending with `blend` reads the destination alpha (DESTALPHA, INVDESTALPHA or
// SRCALPHASAT in a color or alpha factor).
bool ReadsDestinationAlpha(const rhi::BlendDesc& blend) noexcept;
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
    // added by the renderer agent (M3c)
    std::uint64_t alphaToCoverageDraws = 0;  // alpha-tested draws with alpha-to-coverage
    std::uint64_t alphaTestDiscardDraws = 0; // alpha-tested draws with the D3D8 discard
    std::uint64_t anisotropicStages = 0;     // active stages sampled anisotropically
    std::uint64_t sharpNearestStages = 0;    // active stages sampled as anti-aliased nearest
    std::uint64_t destAlphaBlendDraws = 0;   // review M3c finding 5: draws whose blend reads the destination alpha
    bool alphaToCoverageSuspended = false;   // ... which suspended alpha-to-coverage
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

    // After SetCameraData (slot 47) and after every render-target change (slot 15). `pixelCenterX/Y`: the B19
    // shift the camera's projections carry (0.5; texture targets: gfx::TargetPixelCenter).
    void SetCamera(const ni::CameraDesc& camera, std::uint32_t targetWidth, std::uint32_t targetHeight,
                   float pixelCenterX = 0.5f, float pixelCenterY = 0.5f) noexcept;
    // M3c: options of the current target (MSAA, alpha-to-coverage, anisotropy); default = M3.
    void SetRenderOptions(const RenderOptions& options) noexcept;
    RenderOptions GetRenderOptions() const noexcept;
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
