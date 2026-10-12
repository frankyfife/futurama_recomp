// Raw Input bridge of the game window (see kbm_window.h).
#include "input/kbm_window.h"

#include "core/log.h"
#include "input/kbm_source.h"

#include <atomic>
#include <mutex>

namespace inp {
namespace {

constexpr std::uint16_t kVkReturn = 0x0D;
constexpr std::uint16_t kVkLMenu = 0xA4, kVkRMenu = 0xA5;

std::atomic<KeyboardMouseSource*> g_source{nullptr};
std::atomic<HWND> g_frame{nullptr};
std::atomic<WNDPROC> g_original{nullptr};
std::atomic<bool> g_registered{false};
std::atomic<bool> g_captured{false};
std::atomic<bool> g_mouseCaptureOption{true};
UINT g_attachMessage = 0;  // RegisterWindowMessageW, set before the subclass is installed

std::mutex g_statsLock;
KbmWindowStats g_stats;

template <class F>
void WithStats(F f) noexcept {
    std::lock_guard<std::mutex> lock(g_statsLock);
    f(g_stats);
}

bool IsFullscreen(HWND frame) noexcept {
    return (GetWindowLongPtrW(frame, GWL_STYLE) & WS_CAPTION) == 0;
}

void ClipToClient(HWND frame) noexcept {
    RECT r{};
    if (!GetClientRect(frame, &r)) {
        return;
    }
    POINT tl{r.left, r.top};
    POINT br{r.right, r.bottom};
    ClientToScreen(frame, &tl);
    ClientToScreen(frame, &br);
    const RECT clip{tl.x, tl.y, br.x, br.y};
    ClipCursor(&clip);
}

void Capture(HWND frame) noexcept {
    if (!g_mouseCaptureOption.load() || g_captured.exchange(true)) {
        return;
    }
    ClipToClient(frame);
    SetCursor(nullptr);
    WithStats([](KbmWindowStats& s) { ++s.captures; });
    FUT_LOGD(Inp, "input: mouse captured (click into the window; Alt+Tab releases it)");
}

void ReleaseCapture(bool log) noexcept {
    if (!g_captured.exchange(false)) {
        return;
    }
    ClipCursor(nullptr);
    if (log) {
        FUT_LOGD(Inp, "input: mouse released");
    }
}

void FocusLost() noexcept {
    ReleaseCapture(true);
    if (KeyboardMouseSource* s = g_source.load()) {
        s->ReleaseAll();
    }
    WithStats([](KbmWindowStats& s) { ++s.focusReleases; });
}

bool MouseCounts() noexcept {
    return !g_mouseCaptureOption.load() || g_captured.load();
}

void OnRawKeyboard(const RAWKEYBOARD& k, KeyboardMouseSource& s) noexcept {
    const std::uint16_t vk = NormalizeRawKey(k.VKey, k.MakeCode, k.Flags);
    if (vk == 0) {
        return;
    }
    const bool down = (k.Flags & RI_KEY_BREAK) == 0;
    if (down && vk == kVkReturn && (s.KeyDown(kVkLMenu) || s.KeyDown(kVkRMenu))) {
        return;  // Alt+Enter toggles fullscreen (platform/window.cpp), it must not press a game control
    }
    s.KeyEvent(vk, down);
}

void OnRawMouse(const RAWMOUSE& m, KeyboardMouseSource& s) noexcept {
    if (!MouseCounts()) {
        WithStats([](KbmWindowStats& st) { ++st.ignoredMouse; });
        return;
    }
    const USHORT f = m.usButtonFlags;
    static constexpr struct {
        USHORT down, up;
    } kButtons[5] = {
        {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP},
        {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP},
        {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP},
        {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP},
    };
    for (int b = 0; b < 5; ++b) {
        if ((f & kButtons[b].down) != 0) {
            s.MouseButtonEvent(b, true);
        }
        if ((f & kButtons[b].up) != 0) {
            s.MouseButtonEvent(b, false);
        }
    }
    if ((f & RI_MOUSE_WHEEL) != 0) {
        static_assert(WHEEL_DELTA == kWheelDelta);
        s.WheelDeltaEvent(static_cast<SHORT>(m.usButtonData));  // summed: high-resolution wheels send fractions
    }
    if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        WithStats([](KbmWindowStats& st) { ++st.absoluteMouse; });
        return;
    }
    s.MouseMoveEvent(static_cast<int>(m.lLastX), static_cast<int>(m.lLastY));
}

void OnRawInput(HRAWINPUT handle) noexcept {
    KeyboardMouseSource* s = g_source.load();
    if (s == nullptr) {
        return;
    }
    alignas(8) BYTE buffer[sizeof(RAWINPUT) + 16];
    UINT size = sizeof(buffer);
    if (GetRawInputData(handle, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
        return;
    }
    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);
    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        WithStats([](KbmWindowStats& st) { ++st.rawKeyboard; });
        OnRawKeyboard(raw->data.keyboard, *s);
    } else if (raw->header.dwType == RIM_TYPEMOUSE) {
        WithStats([](KbmWindowStats& st) { ++st.rawMouse; });
        OnRawMouse(raw->data.mouse, *s);
    }
}

