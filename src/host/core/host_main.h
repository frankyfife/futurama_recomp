// HostMain: entry of futurama_host.dll (exported via futurama_host.def, called by the launcher).
// Owned by the core agent. Stage order follows design 1.10 (see host_main.cpp and src/INTERFACES.md).
#pragma once

#include "shared/launcher_info.h"

namespace core {

// Process exit codes of HostMain (the launcher passes them to ExitProcess). Launcher codes: 10..14.
inline constexpr int kExitOk = 0;            // normal end, also --stop-at reached
inline constexpr int kExitStartupError = 1;  // bad launcher info, config error
inline constexpr int kExitStageFailed = 2;   // a HostMain stage returned false
// 3 = core::kExitFatal (core::Fatal / KeBugCheck-style abort, core/log.h)
inline constexpr int kExitGuestShutdown = 4;  // the guest ended itself: XLaunchNewImageA (dashboard),
                                              // HalReturnToFirmware (design 1.10 "Ende")
inline constexpr int kExitWatchdog = 5;       // [debug] timeout elapsed (hang; thread dump in the log)

int RunHost(const LauncherInfo* info) noexcept;

// Ends the process from any thread (guest hooks, kernel HLE such as HalReturnToFirmware, the
// --stop-at hook): logs "process exit: <reason> (code N)", flushes the log and terminates the
// process with `exitCode`. Uses TerminateProcess, like core::Fatal: no DLL_PROCESS_DETACH / static
// destructors run while guest threads are frozen at arbitrary points. Never returns. A second call while
// an exit is in progress waits for it - except with kExitWatchdog, which terminates at once (the first
// exit hangs in an exit reporter or the log flush; M2 review finding 7).
[[noreturn]] void ExitHostProcess(int exitCode, const char* reason) noexcept;
// True once ExitHostProcess has started (the watchdog then skips its log-based thread dump).
bool ExitInProgress() noexcept;

// Exit reporters (M2 integration): module statistics that belong into the log at every orderly
// process end through ExitHostProcess (--frames limit, --stop-at, guest shutdown), e.g. the fiber
// switch and APC delivery counters of M2 exit criterion 5. They run on the exiting thread before the
// "process exit" line, once, sorted by name (deterministic across builds and static initialisation
// orders). Not after a watchdog timeout (kExitWatchdog: a hung guest thread may hold a lock that a
// reporter takes). A reporter must not throw, must not block on guest threads and must not call
// ExitHostProcess. At most kMaxExitReporters; registering the same name twice keeps the first.
// core::Fatal does not run them (it has the crash reporters of core/crash.h).
using ExitReporter = void (*)() noexcept;
inline constexpr int kMaxExitReporters = 16;
void AddExitReporter(const char* name, ExitReporter reporter) noexcept;
// Runs every registered exit reporter (sorted by name). Called by ExitHostProcess; exposed for tests.
void RunExitReporters() noexcept;
// Number of registered exit reporters (tests).
int ExitReporterCount() noexcept;

}  // namespace core

extern "C" int __cdecl HostMain(const LauncherInfo* info);
