// GPU path of NiPCRenderer (nipc_gpu.h; GFX_CONTRACT.md 2, 9; design 6 M3).
#include "gfx/nipc_gpu.h"

#include "core/log.h"
#include "gfx/gfx_config.h"
#include "gfx/rhi/d3d11/d3d11_device.h"
#include "platform/frame_pacer.h"
#include "platform/screenshot.h"
#include "platform/window.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gfx {

const char* DrawOutcomeName(DrawOutcome outcome) noexcept {
    switch (outcome) {
    case DrawOutcome::Drawn:
        return "Drawn";
    case DrawOutcome::Skipped:
        return "Skipped";
    case DrawOutcome::Failed:
        return "Failed";
    case DrawOutcome::Empty:
        return "Empty";
    case DrawOutcome::Unsupported:
        return "Unsupported";
    case DrawOutcome::Error:
        return "Error";
    case DrawOutcome::NotInScene:
        return "NotInScene";
    case DrawOutcome::TextureTarget:
        return "TextureTarget";
    case DrawOutcome::Count:
        break;
    }
    return "?";
}

const char* DrawKindName(ni::DrawKind kind) noexcept {
    switch (kind) {
    case ni::DrawKind::Shape:
        return "Shape";
    case ni::DrawKind::Triangles:
        return "Triangles";
    case ni::DrawKind::TriStrip:
        return "TriStrip";
    case ni::DrawKind::TriStrips:
        return "TriStrips";
    case ni::DrawKind::ScreenPolygon:
        return "ScreenPolygon";
    case ni::DrawKind::ScreenTexture:
        return "ScreenTexture";
    }
    return "?";
}

namespace {

const char* TexturePathName(ni::TexturePath path) noexcept {
    switch (path) {
    case ni::TexturePath::SingleStage:
        return "single";
    case ni::TexturePath::MultiMap:
        return "multimap";
    case ni::TexturePath::Screen:
        return "screen";
    }
    return "?";
}

DrawOutcome OutcomeOf(ni::ExtractStatus status) noexcept {
    switch (status) {
    case ni::ExtractStatus::Ok:
        return DrawOutcome::Drawn;
    case ni::ExtractStatus::Empty:
        return DrawOutcome::Empty;
    case ni::ExtractStatus::Unsupported:
        return DrawOutcome::Unsupported;
    case ni::ExtractStatus::Error:
        return DrawOutcome::Error;
    }
    return DrawOutcome::Error;
}

DrawOutcome OutcomeOf(ff::DrawResult result) noexcept {
    switch (result) {
    case ff::DrawResult::Drawn:
        return DrawOutcome::Drawn;
    case ff::DrawResult::Skipped:
        return DrawOutcome::Skipped;
    case ff::DrawResult::Failed:
        return DrawOutcome::Failed;
    }
    return DrawOutcome::Failed;
}

bool Listed(const std::vector<std::uint32_t>& frames, std::uint32_t frame) noexcept {
    return std::binary_search(frames.begin(), frames.end(), frame);
}

}  // namespace

void HalfPixelShift(Matrix4* projection, float viewportWidth, float viewportHeight) noexcept {
    if (projection == nullptr || !(viewportWidth > 0.0f) || !(viewportHeight > 0.0f)) {
        return;
    }
    const float dx = 1.0f / viewportWidth;
    const float dy = -1.0f / viewportHeight;
    for (int i = 0; i < 4; ++i) {
        projection->m[i][0] += projection->m[i][3] * dx;
        projection->m[i][1] += projection->m[i][3] * dy;
    }
}

struct GpuRenderer::Impl {
    std::unique_ptr<rhi::Device> device;
    ff::FixedFunctionPipeline ff;
    ni::ExtractArena arena;
    ni::StateShadow shadow;
    ni::DrawItem item;  // one draw at a time (guest main thread); too large for the slot stack frames
    ni::CameraDesc camera;
    rhi::Viewport viewport;
    std::uint32_t backBufferWidth = 640, backBufferHeight = 480;
    std::uint32_t targetWidth = 640, targetHeight = 480;
    bool textureTarget = false;
    bool leftRightSwap = false;
    ni::TextureResolver resolveTexture = nullptr;
    void* resolveUser = nullptr;
    rhi::TextureHandle defaultTexture, blackTexture;

