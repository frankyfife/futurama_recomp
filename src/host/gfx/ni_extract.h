// Guest NetImmerse objects -> host-side draw descriptions (GFX_CONTRACT.md 4). Owned by the extract agent.
//
// ni_extract reproduces what the original NiXBoxRenderer (REN) derived from the guest data and wrote into
// D3D8: transforms (FUN_001489A0, SetCameraData 0x1469E0), render states (FUN_00159CD0 and helpers),
// texture stage states (texture pipeline +0x5C8: FUN_0015AA60, default pipeline vtable 0x2AC104 - vf2
// 0x164360 lies in a Ghidra gap, read with capstone - pass apply 0x15FFE0, FUN_00162020, FUN_00161E40,
// FUN_00161D60, FUN_00161F70), material (FUN_00161800), lights (FUN_0015BF80 / FUN_0015BA40 /
// FUN_0015B6A0) and vertex/index data (FUN_00146F20, FUN_00157C00, FUN_0015CB30, FUN_001585A0,
// FUN_0014A050). The fixed-function emulation (gfx/ff_pipeline.h) consumes the result; it never reads guest
// memory itself.
//
// Rules:
//   - Pure readers of guest memory (guest::Ptr), no guest writes, no logging above GFX/debug per call
//     (762 000 RenderShape calls in 60 s). Exception: CPU skinning calls the original FUN_0014F710
//     (ni_guest_layouts.h) through core::CallGuestThiscall, like FUN_00157C00 does.
//   - Zero copy where possible: VertexStreamView/indices may point straight into guest arrays (valid
//     until the slot returns). Converted data (skinned vertices, strip/fan conversion, screen quads)
//     goes into the ExtractArena (valid until Reset, i.e. until DisplayFrame). Arena memory lies below
//     0x80000000 (gfx::AllocGuestPages) because FUN_0014F710 writes the skinned vertices into it.
//   - All functions are noexcept; ExtractStatus::Error means "guest data inconsistent" (logged once per
//     object at GFX/warn by the caller), Empty means "the original draws nothing here".
//   - Unit tests build guest objects in gfx::AllocGuestPages memory (below 0x80000000) and call these
//     functions directly (tests/extract_*.cpp).
//
// D3D8 state model (StateShadow): REN writes D3D state through a cache (render-state cache +0x5C0, light
// manager +0x5C4): a value is sent to D3D only if it differs from REN's cached copy. The cache starts with
// REN's table DAT_002D8338 (render states) / DAT_002D82B8 (texture stage states, a PC-ordered D3D8 table
// stored at Xbox indices) while the device starts with the D3D library's defaults (DAT_00242748,
// DAT_00242728). Where both disagree, the device keeps its default until REN writes a different value -
// e.g. D3DRS_DIFFUSEMATERIALSOURCE stays D3DMCS_COLOR1 (device) although REN believes MATERIAL, until the
// first E_A_D + AMB_DIFF draw. StateShadow models both copies and REN's other caches exactly, so the
// D3D state handed to ff_pipeline equals the original's for the same sequence of draws.
//
// Code conventions of the emitted state: TextureStageDesc ops use FF_TOP_* (the PC D3D8 numbering of
// gfx/shaders/ff_shared.h); REN's Xbox D3DTEXTUREOP values differ from 13 on (Xbox 13 BLENDCURRENTALPHA,
// 14 BLENDTEXTUREALPHA, 15 BLENDFACTORALPHA, 16 BLENDTEXTUREALPHAPM, 22 DOTPRODUCT3, 23 MULTIPLYADD,
// 24 LERP, 25 BUMPENVMAP, 26 BUMPENVMAPLUMINANCE) and are translated (TextureOpFromXbox). Arguments (D3DTA),
// material sources, fog modes, light types and TTFF use the same numbers on both platforms. As in D3D8,
// D3DTA_CURRENT on stage 0 means the diffuse color (REN leaves stage 0 COLORARG2 at CURRENT after init).
#pragma once

#include "gfx/gfx_math.h"
#include "gfx/ni_guest_layouts.h"
#include "gfx/rhi/rhi.h"
#include "gfx/shaders/ff_shared.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace gfx::ni {

