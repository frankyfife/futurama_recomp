// GPU path of NiPCRenderer (M3 "Erstes Bild", design 6 M3; GFX_CONTRACT.md 2 and 9). Owned by the
// integration (phase 3) together with gfx/nipc_renderer.cpp.
//
// With [render] backend = d3d11 the NiXBoxRenderer::Create detour creates one GpuRenderer for the main
// window (platform::MainWindow, the 4:3 view of the boot thread's frame window). The slot handlers of
// gfx/nipc_renderer.cpp keep the guest-visible part (object layout, texture renderer data, the source
// order of slot 65, counters, the --frames limit) and hand the drawing to this class:
//
//   slot 15 SetRenderTarget(NULL)    SetBackBufferTarget: back buffer + depth, viewport = whole target
//                                    (D3D8 SetRenderTarget resets the viewport); a texture target
//                                    (render to texture, P2, not reached in M3) makes draws and clears
//                                    no-ops that are counted (DrawOutcome::TextureTarget)
//   slot 17 ClearBuffer(rect, mode)  0x146490: viewport = whole target (0x14659E..0x1465B9, not
//                                    restored), then D3DDevice_Clear with mode bit 1 color, 4 depth,
//                                    2 stencil and the rect of ni::ClearRect (NULL = whole target)
//   slot 45 BeginScene               ni::BeginScene (FUN_00158B90 + FUN_0015B990 cache resets)
//   slot 47 SetCameraData            ni::ExtractCamera -> D3D viewport + ff camera
//   slots 48-51, 54, 55              ni::Extract* -> ff::FixedFunctionPipeline::Draw (16-bit indices,
//                                    transient ring); outcome counted per draw kind and frame
//   slot 16 DisplayFrame             screenshot ([render] screenshot_frames / F12) -> platform::PlanPresent
//                                    (M3b: sync interval / tearing / skip) -> Present -> platform::
//                                    NoteSwapChainPresent + NotePresentedFrame -> arena reset; the caller
//                                    then paces (platform::EndFrame) and counts the frame
//
// Viewport rule (ff_pipeline.h): pre-transformed pixels (slot 55) are mapped relative to
// CameraDesc::viewport, so the camera handed to the pipeline always carries the viewport that is set on
// the context (ClearBuffer changes only the viewport, not the transforms - like the original).
//
// Diagnostics: [render] trace_frames logs every draw of the listed frames ("draw trace frame N: ..."),
// trace and screenshot frames end with a per-frame summary ("draw summary frame N: ..."), the exit
// reporter "gfx" logs the totals (LogStatistics). Debug-layer messages, the "D3D11 debug layer:" exit line
// and the "present:" statistics come from the backend and the frame pacer themselves.
//
// Back buffer contents (review M3 findings 2/8): the device keeps 2 persistent back buffers in rotation, so a
// frame that draws nothing or clears only depth (legal screen, tech notice: 0 or 12 draws per frame, depth
// clear only) shows the picture of two presents ago like the Xbox flip; the exit line "d3d11 frames: ..."
// counts such frames.
//
// Threading: everything runs on the guest main thread (slot handlers; loading fibers run on the same
// OS thread). All functions are noexcept.
//
// M3c - render resolution and anti-aliasing (GFX_CONTRACT.md 12; [render] resolution / render_scale / msaa /
// alpha_to_coverage / anisotropy / screenshot_size, gfx/gfx_config.h):
//   - The guest keeps its 640x480 world (slots 21/22, ni_extract, the ff camera, trace bounds are all in
//     "guest pixels" of the Create size). Only the D3D11 targets grow: the back buffers have the INTERNAL size =
//     the largest 4:3 rectangle inside the window's view (auto) or inside [render] resolution (= the OUTPUT size)
//     times render_scale, with [render] msaa samples per pixel and a depth buffer of the same size and samples.
//   - Guest pixels -> internal pixels: the D3D viewport (ScaleViewport) and the clear rectangles (ScaleRect).
//     Everything inside the viewport is resolution independent: transformed geometry (incl. the +0.5 pixel
//     shift of B19), the pre-transformed quads of slot 55 (pretransform constants from the guest viewport), the
//     screen camera of slot 54 and the NV2A 1/16-pixel snap (in guest pixels, i.e. 1/16 of a 640x480 pixel =
//     internal_size / 640 / 16 internal pixels: the vertices stay where the original rasterizer put them).
//     Lines and points (slots 52/53, wireframe fill) are P2; when they come they must be widened by the scale.
//     Render to texture (slot 15 with a texture, P2) keeps the texture's own size (scale 1).
//   - Present: resolve (MSAA) -> downsample to the output size (render_scale > 1: box / Catmull-Rom with
//     anti-ringing; with [render] linear_aa both on linear light) -> present pass into the window (1:1 when the
//     output fills the view, else [render] scaling).
//   - Review M3c finding 2: when the back buffers of the requested configuration cannot be created (out of video
//     memory, the D3D11 limit per resource) Create and the view-following resize step down (QualityFallbacks:
//     render scale 1, MSAA halved, finally 640x480 without MSAA) with a GFX warning instead of failing.
//   - [render] resolution = auto follows the view: a view size that stays changed for kResizeSettleFrames frames
//     (window resize, Alt+Enter) recreates the back buffers (rhi::Device::ResizeBackBuffers keeps their pictures).
//   - "Classic" (resolution 640x480, render_scale 1, msaa off, anisotropy off) is the M3 renderer bit for bit.
#pragma once

