// NiPCRenderer null renderer: Create detour, slot handlers, texture renderer data, frame counter and
// pacing (see nipc_renderer.h; design 2.2, 5.4; renderer.md 1-7).
#include "gfx/nipc_renderer.h"

#include "core/crash.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "gfx/d3d_shim.h"
#include "gfx/d3d_traps.h"
#include "gfx/gfx_abi.h"
#include "gfx/gfx_config.h"
#include "gfx/guest_memory.h"
#include "gfx/purge_probe.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace gfx {
namespace {

// ---- host state -----------------------------------------------------------------------------------

struct HostRenderer {
    std::uint32_t self = 0;
    // Create arguments (renderer.md 1.4): (640, 480, 2, 1, 0x14000002, 0x4B, 0, 1) from FUN_000D26A0.
    std::uint32_t width = 640, height = 480;
    std::uint32_t backBufferFormat = 0, backBufferCount = 0, flags = 0, depthFormat = 0, refreshRate = 0,
                  presentInterval = 0;
    std::uint32_t renderTarget = 0;              // NiRenderedTexture* or 0 = back buffer
    std::uint32_t targetWidth = 0, targetHeight = 0;  // size of renderTarget (from its renderer data)
    std::uint32_t backgroundColor = 0xFF808080;  // +0x2CC default of FUN_00147CD0 (renderer.md 1.3)
    float depthClear = 1.0f;
    std::uint32_t stencilClear = 0;
    float maxFogValue = 1.0f;  // NiRenderer::vf29 base 0x140610: fld [1.0]
    bool leftRightSwap = false;
    bool inScene = false;
    std::uint32_t batchPropertyState = 0, batchEffectState = 0;
};

HostRenderer* Host(const NiPCRenderer* r) noexcept {
    return reinterpret_cast<HostRenderer*>(static_cast<std::uintptr_t>(r->host));
}

// ---- statistics -----------------------------------------------------------------------------------

std::atomic<std::uint32_t> g_slotCalls[kRendererSlotCount];
std::atomic<bool> g_slotSeen[kRendererSlotCount];
std::atomic<std::uint32_t> g_framesDisplayed{0};
std::atomic<std::uint32_t> g_currentRenderer{0};
std::atomic<std::uint32_t> g_sourceDataCreated{0}, g_renderedDataCreated{0}, g_dataDestroyed{0},
    g_textureFilesRead{0};

std::string SymbolizeSafe(void* address) noexcept {
    try {
        return guest::Symbolize(guest::Addr(address));
    } catch (...) {
        return {};
    }
}

void NoteCall(std::uint32_t slot, void* returnAddress) noexcept {
    g_slotCalls[slot].fetch_add(1, std::memory_order_relaxed);
    if (!g_slotSeen[slot].exchange(true) && core::LogEnabled(core::Channel::Gfx, core::Level::Debug)) {
        FUT_LOGD(Gfx, "NiPCRenderer slot %u (%s, %u stack args) first called from %s", slot,
                 kRendererSlots[slot].name, kRendererSlots[slot].stackArgs, SymbolizeSafe(returnAddress).c_str());
    }
}

// ---- guest memory helpers ---------------------------------------------------------------------------

template <class T = std::uint32_t>
T Read(std::uint32_t addr) noexcept {
    return guest::Read<T>(addr);
}

// Calls the game's operator new (memory.cpp pool, like FUN_0014C170 / FUN_0015E010 did).
std::uint32_t GameNew(std::uint32_t size) noexcept {
    const std::uint32_t p = core::CallGuest(guest::kGameOperatorNew, {size});
    if (p != 0) {
        std::memset(guest::Ptr<void>(p), 0, size);
    }
    return p;
}
void GameDelete(std::uint32_t p) noexcept {
    if (p != 0) {
        core::CallGuest(guest::kGameOperatorDelete, {p});
    }
}

// Releases a NiRefObject smart pointer (+4 refcount, delete via vtable slot 0 with flag 1), the
// sequence `dec [x+4]; jne; push 1; call [vt]` used all over the NetImmerse code.
void ReleaseRefObject(std::uint32_t object) noexcept {
    if (object == 0) {
        return;
    }
    auto* refCount = guest::Ptr<std::uint32_t>(object + 4);
    if (--*refCount == 0) {
        core::CallGuestThiscall(Read(Read(object)), object, {1});
    }
}

// ---- texture renderer data --------------------------------------------------------------------------

std::uint32_t SourceVtableAddr() noexcept;
std::uint32_t RenderedVtableAddr() noexcept;

bool IsOurData(std::uint32_t data) noexcept {
    if (data == 0) {
        return false;
    }
    const std::uint32_t vt = Read(data);
    return vt != 0 && (vt == SourceVtableAddr() || vt == RenderedVtableAddr());
}

// FUN_0015D9B0: append to the chain at NiTexture+0x20 (linked through data+4).
void AppendRendererData(std::uint32_t texture, std::uint32_t data) noexcept {
    std::uint32_t link = texture + kNiTextureRendererData;
    while (Read(link) != 0) {
        link = Read(link) + 4;
    }
    guest::Write<std::uint32_t>(link, data);
}

// FUN_00147880: unlink `data` from the chain of `texture`.
void UnlinkRendererData(std::uint32_t texture, std::uint32_t data) noexcept {
    std::uint32_t link = texture + kNiTextureRendererData;
    while (Read(link) != 0) {
        if (Read(link) == data) {
            guest::Write<std::uint32_t>(link, Read(data + 4));
            return;
        }
        link = Read(link) + 4;
    }
}

// Default texture of NiXBoxRenderer (renderer +0x5A0 pixel data, +0x598 format): FUN_0014AB50 (called by
// the device setup FUN_0014C0F0) creates it as NiPixelData FUN_00111260(8, 8, [+0x598], 1) - 8x8, one mip
// level, a red/blue/yellow test pattern written as 32-bit ARGB (0xFFFF0000, 0xFF0000FF, 0xFFFFFF00).
// FUN_0015E010 uses it instead of the texture's own image when there is no image, the image file cannot
// be read, its width or height is not a power of two, or no D3D format matches (renderer.md 5.2).
inline constexpr std::uint32_t kDefaultTextureSize = 8;
inline constexpr std::uint32_t kDefaultTextureBytes = kDefaultTextureSize * kDefaultTextureSize * 4;
// NiPixelFormat of the default texture (layout of the NiPixelFormat ctor FUN_001409A0: +0 format 1 = RGBA,
// +4..+0x10 R/G/B/A masks, +0x14 bits per pixel, +0x18/+0x1C packed channel descriptors).
// ASSUMPTION: [+0x598] = [+0x53C] is the 32-bit A8R8G8B8 entry FUN_0014A410 builds with
// FUN_00140D80(0xFF0000, 0xFF00, 0xFF, 0xFF000000, 0x20); the packed descriptors depend on runtime
// tables (0x2D7B5C..0x2D7B64, 0x30E8AC) and stay 0 - nothing reads them before M3.
inline constexpr std::uint32_t kDefaultTextureFormat[8] = {1, 0xFF0000, 0xFF00, 0xFF, 0xFF000000, 32, 0, 0};

std::atomic<std::uint32_t> g_defaultNoImage{0}, g_defaultUnreadable{0}, g_defaultNotPow2{0},
    g_defaultNotLoaded{0};

bool IsPowerOfTwo(std::uint32_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;  // = the bit loops of FUN_0015E010 (0x15E0B5..0x15E0D8)
}

void FillDefaultTextureInfo(NiPCTextureData* data) noexcept {
    data->width = kDefaultTextureSize;
    data->height = kDefaultTextureSize;
    data->sizeBytes = kDefaultTextureBytes;
    data->revision = 0;
    for (std::uint32_t i = 0; i < 8; ++i) {
        data->pixelFormat[i] = kDefaultTextureFormat[i];
    }
}

// Size and format of a source texture in the order of FUN_0015E010 (0x15E04B..0x15E0F0):
//   1. image = the file at +0x30 if a path is set, read through the game's NiImageConverter (singleton
//      [0x30EF64], vf2 = ReadImageFile, thiscall (path, 0); the result has refcount 0), else the
//      NiPixelData at +0x34 - a path wins even if +0x34 is set;
//   2. no image, or width/height of mip 0 not a power of two -> the renderer's default texture (8x8).
// The game sees the result through NiSourceTexture::vf11/vf12 (GetWidth/GetHeight 0x118000/0x118020 ->
// renderer data vf2/vf3 = +0x48/+0x4C). Not reproduced: the D3D format choice FUN_001572D0 (its NULL
// result also selects the default) - the NiPixelFormat of the source stands in for it.
// `[render] load_textures = 0` skips the file read; such a texture gets the default like an unreadable
// file (deviation, counted). The original falls back silently, so does this (GFX/debug only).
// NiPixelData (ctor 0x110FF0, FUN_00111260): +0x0C NiPixelFormat (8 DWORDs), +0x2C palette,
// +0x34 width[] / +0x38 height[] per mip, +0x3C offsets[mips + 1], +0x40 mip count, +0x48 revision.
void FillSourceTextureInfo(NiPCTextureData* data, std::uint32_t texture) noexcept {
    const std::uint32_t path = Read(texture + kNiSourceTexturePath);
    std::uint32_t pixels = 0;
    if (path != 0) {
        const std::uint32_t converter = Read(guest::kNiImageConverterPtr);
        if (!GetSettings().loadTextures) {
            g_defaultNotLoaded.fetch_add(1, std::memory_order_relaxed);
            FUT_LOGD(Gfx, "texture '%.200s': not read ([render] load_textures = 0) - default 8x8 texture",
                     guest::Ptr<const char>(path));
            FillDefaultTextureInfo(data);
            return;
        }
        if (converter != 0) {
            pixels = core::CallGuestThiscall(Read(Read(converter) + 8), converter, {path, 0});
            g_textureFilesRead.fetch_add(1, std::memory_order_relaxed);
        }
        if (pixels == 0) {
            g_defaultUnreadable.fetch_add(1, std::memory_order_relaxed);
            FUT_LOGD(Gfx,
                     "texture '%.200s': NiImageConverter could not read it - default 8x8 texture (as "
                     "FUN_0015E010)",
                     guest::Ptr<const char>(path));
            FillDefaultTextureInfo(data);
            return;
        }
    } else {
        pixels = Read(texture + kNiSourceTexturePixelData);
        if (pixels == 0) {
            // E.g. the movie player's texture (NiSourceTexture::Create(NULL, ..) in FUN_000EE220), which
            // then writes its own size into +0x48/+0x4C (renderer.md 5.2).
            g_defaultNoImage.fetch_add(1, std::memory_order_relaxed);
            FillDefaultTextureInfo(data);
            return;
        }
    }
    auto* refCount = guest::Ptr<std::uint32_t>(pixels + 4);
    ++*refCount;
    const std::uint32_t widths = Read(pixels + 0x34), heights = Read(pixels + 0x38);
    const std::uint32_t width = widths != 0 ? Read(widths) : 0;
    const std::uint32_t height = heights != 0 ? Read(heights) : 0;
    if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height)) {
        g_defaultNotPow2.fetch_add(1, std::memory_order_relaxed);
        FUT_LOGD(Gfx,
                 "texture '%.200s' (NiPixelData 0x%08X): %ux%u is not a power of two - default 8x8 "
                 "texture (as FUN_0015E010)",
                 path != 0 ? guest::Ptr<const char>(path) : "(pixel data)", pixels, width, height);
        FillDefaultTextureInfo(data);
        ReleaseRefObject(pixels);  // a file image with refcount 0 is freed here (the original leaks it)
        return;
    }
    data->width = width;
    data->height = height;
    data->revision = Read(pixels + 0x48);
    const std::uint32_t offsets = Read(pixels + 0x3C), mips = Read(pixels + 0x40);
    data->sizeBytes = offsets != 0 && mips < 16 ? Read(offsets + 4 * mips) : 0;
    // NiPixelFormat of the source (NiPixelData +0x0C..+0x2B, ctor 0x110FF0 / FUN_00111260 copy 8 DWORDs).
    // FUN_0015E010 copies the D3D-side format it picked (8 DWORDs to +0x14..+0x33, DWORD 9 to +0x38);
    // without a D3D format the source format is the closest equivalent, +0x38 stays 0.
    for (std::uint32_t i = 0; i < 8; ++i) {
        data->pixelFormat[i] = Read(pixels + 0x0C + 4 * i);
    }
    if (path != 0) {
        FUT_LOGT(Gfx, "texture '%.200s': %ux%u", guest::Ptr<const char>(path), data->width, data->height);
    }
    ReleaseRefObject(pixels);
}

