// Unit tests of gfx/ni_extract (GFX_CONTRACT.md 4): synthetic NetImmerse guest objects in guest-visible
// pages (gfx::AllocGuestPages), built with the verified layouts of gfx/ni_guest_layouts.h. No guest code
// runs (ExtractContext::allowGuestCalls = false); vtables are fake tables holding the implementation
// addresses ni_extract dispatches on.
#include "doctest.h"

#include "gfx/guest_memory.h"
#include "gfx/ni_extract.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

using namespace gfx;
using namespace gfx::ni;

bool Near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

// Bump allocator over one block of guest pages (zero-filled).
class GuestHeap {
public:
    explicit GuestHeap(std::size_t size = 1u << 20) : size_(size) {
        base_ = static_cast<std::uint8_t*>(AllocGuestPages(size));
        REQUIRE(base_ != nullptr);
    }
    ~GuestHeap() { FreeGuestPages(base_); }
    GuestHeap(const GuestHeap&) = delete;
    GuestHeap& operator=(const GuestHeap&) = delete;

    template <class T>
    T* New(std::size_t count = 1) {
        used_ = (used_ + 15) & ~std::size_t{15};
        REQUIRE(used_ + sizeof(T) * count <= size_);
        T* p = reinterpret_cast<T*>(base_ + used_);
        std::memset(static_cast<void*>(p), 0, sizeof(T) * count);
        used_ += sizeof(T) * count;
        return p;
    }
    static GuestPtr Addr(const void* p) { return static_cast<GuestPtr>(reinterpret_cast<std::uintptr_t>(p)); }

private:
    std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    std::size_t used_ = 0;
};

GuestPtr A(const void* p) {
    return GuestHeap::Addr(p);
}

// A NiPropertyState with default-like properties (all flags 0 unless set by the test).
struct Scene {
    GuestHeap heap;
    NiAlphaProperty* alpha;
    NiProperty* dither;
    NiFogProperty* fog;
    NiMaterialProperty* material;
    NiProperty* rendererSpecific;
    NiProperty* shade;
    NiProperty* specular;
    NiStencilProperty* stencil;
    NiTexturingProperty* texturing;
    NiVertexColorProperty* vertexColor;
    NiProperty* wireframe;
    NiZBufferProperty* zbuffer;
    NiPropertyState* state;
    GuestPtr* maps;
    NiTexturingMap* baseMap;
    NiSourceTextureLayout* texture;
    std::uint32_t* textureData;  // renderer data view (+0x48 width, +0x4C height, +0x50 mips)
    ExtractArena arena;
    StateShadow shadow;
    CameraDesc camera = DefaultCamera(640, 480);
    std::uint32_t* lightVtables[5];  // ambient, directional, point, spot, texture effect
    std::uint32_t* geometryVtables[5];
    GuestPtr defaults[12];

    Scene() {
        alpha = heap.New<NiAlphaProperty>();
        dither = heap.New<NiProperty>();
        fog = heap.New<NiFogProperty>();
        material = heap.New<NiMaterialProperty>();
        rendererSpecific = heap.New<NiProperty>();
        shade = heap.New<NiProperty>();
        specular = heap.New<NiProperty>();
        stencil = heap.New<NiStencilProperty>();
        texturing = heap.New<NiTexturingProperty>();
        vertexColor = heap.New<NiVertexColorProperty>();
        wireframe = heap.New<NiProperty>();
        zbuffer = heap.New<NiZBufferProperty>();
        state = heap.New<NiPropertyState>();
        state->alpha = A(alpha);
        state->dither = A(dither);
        state->fog = A(fog);
        state->material = A(material);
        state->rendererSpecific = A(rendererSpecific);
        state->shade = A(shade);
        state->specular = A(specular);
        state->stencil = A(stencil);
        state->texturing = A(texturing);
        state->vertexColor = A(vertexColor);
        state->wireframe = A(wireframe);
        state->zbuffer = A(zbuffer);
        // NetImmerse defaults: alpha 0xEC, z-buffer test+write, smooth shading, modulate, E_A_D + IGNORE.
        alpha->base.flags = 0xEC;
        zbuffer->base.flags = 3;
        shade->flags = 1;
        texturing->applyMode = kApplyModulate;
        vertexColor->source = 0;
        vertexColor->lighting = 1;
        material->ambient = {0.5f, 0.5f, 0.5f};
        material->diffuse = {1.0f, 0.75f, 0.5f};
        material->specular = {0.1f, 0.2f, 0.3f};
        material->emissive = {0.0f, 0.25f, 0.0f};
        material->shininess = 8.0f;
        material->alpha = 0.5f;
        maps = heap.New<GuestPtr>(7);
        texturing->maps = A(maps);
        texturing->mapsMaxSize = 7;
        texturing->mapsSize = 1;
        baseMap = heap.New<NiTexturingMap>();
        texture = heap.New<NiSourceTextureLayout>();
        texture->obj.vtable = kNiSourceTextureVtable;  // compared only, never dereferenced
        textureData = heap.New<std::uint32_t>(0x70 / 4);
        textureData[kTextureDataWidth / 4] = 64;
        textureData[kTextureDataHeight / 4] = 32;
        textureData[kTextureDataMipLevels / 4] = 1;
        texture->rendererData = A(textureData);
        baseMap->texture = A(texture);
        baseMap->clamp = 3;   // WRAP_S_WRAP_T
        baseMap->filter = 1;  // BILERP
        baseMap->uvSet = 0;
        const GuestPtr typeFns[5] = {kGetEffectTypeAmbient, kGetEffectTypeDirectional, kGetEffectTypePoint,
                                     kGetEffectTypeSpot, kGetEffectTypeTextureEffect};
        for (int i = 0; i < 5; ++i) {
            lightVtables[i] = heap.New<std::uint32_t>(48);
            lightVtables[i][kNiDynamicEffectVfGetType / 4] = typeFns[i];
        }
        const GuestPtr vf12[5] = {kVertexCountGeometryData, kVertexCountDynamicData, kVertexCountGeometryData,
                                  kVertexCountTrianglesData, kVertexCountTriStripData};
        const GuestPtr vf15[5] = {kTriangleCountTriBased, kTriangleCountDynamicData, kTriangleCountTriBased,
                                  kTriangleCountActive, kTriangleCountActive};
        for (int i = 0; i < 5; ++i) {  // shape, dynamic shape, strips, triangles, strip
            geometryVtables[i] = heap.New<std::uint32_t>(20);
            geometryVtables[i][kNiGeometryDataVfVertexCount / 4] = vf12[i];
            geometryVtables[i][kNiGeometryDataVfTriangleCount / 4] = vf15[i];
        }
        const GuestPtr own[12] = {A(alpha),     A(dither),   A(fog),     A(material),  A(rendererSpecific),
                                  A(shade),     A(specular), A(stencil), A(texturing), A(vertexColor),
                                  A(wireframe), A(zbuffer)};
        std::memcpy(defaults, own, sizeof(defaults));
    }
    void SetBaseMap(bool on) { maps[0] = on ? A(baseMap) : 0; }
    ExtractContext Context() {
        ExtractContext c;
        c.arena = &arena;
        c.camera = &camera;
        c.shadow = &shadow;
        c.allowGuestCalls = false;
        c.defaultProperties = defaults;
        return c;
    }
    ExtractStatus State(RenderStateDesc* st, LightSetDesc* lights = nullptr, GuestPtr effectState = 0,
                        float scale = 1.0f, std::uint32_t uvSets = 1) {
        StateInput in;
        in.propertyState = A(state);
        in.effectState = effectState;
        in.worldScale = scale;
        in.uvSetCount = uvSets;
        LightSetDesc dummy;
        return ExtractState(Context(), in, st, lights != nullptr ? lights : &dummy, nullptr);
    }
    // A light of the given type (0 ambient .. 3 spot) with colors and world translate.
    NiLightLayout* NewLight(int type, std::uint32_t index, float dimmer = 1.0f) {
        const std::size_t size = type == 3 ? 0x140 : 0x120;
        auto* raw = heap.New<std::uint8_t>(size);
        auto* l = reinterpret_cast<NiLightLayout*>(raw);
        l->av.obj.vtable = A(lightVtables[type]);
        l->on = 1;
        l->index = index;
        l->dimmer = dimmer;
        l->ambient = {0.2f, 0.4f, 0.6f};
        l->diffuse = {1.0f, 0.5f, 0.25f};
        l->specular = {0.5f, 0.5f, 0.5f};
        l->av.world.translate[0] = 1.0f;
        l->av.world.translate[1] = 2.0f;
        l->av.world.translate[2] = 3.0f;
        auto* f = reinterpret_cast<float*>(raw);
        f[0x110 / 4] = 0.25f;  // direction.x / attenuation0
        f[0x114 / 4] = 0.5f;
        f[0x118 / 4] = 0.75f;
        if (type == 3) {
            f[0x120 / 4] = 0.0f;
            f[0x124 / 4] = -1.0f;
            f[0x128 / 4] = 0.0f;
            f[0x12C / 4] = 30.0f;  // degrees
            f[0x130 / 4] = 2.0f;
        }
        return l;
    }
    NiDynamicEffectState* NewEffectState(std::initializer_list<const void*> lights) {
        auto* es = heap.New<NiDynamicEffectState>();
        GuestPtr next = 0;
        // Build the list back to front so the order equals the argument order.
        const void* const* begin = lights.begin();
        for (std::size_t i = lights.size(); i-- > 0;) {
            auto* node = heap.New<NiListNode>();
            node->next = next;
            node->item = A(begin[i]);
            next = A(node);
        }
        es->lights = next;
        return es;
    }
};

}  // namespace

