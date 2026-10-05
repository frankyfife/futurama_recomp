// Guest NetImmerse 4.2.1 (Xbox build of Futurama, retail EU) structure layouts read by the M3 renderer
// (GFX_CONTRACT.md 4). Owned by the extract agent; texconv reads NiPixelFormat/NiPixelData/NiPalette.
//
// Every struct is a POD view of guest memory (all pointers are 32-bit guest addresses, `GuestPtr`),
// verified by static_assert against the offsets the original code uses. Evidence per field: the function
// that reads/writes it (decompiled export analysis/retail_eu_export/decomp, capstone on the XBE).
// Sizes from analysis/retail_eu_ni_classes.json. Fields marked "ASSUMPTION" are not proven by the code
// the M3 renderer replaces. Phase 1 status: offsets verified by the skeleton author (M3 phase 1) from
// the functions named in the comments; the extract agent may extend (add-only for the texconv part).
// Phase 2 (extract agent): every offset ni_extract reads was re-checked against the decompiled/capstone
// code of the original renderer; additions are marked "phase 2" with their evidence.
//
// Never write these structs (except where the original renderer writes the same field, documented
// at the user). Read them through guest::Ptr<T>(address).
#pragma once

#include <cstddef>
#include <cstdint>

namespace gfx::ni {

using GuestPtr = std::uint32_t;  // guest address (0 = NULL)

// ---- basic value types -------------------------------------------------------------------------------

// NiTransform (0x40): NiMatrix3 stored as 3 rows of 4 floats (16-byte stride, 4th float padding - the
// NiAVObject ctor 0x102860 zeroes +0x3C/+0x4C/+0x5C of the local transform), translation, uniform scale.
// Rows are the images of the x/y/z axes (row-vector order): FUN_001489A0 copies them unchanged into the
// D3D world matrix, NiTriShape::vf33 0x1166D0 transforms out.x = t.x + s*(m[0]*x + m[4]*y + m[8]*z).
struct NiTransform {
    float rotate[3][4];   // +0x00 [row][col], col 3 unused
    float translate[3];   // +0x30
    float scale;          // +0x3C
};
static_assert(sizeof(NiTransform) == 0x40 && offsetof(NiTransform, translate) == 0x30 &&
              offsetof(NiTransform, scale) == 0x3C);

// NiBound (0x10): center + radius (NiGeometryData ctor 0x143970 +0x0C..+0x18, NiAVObject ctor +0x20..+0x2C).
struct NiBound {
    float center[3];
    float radius;
};
static_assert(sizeof(NiBound) == 0x10);

// NiFrustum (0x18) as passed to SetCameraData (slot 47 0x1469E0: param_6[0..5]): tangent-space planes at
// distance 1 + near/far. Create's default: (-1, 1, 1, -1, 0.1, 1) (renderer.md 1.4).
struct NiFrustum {
    float left, right, top, bottom, nearPlane, farPlane;
};
static_assert(sizeof(NiFrustum) == 0x18);

// NiRect<float> (0x10): camera port / ClearBuffer rect, normalized with y up (top = 1 is the top of the
// target): viewport y = (1 - top) * height (slot 47 0x146C9C, slot 17 0x1464F3).
struct NiRectF {
    float left, right, top, bottom;
};
static_assert(sizeof(NiRectF) == 0x10);

struct NiColor {
    float r, g, b;
};
struct NiColorA {
    float r, g, b, a;
};

// ---- objects -----------------------------------------------------------------------------------------

// NiObject: +0 vtable, +4 refcount (NiRefObject).
struct NiObjectHeader {
    GuestPtr vtable;
    std::uint32_t refCount;
};

// NiAVObject (0xC0) - only the fields the renderer path uses.
struct NiAVObject {
    NiObjectHeader obj;           // +0x00
    std::uint8_t net[0x20 - 8];   // +0x08 NiObjectNET name/extra data/controllers, +0x14 ...
    NiBound worldBound;           // +0x20 (passed as the NiBound* of slots 48-51 by NiTriShape::vf33)
    NiTransform local;            // +0x30
    NiTransform world;            // +0x70 (passed as the NiTransform* of slots 48-51)
    std::uint16_t flags;          // +0xB0
    std::uint8_t restB2[0xC0 - 0xB2];
};
static_assert(sizeof(NiAVObject) == 0xC0 && offsetof(NiAVObject, worldBound) == 0x20 &&
              offsetof(NiAVObject, local) == 0x30 && offsetof(NiAVObject, world) == 0x70);

// NiGeometryData (0x34) - ctor 0x143970, vertex fill FUN_00157C00/FUN_00157830.
struct NiGeometryData {
    NiObjectHeader obj;           // +0x00
    std::uint16_t vertexCount;    // +0x08 (vf12 0x10E540 of NiGeometryData/NiTriShapeData/NiTriStripsData)
    std::uint16_t uvSetCount;     // +0x0A texture coordinate sets (FUN_001437B0, FUN_00157830; the original
                                  //       caps it at DAT_002D85D0 = 4)
    NiBound bound;                // +0x0C model bound
    GuestPtr vertices;            // +0x1C NiPoint3[vertexCount]
    GuestPtr normals;             // +0x20 NiPoint3[vertexCount] or 0 (FUN_00149AE0: != 0 -> has normals)
    GuestPtr colors;              // +0x24 NiColorA[vertexCount] or 0 (packed to D3DCOLOR with ROUND(x*255))
    GuestPtr uvs;                 // +0x28 NiPoint2[uvSetCount][vertexCount] (set i at +i*vertexCount*8)
    std::uint16_t id;             // +0x2C unique id (global counter DAT_002D7C80)
    std::uint16_t revision;       // +0x2E dirty/revision (FUN_00146F20 rebuilds the VB when != 0 and resets it)
    GuestPtr rendererData;        // +0x30 NiXBoxRenderer geometry data (stays 0 with NiPCRenderer; slot 58)
};
static_assert(sizeof(NiGeometryData) == 0x34 && offsetof(NiGeometryData, vertices) == 0x1C &&
              offsetof(NiGeometryData, uvs) == 0x28 && offsetof(NiGeometryData, revision) == 0x2E &&
              offsetof(NiGeometryData, rendererData) == 0x30);

// NiTriBasedGeomData (0x38) - ctor 0x151D90; vf15 0x115F60 returns the triangle count.
struct NiTriBasedGeomData {
    NiGeometryData base;          // +0x00
    std::uint16_t triangleCount;  // +0x34
    std::uint16_t pad36;
};
static_assert(sizeof(NiTriBasedGeomData) == 0x38);

// NiTriShapeData (0x4C) - ctor 0x115D80; CalculateNormals vf13 0x151AD0 walks triList 3 indices per
// triangle; FUN_00146F20 uses +0x3C as the index list (count = triangleCount * 3).
struct NiTriShapeData {
    NiTriBasedGeomData base;      // +0x00
    std::uint32_t triListLength;  // +0x38 number of indices (3 * triangles)
    GuestPtr triList;             // +0x3C unsigned short[triListLength]
    GuestPtr sharedNormals;       // +0x40 per-vertex {u16 count, u16* list} (8 bytes each, vf13)
    std::uint16_t sharedNormalsCount;  // +0x44 (== vertexCount when used)
    std::uint16_t pad46;
    std::uint32_t field48;        // +0x48
};
static_assert(sizeof(NiTriShapeData) == 0x4C && offsetof(NiTriShapeData, triList) == 0x3C);

// NiTriShapeDynamicData (0x50, vtable 0x2A86E0) - phase 2: overrides vf12 0x11B0F0 = active vertex count
// (u16 +0x4C) and vf15 0x11B100 = active triangle count (u16 +0x4E). REN takes both counts from these
// virtuals (FUN_00146F20 / FUN_00157C00: VB vertex count = vf12, index count = 3 * vf15).
struct NiTriShapeDynamicData {
    NiTriShapeData base;          // +0x00
    std::uint16_t activeVertices; // +0x4C
    std::uint16_t activeTriangles;// +0x4E
};
static_assert(sizeof(NiTriShapeDynamicData) == 0x50 &&
              offsetof(NiTriShapeDynamicData, activeTriangles) == 0x4E);

// Phase 2: vtables of the geometry data classes (retail_eu_ni_classes.json) and the implementations of
// GetVertexCount vf12 (+0x30) / GetTriangleCount vf15 (+0x3C) REN calls (slot 48 0x14C513, FUN_00146F20,
// FUN_00157C00). ni_extract dispatches on the implementation address instead of calling guest code.
inline constexpr GuestPtr kNiTriShapeDataVtable = 0x002A7C68;
inline constexpr GuestPtr kNiTriShapeDynamicDataVtable = 0x002A86E0;
inline constexpr GuestPtr kNiTriStripsDataVtable = 0x002A8890;
inline constexpr GuestPtr kNiTrianglesDataVtable = 0x002A85A8;
inline constexpr GuestPtr kNiTriStripDataVtable = 0x002A8760;
inline constexpr std::uint32_t kNiGeometryDataVfVertexCount = 0x30;    // vtable byte offset (slot 12)
inline constexpr std::uint32_t kNiGeometryDataVfTriangleCount = 0x3C;  // slot 15
inline constexpr GuestPtr kVertexCountGeometryData = 0x0010E540;  // u16 +0x08
inline constexpr GuestPtr kVertexCountDynamicData = 0x0011B0F0;   // u16 +0x4C
inline constexpr GuestPtr kVertexCountTrianglesData = 0x0011AC20; // u16 +0x38 * 3
inline constexpr GuestPtr kVertexCountTriStripData = 0x0011B4B0;  // u16 +0x38 + 2 (0 if +0x38 == 0)
inline constexpr GuestPtr kTriangleCountTriBased = 0x00115F60;    // u16 +0x34
inline constexpr GuestPtr kTriangleCountDynamicData = 0x0011B100; // u16 +0x4E
inline constexpr GuestPtr kTriangleCountActive = 0x0011B4F0;      // u16 +0x38 (NiTriangles/NiTriStripData)

// NiTriStripsData (0x44) - CalculateNormals vf13 0x152460: strips are concatenated in +0x40, strip s has
// stripLengths[s] indices; odd triangles of a strip swap their first two vertices (standard strip rule).
// The original draws each strip with DrawIndexedVertices(TRIANGLESTRIP, length - 2 primitives).
struct NiTriStripsData {
    NiTriBasedGeomData base;      // +0x00 (triangleCount = sum(length - 2))
    std::uint16_t stripCount;     // +0x38
    std::uint16_t pad3A;
    GuestPtr stripLengths;        // +0x3C unsigned short[stripCount]
    GuestPtr strips;              // +0x40 unsigned short[sum(stripLengths)]
};
static_assert(sizeof(NiTriStripsData) == 0x44 && offsetof(NiTriStripsData, strips) == 0x40);

// NiTrianglesData (0x3C, slot 49) - non-indexed triangle list: vf12 0x11AC20 returns +0x38 * 3 vertices,
// vf15 0x11B4F0 returns +0x38 triangles.
struct NiTrianglesData {
    NiTriBasedGeomData base;
    std::uint16_t activeTriangles;  // +0x38
    std::uint16_t pad3A;
};
static_assert(sizeof(NiTrianglesData) == 0x3C);

// NiTriStripData (0x3C, slot 50) - one non-indexed strip: vf12 0x11B4B0 returns +0x38 + 2 vertices (0 if
// +0x38 == 0), vf15 returns +0x38 triangles.
struct NiTriStripData {
    NiTriBasedGeomData base;
    std::uint16_t activeTriangles;  // +0x38
    std::uint16_t pad3A;
};
static_assert(sizeof(NiTriStripData) == 0x3C);

// NiSkinInstance (0x18). CPU skinning is the original FUN_0014F710 (thiscall, ecx = NiSkinInstance*,
// 6 stack args, ret 0x18 at 0x150AB6): (srcPositions, srcNormals or 0, u16 vertexCount, dstPositions,
// dstNormals or 0, dstStride) - writes deformed positions/normals with the given stride; called from
// FUN_00157C00 at most once per frame (frame stamp +0x14 vs the VB manager frame counter) and clears
// DAT_0030EB40 at its end like NiTriShape::vf33 does.
struct NiSkinInstance {
    NiObjectHeader obj;           // +0x00
    GuestPtr skinData;            // +0x08 NiSkinData*
    GuestPtr rootParent;          // +0x0C NiAVObject* (FUN_0014F710 reads its world transform +0x70)
    GuestPtr bones;               // +0x10 NiAVObject*[bone count]
    std::uint32_t frameStamp;     // +0x14 written by the original renderer only (not by NiPCRenderer)
};
static_assert(sizeof(NiSkinInstance) == 0x18);
inline constexpr GuestPtr kNiSkinInstanceDeform = 0x0014F710;  // FUN_0014F710 (see above)
inline constexpr std::uint32_t kNiSkinInstanceDeformStackArgs = 6;

// ---- properties (FUN_00159CD0, renderer.md 5.4) ------------------------------------------------------

// NiProperty base: NiObjectNET (0x14) + u16 flags at +0x14.
struct NiProperty {
    NiObjectHeader obj;
    std::uint8_t net[0x14 - 8];
    std::uint16_t flags;          // +0x14
    std::uint16_t pad16;
};
static_assert(sizeof(NiProperty) == 0x18 && offsetof(NiProperty, flags) == 0x14);

// NiPropertyState (renderer +0x0C): 12 property pointers, always all set (FUN_00159CD0 dereferences
// +0x0C/+0x10/+0x20/+0x30 without a NULL check).
struct NiPropertyState {
    NiObjectHeader obj;           // +0x00
    GuestPtr alpha;               // +0x08 NiAlphaProperty      FUN_00159270
    GuestPtr dither;              // +0x0C NiDitherProperty     flags bit 0 -> D3DRS_DITHERENABLE
    GuestPtr fog;                 // +0x10 NiFogProperty        FUN_00159700, color FUN_00159900
    GuestPtr material;            // +0x14 NiMaterialProperty   FUN_00161A80 + FUN_00161800 (D3DMATERIAL8)
    GuestPtr rendererSpecific;    // +0x18 NiRendererSpecificProperty (FUN_0015AA60: data type 6 = own pipeline)
    GuestPtr shade;               // +0x1C NiShadeProperty      FUN_00159980 bit 0: 1 = Gouraud, 0 = flat
    GuestPtr specular;            // +0x20 NiSpecularProperty   bit 0 -> D3DRS_SPECULARENABLE (0x24486C)
    GuestPtr stencil;             // +0x24 NiStencilProperty    FUN_001599E0 (also sets the cull mode)
    GuestPtr texturing;           // +0x28 NiTexturingProperty  texture pipeline +0x5C8 (FUN_0015AB40 ff.)
    GuestPtr vertexColor;         // +0x2C NiVertexColorProperty lighting setup FUN_0015BF80
    GuestPtr wireframe;           // +0x30 NiWireframeProperty  bit 0 -> FILLMODE wireframe
    GuestPtr zbuffer;             // +0x34 NiZBufferProperty    FUN_00159B60
};
static_assert(sizeof(NiPropertyState) == 0x38 && offsetof(NiPropertyState, zbuffer) == 0x34);

// NiAlphaProperty (0x1C) - FUN_00159270; default flags 0xEC (render-state cache ctor 0x158DE0).
//   bit 0      blend enable          bits 1-4  src AlphaFunction     bits 5-8  dst AlphaFunction
//   bit 9      alpha test enable     bits 10-12 TestFunction          +0x18     alpha ref (0..255)
struct NiAlphaProperty {
    NiProperty base;              // flags at +0x14
    std::uint8_t testRef;         // +0x18
    std::uint8_t pad19[3];
};
static_assert(sizeof(NiAlphaProperty) == 0x1C && offsetof(NiAlphaProperty, testRef) == 0x18);
// AlphaFunction (viewer strings 0x104180) -> D3D blend (table rs+0x100 of FUN_00158DE0, Xbox GL values):
// 0 ONE, 1 ZERO, 2 SRCCOLOR, 3 INVSRCCOLOR, 4 DESTCOLOR, 5 INVDESTCOLOR, 6 SRCALPHA, 7 INVSRCALPHA,
// 8 DESTALPHA, 9 INVDESTALPHA, 10 SRCALPHASAT (11..15 unused).
// Alpha TestFunction (strings 0x119210) -> D3DCMP (table rs+0x12C): 0 ALWAYS, 1 LESS, 2 EQUAL,
// 3 LESSEQUAL, 4 GREATER, 5 NOTEQUAL, 6 GREATEREQUAL, 7 NEVER.

// NiZBufferProperty (0x1C) - FUN_00159B60: bit 0 test (ZFUNC = LESSEQUAL 0x203, off: ALWAYS 0x207 with
// z still enabled), bit 1 write (ZWRITEENABLE).
struct NiZBufferProperty {
    NiProperty base;
    std::uint32_t field18;        // +0x18 not read by REN
};
static_assert(sizeof(NiZBufferProperty) == 0x1C);

// NiStencilProperty (0x38) - FUN_001599E0. Stencil TestFunction (table rs+0x1A0): 0 NEVER, 1 LESS,
// 2 EQUAL, 3 LESSEQUAL, 4 GREATER, 5 NOTEQUAL, 6 GREATEREQUAL, 7 ALWAYS (note: a different order than the
// alpha test). Action (table rs+0x1C0): 0 KEEP, 1 ZERO, 2 REPLACE, 3 INCR (saturating), 4 DECR
// (saturating), 5 INVERT. DrawMode -> cull (table rs+0x1D8, index drawMode * 2 + leftRightSwap):
// 0 CCW_OR_BOTH -> CW (0x900), 1 CCW -> CW, 2 CW -> CCW (0x901), 3 BOTH -> NONE; with left-right swap
// CW and CCW exchange. Default property: drawMode 0 -> D3DCULL_CW.
struct NiStencilProperty {
    NiProperty base;              // +0x00
    std::uint8_t enable;          // +0x18
    std::uint8_t pad19[3];
    std::uint32_t func;           // +0x1C TestFunction
    std::uint32_t ref;            // +0x20
    std::uint32_t mask;           // +0x24
    std::uint32_t failAction;     // +0x28
    std::uint32_t zFailAction;    // +0x2C
    std::uint32_t passAction;     // +0x30
    std::uint32_t drawMode;       // +0x34
};
static_assert(sizeof(NiStencilProperty) == 0x38 && offsetof(NiStencilProperty, drawMode) == 0x34);

// NiMaterialProperty (0x5C) - FUN_00161800 builds D3DMATERIAL8: Diffuse = (+0x28, alpha +0x50),
// Ambient = (+0x1C, alpha), Specular = (+0x34, alpha), Emissive = (+0x40, alpha), Power = +0x4C;
// rebuilt when +0x54 (revision) changes. Renderer data at +0x58 (purged by slot 59).
struct NiMaterialProperty {
    NiProperty base;              // +0x00
    std::uint32_t field18;        // +0x18 not read by the renderer
    NiColor ambient;              // +0x1C
    NiColor diffuse;              // +0x28
    NiColor specular;             // +0x34
    NiColor emissive;             // +0x40
    float shininess;              // +0x4C
    float alpha;                  // +0x50
    std::uint32_t revision;       // +0x54
    GuestPtr rendererData;        // +0x58
};
static_assert(sizeof(NiMaterialProperty) == 0x5C && offsetof(NiMaterialProperty, ambient) == 0x1C &&
              offsetof(NiMaterialProperty, emissive) == 0x40 && offsetof(NiMaterialProperty, alpha) == 0x50);

// NiVertexColorProperty (0x20) - FUN_0015BF80 (viewer strings 0x101370/0x1013F0):
//   source   0 SOURCE_IGNORE, 1 SOURCE_EMISSIVE, 2 SOURCE_AMB_DIFF
//   lighting 0 LIGHTING_E, 1 LIGHTING_E_A_D
struct NiVertexColorProperty {
    NiProperty base;
    std::uint32_t source;         // +0x18
    std::uint32_t lighting;       // +0x1C
};
static_assert(sizeof(NiVertexColorProperty) == 0x20);

// NiFogProperty (0x28) - FUN_00159700: flags bit 1 enable, bits 3-4 function (0 FOG_Z_LINEAR,
// 1 FOG_RANGE_SQ, 2 FOG_VERTEX_ALPHA); +0x18 depth (fraction of far - near); +0x1C color (FUN_00159900).
struct NiFogProperty {
    NiProperty base;
    float depth;                  // +0x18
    NiColor color;                // +0x1C
};
static_assert(sizeof(NiFogProperty) == 0x28);

// NiTexturingProperty (0x3C) - ctor 0x109D30, viewer strings vf08 0x10A970.
//   applyMode 0 APPLY_REPLACE, 1 APPLY_DECAL, 2 APPLY_MODULATE (ctor default 2). REPLACE with a base
//   texture switches lighting off (FUN_0015BF80).
// Map slots (NiTArray at +0x20): 0 base, 1 dark, 2 detail, 3 gloss, 4 glow, 5 bump, 6 decal.
struct NiTexturingProperty {
    NiProperty base;              // +0x00 (flags +0x14)
    std::uint32_t applyMode;      // +0x18 (pipeline vf2 0x1643A6 copies it to pipeline +0x2F7C)
    std::uint8_t hasNonBaseMaps;  // +0x1C phase 2: 1 if any of maps[1 .. size-1] is non-NULL (FUN_00109F70,
                                  //       LoadBinary tail 0x10A730..0x10A74F); pipeline vf2 0x16440C takes
                                  //       the single-stage path only when it is 0
    std::uint8_t pad1D[3];
    GuestPtr mapsVtable;          // +0x20 NiTArray<Map*> vtable 0x2A6BBC
    GuestPtr maps;                // +0x24 Map*[maxSize] (FUN_0015BF80: **(+0x24) = base map)
    std::uint32_t mapsMaxSize;    // +0x28 (7)
    std::uint32_t mapsSize;       // +0x2C slots in use (viewer loop count; vf2 0x164477: decals = size - 6)
    std::uint32_t mapsUsed;       // +0x30 non-empty slots
    std::uint32_t mapsGrowBy;     // +0x34
    std::uint32_t decalMapCount;  // +0x38 phase 2: non-empty maps at index > 5 (vf04 0x10A1B0)
};
static_assert(sizeof(NiTexturingProperty) == 0x3C && offsetof(NiTexturingProperty, maps) == 0x24 &&
              offsetof(NiTexturingProperty, mapsSize) == 0x2C &&
              offsetof(NiTexturingProperty, hasNonBaseMaps) == 0x1C);
inline constexpr std::uint32_t kMapBase = 0, kMapDark = 1, kMapDetail = 2, kMapGloss = 3, kMapGlow = 4,
                               kMapBump = 5, kMapDecal = 6;
// NiTexturingProperty::ApplyMode (viewer strings 0x10A970; FUN_0015BF80 / pipeline vf2 0x1643B9).
inline constexpr std::uint32_t kApplyReplace = 0, kApplyDecal = 1, kApplyModulate = 2;

// NiTexturingProperty::Map (bump maps are longer, +0x18..+0x2C luma/bump matrix) - viewer 0x10A970:
//   clamp  0 CLAMP_S_CLAMP_T, 1 CLAMP_S_WRAP_T, 2 WRAP_S_CLAMP_T, 3 WRAP_S_WRAP_T (strings 0x109960)
//   filter 0 NEAREST, 1 BILERP, 2 TRILERP, 3 NEAREST_MIPNEAREST, 4 NEAREST_MIPLERP, 5 BILERP_MIPNEAREST
//          (strings 0x109A00; the pipeline maps them through the triple table DAT_0030FEFC, FUN_00161E40)
// Phase 2: the pipeline reads texture/clamp/filter/uvSet in vf2 0x16439F..0x1643DD (base map; a missing map
// counts as clamp 3, filter 0) and FUN_00160270 (screen textures).
struct NiTexturingMap {
    GuestPtr vtable;              // +0x00
    GuestPtr texture;             // +0x04 NiTexture* (m_spTexture)
    std::uint32_t clamp;          // +0x08 m_eClamp
    std::uint32_t filter;         // +0x0C m_eFilter
    std::uint32_t uvSet;          // +0x10 "TexCoord"
    std::int16_t l;               // +0x14 m_sL  ASSUMPTION: PS2 mip parameters, unused by REN
    std::int16_t k;               // +0x16 m_sK
};
static_assert(sizeof(NiTexturingMap) == 0x18);

// Phase 2: renderer data of a texture (NiTexture +0x20). NiXBoxSourceTextureData (vtable 0x2AB97C) and
// NiPCTextureData (gfx/nipc_renderer.h) share these fields: data vf2 0x157100 = +0x48 width, vf3 0x157110
// = +0x4C height, vf5 0x157120 = +0x50 mip level count (texture manager vf2 0x15FA04: "has mipmaps" =
// vf5() > 1, used by FUN_00161E40), vf6 0x157140 = +0x3C D3D texture.
inline constexpr std::uint32_t kNiTextureRendererData = 0x20;
inline constexpr std::uint32_t kTextureDataWidth = 0x48;
inline constexpr std::uint32_t kTextureDataHeight = 0x4C;
inline constexpr std::uint32_t kTextureDataMipLevels = 0x50;
// NiTexture::GetWidth/GetHeight (vf11/vf12) without renderer data: NiSourceTexture (vtable 0x2A8034,
// 0x118000/0x118020) reads width[0]/height[0] of its NiPixelData (+0x34 -> +0x34/+0x38); NiRenderedTexture
// (vtable 0x2A9EB0, 0x141E40/0x13FFD0) returns +0x2C/+0x30.
inline constexpr GuestPtr kNiSourceTextureVtable = 0x002A8034;
inline constexpr GuestPtr kNiRenderedTextureVtable = 0x002A9EB0;
inline constexpr std::uint32_t kNiRenderedTextureWidth = 0x2C;
inline constexpr std::uint32_t kNiRenderedTextureHeight = 0x30;

// ---- dynamic effects (renderer.md 5.5, FUN_0015BA40 / FUN_0015B6A0 / FUN_0014D3F0) -------------------

// NiDynamicEffectState (renderer +0x10; may be 0, e.g. screen polygons). FUN_0014D3F0 sorts effects in:
// GetEffectType (vf40, +0xA0) < 4 -> light list (sorted by NiLight index +0xC4), else by
// NiTextureEffect type +0x168.
struct NiDynamicEffectState {
    NiObjectHeader obj;           // +0x00
    std::uint8_t hasTextureEffects;  // +0x08
    std::uint8_t pad09[3];
    GuestPtr lights;              // +0x0C list node {GuestPtr next, GuestPtr light}
    GuestPtr projectedLights;     // +0x10 list (TextureEffect type 0)
    GuestPtr projectedShadows;    // +0x14 list (type 1)
    GuestPtr environmentMap;      // +0x18 NiTextureEffect* (type 2)
    GuestPtr fogMap;              // +0x1C NiTextureEffect* (type 3)
};
static_assert(offsetof(NiDynamicEffectState, lights) == 0x0C && offsetof(NiDynamicEffectState, fogMap) == 0x1C);
struct NiListNode {
    GuestPtr next;                // +0x00
    GuestPtr item;                // +0x04
};

// GetEffectType (vf40) results: 0x13DB0 ambient 0, 0x9B6A0 directional 1, 0x13E10 point 2, 0xF1BD0 spot 3,
// 0xF1C50 texture effect 4.
inline constexpr std::uint32_t kNiDynamicEffectVfGetType = 0xA0;  // vtable byte offset (slot 40)
enum class NiEffectType : std::uint32_t { Ambient = 0, Directional = 1, Point = 2, Spot = 3, TextureEffect = 4 };
// Phase 2: the implementations (capstone: `xor eax,eax; ret` / `mov eax,N; ret`) in the vtables of
// NiAmbientLight 0x2A7010, NiDirectionalLight 0x2A7690, NiPointLight 0x2A2EF8, NiSpotLight 0x2A80D8,
// NiTextureEffect 0x2A8488 (slot 40 each, retail_eu_ni_classes.json). ni_extract compares the slot with
// these addresses instead of calling guest code.
inline constexpr GuestPtr kGetEffectTypeAmbient = 0x00013DB0;
inline constexpr GuestPtr kGetEffectTypeDirectional = 0x0009B6A0;
inline constexpr GuestPtr kGetEffectTypePoint = 0x00013E10;
inline constexpr GuestPtr kGetEffectTypeSpot = 0x000F1BD0;
inline constexpr GuestPtr kGetEffectTypeTextureEffect = 0x000F1C50;

// NiLight (0x110) and subclasses. World position = NiAVObject world translate (+0xA0).
struct NiLightLayout {
    NiAVObject av;                // +0x00 (world transform +0x70, translate +0xA0)
    std::uint8_t on;              // +0xC0 NiDynamicEffect switch (FUN_0015BA40: lights with on == 0 skipped)
    std::uint8_t padC1[3];
    std::uint32_t index;          // +0xC4 D3D light index (D3DLIGHT8 data +0x6C)
    std::uint32_t fieldC8;        // +0xC8
    std::uint32_t revision;       // +0xCC (FUN_0015B6A0 refills the D3DLIGHT8 when it changes)
    std::uint8_t restD0[0xE0 - 0xD0];
    float dimmer;                 // +0xE0 (lights with dimmer < 0.01 are skipped)
    NiColor ambient;              // +0xE4 (D3DLIGHT8.Ambient = dimmer * this; ambient lights add it to D3DRS_AMBIENT)
    NiColor diffuse;              // +0xF0
    NiColor specular;             // +0xFC
    GuestPtr rendererData;        // +0x108 NiXBoxRenderer D3DLIGHT8 (0x70 bytes; stays 0 with NiPCRenderer)
    std::uint32_t field10C;
};
static_assert(sizeof(NiLightLayout) == 0x110 && offsetof(NiLightLayout, on) == 0xC0 &&
              offsetof(NiLightLayout, dimmer) == 0xE0 && offsetof(NiLightLayout, rendererData) == 0x108);
// NiDirectionalLight (0x120): +0x110 world direction (D3DLIGHT8.Direction) = row 0 of the world rotation
//   (NiDirectionalLight::vf39 0x112BF0 copies +0x70..+0x78 and bumps the revision +0xCC).
// NiPointLight (0x120): +0x110 constant, +0x114 linear, +0x118 quadratic attenuation (Range = 0x5F7FFFFF =
//   float(sqrt(double FLT_MAX)), 0x15B761).
// NiSpotLight (0x140): NiPointLight + +0x120 world direction, +0x12C spot angle in degrees,
//   +0x130 exponent: Theta = Phi = angle * DAT_002F91D4 * 0.0111111f (FUN_0015B6A0 0x15B725, constant
//   0x2A5FA8), Falloff = exponent. Phase 2: DAT_002F91D4 = pi, set by the static initializer FUN_00226820
//   (4 * fpatan(1, 1)), so Theta = Phi = 2 * angle in radians (the full cone of the half angle).
inline constexpr std::uint32_t kNiDirectionalLightDirection = 0x110;
inline constexpr std::uint32_t kNiPointLightAttenuation = 0x110;  // 3 floats
inline constexpr std::uint32_t kNiSpotLightDirection = 0x120;
inline constexpr std::uint32_t kNiSpotLightAngle = 0x12C;
inline constexpr std::uint32_t kNiSpotLightExponent = 0x130;
inline constexpr std::uint32_t kNiTextureEffectType = 0x168;      // 0 projected light, 1 projected shadow,
                                                                  // 2 environment map, 3 fog map
// Phase 2: byte +0x170 (clipping plane enable, renderer.md 5.6). Effects with it set increment the default
// pipeline's +0x770 counter (FUN_00164200 -> FUN_00162AD0/FUN_00162BB0), which disables the single-stage
// path of pipeline vf2 (0x1643F7).
inline constexpr std::uint32_t kNiTextureEffectClipped = 0x170;

// Phase 2: default properties of a new NiPropertyState (ctor FUN_0014D770 -> FUN_0014D5A0), guest globals
// holding the property pointers, in NiPropertyState order (alpha .. zbuffer). NiRenderer slot 55
// (0x14C570) builds its temporary state from them.
inline constexpr GuestPtr kDefaultPropertyGlobals[12] = {
    0x002F9148,  // NiAlphaProperty
    0x002F9758,  // NiDitherProperty
    0x002F9174,  // NiFogProperty
    0x002F9824,  // NiMaterialProperty
    0x002F99A4,  // NiRendererSpecificProperty
    0x002F99C0,  // NiShadeProperty
    0x002F9A34,  // NiSpecularProperty
    0x002F9A68,  // NiStencilProperty
    0x002F942C,  // NiTexturingProperty
    0x002F90B4,  // NiVertexColorProperty
    0x002F9BE4,  // NiWireframeProperty
    0x002F9C08,  // NiZBufferProperty
};
// Slot 55's own properties (0x14C570): NiVertexColorProperty (ctor FUN_00011A10) with source 1 EMISSIVE and
// lighting 0 E, NiAlphaProperty (ctor FUN_00013D80, flags 0xEC) with flags = (flags & 0xFEED) | 0xED = 0xED,
// NiZBufferProperty (ctor FUN_0003F460, flags 3) with flags & 0xFC = 0.
inline constexpr std::uint16_t kScreenTextureAlphaFlags = 0xED;
inline constexpr std::uint16_t kScreenTextureZBufferFlags = 0x00;
inline constexpr std::uint32_t kScreenTextureVertexColorSource = 1;
inline constexpr std::uint32_t kScreenTextureVertexColorLighting = 0;
// Identity world transform of slots 54/55: rotation DAT_002D7780 (3 rows of 4 floats), translation
// DAT_002F90BC (NiPoint3::ZERO, BSS), scale 1.0 (0x14B630 / 0x14C700).

// ---- textures (renderer.md 5.2) ------------------------------------------------------------------------

// NiPixelFormat (0x20, ctor FUN_001409A0; copied by FUN_00111260). format:
//   0 RGB, 1 RGBA, 2 PAL, 3 PALA, 4 DXT1, 5 DXT3, 6 DXT5, 8 BUMP, 9 BUMPLUMA (FUN_000EDC70, FUN_00111260:
//   formats 4..6 are block-compressed: ((w+3)&~3)*((h+3)&~3)/16 blocks of 8 (DXT1) or 16 bytes).
struct NiPixelFormat {
    std::uint32_t format;         // +0x00
    std::uint32_t redMask;        // +0x04
    std::uint32_t greenMask;      // +0x08
    std::uint32_t blueMask;       // +0x0C
    std::uint32_t alphaMask;      // +0x10
    std::uint8_t bitsPerPixel;    // +0x14 (FUN_000EDC70 reads the byte)
    std::uint8_t pad15[3];
    std::uint32_t channels0;      // +0x18 packed channel descriptor (FUN_000EDC70: (x & 0xFC000) >> 14 =
                                  //       green bits of 16-bit RGB/RGBA: 6 -> R5G6B5, 4 -> A4R4G4B4, else A1R5G5B5)
    std::uint32_t channels1;      // +0x1C
};
static_assert(sizeof(NiPixelFormat) == 0x20);
enum class NiPixelLayout : std::uint32_t {
    RGB = 0, RGBA = 1, PAL = 2, PALA = 3, DXT1 = 4, DXT3 = 5, DXT5 = 6, BUMP = 8, BUMPLUMA = 9
};

// NiPixelData (0x4C) - ctors 0x110FF0 / FUN_00111260.
struct NiPixelData {
    NiObjectHeader obj;           // +0x00
    std::uint32_t field08;        // +0x08
    NiPixelFormat format;         // +0x0C
    GuestPtr palette;             // +0x2C NiPalette* (PAL/PALA)
    GuestPtr pixels;              // +0x30 all levels (FUN_00151190)
    GuestPtr widths;              // +0x34 u32[mipCount]
    GuestPtr heights;             // +0x38 u32[mipCount]
    GuestPtr offsets;             // +0x3C u32[mipCount + 1] byte offsets into pixels (last = total)
    std::uint32_t mipCount;       // +0x40
    std::uint32_t bytesPerPixel;  // +0x44 (bpp / 8; 0 for DXT)
    std::uint32_t revision;       // +0x48 (ctor: 1)
};
static_assert(sizeof(NiPixelData) == 0x4C && offsetof(NiPixelData, format) == 0x0C &&
              offsetof(NiPixelData, palette) == 0x2C && offsetof(NiPixelData, pixels) == 0x30 &&
              offsetof(NiPixelData, mipCount) == 0x40 && offsetof(NiPixelData, revision) == 0x48);

// NiPalette (0x20) - ctors 0x110AB0 / FUN_00110D40 (256 entries of 4 bytes copied to +0x18).
// ASSUMPTION: entry byte order r, g, b, a (NetImmerse PaletteEntry); texconv verifies with real data.
struct NiPalette {
    NiObjectHeader obj;           // +0x00
    GuestPtr prev;                // +0x08 global palette list
    GuestPtr next;                // +0x0C
    std::uint32_t revision;       // +0x10 (ctor: 1)
    GuestPtr rendererData;        // +0x14 (slot 69; NiPCRenderer leaves it 0)
    GuestPtr entries;             // +0x18 4 bytes x 256
    std::uint8_t hasAlpha;        // +0x1C (ctor argument)
    std::uint8_t pad1D[3];
};
static_assert(sizeof(NiPalette) == 0x20 && offsetof(NiPalette, entries) == 0x18 && offsetof(NiPalette, hasAlpha) == 0x1C);

// NiTexture (0x2C) / NiSourceTexture (0x40). FormatPrefs at +0x14 (FUN_001572D0 param_2):
//   pixelLayout 0 PALETTIZED_8, 1 HIGH_COLOR_16, 2 TRUE_COLOR_32, 3 COMPRESSED, 4 BUMPMAP, 5 PIX_DEFAULT
//   alphaFormat 0 NONE, 1 BINARY, 2 SMOOTH, 3 ALPHA_DEFAULT; mipMapped 0 NO, 1 YES, 2 MIP_DEFAULT
//   (ASSUMPTION: names after NetImmerse 4.2; the numeric use is FUN_001572D0's).
// Field order verified in M3 phase 2 (texconv agent; integration fixed the names): FUN_0015E010 0x15E13B
// derives the mipmap flag from +0x18 (== 1, or == 2 with DAT_0030EF84), FUN_001572D0 prefers PALA/1555
// when +0x1C is 1/2 (alpha BINARY/SMOOTH); the NIF stream order is the same (tex::XboxFormatPrefs).
struct NiTextureFormatPrefs {
    std::uint32_t pixelLayout;    // +0x14
    std::uint32_t mipMapped;      // +0x18
    std::uint32_t alphaFormat;    // +0x1C
};
struct NiSourceTextureLayout {
    NiObjectHeader obj;           // +0x00
    std::uint8_t net[0x14 - 8];
    NiTextureFormatPrefs prefs;   // +0x14
    GuestPtr rendererData;        // +0x20 renderer data chain (gfx::NiPCTextureData, nipc_renderer.h)
    std::uint32_t field24[3];     // +0x24
    GuestPtr fileName;            // +0x30 (FUN_00118520)
    GuestPtr pixelData;           // +0x34 NiPixelData* (FUN_00118600)
    std::uint32_t field38[2];
};
static_assert(sizeof(NiSourceTextureLayout) == 0x40 && offsetof(NiSourceTextureLayout, rendererData) == 0x20 &&
              offsetof(NiSourceTextureLayout, pixelData) == 0x34);

// ---- 2D (renderer.md 4, slots 54/55) ---------------------------------------------------------------------

// NiScreenPolygon (0x24) - ctor FUN_00106D50, draw FUN_00106AF0 (renderer +0x0C = poly +0x08, +0x10 = 0),
// slot 54 0x14B5F0 + FUN_00149F30/FUN_001585A0: each vertex (x, y, z) becomes (d*x, d*y, d) with
// d = near + z * (far - near) of the current camera, drawn with view = identity (+0x354) and the
// projection +0x394 = [[2,0,0,0],[0,2,0,0],[-1,-1,P22,1],[0,0,P32,0]] (P22/P32 of the camera projection),
// i.e. x, y in [0, 1] with y up. Primitive: triangle fan (7), or strip (6) if +0x1E != 0 (FUN_00106A10);
// primitives = vertexCount - 2. Colors packed ARGB with ROUND(c * 255), no lighting effects (effect state 0).
struct NiScreenPolygon {
    NiObjectHeader obj;           // +0x00
    GuestPtr propertyState;       // +0x08 NiPropertyState*
    std::uint16_t vertexCount;    // +0x0C
    std::uint16_t pad0E;
    GuestPtr vertices;            // +0x10 NiPoint3[vertexCount]
    GuestPtr uvs;                 // +0x14 NiPoint2[vertexCount] or 0
    GuestPtr colors;              // +0x18 NiColorA[vertexCount] or 0
    std::uint16_t field1C;        // +0x1C
    std::uint8_t isStrip;         // +0x1E
    std::uint8_t field1F;
    std::uint32_t field20;        // +0x20
};
static_assert(sizeof(NiScreenPolygon) == 0x24 && offsetof(NiScreenPolygon, isStrip) == 0x1E);

// NiScreenTexture (0x20) - ctor 0x10B340; slot 55 0x14C570 + FUN_0014BFF0/FUN_0014A050/FUN_00157EA0.
// Each rect becomes a quad of pre-transformed vertices (FVF XYZRHW|DIFFUSE|TEX1, z = 0, rhw = 1) in pixels
// of the current render target: x = left - 0.5 (+ width for corners 2, 3), y = top - 0.5 (+ height for
// corners 1, 3); left/top wrap into [0, target size) first (while < 0 add size, while >= size subtract);
// u = texLeft / texWidth (+ width / texWidth), v = texTop / texHeight (+ height / texHeight) with the size
// of the base map texture of `texturing` (NiTexture vf11/vf12); corner order (L,T) (L,B) (R,T) (R,B),
// indices (0,1,2) (2,1,3) per quad. The original draws them with its own property state (renderer
// +0x5A8): NiAlphaProperty flags 0xED (blend SRCALPHA/INVSRCALPHA), NiZBufferProperty with test and write
// off, NiVertexColorProperty (SOURCE_EMISSIVE, LIGHTING_E) and the screen texture's NiTexturingProperty.
// Phase 2: LoadBinary NiScreenTexture::vf03 0x10B7C0 streams the six u16 in this order, then the color
// (FUN_00151A30). FUN_0014A050 adds +0x04/+0x06 to left/top as width/height; the game's legal-screen
// tiles store larger values (the rects overlap and are clipped; draw order + CLAMP give the picture).
struct NiScreenTextureRect {
    std::int16_t top;             // +0x00
    std::int16_t left;            // +0x02
    std::uint16_t width;          // +0x04
    std::uint16_t height;         // +0x06
    std::uint16_t texTop;         // +0x08
    std::uint16_t texLeft;        // +0x0A
    NiColorA color;               // +0x0C
};
static_assert(sizeof(NiScreenTextureRect) == 0x1C);
struct NiScreenTexture {
    NiObjectHeader obj;           // +0x00
    GuestPtr rects;               // +0x08 NiScreenTextureRect[]
    std::uint32_t rectCapacity;   // +0x0C phase 2: array {data, capacity, count} (FUN_0010B0C0, FUN_0010B5D0)
    std::uint16_t rectCount;      // +0x10 (u32 in the array object, FUN_0014A050 reads the low u16)
    std::uint16_t pad12;
    GuestPtr texturing;           // +0x14 NiTexturingProperty*
    std::uint16_t revision;       // +0x18 (FUN_0014BFF0 rebuilds the quads when != 0 and resets it)
    std::uint16_t pad1A;
    GuestPtr rendererData;        // +0x1C
};
static_assert(sizeof(NiScreenTexture) == 0x20 && offsetof(NiScreenTexture, texturing) == 0x14);

// ---- camera (renderer.md 3.3) -----------------------------------------------------------------------------

// NiCamera (0x250): NiCamera::Click FUN_00100C50 passes these to SetCameraData (slot 47).
inline constexpr std::uint32_t kNiCameraWorldLocation = 0xA0;   // NiAVObject world translate
inline constexpr std::uint32_t kNiCameraWorldDirection = 0x110;
inline constexpr std::uint32_t kNiCameraWorldUp = 0x11C;
inline constexpr std::uint32_t kNiCameraWorldRight = 0x128;
inline constexpr std::uint32_t kNiCameraFrustum = 0x134;
inline constexpr std::uint32_t kNiCameraPort = 0x14C;
inline constexpr std::uint32_t kNiCameraRenderer = 0x1A8;

}  // namespace gfx::ni