std::uint32_t NewTextureData(NiPCRenderer* self, std::uint32_t texture, std::uint32_t size,
                             std::uint32_t vtable) noexcept {
    const std::uint32_t addr = GameNew(size);
    if (addr == 0) {
        FUT_LOGE(Gfx, "NiPCRenderer: operator new(0x%X) for texture renderer data failed", size);
        return 0;
    }
    auto* d = guest::Ptr<NiPCTextureData>(addr);
    d->vtable = vtable;
    d->texture = texture;
    d->renderer = guest::Addr(self);
    return addr;
}

// ---- renderer slot handlers (__fastcall(self, edx, stack args) == thiscall + ret N) -----------------

// slot 0 - scalar deleting destructor (NiXBoxRenderer::vf00 0x14CB90, ret 4: dtor, operator delete if
// flags & 1, returns this). Design 2.2: destroy through the original base dtor FUN_001406E0 (unregisters
// from 0x2D7B18, releases the accumulator at +8) - like NiRenderer::vf00 0x140750.
std::uint32_t __fastcall SlotDestroy(NiPCRenderer* self, void*, std::uint32_t flags) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(0, _ReturnAddress());
    FUT_HLE_TRY {
        const std::uint32_t addr = guest::Addr(self);
        FUT_LOGI(Gfx, "NiPCRenderer 0x%08X destroyed (flags %u, %u frames displayed)", addr, flags,
                 g_framesDisplayed.load());
        delete Host(self);
        self->host = 0;
        core::CallGuestThiscall(guest::kNiRendererBaseDtor, addr);
        if ((flags & 1) != 0) {
            GameDelete(addr);
        }
        std::uint32_t expected = addr;
        g_currentRenderer.compare_exchange_strong(expected, 0);
        return addr;
    }
    FUT_HLE_CATCH("NiPCRenderer slot 0 (destructor)")
}