// ---- camera
// ------------------------------------------------------------------------------------------------

TEST_CASE("extract: SetCameraData view/projection/viewport (0x1469E0)") {
    const float loc[3] = {1.0f, 2.0f, 3.0f};
    const float dir[3] = {0.0f, 0.0f, 1.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float right[3] = {1.0f, 0.0f, 0.0f};
    const NiFrustum fr{-0.5f, 0.5f, 0.375f, -0.375f, 1.0f, 1001.0f};
    const NiRectF port{0.25f, 0.75f, 0.9f, 0.1f};
    CameraInput in;
    in.location = loc;
    in.direction = dir;
    in.up = up;
    in.right = right;
    in.frustum = &fr;
    in.port = &port;
    in.targetWidth = 640;
    in.targetHeight = 480;
    const CameraDesc c = ExtractCamera(in);
    // view: columns right/up/dir, translation -(loc . axis)
    CHECK(c.view.m[0][0] == 1.0f);
    CHECK(c.view.m[1][1] == 1.0f);
    CHECK(c.view.m[2][2] == 1.0f);
    CHECK(c.view.m[3][0] == -1.0f);
    CHECK(c.view.m[3][1] == -2.0f);
    CHECK(c.view.m[3][2] == -3.0f);
    CHECK(c.view.m[3][3] == 1.0f);
    // a world point on the right frustum edge at distance 10 maps to x_ndc = +1
    const Float4 p =
        TransformFloat4(TransformFloat4(Float4{1.0f + 5.0f, 2.0f, 13.0f, 1.0f}, c.view), c.projection);
    CHECK(Near(p.x / p.w, 1.0f));
    CHECK(Near(p.y / p.w, 0.0f));
    CHECK(Near(c.projection.m[0][0], 2.0f));
    CHECK(Near(c.projection.m[1][1], 2.0f / 0.75f));
    CHECK(Near(c.projection.m[2][2], 1001.0f / 1000.0f));
    CHECK(Near(c.projection.m[3][2], -1001.0f / 1000.0f));
    CHECK(c.projection.m[2][3] == 1.0f);
    CHECK(c.projection.m[3][3] == 0.0f);
    // depth: near -> 0, far -> 1
    const Float4 n = TransformFloat4(Float4{0, 0, 1.0f, 1}, c.projection);
    const Float4 f = TransformFloat4(Float4{0, 0, 1001.0f, 1}, c.projection);
    CHECK(Near(n.z / n.w, 0.0f));
    CHECK(Near(f.z / f.w, 1.0f));
    // viewport: X = trunc(640*0.25) = 160, Y = trunc((1 - 0.9f) * 480) = trunc(48.00001) = 48,
    // W = 320, H = trunc((0.9f - 0.1f) * 480) = trunc(383.99998) = 383 - the original's _ftol truncation
    CHECK(c.viewport.x == 160.0f);
    CHECK(c.viewport.y == 48.0f);
    CHECK(c.viewport.width == 320.0f);
    CHECK(c.viewport.height == 383.0f);
    CHECK(c.viewport.maxDepth == 1.0f);
    CHECK(c.nearPlane == 1.0f);
    CHECK(c.depthRange == 1000.0f);
    // screen projection +0x394 shares P22/P32
    CHECK(c.screenProjection.m[0][0] == 2.0f);
    CHECK(c.screenProjection.m[2][0] == -1.0f);
    CHECK(c.screenProjection.m[2][1] == -1.0f);
    CHECK(c.screenProjection.m[2][2] == c.projection.m[2][2]);
    CHECK(c.screenProjection.m[3][2] == c.projection.m[3][2]);
    CHECK(c.screenProjection.m[2][3] == 1.0f);

    in.leftRightSwap = true;
    const CameraDesc s = ExtractCamera(in);
    CHECK(s.projection.m[0][0] == -c.projection.m[0][0]);
    CHECK(s.projection.m[2][0] == -c.projection.m[2][0]);
}

TEST_CASE("extract: asymmetric frustum offsets and _ftol truncation") {
    const float zero[3] = {0, 0, 0};
    const float dir[3] = {1, 0, 0}, up[3] = {0, 1, 0}, right[3] = {0, 0, 1};
    const NiFrustum fr{-0.25f, 0.75f, 0.5f, -0.5f, 0.1f, 10.0f};
    const NiRectF port{0.333f, 0.999f, 0.777f, 0.0f};
    CameraInput in;
    in.location = zero;
    in.direction = dir;
    in.up = up;
    in.right = right;
    in.frustum = &fr;
    in.port = &port;
    const CameraDesc c = ExtractCamera(in);
    CHECK(Near(c.projection.m[2][0], -(0.75f - 0.25f) / 1.0f));
    CHECK(c.viewport.x == 213.0f);  // trunc(213.12)
    CHECK(c.viewport.y == static_cast<float>(static_cast<int>((1.0 - 0.777f) * 480)));
    CHECK(c.viewport.width == static_cast<float>(static_cast<int>((0.999 - 0.333) * 640)));
    // NetImmerse axes: world +x = view +z (direction), world +z = view +x (right)
    const Float3 v = TransformPoint(Float3{2.0f, 0.0f, 1.0f}, c.view);
    CHECK(v.x == 1.0f);
    CHECK(v.z == 2.0f);
}

TEST_CASE("extract: default camera of NiXBoxRenderer::Create") {
    const CameraDesc c = DefaultCamera(640, 480);
    CHECK(c.viewport.width == 640.0f);
    CHECK(c.viewport.height == 480.0f);
    CHECK(c.viewport.x == 0.0f);
    CHECK(Near(c.nearPlane, 0.1f));
    CHECK(Near(c.depthRange, 0.9f));
    CHECK(c.view.m[0][2] == 1.0f);  // dir = (1,0,0) -> view z
    CHECK(c.view.m[2][0] == 1.0f);  // right = (0,0,1) -> view x
}

TEST_CASE("extract: ClearBuffer rectangle (0x1464C2)") {
    rhi::Rect r;
    CHECK_FALSE(ClearRect(nullptr, 640, 480, &r));
    CHECK(r.right == 640);
    CHECK(r.bottom == 480);
    const NiRectF rect{0.25f, 0.5f, 0.75f, 0.25f};
    CHECK(ClearRect(&rect, 640, 480, &r));
    CHECK(r.left == 160);
    CHECK(r.top == 120);
    CHECK(r.right == 320);
    CHECK(r.bottom == 360);
    const NiRectF outside{-0.5f, 1.5f, 1.5f, -0.5f};
    CHECK(ClearRect(&outside, 640, 480, &r));
    CHECK(r.left == 0);
    CHECK(r.top == 0);
    CHECK(r.right == 640);
    CHECK(r.bottom == 480);
}

// ---- D3D state model
// ----------------------------------------------------------------------------------------

TEST_CASE("extract: Xbox texture ops map to the PC numbering") {
    CHECK(TextureOpFromXbox(1) == FF_TOP_DISABLE);
    CHECK(TextureOpFromXbox(4) == FF_TOP_MODULATE);
    CHECK(TextureOpFromXbox(12) == FF_TOP_BLENDDIFFUSEALPHA);
    CHECK(TextureOpFromXbox(13) == FF_TOP_BLENDCURRENTALPHA);
    CHECK(TextureOpFromXbox(14) == FF_TOP_BLENDTEXTUREALPHA);
    CHECK(TextureOpFromXbox(15) == FF_TOP_BLENDFACTORALPHA);
    CHECK(TextureOpFromXbox(22) == FF_TOP_DOTPRODUCT3);
    CHECK(TextureOpFromXbox(26) == FF_TOP_BUMPENVMAPLUMINANCE);
    CHECK(TextureOpFromXbox(0) == FF_TOP_DISABLE);
    CHECK(TextureOpFromXbox(99) == FF_TOP_DISABLE);
}

TEST_CASE("extract: state shadow after NiXBoxRenderer::Create") {
    StateShadow s;
    auto dev = [&](std::uint32_t state) { return s.devRs[state - StateShadow::kRsFirst]; };
    auto ren = [&](std::uint32_t state) { return s.renRs[state - StateShadow::kRsFirst]; };
    CHECK(dev(57) == 0x207);  // ZFUNC ALWAYS (FUN_00159410)
    CHECK(dev(64) == 0);      // ZWRITEENABLE
    CHECK(dev(62) == 0x302);  // SRCBLEND
    CHECK(dev(63) == 0x303);  // DESTBLEND
    CHECK(dev(93) == 3);      // FOGTABLEMODE LINEAR (z fog in the caps)
    CHECK(dev(102) == 0);     // LIGHTING off (light manager ctor)
    CHECK(dev(105) == 1);     // COLORVERTEX
    CHECK(dev(104) == 1);     // LOCALVIEWER: device default
    CHECK(dev(110) == 2);     // SPECULARMATERIALSOURCE COLOR2
    CHECK(dev(111) == 1);     // DIFFUSEMATERIALSOURCE stays COLOR1 ...
    CHECK(ren(111) == 0);     // ... although REN's cache says MATERIAL
    CHECK(dev(148) == 0xFFFFFFFFu);
    CHECK(s.devTss[0][12] == 2);  // stage 0 COLOROP SELECTARG1 (FUN_0015AB40)
    CHECK(s.devTss[0][15] == 1);  // stage 0 COLORARG2 stays CURRENT (cache 0 = desired 0)
    CHECK(s.devTss[1][12] == 1);  // stage 1 disabled by the init stage objects
    CHECK(s.devTss[0][4] == 1);   // MINFILTER POINT
}

TEST_CASE("extract: default properties produce the expected D3D8 state") {
    Scene sc;
    sc.SetBaseMap(true);
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK_FALSE(st.alphaBlendEnable);  // 0xEC: bit 0 clear
    CHECK_FALSE(st.alphaTestEnable);
    CHECK(st.zFunc == rhi::CompareFunc::LessEqual);
    CHECK(st.zWriteEnable);
    CHECK(st.cullMode == rhi::CullMode::CW);
    CHECK(st.fillMode == rhi::FillMode::Solid);
    CHECK(st.shadeMode == ShadeMode::Gouraud);
    CHECK_FALSE(st.fogEnable);
    CHECK(st.lightingEnable);
    CHECK(st.localViewer);
    CHECK(st.texturePath == TexturePath::SingleStage);
    // material (FUN_00161800): alpha of every color = property alpha
    CHECK(st.material.diffuse.y == 0.75f);
    CHECK(st.material.diffuse.w == 0.5f);
    CHECK(st.material.ambient.x == 0.5f);
    CHECK(st.material.emissive.y == 0.25f);
    CHECK(st.material.specular.z == 0.3f);
    CHECK(st.material.power == 8.0f);
    // stage 0: MODULATE(TEXTURE, CURRENT=diffuse) color and alpha, bilinear, wrap, stage 1 disabled
    CHECK(st.stageCount == 1);
    CHECK(st.stages[0].niTexture == A(sc.texture));
    CHECK(st.stages[0].colorOp == FF_TOP_MODULATE);
    CHECK(st.stages[0].colorArg1 == FF_TA_TEXTURE);
    CHECK(st.stages[0].colorArg2 == FF_TA_CURRENT);
    CHECK(st.stages[0].alphaOp == FF_TOP_MODULATE);
    CHECK(st.stages[0].alphaArg1 == FF_TA_TEXTURE);
    CHECK(st.stages[0].minFilter == rhi::Filter::Linear);
    CHECK(st.stages[0].magFilter == rhi::Filter::Linear);
    CHECK(st.stages[0].mipFilter == rhi::MipFilter::None);
    CHECK(st.stages[0].addressU == rhi::AddressMode::Wrap);
    CHECK(st.stages[0].texCoordIndex == 0);
    CHECK(st.stages[1].colorOp == FF_TOP_DISABLE);
    CHECK(st.stages[1].niTexture == 0);
    // E_A_D + IGNORE as the first draw: REN writes nothing, the device keeps DIFFUSE = COLOR1
    CHECK(st.diffuseSource == FF_MCS_COLOR1);
    CHECK(st.ambientSource == FF_MCS_MATERIAL);
    CHECK(st.emissiveSource == FF_MCS_MATERIAL);
    CHECK(st.specularSource == FF_MCS_COLOR2);
}

TEST_CASE("extract: alpha, z-buffer, stencil/cull, wireframe, shade, dither, specular") {
    Scene sc;
    sc.alpha->base.flags = static_cast<std::uint16_t>(1 | (6 << 1) | (7 << 5) | (1 << 9) | (4 << 10));
    sc.alpha->testRef = 0x80;
    sc.zbuffer->base.flags = 2;  // no test, write
    sc.wireframe->flags = 1;
    sc.shade->flags = 0;
    sc.dither->flags = 1;
    sc.specular->flags = 1;
    sc.stencil->enable = 1;
    sc.stencil->func = 3;
    sc.stencil->ref = 5;
    sc.stencil->mask = 0x0F;
    sc.stencil->failAction = 2;
    sc.stencil->zFailAction = 5;
    sc.stencil->passAction = 3;
    sc.stencil->drawMode = 2;  // CW -> D3DCULL_CCW
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.alphaBlendEnable);
    CHECK(st.srcBlend == rhi::BlendFactor::SrcAlpha);
    CHECK(st.dstBlend == rhi::BlendFactor::InvSrcAlpha);
    CHECK(st.alphaTestEnable);
    CHECK(st.alphaFunc == rhi::CompareFunc::Greater);
    CHECK(st.alphaRef == 0x80);
    CHECK(st.zFunc == rhi::CompareFunc::Always);
    CHECK(st.zWriteEnable);
    CHECK(st.fillMode == rhi::FillMode::Wireframe);
    CHECK(st.shadeMode == ShadeMode::Gouraud);  // flat bit 0 == initial byte cache 0: REN writes nothing
    CHECK(st.ditherEnable);
    CHECK(st.specularEnable);
    CHECK(st.stencilEnable);
    CHECK(st.stencilFunc == rhi::CompareFunc::LessEqual);
    CHECK(st.stencilRef == 5);
    CHECK(st.stencilMask == 0x0F);
    CHECK(st.stencilFail == rhi::StencilOp::Replace);
    CHECK(st.stencilZFail == rhi::StencilOp::Invert);
    CHECK(st.stencilPass == rhi::StencilOp::IncrSat);
    CHECK(st.cullMode == rhi::CullMode::CCW);

    // shade: after a smooth draw a flat property switches to FLAT
    sc.shade->flags = 1;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    sc.shade->flags = 0;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.shadeMode == ShadeMode::Flat);

    // left-right swap exchanges CW/CCW; draw mode 3 = no culling
    ExtractContext c = sc.Context();
    c.leftRightSwap = true;
    StateInput in;
    in.propertyState = A(sc.state);
    LightSetDesc lights;
    REQUIRE(ExtractState(c, in, &st, &lights, nullptr) == ExtractStatus::Ok);
    CHECK(st.cullMode == rhi::CullMode::CW);
    sc.stencil->drawMode = 3;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.cullMode == rhi::CullMode::None);
}