inline constexpr std::uint32_t kMaxLights = FF_MAX_LIGHTS;
inline constexpr std::uint32_t kMaxTextureStages = FF_MAX_STAGES;
inline constexpr std::uint32_t kMaxUvSets = FF_MAX_UV_SETS;

// ---- camera
// ------------------------------------------------------------------------------------------------

// Inputs of SetCameraData (slot 47, thiscall, 6 stack args: loc, dir, up, right, NiFrustum*, NiRect<float>*
// port) as host pointers (guest::Ptr of the slot arguments), plus the current render target size and the
// left-right swap flag (slot 18/19; negates the projection x scale/offset like rs+0x1FC in 0x1469E0).
struct CameraInput {
    const float* location = nullptr;   // 3 floats (NiCamera +0xA0)
    const float* direction = nullptr;  // 3 floats (+0x110)
    const float* up = nullptr;         // 3 floats (+0x11C)
    const float* right = nullptr;      // 3 floats (+0x128)
    const NiFrustum* frustum = nullptr;
    const NiRectF* port = nullptr;
    std::uint32_t targetWidth = 640;
    std::uint32_t targetHeight = 480;
    bool leftRightSwap = false;
};

// Result of SetCameraData, laid out like the original's renderer fields.
struct CameraDesc {
    Matrix4 view;              // this+0x610: rows (right.x, up.x, dir.x, 0) (right.y, up.y, dir.y, 0)
                               // (right.z, up.z, dir.z, 0) (-loc.right, -loc.up, -loc.dir, 1)
    Matrix4 projection;        // this+0x650: [0][0] = 2/(r-l) (negated with swap), [2][0] = -(r+l)/(r-l)
                               // (sign flipped with swap), [1][1] = 2/(t-b), [2][1] = -(t+b)/(t-b),
                               // [2][2] = f/(f-n), [3][2] = -n*f/(f-n), [2][3] = 1, [3][3] = 0
    Matrix4 screenProjection;  // this+0x394 for screen polygons (slot 54): [0][0] = [1][1] = 2,
                               // [2][0] = [2][1] = -1, [2][2] = projection[2][2], [2][3] = 1,
                               // [3][2] = projection[3][2]
    rhi::Viewport viewport;    // D3DVIEWPORT8 of 0x146C81: X = trunc(W*l), Y = trunc((1-t)*H),
                               // Width = trunc((r-l)*W), Height = trunc((t-b)*H), MinZ 0, MaxZ 1
    float nearPlane = 0.1f;    // this+0x34C
    float depthRange = 0.9f;   // this+0x350 = far - near (screen polygon depth d = near + z * depthRange)
    NiFrustum frustum{};
    NiRectF port{};
    Float3 location, direction, up, right;
};

// SetCameraData. Viewport values are truncated like _ftol (FUN_00167080). The arithmetic follows the x87
// sequence of 0x1469E0 (double precision, float stores where the original stores).
// ASSUMPTION: the x87 precision control of the game is 53 bit (MSVC default); 24 bit would round the
// intermediate products differently (last-bit differences in the matrices only).
CameraDesc ExtractCamera(const CameraInput& input) noexcept;
// The camera NiXBoxRenderer::Create sets before any NiCamera::Click: FUN_0014C170 calls vf47 with
// location &DAT_002F90BC (BSS, NiPoint3 zero), direction 0x2D7740 (1,0,0), up 0x2D774C (0,1,0),
// right 0x2D7758 (0,0,1), frustum (-1, 1, 1, -1, 0.1, 1) and port (0, 1, 1, 0).
CameraDesc DefaultCamera(std::uint32_t targetWidth, std::uint32_t targetHeight) noexcept;

// ClearBuffer (slot 17 0x146490) rectangle: rect == nullptr -> whole target (returns false), else D3DRECT
// {trunc(W*l), trunc((1-t)*H), trunc(W*r), trunc((1-b)*H)} (0x1464C2..0x14652B) clamped to the target
// (returns true; an empty rectangle after clamping has right <= left or bottom <= top). Note: slot 17 sets
// the D3D viewport to the whole target (renderer +0x414) before clearing and does not restore it - the
// next draws use the full-target viewport until the next SetCameraData (integration).
bool ClearRect(const NiRectF* rect, std::uint32_t targetWidth, std::uint32_t targetHeight,
               rhi::Rect* out) noexcept;