void RegisterRawInput(HWND frame) noexcept {
    RAWINPUTDEVICE devices[2] = {};
    devices[0].usUsagePage = 0x01;  // generic desktop
    devices[0].usUsage = 0x06;      // keyboard
    devices[0].hwndTarget = frame;
    devices[1].usUsagePage = 0x01;
    devices[1].usUsage = 0x02;  // mouse
    devices[1].hwndTarget = frame;
    if (RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        g_registered = true;
        FUT_LOGI(Inp, "input: keyboard/mouse attached to the game window (Raw Input, foreground only; mouse capture %s)",
                 g_mouseCaptureOption.load() ? "on click" : "off");
    } else {
        FUT_LOGW(Inp, "input: RegisterRawInputDevices failed (%lu) - no keyboard/mouse input", GetLastError());
    }
    if (GetForegroundWindow() == frame && IsFullscreen(frame)) {
        Capture(frame);
    }
}

void UnregisterRawInput() noexcept {
    if (!g_registered.exchange(false)) {
        return;
    }
    RAWINPUTDEVICE devices[2] = {};
    devices[0].usUsagePage = 0x01;
    devices[0].usUsage = 0x06;
    devices[0].dwFlags = RIDEV_REMOVE;
    devices[1].usUsagePage = 0x01;
    devices[1].usUsage = 0x02;
    devices[1].dwFlags = RIDEV_REMOVE;
    RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
}

LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const WNDPROC original = g_original.load();
    if (msg == g_attachMessage && g_attachMessage != 0) {
        RegisterRawInput(hwnd);
        return 0;
    }
    switch (msg) {
    case WM_INPUT:
        OnRawInput(reinterpret_cast<HRAWINPUT>(lp));
        break;  // the original procedure passes it to DefWindowProc (cleanup)
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            FocusLost();
        } else if (IsFullscreen(hwnd) && HIWORD(wp) == 0) {
            Capture(hwnd);
        }
        break;
    case WM_ACTIVATEAPP:
        if (wp == FALSE) {
            FocusLost();
        }
        break;
    case WM_KILLFOCUS:
        FocusLost();
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (g_mouseCaptureOption.load() && !g_captured.load() && GetForegroundWindow() == hwnd) {
            Capture(hwnd);
        }
        break;
    case WM_SETCURSOR:
        if (g_captured.load() && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_SIZE:
    case WM_MOVE:
    case WM_WINDOWPOSCHANGED:
        if (g_captured.load()) {
            const LRESULT r = CallWindowProcW(original, hwnd, msg, wp, lp);
            ClipToClient(hwnd);
            return r;
        }
        break;
    case WM_NCDESTROY: {
        UnregisterRawInput();
        ReleaseCapture(false);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        g_source = nullptr;
        g_frame = nullptr;
        return CallWindowProcW(original, hwnd, msg, wp, lp);
    }
    default:
        break;
    }
    return CallWindowProcW(original, hwnd, msg, wp, lp);
}

}  // namespace

bool AttachKeyboardMouse(void* frame, KeyboardMouseSource* source, const KbmWindowOptions& options) noexcept {
    HWND hwnd = static_cast<HWND>(frame);
    if (hwnd == nullptr || !IsWindow(hwnd) || source == nullptr) {
        return false;
    }
    HWND expected = nullptr;
    if (!g_frame.compare_exchange_strong(expected, hwnd)) {
        return false;
    }
    g_mouseCaptureOption = options.mouseCapture;
    g_source = source;
    g_attachMessage = RegisterWindowMessageW(L"FuturamaPC.InputAttach");
    // The original procedure is stored before the swap: the UI thread may call SubclassProc right away.
    const LONG_PTR previous = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (previous == 0) {
        FUT_LOGW(Inp, "input: the game window has no window procedure (%lu) - no keyboard/mouse input", GetLastError());
        g_frame = nullptr;
        return false;
    }
    g_original = reinterpret_cast<WNDPROC>(previous);
    SetLastError(0);
    if (SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc)) == 0 && GetLastError() != 0) {
        FUT_LOGW(Inp, "input: cannot subclass the game window (%lu) - no keyboard/mouse input", GetLastError());
        g_frame = nullptr;
        return false;
    }
    if (!PostMessageW(hwnd, g_attachMessage, 0, 0)) {
        FUT_LOGW(Inp, "input: cannot post to the game window (%lu) - no keyboard/mouse input", GetLastError());
        return false;
    }
    return true;
}

bool KeyboardMouseAttached() noexcept {
    return g_registered.load();
}

bool MouseCaptured() noexcept {
    return g_captured.load();
}

KbmWindowStats GetKbmWindowStats() noexcept {
    std::lock_guard<std::mutex> lock(g_statsLock);
    return g_stats;
}

}  // namespace inp
