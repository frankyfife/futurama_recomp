#include "ui/screen_info.h"

#include "core/l10n.h"

#include <QGuiApplication>
#include <QLocale>
#include <QScreen>

#include <algorithm>
#include <cmath>

namespace fl::ui {

QString SizeText(QSize size) {
    return QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
}

QSize FitAspect43(QSize size) {
    const qint64 w = size.width();
    const qint64 h = size.height();
    if (w <= 0 || h <= 0) {
        return QSize();
    }
    if (w * 3 <= h * 4) {
        return QSize(static_cast<int>(w), static_cast<int>(std::min<qint64>(h, (w * 3 + 2) / 4)));
    }
    return QSize(static_cast<int>(std::min<qint64>(w, (h * 4 + 1) / 3)), static_cast<int>(h));
}

WindowSizeEstimate EstimateWindowSize(int scale, const GpuOutput* output) {
    WindowSizeEstimate e;
    e.dpi = output != nullptr && output->dpi > 0 ? output->dpi : 96;
    const int s = std::clamp(scale, 1, 8);
    e.client = QSize(640 * s * e.dpi / 96, 480 * s * e.dpi / 96);
    e.fitted = e.client;
    const QSize screen = output != nullptr ? output->nativeSize() : QSize();
    if (screen.isValid()) {
        // Work area minus the window frame: taskbar ~48, title bar ~31 + borders ~8 logical pixels.
        const QSize room(screen.width() - 16 * e.dpi / 96, screen.height() - 87 * e.dpi / 96);
        if (e.client.width() > room.width() || e.client.height() > room.height()) {
            e.shrunk = true;
            e.fitted = FitAspect43(room);
        }
    }
    return e;
}

QString ScreenSignature() {
    QStringList parts;
    for (const QScreen* s : QGuiApplication::screens()) {
        const QSize px = (QSizeF(s->geometry().size()) * s->devicePixelRatio()).toSize();
        parts.append(QStringLiteral("%1:%2x%3@%4%5")
                         .arg(s->name())
                         .arg(px.width())
                         .arg(px.height())
                         .arg(qRound(s->refreshRate() * 100))
                         .arg(s == QGuiApplication::primaryScreen() ? QStringLiteral("*") : QString()));
    }
    return parts.join(QLatin1Char(';'));
}

GpuInfo GpuInfoFromScreens() {
    GpuInfo info;
    for (const QScreen* s : QGuiApplication::screens()) {
        GpuOutput o;
        o.name = s->model().isEmpty() ? s->name() : s->model();
        o.device = s->name();
        o.primary = s == QGuiApplication::primaryScreen();
        const QSize px = (QSizeF(s->geometry().size()) * s->devicePixelRatio()).toSize();
        o.rect = QRect(QPoint(qRound(s->geometry().x() * s->devicePixelRatio()),
                              qRound(s->geometry().y() * s->devicePixelRatio())),
                       px);
        o.dpi = qRound(96.0 * s->devicePixelRatio());
        o.refreshNum = qRound64(s->refreshRate() * 1000.0);
        o.refreshDen = 1000;
        o.modes.append(px);
        for (int k = 1; k <= 8; ++k) {
            const QSize classic(640 * k, 480 * k);
            if (classic.width() <= px.width() && classic.height() <= px.height() &&
                !o.modes.contains(classic)) {
                o.modes.append(classic);
            }
        }
        info.outputs.append(o);
    }
    info.valid = !info.outputs.isEmpty();
    return info;
}

QString OutputDisplayText(const GpuOutput& output, bool markPrimary) {
    QString text = output.name.isEmpty() ? output.device : output.name;
    const QSize size = output.nativeSize();
    if (size.isValid()) {
        text += QStringLiteral(" – ") + SizeText(size);
    }
    const double hz = output.refreshHz();
    if (hz > 0.0) {
        const bool whole = std::fabs(hz - std::round(hz)) < 0.005;
        text += QStringLiteral(", ") + QLocale().toString(hz, 'f', whole ? 0 : 2) + QStringLiteral(" Hz");
    }
    if (output.primary && markPrimary) {
        text += QStringLiteral(" (") + T("monitor.primary") + QLatin1Char(')');
    }
    return text;
}

}  // namespace fl::ui
