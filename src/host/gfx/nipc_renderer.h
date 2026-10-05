// NiPCRenderer: the null renderer that replaces NiXBoxRenderer at the NiRenderer interface
// (design 2.2 "Grafikgrenze", E6; M2 "Null-Renderer"; renderer.md 1, 2, 7).
//
// The detour of NiXBoxRenderer::Create 0x14C170 (cdecl, 8 args) returns a guest-visible object with
// the NiRenderer base layout, built by the original base ctor FUN_00140550 (so it sits in the global
// renderer list 0x2D7B18 that the purge/create broadcasts walk) and an own 75-slot vtable:
//   - slots 2..10 point to the original NiObject/NiRenderer entries of vtable 0x2AA7C0,
//   - every other slot is a host function with the exact stack purge (`ret N`) of the original slot
//     (gen/renderer_slots.gen.inc, from renderer_retn.py; compile-time checked against each handler's
//     signature and measured at runtime by the self-test GfxRendererThunkPurge),
//   - required results (renderer.md 7.1): slot 1 = &NiXBoxRenderer::ms_RTTI 0x30EE00, slots 65/66 TRUE
//     with layout-compatible renderer data, slot 39 FALSE (CPU geometry untouched), slot 16 counts
//     frames for --frames=N; slot 0 destroys through FUN_001406E0 + the game's operator delete.
// [render] backend = null (M2): nothing is drawn. [render] backend = d3d11 (M3): the Create detour also
// creates a gfx::GpuRenderer (gfx/nipc_gpu.h) for the main window and the slots 15, 16, 17, 45, 47-51,
// 54, 55 and 65 draw through ni_extract -> ff_pipeline -> RHI (D3D11); without a window or when the
// device cannot be created the renderer stays the null renderer (GFX/error).
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace gfx {

// ---- slot table (generated) ---------------------------------------------------------------------
inline constexpr std::uint32_t kRendererSlotCount = 75;

struct RendererSlot {
    std::uint32_t slot;
    std::uint32_t vtableOffset;
    std::uint32_t originalAddress;  // entry of the NiXBoxRenderer vtable 0x2AA7C0
    std::uint32_t stackArgs;        // thiscall stack arguments = ret N / 4 of the original
    std::uint32_t overrides;        // 0 inherited, 1 overridden, 2 implements a pure virtual, 3 new
    const char* name;               // renderer.md 2 (guessed names, partly German)
};

inline constexpr RendererSlot kRendererSlots[] = {
#define FUT_RENDERER_SLOT(slot, offset, address, args, overrides, name) {slot, offset, address, args, overrides, name},
#include "gen/renderer_slots.gen.inc"
#undef FUT_RENDERER_SLOT
};
static_assert(std::size(kRendererSlots) == kRendererSlotCount);

// Design 2.2: slots 2..10 (NiObject) keep the original entries.
inline constexpr std::uint32_t kFirstOriginalSlot = 2;
inline constexpr std::uint32_t kLastOriginalSlot = 10;

// ---- guest-visible layouts --------------------------------------------------------------------------
// NiPCRenderer object (design 2.2 table). Everything not listed stays zero.
struct NiPCRenderer {
    std::uint32_t vtable;         // +0x00 own 75-slot vtable
    std::uint32_t refCount;       // +0x04 NiRefObject (game: app+0x104 holds one reference)
    std::uint32_t accumulator;    // +0x08 NiAccumulator* (game writes it: FUN_000A2C20, FUN_0002A660)
    std::uint32_t propertyState;  // +0x0C NiPropertyState* written by the geometry before render calls
    std::uint32_t effectState;    // +0x10 NiDynamicEffectState*
    std::uint8_t reserved14[0x60 - 0x14];
    std::uint32_t device;         // +0x60 dummy IDirect3DDevice8* != 0 (movie player FUN_000EDE40)
    std::uint8_t reserved64[0x6C0 - 0x64];
    std::uint32_t host;           // +0x6C0 host state (HostRenderer*, never read by the guest)
    std::uint8_t reserved6C4[0x6D0 - 0x6C4];
};
static_assert(sizeof(NiPCRenderer) == 0x6D0);
static_assert(offsetof(NiPCRenderer, accumulator) == 0x08 && offsetof(NiPCRenderer, propertyState) == 0x0C &&
              offsetof(NiPCRenderer, effectState) == 0x10 && offsetof(NiPCRenderer, device) == 0x60 &&
              offsetof(NiPCRenderer, host) == 0x6C0);

// NiTexture field holding the head of the renderer data chain (NiTexture dtor FUN_00118400 deletes it
// through vtable slot 0; FUN_0015D9B0 appends, data+4 links the chain).
inline constexpr std::uint32_t kNiTextureRendererData = 0x20;
// NiSourceTexture: +0x30 file path (FUN_00118520), +0x34 NiPixelData* (FUN_00118600).
inline constexpr std::uint32_t kNiSourceTexturePath = 0x30;
inline constexpr std::uint32_t kNiSourceTexturePixelData = 0x34;
// NiRenderedTexture: +0x2C width, +0x30 height (FUN_0013FFE0 puVar2[0xB]/[0xC], vf11 0x141E40, vf12 0x13FFD0).
inline constexpr std::uint32_t kNiRenderedTextureWidth = 0x2C;
inline constexpr std::uint32_t kNiRenderedTextureHeight = 0x30;

// Renderer data of a source texture: layout of NiXBoxSourceTextureData (0x70 bytes, ctor FUN_0015D9E0 on
// base FUN_001576C0, filled by FUN_0015E010). The movie player FUN_000EE220 writes +0x3C (its YUY2
// D3DTexture*), +0x48/+0x4C (size), +0x54 (0), +0x58 (W*H*2) and increments +0x60 (renderer.md 5.2).
struct NiPCTextureData {
    std::uint32_t vtable;          // +0x00 own data vtable (slot 0 = scalar deleting dtor, ret 4)
    std::uint32_t next;            // +0x04 next renderer data of the same texture
    std::uint32_t texture;         // +0x08 owning NiTexture*
    std::uint32_t reserved0C;      // +0x0C
    std::uint32_t renderer;        // +0x10 NiRenderer* (movie player reads +0x5CC through it if DAT_0030EDD4)
    std::uint32_t pixelFormat[10]; // +0x14 NiPixelFormat (data vf4 returns &+0x14)
    std::uint32_t d3dTexture;      // +0x3C D3DTexture* (released by the dtor through the D3D shim)
    std::uint32_t field40;         // +0x40 (data vf7)
    std::uint32_t field44;         // +0x44
    std::uint32_t width;           // +0x48 (data vf2, NiSourceTexture::vf11 GetWidth)
    std::uint32_t height;          // +0x4C (data vf3, NiSourceTexture::vf12 GetHeight)
    std::uint32_t field50;         // +0x50 (data vf5)
    std::uint8_t mipmapped;        // +0x54
    std::uint8_t pad55[3];
    std::uint32_t sizeBytes;       // +0x58 pixel bytes
    std::uint32_t pixelData;       // +0x5C smart pointer, released by the dtor (data vf10)
    std::uint32_t revision;        // +0x60 NiPixelData revision (+0x48)
    // +0x64..+0x6F: host fields (GFX_CONTRACT.md 9; the original keeps an update counter at +0x64, read
    // only by REN code that never runs). Written by slot 65 and the texture resolver (M3).
    std::uint32_t hostTexture;     // +0x64 rhi::TextureHandle::id (0 = none)
    std::uint32_t hostInfo;        // +0x68 bits 0-7 tex::TextureSource, kHostOwnsTexture
    std::uint32_t hostRevision;    // +0x6C NiPixelData revision of the uploaded image
};
static_assert(sizeof(NiPCTextureData) == 0x70);
static_assert(offsetof(NiPCTextureData, hostTexture) == 0x64 && offsetof(NiPCTextureData, hostInfo) == 0x68 &&
              offsetof(NiPCTextureData, hostRevision) == 0x6C);
// NiPCTextureData::hostInfo: the RHI texture belongs to this data (destroyed with it); the shared default
// and black textures do not.
inline constexpr std::uint32_t kHostOwnsTexture = 0x100;
static_assert(offsetof(NiPCTextureData, renderer) == 0x10 && offsetof(NiPCTextureData, d3dTexture) == 0x3C &&
              offsetof(NiPCTextureData, width) == 0x48 && offsetof(NiPCTextureData, height) == 0x4C &&
              offsetof(NiPCTextureData, mipmapped) == 0x54 && offsetof(NiPCTextureData, sizeBytes) == 0x58 &&
              offsetof(NiPCTextureData, pixelData) == 0x5C && offsetof(NiPCTextureData, revision) == 0x60);

// Render-target descriptor of NiXBoxRenderer (vf21/vf22 read +0/+4 through renderer+0x58; vf16 swaps
// only if +0x10 == 0; FUN_0015E430 fills it for rendered textures).
struct NiPCTargetDescriptor {
    std::uint32_t width;         // +0x00
    std::uint32_t height;        // +0x04
    std::uint32_t pixelFormat;   // +0x08
    std::uint32_t reserved0C;
    std::uint32_t texture;       // +0x10 (0 = back buffer)
    std::uint32_t colorSurface;  // +0x14
    std::uint32_t reserved18;
    std::uint32_t reserved1C;
    std::uint32_t depthSurface;  // +0x20
};
static_assert(sizeof(NiPCTargetDescriptor) == 0x24);

// Renderer data of a rendered texture: layout of NiXBoxRenderedTextureData (0x84 bytes, FUN_0015E430;
// data vf10/vf11 get/set +0x7C, vf12/vf13 get/set +0x80 = target descriptor). The descriptor is placed
// right behind the object in the same allocation.
struct NiPCRenderedTextureData {
    NiPCTextureData base;                // +0x00..+0x6F
    std::uint32_t field70[3];            // +0x70..+0x7B
    std::uint32_t field7C;               // +0x7C
    std::uint32_t descriptor;            // +0x80 NiPCTargetDescriptor*
    NiPCTargetDescriptor descriptorData; // +0x84 (own storage of the descriptor)
};
static_assert(offsetof(NiPCRenderedTextureData, field7C) == 0x7C &&
              offsetof(NiPCRenderedTextureData, descriptor) == 0x80 &&
              offsetof(NiPCRenderedTextureData, descriptorData) == 0x84);

// Data vtables: slot count and stack arguments per slot (thiscall), mirroring 0x2AB97C (source:
// 0x15DFF0 ret 4, 0x15E2E0/0x157100..0x157150/0x13DB0/0x114B0/0x15DA60 ret) and 0x2AB9F4 (rendered:
// 0x15E760 ret 4, 0x15E340/0x15E360 ret 4, the rest ret).
inline constexpr std::uint32_t kSourceDataSlotCount = 11;
inline constexpr std::uint32_t kSourceDataStackArgs[kSourceDataSlotCount] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
inline constexpr std::uint32_t kRenderedDataSlotCount = 14;
inline constexpr std::uint32_t kRenderedDataStackArgs[kRenderedDataSlotCount] = {1, 0, 0, 0, 0, 0, 0,
                                                                                 0, 0, 0, 0, 1, 0, 1};

// ---- host API --------------------------------------------------------------------------------------
// The three vtables (guest-visible, below 0x80000000), built on first use; nullptr if the memory
// could not be allocated. Slots 2..10 of the renderer vtable are the original entries.
const std::uint32_t* RendererVtable() noexcept;
const std::uint32_t* SourceTextureDataVtable() noexcept;
const std::uint32_t* RenderedTextureDataVtable() noexcept;

// Most recently created NiPCRenderer (guest address), 0 if none.
std::uint32_t CurrentRenderer() noexcept;
// Frames shown by DisplayFrame (slot 16) while the back buffer was the render target.
std::uint32_t FramesDisplayed() noexcept;
// Calls of a renderer slot since start (0 for slots 2..10, which run original code).
std::uint32_t SlotCallCount(std::uint32_t slot) noexcept;
// Logs frames, per-slot call counts, texture data and D3D shim/trap statistics on GFX/info.
void LogRendererStatistics() noexcept;

}  // namespace gfx
