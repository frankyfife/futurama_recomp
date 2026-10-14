// Configuration schema printed by `futurama.exe --print-config-schema=<file.json>` (shared contract
// item 1, schema_version 1). It is the single source of truth for every key the game accepts: the
// launcher builds its tabs, editors, tooltips and validation from it and never keeps its own key list.
//
//   {"schema_version":1, "build":"<string>",
//    "sections":[{"name":"render", "label":{"de":"..","en":".."},
//                 "keys":[{"name":"msaa",
//                 "type":"choice|bool|int|float|string|path_file|path_dir|resolution",
//                          "default":<value>, "choices":[{"value":"off","label":{"de":"Aus","en":"Off"}}],
//                          "min":<num>, "max":<num>, "step":<num>, "label":{..}, "help":{..},
//                          "advanced":false, "restart_required":true,
//                          "unit":"%",
//                          "ui":{"tab":"game|graphics|audio|controls|cheats|paths|advanced", "order":10,
//                                "widget":"combo|check|spin|dspin|text|file|dir|resolution|monitor|binding|slider|
//                                          audio_device"}}]}]}
//
// M4: tab "controls" (the [input] keys), widget "binding" (a binding list, core/bindings.h, INTERFACES.md 4.26) and
// "slider" (int/float with min/max; optional "unit" shown after the value).
// M5: tab "audio" (section [audio] falls back to it) and widget "audio_device" (a string holding a Windows audio
// endpoint ID, empty = the Windows default device; [audio] device of type string is shown with it even when the
// schema names no widget).
// G8: tab "cheats" (section [cheats] falls back to it).
//
// Tolerant parsing: unknown fields are ignored; an unknown type is read as "string", an unknown widget
// falls back to the type's widget, a missing ui.tab to the section's usual tab - each with a warning.
#pragma once

#include "core/l10n.h"

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace fl {

enum class KeyType { Choice, Bool, Int, Float, String, PathFile, PathDir, Resolution };
enum class UiTab { Game, Graphics, Paths, Advanced, Controls, Audio, Cheats };
enum class UiWidget { Combo, Check, Spin, DSpin, Text, File, Dir, Resolution, Monitor, Binding, Slider, AudioDevice, Color, Fraction };

// Order of the main window's tabs (M4: "Steuerung" after "Grafik"; M5: "Audio" between them; G8: "Cheats" after
// "Steuerung").
inline constexpr UiTab kAllTabs[] = {UiTab::Game,     UiTab::Graphics, UiTab::Audio, UiTab::Controls,
                                     UiTab::Cheats,   UiTab::Paths,    UiTab::Advanced};

QString KeyTypeName(KeyType type);
std::optional<KeyType> ParseKeyType(const QString& name);
QString UiTabName(UiTab tab);  // "game", "graphics", "audio", "controls", "cheats", "paths", "advanced"
std::optional<UiTab> ParseUiTab(const QString& name);
QString UiWidgetName(UiWidget widget);
std::optional<UiWidget> ParseUiWidget(const QString& name);
UiWidget DefaultWidgetFor(KeyType type);

struct ChoiceDef {
    QString value;
    LocalizedText label;
    // A choice "a..b" is a numeric range (host MatchesChoice); it validates but is not listed in a combo.
    bool isRange() const { return value.contains(QLatin1String("..")); }
};

struct KeyDef {
    QString section;  // lower-case
    QString name;     // lower-case
    KeyType type = KeyType::String;
    QJsonValue defaultJson;
    QString defaultText;  // the default as INI text (see DefaultTextFor)
    QList<ChoiceDef> choices;
    std::optional<double> min;
    std::optional<double> max;
    std::optional<double> step;
    // Accepted but not offered values ("hidden_choices", e.g. old spellings or numeric language ids).
    QList<ChoiceDef> hiddenChoices;
    LocalizedText label;
    LocalizedText help;
    bool advanced = false;
    bool restartRequired = true;
    // Optional fields of the host's schema (all tolerated when missing):
    bool rangeEnforced = true;    // "range_enforced": false = min/max/step are hints, the host accepts more
    bool developer = false;       // "developer": shown on the advanced tab only with "developer options"
    bool dynamicDefault = false;  // "dynamic_default": the effective default depends on the build/environment
    QString placeholder;  // "placeholder": what an empty value means (e.g. the default user data path)
    QString fileFilter;   // "ui.filter": file dialog patterns, e.g. "*.iso *.xiso"
    QString unit;         // "unit": shown after a slider value, e.g. "%" (M4)
    UiTab tab = UiTab::Advanced;  // effective tab (advanced keys always on UiTab::Advanced)
    int order = 0;
    UiWidget widget = UiWidget::Text;
    int schemaIndex = 0;  // position in the schema (stable tie-breaker)

    QString fullName() const { return section + QLatin1Char('.') + name; }
    QString displayLabel() const;  // label in the current language, else the key name
    const ChoiceDef* findChoice(const QString& value) const;  // case-insensitive, no ranges
};

struct SectionDef {
    QString name;
    LocalizedText label;
    QList<KeyDef> keys;
};

struct ConfigSchema {
    int version = 0;
    QString build;
    QList<SectionDef> sections;

    bool isEmpty() const { return sections.isEmpty(); }
    const KeyDef* find(const QString& section, const QString& key) const;
    const KeyDef* find(const QString& fullName) const;  // "section.key"
    const SectionDef* findSection(const QString& section) const;
    QList<const KeyDef*> allKeys() const;  // schema order
    // Keys of `tab` sorted by (order, schema position).
    QList<const KeyDef*> keysForTab(UiTab tab) const;
    // First key with ui.widget == `widget` (e.g. the monitor picker), or nullptr.
    const KeyDef* findByWidget(UiWidget widget) const;
};

// Converts a schema default into the INI text the launcher writes: bool -> "1"/"0", numbers in C
// locale (floats with at least one decimal: 1.0, 1.25), strings unchanged, null -> "".
QString DefaultTextFor(KeyType type, const QJsonValue& value);
// Float in C locale with at least one decimal ("1.0", "1.5", "0.25").
QString FormatFloat(double value);

// Parses the schema JSON. Returns false with `error` if the document is unusable (not JSON, no
// sections, schema_version missing/0); `warnings` lists tolerated problems.
bool ParseSchema(const QByteArray& json, ConfigSchema* out, QString* error, QStringList* warnings = nullptr);

}  // namespace fl