// slot 11 - GetDriverInfo (0x146350, ret): const char* describing the adapter.
const char* DriverInfoString() noexcept;
std::uint32_t __fastcall SlotGetDriverInfo(NiPCRenderer*, void*) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(11, _ReturnAddress());
    return guest::Addr(DriverInfoString());
}

// slot 15 - SetRenderTarget(NiRenderedTexture* or NULL = back buffer) (0x146400, ret 4): FALSE inside a
// scene or for a texture without renderer data, else TRUE. Like the original it calls the game's
// screen-offset helpers (back buffer: user offset FUN_000A1930, texture: 0 FUN_000A1920), which reach the
// D3D shim's SetScreenSpaceOffset (design 2.2 "vf15-Logik nachbilden").
std::uint32_t __fastcall SlotSetRenderTarget(NiPCRenderer* self, void*, std::uint32_t texture) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(15, _ReturnAddress());
    HostRenderer* h = Host(self);
    if (h == nullptr || h->inScene) {
        return 0;
    }
    if (texture == 0) {
        core::CallGuest(guest::kGameScreenOffsetUser);
    } else {
        // The original takes the target descriptor from the head of the renderer data chain (data vf12).
        const std::uint32_t data = Read(texture + kNiTextureRendererData);
        if (data == 0) {
            return 0;
        }
        if (IsOurData(data)) {
            h->targetWidth = guest::Ptr<NiPCTextureData>(data)->width;
            h->targetHeight = guest::Ptr<NiPCTextureData>(data)->height;
        } else {  // ASSUMPTION: foreign data (no second renderer exists) - NiRenderedTexture size fields
            h->targetWidth = Read(texture + kNiRenderedTextureWidth);
            h->targetHeight = Read(texture + kNiRenderedTextureHeight);
        }
        core::CallGuest(guest::kGameScreenOffsetZero);
    }
    h->renderTarget = texture;
    return 1;
}

void OnFrameDisplayed() noexcept;

// slot 16 - DisplayFrame (0x146460, ret): D3DDevice_Swap only while the back buffer is the target ->
// frame counter, pacing and the --frames limit (design 5.4).
std::uint32_t __fastcall SlotDisplayFrame(NiPCRenderer* self, void*) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(16, _ReturnAddress());
    const HostRenderer* h = Host(self);
    if (h != nullptr && h->renderTarget == 0) {
        OnFrameDisplayed();
    }
    return 0;
}

// slot 18 - GetLeftRightSwap (0x146600, ret) / slot 19 - SetLeftRightSwap(bool) (0x146620, ret 4: TRUE).
std::uint32_t __fastcall SlotGetLeftRightSwap(NiPCRenderer* self, void*) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(18, _ReturnAddress());
    const HostRenderer* h = Host(self);
    return h != nullptr && h->leftRightSwap ? 1 : 0;
}
std::uint32_t __fastcall SlotSetLeftRightSwap(NiPCRenderer* self, void*, std::uint32_t swap) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(19, _ReturnAddress());
    if (HostRenderer* h = Host(self)) {
        h->leftRightSwap = (swap & 0xFF) != 0;
    }
    return 1;
}

// slot 20 - W-buffer on/off (0x146640, ret 4): enabling fails because the caps bit 0x100 is never set.
std::uint32_t __fastcall SlotSetWBuffer(NiPCRenderer*, void*, std::uint32_t enable) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(20, _ReturnAddress());
    return (enable & 0xFF) != 0 ? 0 : 1;
}

