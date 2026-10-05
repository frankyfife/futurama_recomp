// Phase-2 validation harness (nipc_validate.h).
#include "gfx/nipc_validate.h"

#include "core/guest.h"
#include "core/log.h"
#include "gfx/gfx_config.h"
#include "gfx/nipc_renderer.h"
#include "gfx/texconv.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace gfx::validate {
namespace {

constexpr std::uint32_t kKinds = 6;     // ni::DrawKind
constexpr std::uint32_t kStatuses = 4;  // ni::ExtractStatus
constexpr std::uint32_t kConvertStatuses = 6;
constexpr std::uint32_t kFormats = static_cast<std::uint32_t>(rhi::Format::Count);

const char* const kKindNames[kKinds] = {"Shape", "Triangles", "TriStrip", "TriStrips", "ScreenPolygon",
                                        "ScreenTexture"};

struct State {
    ni::ExtractArena arena;
    ni::StateShadow shadow;
    ni::CameraDesc camera;
    bool haveCamera = false;
    std::atomic<std::uint32_t> cameras{0};
    std::atomic<std::uint32_t> draws[kKinds][kStatuses] = {};
    std::atomic<bool> reported[kKinds][kStatuses] = {};
    std::atomic<std::uint32_t> textures[kConvertStatuses] = {};
    std::atomic<std::uint32_t> textureFormats[kFormats] = {};
    std::atomic<bool> textureReported[16] = {};  // per NiPixelFormat::format (non-Ok results)
    std::atomic<std::uint32_t> sourceFormats[16][5] = {};  // NiPixelFormat::format x bpp 8/16/24/32/other
    std::atomic<std::uint64_t> arenaPeak{0};
};

State& S() noexcept {
    static State state;
    return state;
}

bool InTraceFrame(std::uint32_t* frame) noexcept {
    const std::vector<std::uint32_t>& frames = GetSettings().traceFrames;
    if (frames.empty()) {
        return false;
    }
    const std::uint32_t current = FramesDisplayed() + 1;
    *frame = current;
    return std::binary_search(frames.begin(), frames.end(), current);
}

ni::ExtractContext Context(const TargetInfo& target) noexcept {
    State& s = S();
    if (!s.haveCamera) {
        s.camera = ni::DefaultCamera(target.width, target.height);
        s.haveCamera = true;
    }
    ni::ExtractContext c;
    c.arena = &s.arena;
    c.camera = &s.camera;
    c.targetWidth = target.width;
    c.targetHeight = target.height;
    c.frame = FramesDisplayed();
    c.leftRightSwap = target.leftRightSwap;
    c.shadow = &s.shadow;
    return c;
}

void Count(ni::DrawKind kind, std::uint32_t object, ni::ExtractStatus status, const ni::DrawItem& item) noexcept {
    State& s = S();
    const auto k = static_cast<std::uint32_t>(kind);
    const auto st = static_cast<std::uint32_t>(status);
    if (k >= kKinds || st >= kStatuses) {
        return;
    }
    s.draws[k][st].fetch_add(1, std::memory_order_relaxed);
    if (status != ni::ExtractStatus::Ok && status != ni::ExtractStatus::Empty && !s.reported[k][st].exchange(true)) {
        FUT_LOGI(Gfx, "validate: first %s -> %s (object 0x%08X, frame %u)", kKindNames[k],
                 ni::ExtractStatusName(status), object, FramesDisplayed() + 1);
    }
    std::uint32_t frame = 0;
    if (InTraceFrame(&frame)) {
        const ni::GeometryDesc& g = item.geometry;
        FUT_LOGI(Gfx,
                 "validate frame %u: %s 0x%08X -> %s: %u vertices, %u indices, %u primitives, %u stages "
                 "(stage 0 texture 0x%08X), lighting %d, %u lights, blend %d, z func %u write %d, cull %u",
                 frame, kKindNames[k], object, ni::ExtractStatusName(status), g.vertexCount, g.indexCount,
                 g.primitiveCount, item.state.stageCount, item.state.stages[0].niTexture,
                 item.state.lightingEnable ? 1 : 0, item.lights.count, item.state.alphaBlendEnable ? 1 : 0,
                 static_cast<unsigned>(item.state.zFunc), item.state.zWriteEnable ? 1 : 0,
                 static_cast<unsigned>(item.state.cullMode));
    }
}

}  // namespace