// ---- render state (what REN wrote into D3D8)
// -----------------------------------------------------------------

enum class ShadeMode : std::uint8_t { Flat, Gouraud };

// D3DMATERIAL8 built by FUN_00161800 from NiMaterialProperty (alpha of all four colors = property alpha).
struct MaterialDesc {
    Float4 diffuse{1.0f, 1.0f, 1.0f, 1.0f};
    Float4 ambient{1.0f, 1.0f, 1.0f, 1.0f};
    Float4 specular{0.0f, 0.0f, 0.0f, 1.0f};
    Float4 emissive{0.0f, 0.0f, 0.0f, 1.0f};
    float power = 0.0f;
};

// One D3D8 texture stage as the original texture pipeline configured it. Op/arg codes = FF_TOP_* / FF_TA_*.
struct TextureStageDesc {
    GuestPtr niTexture = 0;      // NiTexture* bound to the stage (0 = none); for diagnostics also set
                                 // when the resolver found no host texture (then `texture` is empty)
    rhi::TextureHandle texture;  // filled through ExtractContext::resolveTexture (integration)
    std::uint8_t colorOp = FF_TOP_DISABLE;
    std::uint8_t colorArg1 = FF_TA_TEXTURE;
    std::uint8_t colorArg2 = FF_TA_CURRENT;
    std::uint8_t colorArg0 = FF_TA_CURRENT;
    std::uint8_t alphaOp = FF_TOP_DISABLE;
    std::uint8_t alphaArg1 = FF_TA_TEXTURE;
    std::uint8_t alphaArg2 = FF_TA_CURRENT;
    std::uint8_t alphaArg0 = FF_TA_CURRENT;
    std::uint8_t resultArg = FF_TA_CURRENT;
    std::uint32_t texCoordIndex = 0;                     // D3DTSS_TEXCOORDINDEX (UV set in the low 16 bits)
    rhi::AddressMode addressU = rhi::AddressMode::Wrap;  // NiTexturingMap clamp -> FUN_00161D60
    rhi::AddressMode addressV = rhi::AddressMode::Wrap;
    rhi::Filter minFilter = rhi::Filter::Linear;  // NiTexturingMap filter -> table DAT_0030FEF8
    rhi::Filter magFilter = rhi::Filter::Linear;
    rhi::MipFilter mipFilter = rhi::MipFilter::None;
    std::uint32_t transformFlags = FF_TTFF_DISABLE;  // D3DTSS_TEXTURETRANSFORMFLAGS (FUN_00161F70)
    Matrix4 transform = IdentityMatrix();            // D3DTS_TEXTUREn
};

// Which branch of the texture pipeline produced the stages (diagnostics, statistics).
enum class TexturePath : std::uint8_t {
    SingleStage,  // default pipeline vf2, base map only (0x16440C..0x16444E): exact
    MultiMap,     // other maps or clipped texture effects (vf2 0x164453 ff.): only the base map stage is
                  // reproduced in M3 (P2), the other maps/effects are ignored
    Screen,       // slot 55 pipeline (vtable 0x2ABDB0, vf2 FUN_00160270): exact
};