TEST_CASE("extract: alpha property is cached by pointer until BeginScene (FUN_00158B90)") {
    Scene sc;
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK_FALSE(st.alphaBlendEnable);
    sc.alpha->base.flags = 0xED;  // same object, new flags: the original keeps the old D3D state
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK_FALSE(st.alphaBlendEnable);
    BeginScene(&sc.shadow);
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.alphaBlendEnable);
}

TEST_CASE("extract: fog (FUN_00159700 / FUN_00159900)") {
    Scene sc;
    const NiFrustum fr{-1, 1, 1, -1, 1.0f, 101.0f};
    const NiRectF port{0, 1, 1, 0};
    const float zero[3] = {0, 0, 0}, dir[3] = {1, 0, 0}, up[3] = {0, 1, 0}, right[3] = {0, 0, 1};
    CameraInput in;
    in.location = zero;
    in.direction = dir;
    in.up = up;
    in.right = right;
    in.frustum = &fr;
    in.port = &port;
    sc.camera = ExtractCamera(in);
    sc.fog->base.flags = 2;  // enabled, Z_LINEAR
    sc.fog->depth = 0.5f;
    sc.fog->color = {1.0f, 0.5f, 1.5f};
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.fogEnable);
    CHECK(st.fogTableMode == FF_FOG_LINEAR);
    CHECK_FALSE(st.rangeFogEnable);
    CHECK(Near(st.fogStart, 101.0f - 50.0f));
    CHECK(Near(st.fogEnd, 101.0f));
    CHECK(Near(st.fogDensity, 1.0f / 50.0f));
    CHECK(Near(st.fogColor.x, 1.0f));
    CHECK(Near(st.fogColor.y, 127.0f / 255.0f));        // trunc(0.5 * 255) = 127
    CHECK(Near(st.fogColor.z, (382 & 0xFF) / 255.0f));  // trunc(1.5 * 255) & 0xFF wraps
    // RANGE_SQ -> EXP + range fog; a new property object is evaluated again
    auto* fog2 = sc.heap.New<NiFogProperty>();
    fog2->base.flags = 2 | (1 << 3);
    fog2->depth = 0.5f;
    sc.state->fog = A(fog2);
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.fogTableMode == FF_FOG_EXP);
    CHECK(st.rangeFogEnable);
    // VERTEX_ALPHA keeps the previous table mode
    auto* fog3 = sc.heap.New<NiFogProperty>();
    fog3->base.flags = 2 | (2 << 3);
    fog3->depth = 0.5f;
    sc.state->fog = A(fog3);
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.fogEnable);
    CHECK(st.fogTableMode == FF_FOG_EXP);
}

