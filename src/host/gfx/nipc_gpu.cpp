// GPU path of NiPCRenderer (nipc_gpu.h; GFX_CONTRACT.md 2, 9; design 6 M3).
#include "gfx/nipc_gpu.h"

#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"
#include "gfx/gfx_config.h"
#include "gfx/interp_history.h"
#include "gfx/movie_uv.h"
#include "gfx/outline.h"
#include "gfx/rhi/d3d11/d3d11_device.h"
#include "gfx/widescreen.h"
#include "platform/frame_pacer.h"
#include "platform/screenshot.h"
#include "platform/window.h"

#include <algorithm>
#include <cmath>
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
    case ni::DrawKind::Points:
        return "Points";
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

RenderQuality RenderQualityOf(const Settings& settings) noexcept {
    RenderQuality q;
    q.resolutionWidth = settings.resolutionWidth;
    q.resolutionHeight = settings.resolutionHeight;
    q.renderScale = settings.renderScale;
    q.msaa = settings.msaa;
    q.alphaToCoverage = settings.alphaToCoverage;
    q.anisotropy = settings.anisotropy;
    q.screenshots = settings.screenshotSize;
    q.linearAa = settings.linearAa;
    return q;
}

QualityLadder QualityFallbacks(const RenderQuality& requested) noexcept {
    QualityLadder ladder;
    auto add = [&ladder](const RenderQuality& q) noexcept {
        if (ladder.count < QualityLadder::kMaxSteps) {
            ladder.steps[ladder.count++] = q;
        }
    };
    RenderQuality q = requested;
    add(q);
    if (q.renderScale > 1.0f) {
        q.renderScale = 1.0f;
        add(q);
    }
    while (q.msaa > 1) {
        q.msaa /= 2;
        add(q);
    }
    if (q.resolutionWidth != 640 || q.resolutionHeight != 480) {
        q.resolutionWidth = 640;
        q.resolutionHeight = 480;
        add(q);
    }
    return ladder;
}

std::string DescribeQuality(const RenderQuality& quality) {
    char text[96];
    char resolution[32];
    if (quality.resolutionWidth == 0) {
        std::snprintf(resolution, sizeof(resolution), "auto");
    } else {
        std::snprintf(resolution, sizeof(resolution), "%ux%u", quality.resolutionWidth, quality.resolutionHeight);
    }
    std::snprintf(text, sizeof(text), "render scale %.2f, MSAA %ux, resolution %s",
                  static_cast<double>(quality.renderScale), quality.msaa, resolution);
    return text;
}

RenderSizes ComputeRenderSizes(std::uint32_t guestWidth, std::uint32_t guestHeight, std::uint32_t resolutionWidth,
                               std::uint32_t resolutionHeight, std::uint32_t viewWidth, std::uint32_t viewHeight,
                               float renderScale, std::uint32_t aspectW, std::uint32_t aspectH) noexcept {
    std::uint32_t gw = guestWidth != 0 ? guestWidth : 640;
    std::uint32_t gh = guestHeight != 0 ? guestHeight : 480;
    if (aspectW != 0 && aspectH != 0) {  // G3: the display aspect (the fitted rectangle below uses gw x gh)
        gw = aspectW;
        gh = aspectH;
    }
    std::uint32_t tw = resolutionWidth, th = resolutionHeight;
    if (tw == 0 || th == 0) {
        tw = viewWidth;
        th = viewHeight;
    }
    if (tw == 0 || th == 0) {
        tw = gw;
        th = gh;
    }
    const platform::ViewRect fit = platform::FitAspectRect(std::min(tw, kMaxRenderDimension),
                                                           std::min(th, kMaxRenderDimension), gw, gh);
    RenderSizes s;
    s.outputWidth = fit.width != 0 ? fit.width : gw;
    s.outputHeight = fit.height != 0 ? fit.height : gh;
    // NaN-safe clamp of the scale (the configuration validated 1..2).
    double k = renderScale >= kMinRenderScale ? static_cast<double>(renderScale) : 1.0;
    k = std::min(k, static_cast<double>(kMaxRenderScale));
    const double limit = std::min(static_cast<double>(kMaxRenderDimension) / s.outputWidth,
                                  static_cast<double>(kMaxRenderDimension) / s.outputHeight);
    if (k > limit) {
        k = std::max(1.0, limit);
    }
    s.internalWidth = static_cast<std::uint32_t>(std::floor(s.outputWidth * k + 0.5));
    s.internalHeight = static_cast<std::uint32_t>(std::floor(s.outputHeight * k + 0.5));
    s.internalWidth = std::clamp<std::uint32_t>(s.internalWidth, s.outputWidth, kMaxRenderDimension);
    s.internalHeight = std::clamp<std::uint32_t>(s.internalHeight, s.outputHeight, kMaxRenderDimension);
    s.renderScale = static_cast<float>(k);
    return s;
}

rhi::Viewport ScaleViewport(const rhi::Viewport& viewport, std::uint32_t guestW, std::uint32_t guestH,
                            std::uint32_t internalW, std::uint32_t internalH) noexcept {
    if ((guestW == internalW && guestH == internalH) || guestW == 0 || guestH == 0) {
        return viewport;
    }
    const double sx = static_cast<double>(internalW) / guestW;
    const double sy = static_cast<double>(internalH) / guestH;
    rhi::Viewport v = viewport;
    v.x = static_cast<float>(viewport.x * sx);
    v.y = static_cast<float>(viewport.y * sy);
    v.width = static_cast<float>(viewport.width * sx);
    v.height = static_cast<float>(viewport.height * sy);
    return v;
}

