// D3D shim: the 8 D3D8 functions game code calls directly (see d3d_shim.h).
#include "gfx/d3d_shim.h"

#include "core/guest.h"
#include "core/hooks.h"
#include "core/log.h"
#include "gfx/gfx_abi.h"
#include "gfx/guest_memory.h"
#include "gfx/purge_probe.h"

#include <intrin.h>

#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace gfx::d3d {
namespace {

// Xbox resource header (layout of the original D3D code, see d3d_shim.h).
struct XboxResource {
    std::uint32_t common;
    std::uint32_t data;
    std::uint32_t lock;
    std::uint32_t format;
    std::uint32_t size;
    std::uint32_t parent;  // surfaces only
};
static_assert(sizeof(XboxResource) == 0x18 && sizeof(XboxResource) <= kGuestBlockSize);
static_assert(offsetof(XboxResource, format) == 0x0C && offsetof(XboxResource, parent) == 0x14);

constexpr std::uint32_t kCommonRefMask = 0xFFFF;
constexpr std::uint32_t kCommonTypeMask = 0x70000;
constexpr std::uint32_t kCommonTypeTexture = 0x40000;
constexpr std::uint32_t kCommonTypeSurface = 0x50000;
constexpr std::uint32_t kCommonIntRefMask = 0x780000;  // Release destroys only if these are 0
constexpr std::uint32_t kCommonD3DCreated = 0x1000000;
constexpr std::uint32_t kTextureCommonInit = kCommonD3DCreated | kCommonTypeTexture | 1;  // 0x1040001 (0x237163)
constexpr std::uint32_t kSurfaceCommonInit = kCommonD3DCreated | kCommonTypeSurface | 1;  // 0x1050001 (0x23CE80)
static_assert(kTextureCommonInit == 0x1040001 && kSurfaceCommonInit == 0x1050001);

constexpr std::uint32_t kResourceTypeTexture = 3;  // D3DRTYPE_TEXTURE (CreateTexture2 arg 7; 4 volume, 5 cube)
constexpr std::uint32_t kDummyDeviceSize = 0x2040;  // 0x810 DWORDs (rep stosd at 0x23448A)

// ASSUMPTION (XDK d3d8types.h, from memory): Xbox D3DFORMAT codes -> bits per pixel and linear flag.
struct FormatInfo {
    std::uint32_t format;
    std::uint32_t bitsPerPixel;
    bool linear;
    bool compressed;  // DXT: 4x4 blocks
};
constexpr FormatInfo kFormats[] = {
    {0x00, 8, false, false},  {0x01, 8, false, false},  {0x02, 16, false, false}, {0x03, 16, false, false},
    {0x04, 16, false, false}, {0x05, 16, false, false}, {0x06, 32, false, false}, {0x07, 32, false, false},
    {0x0B, 8, false, false},  {0x0C, 4, false, true},   {0x0E, 8, false, true},   {0x0F, 8, false, true},
    {0x10, 16, true, false},  {0x11, 16, true, false},  {0x12, 32, true, false},  {0x13, 8, true, false},
    {0x19, 8, false, false},  {0x1A, 16, false, false}, {0x1C, 16, true, false},  {0x1D, 16, true, false},
    {0x1E, 32, true, false},  {0x1F, 8, true, false},   {0x20, 16, true, false},  {0x24, 16, true, false},
    {0x25, 16, true, false},  {0x27, 16, false, false}, {0x28, 16, false, false}, {0x2A, 32, false, false},
    {0x2C, 16, false, false},
};

const FormatInfo* FindFormat(std::uint32_t format) noexcept {
    for (const FormatInfo& f : kFormats) {
        if (f.format == format) {
            return &f;
        }
    }
    return nullptr;
}

std::uint32_t Log2(std::uint32_t v) noexcept {
    std::uint32_t r = 0;
    while (v > 1) {
        v >>= 1;
        ++r;
    }
    return r;
}

std::uint32_t LevelDim(std::uint32_t dim, std::uint32_t level) noexcept {
    const std::uint32_t d = level < 32 ? dim >> level : 0;
    return d != 0 ? d : 1;
}

std::uint32_t LevelPitch(const FormatInfo& f, std::uint32_t width) noexcept {
    if (f.compressed) {
        return ((width + 3) / 4) * (f.bitsPerPixel * 16 / 8);  // bytes per row of 4x4 blocks
    }
    return (width * f.bitsPerPixel + 7) / 8;
}

std::uint32_t LevelBytes(const FormatInfo& f, std::uint32_t width, std::uint32_t height) noexcept {
    const std::uint32_t rows = f.compressed ? (height + 3) / 4 : height;
    return LevelPitch(f, width) * rows;
}

// ASSUMPTION: XDK D3DFORMAT field encoding (DMACHANNEL_A 1, DIMENSION 2 << 4, FORMAT << 8,
// MIPMAP << 16, USIZE << 20, VSIZE << 24) and D3DSIZE for linear formats.
std::uint32_t EncodeFormat(const FormatInfo& f, std::uint32_t width, std::uint32_t height,
                           std::uint32_t levels) noexcept {
    std::uint32_t v = 1u | (2u << 4) | (f.format << 8) | ((levels & 0xF) << 16);
    if (!f.linear) {
        v |= (Log2(width) & 0xF) << 20;
        v |= (Log2(height) & 0xF) << 24;
    }
    return v;
}
std::uint32_t EncodeSize(const FormatInfo& f, std::uint32_t width, std::uint32_t height,
                         std::uint32_t pitch) noexcept {
    if (!f.linear) {
        return 0;
    }
    const std::uint32_t pitchUnits = pitch >= 64 ? pitch / 64 - 1 : 0;
    return ((width - 1) & 0xFFF) | (((height - 1) & 0xFFF) << 12) | ((pitchUnits & 0xFF) << 24);
}

struct Resource {
    ResourceKind kind;
    std::uint32_t width, height, levels, format, pitch;
    void* pages;  // textures: pixel allocation (AllocGuestPages)
    std::uint32_t pixels, pixelBytes;
    std::uint32_t parent;
    std::uint32_t serial = 0;
    std::uint32_t generation = 0;  // textures: content generation (NoteResourceWritten)
};

std::recursive_mutex g_mutex;  // recursive: Release of a surface releases its parent
std::map<std::uint32_t, Resource> g_resources;
std::set<std::uint32_t> g_loggedUnknown;
std::uint32_t g_deviceRefCount = 1;  // the renderer holds the device it "created" (Direct3D_CreateDevice)
bool g_yuvEnable = false;
float g_screenOffsetX = 0.0f, g_screenOffsetY = 0.0f;
std::uint32_t g_texturesCreated = 0, g_surfacesCreated = 0, g_ignoredReleases = 0;
std::uint32_t g_nextSerial = 1;
bool g_loggedNullAddRef = false, g_loggedNullRelease = false;

XboxResource* Header(std::uint32_t addr) noexcept {
    return guest::Ptr<XboxResource>(addr);
}

// Logs a NULL/unknown resource pointer once (design 2.2: "unbekannte Zeiger und NULL ignorieren
// (einmal loggen)"). Caller holds g_mutex.
void NoteIgnored(bool addRef, std::uint32_t resource, void* returnAddress) {
    const char* function = addRef ? "D3DResource_AddRef" : "D3DResource_Release";
    ++g_ignoredReleases;
    if (resource == 0) {
        bool& logged = addRef ? g_loggedNullAddRef : g_loggedNullRelease;
        if (!logged) {
            logged = true;
            FUT_LOGW(Gfx, "%s(NULL) from %s ignored (logged once)", function,
                     guest::Symbolize(guest::Addr(returnAddress)).c_str());
        }
        return;
    }
    if (g_loggedUnknown.size() < 16 && g_loggedUnknown.insert(resource).second) {
        FUT_LOGW(Gfx, "%s(0x%08X) from %s: not a shim resource - ignored (logged once per pointer)", function,
                 resource, guest::Symbolize(guest::Addr(returnAddress)).c_str());
    }
}

void Destroy(std::uint32_t header, Resource& r) noexcept {
    FUT_LOGD(Gfx, "D3D shim: destroy %s 0x%08X (%ux%u fmt 0x%02X)",
             r.kind == ResourceKind::Texture ? "texture" : "surface", header, r.width, r.height, r.format);
    if (r.kind == ResourceKind::Texture) {
        FreeGuestPages(r.pages);
    }
    FreeGuestBlock(Header(header));
    g_resources.erase(header);
}

std::uint32_t AddRefLocked(std::uint32_t resource) noexcept {
    auto it = g_resources.find(resource);
    if (it == g_resources.end()) {
        return 0;
    }
    XboxResource* h = Header(resource);
    // 0x2373B0: a surface whose count goes 0 -> 1 takes a reference on its parent again.
    if ((h->common & kCommonRefMask) == 0 && (h->common & kCommonTypeMask) == kCommonTypeSurface && h->parent != 0) {
        AddRefLocked(h->parent);
    }
    h->common += 1;
    return h->common & kCommonRefMask;
}

std::uint32_t ReleaseLocked(std::uint32_t resource) noexcept {
    auto it = g_resources.find(resource);
    if (it == g_resources.end()) {
        return 0;
    }
    XboxResource* h = Header(resource);
    std::uint32_t common = h->common;
    // 0x2373F0: the last reference of a surface releases the parent, then the resource is destroyed
    // unless internal references (bits 19..22) remain.
    if ((common & kCommonRefMask) == 1) {
        if ((common & kCommonTypeMask) == kCommonTypeSurface && h->parent != 0) {
            ReleaseLocked(h->parent);
        }
        common = h->common;
        if ((common & kCommonIntRefMask) == 0) {
            Destroy(resource, it->second);
            return 0;
        }
    }
    h->common = common - 1;
    return (common - 1) & kCommonRefMask;
}

}  // namespace