    // Frame state: frames displayed so far, whether the current frame is traced / summarized.
    std::uint32_t framesDone = 0;
    bool traceFrame = false;
    bool summaryFrame = false;
    std::uint32_t drawIndex = 0;  // draws of the current frame (trace numbering)

    // Statistics.
    std::uint64_t counts[kDrawKindCount][kDrawOutcomeCount] = {};
    std::uint32_t frameCounts[kDrawKindCount][kDrawOutcomeCount] = {};
    bool reported[kDrawKindCount][kDrawOutcomeCount] = {};
    std::uint64_t clears = 0, clearsSkipped = 0, emptyClearRects = 0, cameras = 0, scenes = 0, presents = 0,
                  occluded = 0, screenshots = 0, texturesCreated = 0, texturesDestroyed = 0,
                  textureFailures = 0;
    std::uint32_t frameClears = 0;
    std::size_t arenaPeak = 0;
    bool loggedPresentFailure = false;
    bool loggedTextureFailure = false;

    rhi::CommandContext& Ctx() noexcept { return device->Context(); }

    rhi::Viewport FullViewport() const noexcept {
        rhi::Viewport v;
        v.x = 0.0f;
        v.y = 0.0f;
        v.width = static_cast<float>(targetWidth);
        v.height = static_cast<float>(targetHeight);
        v.minDepth = 0.0f;
        v.maxDepth = 1.0f;
        return v;
    }

    // D3DDevice_SetViewport + the pipeline's copy of it (pre-transformed pixels map relative to it).
    void ApplyViewport(const rhi::Viewport& v) noexcept {
        viewport = v;
        Ctx().SetViewport(v);
        ni::CameraDesc c = camera;
        c.viewport = v;
        if (v.width > 0.0f && v.height > 0.0f) {
            HalfPixelShift(&c.projection, v.width, v.height);  // design 8.3 B19
            HalfPixelShift(&c.screenProjection, v.width, v.height);
        }
        ff.SetCamera(c, targetWidth, targetHeight);
    }

    ni::ExtractContext Context() noexcept {
        ni::ExtractContext c;
        c.arena = &arena;
        c.camera = &camera;
        c.targetWidth = targetWidth;
        c.targetHeight = targetHeight;
        c.frame = framesDone;
        c.leftRightSwap = leftRightSwap;
        c.resolveTexture = resolveTexture;
        c.resolveUser = resolveUser;
        c.shadow = &shadow;
        return c;
    }

    void Count(ni::DrawKind kind, ni::GuestPtr object, DrawOutcome outcome, bool haveItem) noexcept;
    void Submit(ni::DrawKind kind, ni::GuestPtr object, ni::ExtractStatus status) noexcept;
    void TraceDraw(ni::DrawKind kind, ni::GuestPtr object, DrawOutcome outcome, bool haveItem) noexcept;
    void LogFrameSummary(std::uint32_t frame) noexcept;
    void PrepareFrame(std::uint32_t frame) noexcept;
};