rhi::Rect ScaleRect(const rhi::Rect& rect, std::uint32_t guestW, std::uint32_t guestH, std::uint32_t internalW,
                    std::uint32_t internalH) noexcept {
    if ((guestW == internalW && guestH == internalH) || guestW == 0 || guestH == 0) {
        return rect;
    }
    auto scale = [](std::int32_t v, std::uint32_t from, std::uint32_t to) noexcept {
        return static_cast<std::int32_t>(std::floor(static_cast<double>(v) * to / from + 0.5));
    };
    rhi::Rect r;
    r.left = scale(rect.left, guestW, internalW);
    r.right = scale(rect.right, guestW, internalW);
    r.top = scale(rect.top, guestH, internalH);
    r.bottom = scale(rect.bottom, guestH, internalH);
    return r;
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
    // M8: render to texture. Size of the current target on the device (the back buffer: the internal size; a texture
    // target: its guest size times the render-resolution factor, RenderTargetDimension).
    std::uint32_t targetInternalWidth = 640, targetInternalHeight = 480;
    struct RenderTarget {
        ni::GuestPtr data = 0;                      // NiPCRenderedTextureData (key)
        std::uint32_t guestWidth = 0, guestHeight = 0;
        std::uint32_t width = 0, height = 0;        // on the device
        std::uint32_t samples = 1;
        rhi::TextureHandle color;                   // sampled (the resolve destination with MSAA)
        rhi::TextureHandle msaa;                    // multisampled render target (samples > 1)
        rhi::TextureHandle depth;                   // depth/stencil of the target (D24S8, same samples)
        bool dirty = false;                         // drawn or cleared since the last resolve
    };
    std::vector<RenderTarget> renderTargets;
    std::size_t currentTarget = SIZE_MAX;  // index into renderTargets while textureTarget
    std::uint64_t rttCreated = 0, rttFailures = 0, rttResolves = 0, rttDraws = 0, rttClears = 0, rttSampled = 0;
    bool loggedRttFailure = false;

    void DestroyRenderTarget(RenderTarget& t) noexcept {
        for (rhi::TextureHandle h : {t.color, t.msaa, t.depth}) {
            if (h) {
                device->Destroy(h);
            }
        }
        t.color = t.msaa = t.depth = {};
    }
    // The render target of `data` at the current render resolution (created or recreated as needed); nullptr if the
    // device cannot create it (logged once).
    RenderTarget* GetRenderTarget(ni::GuestPtr data, std::uint32_t guestW, std::uint32_t guestH) noexcept;
    // Leaves the current texture target: resolves its samples into the sampled texture.
    void FinishTextureTarget() noexcept {
        if (currentTarget < renderTargets.size()) {
            RenderTarget& t = renderTargets[currentTarget];
            if (t.dirty && t.msaa && device->ResolveTexture(t.color, t.msaa)) {
                ++rttResolves;
            }
            t.dirty = false;
        }
        currentTarget = SIZE_MAX;
    }
    bool leftRightSwap = false;
    ni::TextureResolver resolveTexture = nullptr;
    void* resolveUser = nullptr;
    rhi::TextureHandle defaultTexture, blackTexture;
    rhi::TextureHandle clipTexture;  // M8: the pipeline's clipping texture (FUN_00163A40) for clipped shadows
    std::uint64_t extraPassDraws = 0;  // M8: projected shadow passes drawn

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
    bool frameColorCleared = false;
    std::uint64_t framesWithoutDraw = 0, framesWithoutColorClear = 0, framesUnchanged = 0;
    std::uint64_t repeatedPictures = 0;  // M3d F1: frames that presented the previous picture again
    // Origin of the picture in each of the device's persistent back buffers (rotated by Present): the last
    // frame that drew into it or cleared its color. A frame without either shows the picture of that frame
    // (two presents ago) - "draw summary" names it.
    struct Picture {
        std::uint32_t frame = 0, draws = 0, drawn = 0;
    };
    static constexpr std::uint32_t kBackBuffers = 2;  // rhi::DeviceDesc::bufferCount in Create
    Picture pictures[kBackBuffers];
    std::uint32_t backBuffer = 0;  // index of the current back buffer
    std::size_t arenaPeak = 0;
    DWORD renderThread = 0;  // creator / DisplayFrame thread
    bool loggedPresentFailure = false;
    bool loggedTextureFailure = false;

    // M3c: render resolution and anti-aliasing (nipc_gpu.h).
    RenderQuality quality;        // in effect (review M3c finding 2: after a step down)
    RenderQuality requested;      // [render] values
    bool reduced = false;         // quality != requested (a step down at Create or at a resize)
    RenderSizes sizes;            // current back-buffer configuration
    std::uint32_t samples = 1;    // samples per pixel the device uses
    void* window = nullptr;       // view (resolution auto follows its size)
    RenderSizes pendingSizes;     // a changed view size waiting for kResizeSettleFrames frames
    std::uint32_t pendingFrames = 0;
    std::uint64_t resizes = 0, resizeFailures = 0;

    // M3d: renderer-level interpolation between the last two logic ticks (gfx/interp_history.h). Passes every
    // camera and draw through unchanged while the frame clock produces no intermediate frames (60 fps).
    std::unique_ptr<interp::History> history;
    // G1: [render] outline_* (gfx/outline.h); nullptr at the defaults (no outline code runs).
    std::unique_ptr<outline::Outlines> outlines;

    // G3 widescreen (gfx/widescreen.h): off (wide == false) = everything as before.
    bool wide = false;
    std::uint32_t aspectW = 0, aspectH = 0;
    widescreen::Placement placed = widescreen::Placement::Center;  // placement of the viewport on the context
    float cameraWiden = 1.0f;          // Hor+ factor of the current camera data (1 = not widened)
    ni::CameraDesc unwidened;          // the current camera with the game's own frustum (cameraWiden != 1)
    bool cameraPort = false;           // the camera's port does not span the width (avatar portrait)
    bool hudCamera = false;            // off-axis camera (l >= 0, b >= 0): the HUD digit cameras
    widescreen::GuestRect portRect;    // its viewport in guest pixels
    bool barsCleared = false;          // the bars of this frame are decided (black or left to the scene)
    std::uint64_t barFramesScene = 0;  // frames whose bars the stretched scene owns (not cleared black)

    widescreen::Mapping WideMapping(widescreen::Placement p) const noexcept {
        return widescreen::MappingOf(p, backBufferWidth, backBufferHeight, sizes.internalWidth, sizes.internalHeight);
    }
    // The pipeline's camera (+ the B19 half-pixel shift) for the guest viewport `v`.
    void SetPipelineCamera(const ni::CameraDesc& desc, const rhi::Viewport& v) noexcept {
        ni::CameraDesc c = desc;
        c.viewport = v;
        if (v.width > 0.0f && v.height > 0.0f) {
            HalfPixelShift(&c.projection, v.width, v.height);  // design 8.3 B19
            HalfPixelShift(&c.screenProjection, v.width, v.height);
        }
        ff.SetCamera(c, targetWidth, targetHeight);
    }
    // Black bars left and right of the centred 4:3 area, once per frame before its first draw or colour clear (the
    // game never clears the whole back buffer; without this, centred pictures would keep old pixels beside them).
    // `stretched`: the first draw/colour clear of the frame covers the whole width (Hor+ scene, full-screen clear):
    // the sides then belong to the scene and are not cleared black - like the centre they keep the previous picture
    // where the scene draws nothing. In a level the game clears no colour (only depth/stencil) and relies on the fog
    // colour around the far plane; pixels beyond the far plane (Level1-2: the sewer's end, z > 150) keep what was
    // there. A black clear at the sides made such holes visible as black shapes cut off at the 4:3 edge (user report
    // 2026-10-13); centre and sides now behave alike.
    void ClearBars(bool stretched = false) noexcept {
        if (barsCleared) {
            return;
        }
        barsCleared = true;
        if (stretched) {
            ++barFramesScene;
            return;
        }
        const widescreen::Mapping m = WideMapping(widescreen::Placement::Center);
        const auto left = static_cast<std::int32_t>(m.offsetX);
        const auto right = static_cast<std::int32_t>(std::floor(m.offsetX + backBufferWidth * m.scaleX + 0.5));
        static const float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        rhi::Rect r;
        r.top = 0;
        r.bottom = static_cast<std::int32_t>(sizes.internalHeight);
        if (left > 0) {
            r.left = 0;
            r.right = left;
            Ctx().Clear(rhi::kClearColor, kBlack, 1.0f, 0, &r);
        }
        if (right < static_cast<std::int32_t>(sizes.internalWidth)) {
            r.left = right;
            r.right = static_cast<std::int32_t>(sizes.internalWidth);
            Ctx().Clear(rhi::kClearColor, kBlack, 1.0f, 0, &r);
        }
    }
    // Places the draw in `item` (widescreen_math.h Classify) and draws it.
    ff::DrawResult DrawWide(ni::DrawKind kind, ni::GuestPtr object) noexcept;

    // M6: movie textures (the D3D shim's YUY2 textures written by the XMV HLE), converted to BGRA8 on upload.
    struct MovieTex {
        std::uint32_t header = 0, serial = 0, generation = 0;
        std::uint32_t width = 0, height = 0;            // host texture (M6b: the pack picture's size)
        std::uint32_t texelWidth = 0, texelHeight = 0;  // the game's texture = its texel coordinate range
        tex::Yuy2Mode mode = tex::Yuy2Mode::ConvertBt601;
        rhi::TextureHandle texture;
        std::shared_ptr<const d3d::HostImage> image;    // M6b: uploaded movie pack picture (nullptr = guest pixels)
        std::uint32_t lastFrame = 0;
    };
    static constexpr std::size_t kMovieTextures = 4;
    MovieTex movies[kMovieTextures];
    std::vector<std::uint8_t> movieScratch;
    std::uint64_t movieUploads = 0, movieRawDraws = 0, movieFailures = 0, movieStageDraws = 0;
    std::uint64_t moviePackUploads = 0;  // M6b: uploads from a movie pack picture (d3d::HostImage)
    std::uint32_t moviePackMaxWidth = 0, moviePackMaxHeight = 0;

    // M6: Xbox linear (non-swizzled) textures - here the movie player's YUY2 texture - are addressed in texels on the
    // NV2A: the movie quad (Open 0xEE5A1..0xEE647) carries UVs 0..W x 0..H. A stage sampling such a texture gets
    // its coordinates scaled by 1/W, 1/H after the stage's own transform (TTFF COUNT2 with a scale if it had none),
    // and clamped addressing. M6b: with a movie pack picture the host texture is larger than W x H; the game's
    // texel UVs are scaled to it (gfx/movie_uv.h MovieTexelScale), so the picture covers the same screen area.
    void ApplyTexelAddressing() noexcept {
        for (std::uint32_t i = 0; i < ni::kMaxTextureStages; ++i) {
            ni::TextureStageDesc& st = item.state.stages[i];
            if (!st.texture) {
                continue;
            }
            for (const MovieTex& m : movies) {
                if (!m.texture || m.texture != st.texture || m.width == 0 || m.height == 0) {
                    continue;
                }
                const TexelScale scale = MovieTexelScale(m.texelWidth, m.texelHeight, m.width, m.height);
                const float sx = scale.sx, sy = scale.sy;
                if ((st.transformFlags & 0xFFu) == FF_TTFF_DISABLE) {
                    st.transform = IdentityMatrix();
                    st.transformFlags = FF_TTFF_COUNT2;
                }
                for (int r = 0; r < 4; ++r) {  // row vector * M * diag(sx, sy, 1, 1)
                    st.transform.m[r][0] *= sx;
                    st.transform.m[r][1] *= sy;
                }
                // ASSUMPTION (NV2A): linear textures support no wrapping - the hardware clamps them; with wrap the
                // bilinear filter would blend the first and last column/row of the picture at the edges.
                st.addressU = rhi::AddressMode::Clamp;
                st.addressV = rhi::AddressMode::Clamp;
                ++movieStageDraws;
                break;
            }
        }
    }

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

    // D3DDevice_SetViewport + the pipeline's copy of it (pre-transformed pixels map relative to it). M3c: `v` is
    // in guest pixels; the D3D viewport of the back buffer is scaled to the internal size, the camera keeps the
    // guest viewport (everything inside the viewport is resolution independent, nipc_gpu.h).
    void ApplyViewport(const rhi::Viewport& v) noexcept {
        viewport = v;
        // G3 placement only on the back buffer; texture targets (M8) keep their own scale (guest -> device size).
        Ctx().SetViewport(wide && !textureTarget
                              ? widescreen::PlaceViewport(v, WideMapping(placed))
                              : ScaleViewport(v, targetWidth, targetHeight, targetInternalWidth, targetInternalHeight));
        SetPipelineCamera(camera, v);
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
        c.clipTexture = clipTexture;
        return c;
    }

    void Count(ni::DrawKind kind, ni::GuestPtr object, DrawOutcome outcome, bool haveItem) noexcept;
    // M8: passes 2.. of the texture pipeline (projected shadows): the same geometry again with the pass's blend, alpha
    // test and stages, fog off (FUN_00149AE0); the item's state is restored afterwards.
    void DrawExtraPasses() noexcept {
        const ni::RenderStateDesc first = item.state;
        const std::uint32_t passes = std::min<std::uint32_t>(first.passCount, 1 + ni::kMaxExtraPasses);
        for (std::uint32_t p = 1; p < passes; ++p) {
            const ni::PassDesc& pass = first.extraPasses[p - 1];
            ni::RenderStateDesc& st = item.state;
            st.alphaBlendEnable = pass.alphaBlendEnable;
            st.srcBlend = pass.srcBlend;
            st.dstBlend = pass.dstBlend;
            st.alphaTestEnable = pass.alphaTestEnable;
            st.alphaFunc = pass.alphaFunc;
            st.alphaRef = pass.alphaRef;
            st.fogEnable = false;
            st.stageCount = pass.stageCount;
            for (std::uint32_t i = 0; i < ni::kMaxTextureStages; ++i) {
                st.stages[i] = pass.stages[i];
            }
            st.textureFactor = pass.textureFactor;
            if (ff.Draw(item) == ff::DrawResult::Drawn) {
                ++extraPassDraws;
            }
        }
        item.state = first;
    }
    // The item and, if it was drawn, its extra passes - with the viewport and pipeline camera in effect (on the back
    // buffer with G3 widescreen: those DrawWide set for the item, so the shadow passes land on the same pixels).
    ff::DrawResult DrawWithPasses() noexcept {
        const ff::DrawResult result = ff.Draw(item);
        if (result == ff::DrawResult::Drawn && item.state.passCount > 1) {
            DrawExtraPasses();
        }
        return result;
    }
    void Submit(ni::DrawKind kind, ni::GuestPtr object, ni::ExtractStatus status) noexcept;
    void TraceDraw(ni::DrawKind kind, ni::GuestPtr object, DrawOutcome outcome, bool haveItem) noexcept;
    // `repeated` (M3d F1): the frame presents the previous picture again.
    void LogFrameSummary(std::uint32_t frame, const Picture& shown, bool repeated) noexcept;
    void PrepareFrame(std::uint32_t frame) noexcept;
    bool Resize(const RenderSizes& want, const char* reason) noexcept;  // M3c
    void FollowView() noexcept;                                          // M3c: resolution auto
    // M3c: MSAA / alpha-to-coverage / anisotropy / anti-aliased nearest of the pipeline for the current sizes.
    void ApplyRenderOptions() noexcept {
        ff::RenderOptions options;
        options.sampleCount = samples;
        options.alphaToCoverage = quality.alphaToCoverage && samples > 1;
        options.maxAnisotropy = std::clamp<std::uint32_t>(quality.anisotropy, 1, 16);
        options.sharpNearest = sizes.internalWidth != backBufferWidth || sizes.internalHeight != backBufferHeight;
        ff.SetRenderOptions(options);
    }
};

