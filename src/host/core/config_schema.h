// Configuration schema (M3c): the ONE definition table of every key the host accepts.
//
// Owned by the host-CLI/schema agent. The table in config_schema.cpp is the single source of truth for
//   - the configuration validation of core::LoadConfig (core/config.cpp): unknown keys, booleans, numbers,
//     choices, enforced ranges, resolutions (configuration error = exit code 1),
//   - core::IsKnownConfigKey / core::ConfigKeyChoices (tests compare them with the modules' parsers),
//   - the JSON schema of `futurama.exe --print-config-schema=<file.json>` (core/cli_tools.h), from which the
//     Qt launcher (FuturamaLauncher.exe) builds its fields: type, default, choices, range, de/en label and help
//     text, advanced flag and UI hints.
// A module that reads a new key adds ONE row to kKeys in config_schema.cpp (tests/schema_tests.cpp scans the
// host sources for `"section", "key"` reads without a row).
//
// Types (JSON "type"):
//   bool        0/1, true/false, yes/no, on/off (case-insensitive)
//   int         unsigned 32-bit, decimal or 0x hex (strtoul base 0, no unit); "min"/"max" enforced unless the
//               row has kConfigRangeIsHint (then the module clamps or replaces out-of-range values itself)
//   float       decimal number with '.' ("1.5"), min/max enforced
//   choice      one of the '|' words (case-insensitive) or numeric ranges "a..b"; hidden choices are accepted
//               but not offered in the UI (JSON "hidden_choices")
//   string      free text; list keys (comma separated) are "string" too
//   path_file / path_dir   paths ([paths] values are resolved against the INI directory when read)
//   resolution  "auto" or <width>x<height> (each kConfigResolutionMin..kConfigResolutionMax); for this type the
//               choices are suggestions (any valid size is accepted)
// Defaults are given in INI form ("1", "0x200000", "auto"). Keys whose default depends on the build, the
// environment or another key carry kConfigDynamicDefault (JSON "dynamic_default": true): the launcher should
// only write them when the player changes them.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace core {

enum class ConfigKeyType : std::uint8_t { Bool, Int, Float, Choice, String, PathFile, PathDir, Resolution };
enum class ConfigUiTab : std::uint8_t { Game, Graphics, Paths, Advanced,
                                        Controls /* M4 input agent: "controls" ([input], the launcher's Steuerung tab) */,
                                        Audio /* M5 tooling agent: "audio" (player keys of [audio], the launcher's Audio tab) */ };
enum class ConfigUiWidget : std::uint8_t {
    Combo, Check, Spin, DSpin, Text, File, Dir, Resolution, Monitor,
    // M4 (input agent, src/INTERFACES.md 4.26): "binding" = a string key holding a binding list (input/bindings.h),
    // "slider" = an int/float key with min/max shown as a slider (optional ConfigKeyDef::unit).
    Binding, Slider,
    // M5 (tooling/launcher agent, src/INTERFACES.md 4.29): "audio_device" = a string key holding a Windows audio
    // endpoint ID (IMMDevice::GetId; empty = the Windows default device), shown as a list of the output devices.
    AudioDevice,
    // G1 (main session): "color" = a string key holding "original" or RRGGBB, shown as a colour button with a
    // colour dialog and an "original" check.
    Color,
    // G1 (main session): "fraction" = a string key holding "original" or a number 0..1, shown as an "original" check
    // plus a percent slider.
    Fraction
};

// Player-facing text in German and English (UTF-8).
struct ConfigText {
    const char* de = "";
    const char* en = "";
};

struct ConfigChoice {
    const char* value = "";  // INI word, or a numeric range "a..b" (always hidden)
    ConfigText label;
    bool hidden = false;     // accepted, but not offered in the UI
};

enum ConfigKeyFlag : std::uint32_t {
    kConfigAdvanced = 1u << 0,        // JSON "advanced": for experienced players / troubleshooting
    kConfigDeveloper = 1u << 1,       // JSON "developer": test or diagnosis knob, not shown to players
    kConfigDynamicDefault = 1u << 2,  // JSON "dynamic_default": default depends on build/environment/other keys
    kConfigRangeIsHint = 1u << 3,     // min/max are UI limits only (the module clamps), not validated
};