void GpuRenderer::Impl::TraceDraw(ni::DrawKind kind, ni::GuestPtr object, DrawOutcome outcome,
                                  bool haveItem) noexcept {
    const std::uint32_t frame = framesDone + 1;
    const char* note =
        outcome == DrawOutcome::Skipped || outcome == DrawOutcome::Failed ? ff.LastDrawNote() : "";
    if (!haveItem) {
        FUT_LOGI(Gfx, "draw trace frame %u #%u: %s 0x%08X -> %s", frame, drawIndex, DrawKindName(kind),
                 object, DrawOutcomeName(outcome));
        return;
    }
    const ni::GeometryDesc& g = item.geometry;
    const ni::RenderStateDesc& s = item.state;
    const ni::TextureStageDesc& st0 = s.stages[0];
    FUT_LOGI(
        Gfx,
        "draw trace frame %u #%u: %s 0x%08X -> %s%s%s: %u vertices, %u indices, %u triangles (%s%s%s%s), "
        "%u stages (%s; stage 0 op %u/%u, NiTexture 0x%08X -> host %u, addr %u/%u, filter %u/%u/%u, uv %u), "
        "lighting %d (%u lights, "
        "sources d%u a%u e%u), colors %d, blend %d (%u/%u), alpha test %d (func %u ref %u), z func %u write "
        "%d, "
        "cull %u, fog %d, material diffuse (%.2f %.2f %.2f %.2f) ambient (%.2f %.2f %.2f) emissive (%.2f "
        "%.2f "
        "%.2f), global ambient (%.2f %.2f %.2f)",
        frame, drawIndex, DrawKindName(kind), object, DrawOutcomeName(outcome), *note != 0 ? ": " : "", note,
        g.vertexCount, g.indexCount, g.primitiveCount,
        g.topology == ni::Topology::TriangleStrip ? "strip" : "list",
        g.pretransformed ? ", pre-transformed" : "", item.useScreenCamera ? ", screen camera" : "",
        g.skinned ? ", skinned" : "", s.stageCount, TexturePathName(s.texturePath),
        static_cast<unsigned>(st0.colorOp), static_cast<unsigned>(st0.alphaOp), st0.niTexture, st0.texture.id,
        static_cast<unsigned>(st0.addressU), static_cast<unsigned>(st0.addressV),
        static_cast<unsigned>(st0.minFilter), static_cast<unsigned>(st0.magFilter),
        static_cast<unsigned>(st0.mipFilter), st0.texCoordIndex, s.lightingEnable ? 1 : 0, item.lights.count,
        static_cast<unsigned>(s.diffuseSource), static_cast<unsigned>(s.ambientSource),
        static_cast<unsigned>(s.emissiveSource), g.colors.data != nullptr ? 1 : 0, s.alphaBlendEnable ? 1 : 0,
        static_cast<unsigned>(s.srcBlend), static_cast<unsigned>(s.dstBlend), s.alphaTestEnable ? 1 : 0,
        static_cast<unsigned>(s.alphaFunc), static_cast<unsigned>(s.alphaRef), static_cast<unsigned>(s.zFunc),
        s.zWriteEnable ? 1 : 0, static_cast<unsigned>(s.cullMode), s.fogEnable ? 1 : 0, s.material.diffuse.x,
        s.material.diffuse.y, s.material.diffuse.z, s.material.diffuse.w, s.material.ambient.x,
        s.material.ambient.y, s.material.ambient.z, s.material.emissive.x, s.material.emissive.y,
        s.material.emissive.z, item.lights.ambient.x, item.lights.ambient.y, item.lights.ambient.z);
}

void GpuRenderer::Impl::Count(ni::DrawKind kind, ni::GuestPtr object, DrawOutcome outcome,
                              bool haveItem) noexcept {
    const auto k = static_cast<std::uint32_t>(kind);
    const auto o = static_cast<std::uint32_t>(outcome);
    if (k >= kDrawKindCount || o >= kDrawOutcomeCount) {
        return;
    }
    ++counts[k][o];
    ++frameCounts[k][o];
    ++drawIndex;
    if (outcome != DrawOutcome::Drawn && outcome != DrawOutcome::Empty &&
        outcome != DrawOutcome::NotInScene && !reported[k][o]) {
        reported[k][o] = true;
        const char* note =
            outcome == DrawOutcome::Skipped || outcome == DrawOutcome::Failed ? ff.LastDrawNote() : "";
        // Error = inconsistent guest data (ni_extract.h: logged by the caller at GFX/warn); the rest is
        // information (Failed was already logged as an error by the pipeline).
        if (outcome == DrawOutcome::Error) {
            FUT_LOGW(Gfx, "d3d11: first %s draw -> %s (object 0x%08X, frame %u)", DrawKindName(kind),
                     DrawOutcomeName(outcome), object, framesDone + 1);
        } else {
            FUT_LOGI(Gfx, "d3d11: first %s draw -> %s%s%s (object 0x%08X, frame %u)", DrawKindName(kind),
                     DrawOutcomeName(outcome), *note != 0 ? ": " : "", note, object, framesDone + 1);
        }
    }
    if (traceFrame) {
        TraceDraw(kind, object, outcome, haveItem);
    }
}