// slot 21/22 - GetBufferWidth/Height of the current target (0x146680 / 0x146690, ret: `[this+0x58]` +0/+4
// = descriptor of the back buffer or of the target texture): Create size (640x480) or the target size
// SetRenderTarget took from the texture's renderer data.
std::uint32_t __fastcall SlotGetBufferWidth(NiPCRenderer* self, void*) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(21, _ReturnAddress());
    const HostRenderer* h = Host(self);
    if (h == nullptr) {
        return 0;
    }
    return h->renderTarget != 0 ? h->targetWidth : h->width;
}
std::uint32_t __fastcall SlotGetBufferHeight(NiPCRenderer* self, void*) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(22, _ReturnAddress());
    const HostRenderer* h = Host(self);
    if (h == nullptr) {
        return 0;
    }
    return h->renderTarget != 0 ? h->targetHeight : h->height;
}

// slot 26 - SetDepthClear(float) (0x14B0B0, ret 4), slot 28 - SetStencilClear(unsigned) (0x14B120, ret 4).
std::uint32_t __fastcall SlotSetDepthClear(NiPCRenderer* self, void*, float depth) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(26, _ReturnAddress());
    if (HostRenderer* h = Host(self)) {
        h->depthClear = depth;
    }
    return 0;
}
std::uint32_t __fastcall SlotSetStencilClear(NiPCRenderer* self, void*, std::uint32_t stencil) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(28, _ReturnAddress());
    if (HostRenderer* h = Host(self)) {
        h->stencilClear = stencil;
    }
    return 0;
}

// slot 27 - SetBackgroundColor(const NiColor&) (0x14B0C0, ret 4): ARGB with A = 0xFF, returns TRUE.
std::uint32_t __fastcall SlotSetBackgroundColor(NiPCRenderer* self, void*, std::uint32_t color) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(27, _ReturnAddress());
    HostRenderer* h = Host(self);
    if (h != nullptr && color != 0) {
        auto channel = [&](std::uint32_t i) -> std::uint32_t {
            const float v = Read<float>(color + 4 * i) * 255.0f;
            return v <= 0.0f ? 0u : v >= 255.0f ? 255u : static_cast<std::uint32_t>(v);
        };
        h->backgroundColor = 0xFF000000u | (channel(0) << 16) | (channel(1) << 8) | channel(2);
    }
    return 1;
}

// slot 29 - GetMaxFogValue (0x146930, ret; float in st(0)) / slot 30 - SetMaxFogValue(float) (0x146940,
// ret 4: x <= 0 -> 1e-5).
float __fastcall SlotGetMaxFogValue(NiPCRenderer* self, void*) noexcept {
    if (InPurgeProbe()) {
        return 0.0f;
    }
    NoteCall(29, _ReturnAddress());
    const HostRenderer* h = Host(self);
    return h != nullptr ? h->maxFogValue : 1.0f;
}
std::uint32_t __fastcall SlotSetMaxFogValue(NiPCRenderer* self, void*, float value) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(30, _ReturnAddress());
    if (HostRenderer* h = Host(self)) {
        h->maxFogValue = value <= 0.0f ? 1e-5f : value;
    }
    return 0;
}

std::uint32_t __fastcall DataDestroy(NiPCTextureData* data, void*, std::uint32_t flags) noexcept;

// slot 33 - PurgeTexture(NiTexture*) (0x148000, ret 4): unlink and delete this renderer's data, TRUE.
std::uint32_t __fastcall SlotPurgeTexture(NiPCRenderer* self, void*, std::uint32_t texture) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(33, _ReturnAddress());
    if (texture == 0) {
        return 1;
    }
    for (std::uint32_t d = Read(texture + kNiTextureRendererData); d != 0; d = Read(d + 4)) {
        if (IsOurData(d) && Read(d + 0x10) == guest::Addr(self)) {
            UnlinkRendererData(texture, d);
            guest::Write<std::uint32_t>(d + 4, 0);
            DataDestroy(guest::Ptr<NiPCTextureData>(d), nullptr, 1);
            break;
        }
    }
    return 1;
}

// slot 40 - inherited NiRenderer 0x140130 (ret 8): `*arg2 = NULL`, returns arg2 (out smart pointer).
std::uint32_t __fastcall SlotClearOutPointer(NiPCRenderer*, void*, std::uint32_t, std::uint32_t out) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(40, _ReturnAddress());
    if (out != 0) {
        guest::Write<std::uint32_t>(out, 0);
    }
    return out;
}

// slot 41 - BeginBatch(NiPropertyState*, NiDynamicEffectState*) (0x146E20, ret 8).
std::uint32_t __fastcall SlotBeginBatch(NiPCRenderer* self, void*, std::uint32_t propertyState,
                                        std::uint32_t effectState) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(41, _ReturnAddress());
    if (HostRenderer* h = Host(self)) {
        h->batchPropertyState = propertyState;
        h->batchEffectState = effectState;
    }
    return 0;
}

// slot 45 - BeginScene (0x148950, ret) / slot 46 - EndScene (0x1469C0, ret): in-scene flag (+0x500 in
// the original; SetRenderTarget refuses to switch inside a scene).
std::uint32_t __fastcall SlotBeginScene(NiPCRenderer* self, void*) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(45, _ReturnAddress());
    if (HostRenderer* h = Host(self)) {
        h->inScene = true;
    }
    return 0;
}
std::uint32_t __fastcall SlotEndScene(NiPCRenderer* self, void*) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(46, _ReturnAddress());
    if (HostRenderer* h = Host(self)) {
        h->inScene = false;
    }
    return 0;
}

