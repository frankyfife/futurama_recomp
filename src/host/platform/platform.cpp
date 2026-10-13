// Window + message loop (design 1.1, 1.10 steps 4 and 6). Owned by the core agent until M2, by the M3
// backend agent from M3 on (GFX_CONTRACT.md 10).
// The boot thread waits here after entry returned until the guest ends the process (design 1.7: "Die
// Rueckkehr von entry ... geht danach in die Message-Loop"); from M3 on it also owns the main window
// (platform/window.h, created in stage 12 when [render] backend = d3d11) and pumps its messages.
#include "platform/platform.h"

#include "core/crash.h"
#include "core/host_main.h"
#include "core/log.h"
#include "gfx/gfx_config.h"
#include "gfx/widescreen.h"
#include "kernel/kernel_hle.h"
#include "platform/window.h"

#include <atomic>
#include <cstdint>

namespace platform {
namespace {

HANDLE g_quitEvent = nullptr;
std::atomic<int> g_exitCode{core::kExitOk};
std::atomic<bool> g_quitRequested{false};
std::uint32_t g_timeoutSeconds = 0;

}  // namespace

bool StageCreateWindow(const core::Config& config) {
    if (g_quitEvent == nullptr) {
        g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (g_quitEvent == nullptr) {
        FUT_LOGE(Core, "CreateWindow: CreateEvent failed (error %lu)", GetLastError());
        return false;
    }
    g_timeoutSeconds = config.GetU32("debug", "timeout", 0);
    const gfx::Settings& settings = gfx::GetSettings();
    if (settings.backend != gfx::RenderBackend::D3D11) {
        FUT_LOGD(Core, "CreateWindow: [render] backend = null - no window; watchdog %u s", g_timeoutSeconds);
        return true;
    }
    // M3: the window the D3D11 swap chain presents into (hidden until the first presented frame). A failure
    // is logged and the game continues without a window (the renderer then falls back to the null renderer).
    WindowOptions options;
    options.scale = settings.windowScale;
    options.fullscreen = settings.fullscreen;
    options.monitor = settings.monitor.empty() ? nullptr : settings.monitor.c_str();
    options.aspectW = gfx::widescreen::GetDisplay().aspectW;  // G3: [render] aspect (0 x 0 = 4:3)
    options.aspectH = gfx::widescreen::GetDisplay().aspectH;
    const char* error = nullptr;
    if (!CreateMainWindow(options, &error)) {
        FUT_LOGE(Core, "CreateWindow: %s (error %lu) - continuing without a window", error != nullptr ? error : "?",
                 GetLastError());
    }
    FUT_LOGD(Core, "CreateWindow: watchdog %u s", g_timeoutSeconds);
    return true;
}

void RequestQuit(int exitCode) noexcept {
    bool expected = false;
    if (g_quitRequested.compare_exchange_strong(expected, true)) {
        g_exitCode = exitCode;
        if (g_quitEvent != nullptr) {
            SetEvent(g_quitEvent);
        }
    }
}

int StageMessageLoop() {
    if (g_quitEvent == nullptr) {
        FUT_LOGE(Core, "MessageLoop: CreateWindow stage did not run");
        return core::kExitStageFailed;
    }
    // Objects: [0] quit request, [1] (optional) "last guest thread terminated" from the kernel HLE.
    HANDLE objects[2] = {g_quitEvent, krnl::GuestThreadsExitedEvent()};
    const DWORD count = objects[1] != nullptr ? 2 : 1;
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG timeoutMs = static_cast<ULONGLONG>(g_timeoutSeconds) * 1000;
    for (;;) {
        DWORD wait = INFINITE;
        if (timeoutMs != 0) {
            const ULONGLONG elapsed = GetTickCount64() - start;
            wait = elapsed >= timeoutMs ? 0 : static_cast<DWORD>(timeoutMs - elapsed);
        }
        const DWORD r = MsgWaitForMultipleObjectsEx(count, objects, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) {
            const int rc = g_exitCode.load();
            FUT_LOGI(Core, "message loop: quit requested (exit code %d)", rc);
            return rc;
        }
        if (count == 2 && r == WAIT_OBJECT_0 + 1) {
            // On the Xbox the main thread never ends (main -> XapiBootToDash -> reboot); if every guest
            // thread terminated anyway, nothing can end the process any more.
            FUT_LOGW(Core, "message loop: all guest threads have terminated - ending the process");
            return core::kExitGuestShutdown;
        }
        if (r == WAIT_OBJECT_0 + count) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    FUT_LOGI(Core, "message loop: WM_QUIT (exit code %d)", static_cast<int>(msg.wParam));
                    return static_cast<int>(msg.wParam);
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        if (r == WAIT_TIMEOUT) {
            if (core::ExitInProgress()) {
                // The orderly exit itself hangs (M2 review finding 7): its thread may hold the log lock, so
                // no thread dump / minidump here; ExitHostProcess terminates with exit code 5.
                core::ExitHostProcess(core::kExitWatchdog, "watchdog timeout during the process exit");
            }
            FUT_LOGE(Core, "watchdog: the process did not end within [debug] timeout = %u s",
                     g_timeoutSeconds);
            core::LogAllThreads("watchdog");
            core::WriteMiniDump(nullptr, "watchdog");
            core::ExitHostProcess(core::kExitWatchdog, "watchdog timeout");
        }
        FUT_LOGE(Core, "message loop: MsgWaitForMultipleObjectsEx failed (error %lu)", GetLastError());
        return core::kExitStageFailed;
    }
}

}  // namespace platform
