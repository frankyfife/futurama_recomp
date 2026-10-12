#include "core/frame_rate.h"

#include "core/l10n.h"

#include <QLocale>

#include <cmath>

namespace fl {
namespace {

// Same constants as src/host/platform/pacer_policy.h.
constexpr double kMaxFrameRate = 120.0;
constexpr double kIntegerTolerance = 0.002;   // R = k x 120 within 0.2 %
constexpr double kFrameLockTolerance = 1e-4;  // the frame rate equals the logic rate within 100 ppm
constexpr int kMaxSyncInterval = 4;

// Frames at or below the logic rate (+ 0.2 %) run one tick per frame without in-between frames: 50, 59.94, 60 Hz
// (pacer_policy.cpp FramesPerTick, M3d review finding 8).
bool IsLogicRate(double fps, int tickRate) {
    return tickRate > 0 && (fps <= tickRate * (1.0 + kIntegerTolerance) ||
                            std::fabs(fps - tickRate) <= kFrameLockTolerance * fps);
}

}  // namespace

FrameRateEstimate EstimateFrameRate(const QString& value, double refreshHz, bool tearing, int tickRate) {
    FrameRateEstimate e;
    const QString v = value.trimmed().toLower();
    const bool display = v == QLatin1String("display");
    if ((!display && v != QLatin1String("120")) || !(refreshHz > 0.0)) {
        return e;
    }
    e.known = true;
    if (refreshHz <= kMaxFrameRate * (1.0 + kIntegerTolerance)) {
        e.fps = refreshHz;  // one frame per refresh (60 Hz display: the logic rate, nothing to interpolate)
    } else {
        const double k = std::round(refreshHz / kMaxFrameRate);
        if (k >= 1.0 && k <= kMaxSyncInterval &&
            std::fabs(refreshHz / k - kMaxFrameRate) <= kIntegerTolerance * kMaxFrameRate) {
            e.fps = refreshHz / k;  // 240 Hz: every second refresh
        } else {
            // 144/165/180 Hz: 120 with VRR (the game's tearing timer), else the cadence or an even divisor.
            double fallback = kMaxFrameRate;
            bool uneven = true;
            const double kk = std::ceil(refreshHz / (kMaxFrameRate * (1.0 + kIntegerTolerance)));
            if (display && kk >= 2.0 && kk <= kMaxSyncInterval) {
                fallback = refreshHz / kk;
                uneven = false;
            } else if (refreshHz / kMaxFrameRate > kMaxSyncInterval) {
                uneven = false;  // above 4 refreshes per frame: the timer at 120
            }
            e.needsVrr = tearing;
            e.fps = tearing ? kMaxFrameRate : fallback;
            e.fallbackFps = fallback;
            e.fallbackUneven = uneven;
        }
    }
    e.interpolated = !IsLogicRate(e.fps, tickRate);
    return e;
}

QString FormatFps(double fps) {
    const QLocale locale(CurrentLanguage() == Language::German ? QLocale::German : QLocale::English);
    QString text = locale.toString(std::round(fps * 100.0) / 100.0, 'f', 2);
    const QString point = locale.decimalPoint();
    if (text.contains(point)) {
        while (text.endsWith(QLatin1Char('0'))) {
            text.chop(1);
        }
        if (text.endsWith(point)) {
            text.chop(point.size());
        }
    }
    return text;
}

QString FrameRateNote(const FrameRateEstimate& e, double refreshHz) {
    if (!e.known) {
        return QString();
    }
    const auto rate = [](double fps, bool interpolated) {
        return (interpolated ? T("fps.interpolated") : T("fps.plain")).arg(FormatFps(fps));
    };
    QString what = rate(e.fps, e.interpolated);
    if (e.needsVrr) {
        const QString without = e.fallbackUneven ? T("fps.uneven").arg(FormatFps(e.fallbackFps))
                                                 : T("fps.plain").arg(FormatFps(e.fallbackFps));
        what = T("fps.vrr").arg(what, without);
    } else if (e.fallbackUneven) {
        what = T("fps.uneven").arg(FormatFps(e.fps));
    }
    return T("fps.note").arg(FormatFps(refreshHz), what);
}

}  // namespace fl