// slot 65 - CreateSourceTextureRendererData(NiSourceTexture*) (0x147350, ret 4). Must return TRUE, else
// NiSourceTexture::Create (FUN_00118600/FUN_00118520 via the broadcast FUN_00117FC0) returns NULL.
// Existing data -> TRUE (like the original). New data: NiPCTextureData (0x70, layout of
// NiXBoxSourceTextureData) appended to the chain at +0x20, size/format as FUN_0015E010 takes them.
std::uint32_t __fastcall SlotCreateSourceTextureData(NiPCRenderer* self, void*, std::uint32_t texture) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(65, _ReturnAddress());
    FUT_HLE_TRY {
        if (texture == 0) {
            FUT_LOGW(Gfx, "NiPCRenderer slot 65 with NULL texture from %s - FALSE",
                     SymbolizeSafe(_ReturnAddress()).c_str());
            return 0;
        }
        if (Read(texture + kNiTextureRendererData) != 0) {
            return 1;
        }
        const std::uint32_t addr = NewTextureData(self, texture, sizeof(NiPCTextureData), SourceVtableAddr());
        if (addr == 0) {
            return 0;
        }
        FillSourceTextureInfo(guest::Ptr<NiPCTextureData>(addr), texture);
        AppendRendererData(texture, addr);
        g_sourceDataCreated.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    FUT_HLE_CATCH("NiPCRenderer slot 65 (CreateSourceTextureRendererData)")
}

// slot 66 - CreateRenderedTextureRendererData(NiRenderedTexture*) (0x147380, ret 4). Must return TRUE,
// else NiRenderedTexture::Create FUN_0013FFE0 deletes the texture and returns NULL. Data: 0x84 bytes
// (layout of NiXBoxRenderedTextureData) + target descriptor {width, height, ...}.
std::uint32_t __fastcall SlotCreateRenderedTextureData(NiPCRenderer* self, void*, std::uint32_t texture) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    NoteCall(66, _ReturnAddress());
    FUT_HLE_TRY {
        if (texture == 0) {
            FUT_LOGW(Gfx, "NiPCRenderer slot 66 with NULL texture from %s - FALSE",
                     SymbolizeSafe(_ReturnAddress()).c_str());
            return 0;
        }
        if (Read(texture + kNiTextureRendererData) != 0) {
            return 1;
        }
        const std::uint32_t addr =
            NewTextureData(self, texture, sizeof(NiPCRenderedTextureData), RenderedVtableAddr());
        if (addr == 0) {
            return 0;
        }
        auto* d = guest::Ptr<NiPCRenderedTextureData>(addr);
        d->base.width = Read(texture + kNiRenderedTextureWidth);
        d->base.height = Read(texture + kNiRenderedTextureHeight);
        d->descriptorData.width = d->base.width;
        d->descriptorData.height = d->base.height;
        d->descriptorData.texture = texture;  // != 0: not the back buffer (DisplayFrame rule of vf16)
        d->descriptor = guest::Addr(&d->descriptorData);
        AppendRendererData(texture, addr);
        g_renderedDataCreated.fetch_add(1, std::memory_order_relaxed);
        FUT_LOGD(Gfx, "NiPCRenderer: rendered texture 0x%08X %ux%u -> data 0x%08X", texture, d->base.width,
                 d->base.height, addr);
        return 1;
    }
    FUT_HLE_CATCH("NiPCRenderer slot 66 (CreateRenderedTextureRendererData)")
}

// Fixed results of the generic slots (all others return 0 in eax):
//   1 GetRTTI -> &NiXBoxRenderer::ms_RTTI (design 2.2), 12 GetFlags -> 0xED (FUN_001474F0 with the
//   game's auto depth/stencil D24S8), 23 GetBufferDepth -> 32 (X8R8G8B8 back buffer, bbFmt 2 -> 0x1E),
//   57 -> TRUE (inherited 0x256C0 `mov al,1`), 69 CreatePaletteRendererData / 70
//   CreateBltSourceRendererData -> TRUE without data (the broadcasts FUN_00110BD0/FUN_00111D70 only test
//   the result; +0x14 of NiPalette/NiBltSource stays NULL). 39 PrecacheGeometry -> FALSE (renderer.md
//   5.3/7.1: CPU arrays must stay), 67/68 cube maps -> FALSE (inherited 0x1605F0, no callers).
constexpr std::uint32_t NoopResult(std::uint32_t slot) {
    switch (slot) {
    case 1:
        return guest::kNiXBoxRendererRtti;
    case 12:
        return 0xED;
    case 23:
        return 32;
    case 57:
    case 69:
    case 70:
        return 1;
    default:
        return 0;
    }
}

template <std::size_t>
using Dword = std::uint32_t;

template <std::uint32_t Slot, class Seq>
struct NoopSlot;
template <std::uint32_t Slot, std::size_t... I>
struct NoopSlot<Slot, std::index_sequence<I...>> {
    static std::uint32_t __fastcall Call(NiPCRenderer*, void*, Dword<I>...) noexcept {
        if (InPurgeProbe()) {
            return 0;
        }
        NoteCall(Slot, _ReturnAddress());
        return NoopResult(Slot);
    }
};

template <std::uint32_t Slot, class F>
const void* Checked(F handler) noexcept {
    static_assert(abi::ThiscallStackArgs<F>() == kRendererSlots[Slot].stackArgs,
                  "NiPCRenderer handler: stack arguments differ from gen/renderer_slots.gen.inc (ret N)");
    return core::detail::CodeAddress(handler);
}

