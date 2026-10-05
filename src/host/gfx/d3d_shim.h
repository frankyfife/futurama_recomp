// D3D shim (design 2.2 table, E6): native replacements for the 8 Xbox D3D8 functions that game code
// calls directly - the movie player 0xEDE40..0xEE7FA (device from NiRenderer+0x60, YUY2 texture,
// YuvEnable) and the screen centering 0xA1920..0xA1AE0 (SetScreenSpaceOffset) - d3d.md 4.3,
// renderer.md 6/7.1. All other D3D entries with external callers are traps (d3d_traps.cpp).
//
// Resources are fake Xbox D3D objects in guest-visible memory with the header layout the original
// D3D code builds (D3DDevice_CreateTexture2 0x237160..0x237178, FUN_0023CE60 for surfaces):
//   +0x00 Common  bits 0..15 refcount, 16..18 type (4 texture, 5 surface), 19..22 internal refs,
//                 24 D3DCREATED (0x1040001 / 0x1050001 at creation)
//   +0x04 Data    ASSUMPTION: guest address of the pixels (the original stores a physical address;
//                 only D3D/REN code, which never runs, interprets it)
//   +0x08 Lock    0
//   +0x0C Format  ASSUMPTION: XDK D3DFORMAT encoding (DMA channel A, 2D, format << 8, mip count << 16,
//                 log2 sizes for swizzled formats)
//   +0x10 Size    ASSUMPTION: XDK D3DSIZE for linear formats (width-1 | height-1 << 12 | pitch/64-1 << 24)
//   +0x14 Parent  surfaces only: owning texture (AddRef'ed, released with the surface)
// Every resource is listed in a host registry (LookupResource), so a later renderer (M3) or the XMV
// HLE (M6) can find the pixels of a movie texture. AddRef/Release follow the original refcount rules
// (0x2373B0/0x2373F0); unknown pointers and NULL are ignored and logged once.
#pragma once

#include <cstdint>

namespace gfx::d3d {

// --- the 8 shim functions (stdcall, purge = ret N of the original, checked by static_assert) -----
// D3D8::D3DDevice_AddRef 0x234440 - stdcall, no args (ret): ++device refcount, returns it.
std::uint32_t __stdcall DeviceAddRef() noexcept;
// D3D8::D3DDevice_Release 0x234460 - stdcall, no args (ret): --refcount (never below 0, the device is
// never destroyed), returns it.
std::uint32_t __stdcall DeviceRelease() noexcept;
// D3D8::D3DDevice_CreateTexture2 0x2370D0 - stdcall 7 args (ret 0x1C): (Width, Height, Depth, Levels,
// Usage, Format, D3DResourceType 3 = texture) -> texture header or NULL. Game call 0xEE220:
// (w, h, 1, 1, 0, 0x24 = YUY2, 3).
std::uint32_t __stdcall DeviceCreateTexture2(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                             std::uint32_t levels, std::uint32_t usage, std::uint32_t format,
                                             std::uint32_t resourceType) noexcept;
// D3D8::D3DTexture_GetSurfaceLevel2 0x2371A0 - stdcall 2 args (ret 8): (texture, level) -> surface
// with refcount 1 that holds a reference on the texture (FUN_0023CE60), NULL for unknown textures.
std::uint32_t __stdcall TextureGetSurfaceLevel2(std::uint32_t texture, std::uint32_t level) noexcept;
// D3D8::D3DResource_AddRef 0x2373B0 - stdcall 1 arg (ret 4) -> new refcount (low 16 bits).
std::uint32_t __stdcall ResourceAddRef(std::uint32_t resource) noexcept;
// D3D8::D3DResource_Release 0x2373F0 - stdcall 1 arg (ret 4) -> remaining refcount; destroys at 0.
std::uint32_t __stdcall ResourceRelease(std::uint32_t resource) noexcept;
// D3D8::D3DDevice_SetRenderState_YuvEnable 0x236DC0 - stdcall 1 arg (ret 4): flag for the renderer.
void __stdcall DeviceSetRenderStateYuvEnable(std::uint32_t enable) noexcept;
// D3D8::D3DDevice_SetScreenSpaceOffset 0x2354F0 - stdcall 2 floats (ret 8): screen offset in pixels
// (game clamps it to +-12, d3d.md 4.3) for the renderer.
void __stdcall DeviceSetScreenSpaceOffset(float x, float y) noexcept;

// --- host API ------------------------------------------------------------------------------------
enum class ResourceKind : std::uint8_t { Texture, Surface };

struct ResourceInfo {
    ResourceKind kind = ResourceKind::Texture;
    std::uint32_t header = 0;     // guest address of the Xbox header
    std::uint32_t width = 0;      // of level 0 (texture) or of the level (surface)
    std::uint32_t height = 0;
    std::uint32_t levels = 0;     // texture: mip levels; surface: its level index
    std::uint32_t format = 0;     // Xbox D3DFORMAT (0x24 = YUY2)
    std::uint32_t pitch = 0;      // bytes per row of level 0 / of the surface's level
    std::uint32_t pixels = 0;     // guest address of the pixels
    std::uint32_t pixelBytes = 0; // texture: whole mip chain; surface: its level
    std::uint32_t parent = 0;     // surface: texture header
    std::uint32_t refCount = 0;   // Common & 0xFFFF
};
bool LookupResource(std::uint32_t header, ResourceInfo& out) noexcept;

// Guest-visible dummy IDirect3DDevice8 (0x2040 zero bytes, the size the original device memset in
// D3DDevice_Release 0x23447E) for NiPCRenderer+0x60; never dereferenced by the shim. 0 on failure.
std::uint32_t DummyDevice() noexcept;

struct ShimState {
    std::uint32_t deviceRefCount;
    bool yuvEnable;
    float screenOffsetX, screenOffsetY;
    std::uint32_t texturesCreated, surfacesCreated, liveResources;
    std::uint32_t ignoredReleases;  // NULL / unknown pointers passed to AddRef/Release
};
ShimState GetShimState() noexcept;
// Logs the shim state on GFX/info (frame-limit exit, crash reporter).
void LogShimState() noexcept;

// Entry points for the purge self-test: index 0..7 -> {code address, guest address, stack DWORDs,
// name}. Returns false past the end.
struct ShimEntry {
    const void* entry;
    std::uint32_t guestAddress;
    std::uint32_t stackDwords;
    const char* name;
};
bool GetShimEntry(std::uint32_t index, ShimEntry& out) noexcept;

}  // namespace gfx::d3d
