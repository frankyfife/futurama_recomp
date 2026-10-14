// M3e-Rest (docs/m8b_report.md): the default texture pipeline's multi-map/effect path (vf2 0x164360) - which stages and
// passes the dark, detail, glow and decal maps and the texture effects (environment map, projected lights and shadows,
// fog map) get, with REN's stage-op tables (FUN_0015A120) and pass alpha properties. Synthetic guest objects in guest
// pages as in extract_tests.cpp; no guest code runs.
#include "doctest.h"

#include "gfx/guest_memory.h"
#include "gfx/ni_extract.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

using namespace gfx;
using namespace gfx::ni;

bool Near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

class Heap {
public:
    Heap() {
        base_ = static_cast<std::uint8_t*>(AllocGuestPages(kSize));
        REQUIRE(base_ != nullptr);
    }
    ~Heap() { FreeGuestPages(base_); }
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;
    template <class T>
    T* New(std::size_t bytes = sizeof(T)) {
        used_ = (used_ + 15) & ~std::size_t{15};
        REQUIRE(used_ + bytes <= kSize);
        auto* p = reinterpret_cast<T*>(base_ + used_);
        std::memset(static_cast<void*>(p), 0, bytes);
        used_ += bytes;
        return p;
    }

private:
    static constexpr std::size_t kSize = 1u << 20;
    std::uint8_t* base_ = nullptr;
    std::size_t used_ = 0;
};

GuestPtr A(const void* p) {
    return static_cast<GuestPtr>(reinterpret_cast<std::uintptr_t>(p));
}

// NiTextureEffect fields (FUN_00163F10, FUN_00164200).
constexpr std::uint32_t kOn = 0xC0, kProjection = 0x120, kTranslation = 0x150, kTexture = 0x15C, kFilter = 0x160,
                        kClamp = 0x164, kType = 0x168, kCoordMode = 0x16C, kClipped = 0x170, kPlane = 0x184;

struct Scene {
    Heap heap;
    NiPropertyState* state;
    NiAlphaProperty* alpha;
    NiTexturingProperty* texturing;
    GuestPtr* maps;
    ExtractArena arena;
    StateShadow shadow;
    CameraDesc camera = DefaultCamera(640, 480);
    GuestPtr textures[8];  // NiSourceTextures (fake vtable value, renderer data with 1 mip level)
    NiDynamicEffectState* effects;

