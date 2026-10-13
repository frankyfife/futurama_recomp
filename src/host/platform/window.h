// Main window of the PC port (design 1.1 UI/boot thread, 1.10 stage 12; GFX_CONTRACT.md 8). Owned by the
// backend agent (M3; platform/** moved from the core agent to the backend agent for M3).
//
// Thread model: platform::StageCreateWindow (HostMain stage 12, boot thread) creates the window when
// [render] backend != null; the same thread later runs platform::StageMessageLoop (stage 14) and thereby
// pumps the window's messages while the guest main thread renders. The D3D11/DXGI swap chain is created
// on the guest main thread for this HWND (allowed: a swap chain may use a window of another thread; the
// UI thread must keep pumping messages, which StageMessageLoop does). The window is hidden until the first
// presented frame (ShowMainWindow; platform::NotePresentedFrame calls it). Closing it (WM_CLOSE) sets
// CloseRequested(); NiPCRenderer's DisplayFrame then ends the process orderly on the guest main thread
// (core::ExitHostProcess(0, "window closed") - exit reporters run on the render thread, exit code 0; M3
// review findings 5/10). If no frame is displayed within 3 s, the UI thread ends the process itself.
//
// Layout: a top-level frame window "Futurama" on the monitor of [render] monitor (M3c; default the primary one;
// resizable; client area 640x480 * [render] window_scale at 100 % desktop scaling, i.e. * DPI / 96 of that
// monitor in physical pixels, shrunk to the largest 4:3 area that fits its work area; [render] fullscreen =
// borderless window on the whole monitor) with a
// black background and one child "view" window that keeps the 4:3 aspect of the back buffer, centered
// (letterbox/pillarbox; M3d: in borderless fullscreen the view covers the whole monitor, ViewLayout, and the present
// pass draws the bars). MainWindow() is the VIEW: the swap chain has its client size (the D3D11 backend
// resizes it in Present) and the present pass scales the 640x480 back buffer onto it ([render] scaling =
// linear | point | integer, gfx/rhi/rhi.h PresentScaling). The view is transparent for the mouse
// (HTTRANSPARENT) and never takes the keyboard focus, so input goes to the frame.
// Keys of the frame: F12 / Print Screen = screenshot (kHotkeyScreenshot), Alt+Enter = toggle borderless
// fullscreen, Alt+F4 / close button = WM_CLOSE. Per-monitor DPI aware (V2): sizes are physical pixels.
#pragma once

#include <cstdint>