std::uint32_t __stdcall DeviceAddRef() noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ++g_deviceRefCount;
    FUT_LOGT(Gfx, "D3DDevice_AddRef() -> %u", g_deviceRefCount);
    return g_deviceRefCount;
}
FUT_HOOK_EX(guest::kD3DDeviceAddRef, &DeviceAddRef, Stdcall, "D3D8::D3DDevice_AddRef", "A1 68 49 24 00", true);
static_assert(abi::StdcallPurge<decltype(&DeviceAddRef)>() == 0);

std::uint32_t __stdcall DeviceRelease() noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_deviceRefCount > 0) {
        --g_deviceRefCount;
    }
    FUT_LOGT(Gfx, "D3DDevice_Release() -> %u", g_deviceRefCount);
    return g_deviceRefCount;
}
FUT_HOOK_EX(guest::kD3DDeviceRelease, &DeviceRelease, Stdcall, "D3D8::D3DDevice_Release", "57 8B 3D 68 49", true);
static_assert(abi::StdcallPurge<decltype(&DeviceRelease)>() == 0);

std::uint32_t __stdcall DeviceCreateTexture2(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                             std::uint32_t levels, std::uint32_t usage, std::uint32_t format,
                                             std::uint32_t resourceType) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    void* const from = _ReturnAddress();
    FUT_HLE_TRY {
        const FormatInfo* f = FindFormat(format);
        if (resourceType != kResourceTypeTexture || depth > 1 || width == 0 || height == 0 || width > 4096 ||
            height > 4096 || levels > 13 || f == nullptr) {
            FUT_LOGW(Gfx, "D3DDevice_CreateTexture2(%u, %u, %u, %u, 0x%X, fmt 0x%X, type %u) from %s: unsupported - "
                     "returning NULL", width, height, depth, levels, usage, format, resourceType,
                     guest::Symbolize(guest::Addr(from)).c_str());
            return 0;
        }
        if (levels == 0) {
            levels = 1;  // ASSUMPTION: 0 = full chain on the PC; the game always passes 1
        }
        std::uint32_t total = 0;
        for (std::uint32_t l = 0; l < levels; ++l) {
            total += LevelBytes(*f, LevelDim(width, l), LevelDim(height, l));
        }
        void* pages = AllocGuestPages(total);
        auto* h = static_cast<XboxResource*>(AllocGuestBlock(sizeof(XboxResource)));
        if (pages == nullptr || h == nullptr) {
            FreeGuestPages(pages);
            FreeGuestBlock(h);
            FUT_LOGE(Gfx, "D3DDevice_CreateTexture2(%u, %u, fmt 0x%X): out of guest memory", width, height, format);
            return 0;
        }
        const std::uint32_t pitch = LevelPitch(*f, width);
        h->common = kTextureCommonInit;
        h->data = guest::Addr(pages);
        h->lock = 0;
        h->format = EncodeFormat(*f, width, height, levels);
        h->size = EncodeSize(*f, width, height, pitch);
        const std::uint32_t addr = guest::Addr(h);
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            g_resources[addr] = Resource{ResourceKind::Texture, width, height, levels, format, pitch, pages,
                                         guest::Addr(pages), total, 0, g_nextSerial++, 0};
            ++g_texturesCreated;
        }
        FUT_LOGI(Gfx, "D3DDevice_CreateTexture2(%u, %u, %u, %u, 0x%X, fmt 0x%02X%s, type %u) from %s -> 0x%08X "
                 "(%u bytes at 0x%08X)", width, height, depth, levels, usage, format,
                 format == kFormatYuy2 ? " YUY2" : "", resourceType, guest::Symbolize(guest::Addr(from)).c_str(),
                 addr, total, guest::Addr(pages));
        return addr;
    }
    FUT_HLE_CATCH("D3DDevice_CreateTexture2")
}
FUT_HOOK_EX(guest::kD3DDeviceCreateTexture2, &DeviceCreateTexture2, Stdcall, "D3D8::D3DDevice_CreateTexture2",
            "56 57 33 D2 8D", true);