void GpuRenderer::Impl::Submit(ni::DrawKind kind, ni::GuestPtr object, ni::ExtractStatus status) noexcept {
    if (status != ni::ExtractStatus::Ok) {
        Count(kind, object, OutcomeOf(status), false);
        return;
    }
    const ff::DrawResult result = ff.Draw(item);
    Count(kind, object, OutcomeOf(result), true);
}

void GpuRenderer::Impl::LogFrameSummary(std::uint32_t frame) noexcept {
    char line[768];
    std::size_t used = 0;
    line[0] = 0;
    std::uint32_t issued = 0, drawn = 0;
    for (std::uint32_t k = 0; k < kDrawKindCount; ++k) {
        std::uint32_t calls = 0;
        for (std::uint32_t o = 0; o < kDrawOutcomeCount; ++o) {
            calls += frameCounts[k][o];
        }
        if (calls == 0) {
            continue;
        }
        issued += calls;
        drawn += frameCounts[k][static_cast<std::uint32_t>(DrawOutcome::Drawn)];
        int w = std::snprintf(line + used, sizeof(line) - used, "%s%s %u (", used != 0 ? "; " : "",
                              DrawKindName(static_cast<ni::DrawKind>(k)), calls);
        if (w < 0 || static_cast<std::size_t>(w) >= sizeof(line) - used) {
            break;
        }
        used += static_cast<std::size_t>(w);
        bool firstOutcome = true;
        for (std::uint32_t o = 0; o < kDrawOutcomeCount; ++o) {
            if (frameCounts[k][o] == 0) {
                continue;
            }
            w = std::snprintf(line + used, sizeof(line) - used, "%s%s %u", firstOutcome ? "" : ", ",
                              DrawOutcomeName(static_cast<DrawOutcome>(o)), frameCounts[k][o]);
            if (w < 0 || static_cast<std::size_t>(w) >= sizeof(line) - used) {
                break;
            }
            used += static_cast<std::size_t>(w);
            firstOutcome = false;
        }
        w = std::snprintf(line + used, sizeof(line) - used, ")");
        if (w < 0 || static_cast<std::size_t>(w) >= sizeof(line) - used) {
            break;
        }
        used += static_cast<std::size_t>(w);
    }
    FUT_LOGI(Gfx, "draw summary frame %u: %u draw calls, %u drawn, %u not drawn, %u clears%s%s", frame,
             issued, drawn, issued - drawn, frameClears, used != 0 ? "; " : "", line);
}

void GpuRenderer::Impl::PrepareFrame(std::uint32_t frame) noexcept {
    const Settings& s = GetSettings();
    traceFrame = Listed(s.traceFrames, frame);
    summaryFrame = traceFrame || Listed(s.screenshotFrames, frame);
    drawIndex = 0;
    frameClears = 0;
    std::memset(frameCounts, 0, sizeof(frameCounts));
}

// ---- GpuRenderer
// ------------------------------------------------------------------------------------------------

GpuRenderer::GpuRenderer() noexcept = default;

GpuRenderer::~GpuRenderer() {
    if (impl_ != nullptr && impl_->device != nullptr) {
        if (impl_->defaultTexture) {
            impl_->device->Destroy(impl_->defaultTexture);
        }
        if (impl_->blackTexture) {
            impl_->device->Destroy(impl_->blackTexture);
        }
        impl_->ff.Shutdown();
    }
}