// The D3D8 state of one draw. Defaults = the state of the default NiPropertyState properties.
struct RenderStateDesc {
    // NiAlphaProperty (FUN_00159270)
    bool alphaBlendEnable = false;
    rhi::BlendFactor srcBlend = rhi::BlendFactor::SrcAlpha;
    rhi::BlendFactor dstBlend = rhi::BlendFactor::InvSrcAlpha;
    bool alphaTestEnable = false;
    rhi::CompareFunc alphaFunc = rhi::CompareFunc::Always;
    std::uint8_t alphaRef = 0;
    // NiZBufferProperty (FUN_00159B60): z stays enabled; test off = ZFUNC ALWAYS
    rhi::CompareFunc zFunc = rhi::CompareFunc::LessEqual;
    bool zWriteEnable = true;
    // NiStencilProperty (FUN_001599E0)
    bool stencilEnable = false;
    rhi::CompareFunc stencilFunc = rhi::CompareFunc::Always;
    std::uint8_t stencilRef = 0;
    std::uint8_t stencilMask = 0xFF;
    rhi::StencilOp stencilFail = rhi::StencilOp::Keep;
    rhi::StencilOp stencilZFail = rhi::StencilOp::Keep;
    rhi::StencilOp stencilPass = rhi::StencilOp::Keep;
    rhi::CullMode cullMode = rhi::CullMode::CW;  // draw mode 0 (CCW_OR_BOTH)
    // Wireframe / shade / dither / specular (FUN_00159CD0, FUN_00159980)
    rhi::FillMode fillMode = rhi::FillMode::Solid;
    ShadeMode shadeMode = ShadeMode::Gouraud;
    bool ditherEnable = false;
    bool specularEnable = false;
    bool normalizeNormals = false;  // FUN_00159200 (world scale outside 0.99..1.01)
    // NiFogProperty (FUN_00159700 / FUN_00159900)
    bool fogEnable = false;
    std::uint8_t fogTableMode = FF_FOG_NONE;  // FF_FOG_*
    bool rangeFogEnable = false;
    float fogStart = 0.0f;
    float fogEnd = 1.0f;
    float fogDensity = 1.0f;
    Float3 fogColor;  // D3DRS_FOGCOLOR bytes / 255 (trunc(c*255) & 0xFF)
    // Lighting (FUN_0015BF80 with NiVertexColorProperty / NiTexturingProperty)
    bool lightingEnable = true;
    std::uint8_t diffuseSource = FF_MCS_MATERIAL;   // D3DRS_DIFFUSEMATERIALSOURCE (0x24488C)
    std::uint8_t ambientSource = FF_MCS_MATERIAL;   // D3DRS_AMBIENTMATERIALSOURCE (0x244890)
    std::uint8_t specularSource = FF_MCS_MATERIAL;  // D3DRS_SPECULARMATERIALSOURCE (0x244888, REN never
                                                    // writes it: device default COLOR2)
    std::uint8_t emissiveSource = FF_MCS_MATERIAL;  // D3DRS_EMISSIVEMATERIALSOURCE (0x244894)
    MaterialDesc material;
    // Texturing (texture pipeline +0x5C8)
    std::uint32_t stageCount = 0;  // stages with colorOp != DISABLE (0..4)
    TextureStageDesc stages[kMaxTextureStages];
    Float4 textureFactor{1.0f, 1.0f, 1.0f, 1.0f};  // D3DRS_TEXTUREFACTOR
    std::uint32_t passCount = 1;                   // multi-pass pipelines (P2): only pass 1 in M3
    // Added in phase 2 (extract agent):
    bool localViewer = true;  // D3DRS_LOCALVIEWER: REN never writes it, device
                              // default TRUE (DAT_00242748[104])
    TexturePath texturePath = TexturePath::SingleStage;
};

// D3DLIGHT8 as FUN_0015B6A0 fills it (world space).
struct LightDesc {
    std::uint32_t type = FF_LIGHT_DIRECTIONAL;  // FF_LIGHT_* (D3DLIGHTTYPE)
    Float4 diffuse;                             // dimmer * NiLight color, alpha 1
    Float4 specular;
    Float4 ambient;
    Float3 position;   // world translate +0xA0 (point/spot)
    Float3 direction;  // world direction (directional +0x110, spot +0x120)
    float range = 0.0f;
    float falloff = 0.0f;
    float attenuation0 = 0.0f, attenuation1 = 0.0f, attenuation2 = 0.0f;
    float theta = 0.0f, phi = 0.0f;
    GuestPtr niLight = 0;
    std::uint32_t index = 0;  // NiLight +0xC4 (D3D light index)
};

struct LightSetDesc {
    Float4 ambient{0.0f, 0.0f, 0.0f, 1.0f};  // D3DRS_AMBIENT: sum of dimmer * ambient of ambient lights
                                             //  (FUN_0015BA40), packed like the original: each channel
                                             //  trunc(c * 255) & 0xFF (wraps above 1.0), then / 255
    std::uint32_t count = 0;
    LightDesc lights[kMaxLights];
    std::uint32_t dropped = 0;  // enabled lights beyond kMaxLights (logged by the caller)
};