namespace platform {

struct WindowOptions {
    std::uint32_t scale = 2;     // client area = (kViewWidth x kViewHeight) * scale * DPI / 96 (1..8)
    bool fullscreen = false;     // borderless window covering the monitor
    const wchar_t* title = L"Futurama";
    // [render] monitor (M3c): GDI device name of the monitor the window opens on, as `futurama.exe
    // --query-gpu` lists it ("\\.\DISPLAY2"); nullptr, "" or "primary" = the primary monitor. A name that is not
    // connected falls back to the primary monitor (Core warning). Window size and DPI follow that monitor.
    const char* monitor = nullptr;
    // G3 widescreen ([render] aspect, gfx/widescreen.h): aspect of the view instead of the back buffer's 4:3
    // (0 x 0 = 4:3). The window's client area at startup is (kViewHeight * aspectW / aspectH) x kViewHeight * scale.
    std::uint32_t aspectW = 0;
    std::uint32_t aspectH = 0;
};

// Monitor selection for [render] monitor (M3c). `name` as in WindowOptions::monitor (blanks around it are
// ignored, case-insensitive). The result is always a valid monitor: the named one, else the primary one.
struct MonitorChoice {
    HMONITOR monitor = nullptr;
    bool primary = true;   // the result is the primary monitor
    bool found = true;     // false: `name` named a monitor that is not connected (fallback to the primary)
    char device[40] = {};  // GDI device name of the result (UTF-8), e.g. "\\.\DISPLAY1"
};
MonitorChoice SelectMonitor(const char* name) noexcept;
inline constexpr std::uint32_t kViewWidth = 640;   // NiXBoxRenderer::Create size (aspect of the view)
inline constexpr std::uint32_t kViewHeight = 480;

// Creates the (hidden) frame + view on the calling thread, which must pump its messages afterwards
// (StageMessageLoop). Idempotent: false + error if creation fails or a window already exists.
bool CreateMainWindow(const WindowOptions& options, const char** error) noexcept;
// Destroys the window (calling thread = creating thread; unit tests). MainWindow() is nullptr afterwards.
void DestroyMainWindow() noexcept;

// The view window the swap chain presents into, nullptr if there is none ([render] backend = null, unit
// tests, creation failed).
HWND MainWindow() noexcept;
// The top-level frame window (nullptr if none).
HWND MainFrameWindow() noexcept;

// Makes the window visible (first presented frame). Any thread (ShowWindowAsync posts to the UI thread);
// idempotent.
void ShowMainWindow() noexcept;
bool MainWindowShown() noexcept;

// Client area size of the view in pixels (0, 0 without a window). The back buffer keeps the guest size
// (NiXBoxRenderer::Create: 640x480) and is scaled to it by the present pass.
void MainWindowClientSize(std::uint32_t* width, std::uint32_t* height) noexcept;

// Hotkeys pressed in the window since the last call, as a bit mask (thread-safe; the renderer polls it in
// DisplayFrame on the guest main thread).
inline constexpr std::uint32_t kHotkeyScreenshot = 1;  // F12 (and Print Screen): save the next frame as PNG
std::uint32_t ConsumeHotkeys() noexcept;
// Sets hotkey bits as if the keys had been pressed (tests, scripted runs).
void PostHotkeys(std::uint32_t mask) noexcept;

// The window received WM_CLOSE (close button, Alt+F4). Thread-safe; NiPCRenderer's DisplayFrame polls it
// and ends the process on the guest main thread.
bool CloseRequested() noexcept;

// Display environment for the frame pacer (M3b display-coupled pacing, platform/frame_pacer.h; analysis/loader/
// pacing60.md 3.4). The frame window's procedure (UI thread) keeps it up to date; the render thread reads it at
// every frame boundary:
//   displayOn      GUID_CONSOLE_DISPLAY_STATE (RegisterPowerSettingNotification on the frame window; Windows
//                  sends the current state right after the registration): 0 = off -> false, on/dimmed -> true
//   sessionLocked  WM_WTSSESSION_CHANGE WTS_SESSION_LOCK/UNLOCK (WTSRegisterSessionNotification); the initial
//                  state comes from WTSQuerySessionInformationW(WTSSessionInfoEx)
//   epoch          +1 on WM_DISPLAYCHANGE (mode/refresh/monitor change), WM_DPICHANGED, the Alt+Enter
//                  fullscreen toggle (VRR/tearing only apply in independent flip), display on/off and
//                  lock/unlock: the pacer queries the refresh rate again and plans anew.
// Foreground (GetForegroundWindow() == MainFrameWindow()), minimized (IsIconic) and the window's monitor
// (MonitorFromWindow) are polled by the pacer itself. Without a window: displayOn, unlocked, epoch 0.
struct DisplayEnvironment {
    bool displayOn = true;
    bool sessionLocked = false;
    std::uint32_t epoch = 0;
};
DisplayEnvironment GetDisplayEnvironment() noexcept;
// Raises the epoch as if the display configuration had changed (tests; any thread).
void NoteDisplayChange() noexcept;

// Largest rectangle of aspect aspectW:aspectH inside a client area of clientW x clientH, centered
// (x, y, width, height). Used for the view layout; pure (tests).
struct ViewRect {
    std::int32_t x = 0, y = 0;
    std::uint32_t width = 0, height = 0;
};
ViewRect FitAspectRect(std::uint32_t clientW, std::uint32_t clientH, std::uint32_t aspectW, std::uint32_t aspectH) noexcept;
// M3d (frame-clock/pacing agent; M3c report: VRR in fullscreen): the view inside the frame's client area. Windowed:
// the centered 4:3 rectangle (FitAspectRect, unchanged). Borderless fullscreen: the whole client area, i.e. the whole
// monitor - only a swap chain that covers the monitor can use independent flip (VRR, tearing); the present pass
// draws the 4:3 picture with black bars itself (gfx/rhi PresentScaling, the renderer's 4:3 output size). Pure (tests).
ViewRect ViewLayout(std::uint32_t clientW, std::uint32_t clientH, bool fullscreen) noexcept;
// G3: the same for a view of aspect aspectW:aspectH (0 x 0 = kViewWidth:kViewHeight).
ViewRect ViewLayout(std::uint32_t clientW, std::uint32_t clientH, bool fullscreen, std::uint32_t aspectW,
                    std::uint32_t aspectH) noexcept;

}  // namespace platform
