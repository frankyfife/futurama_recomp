#include "core/l10n.h"

#include <QHash>
#include <QLocale>
#include <QSet>

#include <atomic>

namespace fl {
namespace {

std::atomic<Language> g_language{Language::English};

const QHash<QByteArray, const TranslationEntry*>& Index() {
    static const QHash<QByteArray, const TranslationEntry*> index = [] {
        QHash<QByteArray, const TranslationEntry*> map;
        for (const TranslationEntry& entry : TranslationTable()) {
            map.insert(QByteArray(entry.id), &entry);
        }
        return map;
    }();
    return index;
}

}  // namespace

Language CurrentLanguage() noexcept {
    return g_language.load();
}

void SetLanguage(Language language) noexcept {
    g_language.store(language);
    // Numbers, sizes and spin boxes follow the UI language ("1,75 GB" / "1.75 GB"), not the system locale.
    QLocale::setDefault(language == Language::German ? QLocale(QLocale::German, QLocale::Germany)
                                                     : QLocale(QLocale::English, QLocale::UnitedStates));
}

Language SystemLanguage() {
    const QStringList languages = QLocale::system().uiLanguages();
    const QString first = languages.isEmpty() ? QLocale::system().name() : languages.first();
    return first.startsWith(QLatin1String("de"), Qt::CaseInsensitive) ? Language::German : Language::English;
}

QString LanguageCode(Language language) {
    return language == Language::German ? QStringLiteral("de") : QStringLiteral("en");
}

std::optional<Language> ParseLanguageCode(QStringView code) {
    const QString c = code.trimmed().toString().toLower();
    if (c == QLatin1String("de") || c == QLatin1String("deu") || c == QLatin1String("german") ||
        c == QLatin1String("deutsch")) {
        return Language::German;
    }
    if (c == QLatin1String("en") || c == QLatin1String("eng") || c == QLatin1String("english")) {
        return Language::English;
    }
    return std::nullopt;
}

QString T(const char* id) {
    return T(id, CurrentLanguage());
}

QString T(const char* id, Language language) {
    const auto& index = Index();
    const auto it = index.constFind(QByteArray::fromRawData(id, static_cast<qsizetype>(qstrlen(id))));
    if (it == index.constEnd()) {
        static QSet<QByteArray> reported;
        if (!reported.contains(QByteArray(id))) {
            reported.insert(QByteArray(id));
            qWarning("l10n: no translation row for id \"%s\"", id);
        }
        return QLatin1Char('[') + QString::fromUtf8(id) + QLatin1Char(']');
    }
    const TranslationEntry* entry = it.value();
    return QString::fromUtf8(language == Language::German ? entry->de : entry->en);
}

QString LocalizedText::text(Language language) const {
    const QString& preferred = language == Language::German ? de : en;
    const QString& other = language == Language::German ? en : de;
    return preferred.isEmpty() ? other : preferred;
}

}  // namespace fl