TEST_CASE("extract: texture apply modes and the lighting switch (FUN_0015BF80, pipeline vf2)") {
    Scene sc;
    sc.SetBaseMap(true);
    RenderStateDesc st;
    sc.texturing->applyMode = kApplyReplace;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK_FALSE(st.lightingEnable);
    CHECK(st.stages[0].colorOp == FF_TOP_SELECTARG1);
    CHECK(st.stages[0].colorArg1 == FF_TA_TEXTURE);
    CHECK(st.stages[0].alphaOp == FF_TOP_SELECTARG1);

    sc.texturing->applyMode = kApplyDecal;  // treated like MODULATE for the base map (+0x90 = mode != 0)
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.lightingEnable);
    CHECK(st.stages[0].colorOp == FF_TOP_MODULATE);

    sc.SetBaseMap(false);  // no texture: SELECTARG2 (diffuse)
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.stages[0].colorOp == FF_TOP_SELECTARG2);
    CHECK(st.stages[0].alphaOp == FF_TOP_SELECTARG2);
    CHECK(st.stages[0].niTexture == 0);
    CHECK(st.stageCount == 1);

    sc.SetBaseMap(true);  // no UV sets -> no stage for the map, default stage instead
    REQUIRE(sc.State(&st, nullptr, 0, 1.0f, 0) == ExtractStatus::Ok);
    CHECK(st.stages[0].colorOp == FF_TOP_SELECTARG2);
    CHECK(st.stages[0].niTexture == 0);

    sc.texturing->hasNonBaseMaps = 1;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.texturePath == TexturePath::MultiMap);
    CHECK(st.stages[0].colorOp == FF_TOP_MODULATE);
    CHECK(sc.shadow.counters.multiMapApproximated == 1);
}

