// UI language and the in-code translation tables (German/English). The Qt installation has no
// Linguist tools (no lrelease), so every user-visible launcher text lives in l10n_table.cpp as one row
// {id, German, English}. Texts of the game configuration (labels, help, choices) come from the
// schema that futurama.exe prints (LocalizedText).
#pragma once

#include <QString>
#include <QStringView>

#include <optional>
#include <span>

namespace fl {

enum class Language { German, English };

Language CurrentLanguage() noexcept;
void SetLanguage(Language language) noexcept;
// German if the system UI language is German, English otherwise.
Language SystemLanguage();
QString LanguageCode(Language language);  // "de" / "en"
// "de"/"deu"/"german" -> German, "en"/"eng"/"english" -> English (case-insensitive), else nullopt.
std::optional<Language> ParseLanguageCode(QStringView code);

// Text of `id` in the current (or the given) language. An unknown id returns "[id]" so that a missing
// row is visible; tests/tst_misc.cpp checks that every T("...") id used in the sources has a row.
QString T(const char* id);
QString T(const char* id, Language language);

struct TranslationEntry {
    const char* id;
    const char* de;  // UTF-8 (the sources are compiled with /utf-8)
    const char* en;
};
std::span<const TranslationEntry> TranslationTable() noexcept;

// A text with German and English variants (schema labels and help texts). text() falls back to the
// other language when one is missing.
struct LocalizedText {
    QString de;
    QString en;

    QString text(Language language) const;
    QString text() const { return text(CurrentLanguage()); }
    bool isEmpty() const { return de.isEmpty() && en.isEmpty(); }
};

}  // namespace fl