static_assert(abi::StdcallPurge<decltype(&DeviceCreateTexture2)>() == 0x1C);

std::uint32_t __stdcall TextureGetSurfaceLevel2(std::uint32_t texture, std::uint32_t level) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    void* const from = _ReturnAddress();
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_resources.find(texture);
    if (it == g_resources.end() || it->second.kind != ResourceKind::Texture || level >= it->second.levels) {
        FUT_LOGW(Gfx, "D3DTexture_GetSurfaceLevel2(0x%08X, %u) from %s: no such shim texture level - NULL", texture,
                 level, guest::Symbolize(guest::Addr(from)).c_str());
        return 0;
    }
    const Resource& t = it->second;
    const FormatInfo* f = FindFormat(t.format);
    std::uint32_t offset = 0;
    for (std::uint32_t l = 0; l < level; ++l) {
        offset += LevelBytes(*f, LevelDim(t.width, l), LevelDim(t.height, l));
    }
    const std::uint32_t w = LevelDim(t.width, level), h = LevelDim(t.height, level);
    auto* s = static_cast<XboxResource*>(AllocGuestBlock(sizeof(XboxResource)));
    if (s == nullptr) {
        FUT_LOGE(Gfx, "D3DTexture_GetSurfaceLevel2: out of guest memory");
        return 0;
    }
    const std::uint32_t pitch = LevelPitch(*f, w);
    s->common = kSurfaceCommonInit;
    s->data = t.pixels + offset;
    s->lock = 0;
    s->format = EncodeFormat(*f, w, h, 1);
    s->size = EncodeSize(*f, w, h, pitch);
    s->parent = texture;
    const std::uint32_t addr = guest::Addr(s);
    try {
        g_resources[addr] = Resource{ResourceKind::Surface, w, h, level, t.format, pitch, nullptr, t.pixels + offset,
                                     LevelBytes(*f, w, h), texture, g_nextSerial++, 0};
    } catch (...) {
        FreeGuestBlock(s);
        return 0;
    }
    ++g_surfacesCreated;
    AddRefLocked(texture);  // FUN_0023CE60: D3DResource_AddRef(parent)
    FUT_LOGT(Gfx, "D3DTexture_GetSurfaceLevel2(0x%08X, %u) -> 0x%08X", texture, level, addr);
    return addr;
}
FUT_HOOK_EX(guest::kD3DTextureGetSurfaceLevel2, &TextureGetSurfaceLevel2, Stdcall, "D3D8::D3DTexture_GetSurfaceLevel2",
            "83 EC 0C 56 8B", true);