    Scene() {
        state = heap.New<NiPropertyState>();
        alpha = heap.New<NiAlphaProperty>();
        alpha->base.flags = 0xEC;
        auto* zbuffer = heap.New<NiProperty>(0x1C);
        zbuffer->flags = 3;
        auto* material = heap.New<NiMaterialProperty>();
        auto* vertexColor = heap.New<NiVertexColorProperty>();
        vertexColor->lighting = 1;
        texturing = heap.New<NiTexturingProperty>();
        texturing->applyMode = kApplyModulate;
        maps = heap.New<GuestPtr>(7 * sizeof(GuestPtr));
        texturing->maps = A(maps);
        texturing->mapsMaxSize = 7;
        texturing->mapsSize = 7;
        auto* other = heap.New<NiProperty>(0x40);
        state->alpha = A(alpha);
        state->dither = state->fog = state->rendererSpecific = state->shade = state->specular = state->stencil =
            state->wireframe = A(other);
        state->material = A(material);
        state->texturing = A(texturing);
        state->vertexColor = A(vertexColor);
        state->zbuffer = A(zbuffer);
        for (GuestPtr& t : textures) {
            auto* tex = heap.New<NiSourceTextureLayout>();
            tex->obj.vtable = kNiSourceTextureVtable;
            auto* data = heap.New<std::uint32_t>(0x70);
            data[kTextureDataWidth / 4] = 16;
            data[kTextureDataHeight / 4] = 16;
            data[kTextureDataMipLevels / 4] = 1;
            tex->rendererData = A(data);
            t = A(tex);
        }
        effects = heap.New<NiDynamicEffectState>();
        SetMap(kMapBase, 0);
    }
    // Map `slot` with textures[texture], WRAP_S_WRAP_T, BILERP, UV set `uv`.
    NiTexturingMap* SetMap(std::uint32_t slot, int texture, std::uint32_t uv = 0) {
        auto* m = heap.New<NiTexturingMap>();
        m->texture = textures[texture];
        m->clamp = 3;
        m->filter = 1;
        m->uvSet = uv;
        maps[slot] = A(m);
        if (slot != kMapBase) {
            texturing->hasNonBaseMaps = 1;
        }
        return m;
    }
    // An NiTextureEffect of `type` (0 light, 1 shadow, 2 environment, 3 fog) with textures[texture].
    std::uint8_t* NewEffect(std::uint32_t type, std::uint32_t coordMode, int texture, bool clipped = false) {
        auto* e = heap.New<std::uint8_t>(0x1A0);
        e[kOn] = 1;
        auto put = [e](std::uint32_t off, std::uint32_t v) { std::memcpy(e + off, &v, 4); };
        auto putf = [e](std::uint32_t off, float v) { std::memcpy(e + off, &v, 4); };
        put(kTexture, textures[texture]);
        put(kFilter, 0);
        put(kClamp, 3);
        put(kType, type);
        put(kCoordMode, coordMode);
        e[kClipped] = clipped ? 1 : 0;
        // Sphere map projection of the game (Level1-1): rows (0, 0, 0) (0, -0.5, 0) (0.5, 0, 0), T = (0.5, 0.5, 0).
        putf(kProjection + 0x14, -0.5f);
        putf(kProjection + 0x20, 0.5f);
        putf(kTranslation, 0.5f);
        putf(kTranslation + 4, 0.5f);
        putf(kPlane + 4, 1.0f);  // plane y = 0 (normal +y)
        return e;
    }
    GuestPtr List(std::initializer_list<const void*> items) {
        GuestPtr head = 0;
        for (auto it = std::rbegin(items); it != std::rend(items); ++it) {
            auto* n = heap.New<NiListNode>();
            n->item = A(*it);
            n->next = head;
            head = A(n);
        }
        return head;
    }
    ExtractStatus State(RenderStateDesc* st, std::uint32_t uvSets = 2) {
        ExtractContext c;
        c.arena = &arena;
        c.camera = &camera;
        c.shadow = &shadow;
        c.allowGuestCalls = false;
        StateInput in;
        in.propertyState = A(state);
        in.effectState = A(effects);
        in.uvSetCount = uvSets;
        in.hasNormals = true;
        LightSetDesc lights;
        return ExtractState(c, in, st, &lights, nullptr);
    }
};

}  // namespace

TEST_CASE("extract multimap: dark, detail and glow maps join the base pass (REN's later-stage ops)") {
    Scene sc;
    sc.SetMap(kMapDark, 1, 1);
    sc.SetMap(kMapDetail, 2, 1);
    sc.SetMap(kMapGlow, 3, 1);
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.texturePath == TexturePath::MultiMap);
    CHECK(st.passCount == 1);
    REQUIRE(st.stageCount == 4);
    // Order of vf2: base + detail (FUN_001600E0), dark (0x1648F2), glow (0x164F4F).
    CHECK(st.stages[0].niTexture == sc.textures[0]);
    CHECK(st.stages[0].colorOp == FF_TOP_MODULATE);  // first-stage table type 2 (apply mode MODULATE)
    CHECK(st.stages[1].niTexture == sc.textures[2]);
    CHECK(st.stages[1].colorOp == FF_TOP_MODULATE2X);  // detail
    CHECK(st.stages[1].alphaOp == FF_TOP_SELECTARG2);
    CHECK(st.stages[1].colorArg1 == FF_TA_TEXTURE);
    CHECK(st.stages[1].colorArg2 == FF_TA_CURRENT);
    CHECK(st.stages[1].texCoordIndex == 1);
    CHECK(st.stages[2].niTexture == sc.textures[1]);
    CHECK(st.stages[2].colorOp == FF_TOP_MODULATE);  // dark
    CHECK(st.stages[2].alphaOp == FF_TOP_MODULATE);
    CHECK(st.stages[3].niTexture == sc.textures[3]);
    CHECK(st.stages[3].colorOp == FF_TOP_ADD);  // glow
    CHECK(st.stages[3].alphaOp == FF_TOP_SELECTARG2);
    CHECK(sc.shadow.counters.multiMapDraws == 1);
    CHECK(sc.shadow.counters.multiMapApproximated == 0);
    CHECK(sc.shadow.counters.extraPasses == 0);
}