template <std::uint32_t Slot>
const void* SlotEntry() noexcept {
    if constexpr (Slot >= kFirstOriginalSlot && Slot <= kLastOriginalSlot) {
        return guest::Ptr<const void>(kRendererSlots[Slot].originalAddress);
    } else if constexpr (Slot == 0) {
        return Checked<Slot>(&SlotDestroy);
    } else if constexpr (Slot == 11) {
        return Checked<Slot>(&SlotGetDriverInfo);
    } else if constexpr (Slot == 15) {
        return Checked<Slot>(&SlotSetRenderTarget);
    } else if constexpr (Slot == 16) {
        return Checked<Slot>(&SlotDisplayFrame);
    } else if constexpr (Slot == 18) {
        return Checked<Slot>(&SlotGetLeftRightSwap);
    } else if constexpr (Slot == 19) {
        return Checked<Slot>(&SlotSetLeftRightSwap);
    } else if constexpr (Slot == 20) {
        return Checked<Slot>(&SlotSetWBuffer);
    } else if constexpr (Slot == 21) {
        return Checked<Slot>(&SlotGetBufferWidth);
    } else if constexpr (Slot == 22) {
        return Checked<Slot>(&SlotGetBufferHeight);
    } else if constexpr (Slot == 26) {
        return Checked<Slot>(&SlotSetDepthClear);
    } else if constexpr (Slot == 27) {
        return Checked<Slot>(&SlotSetBackgroundColor);
    } else if constexpr (Slot == 28) {
        return Checked<Slot>(&SlotSetStencilClear);
    } else if constexpr (Slot == 29) {
        return Checked<Slot>(&SlotGetMaxFogValue);
    } else if constexpr (Slot == 30) {
        return Checked<Slot>(&SlotSetMaxFogValue);
    } else if constexpr (Slot == 33) {
        return Checked<Slot>(&SlotPurgeTexture);
    } else if constexpr (Slot == 40) {
        return Checked<Slot>(&SlotClearOutPointer);
    } else if constexpr (Slot == 41) {
        return Checked<Slot>(&SlotBeginBatch);
    } else if constexpr (Slot == 45) {
        return Checked<Slot>(&SlotBeginScene);
    } else if constexpr (Slot == 46) {
        return Checked<Slot>(&SlotEndScene);
    } else if constexpr (Slot == 65) {
        return Checked<Slot>(&SlotCreateSourceTextureData);
    } else if constexpr (Slot == 66) {
        return Checked<Slot>(&SlotCreateRenderedTextureData);
    } else {
        return core::detail::CodeAddress(
            &NoopSlot<Slot, std::make_index_sequence<kRendererSlots[Slot].stackArgs>>::Call);
    }
}

template <std::size_t... S>
void FillRendererVtable(std::uint32_t* vt, std::index_sequence<S...>) noexcept {
    ((vt[S] = guest::Addr(SlotEntry<static_cast<std::uint32_t>(S)>())), ...);
}

// ---- texture renderer data vtables -----------------------------------------------------------------

// data slot 0 - scalar deleting dtor (0x15DFF0 / 0x15E760, ret 4): FUN_0015DA70 releases +0x3C through
// D3DResource_Release (DAT_0030EDD4 == 0 path) and the smart pointer +0x5C; the base dtor FUN_001570E0
// deletes the rest of the chain (+4) through its vtable slot 0; operator delete if flags & 1.
std::uint32_t __fastcall DataDestroy(NiPCTextureData* data, void*, std::uint32_t flags) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    FUT_HLE_TRY {
        if (data->d3dTexture != 0) {
            d3d::ResourceRelease(data->d3dTexture);
            data->d3dTexture = 0;
        }
        if (data->pixelData != 0) {
            ReleaseRefObject(data->pixelData);
            data->pixelData = 0;
        }
        const std::uint32_t next = data->next;
        data->next = 0;
        if (next != 0) {
            core::CallGuestThiscall(Read(next), next, {1});
        }
        g_dataDestroyed.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t addr = guest::Addr(data);
        if ((flags & 1) != 0) {
            GameDelete(addr);
        }
        return addr;
    }
    FUT_HLE_CATCH("NiPCTextureData destructor")
}

// data slot 1 (0x15E2E0): D3D texture present. Slots 2..7 (0x157100..0x157150): +0x48 width, +0x4C height,
// &+0x14 pixel format, +0x50, +0x3C, +0x40. Source data: 8 -> 0 (0x13DB0), 9 -> this (0x114B0), 10 -> +0x5C
// (0x15DA60). Rendered data: 8 -> this, 9 -> 0, 10/11 get/set +0x7C, 12/13 get/set +0x80 (0x15E330..0x15E360).
std::uint32_t __fastcall DataHasTexture(NiPCTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : (d->d3dTexture != 0 ? 1 : 0);
}
std::uint32_t __fastcall DataWidth(NiPCTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : d->width;
}
std::uint32_t __fastcall DataHeight(NiPCTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : d->height;
}
std::uint32_t __fastcall DataPixelFormat(NiPCTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : guest::Addr(&d->pixelFormat[0]);
}
std::uint32_t __fastcall DataField50(NiPCTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : d->field50;
}
std::uint32_t __fastcall DataD3DTexture(NiPCTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : d->d3dTexture;
}
std::uint32_t __fastcall DataField40(NiPCTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : d->field40;
}
std::uint32_t __fastcall DataZero(NiPCTextureData*, void*) noexcept {
    return 0;
}
std::uint32_t __fastcall DataSelf(NiPCTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : guest::Addr(d);
}
std::uint32_t __fastcall DataPixelData(NiPCTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : d->pixelData;
}
std::uint32_t __fastcall RenderedGet7C(NiPCRenderedTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : d->field7C;
}
std::uint32_t __fastcall RenderedSet7C(NiPCRenderedTextureData* d, void*, std::uint32_t v) noexcept {
    if (!InPurgeProbe()) {
        d->field7C = v;
    }
    return 0;
}
std::uint32_t __fastcall RenderedGetDescriptor(NiPCRenderedTextureData* d, void*) noexcept {
    return InPurgeProbe() ? 0 : d->descriptor;
}
std::uint32_t __fastcall RenderedSetDescriptor(NiPCRenderedTextureData* d, void*, std::uint32_t v) noexcept {
    if (!InPurgeProbe()) {
        d->descriptor = v;
    }
    return 0;
}

// Address of a data vtable handler; the stack arguments are checked against the table at compile time
// (and measured at runtime by the self-test GfxRendererThunkPurge).
template <std::uint32_t StackArgs, class F>
std::uint32_t DataEntry(F handler) noexcept {
    static_assert(abi::ThiscallStackArgs<F>() == StackArgs, "texture data handler: stack arguments differ");
    return guest::Addr(core::detail::CodeAddress(handler));
}

// ---- vtables in guest-visible memory -------------------------------------------------------------------

struct Tables {
    std::uint32_t renderer[kRendererSlotCount];
    std::uint32_t sourceData[kSourceDataSlotCount];
    std::uint32_t renderedData[kRenderedDataSlotCount];
    char driverInfo[64];
};

