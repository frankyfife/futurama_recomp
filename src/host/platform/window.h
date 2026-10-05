// Main window of the PC port (design 1.1 UI/boot thread, 1.10 stage 12; GFX_CONTRACT.md 8). Owned by the
// backend agent (M3; platform/** moved from the core agent to the backend agent for M3).
//
// Thread model: platform::StageCreateWindow (HostMain stage 12, boot thread) creates the window when
// [render] backend != null; the same thread later runs platform::StageMessageLoop (stage 14) and thereby
// pumps the window's messages while the guest main thread renders. The D3D11/DXGI swap chain is created
// on the guest main thread for this HWND (allowed: a swap chain may use a window of another thread; the
// UI thread must keep pumping messages, which StageMessageLoop does). The window is hidden until the first
// presented frame (ShowMainWindow; platform::NotePresentedFrame calls it). Closing it ends the process
// orderly: core::ExitHostProcess(0, "window closed") - exit reporters run, exit code 0.
//
// Layout: a top-level frame window "Futurama" (resizable; client area 640x480 * [render] window_scale at 100 %
// desktop scaling, i.e. * DPI / 96 in physical pixels, shrunk to the largest 4:3 area that fits the work
// area; [render] fullscreen = borderless window on the whole monitor) with a
// black background and one child "view" window that keeps the 4:3 aspect of the back buffer, centered
// (letterbox/pillarbox). MainWindow() is the VIEW: the swap chain stretches the 640x480 back buffer onto it
// (DXGI_SCALING_STRETCH), so resizing never needs ResizeBuffers. The view is transparent for the mouse
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
};
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
// (NiXBoxRenderer::Create: 640x480) and is stretched to it by the swap chain.
void MainWindowClientSize(std::uint32_t* width, std::uint32_t* height) noexcept;

// Hotkeys pressed in the window since the last call, as a bit mask (thread-safe; the renderer polls it in
// DisplayFrame on the guest main thread).
inline constexpr std::uint32_t kHotkeyScreenshot = 1;  // F12 (and Print Screen): save the next frame as PNG
std::uint32_t ConsumeHotkeys() noexcept;
// Sets hotkey bits as if the keys had been pressed (tests, scripted runs).
void PostHotkeys(std::uint32_t mask) noexcept;

// Largest rectangle of aspect aspectW:aspectH inside a client area of clientW x clientH, centered
// (x, y, width, height). Used for the view layout; pure (tests).
struct ViewRect {
    std::int32_t x = 0, y = 0;
    std::uint32_t width = 0, height = 0;
};
ViewRect FitAspectRect(std::uint32_t clientW, std::uint32_t clientH, std::uint32_t aspectW, std::uint32_t aspectH) noexcept;

}  // namespace platform