// M3c: new back-buffer configuration (the device carries the pictures over), then the guest viewport again.
// Review M3c finding 2: if the buffers cannot be created (a larger view in fullscreen, out of video memory) the
// resize steps down like Create - render scale 1, then fewer samples - before it keeps the old buffers.
bool GpuRenderer::Impl::Resize(const RenderSizes& wanted, const char* reason) noexcept {
    if (wanted == sizes) {
        return false;
    }
    const RenderSizes old = sizes;
    RenderSizes want = wanted;
    std::uint32_t trySamples = samples;
    bool stepped = false;
    bool scaleStepped = false;
    for (;;) {
        const bool downsample = want.outputWidth != want.internalWidth || want.outputHeight != want.internalHeight;
        if (device->ResizeBackBuffers(want.internalWidth, want.internalHeight, trySamples,
                                      downsample ? want.outputWidth : 0, downsample ? want.outputHeight : 0)) {
            break;
        }
        const std::string failure = device->LastError();
        RenderSizes next = want;
        std::uint32_t nextSamples = trySamples;
        if (want.renderScale > 1.0f) {
            next.internalWidth = want.outputWidth;
            next.internalHeight = want.outputHeight;
            next.renderScale = 1.0f;
            scaleStepped = true;
        } else if (trySamples > 1) {
            nextSamples = trySamples / 2;
        } else {
            if (resizeFailures++ == 0) {
                FUT_LOGW(Gfx, "render: back buffers not resized to %ux%u (%s): %s - the present pass scales the old ones",
                         wanted.internalWidth, wanted.internalHeight, reason, failure.c_str());
            }
            return false;
        }
        FUT_LOGW(Gfx, "render: back buffers %ux%u with %u sample%s not available (%s): %s - reducing to %ux%u with %u "
                 "sample%s", want.internalWidth, want.internalHeight, trySamples, trySamples == 1 ? "" : "s",
                 reason, failure.c_str(), next.internalWidth, next.internalHeight, nextSamples,
                 nextSamples == 1 ? "" : "s");
        want = next;
        trySamples = nextSamples;
        stepped = true;
    }
    if (stepped) {
        reduced = true;
        quality.renderScale = scaleStepped ? 1.0f : quality.renderScale;
        quality.msaa = std::min(quality.msaa, std::max<std::uint32_t>(trySamples, 1u));
    }
    samples = std::max<std::uint32_t>(device->Info().sampleCount, 1);
    sizes = want;
    ++resizes;
    ApplyRenderOptions();
    if (!textureTarget) {
        targetInternalWidth = sizes.internalWidth;
        targetInternalHeight = sizes.internalHeight;
        ApplyViewport(viewport);
    }
    FUT_LOGI(Gfx, "render: %s - output %ux%u -> %ux%u, internal %ux%u -> %ux%u (render scale %.2f, %u sample%s%s)",
             reason, old.outputWidth, old.outputHeight, sizes.outputWidth, sizes.outputHeight, old.internalWidth,
             old.internalHeight, sizes.internalWidth, sizes.internalHeight, static_cast<double>(sizes.renderScale),
             samples, samples == 1 ? "" : "s", stepped ? ", reduced" : "");
    return true;
}

void GpuRenderer::Impl::FollowView() noexcept {
    if (window == nullptr || quality.resolutionWidth != 0) {
        return;  // fixed [render] resolution (or headless)
    }
    std::uint32_t w = 0, h = 0;
    platform::MainWindowClientSize(&w, &h);
    if (w == 0 || h == 0) {
        pendingFrames = 0;  // minimized: keep the buffers
        return;
    }
    const RenderSizes want =
        ComputeRenderSizes(backBufferWidth, backBufferHeight, 0, 0, w, h, quality.renderScale, aspectW, aspectH);
    if (want == sizes) {
        pendingFrames = 0;
        return;
    }
    if (!(want == pendingSizes)) {
        pendingSizes = want;
        pendingFrames = 0;
    }
    if (++pendingFrames >= kResizeSettleFrames) {
        pendingFrames = 0;
        char reason[64];
        std::snprintf(reason, sizeof(reason), "view resized to %ux%u", w, h);
        Resize(want, reason);
    }
}

// Screen-space bounding box of a draw in D3D8 pixel coordinates (pixel centers on integers, i.e. before the
// half-pixel shift of B19): world * view * projection (or the screen projection of slot 54), perspective
// divide, viewport; pre-transformed vertices are already pixels. Diagnostics of the draw trace (review M3
// finding 9: which pixel row an edge falls on). false if no vertex has w > 0.
namespace {
bool ScreenBounds(const ni::DrawItem& item, const ni::CameraDesc& camera, float* x0, float* y0, float* x1,
                  float* y1) noexcept {
    const ni::GeometryDesc& g = item.geometry;
    if (g.positions.data == nullptr || g.vertexCount == 0) {
        return false;
    }
    const Matrix4 viewProj = item.useScreenCamera ? camera.screenProjection
                                                  : Multiply(camera.view, camera.projection);
    const Matrix4 m = Multiply(item.world, viewProj);
    const rhi::Viewport& v = camera.viewport;
    bool any = false;
    for (std::uint32_t i = 0; i < g.vertexCount; ++i) {
        const auto* p = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(g.positions.data) +
                                                       static_cast<std::size_t>(i) * g.positions.stride);
        float sx, sy;
        if (g.pretransformed) {
            sx = p[0];
            sy = p[1];
        } else {
            const Float4 c = TransformFloat4(Float4{p[0], p[1], p[2], 1.0f}, m);
            if (!(c.w > 0.0f)) {
                continue;
            }
            sx = v.x + (c.x / c.w * 0.5f + 0.5f) * v.width;
            sy = v.y + (0.5f - c.y / c.w * 0.5f) * v.height;
        }
        if (!any) {
            *x0 = *x1 = sx;
            *y0 = *y1 = sy;
            any = true;
        } else {
            *x0 = std::min(*x0, sx);
            *x1 = std::max(*x1, sx);
            *y0 = std::min(*y0, sy);
            *y1 = std::max(*y1, sy);
        }
    }
    return any;
}
}  // namespace

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
    float bx0 = 0.0f, by0 = 0.0f, bx1 = 0.0f, by1 = 0.0f;
    ni::CameraDesc applied = camera;
    applied.viewport = viewport;  // the viewport on the context (ClearBuffer may have changed it)
    const bool bounds = ScreenBounds(item, applied, &bx0, &by0, &bx1, &by1);
    FUT_LOGI(
        Gfx,
        "draw trace frame %u #%u: %s 0x%08X -> %s%s%s: screen %s(%.3f, %.3f)-(%.3f, %.3f), "
        "%u vertices, %u indices, %u triangles (%s%s%s%s), "
        "%u stages (%s; stage 0 op %u/%u, NiTexture 0x%08X -> host %u, addr %u/%u, filter %u/%u/%u, uv %u), "
        "lighting %d (%u lights, "
        "sources d%u a%u e%u), colors %d, blend %d (%u/%u), alpha test %d (func %u ref %u), z func %u write "
        "%d, "
        "cull %u, fog %d, material diffuse (%.2f %.2f %.2f %.2f) ambient (%.2f %.2f %.2f) emissive (%.2f "
        "%.2f "
        "%.2f), global ambient (%.2f %.2f %.2f), passes %u (pass 2: %u stages, NiTexture 0x%08X -> host %u, tci 0x%X)",
        frame, drawIndex, DrawKindName(kind), object, DrawOutcomeName(outcome), *note != 0 ? ": " : "", note,
        bounds ? "" : "(none) ", bx0, by0, bx1, by1, g.vertexCount, g.indexCount, g.primitiveCount,
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
        s.material.emissive.z, item.lights.ambient.x, item.lights.ambient.y, item.lights.ambient.z, s.passCount,
        s.extraPasses[0].stageCount, s.extraPasses[0].stages[0].niTexture, s.extraPasses[0].stages[0].texture.id,
        s.extraPasses[0].stages[0].texCoordIndex);
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
    ApplyTexelAddressing();
    // G3 placement only on the back buffer; texture targets (M8) are drawn at their own scale.
    const ff::DrawResult result = wide && !textureTarget ? DrawWide(kind, object) : DrawWithPasses();
    if (textureTarget && result == ff::DrawResult::Drawn) {
        renderTargets[currentTarget].dirty = true;  // M8: resolved when the target is left
        ++rttDraws;
    }
    Count(kind, object, OutcomeOf(result), true);
}