struct ConfigKeyDef {
    const char* section = "";
    const char* name = "";
    ConfigKeyType type = ConfigKeyType::String;
    const char* defaultValue = "";  // INI form; "" = not set / empty
    std::span<const ConfigChoice> choices{};
    bool ranged = false;            // min/max/step valid (int, float; resolution: per dimension)
    double min = 0.0;
    double max = 0.0;
    double step = 0.0;              // 0 = none (int: 1)
    ConfigText label;
    ConfigText help;
    ConfigUiTab tab = ConfigUiTab::Advanced;
    int order = 0;                  // position within the tab (ascending)
    ConfigUiWidget widget = ConfigUiWidget::Text;
    std::uint32_t flags = 0;        // ConfigKeyFlag
    const char* fileFilter = nullptr;  // path_file: file dialog patterns ("*.iso *.xiso *.bin"), JSON ui.filter
    // M4 (input agent, additive): unit shown after a slider value ("%", "×"), JSON "unit"; nullptr = none.
    const char* unit = nullptr;
    // M4 (input agent, additive): extra check of the value after the type check (e.g. the binding lists of [input],
    // input/bindings.h ValidatePadBindingValue); false + `error` (without the key name) = configuration error.
    bool (*validate)(std::string_view value, std::string& error) noexcept = nullptr;

    bool Has(ConfigKeyFlag flag) const noexcept { return (flags & flag) != 0; }
};

struct ConfigSectionDef {
    const char* name = "";
    ConfigText label;
};

// Sections in schema order, and all keys (grouped by section, in table order).
std::span<const ConfigSectionDef> ConfigSections() noexcept;
std::span<const ConfigKeyDef> ConfigKeys() noexcept;

// Case-insensitive lookup. An empty section matches the key name in any section (section-less INI keys, the
// rule of core::LoadConfig). nullptr if unknown.
const ConfigKeyDef* FindConfigKey(std::string_view section, std::string_view key) noexcept;
bool IsConfigSection(std::string_view section) noexcept;

// Checks a raw INI / --set value against the key's type, choices and enforced range. On failure `error` names
// "[section] key" and the expected form ("invalid boolean for ...", "invalid number for ...", "invalid value for
// ...", "value out of range for ...", "invalid resolution for ...").
bool ValidateConfigValue(const ConfigKeyDef& key, std::string_view value, std::string& error);

// '|'-joined accepted values of a choice key, visible choices first ("60|50",
// "english|german|french|spanish|italian|japanese|korean|tchinese|portuguese|1..9"); nullptr for other types. The
// pointer stays valid for the process lifetime.
const char* ConfigChoiceList(const ConfigKeyDef& key) noexcept;

// Default value (INI form) of [section] key; "" for unknown keys and keys without default. Readers should use it
// instead of repeating the literal, e.g. cfg.GetString("render", "msaa", core::ConfigKeyDefault("render", "msaa")).
std::string_view ConfigKeyDefault(std::string_view section, std::string_view key) noexcept;

// JSON names ("bool", "path_file", "graphics", "dspin", ...).
const char* ConfigKeyTypeName(ConfigKeyType type) noexcept;
const char* ConfigUiTabName(ConfigUiTab tab) noexcept;
const char* ConfigUiWidgetName(ConfigUiWidget widget) noexcept;

// ---- value parsers (shared by the validation and the readers; blanks around the value are ignored) ----------
bool ParseConfigBool(std::string_view text, bool& out) noexcept;
bool ParseConfigU32(std::string_view text, std::uint32_t& out) noexcept;  // decimal, 0x hex (strtoul base 0)
bool ParseConfigFloat(std::string_view text, double& out) noexcept;       // "1.5", finite, '.' only
// True if `value` is one of the '|'-separated choices (words case-insensitive, "a..b" numeric ranges).
bool MatchesConfigChoice(std::string_view value, std::string_view choices) noexcept;

// [render] resolution: "auto" (case-insensitive) or <width>x<height> ('x' or 'X', blanks around the numbers
// allowed), both kConfigResolutionMin..kConfigResolutionMax (16384 = D3D11 maximum texture size).
inline constexpr std::uint32_t kConfigResolutionMin = 160;
inline constexpr std::uint32_t kConfigResolutionMax = 16384;
struct ConfigResolution {
    bool isAuto = true;
    std::uint32_t width = 0;   // 0 for auto
    std::uint32_t height = 0;
};
bool ParseConfigResolution(std::string_view text, ConfigResolution& out) noexcept;

}  // namespace core