#include "gfx/d3d_shim.h"
#include "gfx/ff_pipeline.h"
#include "gfx/gfx_config.h"
#include "gfx/ni_extract.h"
#include "gfx/rhi/rhi.h"
#include "gfx/texconv.h"

#include <cstdint>
#include <memory>
#include <string>

namespace gfx {

// What happened to one draw slot call.
enum class DrawOutcome : std::uint8_t {
    Drawn,          // ff::DrawResult::Drawn
    Skipped,        // ff::DrawResult::Skipped (FixedFunctionPipeline::LastDrawNote says why)
    Failed,         // ff::DrawResult::Failed (device error, logged by the pipeline)
    Empty,          // ni::ExtractStatus::Empty: the original draws nothing either
    Unsupported,    // ni::ExtractStatus::Unsupported
    Error,          // ni::ExtractStatus::Error (inconsistent guest data)
    NotInScene,     // outside BeginScene/EndScene: the original returns at once (+0x500 == 0)
    TextureTarget,  // a texture is the render target (render to texture is P2)
    Count
};
const char* DrawOutcomeName(DrawOutcome outcome) noexcept;
inline constexpr std::uint32_t kDrawKindCount = 6;  // ni::DrawKind
inline constexpr std::uint32_t kDrawOutcomeCount = static_cast<std::uint32_t>(DrawOutcome::Count);
const char* DrawKindName(ni::DrawKind kind) noexcept;

// D3D8 and the NV2A put pixel centers on integer screen coordinates, D3D10+ on +0.5. Moving transformed
// geometry by +0.5 pixel right and down makes it cover the same pixels (the pre-transformed path does the
// same in the shader): clip.x += clip.w / viewportWidth, clip.y -= clip.w / viewportHeight, i.e. column 3 of
// the (row-vector) projection times the offsets is added to columns 0 and 1. Applied by GpuRenderer to the
// projection and the screen projection it hands to the pipeline (design 8.3 B19).
void HalfPixelShift(Matrix4* projection, float viewportWidth, float viewportHeight) noexcept;

// ---- M3c: render resolution and anti-aliasing ---------------------------------------------------------------

// What the back buffer is rendered with. The default is "classic" (the M3 renderer).
struct RenderQuality {
    std::uint32_t resolutionWidth = 0;   // [render] resolution; 0 x 0 = auto (the window's view; headless: the
    std::uint32_t resolutionHeight = 0;  // Create size)
    float renderScale = 1.0f;            // [render] render_scale (supersampling factor on top, 1..2)
    std::uint32_t msaa = 1;              // [render] msaa: requested samples per pixel (the device clamps)
    bool alphaToCoverage = false;        // [render] alpha_to_coverage (only with msaa > 1)
    std::uint32_t anisotropy = 1;        // [render] anisotropy (1 = off)
    ScreenshotSize screenshots = ScreenshotSize::Output;  // [render] screenshot_size
    bool linearAa = false;               // [render] linear_aa: resolve/downsample on linear light (finding 3)
};
RenderQuality RenderQualityOf(const Settings& settings) noexcept;

// Review M3c finding 2: the configurations GpuRenderer::Create tries in this order when the back buffers of one
// cannot be created: the requested one, render scale 1, MSAA halved step by step down to off, finally the classic
// 640x480 without MSAA (anti-aliasing options kept, they have no effect then). Pure (tests).
struct QualityLadder {
    static constexpr std::uint32_t kMaxSteps = 8;
    RenderQuality steps[kMaxSteps];
    std::uint32_t count = 0;
};
QualityLadder QualityFallbacks(const RenderQuality& requested) noexcept;
// "render scale 2.00, MSAA 8x, resolution auto" - the part of a quality the ladder changes (log lines).
std::string DescribeQuality(const RenderQuality& quality);

// Sizes of one back-buffer configuration.
struct RenderSizes {
    std::uint32_t outputWidth = 0;    // the presented 4:3 image ([render] resolution or the view, fitted to 4:3)
    std::uint32_t outputHeight = 0;
    std::uint32_t internalWidth = 0;  // the back buffers = output x render scale
    std::uint32_t internalHeight = 0;
    float renderScale = 1.0f;         // effective render scale (lowered if internal would exceed 16384)
    friend bool operator==(const RenderSizes&, const RenderSizes&) = default;
};
// Pure (tests): guest = Create size (640x480; its aspect is the game's 4:3), resolution 0 x 0 = auto = the view
// (0 x 0 too: the guest size). Output = the largest guest-aspect rectangle inside the resolution/view
// (platform::FitAspectRect, the same rounding as the view layout); internal = round(output x scale).
RenderSizes ComputeRenderSizes(std::uint32_t guestWidth, std::uint32_t guestHeight, std::uint32_t resolutionWidth,
                               std::uint32_t resolutionHeight, std::uint32_t viewWidth, std::uint32_t viewHeight,
                               float renderScale) noexcept;
// Guest pixels (of guestW x guestH) -> internal pixels (of internalW x internalH); identity at equal sizes.
// Rectangles round each edge to the nearest internal pixel, so rectangles that tile the guest target tile the
// internal one.
rhi::Viewport ScaleViewport(const rhi::Viewport& viewport, std::uint32_t guestW, std::uint32_t guestH,
                            std::uint32_t internalW, std::uint32_t internalH) noexcept;
rhi::Rect ScaleRect(const rhi::Rect& rect, std::uint32_t guestW, std::uint32_t guestH, std::uint32_t internalW,
                    std::uint32_t internalH) noexcept;
// Frames a changed view size must stay before the back buffers follow it ([render] resolution = auto).
inline constexpr std::uint32_t kResizeSettleFrames = 12;

class GpuRenderer {
public:
    struct CreateParams {
        void* window = nullptr;     // HWND of the view (platform::MainWindow)
        std::uint32_t width = 640;  // NiXBoxRenderer::Create size = the guest's back buffer size
        std::uint32_t height = 480;
        bool debugLayer = false;
        bool softwareAdapter = false;
        ni::TextureResolver resolveTexture = nullptr;  // NiTexture* -> host texture (nipc_renderer.cpp)
        void* resolveUser = nullptr;
        rhi::PresentScaling scaling = rhi::PresentScaling::Linear;  // [render] scaling
        RenderQuality quality;                // M3c (default: classic)
        std::uint32_t viewWidth = 0;          // M3c: view size for resolution auto; 0 x 0 = platform::
        std::uint32_t viewHeight = 0;         // MainWindowClientSize with a window, else the Create size
    };
    // Device + swap chain + fixed-function pipeline + the 8x8 default texture (FUN_0014AB50) + a black
    // texture. Steps down the quality (QualityFallbacks) while the back buffers cannot be created. nullptr and the
    // reason when even the last step fails (review M3c finding 2: the caller ends the game with core::Fatal
    // instead of running invisibly on the null renderer).
    static std::unique_ptr<GpuRenderer> Create(const CreateParams& params, std::string* error) noexcept;
    ~GpuRenderer();
    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;

