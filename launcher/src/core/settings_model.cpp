#include "core/settings_model.h"

#include "core/l10n.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace fl {

void SettingsModel::setSchema(ConfigSchema schema) {
    schema_ = std::move(schema);
    pending_.clear();
}

bool SettingsModel::load(const QString& iniPath, QString* error) {
    iniPath_ = iniPath;
    pending_.clear();
    pendingUnknownRemovals_.clear();
    return ini_.load(iniPath, error);
}

QString SettingsModel::iniDir() const {
    return iniPath_.isEmpty() ? QString() : QFileInfo(iniPath_).absolutePath();
}

std::optional<QString> SettingsModel::iniValue(const KeyDef& key) const {
    return ini_.effectiveValue(key.section, key.name);
}

QString SettingsModel::value(const KeyDef& key) const {
    const auto it = pending_.constFind(key.fullName());
    if (it != pending_.constEnd()) {
        return it->has_value() ? **it : key.defaultText;
    }
    if (const auto v = iniValue(key)) {
        return *v;
    }
    return key.defaultText;
}

bool SettingsModel::isExplicit(const KeyDef& key) const {
    const auto it = pending_.constFind(key.fullName());
    if (it != pending_.constEnd()) {
        return it->has_value();
    }
    return iniValue(key).has_value();
}

bool SettingsModel::isAtDefault(const KeyDef& key) const {
    if (key.dynamicDefault) {
        return !isExplicit(key);
    }
    return ValuesEquivalent(key, value(key), key.defaultText);
}

void SettingsModel::setValue(const KeyDef& key, const QString& value) {
    // Review M3c finding 10: paths without enclosing quotes (Explorer's "Als Pfad kopieren").
    const QString newValue =
        key.type == KeyType::PathFile || key.type == KeyType::PathDir ? CleanPathInput(value) : value;
    const QString name = key.fullName();
    const std::optional<QString> inFile = ini_.value(key.section, key.name);
    if (inFile) {
        if (ValuesEquivalent(key, newValue, *inFile)) {
            pending_.remove(name);
        } else {
            pending_.insert(name, newValue);
        }
        return;
    }
    // Not in its section: a section-less fallback value or the default is effective. A dynamic default
    // is not known here, so an explicit choice is always written.
    const QString effective = iniValue(key).value_or(key.defaultText);
    if (!key.dynamicDefault && ValuesEquivalent(key, newValue, effective) &&
        ValuesEquivalent(key, newValue, key.defaultText)) {
        pending_.remove(name);
    } else {
        pending_.insert(name, newValue);
    }
}

bool SettingsModel::setValue(const QString& fullName, const QString& newValue) {
    const KeyDef* key = schema_.find(fullName);
    if (key == nullptr) {
        return false;
    }
    setValue(*key, newValue);
    return true;
}

void SettingsModel::resetToDefault(const KeyDef& key) {
    if (ini_.contains(key.section, key.name)) {
        pending_.insert(key.fullName(), std::nullopt);
    } else {
        pending_.remove(key.fullName());
    }
}

void SettingsModel::resetTab(UiTab tab) {
    for (const KeyDef* key : schema_.keysForTab(tab)) {
        resetToDefault(*key);
    }
}

ValidationResult SettingsModel::validate(const KeyDef& key) const {
    return ValidateValue(key, value(key), iniDir());
}

QList<const KeyDef*> SettingsModel::keysWithErrors() const {
    QList<const KeyDef*> out;
    for (const KeyDef* key : schema_.allKeys()) {
        if (validate(*key).isError()) {
            out.append(key);
        }
    }
    return out;
}

QList<UnknownEntry> SettingsModel::unknownEntries() const {
    QList<UnknownEntry> out;
    for (const IniDocument::Entry& e : ini_.entries()) {
        if (pendingUnknownRemovals_.contains(qMakePair(e.section, e.key))) {
            continue;
        }
        if (e.section.isEmpty()) {
            bool known = false;
            for (const KeyDef* key : schema_.allKeys()) {
                known = known || key->name == e.key;
            }
            if (!known) {
                out.append({e.section, e.key, e.value, e.line, false});
            }
            continue;
        }
        const bool sectionKnown = schema_.findSection(e.section) != nullptr;
        if (!sectionKnown) {
            out.append({e.section, e.key, e.value, e.line, true});
        } else if (schema_.find(e.section, e.key) == nullptr) {
            out.append({e.section, e.key, e.value, e.line, false});
        }
    }
    return out;
}

bool SettingsModel::removeUnknown(const UnknownEntry& entry) {
    const auto pair = qMakePair(entry.section, entry.key);
    if (pendingUnknownRemovals_.contains(pair)) {
        return false;
    }
    pendingUnknownRemovals_.append(pair);
    return true;
}

void SettingsModel::applyPending(IniDocument& doc) const {
    // Schema order keeps new sections and keys in a predictable order.
    for (const KeyDef* key : schema_.allKeys()) {
        const auto it = pending_.constFind(key->fullName());
        if (it == pending_.constEnd()) {
            continue;
        }
        if (it->has_value()) {
            doc.setValue(key->section, key->name, **it);
        } else {
            doc.remove(key->section, key->name);
        }
    }
    for (const auto& [section, key] : pendingUnknownRemovals_) {
        doc.remove(section, key);
    }
}

bool SettingsModel::save(QString* error, const QStringList& header) {
    for (const KeyDef* key : schema_.allKeys()) {
        const auto it = pending_.constFind(key->fullName());
        if (it != pending_.constEnd() && it->has_value() && !IniDocument::EncodeValue(**it)) {
            if (error != nullptr) {
                *error = T("save.not_representable").arg(QStringLiteral("[%1] %2").arg(key->section, key->name));
            }
            return false;
        }
    }
    IniDocument doc;
    if (!doc.load(iniPath_, error)) {
        return false;
    }
    const bool isNew = !doc.existed() || doc.isEmpty();
    lastBackup_.clear();
    if (doc.hasInvalidUtf8()) {
        const QString backup = iniPath_ + QStringLiteral(".bak");
        QFile::remove(backup);
        if (!QFile::copy(iniPath_, backup)) {
            if (error != nullptr) {
                *error = T("save.backup_failed").arg(QDir::toNativeSeparators(backup));
            }
            return false;
        }
        lastBackup_ = QDir::toNativeSeparators(backup);
    }
    applyPending(doc);
    if (isNew && !header.isEmpty()) {
        doc.prependComments(header);
    }
    if (!doc.save(iniPath_, error)) {
        return false;
    }
    pending_.clear();
    pendingUnknownRemovals_.clear();
    return ini_.load(iniPath_, error);
}

}  // namespace fl
