// Unit tests of the M3 phase-1 contract pieces that are already implemented in the skeleton
// (GFX_CONTRACT.md): gfx_math.h, the backend-neutral RHI helpers (rhi.cpp) and ni::ExtractArena.
// The module agents add their own files (tests/rhi_*, tests/texconv_*, tests/extract_*, tests/ff_*).
#include "doctest.h"

#include "gfx/gfx_math.h"
#include "gfx/ni_extract.h"
#include "gfx/rhi/rhi.h"
#include "gfx/shaders/ff_shared.h"

#include <cstdint>
#include <cstring>

namespace {

bool Near(float a, float b, float eps = 1e-5f) {
    return a - b < eps && b - a < eps;
}

}  // namespace

TEST_CASE("gfx math: NiTransformToWorld matches the CPU transform of NiTriShape::vf33") {
    // rotate rows = images of the axes (16-byte stride), translate, scale (FUN_001489A0 / 0x1166D0).
    const float rotate[12] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0};  // 90 degrees about z
    const float translate[3] = {10, 20, 30};
    const gfx::Matrix4 w = gfx::NiTransformToWorld(rotate, translate, 2.0f);
    const gfx::Float3 v{1, 2, 3};
    // out.x = t.x + s*(m[0]*x + m[4]*y + m[8]*z), out.y = t.y + s*(m[1]*x + m[5]*y + m[9]*z), ...
    const gfx::Float3 expected{10 + 2 * (0 * 1 + -1 * 2 + 0 * 3), 20 + 2 * (1 * 1 + 0 * 2 + 0 * 3),
                               30 + 2 * (0 * 1 + 0 * 2 + 1 * 3)};
    const gfx::Float3 got = gfx::TransformPoint(v, w);
    CHECK(Near(got.x, expected.x));
    CHECK(Near(got.y, expected.y));
    CHECK(Near(got.z, expected.z));
    CHECK(w.m[0][3] == 0.0f);
    CHECK(w.m[3][3] == 1.0f);
}

TEST_CASE("gfx math: multiply and inverse") {
    const float rotate[12] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0};
    const float translate[3] = {-4, 5, 0.5f};
    const gfx::Matrix4 a = gfx::NiTransformToWorld(rotate, translate, 3.0f);
    gfx::Matrix4 inv;
    REQUIRE(gfx::Inverse(a, &inv));
    const gfx::Matrix4 id = gfx::Multiply(a, inv);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            CHECK(Near(id.m[i][j], i == j ? 1.0f : 0.0f, 1e-4f));
        }
    }
    gfx::Matrix4 singular;
    CHECK_FALSE(gfx::Inverse(singular, &inv));
    CHECK(inv.m[0][0] == 1.0f);
}

TEST_CASE("rhi: format table and level sizes") {
    using gfx::rhi::Format;
    CHECK(std::strcmp(gfx::rhi::GetFormatInfo(Format::BGRA8_UNorm).name, "BGRA8_UNorm") == 0);
    CHECK(gfx::rhi::LevelSize(Format::BGRA8_UNorm, 8, 8) == 256);
    CHECK(gfx::rhi::LevelSize(Format::BC1_UNorm, 64, 32) == 16 * 8 * 8);
    CHECK(gfx::rhi::LevelSize(Format::BC3_UNorm, 2, 2) == 16);  // one 4x4 block
    CHECK(gfx::rhi::RowPitch(Format::BC2_UNorm, 16) == 64);
    CHECK(gfx::rhi::GetFormatInfo(Format::Count).bytesPerBlock == 0);
    CHECK(std::strcmp(gfx::rhi::BackendName(gfx::rhi::Backend::D3D11), "d3d11") == 0);
}

TEST_CASE("rhi: equal descriptors hash equally") {
    gfx::rhi::SamplerDesc a, b;
    CHECK(a == b);
    CHECK(gfx::rhi::HashOf(a) == gfx::rhi::HashOf(b));
    b.mipLodBias = -0.0f;  // +0 and -0 compare and hash equal
    CHECK(a == b);
    CHECK(gfx::rhi::HashOf(a) == gfx::rhi::HashOf(b));
    b.addressU = gfx::rhi::AddressMode::Clamp;
    CHECK_FALSE(a == b);

    gfx::rhi::PipelineDesc p, q;
    p.blend.enable = q.blend.enable = true;
    p.layout.attributeCount = q.layout.attributeCount = 1;
    CHECK(p == q);
    CHECK(gfx::rhi::HashOf(p) == gfx::rhi::HashOf(q));
    q.raster.cull = gfx::rhi::CullMode::CW;
    CHECK_FALSE(p == q);
}

TEST_CASE("rhi: the null backend creates no device") {
    std::string error;
    gfx::rhi::DeviceDesc desc;
    desc.backend = gfx::rhi::Backend::Null;
    CHECK(gfx::rhi::CreateDevice(desc, &error) == nullptr);
    CHECK_FALSE(error.empty());
}

TEST_CASE("ni: extract arena") {
    gfx::ni::ExtractArena arena;
    auto* a = static_cast<std::uint8_t*>(arena.Allocate(10));
    auto* b = arena.AllocateArray<float>(4);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(a) % 16 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(b) % 16 == 0);
    CHECK(reinterpret_cast<std::uint8_t*>(b) >= a + 10);
    void* big = arena.Allocate(3u << 20);  // larger than a block
    REQUIRE(big != nullptr);
    std::memset(big, 0x5A, 3u << 20);
    CHECK(arena.BytesUsed() >= (3u << 20) + 26);
    const std::size_t reserved = arena.BytesReserved();
    arena.Reset();
    CHECK(arena.BytesUsed() == 0);
    CHECK(arena.Allocate(100) == a);  // blocks are reused
    CHECK(arena.BytesReserved() == reserved);
    CHECK(arena.Allocate(16, 3) == nullptr);  // not a power of two
}

TEST_CASE("ff: constant buffer layouts are HLSL-compatible") {
    CHECK(sizeof(gfx::ff::FFFrameConstants) % 16 == 0);
    CHECK(sizeof(gfx::ff::FFDrawConstants) % 16 == 0);
    CHECK(sizeof(gfx::ff::FFLightConstants) == FF_MAX_LIGHTS * 112);
    CHECK(sizeof(gfx::ff::FFStageConstants) == FF_MAX_STAGES * 112);
    CHECK(gfx::ni::kMaxTextureStages == FF_MAX_STAGES);
}
