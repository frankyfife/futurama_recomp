#include "core/cli_cache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace fl {
namespace {

bool WriteAtomically(const QString& path, const QByteArray& data) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

QByteArray ReadAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QJsonObject ReadObject(const QString& path) {
    return QJsonDocument::fromJson(ReadAll(path)).object();
}

}  // namespace

CliCache::CliCache(QString dir) : dir_(std::move(dir)) {}

QString CliCache::path(const QString& name) const {
    return QDir(dir_).filePath(name);
}

QString CliCache::Fingerprint(const QString& gameExe, const QString& hostDll) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (const QString& file : {gameExe, hostDll}) {
        const QFileInfo info(file);
        hash.addData(QDir::cleanPath(info.absoluteFilePath()).toLower().toUtf8());
        hash.addData(QByteArray::number(info.exists() ? info.size() : -1));
        hash.addData(QByteArray::number(info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0));
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString CliCache::SafeBuildName(const QString& build) {
    QString out;
    for (QChar c : build) {
        const bool safe = (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
                          (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                          (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('.') ||
                          c == QLatin1Char('_') || c == QLatin1Char('-');
        out += safe ? c : QLatin1Char('_');
    }
    if (out.isEmpty() || out.size() > 64) {
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(build.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
        out = out.left(48) + QLatin1Char('-') + hash;
    }
    return out;
}

std::optional<QString> CliCache::buildFor(const QString& fingerprint) const {
    const QJsonObject index = ReadObject(path(QStringLiteral("index.json")));
    const QJsonValue v = index.value(QLatin1String("fingerprints")).toObject().value(fingerprint);
    if (!v.isString()) {
        return std::nullopt;
    }
    return v.toString();
}

std::optional<QByteArray> CliCache::loadSchema(const QString& fingerprint) const {
    const auto build = buildFor(fingerprint);
    if (!build) {
        return std::nullopt;
    }
    const QByteArray json =
        ReadAll(path(QStringLiteral("schema-") + SafeBuildName(*build) + QStringLiteral(".json")));
    if (json.isEmpty()) {
        return std::nullopt;
    }
    return json;
}

bool CliCache::storeSchema(const QString& fingerprint, const QString& build, const QByteArray& json) const {
    if (dir_.isEmpty() || !QDir().mkpath(dir_)) {
        return false;
    }
    if (!WriteAtomically(path(QStringLiteral("schema-") + SafeBuildName(build) + QStringLiteral(".json")),
                         json)) {
        return false;
    }
    QJsonObject index = ReadObject(path(QStringLiteral("index.json")));
    QJsonObject fingerprints = index.value(QLatin1String("fingerprints")).toObject();
    fingerprints.insert(fingerprint, build);
    index.insert(QStringLiteral("fingerprints"), fingerprints);
    return WriteAtomically(path(QStringLiteral("index.json")), QJsonDocument(index).toJson());
}

std::optional<QByteArray> CliCache::loadGpu(const QString& build, const QString& screens) const {
    const QJsonObject o =
        ReadObject(path(QStringLiteral("gpu-") + SafeBuildName(build) + QStringLiteral(".json")));
    if (o.value(QLatin1String("screens")).toString() != screens ||
        !o.value(QLatin1String("data")).isObject()) {
        return std::nullopt;
    }
    return QJsonDocument(o.value(QLatin1String("data")).toObject()).toJson(QJsonDocument::Compact);
}

bool CliCache::storeGpu(const QString& build, const QString& screens, const QByteArray& json) const {
    const QJsonDocument data = QJsonDocument::fromJson(json);
    if (!data.isObject() || dir_.isEmpty() || !QDir().mkpath(dir_)) {
        return false;
    }
    QJsonObject o;
    o.insert(QStringLiteral("screens"), screens);
    o.insert(QStringLiteral("data"), data.object());
    return WriteAtomically(path(QStringLiteral("gpu-") + SafeBuildName(build) + QStringLiteral(".json")),
                           QJsonDocument(o).toJson());
}

void CliCache::clear() const {
    QDir dir(dir_);
    if (dir_.isEmpty() || !dir.exists()) {
        return;
    }
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& f : files) {
        dir.remove(f);
    }
}

}  // namespace fl
