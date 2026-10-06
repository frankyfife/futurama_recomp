// Helpers shared by the launcher unit tests.
#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QUuid>

namespace fl::test {

// Fresh directory below <build>\bin\test_tmp\ (git-ignored build tree, never %TEMP% or %LOCALAPPDATA%).
inline QString MakeTestDir(const QString& name) {
    const QString base = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("test_tmp"));
    const QString dir =
        QDir(base).filePath(name + QLatin1Char('-') + QUuid::createUuid().toString(QUuid::Id128).left(8));
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    return QDir::toNativeSeparators(dir);
}

inline bool WriteFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    return f.write(data) == data.size();
}

inline QByteArray ReadFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

inline QString TestdataPath(const char* name) {
    return QDir(QStringLiteral(LAUNCHER_TESTDATA_DIR)).filePath(QLatin1String(name));
}

// The user's ISO name pattern: blanks, parentheses and commas.
inline QString SpecialIsoName() {
    return QStringLiteral("Futurama (Europe) (En,Fr,De,Es,It).iso");
}

}  // namespace fl::test