    rhi::Device& Device() noexcept;
    // "<adapter>, <api>, debug layer <enabled|not installed|off>[ (<note>)]" for the Create log line.
    std::string Describe() const;
    // M3c: the current back-buffer configuration and the samples per pixel the device uses.
    const RenderSizes& Sizes() const noexcept;
    std::uint32_t SampleCount() const noexcept;
    // Review M3c finding 2: the quality in effect (after a step down) and whether it was reduced.
    const RenderQuality& Quality() const noexcept;
    bool QualityReduced() const noexcept;
    // M3c: follow a new view size at once (resolution auto; DisplayFrame does it after kResizeSettleFrames
    // frames). False if nothing changed or the device kept the old buffers.
    bool ResizeForView(std::uint32_t viewWidth, std::uint32_t viewHeight) noexcept;

    // ---- slots
    // ----------------------------------------------------------------------------------------------
    void SetBackBufferTarget() noexcept;                                        // slot 15 (NULL)
    void SetTextureTarget(std::uint32_t width, std::uint32_t height) noexcept;  // slot 15 (texture, P2)
    void SetLeftRightSwap(bool swap) noexcept;                                  // slot 19
    void ClearBuffer(const ni::NiRectF* rect, std::uint32_t mode, std::uint32_t argb, float depth,
                     std::uint32_t stencil) noexcept;           // slot 17
    void BeginScene() noexcept;                                 // slot 45
    void SetCameraData(const ni::CameraInput& input) noexcept;  // slot 47 (target size/swap filled here)
    void DrawGeometry(ni::DrawKind kind, const ni::GeometryInput& input) noexcept;  // slots 48-51
    void DrawScreenPolygon(ni::GuestPtr polygon) noexcept;                          // slot 54
    void DrawScreenTexture(ni::GuestPtr screenTexture) noexcept;                    // slot 55
    // A draw slot called outside a scene (the original returns before touching anything).
    void CountNotInScene(ni::DrawKind kind, ni::GuestPtr object) noexcept;
    // Slot 60 PurgeEffect for an NiLight (FUN_0015B9E0): ni::PurgeLight on the state shadow; returns the
    // number of enabled light entries removed.
    std::uint32_t PurgeLight(ni::GuestPtr niLight) noexcept;
    // Slot 16 on the back buffer. `frame` = number of the frame being displayed (1-based, the
    // --frames / screenshot_frames / trace_frames numbering).
    void DisplayFrame(std::uint32_t frame) noexcept;

