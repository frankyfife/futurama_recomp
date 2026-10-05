// Guest NetImmerse objects -> host draw descriptions (ni_extract.h). Owned by the extract agent.
//
// Every function below names the original NiXBoxRenderer code it reproduces. Order of a draw in the
// original (draw core FUN_00149AE0): pipeline select FUN_0015AA60 -> properties FUN_00159CD0 -> pipeline
// vf2 (stages of the pass list) -> VertexBlend -> world transform FUN_001489A0 (+ FUN_00159200 normalize)
// -> lights FUN_0015BF80 -> per pass: vf3 (pass apply 0x15FFE0 -> FUN_00162020 per stage) -> draw.
#include "gfx/ni_extract.h"

#include "core/guest.h"
#include "core/guest_call.h"
#include "gfx/guest_memory.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <cstdio>
#include <optional>


// TEMPORARY PROBE (extract agent investigation, removed before hand-over).
#include "core/log.h"
namespace gfx::ni::probe {
struct Stats {
    std::uint64_t states = 0, path[3] = {}, apply[4] = {}, vc[4][2] = {}, lightingOn = 0, lightsHist[9] = {};
    std::uint64_t diffuseColor1WithColors = 0, withColors = 0, blend = 0, atest = 0, ztest = 0, zwrite = 0;
    std::uint64_t cull[3] = {}, stage0Tex = 0, filter[8] = {}, clamp[5] = {}, mapsMask[128] = {}, effects = 0;
    std::uint64_t dyn = 0, shape = 0, other = 0, skinned = 0, ambientNonZero = 0, fog = 0, mips = 0;
    std::uint64_t specular = 0, stencil = 0, wire = 0, flat = 0;
};
inline Stats g;
inline void Dump() {
    FUT_LOGI(Gfx, "PROBE states %llu paths single %llu multi %llu screen %llu; apply R %llu D %llu M %llu ?%llu",
             (unsigned long long)g.states, (unsigned long long)g.path[0], (unsigned long long)g.path[1],
             (unsigned long long)g.path[2], (unsigned long long)g.apply[0], (unsigned long long)g.apply[1],
             (unsigned long long)g.apply[2], (unsigned long long)g.apply[3]);
    FUT_LOGI(Gfx, "PROBE vc(src,light) I/E %llu I/EAD %llu Em/E %llu Em/EAD %llu AD/E %llu AD/EAD %llu ?/E %llu ?/EAD %llu; lightingOn %llu",
             (unsigned long long)g.vc[0][0], (unsigned long long)g.vc[0][1], (unsigned long long)g.vc[1][0],
             (unsigned long long)g.vc[1][1], (unsigned long long)g.vc[2][0], (unsigned long long)g.vc[2][1],
             (unsigned long long)g.vc[3][0], (unsigned long long)g.vc[3][1], (unsigned long long)g.lightingOn);
    char buf[512];
    int n = 0;
    for (int i = 0; i < 9; ++i) n += std::snprintf(buf + n, sizeof(buf) - n, " %d:%llu", i, (unsigned long long)g.lightsHist[i]);
    FUT_LOGI(Gfx, "PROBE lights%s; ambient!=0 %llu; withColors %llu diffuseCOLOR1 %llu", buf,
             (unsigned long long)g.ambientNonZero, (unsigned long long)g.withColors, (unsigned long long)g.diffuseColor1WithColors);
    FUT_LOGI(Gfx, "PROBE blend %llu atest %llu ztest %llu zwrite %llu cull none %llu cw %llu ccw %llu fog %llu spec %llu stencil %llu wire %llu flat %llu",
             (unsigned long long)g.blend, (unsigned long long)g.atest, (unsigned long long)g.ztest, (unsigned long long)g.zwrite,
             (unsigned long long)g.cull[0], (unsigned long long)g.cull[1], (unsigned long long)g.cull[2], (unsigned long long)g.fog,
             (unsigned long long)g.specular, (unsigned long long)g.stencil, (unsigned long long)g.wire, (unsigned long long)g.flat);
    n = 0;
    for (int i = 0; i < 8; ++i) n += std::snprintf(buf + n, sizeof(buf) - n, " f%d:%llu", i, (unsigned long long)g.filter[i]);
    for (int i = 0; i < 5; ++i) n += std::snprintf(buf + n, sizeof(buf) - n, " c%d:%llu", i, (unsigned long long)g.clamp[i]);
    FUT_LOGI(Gfx, "PROBE stage0Tex %llu mips %llu%s; effects %llu; shape %llu dyn %llu other %llu skinned %llu", (unsigned long long)g.stage0Tex,
             (unsigned long long)g.mips, buf, (unsigned long long)g.effects, (unsigned long long)g.shape,
             (unsigned long long)g.dyn, (unsigned long long)g.other, (unsigned long long)g.skinned);
    n = 0;
    for (int i = 0; i < 128; ++i) if (g.mapsMask[i]) n += std::snprintf(buf + n, sizeof(buf) - n, " %02X:%llu", i, (unsigned long long)g.mapsMask[i]);
    FUT_LOGI(Gfx, "PROBE map masks (bit i = map i present):%s", buf);
}
}  // namespace gfx::ni::probe

namespace gfx::ni {

// ---- ExtractArena -----------------------------------------------------------------------------------------

struct ExtractArena::Block {
    Block* next = nullptr;
    std::size_t size = 0;    // usable bytes after the header
    std::size_t offset = 0;  // next free byte
    unsigned char* Data() noexcept { return reinterpret_cast<unsigned char*>(this + 1); }
};

namespace {
constexpr std::size_t kArenaBlockSize = 1u << 20;  // 1 MiB; larger requests get their own block
}

ExtractArena::ExtractArena() noexcept = default;

ExtractArena::~ExtractArena() {
    for (Block* b = head_; b != nullptr;) {
        Block* next = b->next;
        FreeGuestPages(b);
        b = next;
    }
}

void* ExtractArena::Allocate(std::size_t bytes, std::size_t alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > 4096) {
        return nullptr;
    }
    for (;;) {
        if (current_ != nullptr) {
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(current_->Data());
            const std::uintptr_t start = (base + current_->offset + alignment - 1) & ~(alignment - 1);
            const std::size_t end = static_cast<std::size_t>(start - base) + bytes;
            if (end <= current_->size) {
                used_ += end - current_->offset;
                current_->offset = end;
                return reinterpret_cast<void*>(start);
            }
            if (current_->next != nullptr) {  // reuse the blocks kept by Reset
                current_ = current_->next;
                current_->offset = 0;
                continue;
            }
        }
        const std::size_t size = bytes + alignment > kArenaBlockSize ? bytes + alignment : kArenaBlockSize;
        // Guest-visible pages: FUN_0014F710 (guest code) writes skinned vertices into arena memory.
        void* mem = AllocGuestPages(sizeof(Block) + size);
        if (mem == nullptr) {
            return nullptr;
        }
        Block* b = new (mem) Block;
        b->size = size;
        if (current_ == nullptr) {
            head_ = b;
        } else {
            b->next = current_->next;
            current_->next = b;
        }
        current_ = b;
    }
}

void ExtractArena::Reset() noexcept {
    current_ = head_;
    if (current_ != nullptr) {
        current_->offset = 0;
    }
    used_ = 0;
}

std::size_t ExtractArena::BytesUsed() const noexcept {
    return used_;
}

std::size_t ExtractArena::BytesReserved() const noexcept {
    std::size_t total = 0;
    for (const Block* b = head_; b != nullptr; b = b->next) {
        total += b->size;
    }
    return total;
}

const char* ExtractStatusName(ExtractStatus status) noexcept {
    switch (status) {
    case ExtractStatus::Ok:
        return "ok";
    case ExtractStatus::Empty:
        return "empty";
    case ExtractStatus::Unsupported:
        return "unsupported";
    case ExtractStatus::Error:
        return "error";
    }
    return "?";
}

