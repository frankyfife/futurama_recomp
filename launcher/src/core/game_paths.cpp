#include "core/game_paths.h"

#include "core/validation.h"

#include <QDir>
#include <QFileInfo>

namespace fl {

QString GamePaths::launcherIni() const {
    return configDir.isEmpty()
               ? QString()
               : QDir::toNativeSeparators(QDir(configDir).filePath(QLatin1String(kLauncherIniName)));
}

QString GamePaths::cacheDir() const {
    return configDir.isEmpty()
               ? QString()
               : QDir::toNativeSeparators(QDir(configDir).filePath(QLatin1String(kCacheDirName)));
}

QString NativeAbsolute(const QString& path) {
    if (path.isEmpty()) {
        return QString();
    }
    return QDir::toNativeSeparators(
        QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(path)).absoluteFilePath()));
}

QString LocalAppDataBase() {
    const QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) {
        return QString();
    }
    return QDir::toNativeSeparators(
        QDir::cleanPath(QDir::fromNativeSeparators(base) + QStringLiteral("/FuturamaPC")));
}

bool HasGameExe(const QString& dir) {
    return !dir.isEmpty() &&
           QFileInfo(QDir(QDir::fromNativeSeparators(dir)).filePath(QLatin1String(kGameExeName))).isFile();
}

bool IsPortableGameDir(const QString& gameDir) {
    return !gameDir.isEmpty() &&
           QFileInfo::exists(
               QDir(QDir::fromNativeSeparators(gameDir)).filePath(QLatin1String(kPortableMarker)));
}

QString FindGameDir(const QString& explicitDir, const QString& launcherDir, const QString& configuredDir) {
    for (const QString& dir : {explicitDir, launcherDir, configuredDir}) {
        if (HasGameExe(dir)) {
            return NativeAbsolute(dir);
        }
    }
    return QString();
}

GamePaths ResolveGamePaths(const QString& gameDir, const QString& explicitIni) {
    GamePaths p;
    if (HasGameExe(gameDir)) {
        p.gameDir = NativeAbsolute(gameDir);
        const QDir dir(QDir::fromNativeSeparators(p.gameDir));
        p.gameExe = QDir::toNativeSeparators(dir.filePath(QLatin1String(kGameExeName)));
        p.hostDll = QDir::toNativeSeparators(dir.filePath(QLatin1String(kHostDllName)));
        p.portable = IsPortableGameDir(p.gameDir);
    }
    const QString base = p.portable ? p.gameDir : LocalAppDataBase();
    p.defaultUserdata = base;
    if (!explicitIni.isEmpty()) {
        p.iniPath = NativeAbsolute(explicitIni);
    } else if (!base.isEmpty()) {
        p.iniPath = QDir::toNativeSeparators(
            QDir(QDir::fromNativeSeparators(base)).filePath(QLatin1String(kIniName)));
    }
    p.configDir =
        p.iniPath.isEmpty() ? QString() : QDir::toNativeSeparators(QFileInfo(p.iniPath).absolutePath());
    return p;
}

QString UserdataDir(const QString& iniUserdataValue, const GamePaths& paths) {
    if (!iniUserdataValue.trimmed().isEmpty()) {
        return ResolvePath(iniUserdataValue, paths.configDir);
    }
    return paths.defaultUserdata;
}

QString LogDir(const QString& userdata) {
    return userdata.isEmpty()
               ? QString()
               : QDir::toNativeSeparators(
                     QDir(QDir::fromNativeSeparators(userdata)).filePath(QStringLiteral("logs")));
}

QString LogFile(const QString& userdata) {
    return userdata.isEmpty() ? QString()
                              : QDir::toNativeSeparators(QDir(QDir::fromNativeSeparators(LogDir(userdata)))
                                                             .filePath(QStringLiteral("futurama.log")));
}

QString ScreenshotDir(const QString& iniValue, const QString& userdata, const QString& iniDir) {
    if (!iniValue.trimmed().isEmpty()) {
        return ResolvePath(iniValue, iniDir);
    }
    return userdata.isEmpty()
               ? QString()
               : QDir::toNativeSeparators(
                     QDir(QDir::fromNativeSeparators(userdata)).filePath(QStringLiteral("screenshots")));
}

}  // namespace fl
