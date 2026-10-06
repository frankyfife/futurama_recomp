// Refresh rate of the monitor a window is on (M3b display-coupled pacing; analysis/loader/pacing60.md 3).
// Owned by the pacing agent (M3b).
//
// Source order (pacing60.md 3.1, measured on the dev machine):
//   1. QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS): the path whose source GDI name (DISPLAYCONFIG_DEVICE_INFO_
//      GET_SOURCE_NAME) equals the monitor's MONITORINFOEXW::szDevice -> the target mode's
//      targetVideoSignalInfo.vSyncFreq, a rational (120/1, 60000/1001, 119880/1000) = the real signal rate
//      ("qdc signal"); without a target mode the path's targetInfo.refreshRate ("qdc path").
//   2. EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS).dmDisplayFrequency (integer: 59 for 59.94; "gdi").
// Not used: DwmGetCompositionTimingInfo (stale while DWM does not compose: qpcVBlank 6.3 s old on the dev
// machine), IDXGIOutput::FindClosestMatchingMode / GetDisplayModeList (available modes, not the current one).
// The friendly monitor name comes from DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME (log only).
//
// Cost: QueryDisplayConfig takes well under a millisecond; the pacer calls it only when the window's monitor
// or the display epoch (WM_DISPLAYCHANGE, WM_DPICHANGED, fullscreen toggle; platform/window.h) changes.
#pragma once

#include <cstdint>

namespace platform {

struct DisplayTiming {
    bool valid = false;              // a refresh rate was found
    std::uint32_t numerator = 0;     // refresh = numerator / denominator Hz
    std::uint32_t denominator = 1;
    double hz = 0.0;
    const char* source = "none";     // "qdc signal" | "qdc path" | "gdi" | "none" (static strings)
    char device[40] = {};            // GDI device name of the monitor, e.g. "\\.\DISPLAY1" (UTF-8)
    char name[72] = {};              // friendly monitor name, e.g. "LG TV SSCR2" (UTF-8, may be empty)
};

// Refresh of `monitor` (an HMONITOR); nullptr -> the primary monitor. Never throws; valid = false if nothing
// could be determined.
DisplayTiming QueryDisplayTiming(void* monitor) noexcept;

}  // namespace platform
