// Unit tests of the G1 outline options (gfx/outline.h, gfx/gfx_config.h; analysis/loader/g1_outlines.md 7.5):
// option parsers and schema rows, shell <-> body pairing and scaling, shell colour override, ribbon colour copy,
// the QDOutline parent-chain test, and that the defaults change nothing.
#include "doctest.h"

#include "core/config_schema.h"
#include "gfx/gfx_config.h"
#include "gfx/guest_memory.h"
#include "gfx/outline.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace gfx;
using gfx::ni::GuestPtr;

bool Near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

// A strip of two quads: the body has split vertices along the middle edge (8 vertices), the shell is welded (6
// vertices) and moved by 0.05 along +z, except vertex 0 (offset 0, like a NoLines vertex).
struct Meshes {
    std::vector<float> body, shell;
    std::vector<std::uint16_t> bodyTris, shellTris;
};
Meshes MakeMeshes() {
    Meshes m;
    const float grid[6][3] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 1, 0}};
    // body: left quad 0 1 3 4 (vertices 0..3), right quad 1 2 4 5 as own vertices (4..7)
    const int bodyFromGrid[8] = {0, 1, 3, 4, 1, 2, 4, 5};
    for (int v : bodyFromGrid) {
        m.body.insert(m.body.end(), grid[v], grid[v] + 3);
    }
    m.bodyTris = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    for (int v = 0; v < 6; ++v) {
        m.shell.insert(m.shell.end(), grid[v], grid[v] + 3);
        if (v != 0) {
            m.shell.back() += 0.05f;
        }
    }
    m.shellTris = {0, 1, 3, 3, 1, 4, 1, 2, 4, 4, 2, 5};
    return m;
}

class GuestHeap {
public:
    GuestHeap() {
        base_ = static_cast<std::uint8_t*>(AllocGuestPages(kSize));
        REQUIRE(base_ != nullptr);
    }
    ~GuestHeap() { FreeGuestPages(base_); }
    GuestHeap(const GuestHeap&) = delete;
    GuestHeap& operator=(const GuestHeap&) = delete;
    std::uint8_t* New(std::size_t bytes) {
        used_ = (used_ + 15) & ~std::size_t{15};
        REQUIRE(used_ + bytes <= kSize);
        std::uint8_t* p = base_ + used_;
        used_ += bytes;
        return p;
    }
    static GuestPtr Addr(const void* p) { return static_cast<GuestPtr>(reinterpret_cast<std::uintptr_t>(p)); }

private:
    static constexpr std::size_t kSize = 1u << 16;
    std::uint8_t* base_ = nullptr;
    std::size_t used_ = 0;
};

void Put32(std::uint8_t* object, std::uint32_t offset, std::uint32_t value) {
    std::memcpy(object + offset, &value, 4);
}

}  // namespace

TEST_CASE("outline options: parsers, schema rows and defaults") {
    bool has = true;
    float rgb[3] = {9, 9, 9};
    CHECK(ParseOutlineColor("original", &has, rgb));
    CHECK_FALSE(has);
    CHECK(ParseOutlineColor(" ORIGINAL ", &has, rgb));
    CHECK(ParseOutlineColor("FF00ff", &has, rgb));
    CHECK(has);
    CHECK(Near(rgb[0], 1.0f));
    CHECK(Near(rgb[1], 0.0f));
    CHECK(Near(rgb[2], 1.0f));
    CHECK(ParseOutlineColor("#808080", &has, rgb));
    CHECK(Near(rgb[0], 128.0f / 255.0f));
    CHECK_FALSE(ParseOutlineColor("FF00F", &has, rgb));
    CHECK_FALSE(ParseOutlineColor("GG0000", &has, rgb));
    CHECK_FALSE(ParseOutlineColor("", &has, rgb));

    float v = 0.0f;
    CHECK(ParseOutlineOpacity("original", &has, &v));
    CHECK_FALSE(has);
    CHECK(ParseOutlineOpacity("0.5", &has, &v));
    CHECK(has);
    CHECK(Near(v, 0.5f));
    CHECK(ParseOutlineOpacity("1", &has, &v));
    CHECK_FALSE(ParseOutlineOpacity("1.5", &has, &v));
    CHECK_FALSE(ParseOutlineOpacity("-0.1", &has, &v));
    CHECK(ParseOutlineThickness("0", &v));
    CHECK(v == 0.0f);
    CHECK(ParseOutlineThickness("2.5", &v));
    CHECK(Near(v, 2.5f));
    CHECK_FALSE(ParseOutlineThickness("4.5", &v));
    CHECK_FALSE(ParseOutlineThickness("thick", &v));

    // The schema accepts the same values and its defaults give the original look (nothing active).
    std::string error;
    const core::ConfigKeyDef* color = core::FindConfigKey("render", "outline_color");
    const core::ConfigKeyDef* opacity = core::FindConfigKey("render", "outline_opacity");
    const core::ConfigKeyDef* thickness = core::FindConfigKey("render", "outline_thickness");
    REQUIRE(color != nullptr);
    REQUIRE(opacity != nullptr);
    REQUIRE(thickness != nullptr);
    CHECK(color->tab == core::ConfigUiTab::Graphics);
    CHECK_FALSE(color->Has(core::kConfigDeveloper));
    CHECK(core::ValidateConfigValue(*color, "FF00FF", error));
    CHECK(core::ValidateConfigValue(*color, "original", error));
    CHECK_FALSE(core::ValidateConfigValue(*color, "red", error));
    CHECK(core::ValidateConfigValue(*opacity, "0.25", error));
    CHECK_FALSE(core::ValidateConfigValue(*opacity, "2", error));
    CHECK(core::ValidateConfigValue(*thickness, "2.5", error));
    CHECK_FALSE(core::ValidateConfigValue(*thickness, "3", error));
    OutlineSettings s;
    CHECK(ParseOutlineColor(core::ConfigKeyDefault("render", "outline_color"), &s.hasColor, s.color));
    CHECK(ParseOutlineOpacity(core::ConfigKeyDefault("render", "outline_opacity"), &s.hasOpacity, &s.opacity));
    CHECK(ParseOutlineThickness(core::ConfigKeyDefault("render", "outline_thickness"), &s.thickness));
    CHECK_FALSE(s.Active());
    CHECK_FALSE(Settings{}.outline.Active());
    s.thickness = 0.0f;
    CHECK(s.Active());
}

