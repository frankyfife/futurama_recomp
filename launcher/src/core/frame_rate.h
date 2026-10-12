// M3d: what [render] frame_rate gives on a display - the launcher's preview of the game's plan selection
// (src/host/platform/pacer_policy.h ChooseFrameRate / SelectPlan, analysis/loader/m3d_frameclock.md 3.2). Pure; the
// game decides at run time (VRR is only known in the game), so the text names both cases where VRR matters.
//
//   "60" / numbers  the fixed rate (no note in the launcher)
//   "120"           one frame per refresh up to 120 Hz, every k-th refresh on k x 120 Hz; 144/165/180 Hz: 120 with
//                   VRR, without VRR 120 on average with frames of different length (1 or 2 refreshes)
//   "display"       like "120", but without VRR the highest even rate R / k (72 on 144 Hz, 82.5 on 165 Hz)
// In-between frames are interpolated whenever the frame rate is not the logic rate (60 Hz display: none).
#pragma once

#include <QString>

namespace fl {

struct FrameRateEstimate {
    bool known = false;           // a refresh-dependent choice ("120" / "display") on a display with a known refresh
    double fps = 0.0;             // frames per second the game shows (with VRR where needsVrr)
    bool interpolated = false;    // the frame rate is not the logic rate: in-between frames are computed
    bool needsVrr = false;        // fps assumes VRR (refresh not a multiple of 120); without VRR: fallbackFps
    double fallbackFps = 0.0;     // without VRR
    bool fallbackUneven = false;  // ... with frames of 1 or 2 refreshes ("120" on 144/165 Hz)
};

// `value` = the [render] frame_rate text, `refreshHz` = the selected monitor (0 = unknown), `tearing` = the GPU
// supports tearing presents (VRR possible), `tickRate` = [game] tick_rate (60 or 50).
FrameRateEstimate EstimateFrameRate(const QString& value, double refreshHz, bool tearing, int tickRate);
// "120", "82,5" / "59,94" (German) and "82.5" / "59.94" (English): at most two decimals, no trailing zeros.
QString FormatFps(double fps);
// Note below the frame rate on the graphics tab, e.g. "Auf diesem Bildschirm (120 Hz): 120 Bilder pro Sekunde mit
// Zwischenbildern." Empty when the estimate is unknown.
QString FrameRateNote(const FrameRateEstimate& estimate, double refreshHz);

}  // namespace fl