// G3: the facts of the draw in `item` -> placement (widescreen_math.h) -> the D3D viewport of that placement; a
// full-screen quad of a Hor+ camera (the pause menu's dimming) is drawn with the game's own projection, stretched.
ff::DrawResult GpuRenderer::Impl::DrawWide(ni::DrawKind kind, ni::GuestPtr object) noexcept {
    namespace ws = widescreen;
    ws::DrawFacts f;
    f.source = kind == ni::DrawKind::ScreenPolygon   ? ws::DrawSource::ScreenPolygon
               : kind == ni::DrawKind::ScreenTexture ? ws::DrawSource::ScreenTexture
                                                     : ws::DrawSource::Geometry;
    f.camera = cameraWiden != 1.0f ? ws::DrawFacts::Camera::Widened
               : cameraPort        ? ws::DrawFacts::Camera::Port
                                   : ws::DrawFacts::Camera::FullWidth;
    f.hudCamera = hudCamera;
    f.port = portRect;
    f.ribbon = kind == ni::DrawKind::ScreenPolygon && outline::IsRibbonPolygon(object);
    f.textured = item.state.stageCount > 0 && (item.state.stages[0].niTexture != 0 || item.state.stages[0].texture);
    f.anchoring = ws::AnchoringAllowed(framesDone + 1);
    bool unwidenedQuad = false;
    ni::CameraDesc applied = camera;
    applied.viewport = viewport;
    if (f.camera == ws::DrawFacts::Camera::Widened && f.source == ws::DrawSource::Geometry) {
        // quads: the only candidates for a full-screen overlay (not particles, M8: 3D billboards, stretched like the
        // rest of the scene)
        if (kind != ni::DrawKind::Points && item.geometry.vertexCount <= 8) {
            ni::CameraDesc game = unwidened;
            game.viewport = viewport;
            ws::GuestRect b;
            if (ScreenBounds(item, game, &b.x0, &b.y0, &b.x1, &b.y1) && ws::CoversScreen(b, 0.0f)) {
                unwidenedQuad = true;
                f.hasBounds = true;
                f.bounds = b;
            }
        }
    } else if (f.source != ws::DrawSource::Geometry ||
               (f.camera == ws::DrawFacts::Camera::FullWidth && f.hudCamera)) {  // (the rules need no box otherwise)
        f.hasBounds = ScreenBounds(item, applied, &f.bounds.x0, &f.bounds.y0, &f.bounds.x1, &f.bounds.y1);
    }
    const ws::Placement p = unwidenedQuad ? ws::Placement::Stretch : ws::Classify(f, ws::AnchorExceptions());
    ClearBars(p == ws::Placement::Stretch);
    ws::NoteDraw(f, p, object, framesDone + 1);
    if (p != placed) {
        placed = p;
        Ctx().SetViewport(ws::PlaceViewport(viewport, WideMapping(p)));
    }
    // M8: the extra passes (projected shadows) with the same placement and camera as the item.
    if (!unwidenedQuad) {
        return DrawWithPasses();
    }
    ws::NoteUnwidenedQuad();
    SetPipelineCamera(unwidened, viewport);
    const ff::DrawResult result = DrawWithPasses();
    SetPipelineCamera(camera, viewport);
    return result;
}

void GpuRenderer::Impl::LogFrameSummary(std::uint32_t frame, const Picture& shown, bool repeated) noexcept {
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
    char picture[128] = "";
    if (shown.frame != frame) {
        std::snprintf(picture, sizeof(picture), "; shows the picture of frame %u (%u draw calls, %u drawn)%s",
                      shown.frame, shown.draws, shown.drawn, repeated ? ", repeated (previous present)" : "");
    }
    FUT_LOGI(Gfx, "draw summary frame %u: %u draw calls, %u drawn, %u not drawn, %u clears%s%s%s", frame,
             issued, drawn, issued - drawn, frameClears, used != 0 ? "; " : "", line, picture);
}