std::unique_ptr<GpuRenderer> GpuRenderer::Create(const CreateParams& params, std::string* error) noexcept {
    std::unique_ptr<GpuRenderer> r;
    try {
        r.reset(new GpuRenderer);
        r->impl_ = std::make_unique<Impl>();
    } catch (...) {
        if (error != nullptr) {
            *error = "out of memory";
        }
        return nullptr;
    }
    Impl& d = *r->impl_;
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::D3D11;
    desc.nativeWindow = params.window;
    desc.width = params.width;
    desc.height = params.height;
    desc.bufferCount = 2;
    desc.backBufferFormat = rhi::Format::BGRA8_UNorm;
    desc.depthFormat = rhi::Format::D24_UNorm_S8_UInt;  // Create gets D3DFMT_D24S8 (0x4B, auto depth/stencil)
    desc.debugLayer = params.debugLayer;
    desc.softwareAdapter = params.softwareAdapter;
    d.device = rhi::CreateDevice(desc, error);
    if (d.device == nullptr) {
        return nullptr;
    }
    if (!d.ff.Init(*d.device, error)) {
        return nullptr;
    }
    d.backBufferWidth = d.targetWidth = params.width;
    d.backBufferHeight = d.targetHeight = params.height;
    d.resolveTexture = params.resolveTexture;
    d.resolveUser = params.resolveUser;
    // Default texture of NiXBoxRenderer (FUN_0014AB50, created by the device setup FUN_0014C0F0).
    tex::HostImage image;
    tex::MakeDefaultTexture(&image);
    d.defaultTexture = r->CreateTexture(image, "NiPCRenderer default texture (FUN_0014AB50)");
    // Black 1x1: movie textures (the shim's YUY2 surface, contents from M6) and rendered textures (P2).
    static const std::uint8_t kBlack[4] = {0, 0, 0, 0xFF};
    rhi::TextureDesc black;
    black.width = 1;
    black.height = 1;
    black.mipLevels = 1;
    black.format = rhi::Format::BGRA8_UNorm;
    black.usage = rhi::kTextureSampled;
    black.debugName = "NiPCRenderer black texture";
    rhi::SubresourceData level;
    level.data = kBlack;
    level.rowPitch = 4;
    level.slicePitch = 4;
    d.blackTexture = d.device->CreateTexture(black, &level);
    if (!d.defaultTexture || !d.blackTexture) {
        if (error != nullptr) {
            try {
                *error = std::string("default texture creation failed: ") + d.device->LastError();
            } catch (...) {
            }
        }
        return nullptr;
    }
    // NiXBoxRenderer::Create ends with SetCameraData(defaults) (FUN_0014C170, ni::DefaultCamera); the back
    // buffer with its depth buffer is the render target.
    d.camera = ni::DefaultCamera(d.targetWidth, d.targetHeight);
    d.Ctx().SetRenderTargets({}, {});
    d.ApplyViewport(d.camera.viewport);
    d.shadow.Reset();
    d.PrepareFrame(1);
    return r;
}

rhi::Device& GpuRenderer::Device() noexcept {
    return *impl_->device;
}

std::string GpuRenderer::Describe() const {
    const rhi::DeviceInfo& info = impl_->device->Info();
    const rhi::DebugLayerInfo layer = impl_->device->DebugLayer();
    std::string text =
        info.adapterName + ", " + info.apiVersion + ", debug layer " + rhi::d3d11::DebugLayerStateName(layer);
    if (!layer.note.empty()) {
        text += " (" + layer.note + ")";
    }
    return text;
}

void GpuRenderer::SetBackBufferTarget() noexcept {
    Impl& d = *impl_;
    d.textureTarget = false;
    d.targetWidth = d.backBufferWidth;
    d.targetHeight = d.backBufferHeight;
    d.Ctx().SetRenderTargets({}, {});
    d.ApplyViewport(d.FullViewport());  // D3D8 SetRenderTarget: viewport = the whole new target
}

void GpuRenderer::SetTextureTarget(std::uint32_t width, std::uint32_t height) noexcept {
    Impl& d = *impl_;
    // P2 (GFX_CONTRACT.md 11): render to texture is not implemented; the back buffer stays bound and
    // every draw/clear until the next SetRenderTarget(NULL) is counted as TextureTarget.
    d.textureTarget = true;
    d.targetWidth = width;
    d.targetHeight = height;
}

void GpuRenderer::SetLeftRightSwap(bool swap) noexcept {
    impl_->leftRightSwap = swap;
}

