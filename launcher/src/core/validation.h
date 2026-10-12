// Validation of configuration values against the schema (choices, ranges, steps, resolutions, paths),
// with the host's parsing rules (src/host/core/config.cpp, config_schema.cpp): booleans
// 0/1/true/false/yes/no/on/off, integers decimal or 0x hex (strtoull base 0), floats with '.' (C locale),
// choices case-insensitive incl. hidden choices and numeric ranges "a..b", resolutions "auto" or
// "<w>x<h>" (blanks around the numbers allowed, each side within the schema's min..max).
// Severity follows the host: what the host rejects (exit code 1) is an Error and blocks saving; ranges
// marked "range_enforced": false and steps are only Warnings.
#pragma once

#include "core/config_schema.h"

#include <QSize>
#include <QString>

#include <optional>

namespace fl {

enum class Severity { Ok, Warning, Error };

enum class IssueCode {
    None,
    NotAChoice,
    NotABool,
    NotAnInteger,
    NotANumber,
    BelowMin,
    AboveMax,
    OffStep,
    BadResolution,
    ResolutionOutOfRange,
    FileMissing,
    DirMissing,
    NotAFile,
    NotADirectory,
    NotRepresentable,  // line break, or quotes the INI cannot carry
    QuotedPath,        // review M3c finding 10: a path wrapped in quotes (the host would keep them)
    BadBinding,        // M4: a binding list of [input] that the input layer rejects (core/bindings.h)
};

struct ValidationResult {
    Severity severity = Severity::Ok;
    IssueCode code = IssueCode::None;
    QString message;  // in the current UI language

    bool isError() const { return severity == Severity::Error; }
    bool isWarning() const { return severity == Severity::Warning; }
};

std::optional<bool> ParseBoolText(QStringView text);
std::optional<qint64> ParseIntText(QStringView text);  // decimal, 0x hex, leading-0 octal (strtoull base 0)
std::optional<double> ParseFloatText(QStringView text);
// "auto" -> QSize() (invalid size), "WxH" / "W x H" -> the size; nullopt for anything else.
std::optional<QSize> ParseResolutionText(QStringView text);
QString FormatResolution(QSize size);  // "1920x1080"
bool IsAutoResolution(QStringView text);

// Host MatchesChoice: one of the choice values (case-insensitive) or inside a numeric range "a..b".
bool MatchesChoice(const KeyDef& key, const QString& value);

// Resolves a (possibly relative) path against `baseDir` (the INI directory, like the host's GetPath).
QString ResolvePath(const QString& path, const QString& baseDir);

// Review M3c finding 10: a path as typed or pasted - trimmed and without one pair of enclosing double quotes.
// Explorer's "Als Pfad kopieren" (Ctrl+Shift+C) puts the quotes around the path. Every path input of the
// launcher (wizard, path editors, --set of path keys, SettingsModel::setValue) goes through it.
QString CleanPathInput(const QString& text);

// Validates `value` (INI text) for `key`. Empty strings are valid for string/path keys (= default).
// Missing files/directories are warnings (a removable drive may be absent), everything else errors.
ValidationResult ValidateValue(const KeyDef& key, const QString& value, const QString& baseDir);

// True if both INI texts mean the same value for `key` ("1" == "on", "0x10" == "16", "1.0" == "1",
// "AUTO" == "auto", paths case-insensitive after cleaning, binding lists as sets of tokens).
bool ValuesEquivalent(const KeyDef& key, const QString& a, const QString& b);

// Bool text in the style of `existing` (1/0, true/false, yes/no, on/off); "1"/"0" if there is none.
QString BoolText(bool value, const QString& existing = QString());

}  // namespace fl
