// Cache of the schema and GPU query results per game build (launcher_cache\ next to the INI), so a
// launcher start does not run futurama.exe twice when nothing changed.
//   index.json               {"fingerprints": {"<fingerprint>": "<build string>"}}
//   schema-<build>.json      the schema JSON as printed
//   gpu-<build>.json         {"screens": "<screen signature>", "data": <query-gpu JSON>}
// The fingerprint covers path, size and modification time of futurama.exe and futurama_host.dll; the
// screen signature (connected monitors, sizes, refresh rates) invalidates the GPU entry when the display
// setup changes.
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace fl {

class CliCache {
public:
    explicit CliCache(QString dir);

    static QString Fingerprint(const QString& gameExe, const QString& hostDll);
    // Build string -> file-name-safe stem (letters, digits, '.', '_', '-'; long strings get a hash).
    static QString SafeBuildName(const QString& build);

    std::optional<QByteArray> loadSchema(const QString& fingerprint) const;
    std::optional<QString> buildFor(const QString& fingerprint) const;
    bool storeSchema(const QString& fingerprint, const QString& build, const QByteArray& json) const;
    std::optional<QByteArray> loadGpu(const QString& build, const QString& screens) const;
    bool storeGpu(const QString& build, const QString& screens, const QByteArray& json) const;
    void clear() const;
    const QString& dir() const { return dir_; }

private:
    QString path(const QString& name) const;

    QString dir_;
};

}  // namespace fl