void GpuRenderer::ClearBuffer(const ni::NiRectF* rect, std::uint32_t mode, std::uint32_t argb, float depth,
                              std::uint32_t stencil) noexcept {
    Impl& d = *impl_;
    ++d.clears;
    ++d.frameClears;
    if (d.textureTarget) {
        ++d.clearsSkipped;
        return;
    }
    // 0x146538: D3DDevice_SetViewport(whole target) before the clear; not restored.
    d.ApplyViewport(d.FullViewport());
    // 0x146558..: Xbox D3DCLEAR_TARGET (0xF0) for mode bit 1, D3DCLEAR_ZBUFFER for bit 4 and
    // D3DCLEAR_STENCIL for bit 2 - both only with a depth surface (the back buffer has the D24S8 one) and
    // stencil only with the caps bit 7 of +0x2D8 (GetFlags 0xED: set).
    std::uint8_t flags = 0;
    if ((mode & 1) != 0) {
        flags |= rhi::kClearColor;
    }
    if ((mode & 4) != 0) {
        flags |= rhi::kClearDepth;
    }
    if ((mode & 2) != 0) {
        flags |= rhi::kClearStencil;
    }
    if (flags == 0) {
        return;
    }
    const float color[4] = {
        static_cast<float>((argb >> 16) & 0xFF) / 255.0f, static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(argb & 0xFF) / 255.0f, static_cast<float>((argb >> 24) & 0xFF) / 255.0f};
    rhi::Rect r;
    const bool hasRect = ni::ClearRect(rect, d.targetWidth, d.targetHeight, &r);
    if (hasRect && (r.right <= r.left || r.bottom <= r.top)) {
        ++d.emptyClearRects;  // D3DDevice_Clear with an empty rectangle clears nothing
        return;
    }
    d.Ctx().Clear(flags, color, depth, static_cast<std::uint8_t>(stencil & 0xFF), hasRect ? &r : nullptr);
}

void GpuRenderer::BeginScene() noexcept {
    ++impl_->scenes;
    ni::BeginScene(&impl_->shadow);
}

void GpuRenderer::SetCameraData(const ni::CameraInput& input) noexcept {
    Impl& d = *impl_;
    ++d.cameras;
    ni::CameraInput in = input;
    in.targetWidth = d.targetWidth;
    in.targetHeight = d.targetHeight;
    in.leftRightSwap = d.leftRightSwap;
    d.camera = ni::ExtractCamera(in);
    rhi::Viewport v = d.camera.viewport;
    v.width = std::max(v.width, 0.0f);
    v.height = std::max(v.height, 0.0f);
    if (d.textureTarget) {
        d.camera.viewport = v;  // transforms kept for the draws (which are skipped on texture targets)
        return;
    }
    d.ApplyViewport(v);
}

void GpuRenderer::DrawGeometry(ni::DrawKind kind, const ni::GeometryInput& input) noexcept {
    Impl& d = *impl_;
    if (d.textureTarget) {
        d.Count(kind, input.data, DrawOutcome::TextureTarget, false);
        return;
    }
    const ni::ExtractContext c = d.Context();
    ni::ExtractStatus status = ni::ExtractStatus::Unsupported;
    switch (kind) {
    case ni::DrawKind::Shape:
        status = ni::ExtractShape(c, input, &d.item);
        break;
    case ni::DrawKind::Triangles:
        status = ni::ExtractTriangles(c, input, &d.item);
        break;
    case ni::DrawKind::TriStrip:
        status = ni::ExtractTriStrip(c, input, &d.item);
        break;
    case ni::DrawKind::TriStrips:
        status = ni::ExtractStrips(c, input, &d.item);
        break;
    default:
        break;
    }
    d.Submit(kind, input.data, status);
}

void GpuRenderer::DrawScreenPolygon(ni::GuestPtr polygon) noexcept {
    Impl& d = *impl_;
    if (d.textureTarget) {
        d.Count(ni::DrawKind::ScreenPolygon, polygon, DrawOutcome::TextureTarget, false);
        return;
    }
    d.Submit(ni::DrawKind::ScreenPolygon, polygon, ni::ExtractScreenPolygon(d.Context(), polygon, &d.item));
}

void GpuRenderer::DrawScreenTexture(ni::GuestPtr screenTexture) noexcept {
    Impl& d = *impl_;
    if (d.textureTarget) {
        d.Count(ni::DrawKind::ScreenTexture, screenTexture, DrawOutcome::TextureTarget, false);
        return;
    }
    d.Submit(ni::DrawKind::ScreenTexture, screenTexture,
             ni::ExtractScreenTexture(d.Context(), screenTexture, &d.item));
}

void GpuRenderer::CountNotInScene(ni::DrawKind kind, ni::GuestPtr object) noexcept {
    impl_->Count(kind, object, DrawOutcome::NotInScene, false);
}