TEST_CASE("extract: map clamp, filter, UV set, mipmaps (FUN_00161D60 / FUN_00161E40 / FUN_00163D60)") {
    Scene sc;
    sc.SetBaseMap(true);
    RenderStateDesc st;
    sc.baseMap->clamp = 1;   // CLAMP_S_WRAP_T
    sc.baseMap->filter = 2;  // TRILERP without mipmaps -> BILERP
    sc.baseMap->uvSet = 3;
    REQUIRE(sc.State(&st, nullptr, 0, 1.0f, 2) == ExtractStatus::Ok);
    CHECK(st.stages[0].addressU == rhi::AddressMode::Clamp);
    CHECK(st.stages[0].addressV == rhi::AddressMode::Wrap);
    CHECK(st.stages[0].mipFilter == rhi::MipFilter::None);
    CHECK(st.stages[0].minFilter == rhi::Filter::Linear);
    CHECK(st.stages[0].texCoordIndex == 1);  // min(uvSet, count - 1)
    sc.textureData[kTextureDataMipLevels / 4] = 4;
    REQUIRE(sc.State(&st, nullptr, 0, 1.0f, 2) == ExtractStatus::Ok);
    CHECK(st.stages[0].mipFilter == rhi::MipFilter::Linear);
    sc.baseMap->filter = 0;  // NEAREST
    REQUIRE(sc.State(&st, nullptr, 0, 1.0f, 2) == ExtractStatus::Ok);
    CHECK(st.stages[0].minFilter == rhi::Filter::Point);
    CHECK(st.stages[0].magFilter == rhi::Filter::Point);
}

namespace {
rhi::TextureHandle FailResolver(void*, GuestPtr) noexcept {
    return {};
}
rhi::TextureHandle OkResolver(void* user, GuestPtr) noexcept {
    return rhi::TextureHandle{*static_cast<std::uint32_t*>(user)};
}
}  // namespace

TEST_CASE("extract: unresolved textures keep the previous stage states (texture manager vf2 fails)") {
    Scene sc;
    sc.SetBaseMap(true);
    std::uint32_t handleId = 7;
    ExtractContext c = sc.Context();
    c.resolveTexture = OkResolver;
    c.resolveUser = &handleId;
    StateInput in;
    in.propertyState = A(sc.state);
    in.uvSetCount = 1;
    RenderStateDesc st;
    LightSetDesc lights;
    REQUIRE(ExtractState(c, in, &st, &lights, nullptr) == ExtractStatus::Ok);
    CHECK(st.stages[0].texture.id == 7);
    CHECK(st.stages[0].minFilter == rhi::Filter::Linear);
    sc.baseMap->filter = 0;
    c.resolveTexture = FailResolver;
    REQUIRE(ExtractState(c, in, &st, &lights, nullptr) == ExtractStatus::Ok);
    CHECK_FALSE(st.stages[0].texture);
    CHECK(st.stages[0].niTexture == 0);
    CHECK(st.stages[0].minFilter == rhi::Filter::Linear);  // not updated
    CHECK(sc.shadow.counters.unresolvedTextures == 1);
}

TEST_CASE("extract: vertex color sources and their persistence (FUN_0015BF80)") {
    Scene sc;
    RenderStateDesc st;
    sc.vertexColor->lighting = 1;
    sc.vertexColor->source = 2;  // AMB_DIFF
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.lightingEnable);
    CHECK(st.diffuseSource == FF_MCS_COLOR1);
    CHECK(st.ambientSource == FF_MCS_COLOR1);
    CHECK(st.emissiveSource == FF_MCS_MATERIAL);
    sc.vertexColor->lighting = 0;  // E + AMB_DIFF: lighting on without lights, sources untouched
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.lightingEnable);
    CHECK(st.diffuseSource == FF_MCS_COLOR1);
    sc.vertexColor->lighting = 1;
    sc.vertexColor->source = 0;  // IGNORE now writes MATERIAL (cache was COLOR1)
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.diffuseSource == FF_MCS_MATERIAL);
    CHECK(st.ambientSource == FF_MCS_MATERIAL);
    sc.vertexColor->source = 1;  // EMISSIVE
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.emissiveSource == FF_MCS_COLOR1);
    sc.vertexColor->lighting = 0;  // E + EMISSIVE: lighting off
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK_FALSE(st.lightingEnable);
    CHECK(st.emissiveSource == FF_MCS_COLOR1);  // off path does not touch the sources
    sc.vertexColor->source = 0;                 // E + IGNORE: lighting on, emissive source reset (mode was 1)
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.lightingEnable);
    CHECK(st.emissiveSource == FF_MCS_MATERIAL);
}

