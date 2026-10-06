// Round-trip INI document for the player configuration (futurama.ini).
//
// Reading follows the rules of the game host exactly (src/host/core/config.cpp ReadIni), so the
// launcher sees the same values as the game:
//   - UTF-8, optional BOM; lines split at '\n' (a trailing '\r' belongs to the line ending)
//   - blanks are ' ', '\t'; a line whose first non-blank character is ';' or '#' is a comment
//   - inline comment: ';' or '#' preceded by a blank and outside double quotes ends the value
//   - "[name]" starts a section; names and keys are case-insensitive (ASCII lower-case)
//   - "key = value": both sides trimmed; a value wrapped in double quotes loses the quotes
//   - the last occurrence of a key wins; keys before the first section are "section-less" and the host
//     uses them as fallback for a key of the same name in any section (effectiveValue)
//   - a line that is neither of the above is an error for the host (problems())
//
// Writing keeps everything it does not touch byte for byte: comments, blank lines, key order and
// spelling, unknown sections and keys, the line-ending style and the BOM. setValue() replaces only the
// value span of the last occurrence (an inline comment keeps its column where possible), remove()
// drops the key's lines, new keys go after the last key of the section's last block (aligned like
// their neighbours) and new sections at the end. save() is atomic (QSaveFile: temporary file + rename).
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace fl {

class IniDocument {
public:
    // Review M3c finding 13: the UI shows ProblemText(problem) in its language; `message` is the English
    // wording of the host (result JSON, logs).
    enum class ProblemCode { BadSectionHeader, ExpectedKeyValue, EmptyKey, InvalidUtf8 };
    struct Problem {
        int line = 0;     // 1-based; 0 = the whole file (InvalidUtf8)
        QString message;  // English, the host's wording ("bad section header", "expected key = value")
        ProblemCode code = ProblemCode::ExpectedKeyValue;
    };
    static QString ProblemText(const Problem& problem);  // in the current UI language
    bool hasInvalidUtf8() const;
    struct Entry {
        QString section;  // lower-case, empty = section-less
        QString key;      // lower-case
        QString value;    // decoded (host rules)
        int line = 0;     // 1-based
    };

    // Parses `bytes`; never fails (invalid lines are kept verbatim and listed in problems()).
    void parse(const QByteArray& bytes);
    QByteArray serialize() const;

    // Loads `path`. A missing file gives an empty document with existed() == false and returns true;
    // an unreadable file returns false with `error`.
    bool load(const QString& path, QString* error);
    // Writes atomically (creates the directory if needed). `error`: a localized message with a hint (write
    // permission, portable mode, --config).
    bool save(const QString& path, QString* error) const;
    bool existed() const { return existed_; }

    // Value of [section] key (last occurrence) - exact section only.
    std::optional<QString> value(const QString& section, const QString& key) const;
    // Host lookup: [section] key, else a section-less key of the same name.
    std::optional<QString> effectiveValue(const QString& section, const QString& key) const;
    bool contains(const QString& section, const QString& key) const {
        return value(section, key).has_value();
    }

    // Sets [section] key = value. Returns false (document unchanged) if the value cannot be written so
    // that the host reads it back unchanged (see EncodeValue).
    bool setValue(const QString& section, const QString& key, const QString& value);
    // Removes every line of [section] key. Returns true if one was removed.
    bool remove(const QString& section, const QString& key);
    // Inserts comment lines at the top (used for a new file). Each line gets a "; " prefix if it has none.
    void prependComments(const QStringList& lines);

    QList<Entry> entries() const;
    QStringList sectionNames() const;  // lower-case, in order of first appearance
    const QList<Problem>& problems() const { return problems_; }
    bool isEmpty() const { return lines_.isEmpty(); }
    int lineCount() const { return static_cast<int>(lines_.size()); }
    QString lineEnding() const { return eol_; }
    bool hasBom() const { return bom_; }

    // Host value rules.
    static QString DecodeValue(QStringView raw);
    // The text to write for `value` so that DecodeValue(text) == value: quoted when it has outer
    // blanks, an inline-comment trigger (" ;", " #"), a leading ';'/'#' or outer quotes. nullopt if it
    // contains a line break, or needs quotes and contains a '"' (the host cannot read that back).
    static std::optional<QString> EncodeValue(const QString& value);
    static QString NormalizeName(QStringView name);  // trim + ASCII lower-case

private:
    enum class Kind { Blank, Comment, Section, KeyValue, Invalid };
    struct Line {
        Kind kind = Kind::Blank;
        QString text;              // without the line ending
        QString section;           // Section: its name; KeyValue: the owning section (lower-case)
        QString key;               // KeyValue: lower-case key
        qsizetype valueStart = 0;  // KeyValue: raw value span in `text`
        qsizetype valueEnd = 0;
    };
    static Line ParseLine(const QString& text, const QString& currentSection);
    void reparse();  // recomputes sections/problems after an edit
    int findLast(const QString& section, const QString& key) const;

    QList<Line> lines_;
    QList<Problem> problems_;
    QString eol_ = QStringLiteral("\r\n");
    bool bom_ = false;
    bool trailingNewline_ = true;
    bool existed_ = false;
    bool badUtf8_ = false;  // the bytes were not valid UTF-8 (U+FFFD in the text)
};

}  // namespace fl