void OnSetCameraData(std::uint32_t location, std::uint32_t direction, std::uint32_t up, std::uint32_t right,
                     std::uint32_t frustum, std::uint32_t port, const TargetInfo& target) noexcept {
    if (location == 0 || direction == 0 || up == 0 || right == 0 || frustum == 0 || port == 0) {
        return;
    }
    ni::CameraInput in;
    in.location = guest::Ptr<const float>(location);
    in.direction = guest::Ptr<const float>(direction);
    in.up = guest::Ptr<const float>(up);
    in.right = guest::Ptr<const float>(right);
    in.frustum = guest::Ptr<const ni::NiFrustum>(frustum);
    in.port = guest::Ptr<const ni::NiRectF>(port);
    in.targetWidth = target.width;
    in.targetHeight = target.height;
    in.leftRightSwap = target.leftRightSwap;
    State& s = S();
    s.camera = ni::ExtractCamera(in);
    s.haveCamera = true;
    s.cameras.fetch_add(1, std::memory_order_relaxed);
}

void OnGeometry(ni::DrawKind kind, std::uint32_t data, std::uint32_t skin, std::uint32_t transform,
                std::uint32_t bound, std::uint32_t propertyState, std::uint32_t effectState,
                const TargetInfo& target) noexcept {
    const ni::ExtractContext c = Context(target);
    ni::GeometryInput in;
    in.data = data;
    in.skinInstance = skin;
    in.transform = transform;
    in.bound = bound;
    in.propertyState = propertyState;
    in.effectState = effectState;
    static ni::DrawItem item;  // guest main thread only; too large for every slot's stack frame
    item = ni::DrawItem{};
    ni::ExtractStatus status = ni::ExtractStatus::Unsupported;
    switch (kind) {
    case ni::DrawKind::Shape:
        status = ni::ExtractShape(c, in, &item);
        break;
    case ni::DrawKind::Triangles:
        status = ni::ExtractTriangles(c, in, &item);
        break;
    case ni::DrawKind::TriStrip:
        status = ni::ExtractTriStrip(c, in, &item);
        break;
    case ni::DrawKind::TriStrips:
        status = ni::ExtractStrips(c, in, &item);
        break;
    default:
        break;
    }
    Count(kind, data, status, item);
}

void OnScreenPolygon(std::uint32_t polygon, const TargetInfo& target) noexcept {
    const ni::ExtractContext c = Context(target);
    static ni::DrawItem item;
    item = ni::DrawItem{};
    Count(ni::DrawKind::ScreenPolygon, polygon, ni::ExtractScreenPolygon(c, polygon, &item), item);
}

void OnScreenTexture(std::uint32_t screenTexture, const TargetInfo& target) noexcept {
    const ni::ExtractContext c = Context(target);
    static ni::DrawItem item;
    item = ni::DrawItem{};
    Count(ni::DrawKind::ScreenTexture, screenTexture, ni::ExtractScreenTexture(c, screenTexture, &item), item);
}