// NiTextureEffect slots of the NiDynamicEffectState (P2: projected lights/shadows, environment and fog
// maps). Captured for diagnostics; M3 does not render them.
struct TextureEffectSlots {
    GuestPtr projectedLights[2] = {};
    GuestPtr projectedShadows[2] = {};
    std::uint32_t projectedLightCount = 0;
    std::uint32_t projectedShadowCount = 0;
    GuestPtr environmentMap = 0;
    GuestPtr fogMap = 0;
};

// ---- geometry
// -------------------------------------------------------------------------------------------------

enum class DrawKind : std::uint8_t {
    Shape,          // slot 48 RenderShape(NiTriShapeData*, NiSkinInstance*, NiTransform*, NiBound*)
    Triangles,      // slot 49 RenderTriangles(NiTrianglesData*, NiTransform*, NiBound*)
    TriStrip,       // slot 50 RenderTriStrip(NiTriStripData*, NiTransform*, NiBound*)
    TriStrips,      // slot 51 RenderTristrips(NiTriStripsData*, NiSkinInstance*, NiTransform*, NiBound*)
    ScreenPolygon,  // slot 54 RenderScreenPolygon(NiScreenPolygon*)
    ScreenTexture,  // slot 55 RenderScreenTexture(NiScreenTexture*)
};

struct VertexStreamView {
    const void* data = nullptr;  // host-readable (guest array or arena); nullptr = attribute absent
    std::uint32_t stride = 0;    // bytes between vertices
};

enum class Topology : std::uint8_t {
    TriangleList,
    TriangleStrip,  // several strips are joined with rhi::kStripCut (0xFFFF) indices
};

struct GeometryDesc {
    std::uint32_t vertexCount = 0;
    VertexStreamView positions;        // float3 (pre-transformed: x, y in D3D8 pixels, z)
    VertexStreamView normals;          // float3
    VertexStreamView colors;           // float4 NiColorA
    VertexStreamView uvs[kMaxUvSets];  // float2
    std::uint32_t uvSetCount = 0;      // min(NiGeometryData +0x0A, 4)
    Topology topology = Topology::TriangleList;
    const std::uint16_t* indices = nullptr;  // nullptr = non-indexed (vertexCount vertices in order)
    std::uint32_t indexCount = 0;
    std::uint32_t primitiveCount = 0;  // triangles the original would draw (statistics)
    bool pretransformed = false;       // D3DFVF_XYZRHW (screen textures)
    bool skinned = false;              // positions/normals were deformed by FUN_0014F710
    // Cache keys for a later static-geometry cache (NiGeometryData* + id +0x2C + revision +0x2E).
    GuestPtr guestData = 0;
    std::uint16_t dataId = 0;
    std::uint16_t dataRevision = 0;
};

struct DrawItem {
    DrawKind kind = DrawKind::Shape;
    GuestPtr guestObject = 0;          // the slot's first argument (diagnostics)
    Matrix4 world = IdentityMatrix();  // D3DTS_WORLD (NiTransformToWorld; identity for 2D)
    float worldScale = 1.0f;
    bool useScreenCamera = false;  // slot 54: view = identity, projection = screenProjection
    GeometryDesc geometry;
    RenderStateDesc state;
    LightSetDesc lights;
    TextureEffectSlots effects;
};

// ---- extraction
// ------------------------------------------------------------------------------------------------

// Bump allocator for per-frame converted data (16-byte aligned blocks). Reset() at DisplayFrame.
// Blocks come from gfx::AllocGuestPages (below 0x80000000; FUN_0014F710 writes skinned vertices into them).
class ExtractArena {
public:
    ExtractArena() noexcept;
    ~ExtractArena();
    ExtractArena(const ExtractArena&) = delete;
    ExtractArena& operator=(const ExtractArena&) = delete;