TEST_CASE("extract: lights (FUN_0015BA40 / FUN_0015B6A0)") {
    Scene sc;
    NiLightLayout* amb = sc.NewLight(0, 0, 2.0f);
    amb->ambient = {0.25f, 0.5f, 0.75f};  // * 2 -> 0.5, 1.0, 1.5 (wraps)
    NiLightLayout* dirL = sc.NewLight(1, 1, 0.5f);
    NiLightLayout* point = sc.NewLight(2, 2, 1.0f);
    NiLightLayout* spot = sc.NewLight(3, 3, 1.0f);
    NiLightLayout* dim = sc.NewLight(1, 4, 0.005f);  // skipped (dimmer < 0.01)
    NiLightLayout* off = sc.NewLight(1, 5, 1.0f);
    off->on = 0;
    NiLightLayout* dup = sc.NewLight(2, 2, 0.5f);  // same index as `point`: replaces it
    NiDynamicEffectState* es = sc.NewEffectState({amb, dirL, point, spot, dim, off, dup});
    RenderStateDesc st;
    LightSetDesc lights;
    REQUIRE(sc.State(&st, &lights, A(es)) == ExtractStatus::Ok);
    CHECK(st.lightingEnable);
    REQUIRE(lights.count == 3);
    CHECK(Near(lights.ambient.x, 127.0f / 255.0f));
    CHECK(Near(lights.ambient.y, 1.0f));
    CHECK(Near(lights.ambient.z, (382 & 0xFF) / 255.0f));
    const LightDesc& d = lights.lights[0];
    CHECK(d.type == FF_LIGHT_DIRECTIONAL);
    CHECK(d.index == 1);
    CHECK(d.direction.x == 0.25f);
    CHECK(d.diffuse.x == 0.5f);  // 1.0 * dimmer 0.5
    CHECK(d.diffuse.w == 1.0f);
    CHECK(d.specular.x == 0.25f);
    CHECK(d.ambient.y == 0.2f);
    const LightDesc& p = lights.lights[1];
    CHECK(p.type == FF_LIGHT_POINT);
    CHECK(p.niLight == A(dup));
    CHECK(p.position.x == 1.0f);
    CHECK(p.position.z == 3.0f);
    CHECK(p.attenuation0 == 0.25f);
    CHECK(p.attenuation2 == 0.75f);
    CHECK(p.range == std::sqrt(std::numeric_limits<float>::max()));
    const LightDesc& s = lights.lights[2];
    CHECK(s.type == FF_LIGHT_SPOT);
    CHECK(s.direction.y == -1.0f);
    CHECK(Near(s.theta, 30.0f * 3.14159265f / 90.0f, 1e-5f));
    CHECK(s.phi == s.theta);
    CHECK(s.falloff == 2.0f);

    // Same effect state again: no re-evaluation (the D3D lights stay) even if a light changed.
    dirL->dimmer = 1.0f;
    const auto evaluations = sc.shadow.counters.lightEvaluations;
    REQUIRE(sc.State(&st, &lights, A(es)) == ExtractStatus::Ok);
    CHECK(sc.shadow.counters.lightEvaluations == evaluations);
    CHECK(lights.lights[0].diffuse.x == 0.5f);
    BeginScene(&sc.shadow);
    REQUIRE(sc.State(&st, &lights, A(es)) == ExtractStatus::Ok);
    CHECK(lights.lights[0].diffuse.x == 1.0f);

    // LIGHTING_E: lights disabled, ambient 0
    sc.vertexColor->lighting = 0;
    REQUIRE(sc.State(&st, &lights, A(es)) == ExtractStatus::Ok);
    CHECK(st.lightingEnable);
    CHECK(lights.count == 0);
    CHECK(lights.ambient.x == 0.0f);
}

TEST_CASE("extract: normalize normals by world scale (FUN_00159200)") {
    Scene sc;
    RenderStateDesc st;
    REQUIRE(sc.State(&st, nullptr, 0, 1.0f) == ExtractStatus::Ok);
    CHECK_FALSE(st.normalizeNormals);
    REQUIRE(sc.State(&st, nullptr, 0, 1.02f) == ExtractStatus::Ok);
    CHECK(st.normalizeNormals);
    REQUIRE(sc.State(&st, nullptr, 0, 1.01f) == ExtractStatus::Ok);
    CHECK_FALSE(st.normalizeNormals);
    REQUIRE(sc.State(&st, nullptr, 0, 0.98f) == ExtractStatus::Ok);
    CHECK(st.normalizeNormals);
    REQUIRE(sc.State(&st, nullptr, 0, std::numeric_limits<float>::quiet_NaN()) == ExtractStatus::Ok);
    CHECK(st.normalizeNormals);
}

TEST_CASE("extract: missing property pointers are an error") {
    Scene sc;
    sc.state->dither = 0;
    RenderStateDesc st;
    CHECK(sc.State(&st) == ExtractStatus::Error);
}

// ---- geometry
// -------------------------------------------------------------------------------------------------

namespace {

struct ShapeObjects {
    NiTriShapeDynamicData* data;  // NiTriShapeData + active counts (vtable picks which counts are used)
    float* positions;
    float* normals;
    float* colors;
    float* uvs;
    std::uint16_t* indices;
    NiTransform* transform;
};

ShapeObjects MakeShape(Scene& sc, bool dynamic) {
    ShapeObjects o;
    o.data = sc.heap.New<NiTriShapeDynamicData>();
    NiGeometryData& g = o.data->base.base.base;
    g.obj.vtable = A(sc.geometryVtables[dynamic ? 1 : 0]);
    g.vertexCount = 4;
    g.uvSetCount = 2;
    o.positions = sc.heap.New<float>(12);
    o.normals = sc.heap.New<float>(12);
    o.colors = sc.heap.New<float>(16);
    o.uvs = sc.heap.New<float>(16);
    for (int i = 0; i < 12; ++i) {
        o.positions[i] = static_cast<float>(i);
    }
    g.vertices = A(o.positions);
    g.normals = A(o.normals);
    g.colors = A(o.colors);
    g.uvs = A(o.uvs);
    g.id = 9;
    g.revision = 3;
    o.data->base.base.triangleCount = 2;
    o.indices = sc.heap.New<std::uint16_t>(6);
    const std::uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
    std::memcpy(o.indices, idx, sizeof(idx));
    o.data->base.triListLength = 6;
    o.data->base.triList = A(o.indices);
    o.data->activeVertices = 3;
    o.data->activeTriangles = 1;
    o.transform = sc.heap.New<NiTransform>();
    const float rot[3][4] = {{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}};
    std::memcpy(o.transform->rotate, rot, sizeof(rot));
    o.transform->translate[0] = 10.0f;
    o.transform->scale = 2.0f;
    return o;
}

}  // namespace

