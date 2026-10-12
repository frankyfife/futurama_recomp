// The PC port's input layer (M4): wiring of router, backends, window and XPP HLE (analysis\loader\m4_input.md 8).
//
// - The router (input/host_router.h) is installed with inp::SetRouter while the host DLL loads, i.e. before the guest
//   runs (input_api.h); until StartInput it routes only the sources registered by other modules (input script).
// - StartInput (first XPP call of the game - XInitDevices from the init FUN_0009C2A0 - or any later XPP entry;
//   idempotent, thread-safe): reads [input] (input/input_config.h), configures the router, registers the four XInput
//   controllers and keyboard/mouse (RegisterSource), attaches keyboard/mouse to the game window (Raw Input, input/
//   kbm_window.h; nothing without a window, e.g. [render] backend = null), registers the exit reporter "input"
//   (summary line, motors off) and the crash reporter "input" (motors off), and logs one INP/info line.
// - Port model (m4_input.md 8.1): pad_mode = single - all controllers, keyboard/mouse and the input script feed port 1
//   (merged: buttons OR'ed, the larger deflection wins), port 1 is always connected; per_port - controller n feeds
//   port n, keyboard/mouse and the script stay on port 1.
// - Focus: physical input counts only while the game window is the foreground window (GetForegroundWindow ==
//   platform::MainFrameWindow; always without a window); background_input = 1 keeps controllers (not keyboard/mouse,
//   whose Raw Input is foreground-only) active.
#pragma once

#include "input/host_router.h"
#include "input/input_config.h"
#include "input/kbm_source.h"
#include "input/xinput_source.h"

namespace inp {

// The router behind inp::GetRouter() (installed at DLL load).
HostRouter& GlobalRouter() noexcept;
// The backends (constructed on first use; registered with the router by StartInput).
KeyboardMouseSource& GlobalKeyboardMouse() noexcept;
XInputPadSource& GlobalXInputPad(std::uint32_t user) noexcept;  // user 0..3

void StartInput() noexcept;
bool InputStarted() noexcept;
// Settings in effect (defaults before StartInput).
const InputSettings& ActiveInputSettings() noexcept;

// True while the game window has the focus (the router's focus query).
bool GameWindowFocused() noexcept;
// True while the game's current context is "Frontend" (CApp+0xBC, kbm menu escape). Guest main thread only.
bool GameInFrontendMenu() noexcept;

// Logs "input summary: ..." (INP/info) and stops all motors. Exit reporter "input".
void LogInputSummary() noexcept;

}  // namespace inp