    // nullptr if out of memory. Memory stays valid until Reset().
    void* Allocate(std::size_t bytes, std::size_t alignment = 16) noexcept;
    template <class T>
    T* AllocateArray(std::size_t count) noexcept {
        return static_cast<T*>(Allocate(count * sizeof(T), alignof(T) < 16 ? 16 : alignof(T)));
    }
    void Reset() noexcept;
    std::size_t BytesUsed() const noexcept;      // since the last Reset
    std::size_t BytesReserved() const noexcept;  // all blocks
    // Incremented by every Reset (phase 2): caches of arena data (skinning) compare it.
    std::uint32_t Generation() const noexcept { return generation_; }

private:
    struct Block;
    Block* head_ = nullptr;
    Block* current_ = nullptr;
    std::size_t used_ = 0;
    std::uint32_t generation_ = 0;
};

enum class ExtractStatus : std::uint8_t {
    Ok,           // *out is complete
    Empty,        // nothing to draw (the original returns early: no vertices/triangles, empty rect list)
    Unsupported,  // valid data the M3 renderer cannot draw yet (logged once per kind by the caller)
    Error,        // inconsistent guest data (bad pointer, count mismatch)
};
const char* ExtractStatusName(ExtractStatus status) noexcept;

// Xbox D3DTEXTUREOP (REN's values) -> FF_TOP_* (PC D3D8 numbering, ff_shared.h); unknown -> FF_TOP_DISABLE.
std::uint8_t TextureOpFromXbox(std::uint32_t xboxOp) noexcept;

// Counters of things the caller may want to log (statistics of the M3 run; reset with the shadow).
struct ExtractCounters {
    std::uint64_t states = 0;                // ExtractState calls that produced a state
    std::uint64_t multiMapApproximated = 0;  // TexturePath::MultiMap draws (other maps/effects ignored)
    std::uint64_t textureEffectDraws = 0;    // draws whose effect state carries NiTextureEffects (P2)
    std::uint64_t lightEvaluations = 0;      // FUN_0015BA40 runs
    std::uint64_t lightsDropped = 0;         // enabled D3D lights beyond kMaxLights
    std::uint64_t unknownLightTypes = 0;     // GetEffectType not one of the known implementations
    std::uint64_t skinnedDraws = 0;          // draws with a NiSkinInstance
    std::uint64_t skinDeforms = 0;           // FUN_0014F710 calls
    std::uint64_t skinCacheHits = 0;         // second draw of the same skin instance in a frame
    std::uint64_t invalidMapModes = 0;       // clamp > 3 or filter > 5 (out of REN's tables)
    std::uint64_t unresolvedTextures = 0;    // stage textures the resolver had no host texture for
    std::uint64_t lightsPurged = 0;          // PurgeLight removed an enabled light (slot 60, review M3 #4)
};

// D3D8 state persistence between draws (GFX_CONTRACT.md 4.2, header comment above): REN's caches and the
// device state, as left by the previous draw. One shadow per renderer, owned by the caller; the default
// constructor and Reset() give the state after NiXBoxRenderer::Create (D3D library defaults FUN_002395C0
// + REN's init FUN_00159410, light manager ctor FUN_0015BCB0, pipeline manager init FUN_0015AB40).
struct StateShadow {
    static constexpr std::uint32_t kRsFirst = 57;   // D3DRS_ZFUNC, first state of REN's cache range
    static constexpr std::uint32_t kRsCount = 109;  // states 57..165 (FUN_00159410 loop 0xE4..0x298)
    static constexpr std::uint32_t kTssCount = 32;  // Xbox D3DTEXTURESTAGESTATETYPE 0..31