void GpuRenderer::DisplayFrame(std::uint32_t frame) noexcept {
    Impl& d = *impl_;
    const Settings& s = GetSettings();
    if (d.summaryFrame) {
        d.LogFrameSummary(frame);
    }
    const bool hotkey = (platform::ConsumeHotkeys() & platform::kHotkeyScreenshot) != 0;
    if (hotkey || Listed(s.screenshotFrames, frame)) {
        ++d.screenshots;
        platform::SaveScreenshot(*d.device, s.screenshotDir, frame);  // logs "screenshot: <path> (WxH)"
    }
    rhi::PresentDesc present;
    present.syncInterval = s.vsync ? 1u : 0u;
    const rhi::PresentResult result = d.device->Present(present);
    ++d.presents;
    switch (result) {
    case rhi::PresentResult::Ok:
        break;
    case rhi::PresentResult::Occluded:
        ++d.occluded;
        break;
    case rhi::PresentResult::DeviceLost:
        FUT_LOGE(Gfx, "d3d11: device lost in Present of frame %u: %s", frame, d.device->LastError());
        core::Fatal("D3D11 device lost (frame %u): %s", frame, d.device->LastError());
    case rhi::PresentResult::Failed:
        if (!d.loggedPresentFailure) {
            d.loggedPresentFailure = true;
            FUT_LOGE(Gfx, "d3d11: Present of frame %u failed: %s", frame, d.device->LastError());
        }
        break;
    }
    platform::NotePresentedFrame();  // first call: shows the window, registers the "present" reporter
    d.arenaPeak = std::max(d.arenaPeak, d.arena.BytesUsed());
    d.arena.Reset();
    d.framesDone = frame;
    d.PrepareFrame(frame + 1);
}

rhi::TextureHandle GpuRenderer::CreateTexture(const tex::HostImage& image, const char* debugName) noexcept {
    Impl& d = *impl_;
    rhi::SubresourceData levels[tex::kMaxMipLevels];
    const std::uint32_t count = tex::MakeSubresources(image, levels, tex::kMaxMipLevels);
    const rhi::TextureDesc desc = tex::MakeTextureDesc(image, debugName);
    const rhi::TextureHandle handle =
        count == desc.mipLevels ? d.device->CreateTexture(desc, levels) : rhi::TextureHandle{};
    if (!handle) {
        ++d.textureFailures;
        if (!d.loggedTextureFailure) {
            d.loggedTextureFailure = true;
            FUT_LOGE(Gfx, "d3d11: texture creation failed (%ux%u, %u levels, %s): %s", image.width,
                     image.height, image.mipCount, rhi::GetFormatInfo(image.format).name,
                     d.device->LastError());
        }
        return {};
    }
    ++d.texturesCreated;
    return handle;
}

void GpuRenderer::DestroyTexture(rhi::TextureHandle texture) noexcept {
    Impl& d = *impl_;
    if (!texture || texture == d.defaultTexture || texture == d.blackTexture) {
        return;
    }
    ++d.texturesDestroyed;
    d.device->Destroy(texture);
}

rhi::TextureHandle GpuRenderer::DefaultTexture() const noexcept {
    return impl_->defaultTexture;
}

rhi::TextureHandle GpuRenderer::BlackTexture() const noexcept {
    return impl_->blackTexture;
}

