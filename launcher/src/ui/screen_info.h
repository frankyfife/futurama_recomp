// Display information from Qt (QScreen): the fallback when `futurama.exe --query-gpu` is not available,
// the signature that invalidates the cached GPU query, and the texts of the monitor pickers.
#pragma once

#include "core/gpu_info.h"

#include <QString>

namespace fl::ui {

// "name:WxH@hz;..." of the connected screens (physical pixels).
QString ScreenSignature();
// Outputs from QScreen (physical pixel sizes, refresh rate, primary flag; modes = classic 4:3 sizes
// up to the native size + the native size). msaa/anisotropy stay unknown.
GpuInfo GpuInfoFromScreens();
// "LG TV SSCR2 - 3840 × 2160, 120 Hz (Hauptbildschirm)"; markPrimary false leaves out the suffix.
QString OutputDisplayText(const GpuOutput& output, bool markPrimary = true);
// "3840 × 2160"
QString SizeText(QSize size);
// The largest 4:3 size inside `size`, rounded like the game (platform::FitAspectRect): 3840x2160 -> 2880x2160.
QSize FitAspect43(QSize size);
// Size of the game window's client area at [render] window_scale `scale` on `output` (640x480 * scale * DPI / 96),
// and whether the game shrinks it to the monitor's work area (estimated with the usual taskbar and title bar).
struct WindowSizeEstimate {
    QSize client;         // before shrinking
    QSize fitted;         // estimated size after shrinking (4:3), = client if it fits
    bool shrunk = false;  // larger than the work area
    int dpi = 96;
};
WindowSizeEstimate EstimateWindowSize(int scale, const GpuOutput* output);

}  // namespace fl::ui