void GpuRenderer::Impl::PrepareFrame(std::uint32_t frame) noexcept {
    const Settings& s = GetSettings();
    traceFrame = Listed(s.traceFrames, frame);
    summaryFrame = traceFrame || Listed(s.screenshotFrames, frame);
    drawIndex = 0;
    frameClears = 0;
    frameColorCleared = false;
    barsCleared = false;  // G3
    std::memset(frameCounts, 0, sizeof(frameCounts));
    if (history != nullptr) {
        history->BeginFrame(frame, traceFrame);  // M3d: per-draw interpolation trace of the trace_frames frames
    }
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
        if (impl_->clipTexture) {
            impl_->device->Destroy(impl_->clipTexture);
        }
        for (Impl::MovieTex& m : impl_->movies) {
            if (m.texture) {
                impl_->device->Destroy(m.texture);
            }
        }
        for (Impl::RenderTarget& t : impl_->renderTargets) {
            impl_->DestroyRenderTarget(t);
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
    // M3c: back-buffer configuration from [render] resolution / render_scale / msaa and the view size.
    d.requested = params.quality;
    d.window = params.window;
    d.wide = params.aspectWidth != 0 && params.aspectHeight != 0;  // G3
    d.aspectW = d.wide ? params.aspectWidth : 0;
    d.aspectH = d.wide ? params.aspectHeight : 0;
    std::uint32_t viewW = params.viewWidth, viewH = params.viewHeight;
    if (params.window != nullptr && (viewW == 0 || viewH == 0)) {
        platform::MainWindowClientSize(&viewW, &viewH);
    }
    rhi::DeviceDesc desc;
    bool downsample = false;
    d.renderThread = GetCurrentThreadId();
    // Review M3c finding 2: step down while the back buffers of a configuration cannot be created.
    const QualityLadder ladder = QualityFallbacks(params.quality);
    std::string firstError;
    for (std::uint32_t step = 0; step < ladder.count && d.device == nullptr; ++step) {
        d.quality = ladder.steps[step];
        d.sizes = ComputeRenderSizes(params.width, params.height, d.quality.resolutionWidth,
                                     d.quality.resolutionHeight, viewW, viewH, d.quality.renderScale,
                                     params.aspectWidth, params.aspectHeight);
        downsample = d.sizes.outputWidth != d.sizes.internalWidth || d.sizes.outputHeight != d.sizes.internalHeight;
        desc = rhi::DeviceDesc{};
        desc.backend = rhi::Backend::D3D11;
        desc.nativeWindow = params.window;
        desc.width = d.sizes.internalWidth;
        desc.height = d.sizes.internalHeight;
        desc.bufferCount = Impl::kBackBuffers;
        desc.backBufferFormat = rhi::Format::BGRA8_UNorm;
        desc.depthFormat = rhi::Format::D24_UNorm_S8_UInt;  // Create gets D3DFMT_D24S8 (0x4B, auto depth/stencil)
        desc.debugLayer = params.debugLayer;
        desc.softwareAdapter = params.softwareAdapter;
        desc.scaling = params.scaling;
        desc.frameLatencyWaitable = 1;  // M3b: display-coupled pacing (platform/frame_pacer.h) waits on it
        desc.sampleCount = std::max<std::uint32_t>(d.quality.msaa, 1);  // M3c (clamped by the device)
        desc.outputWidth = downsample ? d.sizes.outputWidth : 0;
        desc.outputHeight = downsample ? d.sizes.outputHeight : 0;
        desc.linearResolve = d.quality.linearAa;  // review M3c finding 3
        std::string stepError;
        d.device = rhi::CreateDevice(desc, &stepError);
        if (d.device == nullptr) {
            if (firstError.empty()) {
                firstError = stepError;
            }
            if (step + 1 < ladder.count) {
                FUT_LOGW(Gfx, "render: D3D11 back buffers %ux%u with %u sample%s not available (%s; %s) - trying %s",
                         desc.width, desc.height, desc.sampleCount, desc.sampleCount == 1 ? "" : "s",
                         stepError.c_str(), DescribeQuality(d.quality).c_str(),
                         DescribeQuality(ladder.steps[step + 1]).c_str());
            } else if (error != nullptr) {
                try {
                    *error = firstError == stepError ? stepError
                                                     : firstError + "; with " + DescribeQuality(d.quality) +
                                                           " too: " + stepError;
                } catch (...) {
                }
            }
        }
    }
    if (d.device == nullptr) {
        return nullptr;
    }
    d.reduced = d.quality.renderScale != d.requested.renderScale || d.quality.msaa != d.requested.msaa ||
                d.quality.resolutionWidth != d.requested.resolutionWidth ||
                d.quality.resolutionHeight != d.requested.resolutionHeight;
    if (!d.ff.Init(*d.device, error)) {
        return nullptr;
    }
    d.samples = std::max<std::uint32_t>(d.device->Info().sampleCount, 1);
    d.backBufferWidth = params.width;
    d.backBufferHeight = params.height;
    d.ApplyRenderOptions();
    const ff::RenderOptions options = d.ff.GetRenderOptions();
    {
        // "render: ..." (GFX/info; tools/m3c_check.py reads it): the configuration actually used.
        char supported[32] = "";
        std::size_t used = 0;
        for (std::uint32_t n = 2; n <= 8; n *= 2) {
            if (d.device->SupportedSampleCount(desc.backBufferFormat, desc.depthFormat, n) == n) {
                const int w = std::snprintf(supported + used, sizeof(supported) - used, "%s%u", used != 0 ? ", " : "",
                                            n);
                used += w > 0 ? static_cast<std::size_t>(w) : 0;
            }
        }
        char resolution[48];
        if (d.quality.resolutionWidth == 0) {
            std::snprintf(resolution, sizeof(resolution), "auto: view %ux%u", viewW, viewH);
        } else {
            std::snprintf(resolution, sizeof(resolution), "%ux%u", d.quality.resolutionWidth,
                          d.quality.resolutionHeight);
        }
        const rhi::DeviceInfo& info = d.device->Info();
        const std::uint64_t pixels = static_cast<std::uint64_t>(info.backBufferWidth) * info.backBufferHeight;
        const std::uint64_t mib = (pixels * 4 * d.samples * (Impl::kBackBuffers + 1) +
                                   (d.samples > 1 ? pixels * 4 : 0) +
                                   (downsample ? static_cast<std::uint64_t>(d.sizes.outputWidth) * d.sizes.outputHeight * 4
                                               : 0)) >> 20;
        FUT_LOGI(Gfx,
                 "render: output %ux%u (resolution %s), internal %ux%u (render scale %.2f, %.4f x %.4f per %ux%u "
                 "pixel), MSAA %ux (requested %ux; supported: %s), alpha to coverage %s, anisotropy %ux, downsample "
                 "%s, screenshots %s, back buffers %llu MiB, resolve %s%s%s",
                 d.sizes.outputWidth, d.sizes.outputHeight, resolution, info.backBufferWidth, info.backBufferHeight,
                 static_cast<double>(d.sizes.renderScale),
                 static_cast<double>(info.backBufferWidth) / params.width,
                 static_cast<double>(info.backBufferHeight) / params.height, params.width, params.height, d.samples,
                 desc.sampleCount, used != 0 ? supported : "none", options.alphaToCoverage ? "on" : "off",
                 options.maxAnisotropy,
                 !downsample                                                ? "none"
                 : (d.sizes.internalWidth % d.sizes.outputWidth == 0 &&
                    d.sizes.internalHeight % d.sizes.outputHeight == 0) ? "box"
                                                                           : "catmull-rom",
                 d.quality.screenshots == ScreenshotSize::Internal ? "internal" : "output",
                 static_cast<unsigned long long>(mib), info.linearResolve ? "linear light" : "gamma",
                 d.reduced ? ", reduced from " : "", d.reduced ? DescribeQuality(d.requested).c_str() : "");
        if (d.samples != desc.sampleCount) {
            FUT_LOGI(Gfx, "render: [render] msaa = %u is not supported by \"%s\" for BGRA8 + D24S8 - using %ux", desc.sampleCount,
                     info.adapterName.c_str(), d.samples);
        }
    }
    d.backBufferWidth = d.targetWidth = params.width;
    d.backBufferHeight = d.targetHeight = params.height;
    d.targetInternalWidth = d.sizes.internalWidth;
    d.targetInternalHeight = d.sizes.internalHeight;
    d.resolveTexture = params.resolveTexture;
    d.resolveUser = params.resolveUser;
    // Default texture of NiXBoxRenderer (FUN_0014AB50, created by the device setup FUN_0014C0F0).
    tex::HostImage image;
    tex::MakeDefaultTexture(&image);
    d.defaultTexture = r->CreateTexture(image, "NiPCRenderer default texture (FUN_0014AB50)");
    // Black 1x1: movie textures (the shim's YUY2 surface, contents from M6) and rendered textures whose render target
    // cannot be created (M8).
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
    // M8: clipping texture of the default pipeline (FUN_00163A40: NiPixelData 2 x 1 in the default format A8R8G8B8,
    // texel 0 = 0x00000000, texel 1 = 0xFFFFFFFF).
    static const std::uint8_t kClip[8] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    rhi::TextureDesc clip = black;
    clip.width = 2;
    clip.debugName = "NiPCRenderer clipping texture (FUN_00163A40)";
    rhi::SubresourceData clipLevel;
    clipLevel.data = kClip;
    clipLevel.rowPitch = 8;
    clipLevel.slicePitch = 8;
    d.clipTexture = d.device->CreateTexture(clip, &clipLevel);
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
    // M3d: [render] interpolation / interpolation_debug (an unreadable configuration keeps the defaults).
    try {
        interp::Settings interpolation;
        try {
            interpolation = interp::SettingsFromConfig(core::GetConfig());
        } catch (...) {
        }
        d.history = std::make_unique<interp::History>(interpolation);
        d.history->LogConfiguration();
    } catch (...) {
        FUT_LOGW(Gfx, "interpolation: not available (out of memory)");
    }
    // G1: outline options (nothing is created at the defaults).
    if (GetSettings().outline.Active()) {
        try {
            d.outlines = std::make_unique<outline::Outlines>(GetSettings().outline);
        } catch (...) {
            FUT_LOGW(Gfx, "outlines: options not available (out of memory)");
        }
    }
    d.PrepareFrame(1);
    return r;
}

rhi::Device& GpuRenderer::Device() noexcept {
    return *impl_->device;
}

const RenderSizes& GpuRenderer::Sizes() const noexcept {
    return impl_->sizes;
}

const RenderQuality& GpuRenderer::Quality() const noexcept {
    return impl_->quality;
}

bool GpuRenderer::QualityReduced() const noexcept {
    return impl_->reduced;
}

std::uint32_t GpuRenderer::SampleCount() const noexcept {
    return impl_->samples;
}

bool GpuRenderer::ResizeForView(std::uint32_t viewWidth, std::uint32_t viewHeight) noexcept {
    Impl& d = *impl_;
    if (d.quality.resolutionWidth != 0 || viewWidth == 0 || viewHeight == 0) {
        return false;
    }
    // (a step down to 640x480 at Create set resolutionWidth: such a renderer no longer follows the view)
    const RenderSizes want = ComputeRenderSizes(d.backBufferWidth, d.backBufferHeight, 0, 0, viewWidth, viewHeight,
                                                d.quality.renderScale, d.aspectW, d.aspectH);
    return d.Resize(want, "view size");
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
    d.FinishTextureTarget();
    d.textureTarget = false;
    d.targetWidth = d.backBufferWidth;
    d.targetHeight = d.backBufferHeight;
    d.targetInternalWidth = d.sizes.internalWidth;
    d.targetInternalHeight = d.sizes.internalHeight;
    d.Ctx().SetRenderTargets({}, {});
    d.ApplyViewport(d.FullViewport());  // D3D8 SetRenderTarget: viewport = the whole new target
}

std::uint32_t RenderTargetDimension(std::uint32_t guest, std::uint32_t guestBackBuffer,
                                    std::uint32_t internalBackBuffer) noexcept {
    if (guest == 0 || guestBackBuffer == 0 || internalBackBuffer <= guestBackBuffer) {
        return guest;
    }
    const double k = static_cast<double>(internalBackBuffer) / guestBackBuffer;
    const auto v = static_cast<std::uint32_t>(std::floor(guest * k + 0.5));
    return std::clamp<std::uint32_t>(v, guest, std::max(guest, kMaxRenderTargetDimension));
}

GpuRenderer::Impl::RenderTarget* GpuRenderer::Impl::GetRenderTarget(ni::GuestPtr data, std::uint32_t guestW,
                                                                    std::uint32_t guestH) noexcept {
    // M8: the factor of the back buffer (internal height / guest height) also for texture targets - the projected
    // shadows get the configured resolution like the scene, with the same MSAA samples.
    const std::uint32_t w = RenderTargetDimension(guestW, backBufferHeight, sizes.internalHeight);
    const std::uint32_t h = RenderTargetDimension(guestH, backBufferHeight, sizes.internalHeight);
    RenderTarget* t = nullptr;
    for (RenderTarget& e : renderTargets) {
        if (e.data == data) {
            t = &e;
            break;
        }
    }
    if (t != nullptr && t->color && t->width == w && t->height == h && t->samples == samples) {
        return t;
    }
    const ni::GuestPtr current = currentTarget < renderTargets.size() ? renderTargets[currentTarget].data : 0;
    if (t == nullptr) {
        try {
            renderTargets.push_back(RenderTarget{});
        } catch (...) {
            return nullptr;
        }
        t = &renderTargets.back();
        t->data = data;
        // push_back may have moved the entries: find the current target again
        for (std::size_t i = 0; i < renderTargets.size(); ++i) {
            if (current != 0 && renderTargets[i].data == current) {
                currentTarget = i;
            }
        }
    } else {
        DestroyRenderTarget(*t);  // the render resolution or the MSAA level changed
    }
    t->guestWidth = guestW;
    t->guestHeight = guestH;
    t->width = w;
    t->height = h;
    t->samples = samples;
    t->dirty = false;
    rhi::TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.mipLevels = 1;
    desc.format = rhi::Format::BGRA8_UNorm;
    desc.usage = static_cast<std::uint8_t>(rhi::kTextureSampled | (samples > 1 ? 0 : rhi::kTextureRenderTarget));
    desc.debugName = "NiRenderedTexture";
    t->color = device->CreateTexture(desc, nullptr);
    if (samples > 1) {
        desc.usage = rhi::kTextureRenderTarget;
        desc.sampleCount = samples;
        desc.debugName = "NiRenderedTexture (MSAA)";
        t->msaa = device->CreateTexture(desc, nullptr);
    }
    desc.format = rhi::Format::D24_UNorm_S8_UInt;
    desc.usage = rhi::kTextureDepthStencil;
    desc.sampleCount = samples;
    desc.debugName = "NiRenderedTexture depth";
    t->depth = device->CreateTexture(desc, nullptr);
    if (!t->color || !t->depth || (samples > 1 && !t->msaa)) {
        ++rttFailures;
        if (!loggedRttFailure) {
            loggedRttFailure = true;
            FUT_LOGE(Gfx, "d3d11: render target %ux%u (%u sample%s) for rendered texture data 0x%08X failed: %s", w, h,
                     samples, samples == 1 ? "" : "s", data, device->LastError());
        }
        DestroyRenderTarget(*t);
        return nullptr;
    }
    ++rttCreated;
    FUT_LOGI(Gfx, "d3d11: render to texture 0x%08X: %ux%u -> %ux%u, %u sample%s", data, guestW, guestH, w, h, samples,
             samples == 1 ? "" : "s");
    // A new target starts black (Xbox: zeroed contiguous memory) with depth 1 until the game clears it.
    Ctx().SetRenderTargets(t->msaa ? t->msaa : t->color, t->depth);
    static constexpr float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Ctx().Clear(rhi::kClearColor | rhi::kClearDepth | rhi::kClearStencil, kZero, 1.0f, 0, nullptr);
    if (t->msaa) {
        device->ResolveTexture(t->color, t->msaa);
    }
    // Back to the target that was bound (the caller binds the new one when it renders into it).
    if (currentTarget < renderTargets.size() && &renderTargets[currentTarget] != t) {
        const RenderTarget& c = renderTargets[currentTarget];
        Ctx().SetRenderTargets(c.msaa ? c.msaa : c.color, c.depth);
    } else {
        Ctx().SetRenderTargets({}, {});
    }
    ApplyViewport(viewport);
    return t;
}

void GpuRenderer::SetTextureTarget(ni::GuestPtr data, std::uint32_t width, std::uint32_t height) noexcept {
    Impl& d = *impl_;
    d.FinishTextureTarget();
    // M8: render to texture (GFX_CONTRACT.md 11): the texture's own color target and depth buffer, at the guest size
    // times the render-resolution factor with the back buffer's MSAA samples (resolved when the target is left).
    d.textureTarget = true;
    d.targetWidth = width;
    d.targetHeight = height;
    Impl::RenderTarget* t = d.GetRenderTarget(data, width, height);
    if (t == nullptr) {
        // No device target: the draws go nowhere like before M8 (counted as TextureTarget).
        d.targetInternalWidth = width;
        d.targetInternalHeight = height;
        return;
    }
    d.currentTarget = static_cast<std::size_t>(t - d.renderTargets.data());
    d.targetInternalWidth = t->width;
    d.targetInternalHeight = t->height;
    d.Ctx().SetRenderTargets(t->msaa ? t->msaa : t->color, t->depth);
    d.ApplyViewport(d.FullViewport());  // D3D8 SetRenderTarget: viewport = the whole new target
}

rhi::TextureHandle GpuRenderer::RenderedTexture(ni::GuestPtr data, std::uint32_t width, std::uint32_t height) noexcept {
    Impl& d = *impl_;
    Impl::RenderTarget* t = d.GetRenderTarget(data, width, height);
    if (t == nullptr) {
        return d.blackTexture;
    }
    const bool current = d.currentTarget < d.renderTargets.size() && &d.renderTargets[d.currentTarget] == t;
    if (!current && t->dirty && t->msaa && d.device->ResolveTexture(t->color, t->msaa)) {
        ++d.rttResolves;
        t->dirty = false;
    }
    ++d.rttSampled;
    return t->color;
}

void GpuRenderer::ForgetRenderedTexture(ni::GuestPtr data) noexcept {
    Impl& d = *impl_;
    for (std::size_t i = 0; i < d.renderTargets.size(); ++i) {
        if (d.renderTargets[i].data != data) {
            continue;
        }
        if (i == d.currentTarget) {
            d.currentTarget = SIZE_MAX;
            d.Ctx().SetRenderTargets({}, {});  // the guest cannot draw into a destroyed texture any more
            d.textureTarget = false;
            d.targetWidth = d.backBufferWidth;
            d.targetHeight = d.backBufferHeight;
            d.targetInternalWidth = d.sizes.internalWidth;
            d.targetInternalHeight = d.sizes.internalHeight;
        }
        d.DestroyRenderTarget(d.renderTargets[i]);
        if (d.currentTarget != SIZE_MAX && d.currentTarget > i) {
            --d.currentTarget;
        }
        d.renderTargets.erase(d.renderTargets.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void GpuRenderer::SetLeftRightSwap(bool swap) noexcept {
    impl_->leftRightSwap = swap;
}

void GpuRenderer::ClearBuffer(const ni::NiRectF* rect, std::uint32_t mode, std::uint32_t argb, float depth,
                              std::uint32_t stencil) noexcept {
    Impl& d = *impl_;
    ++d.clears;
    ++d.frameClears;
    if (d.textureTarget && d.currentTarget >= d.renderTargets.size()) {
        ++d.clearsSkipped;  // M8: only when the device target could not be created
        return;
    }
    // 0x14659E..0x1465B9: viewport width/height (+0x41C/+0x420) = target size, X/Y keep their 0 from
    // FUN_00147CD0, D3DDevice_SetViewport(+0x414) before the clear; not restored.
    d.ApplyViewport(d.FullViewport());
    // 0x14654D..0x14659C: Xbox D3DCLEAR_TARGET (0xF0) for mode bit 1, D3DCLEAR_ZBUFFER for bit 4 and
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
    if (hasRect && d.wide && !d.textureTarget) {  // G3: the rectangle gets a placement like a draw (full screen ->
        widescreen::DrawFacts f;                      // the whole width); only on the back buffer
        f.source = widescreen::DrawSource::Clear;
        f.hasBounds = true;
        f.bounds = {static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                    static_cast<float>(r.bottom)};
        f.anchoring = widescreen::AnchoringAllowed(d.framesDone + 1);
        const widescreen::Placement p = widescreen::Classify(f, widescreen::AnchorExceptions());
        if ((flags & rhi::kClearColor) != 0) {
            d.ClearBars(p == widescreen::Placement::Stretch);  // a depth/stencil clear leaves the bars to the draws
        }
        widescreen::NoteDraw(f, p, 0, d.framesDone + 1);
        r = widescreen::PlaceRect(r, d.WideMapping(p));
    } else if (hasRect) {  // M3c: guest pixels -> internal pixels of the target (M8: also texture targets)
        r = ScaleRect(r, d.targetWidth, d.targetHeight, d.targetInternalWidth, d.targetInternalHeight);
    }

    d.Ctx().Clear(flags, color, depth, static_cast<std::uint8_t>(stencil & 0xFF), hasRect ? &r : nullptr);
    if (d.textureTarget) {
        d.renderTargets[d.currentTarget].dirty = true;
        ++d.rttClears;
    } else if ((flags & rhi::kClearColor) != 0) {
        d.frameColorCleared = true;
    }
}

void GpuRenderer::BeginScene() noexcept {
    ++impl_->scenes;
    ni::BeginScene(&impl_->shadow);
}

void GpuRenderer::SetCameraData(const ni::CameraInput& input) noexcept {
    Impl& d = *impl_;
    ++d.cameras;
    // M3d: NiCamera::Click passes cam+0xA0 as the location - the camera object keys the history.
    ni::CameraInput in = d.history != nullptr && input.location != nullptr
                             ? d.history->Camera(guest::Addr(input.location) - ni::kNiCameraWorldLocation, input)
                             : input;
    in.targetWidth = d.targetWidth;
    in.targetHeight = d.targetHeight;
    in.leftRightSwap = d.leftRightSwap;
    d.camera = ni::ExtractCamera(in);
    if (d.traceFrame && in.frustum != nullptr && in.port != nullptr) {  // G3: which camera the following draws use
        FUT_LOGI(Gfx,
                 "draw trace frame %u #%u: camera 0x%08X frustum l %.3f r %.3f t %.3f b %.3f n %.3f f %.1f port %.2f "
                 "%.2f %.2f %.2f%s",
                 d.framesDone + 1, d.drawIndex,
                 input.location != nullptr ? guest::Addr(input.location) - ni::kNiCameraWorldLocation : 0u,
                 in.frustum->left, in.frustum->right, in.frustum->top, in.frustum->bottom, in.frustum->nearPlane,
                 in.frustum->farPlane, in.port->left, in.port->right, in.port->top, in.port->bottom,
                 d.textureTarget ? " (texture target)" : "");
    }
    rhi::Viewport v = d.camera.viewport;
    v.width = std::max(v.width, 0.0f);
    v.height = std::max(v.height, 0.0f);
    if (d.wide && in.frustum != nullptr && in.port != nullptr) {
        // G3: is this the data of a Hor+ camera (widened inside its Click), a port camera or a full-width one?
        const std::uint32_t cameraObject =
            input.location != nullptr ? guest::Addr(input.location) - ni::kNiCameraWorldLocation : 0u;
        d.cameraWiden = widescreen::WidenedFactor(cameraObject);
        if (d.cameraWiden != 1.0f) {
            ni::NiFrustum game = *in.frustum;
            game.left /= d.cameraWiden;
            game.right /= d.cameraWiden;
            ni::CameraInput original = in;
            original.frustum = &game;
            d.unwidened = ni::ExtractCamera(original);
        }
        d.cameraPort = in.port->left > 0.01f || in.port->right < 0.99f;
        d.hudCamera = in.frustum->left >= 0.0f && in.frustum->bottom >= 0.0f;
        d.portRect = {v.x, v.y, v.x + v.width, v.y + v.height};
    }
    d.ApplyViewport(v);
}

void GpuRenderer::DrawGeometry(ni::DrawKind kind, const ni::GeometryInput& input) noexcept {
    Impl& d = *impl_;
    if (d.textureTarget && d.currentTarget >= d.renderTargets.size()) {
        d.Count(kind, input.data, DrawOutcome::TextureTarget, false);
        return;
    }
    const ni::ExtractContext c = d.Context();
    // M3d: the transform / skin instance of the interpolated state (arena copies) or the guest input as is.
    interp::GeometryResult interpolated;
    interpolated.input = input;
    if (d.history != nullptr) {
        interpolated = d.history->Geometry(kind, input, d.arena);
    }
    // G1: QDOutline shells (only with a non-default outline option; recognised by the guest transform).
    const bool shell = d.outlines != nullptr && d.outlines->IsShell(input.transform);
    if (shell) {
        const float k = d.outlines->Settings().thickness;
        if (k <= 0.0f) {
            d.outlines->NoteShell(true, false);
            d.Count(kind, input.data, DrawOutcome::Empty, false);  // outline_thickness = 0: outlines off
            return;
        }
        if (k != kDefaultOutlineThickness) {
            interpolated.input.positionsOverride = d.outlines->ScaledPositions(input.transform, input.data, d.arena);
        }
    }
    const ni::GeometryInput& in = interpolated.input;
    ni::ExtractStatus status = ni::ExtractStatus::Unsupported;
    switch (kind) {
    case ni::DrawKind::Shape:
        status = ni::ExtractShape(c, in, &d.item);
        break;
    case ni::DrawKind::Triangles:
        status = ni::ExtractTriangles(c, in, &d.item);
        break;
    case ni::DrawKind::TriStrip:
        status = ni::ExtractTriStrip(c, in, &d.item);
        break;
    case ni::DrawKind::TriStrips:
        status = ni::ExtractStrips(c, in, &d.item);
        break;
    default:
        break;
    }
    if (shell) {
        const bool recolor = status == ni::ExtractStatus::Ok &&
                             (d.outlines->Settings().hasColor || d.outlines->Settings().hasOpacity);
        if (recolor) {
            outline::ApplyShellColor(d.outlines->Settings(), &d.item);
        }
        d.outlines->NoteShell(false, recolor);
    }
    float tint[4];
    if (status == ni::ExtractStatus::Ok && interp::TintColor(interpolated.tint, tint)) {
        interp::ApplyTint(&d.item, tint);  // [render] interpolation_debug = tint
    }
    d.Submit(kind, input.data, status);
}

// M8: slot 52 RenderPoints - camera-facing quads (ni::ExtractPoints). The particles are not interpolated (M3d: they
// stay on the 60-Hz grid of the logic; the camera of the frame orients them).
void GpuRenderer::DrawPoints(const ni::GeometryInput& input) noexcept {
    Impl& d = *impl_;
    if (d.textureTarget && d.currentTarget >= d.renderTargets.size()) {
        d.Count(ni::DrawKind::Points, input.data, DrawOutcome::TextureTarget, false);
        return;
    }
    d.Submit(ni::DrawKind::Points, input.data, ni::ExtractPoints(d.Context(), input, &d.item));
}

void GpuRenderer::DrawScreenPolygon(ni::GuestPtr polygon) noexcept {
    Impl& d = *impl_;
    if (d.textureTarget && d.currentTarget >= d.renderTargets.size()) {
        d.Count(ni::DrawKind::ScreenPolygon, polygon, DrawOutcome::TextureTarget, false);
        return;
    }
    ni::ExtractStatus status = ni::ExtractScreenPolygon(d.Context(), polygon, &d.item);
    const bool ribbon = outline::IsRibbonPolygon(polygon);
    // NiOutline ribbons: one list of the chains instead of the original strip with its bridges (avatar fix,
    // outline.h "Ribbon chains"; always, independent of the outline options).
    std::uint32_t triangles = 0;
    if (ribbon && status == ni::ExtractStatus::Ok &&
        outline::SplitRibbonStrip(polygon, d.arena, &d.item.geometry, &triangles) && triangles == 0) {
        status = ni::ExtractStatus::Empty;
    }
    // G1: NiOutline ribbons (only with a non-default outline option).
    if (d.outlines != nullptr && ribbon) {
        const bool recolored = status == ni::ExtractStatus::Ok &&
                               outline::RecolorRibbon(d.outlines->Settings(), d.arena, &d.item.geometry);
        d.outlines->NoteRibbon(recolored);
    }
    d.Submit(ni::DrawKind::ScreenPolygon, polygon, status);
}

void GpuRenderer::DrawScreenTexture(ni::GuestPtr screenTexture) noexcept {
    Impl& d = *impl_;
    if (d.textureTarget && d.currentTarget >= d.renderTargets.size()) {
        d.Count(ni::DrawKind::ScreenTexture, screenTexture, DrawOutcome::TextureTarget, false);
        return;
    }
    d.Submit(ni::DrawKind::ScreenTexture, screenTexture,
             ni::ExtractScreenTexture(d.Context(), screenTexture, &d.item));
}

void GpuRenderer::CountNotInScene(ni::DrawKind kind, ni::GuestPtr object) noexcept {
    impl_->Count(kind, object, DrawOutcome::NotInScene, false);
}

std::uint32_t GpuRenderer::PurgeLight(ni::GuestPtr niLight) noexcept {
    return ni::PurgeLight(&impl_->shadow, niLight);
}

void GpuRenderer::DisplayFrame(std::uint32_t frame) noexcept {
    Impl& d = *impl_;
    const Settings& s = GetSettings();
    d.renderThread = GetCurrentThreadId();
    platform::NoteFrameWorkDone();  // guest + renderer work of this frame (before screenshots and Present)
    // Frames that leave the back buffer as it was (its picture of two presents ago, review M3 finding 8).
    std::uint32_t issued = 0, drawn = 0;
    for (std::uint32_t k = 0; k < kDrawKindCount; ++k) {
        for (std::uint32_t o = 0; o < kDrawOutcomeCount; ++o) {
            issued += d.frameCounts[k][o];
        }
        drawn += d.frameCounts[k][static_cast<std::uint32_t>(DrawOutcome::Drawn)];
    }
    d.framesWithoutDraw += issued == 0 ? 1 : 0;
    d.framesWithoutColorClear += d.frameColorCleared ? 0 : 1;
    d.framesUnchanged += issued == 0 && !d.frameColorCleared ? 1 : 0;
    Impl::Picture& picture = d.pictures[d.backBuffer];
    if (issued != 0 || d.frameColorCleared) {
        picture = Impl::Picture{frame, issued, drawn};
    }
    // M3b display-coupled pacing: the pacer chooses sync interval, tearing and whether the swap chain gets this
    // frame (platform/frame_pacer.h); OnFrameDisplayed then waits (platform::EndFrame).
    const platform::PacingConfig pacing = PacingConfigOf(s);
    platform::SwapChainInfo chain;
    chain.frameLatencyWaitable = d.device->FrameLatencyWaitable();
    chain.tearingSupported = d.device->Info().tearingSupported;
    // M3d F1 (frame-clock/pacing agent, m3d_frameclock.md 1.3): a frame that drew nothing and cleared no color holds
    // the picture of two presents ago; above the tick rate the pacer keeps the previous picture instead. The device
    // presents the buffer of the previous Present once more (one Present per frame keeps the vblank plans' waitable
    // rhythm) and keeps the rotation; the summary and a screenshot of the frame show that picture.
    chain.frameUnchanged = issued == 0 && !d.frameColorCleared;
    const platform::PresentRequest request = platform::PlanPresent(pacing, chain);
    const bool repeat = request.repeatPrevious && d.device->RepeatPreviousPicture();
    d.repeatedPictures += repeat ? 1u : 0u;
    if (d.summaryFrame) {
        d.LogFrameSummary(frame, repeat ? d.pictures[(d.backBuffer + Impl::kBackBuffers - 1) % Impl::kBackBuffers]
                                        : picture, repeat);
    }
    const bool hotkey = (platform::ConsumeHotkeys() & platform::kHotkeyScreenshot) != 0;
    if (hotkey || Listed(s.screenshotFrames, frame)) {
        ++d.screenshots;
        // M3c: the resolved back buffer (internal size) or the presented image (output size); the PNG is written
        // on a worker thread, which logs "screenshot: <path> (WxH)".
        platform::SaveScreenshot(*d.device, s.screenshotDir, frame,
                                 d.quality.screenshots == ScreenshotSize::Internal ? platform::ScreenshotSource::Internal
                                                                                   : platform::ScreenshotSource::Output);
    }
    if (d.traceFrame && !s.screenshotDir.empty()) {
        // M8 (diagnostics): the render-to-texture targets of a [render] trace_frames frame as PNG next to the
        // screenshots ("frame_NNNNNN_rtt_K.png", resolved, device size).
        for (std::size_t i = 0; i < d.renderTargets.size(); ++i) {
            Impl::RenderTarget& t = d.renderTargets[i];
            if (t.msaa && t.dirty && d.device->ResolveTexture(t.color, t.msaa)) {
                t.dirty = false;
            }
            rhi::Image image;
            std::string error;
            if (d.device->ReadTexture(t.color, 0, image)) {
                char name[64];
                std::snprintf(name, sizeof(name), "frame_%06u_rtt_%zu.png", frame, i);
                if (!platform::WritePngFile(s.screenshotDir / name, image, &error)) {
                    FUT_LOGW(Gfx, "render to texture dump %s: %s", name, error.c_str());
                }
            }
        }
    }
    rhi::PresentDesc present;
    present.syncInterval = request.syncInterval;
    present.allowTearing = request.allowTearing;
    present.skipSwapChain = request.skipSwapChain;
    LARGE_INTEGER qpcCall, qpcReturn;
    QueryPerformanceCounter(&qpcCall);
    const rhi::PresentResult result = d.device->Present(present);
    QueryPerformanceCounter(&qpcReturn);
    {
        const rhi::PresentTiming timing = d.device->LastPresentTiming();
        platform::PresentFeedback feedback;
        feedback.qpcCall = qpcCall.QuadPart;
        feedback.qpcReturn = qpcReturn.QuadPart;
        feedback.swapChainPresented = timing.swapChainPresented;
        feedback.occluded = timing.occluded;
        feedback.presentId = timing.presentId;
        feedback.statsValid = timing.statsValid;
        feedback.statPresentCount = timing.statPresentCount;
        feedback.statSyncRefreshCount = timing.statSyncRefreshCount;
        feedback.statSyncQpc = timing.statSyncQpc;
        platform::NoteSwapChainPresent(feedback);
    }
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
    if (!repeat) {
        d.backBuffer = (d.backBuffer + 1) % Impl::kBackBuffers;  // the device moved on (rhi Device::Present)
    }
    platform::NotePresentedFrame();  // first call: shows the window, registers the "present" reporter
    if (d.history != nullptr) {
        d.history->EndFrame(frame, d.traceFrame);  // M3d: before the arena reset (interpolated transforms live there)
    }
    d.arenaPeak = std::max(d.arenaPeak, d.arena.BytesUsed());
    d.arena.Reset();
    d.framesDone = frame;
    d.FollowView();  // M3c: [render] resolution = auto follows window resizes (between frames)
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

// M6: the host texture of a YUY2 shim texture (movie player). One BGRA8 texture per shim resource (serial), re-uploaded
// when the XMV HLE wrote a new frame (generation) or the YuvEnable state at the draw changed the conversion; the least
// recently used of kMovieTextures entries is reused (the game has one movie texture at a time).
rhi::TextureHandle GpuRenderer::MovieTexture(const d3d::ResourceInfo& info, bool yuvEnable) noexcept {
    Impl& d = *impl_;
    if (info.format != d3d::kFormatYuy2 || info.width == 0 || info.height == 0 || (info.width & 1) != 0 ||
        info.pixels == 0 || info.pitch < 2u * info.width) {
        ++d.movieFailures;
        return {};
    }
    const tex::Yuy2Mode mode = yuvEnable ? tex::Yuy2Mode::ConvertBt601 : tex::Yuy2Mode::Raw;
    if (!yuvEnable && d.movieRawDraws++ == 0) {
        FUT_LOGW(Gfx, "d3d11: YUY2 texture 0x%08X drawn without YuvEnable - raw components (R = Cr, G = Y, B = Cb), "
                 "logged once", info.header);
    }
    // M6b: a movie pack picture attached by the XMV HLE replaces the guest pixels (any even size).
    std::shared_ptr<const d3d::HostImage> image = d3d::GetHostImage(info.header);
    if (image != nullptr && (image->width == 0 || image->height == 0 || (image->width & 1) != 0 ||
                             image->pitch < 2u * image->width ||
                             image->yuy2.size() < static_cast<std::size_t>(image->pitch) * image->height)) {
        image = nullptr;
    }
    const std::uint32_t texW = image != nullptr ? image->width : info.width;
    const std::uint32_t texH = image != nullptr ? image->height : info.height;
    Impl::MovieTex* slot = nullptr;
    for (Impl::MovieTex& m : d.movies) {
        if (m.texture && m.serial == info.serial) {
            slot = &m;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &d.movies[0];
        for (Impl::MovieTex& m : d.movies) {
            if (!m.texture) {
                slot = &m;
                break;
            }
            if (m.lastFrame < slot->lastFrame) {
                slot = &m;
            }
        }
        slot->header = info.header;
        slot->serial = info.serial;
        slot->image = nullptr;
        slot->generation = info.generation + 1;  // force the upload below
    }
    if (slot->texture && (slot->width != texW || slot->height != texH)) {
        d.device->Destroy(slot->texture);  // M6b: pack picture <-> guest pixels (fallback) change the size
        slot->texture = {};
    }
    if (!slot->texture) {
        rhi::TextureDesc desc;
        desc.width = texW;
        desc.height = texH;
        desc.mipLevels = 1;
        desc.format = rhi::Format::BGRA8_UNorm;
        desc.usage = rhi::kTextureSampled;
        desc.debugName = image != nullptr ? "movie texture (pack)" : "movie texture (YUY2)";
        slot->texture = d.device->CreateTexture(desc, nullptr);
        if (!slot->texture) {
            ++d.movieFailures;
            return {};
        }
        ++d.texturesCreated;
        slot->width = texW;
        slot->height = texH;
        slot->generation = info.generation + 1;  // force the upload below
    }
    slot->texelWidth = info.width;
    slot->texelHeight = info.height;
    slot->lastFrame = d.framesDone + 1;
    if (slot->generation != info.generation || slot->mode != mode || slot->image != image) {
        try {
            d.movieScratch.resize(static_cast<std::size_t>(texW) * texH * 4u);
        } catch (...) {
            ++d.movieFailures;
            return slot->texture;
        }
        if (image != nullptr) {
            tex::ConvertYuy2ToBgra(image->yuy2.data(), image->pitch, texW, texH, d.movieScratch.data(), texW * 4u,
                                   mode);
        } else {
            tex::ConvertYuy2ToBgra(guest::Ptr<const std::uint8_t>(info.pixels), info.pitch, texW, texH,
                                   d.movieScratch.data(), texW * 4u, mode);
        }
        rhi::SubresourceData level;
        level.data = d.movieScratch.data();
        level.rowPitch = texW * 4u;
        level.slicePitch = level.rowPitch * texH;
        if (d.device->UpdateTexture(slot->texture, 0, level)) {
            slot->generation = info.generation;
            slot->mode = mode;
            slot->image = std::move(image);
            ++d.movieUploads;
            if (slot->image != nullptr) {
                ++d.moviePackUploads;
                d.moviePackMaxWidth = std::max(d.moviePackMaxWidth, texW);
                d.moviePackMaxHeight = std::max(d.moviePackMaxHeight, texH);
            }
        } else {
            ++d.movieFailures;
        }
    }
    return slot->texture;
}

bool GpuRenderer::OnRenderThread() const noexcept {
    return impl_->renderThread == GetCurrentThreadId();
}

void GpuRenderer::LogStatistics() noexcept {
    Impl& d = *impl_;
    if (!OnRenderThread()) {
        FUT_LOGI(Gfx, "d3d11 statistics skipped: the process ends on thread %lu while the render thread %lu may "
                 "still draw (its counters are not atomic)", GetCurrentThreadId(), d.renderThread);
        return;
    }
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
        "d3d11 frames: %llu presents (%llu occluded; %llu without a draw, %llu without a color clear, %llu "
        "with neither = the back buffer of two presents ago, %llu of them showing the previous picture again (M3d "
        "F1)), %llu screenshots, %llu scenes, %llu cameras, %llu clears (%llu on texture targets skipped, %llu empty "
        "rectangles); textures %llu created, %llu destroyed, %llu failed; arena peak %zu bytes",
        static_cast<unsigned long long>(d.presents), static_cast<unsigned long long>(d.occluded),
        static_cast<unsigned long long>(d.framesWithoutDraw), static_cast<unsigned long long>(d.framesWithoutColorClear),
        static_cast<unsigned long long>(d.framesUnchanged), static_cast<unsigned long long>(d.repeatedPictures),
        static_cast<unsigned long long>(d.screenshots), static_cast<unsigned long long>(d.scenes),
        static_cast<unsigned long long>(d.cameras), static_cast<unsigned long long>(d.clears),
        static_cast<unsigned long long>(d.clearsSkipped), static_cast<unsigned long long>(d.emptyClearRects),
        static_cast<unsigned long long>(d.texturesCreated),
        static_cast<unsigned long long>(d.texturesDestroyed),
        static_cast<unsigned long long>(d.textureFailures), d.arenaPeak);
    if (d.wide) {
        FUT_LOGI(Gfx, "d3d11 widescreen bars: %llu frames with the sides left to the stretched scene (not cleared "
                      "black), the other drawing frames with black bars",
                 static_cast<unsigned long long>(d.barFramesScene));
    }
    FUT_LOGI(Gfx, "d3d11 render to texture (M8): %zu targets (%llu created, %llu failed), %llu draws, %llu clears, %llu "
             "MSAA resolves, %llu texture stages sampling them; %llu projected shadow passes drawn",
             d.renderTargets.size(), static_cast<unsigned long long>(d.rttCreated),
             static_cast<unsigned long long>(d.rttFailures), static_cast<unsigned long long>(d.rttDraws),
             static_cast<unsigned long long>(d.rttClears), static_cast<unsigned long long>(d.rttResolves),
             static_cast<unsigned long long>(d.rttSampled), static_cast<unsigned long long>(d.extraPassDraws));
    if (d.movieUploads != 0 || d.movieFailures != 0 || d.movieRawDraws != 0) {
        FUT_LOGI(Gfx, "d3d11 movie textures (YUY2): %llu frame uploads, %llu texel-addressed stage draws, %llu draws "
                 "without YuvEnable, %llu failures",
                 static_cast<unsigned long long>(d.movieUploads), static_cast<unsigned long long>(d.movieStageDraws),
                 static_cast<unsigned long long>(d.movieRawDraws), static_cast<unsigned long long>(d.movieFailures));
    }
    if (d.moviePackUploads != 0) {
        FUT_LOGI(Gfx, "d3d11 movie pack pictures (M6b): %llu frame uploads, up to %ux%u",
                 static_cast<unsigned long long>(d.moviePackUploads), d.moviePackMaxWidth, d.moviePackMaxHeight);
    }
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
    // M3c: the back-buffer configuration at the end and what the anti-aliasing options did.
    FUT_LOGI(Gfx,
             "render totals: output %ux%u, internal %ux%u, MSAA %ux, %llu back-buffer resizes (%llu failed)%s; alpha "
             "test: %llu draws with alpha to coverage, %llu with discard; %llu anisotropic texture stages, %llu "
             "point-filtered stages as anti-aliased nearest; %llu draws with a destination-alpha blend%s",
             d.sizes.outputWidth, d.sizes.outputHeight, d.sizes.internalWidth, d.sizes.internalHeight, d.samples,
             static_cast<unsigned long long>(d.resizes), static_cast<unsigned long long>(d.resizeFailures),
             d.reduced ? ", quality reduced" : "",
             static_cast<unsigned long long>(f.alphaToCoverageDraws),
             static_cast<unsigned long long>(f.alphaTestDiscardDraws),
             static_cast<unsigned long long>(f.anisotropicStages),
             static_cast<unsigned long long>(f.sharpNearestStages),
             static_cast<unsigned long long>(f.destAlphaBlendDraws),
             f.alphaToCoverageSuspended ? " (alpha to coverage suspended)" : "");
    const ni::ExtractCounters& e = d.shadow.counters;
    FUT_LOGI(
        Gfx,
        "ni_extract: %llu states, %llu multi-map approximated, %llu with texture effects, %llu light "
        "evaluations (%llu lights dropped, %llu unknown types), %llu skinned draws (%llu deforms, %llu cache "
        "hits), %llu invalid map modes, %llu unresolved textures, %llu enabled lights purged (slot 60), %llu projected "
        "shadow passes, %llu texture effects not reproduced",
        static_cast<unsigned long long>(e.states), static_cast<unsigned long long>(e.multiMapApproximated),
        static_cast<unsigned long long>(e.textureEffectDraws),
        static_cast<unsigned long long>(e.lightEvaluations), static_cast<unsigned long long>(e.lightsDropped),
        static_cast<unsigned long long>(e.unknownLightTypes), static_cast<unsigned long long>(e.skinnedDraws),
        static_cast<unsigned long long>(e.skinDeforms), static_cast<unsigned long long>(e.skinCacheHits),
        static_cast<unsigned long long>(e.invalidMapModes),
        static_cast<unsigned long long>(e.unresolvedTextures), static_cast<unsigned long long>(e.lightsPurged),
        static_cast<unsigned long long>(e.shadowPasses), static_cast<unsigned long long>(e.shadowEffectsSkipped));
    const rhi::DeviceStats ds = d.device->Stats();
    FUT_LOGI(Gfx,
             "d3d11 device: %llu draws, %llu primitives, %llu presents, %llu MiB transient, %u pipelines, %u "
             "samplers, %u buffers, %u textures (%llu KiB), %u debug errors, %u debug warnings",
             static_cast<unsigned long long>(ds.draws), static_cast<unsigned long long>(ds.primitives),
             static_cast<unsigned long long>(ds.presents),
             static_cast<unsigned long long>(ds.transientBytes >> 20), ds.pipelines, ds.samplers, ds.buffers,
             ds.textures, static_cast<unsigned long long>(ds.textureBytes >> 10), ds.debugErrors,
             ds.debugWarnings);
    // M3c: GPU time per frame (timestamp queries; the frame work of "present: frame work" is the CPU side).
    FUT_LOGI(Gfx, "d3d11 gpu frame time: %llu frames timed, average %.2f ms, 99th percentile %.1f ms, longest %.1f ms",
             static_cast<unsigned long long>(ds.gpuFramesTimed), ds.gpuFrameMsAverage, ds.gpuFrameMsP99,
             ds.gpuFrameMsMax);
    if (d.history != nullptr) {
        d.history->LogStatistics();  // M3d: "interpolation: ..."
    }
    if (d.outlines != nullptr) {
        d.outlines->LogStatistics();  // G1: "outlines: ..."
    }
    outline::LogRibbonStatistics();  // avatar fix: "outline ribbons: ..."
}

}  // namespace gfx