static_assert(abi::StdcallPurge<decltype(&TextureGetSurfaceLevel2)>() == 8);

std::uint32_t __stdcall ResourceAddRef(std::uint32_t resource) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (resource == 0 || g_resources.find(resource) == g_resources.end()) {
        NoteIgnored(true, resource, _ReturnAddress());
        return 0;
    }
    return AddRefLocked(resource);
}
FUT_HOOK_EX(guest::kD3DResourceAddRef, &ResourceAddRef, Stdcall, "D3D8::D3DResource_AddRef", "56 8B 74 24 08", true);
static_assert(abi::StdcallPurge<decltype(&ResourceAddRef)>() == 4);

std::uint32_t __stdcall ResourceRelease(std::uint32_t resource) noexcept {
    if (InPurgeProbe()) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (resource == 0 || g_resources.find(resource) == g_resources.end()) {
        NoteIgnored(false, resource, _ReturnAddress());
        return 0;
    }
    return ReleaseLocked(resource);
}
FUT_HOOK_EX(guest::kD3DResourceRelease, &ResourceRelease, Stdcall, "D3D8::D3DResource_Release", "56 8B 74 24 08",
            true);
static_assert(abi::StdcallPurge<decltype(&ResourceRelease)>() == 4);

void __stdcall DeviceSetRenderStateYuvEnable(std::uint32_t enable) noexcept {
    if (InPurgeProbe()) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_yuvEnable = enable != 0;
    FUT_LOGT(Gfx, "D3DDevice_SetRenderState_YuvEnable(%u)", enable);
}
FUT_HOOK_EX(guest::kD3DDeviceSetRenderStateYuvEnable, &DeviceSetRenderStateYuvEnable, Stdcall,
            "D3D8::D3DDevice_SetRenderState_YuvEnable", "8B 44 24 04 56", true);
