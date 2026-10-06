#include "core/config_schema.h"

#include "core/ini_document.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

#include <algorithm>
#include <cmath>

namespace fl {
namespace {

struct NamePair {
    const char* name;
    int value;
};

constexpr NamePair kTypeNames[] = {
    {"choice", static_cast<int>(KeyType::Choice)},    {"bool", static_cast<int>(KeyType::Bool)},
    {"int", static_cast<int>(KeyType::Int)},          {"float", static_cast<int>(KeyType::Float)},
    {"string", static_cast<int>(KeyType::String)},    {"path_file", static_cast<int>(KeyType::PathFile)},
    {"path_dir", static_cast<int>(KeyType::PathDir)}, {"resolution", static_cast<int>(KeyType::Resolution)},
};
constexpr NamePair kTabNames[] = {
    {"game", static_cast<int>(UiTab::Game)},
    {"graphics", static_cast<int>(UiTab::Graphics)},
    {"paths", static_cast<int>(UiTab::Paths)},
    {"advanced", static_cast<int>(UiTab::Advanced)},
};
constexpr NamePair kWidgetNames[] = {
    {"combo", static_cast<int>(UiWidget::Combo)},     {"check", static_cast<int>(UiWidget::Check)},
    {"spin", static_cast<int>(UiWidget::Spin)},       {"dspin", static_cast<int>(UiWidget::DSpin)},
    {"text", static_cast<int>(UiWidget::Text)},       {"file", static_cast<int>(UiWidget::File)},
    {"dir", static_cast<int>(UiWidget::Dir)},         {"resolution", static_cast<int>(UiWidget::Resolution)},
    {"monitor", static_cast<int>(UiWidget::Monitor)},
};

template <typename Enum, std::size_t N>
std::optional<Enum> Lookup(const NamePair (&table)[N], const QString& name) {
    const QString lower = name.trimmed().toLower();
    for (const NamePair& p : table) {
        if (lower == QLatin1String(p.name)) {
            return static_cast<Enum>(p.value);
        }
    }
    return std::nullopt;
}

template <typename Enum, std::size_t N>
QString NameOf(const NamePair (&table)[N], Enum value) {
    for (const NamePair& p : table) {
        if (p.value == static_cast<int>(value)) {
            return QLatin1String(p.name);
        }
    }
    return QString();
}

LocalizedText ParseText(const QJsonValue& v) {
    LocalizedText t;
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        t.de = o.value(QLatin1String("de")).toString();
        t.en = o.value(QLatin1String("en")).toString();
    } else if (v.isString()) {
        t.de = t.en = v.toString();
    }
    return t;
}

std::optional<double> ParseNumber(const QJsonValue& v) {
    if (v.isDouble()) {
        return v.toDouble();
    }
    if (v.isString()) {
        bool ok = false;
        const double d = QLocale::c().toDouble(v.toString().trimmed(), &ok);
        if (ok) {
            return d;
        }
    }
    return std::nullopt;
}

// Usual tab of a section when the schema has no ui.tab.
UiTab FallbackTab(const QString& section) {
    if (section == QLatin1String("paths")) {
        return UiTab::Paths;
    }
    if (section == QLatin1String("render")) {
        return UiTab::Graphics;
    }
    if (section == QLatin1String("settings") || section == QLatin1String("game")) {
        return UiTab::Game;
    }
    return UiTab::Advanced;
}

bool WidgetFitsType(UiWidget widget, KeyType type) {
    switch (widget) {
    case UiWidget::Check:
        return type == KeyType::Bool;
    case UiWidget::Spin:
        return type == KeyType::Int;
    case UiWidget::DSpin:
        return type == KeyType::Float;
    case UiWidget::File:
        return type == KeyType::PathFile;
    case UiWidget::Dir:
        return type == KeyType::PathDir;
    case UiWidget::Resolution:
        return type == KeyType::Resolution || type == KeyType::String;
    case UiWidget::Combo:
        return type == KeyType::Choice || type == KeyType::Bool;
    case UiWidget::Monitor:
        return type == KeyType::String || type == KeyType::Choice || type == KeyType::Int;
    case UiWidget::Text:
        return true;
    }
    return false;
}

}  // namespace