TEST_CASE("extract multimap: without UV sets only the base stage logic runs; maps beyond the stages open passes") {
    Scene sc;
    sc.SetMap(kMapDark, 1, 1);
    RenderStateDesc st;
    REQUIRE(sc.State(&st, 0) == ExtractStatus::Ok);  // no UV sets: no map stages (0x164453), default stage
    CHECK(st.stageCount == 1);
    CHECK(st.stages[0].colorOp == FF_TOP_SELECTARG2);
    // A glow map that finds the base pass full gets a pass of its own: type 0 = ONE + ONE, stage 0 SELECTARG1.
    sc.SetMap(kMapDetail, 2, 1);
    sc.SetMap(kMapGlow, 3, 1);
    auto* shadow = sc.NewEffect(1, 0, 4, true);
    auto* shadow2 = sc.NewEffect(1, 0, 5, false);
    sc.effects->projectedShadows = sc.List({shadow, shadow2});
    BeginScene(&sc.shadow);  // the pipeline keeps the effect stages of an effect state until then
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    // pass 0 base detail dark; pass 1 shadow + clip + plain shadow (filled up); glow: pass 1 starts with type 2 -> new
    // pass 2.
    REQUIRE(st.passCount == 3);
    CHECK(st.stageCount == 3);
    const PassDesc& p1 = st.extraPasses[0];
    CHECK(p1.stageCount == 3);
    CHECK(p1.srcBlend == rhi::BlendFactor::Zero);
    CHECK(p1.dstBlend == rhi::BlendFactor::SrcColor);
    const PassDesc& p2 = st.extraPasses[1];
    CHECK(p2.stageCount == 1);
    CHECK(p2.stages[0].niTexture == sc.textures[3]);
    CHECK(p2.stages[0].colorOp == FF_TOP_SELECTARG1);
    CHECK(p2.alphaBlendEnable);
    CHECK(p2.srcBlend == rhi::BlendFactor::One);
    CHECK(p2.dstBlend == rhi::BlendFactor::One);
}

TEST_CASE("extract multimap: a decal map is a pass of its own blended SRCALPHA / INVSRCALPHA") {
    Scene sc;
    sc.SetMap(kMapDecal, 6, 0);
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    REQUIRE(st.passCount == 2);
    CHECK(st.stageCount == 1);
    const PassDesc& p = st.extraPasses[0];
    CHECK(p.stageCount == 1);
    CHECK(p.stages[0].niTexture == sc.textures[6]);
    CHECK(p.stages[0].colorOp == FF_TOP_MODULATE);  // first-stage table type 1 (+0x90 = apply mode != REPLACE)
    CHECK(p.alphaBlendEnable);
    CHECK(p.srcBlend == rhi::BlendFactor::SrcAlpha);
    CHECK(p.dstBlend == rhi::BlendFactor::InvSrcAlpha);
    CHECK_FALSE(p.alphaTestEnable);
    // The geometry's own alpha property is used again by the next draw (the cache holds the pass property).
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK_FALSE(st.alphaBlendEnable);
}

TEST_CASE("extract multimap: the sphere-mapped environment map (all levels) - stage of the base pass") {
    Scene sc;
    auto* env = sc.NewEffect(2, 2, 4);
    sc.effects->environmentMap = A(env);
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.texturePath == TexturePath::MultiMap);
    CHECK(st.passCount == 1);
    REQUIRE(st.stageCount == 2);
    const TextureStageDesc& e = st.stages[1];
    CHECK(e.niTexture == sc.textures[4]);
    CHECK(e.texCoordIndex == FF_TCI_CAMERASPACENORMAL);
    CHECK(e.transformFlags == FF_TTFF_COUNT2);
    CHECK(e.colorOp == FF_TOP_MODULATE);  // later-stage table type 2
    CHECK(e.alphaOp == FF_TOP_MODULATE);
    CHECK(e.minFilter == rhi::Filter::Point);
    // Default camera: right (0,0,1), up (0,1,0), direction (1,0,0): u = 0.5 * world n.z + 0.5 = 0.5 * view n.x + 0.5.
    CHECK(Near(e.transform.m[0][0], 0.5f));
    CHECK(Near(e.transform.m[1][1], -0.5f));
    CHECK(Near(e.transform.m[3][0], 0.5f));
    CHECK(Near(e.transform.m[3][1], 0.5f));
    // Switched off: no stage (FUN_00163F10), single-stage path.
    env[kOn] = 0;
    sc.shadow.effects.effectState = 0;  // a new effect state for the pipeline (cache, see below)
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.texturePath == TexturePath::SingleStage);
    CHECK(st.stageCount == 1);
}

