// XAPI replacement functions (design E5, 2.1, Anhang B). Owned by the core agent.
//
// M1 hooks (registered with FUT_HOOK_EX in xapi_hooks.cpp, installed by core::StageInstallHooks):
//   0xF641B XAPILIB::QueryPerformanceCounter   -> Win32 QueryPerformanceCounter (stdcall 1, TRUE)
//   0xF642C XAPILIB::QueryPerformanceFrequency -> Win32 QueryPerformanceFrequency (stdcall 1, TRUE)
//   0xF3DE0 XAPILIB::OutputDebugStringA        -> log channel XAPI (stdcall 1)
//   0xF7F7C XAPILIB::XMountUtilityDrive        -> \??\Z: on <userdata>\cache\Z, TRUE (stdcall 1)
//   0xF82E3 XAPILIB::XLaunchNewImageA          -> log + process exit (stdcall 2)
// M2: 0x17094F XAPILIB::SwitchToFiber (MASM, src/host/asm).
// The addresses are the guest::kXapi* constants in core/guest.h.
#pragma once

#include "shared/win32.h"

#include <cstdint>

namespace xapi {

// Device object behind Z: (files.md 1.1: "\Device\Harddisk0\Partition<3 + cache index>").
// ASSUMPTION: the first cache partition (index 0) -> Partition3; the game never uses Z: (files.md 1.1).
inline constexpr char kUtilityDriveDevice[] = "\\Device\\Harddisk0\\Partition3";
inline constexpr char kUtilityDriveLink[] = "\\??\\Z:";

BOOL __stdcall QueryPerformanceCounterHle(LARGE_INTEGER* counter) noexcept;
BOOL __stdcall QueryPerformanceFrequencyHle(LARGE_INTEGER* frequency) noexcept;
void __stdcall OutputDebugStringAHle(const char* text) noexcept;
BOOL __stdcall XMountUtilityDriveHle(BOOL formatClean) noexcept;
DWORD __stdcall XLaunchNewImageAHle(const char* titlePath, const std::uint32_t* launchData) noexcept;

}  // namespace xapi