QString KeyTypeName(KeyType type) {
    return NameOf(kTypeNames, type);
}
std::optional<KeyType> ParseKeyType(const QString& name) {
    return Lookup<KeyType>(kTypeNames, name);
}
QString UiTabName(UiTab tab) {
    return NameOf(kTabNames, tab);
}
std::optional<UiTab> ParseUiTab(const QString& name) {
    return Lookup<UiTab>(kTabNames, name);
}
QString UiWidgetName(UiWidget widget) {
    return NameOf(kWidgetNames, widget);
}
std::optional<UiWidget> ParseUiWidget(const QString& name) {
    return Lookup<UiWidget>(kWidgetNames, name);
}

UiWidget DefaultWidgetFor(KeyType type) {
    switch (type) {
    case KeyType::Choice:
        return UiWidget::Combo;
    case KeyType::Bool:
        return UiWidget::Check;
    case KeyType::Int:
        return UiWidget::Spin;
    case KeyType::Float:
        return UiWidget::DSpin;
    case KeyType::PathFile:
        return UiWidget::File;
    case KeyType::PathDir:
        return UiWidget::Dir;
    case KeyType::Resolution:
        return UiWidget::Resolution;
    case KeyType::String:
        return UiWidget::Text;
    }
    return UiWidget::Text;
}

QString FormatFloat(double value) {
    QString s = QLocale::c().toString(value, 'g', 12);
    if (!s.contains(QLatin1Char('.')) && !s.contains(QLatin1Char('e')) && !s.contains(QLatin1String("inf")) &&
        !s.contains(QLatin1String("nan"))) {
        s += QLatin1String(".0");
    }
    return s;
}

QString DefaultTextFor(KeyType type, const QJsonValue& value) {
    if (value.isNull() || value.isUndefined()) {
        return QString();
    }
    switch (type) {
    case KeyType::Bool:
        if (value.isBool()) {
            return value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
        }
        if (value.isDouble()) {
            return value.toDouble() != 0.0 ? QStringLiteral("1") : QStringLiteral("0");
        }
        return value.toString();
    case KeyType::Int:
        if (value.isDouble()) {
            return QString::number(static_cast<qint64>(std::llround(value.toDouble())));
        }
        return value.toString();
    case KeyType::Float:
        if (value.isDouble()) {
            return FormatFloat(value.toDouble());
        }
        return value.toString();
    default:
        if (value.isDouble()) {
            const double d = value.toDouble();
            return d == std::floor(d) ? QString::number(static_cast<qint64>(d)) : FormatFloat(d);
        }
        if (value.isBool()) {
            return value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
        }
        return value.toString();
    }
}

QString KeyDef::displayLabel() const {
    const QString t = label.text();
    return t.isEmpty() ? name : t;
}

const ChoiceDef* KeyDef::findChoice(const QString& value) const {
    const QString v = value.trimmed();
    for (const ChoiceDef& c : choices) {
        if (!c.isRange() && c.value.compare(v, Qt::CaseInsensitive) == 0) {
            return &c;
        }
    }
    return nullptr;
}

const KeyDef* ConfigSchema::find(const QString& section, const QString& key) const {
    const QString s = IniDocument::NormalizeName(section);
    const QString k = IniDocument::NormalizeName(key);
    for (const SectionDef& sec : sections) {
        if (sec.name != s) {
            continue;
        }
        for (const KeyDef& def : sec.keys) {
            if (def.name == k) {
                return &def;
            }
        }
    }
    return nullptr;
}

const KeyDef* ConfigSchema::find(const QString& fullName) const {
    const qsizetype dot = fullName.indexOf(QLatin1Char('.'));
    if (dot <= 0) {
        return nullptr;
    }
    return find(fullName.left(dot), fullName.mid(dot + 1));
}