static_assert(abi::StdcallPurge<decltype(&DeviceSetRenderStateYuvEnable)>() == 4);

void __stdcall DeviceSetScreenSpaceOffset(float x, float y) noexcept {
    if (InPurgeProbe()) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (x != g_screenOffsetX || y != g_screenOffsetY) {
        FUT_LOGD(Gfx, "D3DDevice_SetScreenSpaceOffset(%.2f, %.2f) from %s", static_cast<double>(x),
                 static_cast<double>(y), guest::Symbolize(guest::Addr(_ReturnAddress())).c_str());
    }
    g_screenOffsetX = x;
    g_screenOffsetY = y;
}
FUT_HOOK_EX(guest::kD3DDeviceSetScreenSpaceOffset, &DeviceSetScreenSpaceOffset, Stdcall,
            "D3D8::D3DDevice_SetScreenSpaceOffset", "D9 05 B0 21 24 00", true);
static_assert(abi::StdcallPurge<decltype(&DeviceSetScreenSpaceOffset)>() == 8);

// ---- host API ---------------------------------------------------------------------------------------

bool LookupResource(std::uint32_t header, ResourceInfo& out) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_resources.find(header);
    if (it == g_resources.end()) {
        return false;
    }
    const Resource& r = it->second;
    out = ResourceInfo{r.kind,     header,  r.width,  r.height,     r.levels,
                       r.format,   r.pitch, r.pixels, r.pixelBytes, r.parent,
                       Header(header)->common & kCommonRefMask};
    out.serial = r.serial;
    out.generation = r.generation;
    if (r.kind == ResourceKind::Surface) {
        const auto p = g_resources.find(r.parent);
        out.generation = p != g_resources.end() ? p->second.generation : 0;
    }
    return true;
}