TEST_CASE("extract: RenderShape (slot 48) streams, indices, transform") {
    Scene sc;
    ShapeObjects o = MakeShape(sc, false);
    GeometryInput in;
    in.data = A(o.data);
    in.transform = A(o.transform);
    in.propertyState = A(sc.state);
    DrawItem item;
    REQUIRE(ExtractShape(sc.Context(), in, &item) == ExtractStatus::Ok);
    const GeometryDesc& g = item.geometry;
    CHECK(item.kind == DrawKind::Shape);
    CHECK(g.vertexCount == 4);
    CHECK(g.positions.data == o.positions);  // zero copy
    CHECK(g.positions.stride == 12);
    CHECK(g.normals.data == o.normals);
    CHECK(g.colors.data == o.colors);
    CHECK(g.colors.stride == 16);
    CHECK(g.uvSetCount == 2);
    CHECK(g.uvs[0].data == o.uvs);
    CHECK(g.uvs[1].data == o.uvs + 8);  // set 1 at +vertexCount * 8 bytes
    CHECK(g.indices == o.indices);
    CHECK(g.indexCount == 6);
    CHECK(g.primitiveCount == 2);
    CHECK(g.topology == Topology::TriangleList);
    CHECK(g.dataId == 9);
    CHECK(g.dataRevision == 3);
    CHECK(item.worldScale == 2.0f);
    CHECK(item.world.m[0][1] == 2.0f);  // rotate * scale, no transpose
    CHECK(item.world.m[1][0] == -2.0f);
    CHECK(item.world.m[3][0] == 10.0f);
    CHECK(item.state.normalizeNormals);  // scale 2

    // NiTriShapeDynamicData: active counts (vf12 +0x4C, vf15 +0x4E)
    ShapeObjects d = MakeShape(sc, true);
    in.data = A(d.data);
    REQUIRE(ExtractShape(sc.Context(), in, &item) == ExtractStatus::Ok);
    CHECK(item.geometry.vertexCount == 3);
    CHECK(item.geometry.indexCount == 3);

    // no vertices -> Empty; index out of range -> Error; unknown vtable without guest calls -> Unsupported
    o.data->base.base.base.vertexCount = 0;
    in.data = A(o.data);
    CHECK(ExtractShape(sc.Context(), in, &item) == ExtractStatus::Empty);
    o.data->base.base.base.vertexCount = 4;
    o.indices[5] = 4;
    CHECK(ExtractShape(sc.Context(), in, &item) == ExtractStatus::Error);
    o.indices[5] = 3;
    auto* unknownVtable = sc.heap.New<std::uint32_t>(20);
    o.data->base.base.base.obj.vtable = A(unknownVtable);
    CHECK(ExtractShape(sc.Context(), in, &item) == ExtractStatus::Unsupported);
    o.data->base.base.base.obj.vtable = A(sc.geometryVtables[0]);
    // skinned geometry needs FUN_0014F710 (guest code)
    auto* skin = sc.heap.New<NiSkinInstance>();
    in.skinInstance = A(skin);
    CHECK(ExtractShape(sc.Context(), in, &item) == ExtractStatus::Unsupported);
    CHECK(sc.shadow.counters.skinnedDraws == 1);
}

// G1 (gfx/outline.h): GeometryInput::positionsOverride replaces the position stream (rigid draws) and stays 0 =
// the data's own positions at the defaults.
TEST_CASE("extract: positionsOverride replaces the position stream") {
    Scene sc;
    ShapeObjects o = MakeShape(sc, false);
    GeometryInput in;
    in.data = A(o.data);
    in.transform = A(o.transform);
    in.propertyState = A(sc.state);
    CHECK(in.positionsOverride == 0);
    DrawItem item;
    REQUIRE(ExtractShape(sc.Context(), in, &item) == ExtractStatus::Ok);
    CHECK(item.geometry.positions.data == o.positions);
    auto* scaled = sc.heap.New<float>(12);
    in.positionsOverride = A(scaled);
    REQUIRE(ExtractShape(sc.Context(), in, &item) == ExtractStatus::Ok);
    CHECK(item.geometry.positions.data == scaled);
    CHECK(item.geometry.positions.stride == 12);
    CHECK(item.geometry.normals.data == o.normals);
}

TEST_CASE("extract: RenderTristrips (slot 51) joins strips with 0xFFFF") {
    Scene sc;
    auto* data = sc.heap.New<NiTriStripsData>();
    NiGeometryData& g = data->base.base;
    g.obj.vtable = A(sc.geometryVtables[2]);
    g.vertexCount = 8;
    auto* pos = sc.heap.New<float>(24);
    g.vertices = A(pos);
    data->base.triangleCount = 3;
    data->stripCount = 3;
    auto* lengths = sc.heap.New<std::uint16_t>(3);
    lengths[0] = 4;
    lengths[1] = 2;  // degenerate: no triangle
    lengths[2] = 3;
    data->stripLengths = A(lengths);
    auto* strips = sc.heap.New<std::uint16_t>(9);
    const std::uint16_t s[9] = {0, 1, 2, 3, 4, 5, 5, 6, 7};
    std::memcpy(strips, s, sizeof(s));
    data->strips = A(strips);
    auto* t = sc.heap.New<NiTransform>();
    t->rotate[0][0] = t->rotate[1][1] = t->rotate[2][2] = 1.0f;
    t->scale = 1.0f;
    GeometryInput in;
    in.data = A(data);
    in.transform = A(t);
    in.propertyState = A(sc.state);
    DrawItem item;
    REQUIRE(ExtractStrips(sc.Context(), in, &item) == ExtractStatus::Ok);
    const GeometryDesc& gd = item.geometry;
    CHECK(gd.topology == Topology::TriangleStrip);
    CHECK(gd.primitiveCount == 3);
    const std::uint16_t expected[11] = {0, 1, 2, 3, 0xFFFF, 4, 5, 0xFFFF, 5, 6, 7};
    REQUIRE(gd.indexCount == 11);
    for (int i = 0; i < 11; ++i) {
        CHECK(gd.indices[i] == expected[i]);
    }
    CHECK_FALSE(item.state.normalizeNormals);
    // one strip: zero copy
    data->stripCount = 1;
    REQUIRE(ExtractStrips(sc.Context(), in, &item) == ExtractStatus::Ok);
    CHECK(item.geometry.indices == strips);
    CHECK(item.geometry.indexCount == 4);
}

TEST_CASE("extract: RenderTriangles / RenderTriStrip (slots 49/50) are non-indexed") {
    Scene sc;
    auto* tri = sc.heap.New<NiTrianglesData>();
    tri->base.base.obj.vtable = A(sc.geometryVtables[3]);
    tri->base.base.vertexCount = 9;
    tri->activeTriangles = 2;
    auto* pos = sc.heap.New<float>(27);
    tri->base.base.vertices = A(pos);
    auto* t = sc.heap.New<NiTransform>();
    t->scale = 1.0f;
    GeometryInput in;
    in.data = A(tri);
    in.transform = A(t);
    in.propertyState = A(sc.state);
    DrawItem item;
    REQUIRE(ExtractTriangles(sc.Context(), in, &item) == ExtractStatus::Ok);
    CHECK(item.geometry.indices == nullptr);
    CHECK(item.geometry.vertexCount == 6);
    CHECK(item.geometry.topology == Topology::TriangleList);

    auto* strip = sc.heap.New<NiTriStripData>();
    strip->base.base.obj.vtable = A(sc.geometryVtables[4]);
    strip->base.base.vertexCount = 9;
    strip->activeTriangles = 5;
    strip->base.base.vertices = A(pos);
    in.data = A(strip);
    REQUIRE(ExtractTriStrip(sc.Context(), in, &item) == ExtractStatus::Ok);
    CHECK(item.geometry.vertexCount == 7);
    CHECK(item.geometry.topology == Topology::TriangleStrip);
    strip->activeTriangles = 0;
    CHECK(ExtractTriStrip(sc.Context(), in, &item) == ExtractStatus::Empty);
}

