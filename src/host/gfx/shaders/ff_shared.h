// Fixed-function ubershader interface shared by C++ (ff_pipeline, ni_extract) and HLSL (ff_*.hlsl,
// compiled at build time by fxc, GFX_CONTRACT.md 6). Owned by the shader agent; the layout below is the
// phase-1 contract (changes add-only, logged in INTERFACES.md section 8).
//
// The ubershader emulates the D3D8 fixed-function pipeline as the original NiXBoxRenderer configured it
// (D3D render states, texture stage states, D3DLIGHT8, D3DMATERIAL8). The numeric codes below are the
// PC D3D8 ones. PC and Xbox use the same D3DTA / D3DMCS / D3DFOG / D3DLIGHTTYPE / D3DTTFF / D3DTSS_TCI
// numbers, but NOT the same D3DTEXTUREOP numbers from 13 on (shader agent, M3 phase 2; evidence at
// FF_XBOX_TOP_* below): values REN writes into D3D_g_DeferredTextureState must be translated with
// gfx::ni::TextureOpFromXbox (ni_extract.h) before they are stored as FF_TOP_* codes.
//
// Packing rule: every constant-buffer struct uses only float4, uint4 and float4x4 members, so the HLSL
// packing (16-byte registers) equals the C++ layout; static_asserts below check the sizes. Matrices are
// `row_major` in HLSL and use the row-vector convention of gfx/gfx_math.h: mul(float4(p, 1), M).
//
// Vertex input (one layout for every draw, absent attributes are bound as zero-stride default streams):
//   stream 0 POSITION  float3  (pre-transformed draws: x, y = D3D8 screen pixels, z = depth; rhw = 1)
//   stream 1 NORMAL    float3
//   stream 2 COLOR0    float4  NiColorA r, g, b, a (the original packs ROUND(c * 255) into a D3DCOLOR;
//                              the shader saturates instead of wrapping - documented deviation)
//   stream 3..6 TEXCOORD0..3 float2
#ifndef FUT_GFX_FF_SHARED_H
#define FUT_GFX_FF_SHARED_H

