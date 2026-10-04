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
// destructors run while guest threads are frozen at arbitrary points. Never returns.
[[noreturn]] void ExitHostProcess(int exitCode, const char* reason) noexcept;

}  // namespace core

extern "C" int __cdecl HostMain(const LauncherInfo* info);