    // ---- textures (slot 65 and the texture resolver of nipc_renderer.cpp)
    // -------------------------------------
    rhi::TextureHandle CreateTexture(const tex::HostImage& image, const char* debugName) noexcept;
    void DestroyTexture(rhi::TextureHandle texture) noexcept;
    rhi::TextureHandle DefaultTexture() const noexcept;  // FUN_0014AB50 8x8 test pattern
    rhi::TextureHandle BlackTexture() const noexcept;    // rendered textures (P2)
    // M6: host texture of a YUY2 shim texture (the movie player's, written by the XMV HLE): BGRA8, re-uploaded when
    // the shim generation changed; `yuvEnable` = D3DRS_YUVENABLE at the draw (BT.601 conversion, else raw components,
    // tex::Yuy2Mode). Empty handle for anything but a valid YUY2 resource.
    rhi::TextureHandle MovieTexture(const d3d::ResourceInfo& info, bool yuvEnable) noexcept;

    // Totals (exit reporter "gfx"): draws per kind and outcome, clears, frames without draws / color clear,
    // ff/extract/device statistics. On another thread than the render thread (the WM_CLOSE fallback of
    // platform/window.h, a crash report) only a note is logged: the counters are not atomic.
    void LogStatistics() noexcept;
    // The calling thread is the one that created the renderer / displays its frames.
    bool OnRenderThread() const noexcept;

private:
    GpuRenderer() noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gfx
