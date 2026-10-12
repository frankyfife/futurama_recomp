// Raw Input of the game window -> KeyboardMouseSource (analysis\loader\m4_input.md 8.1/8.3; [input] mouse_capture).
//
// The window belongs to platform\ (UI thread = boot thread, StageMessageLoop). The input layer does not change it:
// AttachKeyboardMouse subclasses the frame window (SetWindowLongPtrW GWLP_WNDPROC, allowed from another thread of
// the same process) and posts a registered message; the subclass procedure, running on the UI thread, then registers
// Raw Input for keyboard (usage 1/6) and mouse (1/2) with hwndTarget = frame and without RIDEV_INPUTSINK, so input
// only arrives while the game window is in the foreground. Legacy WM_KEYDOWN/WM_MOUSE* messages stay on (no
// RIDEV_NOLEGACY): the frame's own keys (F12 screenshot, Alt+Enter fullscreen) keep working; Enter while Alt is
// held is not passed to the game (Alt+Enter must not press A).
//
// Focus: WM_ACTIVATE (inactive), WM_ACTIVATEAPP (FALSE) and WM_KILLFOCUS release every key/button of the source and
// the mouse capture. Mouse capture (mouse_capture = 1): a click into the client area captures the mouse (cursor hidden
// and confined to the client area; the capturing click itself is not passed on); in borderless fullscreen the mouse is
// captured whenever the window gets the focus. While not captured, mouse buttons, wheel and movement are ignored.
// mouse_capture = 0: mouse input counts whenever the window has the focus, the cursor stays free.
#pragma once

#include <cstdint>

namespace inp {

class KeyboardMouseSource;

struct KbmWindowOptions {
    bool mouseCapture = true;
};

// Subclasses `frame` (HWND of platform::MainFrameWindow) and starts Raw Input on its thread. False if the window is
// invalid or already attached. Any thread.
bool AttachKeyboardMouse(void* frame, KeyboardMouseSource* source, const KbmWindowOptions& options) noexcept;
// True once the UI thread registered the Raw Input devices.
bool KeyboardMouseAttached() noexcept;
// The mouse is captured right now (UI thread state).
bool MouseCaptured() noexcept;

struct KbmWindowStats {
    std::uint64_t rawKeyboard = 0;
    std::uint64_t rawMouse = 0;
    std::uint64_t ignoredMouse = 0;  // mouse records while not captured
    std::uint32_t captures = 0;
    std::uint32_t focusReleases = 0;
    std::uint32_t absoluteMouse = 0;  // MOUSE_MOVE_ABSOLUTE records (remote desktop, tablets): movement ignored
};
KbmWindowStats GetKbmWindowStats() noexcept;

}  // namespace inp