    // Render-state cache rs+0x22C + S*8 (REN's belief) and the device value (D3D_g_RenderState 0x2446D0).
    std::uint32_t renRs[kRsCount];
    std::uint32_t devRs[kRsCount];
    // Texture stage state cache rs+0x76C + (stage * 32 + T) * 8 and the device value
    // (D3D_g_DeferredTextureState 0x2444D0 + (stage * 32 + T) * 4).
    std::uint32_t renTss[kMaxTextureStages][kTssCount];
    std::uint32_t devTss[kMaxTextureStages][kTssCount];
    // Bound textures (D3DDevice_SetTexture; REN cache rs+0xB6C + stage * 4 always equals the device).
    GuestPtr boundNiTexture[kMaxTextureStages];
    rhi::TextureHandle boundTexture[kMaxTextureStages];
    Matrix4 textureTransform[kMaxTextureStages];  // D3DTS_TEXTUREn (FUN_00161F70)
    MaterialDesc material;                        // D3DDevice_SetMaterial (FUN_00161880)
    // FUN_00159CD0 property caches (rs+0x195..+0x19C bytes, +0x164 alpha, +0x190 fog, +0x16C fog color,
    // +0x1F8 cull, +0x220 normalize).
    // ASSUMPTION: the byte caches start at 0 (fresh heap memory of the render-state cache, FUN_00159C20 does
    // not initialize them; Xbox heap pages are zero-filled).
    std::uint8_t ditherBit = 0, shadeBit = 0, specularBit = 0, stencilOn = 0, wireframeBit = 0, zWriteBit = 0,
                 zTestBit = 0, normalizeOn = 0;
    GuestPtr lastAlphaProperty = 0;
    GuestPtr lastFogProperty = 0;
    std::uint32_t lastFogColor = 0;
    std::uint32_t lastCull = 0x900;
    // Light manager (renderer +0x5C4, ctor FUN_0015BCB0): +0x48 LIGHTING on, +0x49 lights enabled,
    // +0x4C vertex-color source mode, +0x3C/+0x40 effect state / lights flag of the last light evaluation,
    // +0x50 packed ambient of the last evaluation.
    bool lightingOn = false;
    bool lightsEnabled = false;
    std::uint32_t vertexColorSourceMode = 0;
    GuestPtr lastEffectState = 0;
    bool lastLightsEnabled = false;
    std::uint32_t lastAmbient = 0;
    LightSetDesc lights;  // enabled D3D lights (SetLight/LightEnable) + ambient
    // CPU skinning cache (FUN_00157C00 deforms once per skin instance and frame).
    struct SkinEntry {
        GuestPtr skin = 0;
        GuestPtr data = 0;
        std::uint32_t frame = 0;
        std::uint32_t arenaGeneration = 0;
        std::uint32_t vertexCount = 0;
        const float* vertices = nullptr;  // interleaved position (+ normal) in the arena
        std::uint32_t stride = 0;
        bool hasNormals = false;
    };
    static constexpr std::uint32_t kSkinCacheSize = 32;
    SkinEntry skinCache[kSkinCacheSize];
    std::uint32_t skinCacheNext = 0;
    ExtractCounters counters;

    StateShadow() noexcept { Reset(); }
    void Reset() noexcept;  // implemented in ni_extract.cpp
};

// Slot 45 BeginScene (0x148950): FUN_00158B90 forgets the cached alpha/fog property (rs+0x164/+0x190) and
// FUN_0015B990 disables every light (light manager +0x3C = 0), so the first draw of a scene re-evaluates
// them. The integration calls it from slot 45 (the validation harness does not).
void BeginScene(StateShadow* shadow) noexcept;

// Slot 60 PurgeEffect (0x146E40) for an NiLight: the light manager FUN_0015B9E0 removes the light from the
// list of active lights and disables its D3D light index at once (D3DDevice_LightEnable(+0xC4, FALSE),
// 0x15BA0D); its "last effect state" (+0x3C) stays, so a later draw with the same effect state does not
// re-evaluate the lights. Here: the light's entries leave StateShadow::lights (the enabled D3D lights),
// lastEffectState stays. Returns the number of entries removed (0: the light was not enabled).
std::uint32_t PurgeLight(StateShadow* shadow, GuestPtr niLight) noexcept;

// Resolves the host texture of an NiTexture* (renderer data at +0x20, created on demand through texconv).
// Supplied by the integration (gfx/nipc_renderer.cpp); nullptr -> TextureStageDesc::texture stays empty and
// the texture counts as resolved. An empty handle from the resolver = the original's texture manager
// failing (texture manager vf2 0x15F930 returns false): the stage gets no texture and its filter, address,
// texcoord and transform states keep their previous values.
using TextureResolver = rhi::TextureHandle (*)(void* user, GuestPtr niTexture) noexcept;