TEST_CASE("extract multimap: environment map after a clipped shadow pass - own pass ZERO / SRCCOLOR, SELECTARG1") {
    Scene sc;
    auto* env = sc.NewEffect(2, 2, 4);
    auto* shadow = sc.NewEffect(1, 0, 5, true);
    sc.effects->environmentMap = A(env);
    sc.effects->projectedShadows = sc.List({shadow});
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    REQUIRE(st.passCount == 3);
    CHECK(st.stageCount == 1);
    CHECK(st.extraPasses[0].stageCount == 2);  // shadow + clip
    CHECK(st.extraPasses[0].stages[0].niTexture == sc.textures[5]);
    const PassDesc& p = st.extraPasses[1];
    REQUIRE(p.stageCount == 1);
    CHECK(p.stages[0].niTexture == sc.textures[4]);
    CHECK(p.stages[0].colorOp == FF_TOP_SELECTARG1);
    CHECK(p.stages[0].texCoordIndex == FF_TCI_CAMERASPACENORMAL);
    CHECK(p.srcBlend == rhi::BlendFactor::Zero);
    CHECK(p.dstBlend == rhi::BlendFactor::SrcColor);
    CHECK(sc.shadow.counters.shadowPasses == 1);
    CHECK(sc.shadow.counters.extraPasses == 2);
    // Blended geometry gets no shadow pass (0x1649A0): the environment map joins the base pass again.
    sc.alpha->base.flags = 0xED;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.passCount == 1);
    CHECK(st.stageCount == 2);
}

TEST_CASE("extract multimap: projected lights come first, the base pass multiplies (ZERO / SRCCOLOR)") {
    Scene sc;
    auto* light = sc.NewEffect(0, 0, 4, true);
    auto* light2 = sc.NewEffect(0, 1, 5, false);  // WORLD_PERSPECTIVE
    sc.effects->projectedLights = sc.List({light, light2});
    sc.SetMap(kMapDark, 1, 1);
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    REQUIRE(st.passCount == 2);
    // Pass 0 (the geometry's own alpha): clipped light SELECTARG1, clip MODULATE, dark MODULATE, plain light ADD.
    REQUIRE(st.stageCount == 4);
    CHECK(st.stages[0].niTexture == sc.textures[4]);
    CHECK(st.stages[0].colorOp == FF_TOP_SELECTARG1);
    CHECK(st.stages[0].texCoordIndex == FF_TCI_CAMERASPACEPOSITION);
    CHECK(st.stages[1].niTexture == 0);  // clip stage: the clip texture (none in the test context)
    CHECK(st.stages[1].colorOp == FF_TOP_MODULATE);
    CHECK(st.stages[2].niTexture == sc.textures[1]);
    CHECK(st.stages[2].colorOp == FF_TOP_MODULATE);
    CHECK(st.stages[3].niTexture == sc.textures[5]);
    CHECK(st.stages[3].colorOp == FF_TOP_ADD);
    CHECK(st.stages[3].transformFlags == (FF_TTFF_COUNT3 | FF_TTFF_PROJECTED));
    CHECK_FALSE(st.alphaBlendEnable);
    const PassDesc& base = st.extraPasses[0];
    REQUIRE(base.stageCount == 1);
    CHECK(base.stages[0].niTexture == sc.textures[0]);
    CHECK(base.stages[0].colorOp == FF_TOP_MODULATE);
    CHECK(base.srcBlend == rhi::BlendFactor::Zero);
    CHECK(base.dstBlend == rhi::BlendFactor::SrcColor);
}