void GpuRenderer::LogStatistics() noexcept {
    Impl& d = *impl_;
    for (std::uint32_t k = 0; k < kDrawKindCount; ++k) {
        std::uint64_t calls = 0;
        for (std::uint32_t o = 0; o < kDrawOutcomeCount; ++o) {
            calls += d.counts[k][o];
        }
        if (calls == 0) {
            continue;
        }
        const auto& c = d.counts[k];
        FUT_LOGI(
            Gfx,
            "d3d11 draws %s: %llu calls - drawn %llu, skipped %llu, failed %llu, empty %llu, unsupported "
            "%llu, error %llu, not in scene %llu, texture target %llu",
            DrawKindName(static_cast<ni::DrawKind>(k)), static_cast<unsigned long long>(calls),
            static_cast<unsigned long long>(c[0]), static_cast<unsigned long long>(c[1]),
            static_cast<unsigned long long>(c[2]), static_cast<unsigned long long>(c[3]),
            static_cast<unsigned long long>(c[4]), static_cast<unsigned long long>(c[5]),
            static_cast<unsigned long long>(c[6]), static_cast<unsigned long long>(c[7]));
    }
    FUT_LOGI(
        Gfx,
        "d3d11 frames: %llu presents (%llu occluded), %llu screenshots, %llu scenes, %llu cameras, %llu "
        "clears "
        "(%llu on texture targets skipped, %llu empty rectangles); textures %llu created, %llu destroyed, "
        "%llu failed; arena peak %zu bytes",
        static_cast<unsigned long long>(d.presents), static_cast<unsigned long long>(d.occluded),
        static_cast<unsigned long long>(d.screenshots), static_cast<unsigned long long>(d.scenes),
        static_cast<unsigned long long>(d.cameras), static_cast<unsigned long long>(d.clears),
        static_cast<unsigned long long>(d.clearsSkipped), static_cast<unsigned long long>(d.emptyClearRects),
        static_cast<unsigned long long>(d.texturesCreated),
        static_cast<unsigned long long>(d.texturesDestroyed),
        static_cast<unsigned long long>(d.textureFailures), d.arenaPeak);
    const ff::PipelineStats f = d.ff.Stats();
    FUT_LOGI(
        Gfx,
        "ff pipeline: %llu draws, %llu skipped, %llu failed, %llu vertices, %llu MiB uploaded, %u shader "
        "permutations, %u pipeline states, %llu lit, %llu pre-transformed, uploads b0 %llu b2 %llu b3 %llu, "
        "%llu stages without host texture",
        static_cast<unsigned long long>(f.draws), static_cast<unsigned long long>(f.skipped),
        static_cast<unsigned long long>(f.failed), static_cast<unsigned long long>(f.vertices),
        static_cast<unsigned long long>(f.uploadBytes >> 20), f.permutations, f.pipelineStates,
        static_cast<unsigned long long>(f.litDraws), static_cast<unsigned long long>(f.pretransformedDraws),
        static_cast<unsigned long long>(f.frameUploads), static_cast<unsigned long long>(f.lightUploads),
        static_cast<unsigned long long>(f.stageUploads), static_cast<unsigned long long>(f.missingTextures));
    const ni::ExtractCounters& e = d.shadow.counters;
    FUT_LOGI(
        Gfx,
        "ni_extract: %llu states, %llu multi-map approximated, %llu with texture effects, %llu light "
        "evaluations (%llu lights dropped, %llu unknown types), %llu skinned draws (%llu deforms, %llu cache "
        "hits), %llu invalid map modes, %llu unresolved textures",
        static_cast<unsigned long long>(e.states), static_cast<unsigned long long>(e.multiMapApproximated),
        static_cast<unsigned long long>(e.textureEffectDraws),
        static_cast<unsigned long long>(e.lightEvaluations), static_cast<unsigned long long>(e.lightsDropped),
        static_cast<unsigned long long>(e.unknownLightTypes), static_cast<unsigned long long>(e.skinnedDraws),
        static_cast<unsigned long long>(e.skinDeforms), static_cast<unsigned long long>(e.skinCacheHits),
        static_cast<unsigned long long>(e.invalidMapModes),
        static_cast<unsigned long long>(e.unresolvedTextures));
    const rhi::DeviceStats ds = d.device->Stats();
    FUT_LOGI(Gfx,
             "d3d11 device: %llu draws, %llu primitives, %llu presents, %llu MiB transient, %u pipelines, %u "
             "samplers, %u buffers, %u textures (%llu KiB), %u debug errors, %u debug warnings",
             static_cast<unsigned long long>(ds.draws), static_cast<unsigned long long>(ds.primitives),
             static_cast<unsigned long long>(ds.presents),
             static_cast<unsigned long long>(ds.transientBytes >> 20), ds.pipelines, ds.samplers, ds.buffers,
             ds.textures, static_cast<unsigned long long>(ds.textureBytes >> 10), ds.debugErrors,
             ds.debugWarnings);
}

}  // namespace gfx