Tables* BuildTables() noexcept {
    auto* t = static_cast<Tables*>(AllocGuestPages(sizeof(Tables)));
    if (t == nullptr) {
        return nullptr;
    }
    FillRendererVtable(t->renderer, std::make_index_sequence<kRendererSlotCount>{});
    constexpr const std::uint32_t* sa = kSourceDataStackArgs;
    const std::uint32_t source[kSourceDataSlotCount] = {
        DataEntry<sa[0]>(&DataDestroy),     DataEntry<sa[1]>(&DataHasTexture), DataEntry<sa[2]>(&DataWidth),
        DataEntry<sa[3]>(&DataHeight),      DataEntry<sa[4]>(&DataPixelFormat), DataEntry<sa[5]>(&DataField50),
        DataEntry<sa[6]>(&DataD3DTexture),  DataEntry<sa[7]>(&DataField40),    DataEntry<sa[8]>(&DataZero),
        DataEntry<sa[9]>(&DataSelf),        DataEntry<sa[10]>(&DataPixelData)};
    constexpr const std::uint32_t* ra = kRenderedDataStackArgs;
    const std::uint32_t rendered[kRenderedDataSlotCount] = {
        DataEntry<ra[0]>(&DataDestroy),           DataEntry<ra[1]>(&DataHasTexture),
        DataEntry<ra[2]>(&DataWidth),             DataEntry<ra[3]>(&DataHeight),
        DataEntry<ra[4]>(&DataPixelFormat),       DataEntry<ra[5]>(&DataField50),
        DataEntry<ra[6]>(&DataD3DTexture),        DataEntry<ra[7]>(&DataField40),
        DataEntry<ra[8]>(&DataSelf),              DataEntry<ra[9]>(&DataZero),
        DataEntry<ra[10]>(&RenderedGet7C),        DataEntry<ra[11]>(&RenderedSet7C),
        DataEntry<ra[12]>(&RenderedGetDescriptor), DataEntry<ra[13]>(&RenderedSetDescriptor)};
    std::memcpy(t->sourceData, source, sizeof(source));
    std::memcpy(t->renderedData, rendered, sizeof(rendered));
    std::snprintf(t->driverInfo, sizeof(t->driverInfo), "NiPCRenderer (null renderer, M2)");
    return t;
}

Tables* GetTables() noexcept {
    static std::once_flag once;
    static Tables* tables = nullptr;
    try {
        std::call_once(once, [] { tables = BuildTables(); });
    } catch (...) {
        return nullptr;
    }
    return tables;
}

std::uint32_t SourceVtableAddr() noexcept {
    Tables* t = GetTables();
    return t != nullptr ? guest::Addr(t->sourceData) : 0;
}
std::uint32_t RenderedVtableAddr() noexcept {
    Tables* t = GetTables();
    return t != nullptr ? guest::Addr(t->renderedData) : 0;
}
const char* DriverInfoString() noexcept {
    Tables* t = GetTables();
    return t != nullptr ? t->driverInfo : nullptr;
}

// ---- frames: counter, pacing, --frames limit (design 5.4) ---------------------------------------------

std::mutex g_paceMutex;
LONGLONG g_nextFrame = 0;

// Waits for the next 1/hz s slot like D3DDevice_Swap waits for the vertical blank (PresentationInterval
// ONE from FUN_000D26A0). Falls back to a new schedule when the game is more than one frame late.
void PaceFrame(std::uint32_t hz) noexcept {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);
    std::lock_guard<std::mutex> lock(g_paceMutex);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const LONGLONG period = freq.QuadPart / hz;
    if (g_nextFrame == 0 || now.QuadPart - g_nextFrame > period) {
        g_nextFrame = now.QuadPart + period;
        return;
    }
    if (now.QuadPart < g_nextFrame) {
        const LONGLONG ticks = g_nextFrame - now.QuadPart;
        LARGE_INTEGER due;
        due.QuadPart = -(ticks * 10000000 / freq.QuadPart);
        if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(static_cast<DWORD>(ticks * 1000 / freq.QuadPart));
        }
    }
    g_nextFrame += period;
}

void OnFrameDisplayed() noexcept {
    const Settings& s = GetSettings();
    if (s.frameRate != 0) {
        PaceFrame(s.frameRate);
    }
    const std::uint32_t n = g_framesDisplayed.fetch_add(1) + 1;
    if (n == 1) {
        FUT_LOGI(Gfx, "NiPCRenderer: first frame displayed (pacing %u Hz, frame limit %u)", s.frameRate,
                 s.frameLimit);
    } else if (n % 1000 == 0) {
        FUT_LOGI(Gfx, "NiPCRenderer: %u frames displayed", n);
    }
    if (s.frameLimit != 0 && n >= s.frameLimit) {
        FUT_LOGI(Gfx, "FRAMES: limit of %u frames reached ([debug] frames / --frames)", s.frameLimit);
        // Orderly end (design 5.4). The statistics the M2 exit check reads (renderer, fiber switches, APC
        // deliveries, XDK calls, game state) are logged by the exit reporters of the modules
        // (core::AddExitReporter, run inside ExitHostProcess).
        core::ExitHostProcess(core::kExitOk, "frame limit reached");
    }
}

void CrashReport() noexcept {
    LogRendererStatistics();
}

// ---- NiXBoxRenderer::Create detour -------------------------------------------------------------------

