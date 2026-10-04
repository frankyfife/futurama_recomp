// Window and message loop (design 1.1 UI/boot thread, 1.10 steps 4 and 6). Owned by the core agent.
#pragma once

#include "core/config.h"

namespace platform {

// HostMain stage "CreateWindow": main window, invisible until the first Present (needed from M3 on;
// in M1 it only prepares the message loop: quit event, [debug] timeout watchdog).
bool StageCreateWindow(const core::Config& config);

// HostMain final stage: message loop on the UI/boot thread until the process ends. Returns the
// process exit code if the loop ends (RequestQuit or WM_QUIT). The guest normally ends the process
// itself (core::ExitHostProcess from the --stop-at hook, XLaunchNewImageA or HalReturnToFirmware).
// The loop also returns core::kExitGuestShutdown when krnl::GuestThreadsExitedEvent() is signaled
// (every thread of PsCreateSystemThreadEx has terminated).
// With [debug] timeout = N (seconds, 0 = off) the loop logs a thread dump, writes a minidump and
// exits with core::kExitWatchdog after N seconds (hang protection for test runs).
int StageMessageLoop();

// Makes StageMessageLoop return `exitCode` (thread-safe; the first request wins). For host threads
// that want an orderly end of HostMain instead of core::ExitHostProcess.
void RequestQuit(int exitCode) noexcept;

}  // namespace platform
