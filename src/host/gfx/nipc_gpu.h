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
//   slot 16 DisplayFrame             screenshot ([render] screenshot_frames / F12) -> Present (VSync per
//                                    [render] vsync) -> platform::NotePresentedFrame -> arena reset; the
//                                    caller then paces (platform::PaceFrame) and counts the frame
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
#pragma once

#include "gfx/ff_pipeline.h"
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

class GpuRenderer {
public:
    struct CreateParams {
        void* window = nullptr;     // HWND of the view (platform::MainWindow)
        std::uint32_t width = 640;  // NiXBoxRenderer::Create size = back buffer
        std::uint32_t height = 480;
        bool debugLayer = false;
        bool softwareAdapter = false;
        ni::TextureResolver resolveTexture = nullptr;  // NiTexture* -> host texture (nipc_renderer.cpp)
        void* resolveUser = nullptr;
        rhi::PresentScaling scaling = rhi::PresentScaling::Linear;  // [render] scaling
    };
    // Device + swap chain + fixed-function pipeline + the 8x8 default texture (FUN_0014AB50) + a black
    // texture. nullptr and the reason on failure (the caller falls back to the null renderer).
    static std::unique_ptr<GpuRenderer> Create(const CreateParams& params, std::string* error) noexcept;
    ~GpuRenderer();
    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;

    rhi::Device& Device() noexcept;
    // "<adapter>, <api>, debug layer <enabled|not installed|off>[ (<note>)]" for the Create log line.
    std::string Describe() const;

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
    rhi::TextureHandle BlackTexture() const noexcept;    // movie (YUY2, M6) and rendered (P2) textures

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