namespace {

// ---- Xbox D3D8 codes REN uses -------------------------------------------------------------------------------

// D3DRENDERSTATETYPE (Xbox): index into D3D_g_RenderState 0x2446D0 (D3DRS_ZFUNC 57 = 0x2447B4, FOGENABLE 92 =
// 0x244840, LIGHTING 102 = 0x244868, AMBIENT 115 = 0x24489C) and REN's cache rs+0x22C + S*8 (FUN_001478C0:
// 0x88..0x96 dispatch to the complex setters PSTextureModes .. LogicOp).
namespace rs {
constexpr std::uint32_t ZFUNC = 57, ALPHAFUNC = 58, ALPHABLENDENABLE = 59, ALPHATESTENABLE = 60, ALPHAREF = 61,
                        SRCBLEND = 62, DESTBLEND = 63, ZWRITEENABLE = 64, DITHERENABLE = 65, SHADEMODE = 66,
                        STENCILZFAIL = 68, STENCILPASS = 69, STENCILFUNC = 70, STENCILREF = 71, STENCILMASK = 72,
                        FOGENABLE = 92, FOGTABLEMODE = 93, FOGSTART = 94, FOGEND = 95, FOGDENSITY = 96,
                        RANGEFOGENABLE = 97, WRAP0 = 98, WRAP1 = 99, LIGHTING = 102, SPECULARENABLE = 103,
                        LOCALVIEWER = 104, COLORVERTEX = 105, SPECULARMATERIALSOURCE = 110,
                        DIFFUSEMATERIALSOURCE = 111, AMBIENTMATERIALSOURCE = 112, EMISSIVEMATERIALSOURCE = 113,
                        AMBIENT = 115, VERTEXBLEND = 137, FOGCOLOR = 138, FILLMODE = 139,
                        NORMALIZENORMALS = 142, ZENABLE = 143, STENCILENABLE = 144, STENCILFAIL = 145,
                        CULLMODE = 147, TEXTUREFACTOR = 148, MULTISAMPLEANTIALIAS = 152;
}  // namespace rs

// D3DTEXTURESTAGESTATETYPE (Xbox): D3D_g_DeferredTextureState 0x2444D0 + (stage*32 + T)*4; REN's cache
// rs+0x76C + (stage*32 + T)*8 (FUN_00161D60 +0x76C/+0x774, FUN_00161E40 +0x784/+0x78C/+0x794, FUN_00162020
// +0x7CC..+0x804, FUN_00161F70 +0x814/+0x84C; D3D default bytes DAT_00242728).
namespace tss {
constexpr std::uint32_t ADDRESSU = 0, ADDRESSV = 1, MAGFILTER = 3, MINFILTER = 4, MIPFILTER = 5, COLOROP = 12,
                        COLORARG0 = 13, COLORARG1 = 14, COLORARG2 = 15, ALPHAOP = 16, ALPHAARG0 = 17,
                        ALPHAARG1 = 18, ALPHAARG2 = 19, RESULTARG = 20, TEXTURETRANSFORMFLAGS = 21,
                        TEXCOORDINDEX = 28;
}  // namespace tss

// Xbox D3D enum values (render-state cache ctor FUN_00158DE0 tables, FUN_00159410).
constexpr std::uint32_t kCmpNever = 0x200, kCmpLessEqual = 0x203, kCmpAlways = 0x207;
constexpr std::uint32_t kCullNone = 0, kCullCW = 0x900, kCullCCW = 0x901;
constexpr std::uint32_t kFillPoint = 0x1B00, kFillWireframe = 0x1B01, kFillSolid = 0x1B02;
constexpr std::uint32_t kShadeFlat = 0x1D00, kShadeGouraud = 0x1D01;
constexpr std::uint32_t kXTopDisable = 1, kXTopSelectArg1 = 2, kXTopSelectArg2 = 3, kXTopModulate = 4;

// AlphaFunction -> D3DBLEND (rs+0x100, FUN_00158DE0 param_1[0x40..0x4A]); indices 11..15 overlap the alpha
// test table rs+0x12C (reproduced: the cache object stores both tables back to back).
constexpr std::uint32_t kBlendTable[16] = {1,     0,     0x300, 0x301, 0x306, 0x307, 0x302, 0x303,
                                           0x304, 0x305, 0x308, 0x207, 0x201, 0x202, 0x203, 0x204};
// TestFunction -> D3DCMP (rs+0x12C, param_1[0x4B..0x52]): ALWAYS LESS EQUAL LESSEQUAL GREATER NOTEQUAL
// GREATEREQUAL NEVER.
constexpr std::uint32_t kAlphaTestTable[8] = {0x207, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x200};
// Stencil TestFunction -> D3DCMP (rs+0x1A0, param_1[0x68..0x6F]): NEVER .. ALWAYS.
constexpr std::uint32_t kStencilTestTable[8] = {0x200, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207};
// Stencil Action -> D3DSTENCILOP (rs+0x1C0, param_1[0x70..0x75]): KEEP ZERO REPLACE INCRSAT DECRSAT INVERT.
constexpr std::uint32_t kStencilOpTable[6] = {0x1E00, 0, 0x1E01, 0x1E02, 0x1E03, 0x150A};
// DrawMode * 2 + left-right swap -> D3DCULL (rs+0x1D8, param_1[0x76..0x7D]).
constexpr std::uint32_t kCullTable[8] = {kCullCW, kCullCCW, kCullCW, kCullCCW, kCullCCW, kCullCW, kCullNone, kCullNone};

// NiTexturingMap clamp -> (ADDRESSU, ADDRESSV) (DAT_0030FED8, set by FUN_0015A120): CLAMP_S_CLAMP_T,
// CLAMP_S_WRAP_T, WRAP_S_CLAMP_T, WRAP_S_WRAP_T (D3DTADDRESS_WRAP 1, CLAMP 3).
constexpr std::uint32_t kClampTable[4][2] = {{3, 3}, {3, 1}, {1, 3}, {1, 1}};
// NiTexturingMap filter -> (MINFILTER, MAGFILTER, MIPFILTER) (DAT_0030FEF8 + 12 * f, set by FUN_00159F40;
// D3DTEXF_NONE 0, POINT 1, LINEAR 2): NEAREST, BILERP, TRILERP, NEAREST_MIPNEAREST, NEAREST_MIPLERP,
// BILERP_MIPNEAREST.
constexpr std::uint32_t kFilterTable[6][3] = {{1, 1, 0}, {2, 2, 0}, {2, 2, 2}, {1, 1, 1}, {1, 1, 2}, {2, 2, 1}};

// Pipeline manager stage-op tables (FUN_0015A120): per map type (+0x94 of the stage object) COLOROP,
// COLORARG1, COLORARG2, ALPHAOP, ALPHAARG1, ALPHAARG2. First table (+0x108, stage 0 of a pass) and second
// table (+0x1F8, later stages). Type 8 (bump) uses manager +0x104 = 0x1A BUMPENVMAPLUMINANCE (TextureOpCaps
// 0x00FFFFFF of the static caps table DAT_002D7580 has bit 0x400000). Types 3, 5, 7 of the first table and
// 9 are never written by FUN_0015A120 (uninitialized heap; the default stage of type 9 only uses
// SELECTARG2 / ARG2, see SetupStageOps) - 0 here.
constexpr std::uint32_t kFirstStageOps[10][6] = {
    {2, 2, 0, 2, 2, 0},     // 0 glow
    {4, 2, 0, 4, 2, 0},     // 1 decal
    {4, 2, 0, 4, 2, 0},     // 2 base / dark
    {0, 0, 0, 0, 0, 0},     // 3 detail (unset)
    {4, 2, 0, 4, 2, 0},     // 4
    {0, 0, 0, 0, 0, 0},     // 5 (unset)
    {4, 2, 0, 2, 2, 0},     // 6
    {0, 0, 0, 0, 0, 0},     // 7 (unset)
    {0x1A, 2, 1, 3, 2, 1},  // 8 bump
    {0, 0, 0, 0, 0, 0},     // 9 default stage (unset)
};
constexpr std::uint32_t kLaterStageOps[10][6] = {
    {7, 2, 1, 3, 2, 1},     // 0 glow: ADD
    {0xE, 2, 1, 3, 2, 1},   // 1 decal: Xbox 14 = BLENDTEXTUREALPHA
    {4, 2, 1, 4, 2, 1},     // 2 base / dark: MODULATE
    {5, 2, 1, 3, 2, 1},     // 3 detail: MODULATE2X
    {2, 2, 1, 2, 2, 1},     // 4
    {2, 2, 1, 3, 2, 1},     // 5
    {4, 2, 1, 2, 2, 1},     // 6
    {4, 2, 1, 3, 2, 1},     // 7
    {0x1A, 2, 1, 3, 2, 1},  // 8 bump
    {4, 2, 1, 4, 2, 1},     // 9
};
constexpr std::uint32_t kMapTypeBase = 2;     // vf2 0x1643E9 (push 2)
constexpr std::uint32_t kMapTypeDefault = 9;  // stage ctor FUN_00161B20 (+0x94 = 9)

// REN's render-state cache start values DAT_002D8338[57..165] (FUN_00159410 loop 0x159422).
constexpr std::uint32_t kRenRsInit[StateShadow::kRsCount] = {
    0x00000203, 0x00000207, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00000001,
    0x00000000, 0x00001D01, 0x0000000F, 0x00001E00, 0x00001E00, 0x00000207, 0x00000000, 0xFFFFFFFF,
    0xFFFFFFFF, 0x00008006, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x3F800000, 0x3F800000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00000001, 0x00000001,
    0x00000002, 0x00000001, 0x00000002, 0x00000000, 0x00000002, 0x00000001, 0x00000002, 0x00000000,
    0x00000000, 0x00000000, 0x3F800000, 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000040, 0x00000000, 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00001B02, 0x00001B02, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00001E00, 0x00000900,
    0x00000901, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF, 0x00000000,
    0x00000000, 0x00000200, 0x00000000, 0x00000001, 0x00000000, 0x00000001, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
// The D3D library's render-state defaults DAT_00242748[0..108] = states 57..165, set by FUN_002395C0 through
// D3DDevice_SetRenderStateNotInline at device creation (states 126, 127 and 154 are skipped: 0xDEADBEEF
// entries; they stay 0 in BSS).
constexpr std::uint32_t kDevRsInit[StateShadow::kRsCount] = {
    0x00000203, 0x00000207, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00000001,
    0x00000000, 0x00001D01, 0x01010101, 0x00001E00, 0x00001E00, 0x00000207, 0x00000000, 0xFFFFFFFF,
    0xFFFFFFFF, 0x00008006, 0x00000000, 0x00000004, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3F800000, 0x3F800000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00000001,
    0x00000001, 0x00000002, 0x00000001, 0x00000000, 0x00000000, 0x00000002, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x3F800000, 0x00000000, 0x00000000, 0x00000000, 0x3F800000,
    0x00000000, 0x00000000, 0x42800000, 0x00000000, 0x3F800000, 0xDEADBEEF, 0xDEADBEEF, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00001B02, 0x00001B02, 0x00000000, 0x00000000, 0x00000002, 0x00000000,
    0x00001E00, 0x00000900, 0x00000901, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0xFFFFFFFF, 0xDEADBEEF, 0x00000000, 0x00000200, 0x3F800000, 0x00000000, 0x00000001, 0x00000000,
    0x00000001, 0x00000001, 0x00000000, 0x00000000, 0x00000000,
};
// REN's texture-stage cache start values DAT_002D82B8 (FUN_00158BB0): a PC-ordered D3D8 table (1 COLOROP
// MODULATE, 2 COLORARG1 TEXTURE, ...) stored at the Xbox indices; COLOROP/ALPHAOP of stages 1..3 = 1,
// TEXCOORDINDEX = stage.
constexpr std::uint32_t kRenTssInit[StateShadow::kTssCount] = {
    0x7FFFFFFF, 0x00000004, 0x00000002, 0x00000001, 0x00000002, 0x00000002, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x7FFFFFFF, 0x00000001, 0x00000001, 0x00000000,
    0x00000001, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000001, 0x00000001, 0x00000001, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF,
};
// The D3D library's texture-stage defaults (bytes DAT_00242728, FUN_002395C0 0x239642; then TEXCOORDINDEX =
// stage, stage 0 COLOROP = MODULATE, ALPHAOP = SELECTARG1).
constexpr std::uint8_t kDevTssInit[StateShadow::kTssCount] = {1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 1,
                                                              1, 1, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// ---- small helpers ------------------------------------------------------------------------------------------

template <class T>
const T* G(GuestPtr address) noexcept {
    return guest::Ptr<const T>(address);
}
std::uint32_t R32(GuestPtr address) noexcept {
    return guest::Read<std::uint32_t>(address);
}
std::uint16_t R16(GuestPtr address) noexcept {
    return guest::Read<std::uint16_t>(address);
}
std::uint8_t R8(GuestPtr address) noexcept {
    return guest::Read<std::uint8_t>(address);
}
float RF(GuestPtr address) noexcept {
    return guest::Read<float>(address);
}
// Cheap plausibility check of a guest pointer (the guest uses the lower 2 GiB, the XBE starts at 0x10000).
bool Plausible(GuestPtr address) noexcept {
    return address >= 0x10000u && address < 0x80000000u;
}
GuestPtr GuestAddr(const void* p) noexcept {
    return guest::Addr(p);
}
std::uint32_t FloatBits(float f) noexcept {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float BitsFloat(std::uint32_t u) noexcept {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// _ftol2 (FUN_00167080): truncation toward zero; out of range -> 0x80000000 (integer indefinite).
std::int32_t Ftol(double v) noexcept {
    if (!(v > -2147483649.0 && v < 2147483648.0)) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(v);  // C++ conversion truncates toward zero
}
// D3DCOLOR byte of FUN_00159900 / FUN_0015BA40: _ftol(c * 255) & 0xFF.
std::uint32_t ColorByte(float c) noexcept {
    return static_cast<std::uint32_t>(Ftol(static_cast<double>(c) * 255.0)) & 0xFFu;
}
std::uint32_t PackRgb(float r, float g, float b) noexcept {
    return 0xFF000000u | (ColorByte(r) << 16) | (ColorByte(g) << 8) | ColorByte(b);
}
Float4 UnpackArgb(std::uint32_t c) noexcept {
    return {static_cast<float>((c >> 16) & 0xFF) / 255.0f, static_cast<float>((c >> 8) & 0xFF) / 255.0f,
            static_cast<float>(c & 0xFF) / 255.0f, static_cast<float>(c >> 24) / 255.0f};
}

// ---- D3D8 state model (REN cache + device) ----------------------------------------------------------------

std::uint32_t RsIndex(std::uint32_t state) noexcept {
    return state - StateShadow::kRsFirst;
}
// REN's cached render-state write (FUN_001478C0 pattern: `if (cache != v) { cache = v; D3D = v; }`).
void SetRs(StateShadow& s, std::uint32_t state, std::uint32_t value) noexcept {
    const std::uint32_t i = RsIndex(state);
    if (s.renRs[i] != value) {
        s.renRs[i] = value;
        s.devRs[i] = value;
    }
}
// The float variant of FUN_00159700 (fog start/end/density compared as float).
void SetRsFloat(StateShadow& s, std::uint32_t state, float value) noexcept {
    const std::uint32_t i = RsIndex(state);
    if (BitsFloat(s.renRs[i]) != value) {
        s.renRs[i] = FloatBits(value);
        s.devRs[i] = FloatBits(value);
    }
}
std::uint32_t DevRs(const StateShadow& s, std::uint32_t state) noexcept {
    return s.devRs[RsIndex(state)];
}
void SetTss(StateShadow& s, std::uint32_t stage, std::uint32_t type, std::uint32_t value) noexcept {
    if (s.renTss[stage][type] != value) {
        s.renTss[stage][type] = value;
        s.devTss[stage][type] = value;
    }
}
void SetTexture(StateShadow& s, std::uint32_t stage, GuestPtr niTexture, rhi::TextureHandle handle) noexcept {
    s.boundNiTexture[stage] = niTexture;
    s.boundTexture[stage] = handle;
}

}  // namespace

std::uint8_t TextureOpFromXbox(std::uint32_t xboxOp) noexcept {
    // Xbox D3DTEXTUREOP order (XDK d3d8types.h; evidence for the tail: FUN_0015A120 picks 0x1A / 0x19 for the
    // bump map stage by TextureOpCaps 0x400000 BUMPENVMAPLUMINANCE / 0x200000 BUMPENVMAP, and the decal map
    // stage uses 14 = blend by texture alpha).
    switch (xboxOp) {
    case 1:
        return FF_TOP_DISABLE;
    case 2:
        return FF_TOP_SELECTARG1;
    case 3:
        return FF_TOP_SELECTARG2;
    case 4:
        return FF_TOP_MODULATE;
    case 5:
        return FF_TOP_MODULATE2X;
    case 6:
        return FF_TOP_MODULATE4X;
    case 7:
        return FF_TOP_ADD;
    case 8:
        return FF_TOP_ADDSIGNED;
    case 9:
        return FF_TOP_ADDSIGNED2X;
    case 10:
        return FF_TOP_SUBTRACT;
    case 11:
        return FF_TOP_ADDSMOOTH;
    case 12:
        return FF_TOP_BLENDDIFFUSEALPHA;
    case 13:
        return FF_TOP_BLENDCURRENTALPHA;
    case 14:
        return FF_TOP_BLENDTEXTUREALPHA;
    case 15:
        return FF_TOP_BLENDFACTORALPHA;
    case 16:
        return FF_TOP_BLENDTEXTUREALPHAPM;
    case 17:
        return FF_TOP_PREMODULATE;
    case 18:
        return FF_TOP_MODULATEALPHA_ADDCOLOR;
    case 19:
        return FF_TOP_MODULATECOLOR_ADDALPHA;
    case 20:
        return FF_TOP_MODULATEINVALPHA_ADDCOLOR;
    case 21:
        return FF_TOP_MODULATEINVCOLOR_ADDALPHA;
    case 22:
        return FF_TOP_DOTPRODUCT3;
    case 23:
        return FF_TOP_MULTIPLYADD;
    case 24:
        return FF_TOP_LERP;
    case 25:
        return FF_TOP_BUMPENVMAP;
    case 26:
        return FF_TOP_BUMPENVMAPLUMINANCE;
    default:
        return FF_TOP_DISABLE;
    }
}

void StateShadow::Reset() noexcept {
    // Device: D3D library defaults (FUN_002395C0), ZENABLE = depth buffer present (0x239620).
    for (std::uint32_t i = 0; i < kRsCount; ++i) {
        renRs[i] = kRenRsInit[i];
        devRs[i] = kDevRsInit[i] == 0xDEADBEEFu ? 0u : kDevRsInit[i];
    }
    devRs[RsIndex(rs::ZENABLE)] = 1;
    for (std::uint32_t st = 0; st < kMaxTextureStages; ++st) {
        for (std::uint32_t t = 0; t < kTssCount; ++t) {
            devTss[st][t] = kDevTssInit[t];
            if (t == tss::TEXCOORDINDEX) {
                renTss[st][t] = st;  // FUN_00158BB0 (uVar3 == 0x1C)
            } else if (t == tss::COLOROP || t == tss::ALPHAOP) {
                renTss[st][t] = st == 0 ? kRenTssInit[t] : 1u;
            } else {
                renTss[st][t] = kRenTssInit[t];
            }
        }
        devTss[st][tss::TEXCOORDINDEX] = st;
        boundNiTexture[st] = 0;
        boundTexture[st] = {};
        textureTransform[st] = IdentityMatrix();  // FUN_002395C0 sets transforms 0..9 to identity
    }
    devTss[0][tss::COLOROP] = kXTopModulate;     // 0x23966E
    devTss[0][tss::ALPHAOP] = kXTopSelectArg1;   // 0x239679
    material = MaterialDesc{};
    ditherBit = shadeBit = specularBit = stencilOn = wireframeBit = zWriteBit = zTestBit = normalizeOn = 0;
    lastAlphaProperty = lastFogProperty = 0;
    lastFogColor = 0;   // rs+0x16C = param_1[0x5B] = 0 (FUN_00158DE0)
    lastCull = kCullCW; // rs+0x1F8 = param_1[0x7E] = 0x900

    // REN init FUN_00159410 (after the cache tables): explicit cached writes.
    SetRs(*this, rs::CULLMODE, kCullCW);
    SetRs(*this, rs::MULTISAMPLEANTIALIAS, 0);
    SetRs(*this, rs::FILLMODE, kFillSolid);
    SetRs(*this, rs::SHADEMODE, kShadeGouraud);
    SetRs(*this, rs::ALPHABLENDENABLE, 0);
    SetRs(*this, rs::SRCBLEND, kBlendTable[6]);   // rs+0x118
    SetRs(*this, rs::DESTBLEND, kBlendTable[7]);  // rs+0x11C
    SetRs(*this, rs::ALPHATESTENABLE, 0);
    SetRs(*this, rs::ALPHAFUNC, kCmpAlways);
    SetRs(*this, rs::ALPHAREF, 0);
    SetRs(*this, rs::ZFUNC, kCmpAlways);
    SetRs(*this, rs::ZWRITEENABLE, 0);
    SetRs(*this, rs::DITHERENABLE, 0);
    SetRs(*this, rs::SPECULARENABLE, 0);
    SetRs(*this, rs::FOGENABLE, 0);
    // rs+0xE0 bit 0 = per-pixel z fog (FUN_00158D20: RasterCaps FOGTABLE 0x100 and ZFOG 0x100000; the static
    // caps table DAT_002D7580 copied by D3DDevice_GetDeviceCaps 0x2342C0 has RasterCaps 0x00377101) -> 3.
    SetRs(*this, rs::FOGTABLEMODE, FF_FOG_LINEAR);
    SetRs(*this, rs::RANGEFOGENABLE, 0);  // FUN_001478C0(0x61, 0)
    SetRs(*this, rs::FOGCOLOR, 0);        // (0x8A, 0)
    SetRs(*this, rs::NORMALIZENORMALS, 0);// (0x8E, 0)
    SetRs(*this, rs::ZENABLE, 1);         // (0x8F, 1): DAT_0030EDE4 (W-buffer request) is never written
    SetRs(*this, rs::COLORVERTEX, 1);     // (0x69, 1)

    // Light manager ctor FUN_0015BCB0.
    lastAmbient = 0;
    SetRs(*this, rs::AMBIENT, 0);
    SetRs(*this, rs::LIGHTING, 0);
    lightingOn = false;
    lightsEnabled = false;
    vertexColorSourceMode = 0;
    SetRs(*this, rs::EMISSIVEMATERIALSOURCE, 0);
    SetRs(*this, rs::AMBIENTMATERIALSOURCE, 0);
    SetRs(*this, rs::DIFFUSEMATERIALSOURCE, 0);  // cache already 0: the device keeps COLOR1
    lastEffectState = 0;
    lastLightsEnabled = false;
    lights = LightSetDesc{};

    // Pipeline manager init FUN_0015AB40: stage ops, textures, texcoord, address, filters, TTFF, WRAP0/1.
    static constexpr std::uint32_t kInitOps[4][6] = {
        {2, 2, 0, 2, 2, 0}, {2, 2, 1, 2, 2, 1}, {1, 2, 1, 1, 2, 1}, {1, 2, 1, 1, 2, 1}};
    for (std::uint32_t st = 0; st < kMaxTextureStages; ++st) {
        SetTss(*this, st, tss::COLOROP, kInitOps[st][0]);
        SetTss(*this, st, tss::COLORARG1, kInitOps[st][1]);
        SetTss(*this, st, tss::COLORARG2, kInitOps[st][2]);
        SetTss(*this, st, tss::ALPHAOP, kInitOps[st][3]);
        SetTss(*this, st, tss::ALPHAARG1, kInitOps[st][4]);
        SetTss(*this, st, tss::ALPHAARG2, kInitOps[st][5]);
    }
    for (std::uint32_t st = 0; st < kMaxTextureStages; ++st) {
        SetTexture(*this, st, 0, {});
        SetTss(*this, st, tss::TEXCOORDINDEX, 0);
        SetTss(*this, st, tss::ADDRESSU, 1);
        SetTss(*this, st, tss::MAGFILTER, 1);
        SetTss(*this, st, tss::MINFILTER, 1);
        SetTss(*this, st, tss::MIPFILTER, 0);
        SetTss(*this, st, tss::TEXTURETRANSFORMFLAGS, 0);
    }
    SetRs(*this, rs::WRAP0, 0);
    SetRs(*this, rs::WRAP1, 0);
    // ... then the three stage objects +0x10/+0x14/+0x18 through FUN_00162020 + FUN_00161E40(1, 1) +
    // FUN_00161D60 (no texture: only ops, texture factor, filters (1,1,0) and address (1,1)).
    static constexpr std::uint32_t kInitStageObjects[3][6] = {
        {2, 2, 0, 2, 2, 0}, {1, 2, 1, 1, 2, 1}, {1, 1, 0, 1, 1, 0}};
    for (std::uint32_t st = 0; st < 3; ++st) {
        SetTexture(*this, st, 0, {});
        SetTss(*this, st, tss::COLOROP, kInitStageObjects[st][0]);
        SetTss(*this, st, tss::COLORARG1, kInitStageObjects[st][1]);
        SetTss(*this, st, tss::COLORARG2, kInitStageObjects[st][2]);
        SetTss(*this, st, tss::ALPHAOP, kInitStageObjects[st][3]);
        SetTss(*this, st, tss::ALPHAARG1, kInitStageObjects[st][4]);
        SetTss(*this, st, tss::ALPHAARG2, kInitStageObjects[st][5]);
        SetRs(*this, rs::TEXTUREFACTOR, 0xFFFFFFFFu);  // stage ctor FUN_00161B20: +0x8C = 0xFFFFFFFF
        SetTss(*this, st, tss::MAGFILTER, 1);
        SetTss(*this, st, tss::MINFILTER, 1);
        SetTss(*this, st, tss::MIPFILTER, 0);
        SetTss(*this, st, tss::ADDRESSU, 1);
        SetTss(*this, st, tss::ADDRESSV, 1);  // manager +0xFC bit 0 (INDEPENDENTUV, TextureAddressCaps 0x17)
    }

    for (SkinEntry& e : skinCache) {
        e = SkinEntry{};
    }
    skinCacheNext = 0;
    counters = ExtractCounters{};
}

void BeginScene(StateShadow* shadow) noexcept {
    if (shadow == nullptr) {
        return;
    }
    // FUN_00158B90: rs+0x190 (fog property) = rs+0x164 (alpha property) = 0.
    shadow->lastAlphaProperty = 0;
    shadow->lastFogProperty = 0;
    // FUN_0015B990: LightEnable(index, FALSE) for every enabled light, light manager +0x3C = 0.
    shadow->lights.count = 0;
    shadow->lights.dropped = 0;
    shadow->lastEffectState = 0;
}

// ---- camera -----------------------------------------------------------------------------------------------

CameraDesc ExtractCamera(const CameraInput& input) noexcept {
    CameraDesc c;
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    const float* loc = input.location != nullptr ? input.location : zero;
    const float* dir = input.direction != nullptr ? input.direction : zero;
    const float* up = input.up != nullptr ? input.up : zero;
    const float* right = input.right != nullptr ? input.right : zero;
    const NiFrustum fr = input.frustum != nullptr ? *input.frustum : NiFrustum{-1.0f, 1.0f, 1.0f, -1.0f, 0.1f, 1.0f};
    const NiRectF port = input.port != nullptr ? *input.port : NiRectF{0.0f, 1.0f, 1.0f, 0.0f};
    c.location = {loc[0], loc[1], loc[2]};
    c.direction = {dir[0], dir[1], dir[2]};
    c.up = {up[0], up[1], up[2]};
    c.right = {right[0], right[1], right[2]};
    c.frustum = fr;
    c.port = port;

    // View matrix this+0x610 (0x1469E4..0x146ABD): columns right/up/dir, row 3 = -(loc . axis).
    using D = double;
    Matrix4& v = c.view;
    v.m[0][0] = right[0], v.m[0][1] = up[0], v.m[0][2] = dir[0], v.m[0][3] = 0.0f;
    v.m[1][0] = right[1], v.m[1][1] = up[1], v.m[1][2] = dir[1], v.m[1][3] = 0.0f;
    v.m[2][0] = right[2], v.m[2][1] = up[2], v.m[2][2] = dir[2], v.m[2][3] = 0.0f;
    v.m[3][0] = static_cast<float>(-((D(loc[0]) * right[0] + D(loc[2]) * right[2]) + D(loc[1]) * right[1]));
    v.m[3][1] = static_cast<float>(-((D(loc[2]) * up[2] + D(loc[0]) * up[0]) + D(loc[1]) * up[1]));
    v.m[3][2] = static_cast<float>(-((D(loc[1]) * dir[1] + D(loc[0]) * dir[0]) + D(loc[2]) * dir[2]));
    v.m[3][3] = 1.0f;

    // Projection this+0x650 (0x146B4D..0x146C6A), x87 order with float stores of r-l, l+r, t-b, t+b.
    c.nearPlane = fr.nearPlane;                                          // +0x34C
    const D fn = D(fr.farPlane) - D(fr.nearPlane);
    c.depthRange = static_cast<float>(fn);                              // +0x350 (fst)
    const float rl = static_cast<float>(D(fr.right) - D(fr.left));
    const float lr = static_cast<float>(D(fr.left) + D(fr.right));
    const float tb = static_cast<float>(D(fr.top) - D(fr.bottom));
    const float tbs = static_cast<float>(D(fr.top) + D(fr.bottom));
    const D invFN = 1.0 / fn;
    const D invRL = 1.0 / D(rl);
    Matrix4& p = c.projection;
    if (!input.leftRightSwap) {
        p.m[0][0] = static_cast<float>(invRL + invRL);
        p.m[2][0] = static_cast<float>(-(invRL * D(lr)));
    } else {
        p.m[0][0] = static_cast<float>(-2.0 * invRL);
        p.m[2][0] = static_cast<float>(invRL * D(lr));
    }
    const D invTB = 1.0 / D(tb);
    p.m[1][1] = static_cast<float>(invTB + invTB);
    p.m[2][1] = static_cast<float>(-(invTB * D(tbs)));
    p.m[2][2] = static_cast<float>(invFN * D(fr.farPlane));
    p.m[2][3] = 1.0f;
    p.m[3][2] = static_cast<float>(-((D(fr.farPlane) * D(fr.nearPlane)) * invFN));
    p.m[3][3] = 0.0f;

    // Screen-polygon projection this+0x394 (init FUN_00147CD0; P22/P32 copied to +0x3BC/+0x3CC).
    Matrix4& sp = c.screenProjection;
    sp = Matrix4{};
    sp.m[0][0] = 2.0f;
    sp.m[1][1] = 2.0f;
    sp.m[2][0] = -1.0f;
    sp.m[2][1] = -1.0f;
    sp.m[2][2] = p.m[2][2];
    sp.m[2][3] = 1.0f;
    sp.m[3][2] = p.m[3][2];

    // Viewport (0x146C71..0x146D1A): target size from renderer +0x58 (unsigned), _ftol truncation.
    const D w = D(input.targetWidth), h = D(input.targetHeight);
    c.viewport.x = static_cast<float>(Ftol(w * D(port.left)));
    c.viewport.y = static_cast<float>(Ftol((1.0 - D(port.top)) * h));
    c.viewport.width = static_cast<float>(Ftol((D(port.right) - D(port.left)) * w));
    c.viewport.height = static_cast<float>(Ftol((D(port.top) - D(port.bottom)) * h));
    c.viewport.minDepth = 0.0f;
    c.viewport.maxDepth = 1.0f;
    {  // TEMPORARY PROBE
        static int logged = 0;
        static float lastKey = -1.0f;
        const float key = fr.left * 3 + fr.right * 5 + fr.nearPlane * 7 + fr.farPlane + port.left + port.top * 11;
        if (logged < 8 && key != lastKey) {
            ++logged;
            lastKey = key;
            FUT_LOGI(Gfx, "PROBE camera loc (%.2f %.2f %.2f) dir (%.2f %.2f %.2f) up (%.2f %.2f %.2f) right (%.2f %.2f %.2f) frustum l %.3f r %.3f t %.3f b %.3f n %.3f f %.1f port l %.2f r %.2f t %.2f b %.2f -> vp %.0f %.0f %.0f %.0f",
                     loc[0], loc[1], loc[2], dir[0], dir[1], dir[2], up[0], up[1], up[2], right[0], right[1], right[2], fr.left, fr.right, fr.top, fr.bottom, fr.nearPlane, fr.farPlane, port.left, port.right, port.top, port.bottom, c.viewport.x, c.viewport.y, c.viewport.width, c.viewport.height);
        }
    }
    return c;
}

CameraDesc DefaultCamera(std::uint32_t targetWidth, std::uint32_t targetHeight) noexcept {
    static constexpr float kLoc[3] = {0.0f, 0.0f, 0.0f};    // DAT_002F90BC
    static constexpr float kDir[3] = {1.0f, 0.0f, 0.0f};    // 0x2D7740
    static constexpr float kUp[3] = {0.0f, 1.0f, 0.0f};     // 0x2D774C
    static constexpr float kRight[3] = {0.0f, 0.0f, 1.0f};  // 0x2D7758
    static constexpr NiFrustum kFrustum{-1.0f, 1.0f, 1.0f, -1.0f, 0.1f, 1.0f};
    static constexpr NiRectF kPort{0.0f, 1.0f, 1.0f, 0.0f};
    CameraInput in;
    in.location = kLoc;
    in.direction = kDir;
    in.up = kUp;
    in.right = kRight;
    in.frustum = &kFrustum;
    in.port = &kPort;
    in.targetWidth = targetWidth;
    in.targetHeight = targetHeight;
    return ExtractCamera(in);
}

bool ClearRect(const NiRectF* rect, std::uint32_t targetWidth, std::uint32_t targetHeight, rhi::Rect* out) noexcept {
    const auto w = static_cast<std::int32_t>(targetWidth);
    const auto h = static_cast<std::int32_t>(targetHeight);
    if (rect == nullptr) {
        if (out != nullptr) {
            *out = rhi::Rect{0, 0, w, h};
        }
        return false;
    }
    const double dw = targetWidth, dh = targetHeight;
    std::int32_t l = Ftol(dw * rect->left);
    std::int32_t t = Ftol((1.0 - static_cast<double>(rect->top)) * dh);
    std::int32_t r = Ftol(dw * rect->right);
    std::int32_t b = Ftol((1.0 - static_cast<double>(rect->bottom)) * dh);
    auto clampTo = [](std::int32_t v, std::int32_t hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    l = clampTo(l, w);
    r = clampTo(r, w);
    t = clampTo(t, h);
    b = clampTo(b, h);
    if (out != nullptr) {
        *out = rhi::Rect{l, t, r, b};
    }
    return true;
}

namespace {

// ---- texture queries ----------------------------------------------------------------------------------------

struct TextureInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 1;
};

// NiTexture::GetWidth/GetHeight (vf11/vf12) and the renderer data's mip count (data vf5), without calling
// guest code (ni_guest_layouts.h, kTextureData*). Renderer data that does not record the mip count (0) falls
// back to the NiPixelData mip count: texconv uploads the levels as stored (GFX_CONTRACT.md 7).
TextureInfo QueryTexture(GuestPtr niTexture) noexcept {
    TextureInfo info;
    if (!Plausible(niTexture)) {
        return info;
    }
    const GuestPtr vtable = R32(niTexture);
    std::uint32_t pixelMips = 0;
    std::uint32_t pixelWidth = 0, pixelHeight = 0;
    if (vtable == kNiSourceTextureVtable) {
        const GuestPtr pixels = R32(niTexture + offsetof(NiSourceTextureLayout, pixelData));
        if (Plausible(pixels)) {
            const NiPixelData* pd = G<NiPixelData>(pixels);
            pixelMips = pd->mipCount;
            if (Plausible(pd->widths)) {
                pixelWidth = R32(pd->widths);
            }
            if (Plausible(pd->heights)) {
                pixelHeight = R32(pd->heights);
            }
        }
    }
    const GuestPtr data = R32(niTexture + kNiTextureRendererData);
    if (Plausible(data)) {
        info.width = R32(data + kTextureDataWidth);
        info.height = R32(data + kTextureDataHeight);
        const std::uint32_t mips = R32(data + kTextureDataMipLevels);
        info.mipLevels = mips != 0 ? mips : (pixelMips != 0 ? pixelMips : 1);
        return info;
    }
    if (vtable == kNiSourceTextureVtable) {
        info.width = pixelWidth;
        info.height = pixelHeight;
        info.mipLevels = pixelMips != 0 ? pixelMips : 1;
    } else if (vtable == kNiRenderedTextureVtable) {
        info.width = R32(niTexture + kNiRenderedTextureWidth);
        info.height = R32(niTexture + kNiRenderedTextureHeight);
    }
    return info;
}

struct Resolved {
    bool ok = false;
    rhi::TextureHandle handle;
};
Resolved ResolveTexture(const ExtractContext& context, StateShadow& s, GuestPtr niTexture) noexcept {
    if (niTexture == 0) {
        return {};
    }
    if (context.resolveTexture == nullptr) {
        return {true, {}};
    }
    Resolved r;
    r.handle = context.resolveTexture(context.resolveUser, niTexture);
    r.ok = static_cast<bool>(r.handle);
    if (!r.ok) {
        ++s.counters.unresolvedTextures;
    }
    return r;
}

// ---- properties (FUN_00159CD0) ------------------------------------------------------------------------------

// The 12 property pointers of a NiPropertyState (+0x08..+0x34).
struct Props {
    GuestPtr alpha, dither, fog, material, rendererSpecific, shade, specular, stencil, texturing, vertexColor,
        wireframe, zbuffer;
};
bool ReadProps(GuestPtr propertyState, Props* p) noexcept {
    if (!Plausible(propertyState)) {
        return false;
    }
    const NiPropertyState* ps = G<NiPropertyState>(propertyState);
    *p = Props{ps->alpha,  ps->dither,   ps->fog,         ps->material,  ps->rendererSpecific, ps->shade,
               ps->specular, ps->stencil, ps->texturing, ps->vertexColor, ps->wireframe,      ps->zbuffer};
    const GuestPtr all[12] = {p->alpha,  p->dither,   p->fog,         p->material,  p->rendererSpecific,
                              p->shade,  p->specular, p->stencil,     p->texturing, p->vertexColor,
                              p->wireframe, p->zbuffer};
    for (GuestPtr q : all) {
        if (!Plausible(q)) {  // FUN_00159CD0 dereferences every entry without a NULL check
            return false;
        }
    }
    return true;
}
std::uint16_t Flags(GuestPtr property) noexcept {
    return R16(property + offsetof(NiProperty, flags));
}

// FUN_00159270 (alpha property; cached by pointer, rs+0x164).
void ApplyAlpha(StateShadow& s, GuestPtr property) noexcept {
    if (property == s.lastAlphaProperty) {
        return;  // rs+0x228 (force flag) is never set (only cleared, 0x159115 / 0x159DF1)
    }
    s.lastAlphaProperty = property;
    const std::uint32_t f = Flags(property);
    if ((f & 1u) == 0) {
        SetRs(s, rs::ALPHABLENDENABLE, 0);
    } else {
        SetRs(s, rs::ALPHABLENDENABLE, 1);
        SetRs(s, rs::SRCBLEND, kBlendTable[(f >> 1) & 0xFu]);
        SetRs(s, rs::DESTBLEND, kBlendTable[(f >> 5) & 0xFu]);
    }
    if (((f >> 9) & 1u) == 0) {
        SetRs(s, rs::ALPHATESTENABLE, 0);
    } else {
        SetRs(s, rs::ALPHATESTENABLE, 1);
        SetRs(s, rs::ALPHAFUNC, kAlphaTestTable[(f >> 10) & 7u]);
        SetRs(s, rs::ALPHAREF, R8(property + offsetof(NiAlphaProperty, testRef)));
    }
}

// FUN_00159B60 (z buffer; byte caches rs+0x19C test, rs+0x19B write).
void ApplyZBuffer(StateShadow& s, GuestPtr property) noexcept {
    const std::uint32_t f = Flags(property);
    const std::uint8_t test = f & 1u, write = (f >> 1) & 1u;
    if (s.zTestBit != test) {
        SetRs(s, rs::ZFUNC, test != 0 ? kCmpLessEqual : kCmpAlways);
        s.zTestBit = test;
    }
    if (s.zWriteBit != write) {
        SetRs(s, rs::ZWRITEENABLE, write);
        s.zWriteBit = write;
    }
}

// FUN_00159700 (fog; cached by pointer rs+0x190). near/far/far-near/1-over-maxFog-minus-1 are rs+0x178..
// +0x184 (SetCameraData 0x146D29, slot 30); max fog stays 1.0 (slot 30 is never called) -> +0x184 = 0.
void ApplyFog(StateShadow& s, GuestPtr property, const CameraDesc& camera) noexcept {
    if (property == s.lastFogProperty) {
        return;
    }
    s.lastFogProperty = property;
    const std::uint32_t f = Flags(property);
    if (((f >> 1) & 1u) == 0) {
        SetRs(s, rs::FOGENABLE, 0);
        return;
    }
    SetRs(s, rs::FOGENABLE, 1);
    const float farPlane = camera.frustum.farPlane;      // rs+0x17C (0x146D37)
    const float depth = RF(property + offsetof(NiFogProperty, depth));
    const float d = depth * camera.depthRange;           // fVar1 = depth * (far - near)
    const float start = farPlane - d;                    // far - fVar1
    const float end = d * 0.0f + farPlane;               // fVar1 * (1/maxFog - 1) + far
    SetRsFloat(s, rs::FOGDENSITY, 1.0f / d);
    SetRsFloat(s, rs::FOGSTART, start);
    SetRsFloat(s, rs::FOGEND, end);
    const std::uint32_t function = (f >> 3) & 3u;
    if (function == 0) {         // FOG_Z_LINEAR
        SetRs(s, rs::FOGTABLEMODE, FF_FOG_LINEAR);
        SetRs(s, rs::RANGEFOGENABLE, 0);
    } else if (function == 1) {  // FOG_RANGE_SQ (rs+0xE0 bit 0 set: z fog supported)
        SetRs(s, rs::FOGTABLEMODE, FF_FOG_EXP);
        SetRs(s, rs::RANGEFOGENABLE, 1);
    }
    // FOG_VERTEX_ALPHA: rs+0x18C (byte) is never written (BSS of the cache object: 0) -> the table mode of
    // the previous draw stays.
}

// FUN_00159900 (fog color; cached packed value rs+0x16C).
void ApplyFogColor(StateShadow& s, GuestPtr fogProperty) noexcept {
    const NiFogProperty* fog = G<NiFogProperty>(fogProperty);
    const std::uint32_t c = PackRgb(fog->color.r, fog->color.g, fog->color.b);
    if (c != s.lastFogColor) {
        SetRs(s, rs::FOGCOLOR, c);
        s.lastFogColor = c;
    }
}

// FUN_001599E0 (stencil + cull).
void ApplyStencil(StateShadow& s, GuestPtr property, bool leftRightSwap) noexcept {
    const NiStencilProperty* st = G<NiStencilProperty>(property);
    if (st->enable == 0) {
        if (s.stencilOn != 0) {
            s.stencilOn = 0;
            SetRs(s, rs::STENCILENABLE, 0);
        }
    } else {
        s.stencilOn = 1;
        SetRs(s, rs::STENCILENABLE, 1);
        SetRs(s, rs::STENCILFUNC, kStencilTestTable[st->func & 7u]);
        SetRs(s, rs::STENCILREF, st->ref);
        SetRs(s, rs::STENCILMASK, st->mask);
        SetRs(s, rs::STENCILFAIL, st->failAction < 6 ? kStencilOpTable[st->failAction] : kStencilOpTable[0]);
        SetRs(s, rs::STENCILZFAIL, st->zFailAction < 6 ? kStencilOpTable[st->zFailAction] : kStencilOpTable[0]);
        SetRs(s, rs::STENCILPASS, st->passAction < 6 ? kStencilOpTable[st->passAction] : kStencilOpTable[0]);
    }
    const std::uint32_t index = (st->drawMode & 3u) * 2u + (leftRightSwap ? 1u : 0u);
    const std::uint32_t cull = kCullTable[index];
    if (cull != s.lastCull) {
        s.lastCull = cull;
        SetRs(s, rs::CULLMODE, cull);
    }
}

// FUN_00161A80 + FUN_00161800 + FUN_00161880 (D3DMATERIAL8; rebuilt when the revision +0x54 changes).
// ASSUMPTION: the game changes material colors only through NiMaterialProperty setters that bump the
// revision, so the current values equal the original's cached D3DMATERIAL8.
void ApplyMaterial(StateShadow& s, GuestPtr property) noexcept {
    const NiMaterialProperty* m = G<NiMaterialProperty>(property);
    const float a = m->alpha;
    s.material.diffuse = {m->diffuse.r, m->diffuse.g, m->diffuse.b, a};
    s.material.ambient = {m->ambient.r, m->ambient.g, m->ambient.b, a};
    s.material.specular = {m->specular.r, m->specular.g, m->specular.b, a};
    s.material.emissive = {m->emissive.r, m->emissive.g, m->emissive.b, a};
    s.material.power = m->shininess;
}

void ApplyProperties(StateShadow& s, const Props& p, const CameraDesc& camera, bool leftRightSwap) noexcept {
    // Wireframe (rs+0x19A): 0x1B02 - (bit != 0).
    const std::uint8_t wire = Flags(p.wireframe) & 1u;
    if (wire != s.wireframeBit) {
        SetRs(s, rs::FILLMODE, wire != 0 ? kFillWireframe : kFillSolid);
        s.wireframeBit = wire;
    }
    // FUN_00159980 shade (rs+0x196): 0x1D00 + (bit != 0).
    const std::uint8_t shade = Flags(p.shade) & 1u;
    if (shade != s.shadeBit) {
        SetRs(s, rs::SHADEMODE, shade != 0 ? kShadeGouraud : kShadeFlat);
        s.shadeBit = shade;
    }
    ApplyAlpha(s, p.alpha);
    ApplyZBuffer(s, p.zbuffer);
    ApplyFog(s, p.fog, camera);
    ApplyFogColor(s, p.fog);
    // Dither (rs+0x195).
    const std::uint8_t dither = Flags(p.dither) & 1u;
    if (dither != s.ditherBit) {
        SetRs(s, rs::DITHERENABLE, dither);
        s.ditherBit = dither;
    }
    // Specular (rs+0x197).
    const std::uint8_t specular = Flags(p.specular) & 1u;
    if (specular != s.specularBit) {
        SetRs(s, rs::SPECULARENABLE, specular);
        s.specularBit = specular;
    }
    ApplyStencil(s, p.stencil, leftRightSwap);
    ApplyMaterial(s, p.material);
}

// FUN_00159200 (via FUN_001489A0): NORMALIZENORMALS for world scales outside [0.99f, 1.01f] or NaN
// (fcomp 0.99f / test ah,1 / jne; fcomp 1.01f / test ah,0x41 / jp).
void ApplyNormalize(StateShadow& s, float scale) noexcept {
    const bool outside = !(scale >= 0.99f) || !(scale <= 1.01f);
    const std::uint8_t want = outside ? 1u : 0u;
    if (want == s.normalizeOn) {
        return;
    }
    SetRs(s, rs::NORMALIZENORMALS, want);
    s.normalizeOn = want;
}

// ---- texture pipeline ----------------------------------------------------------------------------------------

// The fields of the pipeline's 0xCC-byte stage object (ctor FUN_00161B20) that reach D3D.
struct PipeStage {
    GuestPtr texture = 0;          // +0x0C
    std::uint32_t ops[6] = {};     // +0x10..+0x24 COLOROP, COLORARG1, COLORARG2, ALPHAOP, ALPHAARG1, ALPHAARG2
    std::uint32_t texCoordIndex = 0;  // +0x28
    std::uint32_t addressU = 1;    // +0x2C
    std::uint32_t addressV = 1;    // +0x30
    std::uint32_t filter = 0;      // +0x38 (NI filter mode)
    std::uint32_t ttff = 0;        // +0x48
    std::uint32_t textureFactor = 0xFFFFFFFFu;  // +0x8C
    bool notReplace = true;        // +0x90 (applyMode != REPLACE)
    std::uint32_t type = kMapTypeDefault;  // +0x94
};

// FUN_00163D60: a stage for a map; no texture or no UV sets -> none.
bool MakeMapStage(StateShadow& s, GuestPtr texture, std::uint32_t clamp, std::uint32_t filter, std::uint32_t type,
                  std::uint32_t uvSet, std::uint32_t uvSetCount, PipeStage* out) noexcept {
    if (texture == 0 || uvSetCount == 0) {
        return false;
    }
    PipeStage st;
    st.texture = texture;
    if (clamp > 3) {  // beyond DAT_0030FED8 (garbage in the original)
        ++s.counters.invalidMapModes;
        clamp = 3;
    }
    st.addressU = kClampTable[clamp][0];  // FUN_00159E00
    st.addressV = kClampTable[clamp][1];
    st.filter = filter;                   // FUN_00159E20
    st.type = type;
    st.texCoordIndex = uvSet < uvSetCount ? uvSet : uvSetCount - 1;
    st.ttff = 0;
    st.notReplace = false;
    *out = st;
    return true;
}

// FUN_001629C0 for a stage at position `index` of its pass (param_3 = 0).
void SetupStageOps(PipeStage* st, std::uint32_t index) noexcept {
    const std::uint32_t type = st->type < 10 ? st->type : kMapTypeDefault;
    if (index == 0) {
        for (int i = 0; i < 6; ++i) {
            st->ops[i] = kFirstStageOps[type][i];
        }
        if (!st->notReplace) {
            st->ops[0] = kXTopSelectArg1;  // COLOROP
            st->ops[1] = 2;                // COLORARG1 TEXTURE
            st->ops[3] = kXTopSelectArg1;  // ALPHAOP
            st->ops[4] = 2;                // ALPHAARG1 TEXTURE
        } else if (st->texture == 0) {
            st->ops[0] = kXTopSelectArg2;
            st->ops[3] = kXTopSelectArg2;
            st->ops[2] = 0;                // COLORARG2 DIFFUSE
            st->ops[5] = 0;                // ALPHAARG2 DIFFUSE
        }
    } else {
        for (int i = 0; i < 6; ++i) {
            st->ops[i] = kLaterStageOps[type][i];
        }
    }
}

// The single pass of the pipeline (stages in order).
struct PassSetup {
    PipeStage stages[kMaxTextureStages];
    std::uint32_t count = 0;
    TexturePath path = TexturePath::SingleStage;
};

const NiTexturingMap* BaseMap(GuestPtr texturing) noexcept {
    const NiTexturingProperty* t = G<NiTexturingProperty>(texturing);
    if (!Plausible(t->maps)) {
        return nullptr;
    }
    const GuestPtr base = R32(t->maps);
    return Plausible(base) ? G<NiTexturingMap>(base) : nullptr;
}

// True if the effect state holds a projected light/shadow with the clipping flag (pipeline +0x770 != 0).
bool HasClippedTextureEffects(GuestPtr effectState) noexcept {
    if (!Plausible(effectState)) {
        return false;
    }
    const NiDynamicEffectState* es = G<NiDynamicEffectState>(effectState);
    for (GuestPtr list : {es->projectedLights, es->projectedShadows}) {
        int guard = 0;
        for (GuestPtr node = list; Plausible(node) && guard < 256; ++guard) {
            const NiListNode* n = G<NiListNode>(node);
            if (Plausible(n->item) && R8(n->item + kNiTextureEffectClipped) != 0) {
                return true;
            }
            node = n->next;
        }
    }
    return false;
}

// Default pipeline vf2 0x164360 (single-stage path 0x16437D..0x16444E, end 0x1650A8..0x1650DE) and the screen
// pipeline vf2 FUN_00160270. Multi-map/effect paths (vf2 0x164453 ff.) keep only the base map stage (P2).
void BuildPass(StateShadow& s, GuestPtr texturing, GuestPtr effectState, std::uint32_t uvSetCount, bool screen,
               PassSetup* pass) noexcept {
    const NiTexturingProperty* t = G<NiTexturingProperty>(texturing);
    const NiTexturingMap* base = BaseMap(texturing);
    const GuestPtr texture = base != nullptr ? base->texture : 0;
    const std::uint32_t clamp = base != nullptr ? base->clamp : 3;
    const std::uint32_t filter = base != nullptr ? base->filter : 0;
    // Screen pipeline: UV set 0 (FUN_00160270 pushes 0); default pipeline: the map's UV set.
    const std::uint32_t uvSet = screen ? 0 : (base != nullptr ? base->uvSet : 0);
    const bool notReplace = t->applyMode != kApplyReplace;

    if (screen) {
        pass->path = TexturePath::Screen;
    } else if (t->hasNonBaseMaps != 0 || HasClippedTextureEffects(effectState)) {
        pass->path = TexturePath::MultiMap;
        ++s.counters.multiMapApproximated;
    } else {
        pass->path = TexturePath::SingleStage;
    }

    PipeStage st;
    if (MakeMapStage(s, texture, clamp, filter, kMapTypeBase, uvSet, uvSetCount, &st)) {
        st.notReplace = notReplace;  // FUN_00163E20 succeeded: +0x90 = (applyMode != 0)
    } else {
        // Default stage +0x6A4 (ctor: no texture, type 9, +0x90 = 1; the screen pipeline sets +0x734 =
        // applyMode != 0).
        st = PipeStage{};
        st.notReplace = screen ? notReplace : true;
    }
    SetupStageOps(&st, 0);
    pass->stages[0] = st;
    pass->count = 1;
}

// Pass apply 0x15FFE0: FUN_00162020 per stage, FUN_00161CD0 for the next stage, TEXCOORDINDEX = stage after.
void ApplyPass(const ExtractContext& context, StateShadow& s, const PassSetup& pass) noexcept {
    std::uint32_t i = 0;
    for (; i < pass.count && i < kMaxTextureStages; ++i) {
        const PipeStage& st = pass.stages[i];
        // FUN_00162020: texture through the texture manager vf2 0x15F930.
        if (st.texture == 0) {
            SetTexture(s, i, 0, {});
        } else {
            const Resolved r = ResolveTexture(context, s, st.texture);
            SetTexture(s, i, r.ok ? st.texture : 0, r.handle);
            if (r.ok) {
                // FUN_00161F70: TTFF (+ D3DTS_TEXTUREn if != 0), TEXCOORDINDEX.
                SetTss(s, i, tss::TEXTURETRANSFORMFLAGS, st.ttff);
                SetTss(s, i, tss::TEXCOORDINDEX, st.texCoordIndex);
                // FUN_00161E40(stage, renderer, hasMips, 1).
                std::uint32_t f = st.filter;
                if (f > 5) {
                    ++s.counters.invalidMapModes;
                    f = 0;
                }
                if (QueryTexture(st.texture).mipLevels <= 1) {
                    if (f == 3 || f == 4) {
                        f = 0;
                    } else if (f == 5 || f == 2) {
                        f = 1;
                    }
                }
                SetTss(s, i, tss::MAGFILTER, kFilterTable[f][1]);
                SetTss(s, i, tss::MINFILTER, kFilterTable[f][0]);
                SetTss(s, i, tss::MIPFILTER, kFilterTable[f][2]);
                // FUN_00161D60 (manager +0xFC bit 0 set: independent U/V).
                SetTss(s, i, tss::ADDRESSU, st.addressU);
                SetTss(s, i, tss::ADDRESSV, st.addressV);
            }
        }
        SetTss(s, i, tss::COLOROP, st.ops[0]);
        SetTss(s, i, tss::COLORARG1, st.ops[1]);
        SetTss(s, i, tss::COLORARG2, st.ops[2]);
        SetTss(s, i, tss::ALPHAOP, st.ops[3]);
        SetTss(s, i, tss::ALPHAARG1, st.ops[4]);
        SetTss(s, i, tss::ALPHAARG2, st.ops[5]);
        SetRs(s, rs::TEXTUREFACTOR, st.textureFactor);
    }
    if (i < kMaxTextureStages) {
        // FUN_00161CD0: no texture, COLOROP = ALPHAOP = DISABLE.
        SetTexture(s, i, 0, {});
        SetTss(s, i, tss::COLOROP, kXTopDisable);
        SetTss(s, i, tss::ALPHAOP, kXTopDisable);
    }
    for (; i < kMaxTextureStages; ++i) {
        SetTss(s, i, tss::TEXCOORDINDEX, i);
    }
}

// ---- lights (FUN_0015BF80 / FUN_0015BA40 / FUN_0015B6A0) -------------------------------------------------------

// GetEffectType (vf40) by implementation address; -1 = unknown.
std::int32_t EffectType(const ExtractContext& context, GuestPtr effect) noexcept {
    const GuestPtr vtable = R32(effect);
    if (!Plausible(vtable)) {
        return -1;
    }
    const GuestPtr fn = R32(vtable + kNiDynamicEffectVfGetType);
    switch (fn) {
    case kGetEffectTypeAmbient:
        return 0;
    case kGetEffectTypeDirectional:
        return 1;
    case kGetEffectTypePoint:
        return 2;
    case kGetEffectTypeSpot:
        return 3;
    case kGetEffectTypeTextureEffect:
        return 4;
    default:
        break;
    }
    if (context.allowGuestCalls && Plausible(fn)) {
        return static_cast<std::int32_t>(core::CallGuestThiscall(fn, effect, {}));
    }
    return -1;
}

// FUN_0015B6A0: D3DLIGHT8 of a non-ambient light.
bool FillLight(std::int32_t type, GuestPtr light, LightDesc* out) noexcept {
    const NiLightLayout* l = G<NiLightLayout>(light);
    LightDesc d;
    d.niLight = light;
    d.index = l->index;
    const float* translate = l->av.world.translate;
    switch (type) {
    case 1: {  // directional
        d.type = FF_LIGHT_DIRECTIONAL;
        const float* dir = G<float>(light + kNiDirectionalLightDirection);
        d.direction = {dir[0], dir[1], dir[2]};
        break;
    }
    case 2:
    case 3: {  // point / spot
        d.type = type == 2 ? FF_LIGHT_POINT : FF_LIGHT_SPOT;
        d.position = {translate[0], translate[1], translate[2]};
        const float* att = G<float>(light + kNiPointLightAttenuation);
        d.attenuation0 = att[0];
        d.attenuation1 = att[1];
        d.attenuation2 = att[2];
        d.range = static_cast<float>(std::sqrt(static_cast<double>(std::numeric_limits<float>::max())));
        if (type == 3) {
            const float* dir = G<float>(light + kNiSpotLightDirection);
            d.direction = {dir[0], dir[1], dir[2]};
            // angle * DAT_002F91D4 (pi as float) * 0.0111111f (0x2A5FA8), x87 then float store.
            static constexpr float kPi = 3.14159265358979323846f;
            static constexpr float kOneOver90 = 0.0111111112f;  // 0x3C360B61
            const float angle = RF(light + kNiSpotLightAngle);
            d.theta = d.phi = static_cast<float>(static_cast<double>(angle) * kPi * kOneOver90);
            d.falloff = RF(light + kNiSpotLightExponent);
        }
        break;
    }
    default:
        return false;  // no D3D light type (FUN_0015B6A0 leaves Type = 0)
    }
    const float dim = l->dimmer;
    d.diffuse = {l->diffuse.r * dim, l->diffuse.g * dim, l->diffuse.b * dim, 1.0f};
    d.specular = {l->specular.r * dim, l->specular.g * dim, l->specular.b * dim, 1.0f};
    d.ambient = {l->ambient.r * dim, l->ambient.g * dim, l->ambient.b * dim, 1.0f};
    *out = d;
    return true;
}

// FUN_0015BA40: disable the previous lights, enable the effect state's lights, sum the ambient lights.
void EvaluateLights(const ExtractContext& context, StateShadow& s, GuestPtr effectState) noexcept {
    ++s.counters.lightEvaluations;
    s.lights.count = 0;
    s.lights.dropped = 0;
    float ar = 0.0f, ag = 0.0f, ab = 0.0f;
    if (s.lightsEnabled && Plausible(effectState)) {
        const NiDynamicEffectState* es = G<NiDynamicEffectState>(effectState);
        int guard = 0;
        for (GuestPtr node = es->lights; Plausible(node) && guard < 4096; ++guard) {
            const NiListNode* n = G<NiListNode>(node);
            const GuestPtr light = n->item;
            node = n->next;
            if (!Plausible(light)) {
                continue;
            }
            const NiLightLayout* l = G<NiLightLayout>(light);
            if (l->on == 0 || l->dimmer < 0.01f) {  // fcomp 0.01 / test ah,5 / jnp: NaN passes
                continue;
            }
            const std::int32_t type = EffectType(context, light);
            if (type == 0) {
                const double dim = l->dimmer;
                ar = static_cast<float>(dim * l->ambient.r + ar);
                ag = static_cast<float>(dim * l->ambient.g + ag);
                ab = static_cast<float>(dim * l->ambient.b + ab);
                continue;
            }
            LightDesc d;
            if (!FillLight(type, light, &d)) {
                ++s.counters.unknownLightTypes;
                continue;
            }
            // D3DDevice_SetLight(index) + LightEnable(index, TRUE): the same index replaces the light.
            std::uint32_t slot = 0;
            while (slot < s.lights.count && s.lights.lights[slot].index != d.index) {
                ++slot;
            }
            if (slot == s.lights.count) {
                if (s.lights.count == kMaxLights) {  // MaxActiveLights 8 (caps DAT_002D7580 +0xA0)
                    ++s.lights.dropped;
                    ++s.counters.lightsDropped;
                    continue;
                }
                ++s.lights.count;
            }
            s.lights.lights[slot] = d;
        }
    }
    const std::uint32_t packed = PackRgb(ar, ag, ab);
    if (packed != s.lastAmbient) {
        SetRs(s, rs::AMBIENT, packed);
        s.lastAmbient = packed;
    }
}

// FUN_0015BF80(effectState, texturing, vertexColor).
void ApplyLighting(const ExtractContext& context, StateShadow& s, GuestPtr effectState, GuestPtr texturing,
                   GuestPtr vertexColor) noexcept {
    const NiTexturingProperty* t = G<NiTexturingProperty>(texturing);
    const NiTexturingMap* base = BaseMap(texturing);
    const NiVertexColorProperty* vc = G<NiVertexColorProperty>(vertexColor);
    bool off = base != nullptr && base->texture != 0 && t->applyMode == kApplyReplace;
    if (!off) {
        const std::uint32_t source = vc->source;
        if (vc->lighting == 0) {  // LIGHTING_E
            if (source != 0 && source != 2) {
                off = true;
            } else {
                s.lightsEnabled = false;
                if (s.vertexColorSourceMode == 1) {
                    SetRs(s, rs::EMISSIVEMATERIALSOURCE, FF_MCS_MATERIAL);
                    s.vertexColorSourceMode = 0;
                }
            }
        } else {  // LIGHTING_E_A_D
            if (source != s.vertexColorSourceMode) {
                if (source == 0) {
                    SetRs(s, rs::EMISSIVEMATERIALSOURCE, FF_MCS_MATERIAL);
                    SetRs(s, rs::AMBIENTMATERIALSOURCE, FF_MCS_MATERIAL);
                    SetRs(s, rs::DIFFUSEMATERIALSOURCE, FF_MCS_MATERIAL);
                } else if (source == 1) {
                    SetRs(s, rs::EMISSIVEMATERIALSOURCE, FF_MCS_COLOR1);
                    SetRs(s, rs::AMBIENTMATERIALSOURCE, FF_MCS_MATERIAL);
                    SetRs(s, rs::DIFFUSEMATERIALSOURCE, FF_MCS_MATERIAL);
                } else if (source == 2) {
                    SetRs(s, rs::EMISSIVEMATERIALSOURCE, FF_MCS_MATERIAL);
                    SetRs(s, rs::AMBIENTMATERIALSOURCE, FF_MCS_COLOR1);
                    SetRs(s, rs::DIFFUSEMATERIALSOURCE, FF_MCS_COLOR1);
                }
                s.vertexColorSourceMode = source;
            }
            s.lightsEnabled = true;
        }
    }
    if (off) {
        if (s.lightingOn) {
            SetRs(s, rs::LIGHTING, 0);
            s.lightingOn = false;
        }
        return;
    }
    if (!s.lightingOn) {
        SetRs(s, rs::LIGHTING, 1);
        s.lightingOn = true;
    }
    if (effectState == s.lastEffectState && s.lightsEnabled == s.lastLightsEnabled) {
        return;  // the D3D lights of the previous evaluation stay
    }
    EvaluateLights(context, s, effectState);
    s.lastEffectState = effectState;
    s.lastLightsEnabled = s.lightsEnabled;
}

// ---- device state -> RenderStateDesc ---------------------------------------------------------------------------

rhi::BlendFactor BlendFrom(std::uint32_t v) noexcept {
    switch (v) {
    case 0:
        return rhi::BlendFactor::Zero;
    case 1:
        return rhi::BlendFactor::One;
    case 0x300:
        return rhi::BlendFactor::SrcColor;
    case 0x301:
        return rhi::BlendFactor::InvSrcColor;
    case 0x302:
        return rhi::BlendFactor::SrcAlpha;
    case 0x303:
        return rhi::BlendFactor::InvSrcAlpha;
    case 0x304:
        return rhi::BlendFactor::DestAlpha;
    case 0x305:
        return rhi::BlendFactor::InvDestAlpha;
    case 0x306:
        return rhi::BlendFactor::DestColor;
    case 0x307:
        return rhi::BlendFactor::InvDestColor;
    case 0x308:
        return rhi::BlendFactor::SrcAlphaSat;
    default:
        return rhi::BlendFactor::One;  // e.g. table entries 11..15 (D3DCMP values): not a blend factor
    }
}
rhi::CompareFunc CompareFrom(std::uint32_t v) noexcept {
    return v >= kCmpNever && v <= kCmpAlways ? static_cast<rhi::CompareFunc>(v - kCmpNever)
                                              : rhi::CompareFunc::Always;
}
rhi::StencilOp StencilOpFrom(std::uint32_t v) noexcept {
    switch (v) {
    case 0x1E00:
        return rhi::StencilOp::Keep;
    case 0:
        return rhi::StencilOp::Zero;
    case 0x1E01:
        return rhi::StencilOp::Replace;
    case 0x1E02:
        return rhi::StencilOp::IncrSat;
    case 0x1E03:
        return rhi::StencilOp::DecrSat;
    case 0x150A:
        return rhi::StencilOp::Invert;
    case 0x8507:
        return rhi::StencilOp::Incr;
    case 0x8508:
        return rhi::StencilOp::Decr;
    default:
        return rhi::StencilOp::Keep;
    }
}
rhi::AddressMode AddressFrom(std::uint32_t v) noexcept {
    switch (v) {
    case 2:
        return rhi::AddressMode::Mirror;
    case 3:
    case 5:  // CLAMPTOEDGE
        return rhi::AddressMode::Clamp;
    case 4:
        return rhi::AddressMode::Border;
    default:
        return rhi::AddressMode::Wrap;
    }
}
rhi::Filter FilterFrom(std::uint32_t v) noexcept {
    return v >= 2 ? rhi::Filter::Linear : rhi::Filter::Point;  // NONE/POINT -> point; LINEAR, ANISO.. -> linear
}
rhi::MipFilter MipFrom(std::uint32_t v) noexcept {
    return v == 0 ? rhi::MipFilter::None : (v == 1 ? rhi::MipFilter::Point : rhi::MipFilter::Linear);
}
std::uint8_t Arg(std::uint32_t v) noexcept {
    return static_cast<std::uint8_t>(v & 0x3Fu);  // D3DTA select (0..5) + COMPLEMENT/ALPHAREPLICATE
}
std::uint8_t Source(std::uint32_t v) noexcept {
    return static_cast<std::uint8_t>(v <= 2 ? v : 0);
}

void DeriveState(const StateShadow& s, TexturePath path, RenderStateDesc* out) noexcept {
    RenderStateDesc d;
    d.alphaBlendEnable = DevRs(s, rs::ALPHABLENDENABLE) != 0;
    d.srcBlend = BlendFrom(DevRs(s, rs::SRCBLEND));
    d.dstBlend = BlendFrom(DevRs(s, rs::DESTBLEND));
    d.alphaTestEnable = DevRs(s, rs::ALPHATESTENABLE) != 0;
    d.alphaFunc = CompareFrom(DevRs(s, rs::ALPHAFUNC));
    d.alphaRef = static_cast<std::uint8_t>(DevRs(s, rs::ALPHAREF));
    d.zFunc = DevRs(s, rs::ZENABLE) != 0 ? CompareFrom(DevRs(s, rs::ZFUNC)) : rhi::CompareFunc::Always;
    d.zWriteEnable = DevRs(s, rs::ZENABLE) != 0 && DevRs(s, rs::ZWRITEENABLE) != 0;
    d.stencilEnable = DevRs(s, rs::STENCILENABLE) != 0;
    d.stencilFunc = CompareFrom(DevRs(s, rs::STENCILFUNC));
    d.stencilRef = static_cast<std::uint8_t>(DevRs(s, rs::STENCILREF));
    d.stencilMask = static_cast<std::uint8_t>(DevRs(s, rs::STENCILMASK));
    d.stencilFail = StencilOpFrom(DevRs(s, rs::STENCILFAIL));
    d.stencilZFail = StencilOpFrom(DevRs(s, rs::STENCILZFAIL));
    d.stencilPass = StencilOpFrom(DevRs(s, rs::STENCILPASS));
    const std::uint32_t cull = DevRs(s, rs::CULLMODE);
    d.cullMode = cull == kCullCW ? rhi::CullMode::CW : (cull == kCullCCW ? rhi::CullMode::CCW : rhi::CullMode::None);
    const std::uint32_t fill = DevRs(s, rs::FILLMODE);
    d.fillMode = (fill == kFillWireframe || fill == kFillPoint) ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    d.shadeMode = DevRs(s, rs::SHADEMODE) == kShadeFlat ? ShadeMode::Flat : ShadeMode::Gouraud;
    d.ditherEnable = DevRs(s, rs::DITHERENABLE) != 0;
    d.specularEnable = DevRs(s, rs::SPECULARENABLE) != 0;
    d.normalizeNormals = DevRs(s, rs::NORMALIZENORMALS) != 0;
    d.fogEnable = DevRs(s, rs::FOGENABLE) != 0;
    d.fogTableMode = static_cast<std::uint8_t>(DevRs(s, rs::FOGTABLEMODE) & 3u);
    d.rangeFogEnable = DevRs(s, rs::RANGEFOGENABLE) != 0;
    d.fogStart = BitsFloat(DevRs(s, rs::FOGSTART));
    d.fogEnd = BitsFloat(DevRs(s, rs::FOGEND));
    d.fogDensity = BitsFloat(DevRs(s, rs::FOGDENSITY));
    const Float4 fogColor = UnpackArgb(DevRs(s, rs::FOGCOLOR));
    d.fogColor = {fogColor.x, fogColor.y, fogColor.z};
    d.lightingEnable = DevRs(s, rs::LIGHTING) != 0;
    // D3DRS_COLORVERTEX = FALSE would make every source MATERIAL (REN sets it to TRUE at init).
    const bool colorVertex = DevRs(s, rs::COLORVERTEX) != 0;
    d.diffuseSource = colorVertex ? Source(DevRs(s, rs::DIFFUSEMATERIALSOURCE)) : FF_MCS_MATERIAL;
    d.ambientSource = colorVertex ? Source(DevRs(s, rs::AMBIENTMATERIALSOURCE)) : FF_MCS_MATERIAL;
    d.specularSource = colorVertex ? Source(DevRs(s, rs::SPECULARMATERIALSOURCE)) : FF_MCS_MATERIAL;
    d.emissiveSource = colorVertex ? Source(DevRs(s, rs::EMISSIVEMATERIALSOURCE)) : FF_MCS_MATERIAL;
    d.localViewer = DevRs(s, rs::LOCALVIEWER) != 0;
    d.material = s.material;
    const Float4 factor = UnpackArgb(DevRs(s, rs::TEXTUREFACTOR));
    d.textureFactor = factor;
    d.stageCount = 0;
    bool counting = true;
    for (std::uint32_t i = 0; i < kMaxTextureStages; ++i) {
        TextureStageDesc& st = d.stages[i];
        const std::uint32_t* t = s.devTss[i];
        st.niTexture = s.boundNiTexture[i];
        st.texture = s.boundTexture[i];
        st.colorOp = TextureOpFromXbox(t[tss::COLOROP]);
        st.colorArg1 = Arg(t[tss::COLORARG1]);
        st.colorArg2 = Arg(t[tss::COLORARG2]);
        st.colorArg0 = Arg(t[tss::COLORARG0]);
        st.alphaOp = TextureOpFromXbox(t[tss::ALPHAOP]);
        st.alphaArg1 = Arg(t[tss::ALPHAARG1]);
        st.alphaArg2 = Arg(t[tss::ALPHAARG2]);
        st.alphaArg0 = Arg(t[tss::ALPHAARG0]);
        st.resultArg = Arg(t[tss::RESULTARG]);
        st.texCoordIndex = t[tss::TEXCOORDINDEX];
        st.addressU = AddressFrom(t[tss::ADDRESSU]);
        st.addressV = AddressFrom(t[tss::ADDRESSV]);
        st.minFilter = FilterFrom(t[tss::MINFILTER]);
        st.magFilter = FilterFrom(t[tss::MAGFILTER]);
        st.mipFilter = MipFrom(t[tss::MIPFILTER]);
        st.transformFlags = t[tss::TEXTURETRANSFORMFLAGS];
        st.transform = s.textureTransform[i];
        if (counting && st.colorOp != FF_TOP_DISABLE) {
            ++d.stageCount;
        } else {
            counting = false;
        }
    }
    d.passCount = 1;
    d.texturePath = path;
    *out = d;
}

void CaptureEffects(StateShadow& s, GuestPtr effectState, TextureEffectSlots* out) noexcept {
    TextureEffectSlots e;
    if (Plausible(effectState)) {
        const NiDynamicEffectState* es = G<NiDynamicEffectState>(effectState);
        int guard = 0;
        for (GuestPtr node = es->projectedLights; Plausible(node) && guard < 256; ++guard) {
            const NiListNode* n = G<NiListNode>(node);
            if (e.projectedLightCount < 2) {
                e.projectedLights[e.projectedLightCount] = n->item;
            }
            ++e.projectedLightCount;
            node = n->next;
        }
        guard = 0;
        for (GuestPtr node = es->projectedShadows; Plausible(node) && guard < 256; ++guard) {
            const NiListNode* n = G<NiListNode>(node);
            if (e.projectedShadowCount < 2) {
                e.projectedShadows[e.projectedShadowCount] = n->item;
            }
            ++e.projectedShadowCount;
            node = n->next;
        }
        e.environmentMap = es->environmentMap;
        e.fogMap = es->fogMap;
        if (e.projectedLightCount != 0 || e.projectedShadowCount != 0 || e.environmentMap != 0 || e.fogMap != 0) {
            ++s.counters.textureEffectDraws;
        }
    }
    if (out != nullptr) {
        *out = e;
    }
}

}  // namespace

ExtractStatus ExtractState(const ExtractContext& context, const StateInput& input, RenderStateDesc* state,
                           LightSetDesc* lights, TextureEffectSlots* effects) noexcept {
    Props p;
    if (!ReadProps(input.propertyState, &p)) {
        return ExtractStatus::Error;
    }
    std::optional<StateShadow> local;  // every draw from the defaults when the caller keeps no shadow
    if (context.shadow == nullptr) {
        local.emplace();
    }
    StateShadow& s = context.shadow != nullptr ? *context.shadow : *local;
    const CameraDesc camera = context.camera != nullptr ? *context.camera
                                                        : DefaultCamera(context.targetWidth, context.targetHeight);

    // FUN_00159CD0
    ApplyProperties(s, p, camera, context.leftRightSwap);
    // pipeline vf2 (stages of the pass; FUN_00164200 effect bookkeeping only matters for the P2 paths)
    PassSetup pass;
    BuildPass(s, p.texturing, input.effectState, input.uvSetCount, input.screenTexture, &pass);
    // VertexBlend (rs+0xE8 = 0, FUN_00158DE0) - no visible state.
    SetRs(s, rs::VERTEXBLEND, 0);
    // FUN_001489A0 -> FUN_00159200 (not for the screen pipeline: flags 0x800000)
    if (!input.screenTexture) {
        ApplyNormalize(s, input.worldScale);
    }
    // FUN_0015BF80
    ApplyLighting(context, s, input.effectState, p.texturing, p.vertexColor);
    // pass 1: vf3 0x1628B0 -> pass apply 0x15FFE0
    ApplyPass(context, s, pass);

    ++s.counters.states;
    if (state != nullptr) {
        DeriveState(s, pass.path, state);
    }
    {  // TEMPORARY PROBE
        auto& g = probe::g;
        ++g.states;
        g.path[static_cast<int>(pass.path)]++;
        const NiTexturingProperty* tp = G<NiTexturingProperty>(p.texturing);
        g.apply[tp->applyMode < 3 ? tp->applyMode : 3]++;
        const NiVertexColorProperty* vcp = G<NiVertexColorProperty>(p.vertexColor);
        g.vc[vcp->source < 3 ? vcp->source : 3][vcp->lighting != 0 ? 1 : 0]++;
        RenderStateDesc d;
        DeriveState(s, pass.path, &d);
        if (d.lightingEnable) { g.lightingOn++; g.lightsHist[s.lights.count < 9 ? s.lights.count : 8]++; if ((DevRs(s, rs::AMBIENT) & 0xFFFFFF) != 0) g.ambientNonZero++; }
        if (input.hasNormals || true) {}
        if (d.alphaBlendEnable) g.blend++;
        if (d.alphaTestEnable) g.atest++;
        if (d.zFunc != rhi::CompareFunc::Always) g.ztest++;
        if (d.zWriteEnable) g.zwrite++;
        g.cull[static_cast<int>(d.cullMode)]++;
        if (d.fogEnable) g.fog++;
        if (d.specularEnable) g.specular++;
        if (d.stencilEnable) g.stencil++;
        if (d.fillMode == rhi::FillMode::Wireframe) g.wire++;
        if (d.shadeMode == ShadeMode::Flat) g.flat++;
        if (d.stages[0].niTexture != 0) { g.stage0Tex++; if (QueryTexture(d.stages[0].niTexture).mipLevels > 1) g.mips++; }
        const NiTexturingMap* bm = BaseMap(p.texturing);
        if (bm != nullptr) { g.filter[bm->filter < 8 ? bm->filter : 7]++; g.clamp[bm->clamp < 4 ? bm->clamp : 4]++; }
        if (Plausible(tp->maps)) { unsigned mask = 0; for (std::uint32_t i = 0; i < tp->mapsSize && i < 7; ++i) if (R32(tp->maps + 4 * i)) mask |= 1u << i; g.mapsMask[mask & 127]++; }
        if (Plausible(input.effectState)) { const NiDynamicEffectState* es = G<NiDynamicEffectState>(input.effectState); if (es->projectedLights || es->projectedShadows || es->environmentMap || es->fogMap) g.effects++; }
        if (g.states == 20000 || g.states == 60000 || g.states % 100000 == 0) probe::Dump();
    }
    if (lights != nullptr) {
        *lights = s.lights;
        lights->ambient = UnpackArgb(DevRs(s, rs::AMBIENT));
        lights->ambient.w = 1.0f;
    }
    CaptureEffects(s, input.effectState, effects);
    return ExtractStatus::Ok;
}

namespace {

// ---- geometry -------------------------------------------------------------------------------------------------

// GetVertexCount (vf12) / GetTriangleCount (vf15) of a geometry data object by implementation address.
bool GeometryCounts(const ExtractContext& context, GuestPtr data, std::uint32_t* vertices,
                    std::uint32_t* triangles) noexcept {
    const GuestPtr vtable = R32(data);
    if (!Plausible(vtable)) {
        return false;
    }
    const GuestPtr vf12 = R32(vtable + kNiGeometryDataVfVertexCount);
    const GuestPtr vf15 = R32(vtable + kNiGeometryDataVfTriangleCount);
    switch (vf12) {
    case kVertexCountGeometryData:
        *vertices = R16(data + offsetof(NiGeometryData, vertexCount));
        break;
    case kVertexCountDynamicData:
        *vertices = R16(data + offsetof(NiTriShapeDynamicData, activeVertices));
        break;
    case kVertexCountTrianglesData:
        *vertices = static_cast<std::uint32_t>(R16(data + offsetof(NiTrianglesData, activeTriangles))) * 3u;
        break;
    case kVertexCountTriStripData: {
        const std::uint32_t active = R16(data + offsetof(NiTriStripData, activeTriangles));
        *vertices = active != 0 ? active + 2u : 0u;
        break;
    }
    default:
        if (!context.allowGuestCalls || !Plausible(vf12)) {
            return false;
        }
        *vertices = core::CallGuestThiscall(vf12, data, {}) & 0xFFFFu;
        break;
    }
    switch (vf15) {
    case kTriangleCountTriBased:
        *triangles = R16(data + offsetof(NiTriBasedGeomData, triangleCount));
        break;
    case kTriangleCountDynamicData:
        *triangles = R16(data + offsetof(NiTriShapeDynamicData, activeTriangles));
        break;
    case kTriangleCountActive:
        *triangles = R16(data + offsetof(NiTrianglesData, activeTriangles));
        break;
    default:
        if (!context.allowGuestCalls || !Plausible(vf15)) {
            return false;
        }
        *triangles = core::CallGuestThiscall(vf15, data, {}) & 0xFFFFu;
        break;
    }
    return true;
}

// Zero-copy vertex streams of a NiGeometryData (FUN_00157C00 copies `vertexCount` vertices of each array;
// UV set i at uvs + i * (+0x08) * 8, FUN_001437B0; at most DAT_002D85D0 = 4 sets).
void FillStreams(GuestPtr data, std::uint32_t vertexCount, GeometryDesc* g) noexcept {
    const NiGeometryData* gd = G<NiGeometryData>(data);
    g->vertexCount = vertexCount;
    g->positions = {G<float>(gd->vertices), 12};
    g->normals = Plausible(gd->normals) ? VertexStreamView{G<float>(gd->normals), 12} : VertexStreamView{};
    g->colors = Plausible(gd->colors) ? VertexStreamView{G<float>(gd->colors), 16} : VertexStreamView{};
    const std::uint32_t sets = gd->uvSetCount < kMaxUvSets ? gd->uvSetCount : kMaxUvSets;
    g->uvSetCount = sets;
    for (std::uint32_t i = 0; i < kMaxUvSets; ++i) {
        g->uvs[i] = {};
    }
    if (Plausible(gd->uvs)) {
        for (std::uint32_t i = 0; i < sets; ++i) {
            g->uvs[i] = {G<float>(gd->uvs + gd->vertexCount * i * 8u), 8};
        }
    }
    g->guestData = data;
    g->dataId = gd->id;
    g->dataRevision = gd->revision;
}

bool IndicesInRange(const std::uint16_t* indices, std::uint32_t count, std::uint32_t vertexCount,
                    bool allowCut) noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t v = indices[i];
        if (v >= vertexCount && !(allowCut && v == rhi::kStripCut)) {
            return false;
        }
    }
    return true;
}

// FUN_00157C00 skinning: FUN_0014F710 (thiscall, ecx = skin instance; srcPositions, srcNormals, u16
// vertexCount (+0x08, not vf12), dstPositions, dstNormals or 0, dstStride) once per skin instance and frame
// (skin +0x14 frame stamp vs VB manager frame counter). The output lands in the arena.
// Deviation: the cache key includes the geometry data (the original would leave the second geometry of a
// shared skin instance undeformed within a frame).
ExtractStatus Skin(const ExtractContext& context, StateShadow& s, GuestPtr skin, GuestPtr data,
                   GeometryDesc* g) noexcept {
    ++s.counters.skinnedDraws;
    if (!context.allowGuestCalls) {
        return ExtractStatus::Unsupported;
    }
    const NiGeometryData* gd = G<NiGeometryData>(data);
    const std::uint32_t count = gd->vertexCount;
    const bool hasNormals = Plausible(gd->normals);
    for (const StateShadow::SkinEntry& e : s.skinCache) {
        if (e.skin == skin && e.data == data && e.frame == context.frame && e.vertexCount == count &&
            e.vertices != nullptr) {
            ++s.counters.skinCacheHits;
            g->positions = {e.vertices, e.stride};
            g->normals = e.hasNormals ? VertexStreamView{e.vertices + 3, e.stride} : VertexStreamView{};
            g->skinned = true;
            return ExtractStatus::Ok;
        }
    }
    const std::uint32_t stride = hasNormals ? 24u : 12u;
    auto* out = static_cast<float*>(context.arena->Allocate(static_cast<std::size_t>(count) * stride + 16));
    if (out == nullptr) {
        return ExtractStatus::Error;
    }
    ++s.counters.skinDeforms;
    core::CallGuestThiscall(kNiSkinInstanceDeform, skin,
                            {gd->vertices, hasNormals ? gd->normals : 0u, count, GuestAddr(out),
                             hasNormals ? GuestAddr(out + 3) : 0u, stride});
    StateShadow::SkinEntry& e = s.skinCache[s.skinCacheNext];
    s.skinCacheNext = (s.skinCacheNext + 1) % StateShadow::kSkinCacheSize;
    e = {skin, data, context.frame, count, out, stride, hasNormals};
    g->positions = {out, stride};
    g->normals = hasNormals ? VertexStreamView{out + 3, stride} : VertexStreamView{};
    g->skinned = true;
    return ExtractStatus::Ok;
}

// Common part of slots 48-51: transform, state, counts. `vertexCount` = vf12, streams from the data.
ExtractStatus FinishGeometry(const ExtractContext& context, const GeometryInput& input, DrawKind kind,
                             std::uint32_t vertexCount, DrawItem* out) noexcept {
    if (!Plausible(input.transform)) {
        return ExtractStatus::Error;
    }
    const NiTransform* t = G<NiTransform>(input.transform);
    out->kind = kind;
    out->guestObject = input.data;
    out->world = NiTransformToWorld(&t->rotate[0][0], t->translate, t->scale);
    out->worldScale = t->scale;
    out->useScreenCamera = false;
    FillStreams(input.data, vertexCount, &out->geometry);
    std::optional<StateShadow> local;
    ExtractContext ctx = context;
    if (ctx.shadow == nullptr) {
        ctx.shadow = &local.emplace();
    }
    if (input.skinInstance != 0) {
        if (!Plausible(input.skinInstance)) {
            return ExtractStatus::Error;
        }
        const ExtractStatus st = Skin(ctx, *ctx.shadow, input.skinInstance, input.data, &out->geometry);
        if (st != ExtractStatus::Ok) {
            return st;
        }
    }
    StateInput si;
    si.propertyState = input.propertyState;
    si.effectState = input.effectState;
    si.worldScale = t->scale;
    si.hasNormals = out->geometry.normals.data != nullptr;
    si.uvSetCount = out->geometry.uvSetCount;
    const ExtractStatus res = ExtractState(ctx, si, &out->state, &out->lights, &out->effects);
    if (out->geometry.skinned && Plausible(input.bound)) {  // TEMPORARY PROBE: space of the skinned vertices
        static int logged = 0;
        if (logged < 6) {
            ++logged;
            const NiBound* b = G<NiBound>(input.bound);
            const auto* pp = static_cast<const float*>(out->geometry.positions.data);
            const std::uint32_t stride = out->geometry.positions.stride / 4;
            double maxWorld = 0, maxRaw = 0, maxSrc = 0;
            const float* src = G<float>(G<NiGeometryData>(input.data)->vertices);
            for (std::uint32_t v = 0; v < out->geometry.vertexCount; ++v) {
                const Float3 raw{pp[v * stride], pp[v * stride + 1], pp[v * stride + 2]};
                const Float3 w = TransformPoint(raw, out->world);
                const Float3 sw = TransformPoint(Float3{src[v * 3], src[v * 3 + 1], src[v * 3 + 2]}, out->world);
                auto dist = [&](const Float3& q) { const double dx = q.x - b->center[0], dy = q.y - b->center[1], dz = q.z - b->center[2]; return std::sqrt(dx * dx + dy * dy + dz * dz); };
                maxWorld = std::fmax(maxWorld, dist(w));
                maxRaw = std::fmax(maxRaw, dist(raw));
                maxSrc = std::fmax(maxSrc, dist(sw));
            }
            FUT_LOGI(Gfx, "PROBE skin 0x%08X data 0x%08X: %u vertices, world bound c=(%.2f %.2f %.2f) r=%.2f; max dist: skinned*world %.2f, skinned raw %.2f, unskinned*world %.2f; scale %.3f t=(%.2f %.2f %.2f)",
                     input.skinInstance, input.data, out->geometry.vertexCount, b->center[0], b->center[1], b->center[2], b->radius, maxWorld, maxRaw, maxSrc, out->worldScale, out->world.m[3][0], out->world.m[3][1], out->world.m[3][2]);
        }
    }
    {  // TEMPORARY PROBE
        if (out->geometry.colors.data != nullptr) {
            probe::g.withColors++;
            if (out->state.lightingEnable && out->state.diffuseSource == FF_MCS_COLOR1) probe::g.diffuseColor1WithColors++;
            if (!out->state.lightingEnable) probe::g.other += 0;
        }
    }
    return res;
}

bool ValidData(GuestPtr data) noexcept {
    if (!Plausible(data)) {
        return false;
    }
    return Plausible(G<NiGeometryData>(data)->vertices);
}

}  // namespace

ExtractStatus ExtractShape(const ExtractContext& context, const GeometryInput& input, DrawItem* out) noexcept {
    if (out == nullptr || context.arena == nullptr || !Plausible(input.data)) {
        return ExtractStatus::Error;
    }
    std::uint32_t vertices = 0, triangles = 0;
    if (!GeometryCounts(context, input.data, &vertices, &triangles)) {
        return ExtractStatus::Unsupported;
    }
    if (vertices == 0) {
        return ExtractStatus::Empty;  // slot 48 0x14C513: vf12() == 0 -> nothing
    }
    if (!ValidData(input.data)) {
        return ExtractStatus::Error;
    }
    const NiTriShapeData* sd = G<NiTriShapeData>(input.data);
    {  // TEMPORARY PROBE
        const GuestPtr vt = R32(input.data);
        if (vt == kNiTriShapeDataVtable) probe::g.shape++; else if (vt == kNiTriShapeDynamicDataVtable) probe::g.dyn++; else probe::g.other++;
        if (input.skinInstance) probe::g.skinned++;
    }
    if (vertices > sd->base.base.vertexCount) {
        return ExtractStatus::Error;
    }
    const std::uint32_t indexCount = triangles * 3u;  // FUN_00146F20: IB of vf15 * 3 indices from +0x3C
    if (indexCount == 0) {
        return ExtractStatus::Empty;
    }
    if (!Plausible(sd->triList)) {
        return ExtractStatus::Error;
    }
    const auto* indices = G<std::uint16_t>(sd->triList);
    if (!IndicesInRange(indices, indexCount, vertices, false)) {
        return ExtractStatus::Error;
    }
    *out = DrawItem{};
    GeometryDesc& g = out->geometry;
    g.topology = Topology::TriangleList;
    g.indices = indices;
    g.indexCount = indexCount;
    g.primitiveCount = triangles;
    const ExtractStatus st = FinishGeometry(context, input, DrawKind::Shape, vertices, out);
    return st;
}

ExtractStatus ExtractStrips(const ExtractContext& context, const GeometryInput& input, DrawItem* out) noexcept {
    if (out == nullptr || context.arena == nullptr || !Plausible(input.data)) {
        return ExtractStatus::Error;
    }
    std::uint32_t vertices = 0, triangles = 0;
    if (!GeometryCounts(context, input.data, &vertices, &triangles)) {
        return ExtractStatus::Unsupported;
    }
    if (vertices == 0) {
        return ExtractStatus::Empty;
    }
    if (!ValidData(input.data)) {
        return ExtractStatus::Error;
    }
    const NiTriStripsData* sd = G<NiTriStripsData>(input.data);
    if (vertices > sd->base.base.vertexCount) {
        return ExtractStatus::Error;
    }
    const std::uint32_t stripCount = sd->stripCount;
    if (stripCount == 0) {
        return ExtractStatus::Empty;
    }
    if (!Plausible(sd->stripLengths) || !Plausible(sd->strips)) {
        return ExtractStatus::Error;
    }
    const auto* lengths = G<std::uint16_t>(sd->stripLengths);
    const auto* src = G<std::uint16_t>(sd->strips);
    std::uint32_t total = 0, primitives = 0;
    for (std::uint32_t i = 0; i < stripCount; ++i) {
        total += lengths[i];
        primitives += lengths[i] >= 3 ? lengths[i] - 2u : 0u;
    }
    if (primitives == 0) {
        return ExtractStatus::Empty;
    }
    if (!IndicesInRange(src, total, vertices, false)) {
        return ExtractStatus::Error;
    }
    *out = DrawItem{};
    GeometryDesc& g = out->geometry;
    g.topology = Topology::TriangleStrip;
    g.primitiveCount = primitives;
    if (stripCount == 1) {
        g.indices = src;  // zero copy
        g.indexCount = total;
    } else {
        // The original draws each strip with its own DrawIndexedVertices (0x149C9C loop); strips joined by
        // 0xFFFF restart the winding the same way.
        const std::uint32_t count = total + stripCount - 1;
        auto* joined = context.arena->AllocateArray<std::uint16_t>(count);
        if (joined == nullptr) {
            return ExtractStatus::Error;
        }
        std::uint32_t w = 0, r = 0;
        for (std::uint32_t i = 0; i < stripCount; ++i) {
            if (i != 0) {
                joined[w++] = rhi::kStripCut;
            }
            std::memcpy(joined + w, src + r, lengths[i] * sizeof(std::uint16_t));
            w += lengths[i];
            r += lengths[i];
        }
        g.indices = joined;
        g.indexCount = w;
    }
    return FinishGeometry(context, input, DrawKind::TriStrips, vertices, out);
}

ExtractStatus ExtractTriangles(const ExtractContext& context, const GeometryInput& input, DrawItem* out) noexcept {
    if (out == nullptr || context.arena == nullptr || !Plausible(input.data)) {
        return ExtractStatus::Error;
    }
    std::uint32_t vertices = 0, triangles = 0;
    if (!GeometryCounts(context, input.data, &vertices, &triangles)) {
        return ExtractStatus::Unsupported;
    }
    if (vertices == 0 || triangles == 0) {
        return ExtractStatus::Empty;  // slot 49 0x14B5AF
    }
    if (!ValidData(input.data) || vertices > G<NiGeometryData>(input.data)->vertexCount ||
        triangles * 3u > vertices) {
        return ExtractStatus::Error;
    }
    *out = DrawItem{};
    GeometryDesc& g = out->geometry;
    g.topology = Topology::TriangleList;  // DrawVertices(TRIANGLELIST, 3 * vf15)
    g.primitiveCount = triangles;
    const ExtractStatus st = FinishGeometry(context, GeometryInput{input.data, 0, input.transform, input.bound,
                                                                   input.propertyState, input.effectState},
                                            DrawKind::Triangles, vertices, out);
    out->geometry.vertexCount = triangles * 3u;
    return st;
}

ExtractStatus ExtractTriStrip(const ExtractContext& context, const GeometryInput& input, DrawItem* out) noexcept {
    if (out == nullptr || context.arena == nullptr || !Plausible(input.data)) {
        return ExtractStatus::Error;
    }
    std::uint32_t vertices = 0, triangles = 0;
    if (!GeometryCounts(context, input.data, &vertices, &triangles)) {
        return ExtractStatus::Unsupported;
    }
    if (vertices == 0 || triangles == 0) {
        return ExtractStatus::Empty;
    }
    if (!ValidData(input.data) || vertices > G<NiGeometryData>(input.data)->vertexCount ||
        triangles + 2u > vertices) {
        return ExtractStatus::Error;
    }
    *out = DrawItem{};
    GeometryDesc& g = out->geometry;
    g.topology = Topology::TriangleStrip;  // DrawVertices(TRIANGLESTRIP, vf15 + 2)
    g.primitiveCount = triangles;
    const ExtractStatus st = FinishGeometry(context, GeometryInput{input.data, 0, input.transform, input.bound,
                                                                   input.propertyState, input.effectState},
                                            DrawKind::TriStrip, vertices, out);
    out->geometry.vertexCount = triangles + 2u;
    return st;
}

ExtractStatus ExtractScreenPolygon(const ExtractContext& context, GuestPtr screenPolygon, DrawItem* out) noexcept {
    if (out == nullptr || context.arena == nullptr || !Plausible(screenPolygon)) {
        return ExtractStatus::Error;
    }
    const NiScreenPolygon* poly = G<NiScreenPolygon>(screenPolygon);
    const std::uint32_t n = poly->vertexCount;
    if (n == 0) {
        return ExtractStatus::Empty;  // slot 54 0x14B60B
    }
    if (!Plausible(poly->vertices)) {
        return ExtractStatus::Error;
    }
    *out = DrawItem{};
    out->kind = DrawKind::ScreenPolygon;
    out->guestObject = screenPolygon;
    out->world = IdentityMatrix();
    out->worldScale = 1.0f;
    out->useScreenCamera = true;
    const CameraDesc camera = context.camera != nullptr ? *context.camera
                                                        : DefaultCamera(context.targetWidth, context.targetHeight);
    // FUN_001585A0: (d*x, d*y, d) with d = range * z + near (+0x350, +0x34C).
    auto* pos = context.arena->AllocateArray<float>(static_cast<std::size_t>(n) * 3u);
    if (pos == nullptr) {
        return ExtractStatus::Error;
    }
    const float* src = G<float>(poly->vertices);
    for (std::uint32_t i = 0; i < n; ++i) {
        // x87: d stays in a register for the three stores.
        const double d = static_cast<double>(camera.depthRange) * src[i * 3 + 2] + camera.nearPlane;
        pos[i * 3 + 0] = static_cast<float>(d * src[i * 3 + 0]);
        pos[i * 3 + 1] = static_cast<float>(d * src[i * 3 + 1]);
        pos[i * 3 + 2] = static_cast<float>(d);
    }
    GeometryDesc& g = out->geometry;
    g.vertexCount = n;
    g.positions = {pos, 12};
    g.colors = Plausible(poly->colors) ? VertexStreamView{G<float>(poly->colors), 16} : VertexStreamView{};
    g.uvSetCount = Plausible(poly->uvs) ? 1u : 0u;  // renderer +0x694 bits 24..: 1 UV set if +0x14
    if (g.uvSetCount != 0) {
        g.uvs[0] = {G<float>(poly->uvs), 8};
    }
    g.guestData = screenPolygon;
    g.primitiveCount = n >= 3 ? n - 2u : 0u;  // +0x6C0 = count - 2
    if (n < 3) {
        return ExtractStatus::Empty;
    }
    if (poly->isStrip != 0) {
        g.topology = Topology::TriangleStrip;  // prim type 6 (FUN_00149F30: 7 - (+0x1E != 0))
    } else {
        // Fan (prim type 7) -> list (0, i, i + 1): same winding as D3D's fan triangles.
        auto* idx = context.arena->AllocateArray<std::uint16_t>(static_cast<std::size_t>(n - 2) * 3u);
        if (idx == nullptr) {
            return ExtractStatus::Error;
        }
        for (std::uint32_t i = 1; i + 1 < n; ++i) {
            idx[(i - 1) * 3 + 0] = 0;
            idx[(i - 1) * 3 + 1] = static_cast<std::uint16_t>(i);
            idx[(i - 1) * 3 + 2] = static_cast<std::uint16_t>(i + 1);
        }
        g.topology = Topology::TriangleList;
        g.indices = idx;
        g.indexCount = (n - 2) * 3u;
    }
    StateInput si;
    si.propertyState = poly->propertyState;  // FUN_00106AF0: renderer +0x0C = polygon +0x08, +0x10 = 0
    si.effectState = 0;
    si.worldScale = 1.0f;
    si.hasNormals = false;
    si.uvSetCount = g.uvSetCount;
    const ExtractStatus res = ExtractState(context, si, &out->state, &out->lights, &out->effects);
    {  // TEMPORARY PROBE
        static int logged = 0;
        if (logged < 6) {
            ++logged;
            const float* c4 = static_cast<const float*>(g.colors.data);
            FUT_LOGI(Gfx, "PROBE screenpoly 0x%08X: %u verts strip %u src0 (%.3f %.3f %.3f) pos0 (%.3f %.3f %.3f) colors %d (%.2f %.2f %.2f %.2f) uvs %u near %.3f range %.3f; lighting %d vc %u/%u stage0 op %u tex 0x%08X blend %d z %u cull %u",
                     screenPolygon, n, poly->isStrip, src[0], src[1], src[2], pos[0], pos[1], pos[2], c4 != nullptr, c4 ? c4[0] : 0.f, c4 ? c4[1] : 0.f, c4 ? c4[2] : 0.f, c4 ? c4[3] : 0.f, g.uvSetCount, camera.nearPlane, camera.depthRange,
                     out->state.lightingEnable, G<NiVertexColorProperty>(G<NiPropertyState>(poly->propertyState)->vertexColor)->source, G<NiVertexColorProperty>(G<NiPropertyState>(poly->propertyState)->vertexColor)->lighting,
                     out->state.stages[0].colorOp, out->state.stages[0].niTexture, out->state.alphaBlendEnable, static_cast<unsigned>(out->state.zFunc), static_cast<unsigned>(out->state.cullMode));
        }
    }
    return res;
}

ExtractStatus ExtractScreenTexture(const ExtractContext& context, GuestPtr screenTexture, DrawItem* out) noexcept {
    if (out == nullptr || context.arena == nullptr || !Plausible(screenTexture)) {
        return ExtractStatus::Error;
    }
    const NiScreenTexture* st = G<NiScreenTexture>(screenTexture);
    const std::uint32_t rects = st->rectCount;
    if (rects == 0) {
        return ExtractStatus::Empty;  // slot 55 0x14C59E: +0x10 == 0
    }
    if (rects > 0x3FFFu || !Plausible(st->rects) || !Plausible(st->texturing)) {
        return ExtractStatus::Error;  // 4 * count must fit the u16 vertex count of FUN_0014A050
    }
    const auto width = static_cast<std::int32_t>(context.targetWidth);
    const auto height = static_cast<std::int32_t>(context.targetHeight);
    if (width <= 0 || height <= 0 || width > 0x7FFF || height > 0x7FFF) {
        return ExtractStatus::Error;  // the wrap loops of FUN_0014A050 need a non-empty target
    }
    // FUN_0014A050 calls GetWidth/GetHeight of the base map's texture (a NULL map crashes the original).
    const NiTexturingMap* base = BaseMap(st->texturing);
    if (base == nullptr || !Plausible(base->texture)) {
        return ExtractStatus::Error;
    }
    const TextureInfo tex = QueryTexture(base->texture);
    const auto texW = static_cast<std::uint16_t>(tex.width);   // ushort uVar10
    const auto texH = static_cast<std::uint16_t>(tex.height);  // ushort uVar11
    const double invW = 1.0 / static_cast<double>(static_cast<float>(texW));
    const double invH = 1.0 / static_cast<double>(static_cast<float>(texH));

    *out = DrawItem{};
    out->kind = DrawKind::ScreenTexture;
    out->guestObject = screenTexture;
    out->world = IdentityMatrix();
    out->worldScale = 1.0f;
    const std::uint32_t n = rects * 4u;
    auto* pos = context.arena->AllocateArray<float>(static_cast<std::size_t>(n) * 3u);
    auto* col = context.arena->AllocateArray<float>(static_cast<std::size_t>(n) * 4u);
    auto* uv = context.arena->AllocateArray<float>(static_cast<std::size_t>(n) * 2u);
    auto* idx = context.arena->AllocateArray<std::uint16_t>(static_cast<std::size_t>(rects) * 6u);
    if (pos == nullptr || col == nullptr || uv == nullptr || idx == nullptr) {
        return ExtractStatus::Error;
    }
    for (std::uint32_t q = 0; q < rects; ++q) {
        const NiScreenTextureRect& r = G<NiScreenTextureRect>(st->rects)[q];
        // Wrap left/top into the target (16-bit arithmetic of 0x14A1A6..0x14A208).
        std::int32_t left = r.left;
        while (static_cast<std::int16_t>(left) < 0) {
            left += width;
        }
        left = static_cast<std::int16_t>(left);
        while (left >= width) {
            left = static_cast<std::int16_t>(left - width);
        }
        std::int32_t top = r.top;
        while (static_cast<std::int16_t>(top) < 0) {
            top += height;
        }
        top = static_cast<std::int16_t>(top);
        while (top >= height) {
            top = static_cast<std::int16_t>(top - height);
        }
        const float x0 = static_cast<float>(static_cast<double>(left) - 0.5);
        const float y0 = static_cast<float>(static_cast<double>(top) - 0.5);
        const float x1 = static_cast<float>(static_cast<double>(r.width) + (static_cast<double>(left) - 0.5));
        const float y1 = static_cast<float>(static_cast<double>(r.height) + (static_cast<double>(top) - 0.5));
        const float u0 = static_cast<float>(static_cast<double>(r.texLeft) * invW);
        const float v0 = static_cast<float>(static_cast<double>(r.texTop) * invH);
        const float u1 = static_cast<float>(static_cast<double>(r.width) * invW + u0);
        const float v1 = static_cast<float>(static_cast<double>(r.height) * invH + v0);
        for (std::uint32_t c = 0; c < 4; ++c) {  // (L,T) (L,B) (R,T) (R,B)
            const std::uint32_t v = q * 4 + c;
            const bool rightSide = (c >> 1) != 0, bottom = (c & 1u) != 0;
            pos[v * 3 + 0] = rightSide ? x1 : x0;
            pos[v * 3 + 1] = bottom ? y1 : y0;
            pos[v * 3 + 2] = 0.0f;  // FUN_00157EA0: z = 0, rhw = 1
            col[v * 4 + 0] = r.color.r;
            col[v * 4 + 1] = r.color.g;
            col[v * 4 + 2] = r.color.b;
            col[v * 4 + 3] = r.color.a;
            uv[v * 2 + 0] = rightSide ? u1 : u0;
            uv[v * 2 + 1] = bottom ? v1 : v0;
        }
        const auto b = static_cast<std::uint16_t>(q * 4);
        const std::uint16_t quad[6] = {b, static_cast<std::uint16_t>(b + 1), static_cast<std::uint16_t>(b + 2),
                                       static_cast<std::uint16_t>(b + 2), static_cast<std::uint16_t>(b + 1),
                                       static_cast<std::uint16_t>(b + 3)};
        std::memcpy(idx + q * 6, quad, sizeof(quad));
    }
    GeometryDesc& g = out->geometry;
    g.vertexCount = n;
    g.positions = {pos, 12};
    g.colors = {col, 16};
    g.uvs[0] = {uv, 8};
    g.uvSetCount = 1;  // renderer data +0x04 = 0x1000000 (FUN_0014BFF0)
    g.topology = Topology::TriangleList;
    g.indices = idx;
    g.indexCount = rects * 6u;
    g.primitiveCount = rects * 2u;
    g.pretransformed = true;
    g.guestData = screenTexture;
    g.dataRevision = st->revision;

    // Slot 55's temporary NiPropertyState (renderer +0x5A8): the default properties, then its own vertex
    // color / alpha / z-buffer properties and the screen texture's NiTexturingProperty (FUN_00013C80).
    // Built in the arena as guest-format objects so the regular state code reads them.
    GuestPtr defaults[12];
    for (std::uint32_t i = 0; i < 12; ++i) {
        defaults[i] = context.defaultProperties != nullptr ? context.defaultProperties[i]
                                                            : R32(kDefaultPropertyGlobals[i]);
    }
    auto* vc = context.arena->AllocateArray<NiVertexColorProperty>(1);
    auto* alpha = context.arena->AllocateArray<NiAlphaProperty>(1);
    auto* zb = context.arena->AllocateArray<NiZBufferProperty>(1);
    auto* ps = context.arena->AllocateArray<NiPropertyState>(1);
    if (vc == nullptr || alpha == nullptr || zb == nullptr || ps == nullptr) {
        return ExtractStatus::Error;
    }
    std::memset(vc, 0, sizeof(*vc));
    std::memset(alpha, 0, sizeof(*alpha));
    std::memset(zb, 0, sizeof(*zb));
    std::memset(ps, 0, sizeof(*ps));
    vc->source = kScreenTextureVertexColorSource;
    vc->lighting = kScreenTextureVertexColorLighting;
    alpha->base.flags = kScreenTextureAlphaFlags;
    alpha->testRef = 0;
    zb->base.flags = kScreenTextureZBufferFlags;
    zb->field18 = 3;
    ps->alpha = GuestAddr(alpha);
    ps->dither = defaults[1];
    ps->fog = defaults[2];
    ps->material = defaults[3];
    ps->rendererSpecific = defaults[4];  // replaced by the screen pipeline (StateInput::screenTexture)
    ps->shade = defaults[5];
    ps->specular = defaults[6];
    ps->stencil = defaults[7];
    ps->texturing = st->texturing;
    ps->vertexColor = GuestAddr(vc);
    ps->wireframe = defaults[10];
    ps->zbuffer = GuestAddr(zb);
    StateInput si;
    si.propertyState = GuestAddr(ps);
    si.effectState = 0;  // lighting is off (vertex color EMISSIVE + LIGHTING_E): the lights are not evaluated
    si.worldScale = 1.0f;
    si.hasNormals = false;
    si.uvSetCount = 1;
    si.screenTexture = true;
    {  // TEMPORARY PROBE
        static int logged = 0;
        if (logged < 6 || (logged < 12 && rects > 8)) {
            ++logged;
            FUT_LOGI(Gfx, "PROBE screentex 0x%08X: %u rects, tex %ux%u (mips %u), apply %u, first quad (%.1f %.1f)-(%.1f %.1f) uv (%.3f %.3f)-(%.3f %.3f) color (%.2f %.2f %.2f %.2f), target %dx%d",
                     screenTexture, rects, tex.width, tex.height, tex.mipLevels, G<NiTexturingProperty>(st->texturing)->applyMode, pos[0], pos[1], pos[9], pos[10], uv[0], uv[1], uv[6], uv[7], col[0], col[1], col[2], col[3], width, height);
        }
    }
    return ExtractState(context, si, &out->state, &out->lights, &out->effects);
}

}  // namespace gfx::ni