const SectionDef* ConfigSchema::findSection(const QString& section) const {
    const QString s = IniDocument::NormalizeName(section);
    for (const SectionDef& sec : sections) {
        if (sec.name == s) {
            return &sec;
        }
    }
    return nullptr;
}

QList<const KeyDef*> ConfigSchema::allKeys() const {
    QList<const KeyDef*> out;
    for (const SectionDef& sec : sections) {
        for (const KeyDef& def : sec.keys) {
            out.append(&def);
        }
    }
    return out;
}

QList<const KeyDef*> ConfigSchema::keysForTab(UiTab tab) const {
    QList<const KeyDef*> out;
    for (const KeyDef* def : allKeys()) {
        if (def->tab == tab) {
            out.append(def);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const KeyDef* a, const KeyDef* b) {
        return a->order != b->order ? a->order < b->order : a->schemaIndex < b->schemaIndex;
    });
    return out;
}

const KeyDef* ConfigSchema::findByWidget(UiWidget widget) const {
    for (const KeyDef* def : allKeys()) {
        if (def->widget == widget) {
            return def;
        }
    }
    return nullptr;
}

bool ParseSchema(const QByteArray& json, ConfigSchema* out, QString* error, QStringList* warnings) {
    auto fail = [&](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    auto warn = [&](const QString& message) {
        if (warnings != nullptr) {
            warnings->append(message);
        }
    };
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        return fail(QStringLiteral("schema is not a JSON object: ") + parseError.errorString());
    }
    const QJsonObject root = doc.object();
    ConfigSchema schema;
    schema.version = root.value(QLatin1String("schema_version")).toInt(0);
    if (schema.version <= 0) {
        return fail(QStringLiteral("schema_version missing or invalid"));
    }
    if (schema.version > 1) {
        warn(QStringLiteral("schema_version %1 is newer than the launcher (1); unknown fields are ignored")
                 .arg(schema.version));
    }
    schema.build = root.value(QLatin1String("build")).toString();
    const QJsonArray sections = root.value(QLatin1String("sections")).toArray();
    if (sections.isEmpty()) {
        return fail(QStringLiteral("schema has no sections"));
    }
    int index = 0;
    for (const QJsonValue& sv : sections) {
        const QJsonObject so = sv.toObject();
        SectionDef section;
        section.name = IniDocument::NormalizeName(so.value(QLatin1String("name")).toString());
        if (section.name.isEmpty()) {
            warn(QStringLiteral("section without a name ignored"));
            continue;
        }
        if (schema.findSection(section.name) != nullptr) {
            warn(QStringLiteral("section [%1] listed twice; the second entry is ignored").arg(section.name));
            continue;
        }
        section.label = ParseText(so.value(QLatin1String("label")));
        for (const QJsonValue& kv : so.value(QLatin1String("keys")).toArray()) {
            const QJsonObject ko = kv.toObject();
            KeyDef key;
            key.section = section.name;
            key.name = IniDocument::NormalizeName(ko.value(QLatin1String("name")).toString());
            if (key.name.isEmpty()) {
                warn(QStringLiteral("key without a name in [%1] ignored").arg(section.name));
                continue;
            }
            const bool duplicate = std::any_of(section.keys.cbegin(), section.keys.cend(),
                                               [&](const KeyDef& k) { return k.name == key.name; });
            if (duplicate) {
                warn(QStringLiteral("key %1 listed twice; the second entry is ignored").arg(key.fullName()));
                continue;
            }
            const QString typeName = ko.value(QLatin1String("type")).toString();
            if (const auto type = ParseKeyType(typeName)) {
                key.type = *type;
            } else {
                warn(QStringLiteral("key %1: unknown type \"%2\", edited as string")
                         .arg(key.fullName(), typeName));
                key.type = KeyType::String;
            }
            auto parseChoices = [](const QJsonValue& list) {
                QList<ChoiceDef> parsed;
                for (const QJsonValue& cv : list.toArray()) {
                    ChoiceDef choice;
                    if (cv.isObject()) {
                        const QJsonObject co = cv.toObject();
                        const QJsonValue value = co.value(QLatin1String("value"));
                        choice.value =
                            value.isDouble() ? DefaultTextFor(KeyType::Choice, value) : value.toString();
                        choice.label = ParseText(co.value(QLatin1String("label")));
                    } else {
                        choice.value = cv.isDouble() ? DefaultTextFor(KeyType::Choice, cv) : cv.toString();
                    }
                    if (choice.label.isEmpty()) {
                        choice.label.de = choice.label.en = choice.value;
                    }
                    parsed.append(choice);
                }
                return parsed;
            };
            key.choices = parseChoices(ko.value(QLatin1String("choices")));
            key.hiddenChoices = parseChoices(ko.value(QLatin1String("hidden_choices")));
            if (key.type == KeyType::Choice && key.choices.isEmpty()) {
                warn(QStringLiteral("choice key %1 has no choices, edited as string").arg(key.fullName()));
                key.type = KeyType::String;
            }
            key.defaultJson = ko.value(QLatin1String("default"));
            key.defaultText = DefaultTextFor(key.type, key.defaultJson);
            key.min = ParseNumber(ko.value(QLatin1String("min")));
            key.max = ParseNumber(ko.value(QLatin1String("max")));
            key.step = ParseNumber(ko.value(QLatin1String("step")));
            if (key.step && *key.step <= 0.0) {
                warn(QStringLiteral("key %1: step <= 0 ignored").arg(key.fullName()));
                key.step.reset();
            }
            key.label = ParseText(ko.value(QLatin1String("label")));
            key.help = ParseText(ko.value(QLatin1String("help")));
            key.advanced = ko.value(QLatin1String("advanced")).toBool(false);
            key.restartRequired = ko.value(QLatin1String("restart_required")).toBool(true);
            key.rangeEnforced = ko.value(QLatin1String("range_enforced")).toBool(true);
            key.developer = ko.value(QLatin1String("developer")).toBool(false);
            key.dynamicDefault = ko.value(QLatin1String("dynamic_default")).toBool(false);
            key.placeholder = ko.value(QLatin1String("placeholder")).toString();
            const QJsonObject ui = ko.value(QLatin1String("ui")).toObject();
            key.fileFilter = ui.value(QLatin1String("filter")).toString().trimmed();
            const QString tabName = ui.value(QLatin1String("tab")).toString();
            if (const auto tab = ParseUiTab(tabName)) {
                key.tab = *tab;
            } else {
                key.tab = FallbackTab(key.section);
                if (!tabName.isEmpty()) {
                    warn(QStringLiteral("key %1: unknown ui.tab \"%2\"").arg(key.fullName(), tabName));
                }
            }
            if (key.advanced) {
                key.tab = UiTab::Advanced;
            }
            key.order = ui.value(QLatin1String("order")).toInt(1000);
            const QString widgetName = ui.value(QLatin1String("widget")).toString();
            key.widget = DefaultWidgetFor(key.type);
            if (const auto widget = ParseUiWidget(widgetName)) {
                if (WidgetFitsType(*widget, key.type)) {
                    key.widget = *widget;
                } else {
                    warn(QStringLiteral("key %1: widget \"%2\" does not fit type %3, using %4")
                             .arg(key.fullName(), widgetName, KeyTypeName(key.type),
                                  UiWidgetName(key.widget)));
                }
            } else if (!widgetName.isEmpty()) {
                warn(QStringLiteral("key %1: unknown ui.widget \"%2\"").arg(key.fullName(), widgetName));
            }
            key.schemaIndex = index++;
            section.keys.append(key);
        }
        schema.sections.append(section);
    }
    if (schema.allKeys().isEmpty()) {
        return fail(QStringLiteral("schema has no keys"));
    }
    *out = std::move(schema);
    return true;
}

}  // namespace fl