#ifdef __cplusplus
#include <cstdint>
namespace gfx::ff {
struct float4 {
    float x, y, z, w;
};
struct uint4 {
    std::uint32_t x, y, z, w;
};
struct float4x4 {
    float m[4][4];
};
#define FF_ROW_MAJOR
#define FF_UINT std::uint32_t
#else
#define FF_ROW_MAJOR row_major
#define FF_UINT uint
#endif

// ---- limits, slots ---------------------------------------------------------------------------------
#define FF_MAX_LIGHTS 8          // D3D8/Xbox active light limit (FUN_0015BA40 enables per light index)
#define FF_MAX_STAGES 4          // Xbox texture stages; M3 uses stage 0 (base map)
#define FF_MAX_UV_SETS 4         // DAT_002D85D0 = 4

#define FF_CB_FRAME 0            // b0 FFFrameConstants  (per SetCameraData / target change)
#define FF_CB_DRAW 1             // b1 FFDrawConstants   (per draw)
#define FF_CB_LIGHTS 2           // b2 FFLightConstants  (per draw when lighting is on)
#define FF_CB_STAGES 3           // b3 FFStageConstants  (per draw)
// Textures t0..t3 and samplers s0..s3 = texture stages 0..3.

#define FF_STREAM_POSITION 0
#define FF_STREAM_NORMAL 1
#define FF_STREAM_COLOR 2
#define FF_STREAM_UV0 3
#define FF_STREAM_COUNT 7

// ---- feature key (ff::MakeFeatureKey(DrawItem)) ----------------------------------------------------------
// The low bits select precompiled shader permutations (FF_FEATURE_COMPILE_MASK, shader agent's choice
// which ones); the whole key is also passed to the shader in FFDrawConstants::features.x for uniform
// branching on the rest.
#define FF_FEATURE_PRETRANSFORMED 0x00000001u   // D3DFVF_XYZRHW: no transform, no lighting (slot 55 quads)
#define FF_FEATURE_SCREEN_CAMERA 0x00000002u    // slot 54: view = identity, projection = screenProjection
#define FF_FEATURE_LIGHTING 0x00000004u         // D3DRS_LIGHTING
#define FF_FEATURE_HAS_NORMAL 0x00000008u
#define FF_FEATURE_HAS_COLOR 0x00000010u        // vertex diffuse present (D3DFVF_DIFFUSE)
#define FF_FEATURE_SPECULAR 0x00000020u         // D3DRS_SPECULARENABLE
#define FF_FEATURE_NORMALIZE 0x00000040u        // D3DRS_NORMALIZENORMALS (FUN_00159200: scale outside 0.99..1.01)
#define FF_FEATURE_ALPHA_TEST 0x00000080u       // D3DRS_ALPHATESTENABLE
#define FF_FEATURE_FOG 0x00000100u              // D3DRS_FOGENABLE
#define FF_FEATURE_FLAT_SHADE 0x00000200u       // D3DRS_SHADEMODE = FLAT
#define FF_FEATURE_LOCAL_VIEWER 0x00000400u     // D3DRS_LOCALVIEWER (specular half vector)
#define FF_FEATURE_STAGE_SHIFT 12               // 3 bits: number of active texture stages 0..4
#define FF_FEATURE_STAGE_MASK 0x00007000u
#define FF_FEATURE_UV_SHIFT 16                  // 3 bits: number of UV sets bound 0..4
#define FF_FEATURE_UV_MASK 0x00070000u
// Compiled permutations (shader agent, M3 phase 2): the vertex shader is specialised on PRETRANSFORMED
// and LIGHTING (3 variants: pretransformed, transformed, transformed + lit), the pixel shader on
// FLAT_SHADE (nointerpolation colors; 2 variants). Everything else is uniform branching on features.x.
#define FF_FEATURE_VS_COMPILE_MASK 0x00000005u  // PRETRANSFORMED | LIGHTING
#define FF_FEATURE_PS_COMPILE_MASK 0x00000200u  // FLAT_SHADE
#define FF_FEATURE_COMPILE_MASK 0x00000205u     // VS | PS masks (phase 1: 0x5)

// ---- D3D8 codes used in the constants ---------------------------------------------------------------------
// D3DTEXTUREOP (D3DTSS_COLOROP / ALPHAOP)
#define FF_TOP_DISABLE 1
#define FF_TOP_SELECTARG1 2
#define FF_TOP_SELECTARG2 3
#define FF_TOP_MODULATE 4
#define FF_TOP_MODULATE2X 5
#define FF_TOP_MODULATE4X 6
#define FF_TOP_ADD 7
#define FF_TOP_ADDSIGNED 8
#define FF_TOP_ADDSIGNED2X 9
#define FF_TOP_SUBTRACT 10
#define FF_TOP_ADDSMOOTH 11
#define FF_TOP_BLENDDIFFUSEALPHA 12
#define FF_TOP_BLENDTEXTUREALPHA 13
#define FF_TOP_BLENDFACTORALPHA 14
#define FF_TOP_BLENDTEXTUREALPHAPM 15
#define FF_TOP_BLENDCURRENTALPHA 16
#define FF_TOP_PREMODULATE 17
#define FF_TOP_MODULATEALPHA_ADDCOLOR 18
#define FF_TOP_MODULATECOLOR_ADDALPHA 19
#define FF_TOP_MODULATEINVALPHA_ADDCOLOR 20
#define FF_TOP_MODULATEINVCOLOR_ADDALPHA 21
#define FF_TOP_BUMPENVMAP 22
#define FF_TOP_BUMPENVMAPLUMINANCE 23
#define FF_TOP_DOTPRODUCT3 24
#define FF_TOP_MULTIPLYADD 25
#define FF_TOP_LERP 26
// Xbox D3DTEXTUREOP (what REN writes into the deferred texture state; added by the shader agent, M3 phase 2).
// 1..12 and 17..21 equal the PC codes above; the others are renumbered. Evidence: the Xbox D3D combiner
// setup FUN_0023FDB0 dispatches on COLOROP - 1 through the jump table 0x240328: ops 12..15 share the
// handler 0x23FFA4, which turns `op - 12` into the D3DTA source of the blend alpha (0 DIFFUSE, 1 CURRENT,
// 2 TEXTURE, 3 TFACTOR) -> 12 BLENDDIFFUSEALPHA, 13 BLENDCURRENTALPHA, 14 BLENDTEXTUREALPHA, 15
// BLENDFACTORALPHA; op 16 (0x23FFD5: TEXTURE alpha, complemented) = BLENDTEXTUREALPHAPM; ops 25/26 share a
// handler (bump env). REN uses 0xE for decal maps (FUN_0015A120, manager +0x210) and 0x19/0x1A for bump
// maps selected by the D3DTEXOPCAPS_BUMPENVMAP (0x200000) / BUMPENVMAPLUMINANCE (0x400000) caps bits.
// ASSUMPTION (XDK d3d8types.h order, not separately evidenced): 22 DOTPRODUCT3, 23 MULTIPLYADD, 24 LERP.
#define FF_XBOX_TOP_BLENDDIFFUSEALPHA 12
#define FF_XBOX_TOP_BLENDCURRENTALPHA 13
#define FF_XBOX_TOP_BLENDTEXTUREALPHA 14
#define FF_XBOX_TOP_BLENDFACTORALPHA 15
#define FF_XBOX_TOP_BLENDTEXTUREALPHAPM 16
#define FF_XBOX_TOP_PREMODULATE 17
#define FF_XBOX_TOP_DOTPRODUCT3 22
#define FF_XBOX_TOP_MULTIPLYADD 23
#define FF_XBOX_TOP_LERP 24
#define FF_XBOX_TOP_BUMPENVMAP 25
#define FF_XBOX_TOP_BUMPENVMAPLUMINANCE 26
// D3DTSS_TEXCOORDINDEX high word: texture coordinate generation (same numbers on PC and Xbox).
#define FF_TCI_PASSTHRU 0x00000000u
#define FF_TCI_CAMERASPACENORMAL 0x00010000u
#define FF_TCI_CAMERASPACEPOSITION 0x00020000u
#define FF_TCI_CAMERASPACEREFLECTIONVECTOR 0x00030000u
#define FF_TCI_MASK 0xFFFF0000u
// D3DTA (argument select + modifier bits)
#define FF_TA_DIFFUSE 0
#define FF_TA_CURRENT 1
#define FF_TA_TEXTURE 2
#define FF_TA_TFACTOR 3
#define FF_TA_SPECULAR 4
#define FF_TA_TEMP 5
#define FF_TA_SELECTMASK 0x0F
#define FF_TA_COMPLEMENT 0x10
#define FF_TA_ALPHAREPLICATE 0x20
// D3DMATERIALCOLORSOURCE
#define FF_MCS_MATERIAL 0
#define FF_MCS_COLOR1 1
#define FF_MCS_COLOR2 2
// D3DFOGMODE (table/vertex fog mode)
#define FF_FOG_NONE 0
#define FF_FOG_EXP 1
#define FF_FOG_EXP2 2
#define FF_FOG_LINEAR 3
// D3DLIGHTTYPE
#define FF_LIGHT_POINT 1
#define FF_LIGHT_SPOT 2
#define FF_LIGHT_DIRECTIONAL 3
// D3DTEXTURETRANSFORMFLAGS
#define FF_TTFF_DISABLE 0
#define FF_TTFF_COUNT1 1
#define FF_TTFF_COUNT2 2
#define FF_TTFF_COUNT3 3
#define FF_TTFF_COUNT4 4
#define FF_TTFF_PROJECTED 256
// FFStage::coord.w flags (M3c; bit 0 was the phase-1 "texture bound" value 1).
#define FF_STAGE_TEXTURE 0x1u        // a texture is bound to the stage
#define FF_STAGE_SHARP_NEAREST 0x2u  // M3c: point-filtered texture on a target larger than 640x480 - sampled as
                                     // "anti-aliased nearest" (flat texels, linear seams one pixel wide, the stage
                                     // sampler is bilinear): every texel keeps the same size at a non-integer scale
// rhi::CompareFunc order (alpha test function in FFDrawConstants::alphaTest.y)
#define FF_CMP_NEVER 0
#define FF_CMP_LESS 1
#define FF_CMP_EQUAL 2
#define FF_CMP_LESSEQUAL 3
#define FF_CMP_GREATER 4
#define FF_CMP_NOTEQUAL 5
#define FF_CMP_GREATEREQUAL 6
#define FF_CMP_ALWAYS 7

// ---- constant buffers -----------------------------------------------------------------------------------

// b0: camera and target (ff::FixedFunctionPipeline::SetCamera).
struct FFFrameConstants {
    FF_ROW_MAJOR float4x4 view;              // CameraDesc::view (SetCameraData this+0x610)
    FF_ROW_MAJOR float4x4 projection;        // CameraDesc::projection (this+0x650)
    FF_ROW_MAJOR float4x4 viewProjection;    // view * projection
    FF_ROW_MAJOR float4x4 screenProjection;  // CameraDesc::screenProjection (this+0x394), view = identity
    float4 pretransform;    // pre-transformed pixels -> NDC: ndc.xy = (pixel.xy + pixelCenter.xy) * pretransform.xy +
                            // pretransform.zw
    float4 fogColor;        // rgb (D3DRS_FOGCOLOR), a unused
    float4 fogParams;       // x start, y end, z density, w 1 / (end - start) (0 if end == start)
    float4 targetSize;      // x width, y height, z 1/width, w 1/height of the current render target
    float4 pixelCenter;     // xy: the B19 shift in viewport pixels (D3D8 pixel centers at integer coordinates,
                            // D3D11 at +0.5): 0.5; 0 on upscaled or multisampled texture targets
                            // (gfx::TargetPixelCenter); zw unused
};

// b1: per draw.
struct FFDrawConstants {
    FF_ROW_MAJOR float4x4 world;                // D3DTS_WORLD (FUN_001489A0)
    FF_ROW_MAJOR float4x4 worldView;            // world * view (or world for the screen camera)
    FF_ROW_MAJOR float4x4 worldViewProjection;  // world * view * projection (or * screenProjection)
    FF_ROW_MAJOR float4x4 normalMatrix;         // inverse transpose of worldView (upper 3x3 used)
    float4 materialDiffuse;     // D3DMATERIAL8 (FUN_00161800)
    float4 materialAmbient;
    float4 materialSpecular;
    float4 materialEmissive;
    float4 materialPower;       // x = Power
    float4 globalAmbient;       // D3DRS_AMBIENT as float rgba (sum of ambient lights, FUN_0015BA40)
    float4 textureFactor;       // D3DRS_TEXTUREFACTOR rgba
    uint4 features;             // x feature key, y light count, z UV sets, w active stages
    uint4 colorSources;         // D3DMCS: x diffuse, y ambient, z specular, w emissive
    uint4 alphaTest;            // x enable, y FF_CMP_* func, z ref 0..255, w 1 = alpha-to-coverage (M3c: MSAA
                                // target; the shader outputs the sharpened coverage alpha instead of discarding)
    uint4 fog;                  // x table mode FF_FOG_*, y range fog, z/w unused
};

// One light (world-space D3DLIGHT8 transformed to view space by ff_pipeline).
struct FFLight {
    float4 diffuse;
    float4 specular;
    float4 ambient;
    float4 position;       // view space xyz, w = range
    float4 direction;      // view space xyz (D3D: direction the light points to), w = falloff
    float4 attenuation;    // x att0, y att1, z att2, w = FF_LIGHT_* type
    float4 spot;           // x cos(theta / 2), y cos(phi / 2), z 1 / (x - y) (0 if x == y), w unused
};
// b2
struct FFLightConstants {
    FFLight lights[FF_MAX_LIGHTS];
};

// One texture stage (D3D8 texture stage states of NiXBoxRenderer's pipeline, FUN_00162020/FUN_00161E40/
// FUN_00161D60/FUN_00161F70).
struct FFStage {
    uint4 color;           // x COLOROP, y COLORARG1, z COLORARG2, w COLORARG0
    uint4 alpha;           // x ALPHAOP, y ALPHAARG1, z ALPHAARG2, w ALPHAARG0
    uint4 coord;           // x TEXCOORDINDEX (UV set, low 16 bits), y TEXTURETRANSFORMFLAGS, z RESULTARG
                           // (FF_TA_CURRENT or FF_TA_TEMP), w flags: FF_STAGE_TEXTURE (a texture is bound),
                           // FF_STAGE_SHARP_NEAREST (M3c)
    FF_ROW_MAJOR float4x4 transform;  // D3DTS_TEXTUREn (used when coord.y != 0)
};
// b3
struct FFStageConstants {
    FFStage stages[FF_MAX_STAGES];
};

#ifdef __cplusplus
static_assert(sizeof(FFFrameConstants) == 4 * 64 + 5 * 16);
static_assert(sizeof(FFDrawConstants) == 4 * 64 + 11 * 16);
static_assert(sizeof(FFLight) == 7 * 16 && sizeof(FFLightConstants) == FF_MAX_LIGHTS * 7 * 16);
static_assert(sizeof(FFStage) == 3 * 16 + 64 && sizeof(FFStageConstants) == FF_MAX_STAGES * (3 * 16 + 64));
static_assert(sizeof(FFFrameConstants) % 16 == 0 && sizeof(FFDrawConstants) % 16 == 0);
}  // namespace gfx::ff
#endif

#endif  // FUT_GFX_FF_SHARED_H