TEST_CASE("outline: shell <-> body pairing in triangle order and thickness scaling") {
    const Meshes m = MakeMeshes();
    std::vector<std::uint16_t> map(6);
    float worst = 0.0f;
    REQUIRE(outline::PairShellToBody(m.shell.data(), 6, m.shellTris.data(), m.body.data(), 8, m.bodyTris.data(), 4,
                                     0.1f, map.data(), &worst));
    CHECK(Near(worst, 0.05f));
    CHECK(map[0] == 0);
    CHECK(map[4] == 3);  // first corner that uses shell vertex 4 is body vertex 3
    // k = 3: every offset becomes three times as large; the zero-offset vertex stays put.
    std::vector<float> out(18);
    REQUIRE(outline::ScaleShell(m.shell.data(), m.body.data(), map.data(), 6, 3.0f, 0.1f, out.data()));
    CHECK(Near(out[2], 0.0f));
    for (int v = 1; v < 6; ++v) {
        CHECK(Near(out[v * 3 + 2], 0.15f));
        CHECK(Near(out[v * 3 + 0], m.shell[v * 3 + 0]));
    }
    // k = 1 reproduces the shell exactly.
    REQUIRE(outline::ScaleShell(m.shell.data(), m.body.data(), map.data(), 6, 1.0f, 0.1f, out.data()));
    CHECK(std::memcmp(out.data(), m.shell.data(), out.size() * sizeof(float)) == 0);
    // A different mesh with the same triangle count is rejected by the tolerance; bad indices too.
    std::vector<float> moved = m.body;
    moved[5] += 1.0f;
    CHECK_FALSE(outline::PairShellToBody(m.shell.data(), 6, m.shellTris.data(), moved.data(), 8, m.bodyTris.data(), 4,
                                         0.1f, map.data(), nullptr));
    std::vector<std::uint16_t> badTris = m.bodyTris;
    badTris[0] = 8;
    CHECK_FALSE(outline::PairShellToBody(m.shell.data(), 6, m.shellTris.data(), m.body.data(), 8, badTris.data(), 4,
                                         0.1f, map.data(), nullptr));
    // The guest data changed after the pairing: ScaleShell refuses.
    REQUIRE(outline::PairShellToBody(m.shell.data(), 6, m.shellTris.data(), m.body.data(), 8, m.bodyTris.data(), 4,
                                     0.1f, map.data(), nullptr));
    CHECK_FALSE(outline::ScaleShell(m.shell.data(), moved.data(), map.data(), 6, 2.0f, 0.1f, out.data()));
}