struct ExtractContext {
    ExtractArena* arena = nullptr;       // required
    const CameraDesc* camera = nullptr;  // current SetCameraData result (screen polygons need near/range)
    std::uint32_t targetWidth = 640;     // current render target (screen textures wrap/position in it)
    std::uint32_t targetHeight = 480;
    std::uint32_t frame = 0;     // displayed-frame counter (skinning: once per instance and frame)
    bool leftRightSwap = false;  // cull table index (FUN_001599E0)
    TextureResolver resolveTexture = nullptr;
    void* resolveUser = nullptr;
    StateShadow* shadow = nullptr;  // persistent D3D state (nullptr: every draw starts from defaults)
    // Added in phase 2 (extract agent):
    // The 12 default property pointers slot 55 starts from (kDefaultPropertyGlobals); nullptr -> read the
    // guest globals. Unit tests pass their own array.
    const GuestPtr* defaultProperties = nullptr;
    // false: never call guest code (unit tests): skinned draws return Unsupported, lights whose
    // GetEffectType is not a known implementation are skipped.
    bool allowGuestCalls = true;
};

// Property/effect state -> RenderStateDesc + LightSetDesc + effect slots (FUN_00159CD0, FUN_0015BF80,
// texture pipeline). `hasNormals` / `worldScale` as FUN_00149AE0 passes them on (FUN_00159200 normalize).
struct StateInput {
    GuestPtr propertyState = 0;  // renderer +0x0C (required)
    GuestPtr effectState = 0;    // renderer +0x10 (0 = no lights / effects)
    float worldScale = 1.0f;
    bool hasNormals = false;
    std::uint32_t uvSetCount = 0;
    // Added in phase 2: slot 55 (screen texture pipeline FUN_00160270, which also returns 0x800000 = no
    // world transform, so FUN_00159200 is not reached).
    bool screenTexture = false;
};
ExtractStatus ExtractState(const ExtractContext& context, const StateInput& input, RenderStateDesc* state,
                           LightSetDesc* lights, TextureEffectSlots* effects) noexcept;

// Slots 48-51: data/skin/transform/bound are the slot's guest arguments; propertyState/effectState are
// renderer +0x0C/+0x10 at the time of the call. The caller checks the in-scene flag (renderer +0x500).
struct GeometryInput {
    GuestPtr data = 0;          // NiTriShapeData / NiTrianglesData / NiTriStripData / NiTriStripsData
    GuestPtr skinInstance = 0;  // slots 48/51 only
    GuestPtr transform = 0;     // NiTransform*
    GuestPtr bound = 0;         // NiBound* (unused for drawing)
    GuestPtr propertyState = 0;
    GuestPtr effectState = 0;
    // G1 (gfx/outline.h, [render] outline_thickness != 1): guest address of replacement bind-pose positions
    // (float3 x data vertex count) used instead of NiGeometryData +0x1C - as the source of FUN_0014F710 for
    // skinned draws, as the position stream otherwise. 0 = the data's own positions (always at the defaults).
    GuestPtr positionsOverride = 0;
};
ExtractStatus ExtractShape(const ExtractContext& context, const GeometryInput& input, DrawItem* out) noexcept;
ExtractStatus ExtractStrips(const ExtractContext& context, const GeometryInput& input,
                            DrawItem* out) noexcept;
ExtractStatus ExtractTriangles(const ExtractContext& context, const GeometryInput& input,
                               DrawItem* out) noexcept;
ExtractStatus ExtractTriStrip(const ExtractContext& context, const GeometryInput& input,
                              DrawItem* out) noexcept;

// Slot 54: property state = polygon +0x08, no effect state; positions -> (d*x, d*y, d) in the arena,
// fan -> triangle list (strip if +0x1E), useScreenCamera = true, world = identity.
ExtractStatus ExtractScreenPolygon(const ExtractContext& context, GuestPtr screenPolygon,
                                   DrawItem* out) noexcept;
// Slot 55: quads of pre-transformed vertices in the arena (FUN_0014A050 rules, ni_guest_layouts.h) and
// the state of slot 55's temporary property state (defaults + alpha 0xED, z off, vertex color EMISSIVE/E,
// the NiScreenTexture's NiTexturingProperty) through the screen-texture pipeline.
ExtractStatus ExtractScreenTexture(const ExtractContext& context, GuestPtr screenTexture,
                                   DrawItem* out) noexcept;

}  // namespace gfx::ni