std::uint32_t CreateRenderer(std::uint32_t width, std::uint32_t height, std::uint32_t backBufferFormat,
                             std::uint32_t backBufferCount, std::uint32_t flags, std::uint32_t depthFormat,
                             std::uint32_t refreshRate, std::uint32_t presentInterval, void* from) {
    Tables* t = GetTables();
    const std::uint32_t device = d3d::DummyDevice();
    if (t == nullptr || device == 0) {
        FUT_LOGE(Gfx, "NiXBoxRenderer::Create: cannot allocate the NiPCRenderer tables - returning NULL");
        return 0;
    }
    const std::uint32_t addr = GameNew(guest::kNiXBoxRendererSize);
    if (addr == 0) {
        FUT_LOGE(Gfx, "NiXBoxRenderer::Create: operator new(0x%X) failed - returning NULL", guest::kNiXBoxRendererSize);
        return 0;
    }
    core::CallGuestThiscall(guest::kNiRendererBaseCtor, addr);  // NiObject ctor, vtable NiRenderer, +8 = 0, list
    // The base ctor appended a node {renderer, next, prev} as the new tail (+8) of the list 0x2D7B18 that
    // the create/purge broadcasts walk (renderer.md 1.2, Verifikation 7).
    const std::uint32_t listCount = Read(guest::kNiRendererList);
    const std::uint32_t tail = Read(guest::kNiRendererList + 8);
    if (tail == 0 || Read(tail) != addr) {
        FUT_LOGW(Gfx, "NiXBoxRenderer::Create: NiPCRenderer 0x%08X is not the tail of the renderer list 0x%08X "
                 "(tail node 0x%08X) - broadcasts will not reach it", addr, guest::kNiRendererList, tail);
    }
    auto* r = guest::Ptr<NiPCRenderer>(addr);
    r->vtable = guest::Addr(t->renderer);
    r->device = device;
    auto* host = new HostRenderer;
    host->self = addr;
    host->width = width != 0 ? width : 640;
    host->height = height != 0 ? height : 480;
    host->backBufferFormat = backBufferFormat;
    host->backBufferCount = backBufferCount;
    host->flags = flags;
    host->depthFormat = depthFormat;
    host->refreshRate = refreshRate;
    host->presentInterval = presentInterval;
    r->host = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(host));
    g_currentRenderer.store(addr);
    static std::once_flag reporter;
    std::call_once(reporter, [] {
        core::AddCrashReporter("gfx", &CrashReport);
        core::AddExitReporter("gfx", &LogRendererStatistics);
    });
    const Settings& s = GetSettings();
    FUT_LOGI(Gfx, "NiXBoxRenderer::Create(%u, %u, %u, %u, 0x%08X, 0x%X, %u, %u) from %s -> NiPCRenderer 0x%08X "
             "(null renderer: vtable 0x%08X, device 0x%08X, renderer list %u, pacing %u Hz, frame limit %u, "
             "d3d traps %s)", width, height, backBufferFormat, backBufferCount, flags, depthFormat, refreshRate,
             presentInterval, SymbolizeSafe(from).c_str(), addr, guest::Addr(t->renderer), device, listCount,
             s.frameRate, s.frameLimit, s.trapMode == TrapMode::Abort ? "abort" : "log");
    return addr;
}

}  // namespace

// NiXBoxRenderer::Create 0x14C170 - cdecl, 8 args (call site 0xD26BE..0xD26E7 pushes 8 DWORDs, then
// `add esp,0x20`; renderer.md 1.4): (width, height, backBufferFormat, backBufferCount, flags,
// depthFormat, refreshRate, presentInterval) -> NiRenderer* or NULL (FUN_000D26A0 then returns false).
// Replaced by the NiPCRenderer null renderer (design 2.2, E6).
std::uint32_t __cdecl NiXBoxRendererCreateHle(std::uint32_t width, std::uint32_t height, std::uint32_t backBufferFormat,
                                              std::uint32_t backBufferCount, std::uint32_t flags,
                                              std::uint32_t depthFormat, std::uint32_t refreshRate,
                                              std::uint32_t presentInterval) noexcept {
    void* const from = _ReturnAddress();
    FUT_HLE_TRY {
        return CreateRenderer(width, height, backBufferFormat, backBufferCount, flags, depthFormat, refreshRate,
                              presentInterval, from);
    }
    FUT_HLE_CATCH("NiXBoxRenderer::Create")
}
FUT_HOOK_EX(guest::kNiXBoxRendererCreate, &NiXBoxRendererCreateHle, Cdecl, "NiXBoxRenderer::Create",
            "64 A1 00 00 00 00", true);

// ---- host API -------------------------------------------------------------------------------------------

const std::uint32_t* RendererVtable() noexcept {
    Tables* t = GetTables();
    return t != nullptr ? t->renderer : nullptr;
}
const std::uint32_t* SourceTextureDataVtable() noexcept {
    Tables* t = GetTables();
    return t != nullptr ? t->sourceData : nullptr;
}
const std::uint32_t* RenderedTextureDataVtable() noexcept {
    Tables* t = GetTables();
    return t != nullptr ? t->renderedData : nullptr;
}

std::uint32_t CurrentRenderer() noexcept {
    return g_currentRenderer.load();
}

std::uint32_t FramesDisplayed() noexcept {
    return g_framesDisplayed.load();
}

std::uint32_t SlotCallCount(std::uint32_t slot) noexcept {
    return slot < kRendererSlotCount ? g_slotCalls[slot].load(std::memory_order_relaxed) : 0;
}

void LogRendererStatistics() noexcept {
    FUT_LOGI(
        Gfx,
        "NiPCRenderer: %u frames displayed; texture data: %u source + %u rendered created, %u destroyed; "
        "%u texture files read; default 8x8 texture for %u without image, %u unreadable, %u not a power "
        "of two, %u not loaded",
        g_framesDisplayed.load(), g_sourceDataCreated.load(), g_renderedDataCreated.load(),
        g_dataDestroyed.load(), g_textureFilesRead.load(), g_defaultNoImage.load(),
        g_defaultUnreadable.load(), g_defaultNotPow2.load(), g_defaultNotLoaded.load());
    char line[512];
    std::size_t used = 0;
    line[0] = 0;
    for (std::uint32_t s = 0; s < kRendererSlotCount; ++s) {
        const std::uint32_t n = g_slotCalls[s].load(std::memory_order_relaxed);
        if (n == 0) {
            continue;
        }
        const int w = std::snprintf(line + used, sizeof(line) - used, " %u:%u", s, n);
        if (w < 0) {
            break;
        }
        used += static_cast<std::size_t>(w);
        if (used > sizeof(line) - 24) {
            FUT_LOGI(Gfx, "NiPCRenderer slot calls (slot:count):%s", line);
            used = 0;
            line[0] = 0;
        }
    }
    if (used != 0) {
        FUT_LOGI(Gfx, "NiPCRenderer slot calls (slot:count):%s", line);
    }
    d3d::LogShimState();
    d3d::LogTrapStatistics();
}

}  // namespace gfx
