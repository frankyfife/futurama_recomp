// The player configuration as the launcher edits it: the schema (what keys exist) + the INI document
// (what the file says) + pending edits (what the user changed but has not saved yet).
//
// Rules:
//   - value(): pending edit, else the INI value (host lookup incl. section-less fallback), else the
//     schema default.
//   - setValue(): a value equivalent to what is already effective is not an edit; a key that is absent
//     from the INI is only written when it differs from the default (the file stays minimal and later
//     builds can change defaults).
//     Keys with a dynamic default (schema "dynamic_default") are written whenever the user picks a value:
//     their static default is only the usual case, and an explicit choice must survive another build.
//   - resetToDefault(): removes the key from the INI (the build's default applies).
//   - save(): re-reads the file, applies the pending edits on top (external edits of other keys survive)
//     and writes atomically. A new file gets a short header comment.
#pragma once

#include "core/config_schema.h"
#include "core/ini_document.h"
#include "core/validation.h"

#include <QHash>
#include <QString>

#include <optional>

namespace fl {

struct UnknownEntry {
    QString section;
    QString key;
    QString value;
    int line = 0;
    bool unknownSection = false;  // whole section unknown (host: warning) vs. unknown key (host: error)
};

class SettingsModel {
public:
    void setSchema(ConfigSchema schema);
    const ConfigSchema& schema() const { return schema_; }

    // Loads the INI (a missing file is fine). Discards pending edits.
    bool load(const QString& iniPath, QString* error);
    const QString& iniPath() const { return iniPath_; }
    QString iniDir() const;
    bool fileExists() const { return ini_.existed(); }
    const IniDocument& ini() const { return ini_; }

    QString value(const KeyDef& key) const;
    std::optional<QString> iniValue(const KeyDef& key) const;  // effective INI value (no pending, no default)
    // True if the default applies: for keys with a dynamic default ("dynamic_default") this means the
    // key is not set at all, otherwise the value is equivalent to the schema default.
    bool isAtDefault(const KeyDef& key) const;
    // True if the INI (or a pending edit) sets the key explicitly.
    bool isExplicit(const KeyDef& key) const;
    bool isPending(const KeyDef& key) const { return pending_.contains(key.fullName()); }

    void setValue(const KeyDef& key, const QString& value);
    // "section.key" lookup; false if the schema has no such key.
    bool setValue(const QString& fullName, const QString& value);
    void resetToDefault(const KeyDef& key);
    void resetTab(UiTab tab);
    void discardPending() { pending_.clear(); }
    bool isDirty() const { return !pending_.isEmpty(); }
    int pendingCount() const { return static_cast<int>(pending_.size()); }

    ValidationResult validate(const KeyDef& key) const;
    QList<const KeyDef*> keysWithErrors() const;

    // INI entries the schema does not know (the host rejects unknown keys in known sections).
    QList<UnknownEntry> unknownEntries() const;
    bool removeUnknown(const UnknownEntry& entry);  // pending until save()

    // header: comment lines for a new file. Review M3c finding 20: a file with invalid UTF-8 is copied to
    // <ini>.bak first (saving replaces the invalid bytes with U+FFFD). `error` is localized.
    bool save(QString* error, const QStringList& header = {});
    // <ini>.bak written by the last save (empty if none).
    const QString& lastBackup() const { return lastBackup_; }

private:
    void applyPending(IniDocument& doc) const;

    ConfigSchema schema_;
    IniDocument ini_;
    QString lastBackup_;
    QString iniPath_;
    // fullName -> new INI text, or nullopt = remove the key
    QHash<QString, std::optional<QString>> pending_;
    QList<QPair<QString, QString>> pendingUnknownRemovals_;
};

}  // namespace fl