std::uint32_t NoteResourceWritten(std::uint32_t header) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_resources.find(header);
    if (it == g_resources.end()) {
        return 0;
    }
    if (it->second.kind == ResourceKind::Surface) {
        it = g_resources.find(it->second.parent);
        if (it == g_resources.end()) {
            return 0;
        }
    }
    if (++it->second.generation == 0) {
        it->second.generation = 1;
    }
    return it->second.generation;
}

std::uint32_t DummyDevice() noexcept {
    static std::once_flag once;
    static std::uint32_t device = 0;
    try {
        std::call_once(once, [] { device = guest::Addr(AllocGuestPages(kDummyDeviceSize)); });
    } catch (...) {
        return 0;
    }
    return device;
}

ShimState GetShimState() noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return ShimState{g_deviceRefCount,  g_yuvEnable,       g_screenOffsetX,
                     g_screenOffsetY,   g_texturesCreated, g_surfacesCreated,
                     static_cast<std::uint32_t>(g_resources.size()), g_ignoredReleases};
}

void LogShimState() noexcept {
    const ShimState s = GetShimState();
    FUT_LOGI(Gfx, "D3D shim: device refs %u, textures created %u, surfaces created %u, live resources %u, "
             "ignored AddRef/Release %u, YuvEnable %u, screen offset (%.1f, %.1f)", s.deviceRefCount,
             s.texturesCreated, s.surfacesCreated, s.liveResources, s.ignoredReleases, s.yuvEnable ? 1u : 0u,
             static_cast<double>(s.screenOffsetX), static_cast<double>(s.screenOffsetY));
}

bool GetShimEntry(std::uint32_t index, ShimEntry& out) noexcept {
    static const ShimEntry kEntries[] = {
        {core::detail::CodeAddress(&DeviceAddRef), guest::kD3DDeviceAddRef, 0, "D3D8::D3DDevice_AddRef"},
        {core::detail::CodeAddress(&DeviceRelease), guest::kD3DDeviceRelease, 0, "D3D8::D3DDevice_Release"},
        {core::detail::CodeAddress(&DeviceCreateTexture2), guest::kD3DDeviceCreateTexture2, 7,
         "D3D8::D3DDevice_CreateTexture2"},
        {core::detail::CodeAddress(&TextureGetSurfaceLevel2), guest::kD3DTextureGetSurfaceLevel2, 2,
         "D3D8::D3DTexture_GetSurfaceLevel2"},
        {core::detail::CodeAddress(&ResourceAddRef), guest::kD3DResourceAddRef, 1, "D3D8::D3DResource_AddRef"},
        {core::detail::CodeAddress(&ResourceRelease), guest::kD3DResourceRelease, 1, "D3D8::D3DResource_Release"},
        {core::detail::CodeAddress(&DeviceSetRenderStateYuvEnable), guest::kD3DDeviceSetRenderStateYuvEnable, 1,
         "D3D8::D3DDevice_SetRenderState_YuvEnable"},
        {core::detail::CodeAddress(&DeviceSetScreenSpaceOffset), guest::kD3DDeviceSetScreenSpaceOffset, 2,
         "D3D8::D3DDevice_SetScreenSpaceOffset"},
    };
    if (index >= std::size(kEntries)) {
        return false;
    }
    out = kEntries[index];
    return true;
}

}  // namespace gfx::d3d
