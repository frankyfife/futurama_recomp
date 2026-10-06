#include "ui/app_style.h"

#include <QApplication>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QWidget>

namespace fl::ui {
namespace {

bool g_forcedDark = false;

// Review M3c finding 23: Fusion draws a check box as a Base-filled square with an outline of Window.darker(140).
// With Base darker than Window (25 on 32) an unchecked box was nearly invisible; input fields and check boxes are
// now lighter than the window (like the Windows 11 dark controls) and the 3D roles have a visible spread.
QPalette DarkPalette() {
    QPalette p;
    const QColor window(32, 32, 32);
    const QColor base(48, 48, 48);
    const QColor text(240, 240, 240);
    const QColor disabled(128, 128, 128);
    const QColor accent(76, 160, 224);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(40, 40, 40));
    p.setColor(QPalette::ToolTipBase, QColor(45, 45, 45));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(150, 150, 150));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(52, 52, 52));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::Accent, accent);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Light, QColor(96, 96, 96));
    p.setColor(QPalette::Midlight, QColor(72, 72, 72));
    p.setColor(QPalette::Mid, QColor(60, 60, 60));
    p.setColor(QPalette::Dark, QColor(16, 16, 16));
    p.setColor(QPalette::Shadow, Qt::black);
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, disabled);
    }
    return p;
}

}  // namespace

bool IsDarkMode() {
    if (g_forcedDark) {
        return true;
    }
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark) {
        return true;
    }
    // Fallback: a dark window background.
    return qApp != nullptr && QGuiApplication::palette().color(QPalette::Window).lightness() < 100;
}

void ApplyAppStyle(QApplication& app, const QString& requestedScheme) {
    const bool wantDark = requestedScheme.compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0;
    const bool wantLight = requestedScheme.compare(QLatin1String("light"), Qt::CaseInsensitive) == 0;
    if (wantDark || wantLight) {
        QGuiApplication::styleHints()->setColorScheme(wantDark ? Qt::ColorScheme::Dark
                                                               : Qt::ColorScheme::Light);
    }
    const QStringList keys = QStyleFactory::keys();
    auto has = [&](const char* name) { return keys.contains(QLatin1String(name), Qt::CaseInsensitive); };
    const bool platformDark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    // Review M3c finding 23: the Windows 11 style only on Windows 11 and on a real window system (offscreen it has no
    // native theme: frameless buttons); Windows 10 gets "windowsvista" (light) or Fusion (dark) - LIESMICH names both.
    const bool offscreen = QGuiApplication::platformName() == QLatin1String("offscreen");
    const bool windows11 = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11;
    QString style;
    if (!offscreen && windows11 && has("windows11") && !(wantDark && !platformDark)) {
        style = QStringLiteral("windows11");
    } else if (!offscreen && has("windowsvista") && !platformDark && !wantDark) {
        style = QStringLiteral("windowsvista");
    } else {
        style = QStringLiteral("Fusion");
    }
    app.setStyle(QStyleFactory::create(style));
    if (style == QLatin1String("Fusion") && (wantDark || platformDark)) {
        // The platform does not provide a dark scheme (e.g. -platform offscreen): explicit palette.
        g_forcedDark = true;
        app.setPalette(DarkPalette());
    }
}

QColor SeverityColor(Severity severity) {
    const bool dark = IsDarkMode();
    switch (severity) {
    case Severity::Error:
        return dark ? QColor(255, 153, 164) : QColor(196, 43, 28);
    case Severity::Warning:
        return dark ? QColor(252, 225, 0) : QColor(157, 93, 0);
    case Severity::Ok:
        return dark ? QColor(108, 203, 95) : QColor(16, 124, 16);
    }
    return QColor();
}

QColor ToneColor(ExitTone tone) {
    switch (tone) {
    case ExitTone::Good:
        return SeverityColor(Severity::Ok);
    case ExitTone::Neutral:
        return IsDarkMode() ? QColor(96, 205, 255) : QColor(0, 95, 184);
    case ExitTone::Bad:
        return SeverityColor(Severity::Error);
    }
    return QColor();
}

QColor MutedTextColor(const QWidget* widget) {
    const QPalette p = widget != nullptr ? widget->palette() : QGuiApplication::palette();
    QColor c = p.color(QPalette::PlaceholderText);
    if (!c.isValid() || c == p.color(QPalette::WindowText)) {
        c = p.color(QPalette::Disabled, QPalette::WindowText);
    }
    return c;
}

QString ColorCss(const QColor& color) {
    return color.name(QColor::HexRgb);
}

QIcon AppIcon() {
    static const QIcon icon = [] {
        QIcon i;
        for (int size : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
            i.addFile(QStringLiteral(":/icons/icon_%1.png").arg(size), QSize(size, size));
        }
        return i;
    }();
    return icon;
}

QPixmap AppIconPixmap(int logicalSize, qreal devicePixelRatio) {
    return AppIcon().pixmap(QSize(logicalSize, logicalSize), devicePixelRatio);
}

void ScaleFont(QWidget* widget, qreal factor, bool bold) {
    QFont f = widget->font();
    if (f.pointSizeF() > 0) {
        f.setPointSizeF(f.pointSizeF() * factor);
    } else {
        f.setPixelSize(qRound(f.pixelSize() * factor));
    }
    f.setBold(bold);
    widget->setFont(f);
}

}  // namespace fl::ui