TEST_CASE("extract multimap: fog map in a pass of its own after the first draw; bump maps are counted") {
    Scene sc;
    auto* fog = sc.NewEffect(3, 0, 4);
    sc.effects->fogMap = A(fog);
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);  // first draw: pipeline +8 = 0, the fog map joins pass 0
    CHECK(st.passCount == 1);
    CHECK(st.stageCount == 2);
    CHECK(st.stages[1].colorOp == FF_TOP_BLENDTEXTUREALPHA);  // later-stage table type 1
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    REQUIRE(st.passCount == 2);
    CHECK(st.extraPasses[0].stages[0].colorOp == FF_TOP_SELECTARG1);
    CHECK(st.extraPasses[0].srcBlend == rhi::BlendFactor::SrcAlpha);
    CHECK(st.extraPasses[0].dstBlend == rhi::BlendFactor::InvSrcAlpha);
    // Bump map with an environment map: BUMPENVMAPLUMINANCE stage + environment stage (approximated).
    sc.effects->fogMap = 0;
    auto* env = sc.NewEffect(2, 2, 5);
    sc.effects->environmentMap = A(env);
    sc.SetMap(kMapBump, 6, 0);
    sc.shadow.effects.effectState = 0;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(st.stageCount == 3);
    CHECK(st.stages[1].colorOp == FF_TOP_BUMPENVMAPLUMINANCE);
    CHECK(st.stages[2].niTexture == sc.textures[5]);
    CHECK(sc.shadow.counters.multiMapApproximated == 1);
}

TEST_CASE("extract multimap: effect stages are kept per effect state until BeginScene (manager +0xB08)") {
    Scene sc;
    auto* env = sc.NewEffect(2, 2, 4);
    sc.effects->environmentMap = A(env);
    RenderStateDesc st;
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    const Matrix4 first = st.stages[1].transform;
    // Another camera: the same effect state keeps the stage built with the first camera (FUN_00164200).
    sc.camera.right = {1, 0, 0};
    sc.camera.direction = {0, 0, 1};
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(std::memcmp(&first, &st.stages[1].transform, sizeof(Matrix4)) == 0);
    BeginScene(&sc.shadow);
    REQUIRE(sc.State(&st) == ExtractStatus::Ok);
    CHECK(Near(st.stages[1].transform.m[2][0], 0.5f));  // direction (0,0,1) -> u = 0.5 * view n.z + 0.5
    CHECK(Near(st.stages[1].transform.m[0][0], 0.0f));
}

TEST_CASE("extract multimap: coordinate transforms of the sphere map and the light clip plane") {
    const Float3 inverseView[4] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {5, 6, 7}};
    const float projection[3][4] = {{0, 0, 0, 0}, {0, -0.5f, 0, 0}, {0.5f, 0, 0, 0}};
    const float translation[3] = {0.5f, 0.5f, 0.0f};
    const Matrix4 m = SphereMapTransform(inverseView, projection, translation);
    CHECK(Near(m.m[0][0], 0.5f));   // right = world z -> P[2][0]
    CHECK(Near(m.m[1][1], -0.5f));  // up = world y -> P[1][1]
    CHECK(Near(m.m[3][0], 0.5f));   // T alone, no camera location
    CHECK(Near(m.m[3][1], 0.5f));
    const Matrix4 shadowM = ProjectedShadowTransform(inverseView, projection, translation);
    CHECK(Near(shadowM.m[3][0], 0.5f * 7 + 0.5f));  // WORLD_PARALLEL adds location . P
    // Light clip plane y = 2 (FUN_00162AD0): u = n . p + 0.5 - d -> lit (>= 0.5) above the plane.
    const Float3 identity[4] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};
    const Matrix4 c = LightClipTransform(identity, {0, 1, 0}, 2.0f);
    const float uAbove = 3.0f * c.m[1][0] + c.m[3][0];
    const float uBelow = 1.0f * c.m[1][0] + c.m[3][0];
    CHECK(uAbove >= 0.5f);
    CHECK(uBelow < 0.5f);
    const Matrix4 s = ShadowClipTransform(identity, {0, 1, 0}, 2.0f);
    CHECK(3.0f * s.m[1][0] + s.m[3][0] < 0.5f);  // shadows: the opposite side (ADD white below)
}