TEST_CASE("extract: RenderScreenPolygon (slot 54, FUN_001585A0)") {
    Scene sc;
    auto* poly = sc.heap.New<NiScreenPolygon>();
    poly->propertyState = A(sc.state);
    poly->vertexCount = 4;
    auto* v = sc.heap.New<float>(12);
    const float verts[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0.5f, 0, 1, 1};
    std::memcpy(v, verts, sizeof(verts));
    poly->vertices = A(v);
    auto* uv = sc.heap.New<float>(8);
    poly->uvs = A(uv);
    DrawItem item;
    REQUIRE(ExtractScreenPolygon(sc.Context(), A(poly), &item) == ExtractStatus::Ok);
    CHECK(item.useScreenCamera);
    CHECK(item.kind == DrawKind::ScreenPolygon);
    const GeometryDesc& g = item.geometry;
    const auto* p = static_cast<const float*>(g.positions.data);
    // default camera: near 0.1, range 0.9 -> d = 0.1 + z * 0.9
    CHECK(Near(p[2], 0.1f));
    CHECK(Near(p[6 + 0], (0.1f + 0.45f) * 1.0f));
    CHECK(Near(p[6 + 2], 0.55f));
    CHECK(Near(p[9 + 1], 1.0f));
    CHECK(Near(p[9 + 2], 1.0f));
    CHECK(g.uvSetCount == 1);
    CHECK(g.uvs[0].data == uv);
    CHECK(g.colors.data == nullptr);
    CHECK(g.topology == Topology::TriangleList);
    const std::uint16_t fan[6] = {0, 1, 2, 0, 2, 3};
    REQUIRE(g.indexCount == 6);
    for (int i = 0; i < 6; ++i) {
        CHECK(g.indices[i] == fan[i]);
    }
    CHECK(g.primitiveCount == 2);
    CHECK(item.lights.count == 0);
    poly->isStrip = 1;
    REQUIRE(ExtractScreenPolygon(sc.Context(), A(poly), &item) == ExtractStatus::Ok);
    CHECK(item.geometry.topology == Topology::TriangleStrip);
    CHECK(item.geometry.indices == nullptr);
    poly->vertexCount = 0;
    CHECK(ExtractScreenPolygon(sc.Context(), A(poly), &item) == ExtractStatus::Empty);
}

TEST_CASE("extract: RenderScreenTexture (slot 55, FUN_0014A050)") {
    Scene sc;
    sc.SetBaseMap(true);
    auto* tex = sc.heap.New<NiScreenTexture>();
    auto* rects = sc.heap.New<NiScreenTextureRect>(2);
    rects[0].left = 10;
    rects[0].top = 20;
    rects[0].width = 32;
    rects[0].height = 16;
    rects[0].texLeft = 16;
    rects[0].texTop = 8;
    rects[0].color = {1.0f, 0.5f, 0.25f, 0.75f};
    rects[1].left = -10;  // wraps to 630
    rects[1].top = 500;   // wraps to 20
    rects[1].width = 8;
    rects[1].height = 8;
    tex->rects = A(rects);
    tex->rectCount = 2;
    tex->texturing = A(sc.texturing);
    DrawItem item;
    REQUIRE(ExtractScreenTexture(sc.Context(), A(tex), &item) == ExtractStatus::Ok);
    const GeometryDesc& g = item.geometry;
    CHECK(g.pretransformed);
    CHECK(g.vertexCount == 8);
    const auto* p = static_cast<const float*>(g.positions.data);
    const auto* uv = static_cast<const float*>(g.uvs[0].data);
    const auto* c = static_cast<const float*>(g.colors.data);
    // corners (L,T) (L,B) (R,T) (R,B), x - 0.5
    CHECK(p[0] == 9.5f);
    CHECK(p[1] == 19.5f);
    CHECK(p[3 + 1] == 35.5f);
    CHECK(p[6 + 0] == 41.5f);
    CHECK(p[9 + 0] == 41.5f);
    CHECK(p[9 + 1] == 35.5f);
    CHECK(p[2] == 0.0f);
    // texture 64x32: u = 16/64 .. 48/64, v = 8/32 .. 24/32
    CHECK(Near(uv[0], 0.25f));
    CHECK(Near(uv[1], 0.25f));
    CHECK(Near(uv[6 + 0], 0.75f));
    CHECK(Near(uv[2 + 1], 0.75f));
    CHECK(c[3] == 0.75f);
    CHECK(p[12 + 0] == 629.5f);
    CHECK(p[12 + 1] == 19.5f);
    const std::uint16_t idx[12] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    REQUIRE(g.indexCount == 12);
    for (int i = 0; i < 12; ++i) {
        CHECK(g.indices[i] == idx[i]);
    }
    // slot 55 state: blend SRCALPHA/INVSRCALPHA, z off, lighting off, MODULATE texture * vertex color
    const RenderStateDesc& st = item.state;
    CHECK(st.alphaBlendEnable);
    CHECK(st.srcBlend == rhi::BlendFactor::SrcAlpha);
    CHECK(st.dstBlend == rhi::BlendFactor::InvSrcAlpha);
    CHECK(st.zFunc == rhi::CompareFunc::Always);
    CHECK_FALSE(st.zWriteEnable);
    CHECK_FALSE(st.lightingEnable);
    CHECK(st.texturePath == TexturePath::Screen);
    CHECK(st.stages[0].colorOp == FF_TOP_MODULATE);
    CHECK(st.stages[0].niTexture == A(sc.texture));
    CHECK(st.stages[0].texCoordIndex == 0);
    // no rects -> Empty; no base map -> Error (the original crashes)
    tex->rectCount = 0;
    CHECK(ExtractScreenTexture(sc.Context(), A(tex), &item) == ExtractStatus::Empty);
    tex->rectCount = 2;
    sc.SetBaseMap(false);
    CHECK(ExtractScreenTexture(sc.Context(), A(tex), &item) == ExtractStatus::Error);
}

TEST_CASE("extract: arena memory is guest-visible") {
    ExtractArena arena;
    void* p = arena.Allocate(64);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) < 0x80000000u);
    const std::uint32_t generation = arena.Generation();
    arena.Reset();
    CHECK(arena.Generation() == generation + 1);
}

TEST_CASE("extract: PurgeLight drops an enabled light like FUN_0015B9E0 (slot 60)") {
    StateShadow s;
    s.lights.count = 3;
    s.lights.lights[0].niLight = 0x1000;
    s.lights.lights[0].index = 1;
    s.lights.lights[1].niLight = 0x2000;
    s.lights.lights[1].index = 2;
    s.lights.lights[2].niLight = 0x3000;
    s.lights.lights[2].index = 3;
    s.lastEffectState = 0x4000;
    CHECK(PurgeLight(&s, 0x2000) == 1);
    CHECK(s.lights.count == 2);
    CHECK(s.lights.lights[0].niLight == 0x1000);
    CHECK(s.lights.lights[1].niLight == 0x3000);
    CHECK(s.lights.lights[1].index == 3);
    CHECK(s.lastEffectState == 0x4000);  // the light manager's last effect state stays (+0x3C)
    CHECK(s.counters.lightsPurged == 1);
    CHECK(PurgeLight(&s, 0x2000) == 0);  // not enabled (any more)
    CHECK(PurgeLight(&s, 0) == 0);
    CHECK(PurgeLight(nullptr, 0x1000) == 0);
    CHECK(s.lights.count == 2);
}
