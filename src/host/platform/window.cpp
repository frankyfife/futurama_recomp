// Main window (window.h): frame + 4:3 view, hotkeys, borderless fullscreen toggle.
#include "platform/window.h"

#include "core/host_main.h"
#include "core/log.h"

#include <atomic>

extern "C" IMAGE_DOS_HEADER __ImageBase;  // the host DLL (window classes are registered for it)

namespace platform {
namespace {

constexpr wchar_t kFrameClass[] = L"FuturamaFrame";
constexpr wchar_t kViewClass[] = L"FuturamaView";
constexpr DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
constexpr DWORD kFullscreenStyle = WS_POPUP | WS_CLIPCHILDREN;

std::atomic<HWND> g_frame{nullptr};
std::atomic<HWND> g_view{nullptr};
std::atomic<std::uint32_t> g_hotkeys{0};
std::atomic<std::uint32_t> g_viewWidth{0};
std::atomic<std::uint32_t> g_viewHeight{0};
std::atomic<bool> g_shown{false};
std::atomic<bool> g_closeRequested{false};
// WM_CLOSE fallback: end the process from the UI thread if the guest main thread displays no frame within
// this time after the close request (loading hang, no renderer).
constexpr UINT_PTR kCloseTimer = 1;
constexpr UINT kCloseFallbackMs = 3000;
// UI thread only.
bool g_fullscreen = false;
RECT g_windowedRect{};

HINSTANCE ModuleInstance() noexcept {
    return reinterpret_cast<HINSTANCE>(&__ImageBase);
}

void LayoutView(HWND frame) noexcept {
    const HWND view = g_view.load();
    if (view == nullptr) {
        return;
    }
    RECT rc{};
    GetClientRect(frame, &rc);
    const ViewRect r = FitAspectRect(static_cast<std::uint32_t>(rc.right - rc.left),
                                     static_cast<std::uint32_t>(rc.bottom - rc.top), kViewWidth, kViewHeight);
    MoveWindow(view, r.x, r.y, static_cast<int>(r.width), static_cast<int>(r.height), TRUE);
    g_viewWidth.store(r.width);
    g_viewHeight.store(r.height);
}

RECT MonitorRect(HWND window, bool workArea) noexcept {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    const HMONITOR monitor = window != nullptr ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                               : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(monitor, &mi)) {
        return workArea ? mi.rcWork : mi.rcMonitor;
    }
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

void SetFullscreen(HWND frame, bool fullscreen) noexcept {
    if (fullscreen == g_fullscreen) {
        return;
    }
    const DWORD visible = IsWindowVisible(frame) ? WS_VISIBLE : 0u;
    if (fullscreen) {
        GetWindowRect(frame, &g_windowedRect);
        const RECT m = MonitorRect(frame, false);
        SetWindowLongPtrW(frame, GWL_STYLE, static_cast<LONG_PTR>(kFullscreenStyle | visible));
        SetWindowPos(frame, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(frame, GWL_STYLE, static_cast<LONG_PTR>(kWindowedStyle | visible));
        SetWindowPos(frame, nullptr, g_windowedRect.left, g_windowedRect.top, g_windowedRect.right - g_windowedRect.left,
                     g_windowedRect.bottom - g_windowedRect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    g_fullscreen = fullscreen;
    LayoutView(frame);
}

LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        LayoutView(hwnd);
        return 0;
    case WM_CLOSE:
        // Orderly end on the guest main thread (M3 review findings 5/10): the renderer ends the process at
        // its next DisplayFrame (CloseRequested), so the exit reporters run on the render thread instead of
        // racing with it. Fallback timer for a guest that displays no frame any more.
        if (!g_closeRequested.exchange(true)) {
            FUT_LOGI(Core, "window: close requested - the process ends after the next displayed frame");
            SetTimer(hwnd, kCloseTimer, kCloseFallbackMs, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (wp == kCloseTimer) {
            KillTimer(hwnd, kCloseTimer);
            // Never returns (exit reporters run here, on the UI thread).
            core::ExitHostProcess(core::kExitOk, "window closed (no frame displayed within 3 s of the close request)");
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F12 && (HIWORD(lp) & KF_REPEAT) == 0) {
            g_hotkeys.fetch_or(kHotkeyScreenshot);
            return 0;
        }
        break;
    case WM_KEYUP:
        if (wp == VK_SNAPSHOT) {  // Print Screen only reports key-up
            g_hotkeys.fetch_or(kHotkeyScreenshot);
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (HIWORD(lp) & KF_ALTDOWN) != 0) {
            if ((HIWORD(lp) & KF_REPEAT) == 0) {
                SetFullscreen(hwnd, !g_fullscreen);
            }
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) {
            return 0;  // no beep for Alt+Enter
        }
        break;
    case WM_DPICHANGED:
        if (!g_fullscreen) {
            const auto* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = 200;
        mm->ptMinTrackSize.y = 150;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // mouse input goes to the frame
    case WM_ERASEBKGND:
        return 1;  // DXGI owns the content
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterClasses() noexcept {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &FrameProc;
    wc.hInstance = ModuleInstance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kFrameClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    WNDCLASSEXW vc{};
    vc.cbSize = sizeof(vc);
    vc.lpfnWndProc = &ViewProc;
    vc.hInstance = ModuleInstance();
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = nullptr;
    vc.lpszClassName = kViewClass;
    return RegisterClassExW(&vc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

ViewRect FitAspectRect(std::uint32_t clientW, std::uint32_t clientH, std::uint32_t aspectW, std::uint32_t aspectH) noexcept {
    ViewRect r;
    if (clientW == 0 || clientH == 0 || aspectW == 0 || aspectH == 0) {
        return r;
    }
    if (static_cast<std::uint64_t>(clientW) * aspectH <= static_cast<std::uint64_t>(clientH) * aspectW) {
        r.width = clientW;  // width-limited: bars above and below
        r.height = static_cast<std::uint32_t>((static_cast<std::uint64_t>(clientW) * aspectH + aspectW / 2) / aspectW);
        if (r.height > clientH) {
            r.height = clientH;
        }
    } else {
        r.height = clientH;  // height-limited: bars left and right
        r.width = static_cast<std::uint32_t>((static_cast<std::uint64_t>(clientH) * aspectW + aspectH / 2) / aspectH);
        if (r.width > clientW) {
            r.width = clientW;
        }
    }
    r.x = static_cast<std::int32_t>((clientW - r.width) / 2);
    r.y = static_cast<std::int32_t>((clientH - r.height) / 2);
    return r;
}

bool CreateMainWindow(const WindowOptions& options, const char** error) noexcept {
    const char* dummy = nullptr;
    const char** err = error != nullptr ? error : &dummy;
    if (g_frame.load() != nullptr) {
        *err = "the main window already exists";
        return false;
    }
    if (!RegisterClasses()) {
        *err = "RegisterClassExW failed";
        return false;
    }
    // Per-monitor DPI awareness (V2) for the windows of this thread: physical pixels, no bitmap stretching.
    // Everything that measures or moves the windows below runs inside this context (the boot thread itself
    // may be DPI unaware; the window procedures always run in the windows' own context).
    const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const UINT dpi = GetDpiForSystem();
    const RECT work = MonitorRect(nullptr, true);
    const LONG workW = work.right - work.left;
    const LONG workH = work.bottom - work.top;
    // window_scale is relative to the desktop scaling: 640x480 * scale at 100 %, * dpi / 96 otherwise.
    const std::uint32_t scale = options.scale < 1 ? 1 : options.scale > 8 ? 8 : options.scale;
    LONG clientW = static_cast<LONG>(MulDiv(static_cast<int>(kViewWidth * scale), static_cast<int>(dpi), 96));
    LONG clientH = static_cast<LONG>(MulDiv(static_cast<int>(kViewHeight * scale), static_cast<int>(dpi), 96));
    RECT wr{0, 0, clientW, clientH};
    AdjustWindowRectExForDpi(&wr, kWindowedStyle, FALSE, 0, dpi);
    bool reduced = false;
    if (wr.right - wr.left > workW || wr.bottom - wr.top > workH) {
        // The largest 4:3 client area that fits the work area.
        const LONG borderW = (wr.right - wr.left) - clientW;
        const LONG borderH = (wr.bottom - wr.top) - clientH;
        const ViewRect fit = FitAspectRect(static_cast<std::uint32_t>(workW > borderW ? workW - borderW : 1),
                                           static_cast<std::uint32_t>(workH > borderH ? workH - borderH : 1),
                                           kViewWidth, kViewHeight);
        clientW = static_cast<LONG>(fit.width);
        clientH = static_cast<LONG>(fit.height);
        wr = RECT{0, 0, clientW, clientH};
        AdjustWindowRectExForDpi(&wr, kWindowedStyle, FALSE, 0, dpi);
        reduced = true;
    }
    const LONG w = wr.right - wr.left;
    const LONG h = wr.bottom - wr.top;
    g_windowedRect = RECT{work.left + (workW - w) / 2, work.top + (workH - h) / 2, 0, 0};
    if (g_windowedRect.left < work.left) {
        g_windowedRect.left = work.left;
    }
    if (g_windowedRect.top < work.top) {
        g_windowedRect.top = work.top;
    }
    g_windowedRect.right = g_windowedRect.left + w;
    g_windowedRect.bottom = g_windowedRect.top + h;
    g_fullscreen = false;
    const HWND frame = CreateWindowExW(0, kFrameClass, options.title != nullptr ? options.title : L"Futurama",
                                       kWindowedStyle, g_windowedRect.left, g_windowedRect.top, w, h, nullptr, nullptr,
                                       ModuleInstance(), nullptr);
    HWND view = nullptr;
    if (frame != nullptr) {
        view = CreateWindowExW(WS_EX_NOPARENTNOTIFY, kViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                               static_cast<int>(clientW), static_cast<int>(clientH), frame, nullptr, ModuleInstance(),
                               nullptr);
    }
    if (frame == nullptr || view == nullptr) {
        if (frame != nullptr) {
            DestroyWindow(frame);
        }
        if (previous != nullptr) {
            SetThreadDpiAwarenessContext(previous);
        }
        *err = "CreateWindowExW failed";
        return false;
    }
    g_view.store(view);
    g_frame.store(frame);
    g_shown.store(false);
    g_hotkeys.store(0);
    g_closeRequested.store(false);
    LayoutView(frame);
    if (options.fullscreen) {
        SetFullscreen(frame, true);
    }
    if (previous != nullptr) {
        SetThreadDpiAwarenessContext(previous);
    }
    FUT_LOGI(Core, "window: frame %p, view %p %ux%u (window_scale %u, DPI %u%s, %s)", static_cast<void*>(frame),
             static_cast<void*>(view), g_viewWidth.load(), g_viewHeight.load(), scale, dpi,
             reduced ? ", reduced to fit the work area" : "", options.fullscreen ? "borderless fullscreen" : "windowed");
    return true;
}

void DestroyMainWindow() noexcept {
    const HWND frame = g_frame.exchange(nullptr);
    g_view.store(nullptr);
    if (frame != nullptr) {
        DestroyWindow(frame);
    }
    g_viewWidth.store(0);
    g_viewHeight.store(0);
    g_shown.store(false);
    g_fullscreen = false;
}

HWND MainWindow() noexcept {
    return g_view.load();
}

HWND MainFrameWindow() noexcept {
    return g_frame.load();
}

void ShowMainWindow() noexcept {
    const HWND frame = g_frame.load();
    if (frame != nullptr && !g_shown.exchange(true)) {
        ShowWindowAsync(frame, SW_SHOWNORMAL);
    }
}

bool MainWindowShown() noexcept {
    return g_shown.load();
}

void MainWindowClientSize(std::uint32_t* width, std::uint32_t* height) noexcept {
    if (width != nullptr) {
        *width = g_view.load() != nullptr ? g_viewWidth.load() : 0;
    }
    if (height != nullptr) {
        *height = g_view.load() != nullptr ? g_viewHeight.load() : 0;
    }
}

bool CloseRequested() noexcept {
    return g_closeRequested.load();
}

std::uint32_t ConsumeHotkeys() noexcept {
    return g_hotkeys.exchange(0);
}

void PostHotkeys(std::uint32_t mask) noexcept {
    g_hotkeys.fetch_or(mask);
}

}  // namespace platform
