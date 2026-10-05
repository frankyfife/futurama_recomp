// Window + message loop (design 1.1, 1.10 steps 4 and 6). Owned by the core agent.
// M1: no window yet; the boot thread waits here after entry returned until the guest ends the
// process (design 1.7: "Die Rueckkehr von entry ... geht danach in die Message-Loop").
#include "platform/platform.h"

#include "core/crash.h"
#include "core/host_main.h"
#include "core/log.h"
#include "kernel/kernel_hle.h"

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
    FUT_LOGD(Core, "CreateWindow: no window before M3; watchdog %u s", g_timeoutSeconds);
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