void OnSourceTexture(std::uint32_t pixelData) noexcept {
    if (pixelData == 0) {
        return;
    }
    State& s = S();
    tex::HostImage image;
    const tex::ConvertStatus status = tex::ConvertPixelData(pixelData, 0, tex::ConvertOptions{}, &image);
    const auto st = static_cast<std::uint32_t>(status);
    if (st < kConvertStatuses) {
        s.textures[st].fetch_add(1, std::memory_order_relaxed);
    }
    const auto f = static_cast<std::uint32_t>(image.format);
    if (status == tex::ConvertStatus::Ok && f < kFormats) {
        s.textureFormats[f].fetch_add(1, std::memory_order_relaxed);
    }
    const auto* pd = guest::Ptr<const ni::NiPixelData>(pixelData);
    const std::uint32_t niFormat = pd->format.format;
    const std::uint32_t bpp = pd->format.bitsPerPixel;
    const std::uint32_t bppIndex = bpp == 8 ? 0 : bpp == 16 ? 1 : bpp == 24 ? 2 : bpp == 32 ? 3 : 4;
    if (niFormat < 16) {
        s.sourceFormats[niFormat][bppIndex].fetch_add(1, std::memory_order_relaxed);
    }
    if (status != tex::ConvertStatus::Ok && niFormat < 16 && !s.textureReported[niFormat].exchange(true)) {
        FUT_LOGI(Gfx, "validate: first texconv result '%s' for NiPixelFormat %u (%u bpp, NiPixelData 0x%08X)",
                 tex::ConvertStatusName(status), niFormat, static_cast<unsigned>(pd->format.bitsPerPixel), pixelData);
    }
}

void OnFrameDisplayed(std::uint32_t displayed) noexcept {
    State& s = S();
    const std::uint64_t used = s.arena.BytesUsed();
    if (used > s.arenaPeak.load(std::memory_order_relaxed)) {
        s.arenaPeak.store(used, std::memory_order_relaxed);
    }
    s.arena.Reset();
    (void)displayed;
}

void LogSummary() noexcept {
    State& s = S();
    for (std::uint32_t k = 0; k < kKinds; ++k) {
        std::uint32_t total = 0;
        for (std::uint32_t st = 0; st < kStatuses; ++st) {
            total += s.draws[k][st].load(std::memory_order_relaxed);
        }
        if (total == 0) {
            continue;
        }
        FUT_LOGI(Gfx, "validate: extract %s: %u calls - ok %u, empty %u, unsupported %u, error %u", kKindNames[k],
                 total, s.draws[k][0].load(), s.draws[k][1].load(), s.draws[k][2].load(), s.draws[k][3].load());
    }
    char formats[256];
    std::size_t used = 0;
    formats[0] = 0;
    for (std::uint32_t f = 0; f < kFormats; ++f) {
        const std::uint32_t n = s.textureFormats[f].load(std::memory_order_relaxed);
        if (n == 0) {
            continue;
        }
        const int w = std::snprintf(formats + used, sizeof(formats) - used, " %s:%u",
                                    rhi::GetFormatInfo(static_cast<rhi::Format>(f)).name, n);
        if (w < 0 || static_cast<std::size_t>(w) >= sizeof(formats) - used) {
            break;
        }
        used += static_cast<std::size_t>(w);
    }
    char sources[256];
    std::size_t usedSources = 0;
    sources[0] = 0;
    static const char* const kBpp[5] = {"8", "16", "24", "32", "?"};
    for (std::uint32_t f = 0; f < 16; ++f) {
        for (std::uint32_t b = 0; b < 5; ++b) {
            const std::uint32_t n = s.sourceFormats[f][b].load(std::memory_order_relaxed);
            if (n == 0) {
                continue;
            }
            const int w = std::snprintf(sources + usedSources, sizeof(sources) - usedSources, " %u/%s:%u", f,
                                        kBpp[b], n);
            if (w < 0 || static_cast<std::size_t>(w) >= sizeof(sources) - usedSources) {
                break;
            }
            usedSources += static_cast<std::size_t>(w);
        }
    }
    FUT_LOGI(Gfx,
             "validate: %u cameras; texconv: ok %u, no pixel data %u, unsupported %u, no palette %u, bad data %u, "
             "out of memory %u; source formats (NiPixelFormat/bpp:count):%s; host formats:%s; arena peak %llu bytes",
             s.cameras.load(), s.textures[0].load(), s.textures[1].load(), s.textures[2].load(), s.textures[3].load(),
             s.textures[4].load(), s.textures[5].load(), usedSources != 0 ? sources : " none",
             used != 0 ? formats : " none", static_cast<unsigned long long>(s.arenaPeak.load()));
}

}  // namespace gfx::validate