TEST_CASE("outline: shell colour / opacity override, defaults leave the draw untouched") {
    ni::DrawItem item;
    item.state.lightingEnable = true;
    item.state.diffuseSource = FF_MCS_COLOR1;
    item.state.material.diffuse = {0.5f, 0.5f, 0.5f, 0.75f};
    item.state.material.ambient = {0.5f, 0.5f, 0.5f, 0.75f};
    item.state.material.emissive = {0.0f, 0.0f, 0.0f, 0.75f};
    item.lights.count = 2;
    item.lights.ambient = {0.3f, 0.3f, 0.3f, 1.0f};

    ni::DrawItem before;
    std::memcpy(static_cast<void*>(&before), &item, sizeof(item));
    outline::ApplyShellColor(OutlineSettings{}, &item);
    CHECK(std::memcmp(&before, &item, sizeof(item)) == 0);

    OutlineSettings s;
    s.hasColor = true;
    s.color[0] = 1.0f;
    s.color[2] = 1.0f;
    outline::ApplyShellColor(s, &item);
    CHECK(item.state.material.emissive.x == 1.0f);
    CHECK(item.state.material.emissive.y == 0.0f);
    CHECK(item.state.material.emissive.z == 1.0f);
    CHECK(item.state.material.diffuse.x == 0.0f);
    CHECK(item.state.material.ambient.x == 0.0f);
    CHECK(item.state.material.diffuse.w == 0.75f);  // original opacity kept
    CHECK(item.state.diffuseSource == FF_MCS_MATERIAL);
    CHECK(item.lights.count == 0);
    CHECK(item.lights.ambient.x == 0.0f);

    ni::DrawItem only;
    only.state.material.diffuse = {0.5f, 0.5f, 0.5f, 0.75f};
    OutlineSettings o;
    o.hasOpacity = true;
    o.opacity = 1.0f;
    outline::ApplyShellColor(o, &only);
    CHECK(only.state.material.diffuse.w == 1.0f);
    CHECK(only.state.material.diffuse.x == 0.5f);  // colour untouched
}

TEST_CASE("outline: ribbon colours are an arena copy") {
    ni::ExtractArena arena;
    float colors[3][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}};
    ni::GeometryDesc g;
    g.vertexCount = 3;
    g.colors = {colors, 16};
    CHECK_FALSE(outline::RecolorRibbon(OutlineSettings{}, arena, &g));
    CHECK(g.colors.data == colors);

    OutlineSettings s;
    s.hasColor = true;
    s.color[1] = 1.0f;
    s.hasOpacity = true;
    s.opacity = 0.5f;
    REQUIRE(outline::RecolorRibbon(s, arena, &g));
    CHECK(g.colors.data != colors);
    CHECK(g.colors.stride == 16);
    const auto* c = static_cast<const float*>(g.colors.data);
    CHECK(c[1] == 1.0f);
    CHECK(c[3] == 0.5f);
    CHECK(c[11] == 0.0f);   // the band end keeps alpha 0
    CHECK(colors[0][1] == 0.0f);  // guest data unchanged
}

TEST_CASE("outline: QDOutline node in the parent chain") {
    GuestHeap heap;
    auto* names = reinterpret_cast<char*>(heap.New(64));
    std::strcpy(names, "QDOutline");
    std::strcpy(names + 16, "QDOutlineX");
    std::strcpy(names + 32, "Fry_MESHShape:1");
    std::uint8_t* shell = heap.New(0xE0);
    std::uint8_t* lod0 = heap.New(0xE0);
    std::uint8_t* outlineNode = heap.New(0x130);
    std::uint8_t* root = heap.New(0xE0);
    std::memset(shell, 0, 0xE0);
    std::memset(lod0, 0, 0xE0);
    std::memset(outlineNode, 0, 0x130);
    std::memset(root, 0, 0xE0);
    Put32(shell, outline::kAvParent, GuestHeap::Addr(lod0));
    Put32(lod0, outline::kAvParent, GuestHeap::Addr(outlineNode));
    Put32(outlineNode, outline::kAvParent, GuestHeap::Addr(root));
    Put32(outlineNode, outline::kObjectName, GuestHeap::Addr(names));
    Put32(root, outline::kObjectName, GuestHeap::Addr(names + 32));
    const GuestPtr transform = GuestHeap::Addr(shell) + outline::kAvWorld;
    CHECK(outline::FindOutlineNode(transform) == GuestHeap::Addr(outlineNode));
    Put32(outlineNode, outline::kObjectName, GuestHeap::Addr(names + 16));  // longer name: no match
    CHECK(outline::FindOutlineNode(transform) == 0);
    Put32(outlineNode, outline::kObjectName, GuestHeap::Addr(names));
    // a body mesh: parent chain without QDOutline
    std::uint8_t* body = heap.New(0xE0);
    std::memset(body, 0, 0xE0);
    Put32(body, outline::kAvParent, GuestHeap::Addr(root));
    CHECK(outline::FindOutlineNode(GuestHeap::Addr(body) + outline::kAvWorld) == 0);
    CHECK(outline::FindOutlineNode(0) == 0);
}
