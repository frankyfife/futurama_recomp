// Unit tests of the configuration schema (M3c, core/config_schema.h): completeness (every key the host reads
// has a row, every row is read somewhere, the configuration accepts exactly the rows), metadata quality of the
// rows, value validation, agreement of the defaults with the modules, and the JSON document of
// --print-config-schema.
#include "doctest.h"

#include "cli_json.h"

#include "core/cli_tools.h"
#include "core/config.h"
#include "core/config_schema.h"
#include "gfx/gfx_config.h"
#include "kernel/kernel_settings.h"
#include "xapi/game_timing.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string Lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return s;
}

std::string FullName(const core::ConfigKeyDef& k) {
    return std::string(k.section) + "." + k.name;
}

// The source tree from this file's path (CMake compiles with absolute paths); empty if it is not there.
fs::path SourceRoot() {
    const fs::path here = fs::path(__FILE__);
    std::error_code ec;
    const fs::path root = here.parent_path().parent_path();
    return fs::is_directory(root / "src" / "host" / "core", ec) ? root : fs::path();
}

std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// An empty INI passed with --config (never the player's %LOCALAPPDATA%\FuturamaPC\futurama.ini) and --userdata.
bool LoadWith(const std::wstring& extra, core::Config& cfg, std::string& error) {
    static const fs::path ini = [] {
        const fs::path dir = fs::temp_directory_path() / ("fut_schema_" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(dir);
        std::ofstream(dir / "empty.ini", std::ios::binary) << "; schema tests\n";
        return dir / "empty.ini";
    }();
    const std::wstring cmd = L"futurama.exe \"--config=" + ini.wstring() + L"\" --userdata=C:\\fut_schema_test" + extra;
    return core::LoadConfig(cmd.c_str(), L"C:\\nowhere\\futurama.exe", cfg, error);
}

core::ConfigUiWidget ExpectedWidget(core::ConfigKeyType type) {
    switch (type) {
    case core::ConfigKeyType::Bool:
        return core::ConfigUiWidget::Check;
    case core::ConfigKeyType::Int:
        return core::ConfigUiWidget::Spin;
    case core::ConfigKeyType::Float:
        return core::ConfigUiWidget::DSpin;
    case core::ConfigKeyType::Choice:
        return core::ConfigUiWidget::Combo;
    case core::ConfigKeyType::String:
        return core::ConfigUiWidget::Text;
    case core::ConfigKeyType::PathFile:
        return core::ConfigUiWidget::File;
    case core::ConfigKeyType::PathDir:
        return core::ConfigUiWidget::Dir;
    case core::ConfigKeyType::Resolution:
        return core::ConfigUiWidget::Resolution;
    }
    return core::ConfigUiWidget::Text;
}

}  // namespace

TEST_CASE("schema: every row has complete player-facing metadata") {
    std::set<std::string> names;
    std::map<std::string, std::set<int>> orders;  // tab -> ui.order
    REQUIRE(core::ConfigKeys().size() >= 50u);
    for (const core::ConfigKeyDef& k : core::ConfigKeys()) {
        CAPTURE(FullName(k));
        CHECK(names.insert(FullName(k)).second);  // unique
        CHECK(core::IsConfigSection(k.section));
        CHECK(std::regex_match(std::string(k.name), std::regex("[a-z][a-z0-9_]*")));
        CHECK(std::string(k.label.de).size() > 1);
        CHECK(std::string(k.label.en).size() > 1);
        CHECK(std::string(k.help.de).size() > 20);
        CHECK(std::string(k.help.en).size() > 20);
        // Help texts are sentences; German texts use real umlauts, not ae/oe/ue transcriptions of the usual words.
        CHECK(std::string(k.help.de).back() == '.');
        CHECK(std::string(k.help.en).back() == '.');
        for (const char* ascii : {"Kantenglaettung", "fuer", "Groesse", "Aufloesung", "koennen", "waehrend"}) {
            CHECK(std::string(k.label.de).find(ascii) == std::string::npos);
            CHECK(std::string(k.help.de).find(ascii) == std::string::npos);
        }
        // The only exception: [render] monitor (M3c integration) is a string shown as a monitor list.
        if (FullName(k) == "render.monitor") {
            CHECK(k.type == core::ConfigKeyType::String);
            CHECK(k.widget == core::ConfigUiWidget::Monitor);
        } else if (k.widget == core::ConfigUiWidget::Binding) {
            // M4 (input agent): [input] pad_* / kbm_* binding lists are strings with their own validator.
            CHECK(k.type == core::ConfigKeyType::String);
            CHECK(std::string(k.section) == "input");
            CHECK(k.validate != nullptr);
        } else if (k.widget == core::ConfigUiWidget::AudioDevice) {
            // M5 (tooling/launcher agent): an audio endpoint ID shown as a device list ([audio] device).
            CHECK(k.type == core::ConfigKeyType::String);
            CHECK(std::string(k.section) == "audio");
        } else if (k.widget == core::ConfigUiWidget::Color || k.widget == core::ConfigUiWidget::Fraction) {
            // G1: "original" or RRGGBB shown as a colour button ([render] outline_color), with a validator.
            CHECK(k.type == core::ConfigKeyType::String);
            CHECK(k.validate != nullptr);
        } else if (k.widget == core::ConfigUiWidget::Slider) {
            // M4 (input agent): sliders are ranged int/float keys.
            CHECK((k.type == core::ConfigKeyType::Int || k.type == core::ConfigKeyType::Float));
            CHECK(k.ranged);
        } else {
            CHECK(k.widget == ExpectedWidget(k.type));
        }
        CHECK(orders[core::ConfigUiTabName(k.tab)].insert(k.order).second);  // unique within its tab
        if (k.Has(core::kConfigDeveloper)) {
            CHECK(k.Has(core::kConfigAdvanced));
        }
        // The default is a valid value (empty = "not set" for text and path keys).
        if (std::string(k.defaultValue).empty()) {
            CHECK((k.type == core::ConfigKeyType::String || k.type == core::ConfigKeyType::PathFile ||
                   k.type == core::ConfigKeyType::PathDir));
        } else {
            std::string error;
            CHECK_MESSAGE(core::ValidateConfigValue(k, k.defaultValue, error), error);
        }
        if (k.type == core::ConfigKeyType::Choice || k.type == core::ConfigKeyType::Resolution) {
            REQUIRE_FALSE(k.choices.empty());
            bool defaultVisible = false;
            for (const core::ConfigChoice& c : k.choices) {
                CAPTURE(c.value);
                CHECK(std::string(c.label.de).size() > 0);
                CHECK(std::string(c.label.en).size() > 0);
                if (std::string(c.value).find("..") != std::string::npos) {
                    CHECK(c.hidden);  // ranges are never offered in the UI
                }
                defaultVisible = defaultVisible || (!c.hidden && std::string(c.value) == k.defaultValue);
                std::string error;
                CHECK_MESSAGE((c.hidden || core::ValidateConfigValue(k, c.value, error)), error);
            }
            CHECK(defaultVisible);
        } else {
            CHECK(k.choices.empty());
        }
        if (k.type == core::ConfigKeyType::Int || k.type == core::ConfigKeyType::Float) {
            if (k.ranged) {
                CHECK(k.min <= k.max);
                CHECK(k.step > 0.0);
                double d = 0.0;
                std::uint32_t u = 0;
                const bool parsed = k.type == core::ConfigKeyType::Float ? core::ParseConfigFloat(k.defaultValue, d)
                                                                          : core::ParseConfigU32(k.defaultValue, u);
                REQUIRE(parsed);
                const double v = k.type == core::ConfigKeyType::Float ? d : static_cast<double>(u);
                CHECK(v >= k.min);
                CHECK(v <= k.max);
            }
        }
        if (k.fileFilter != nullptr) {
            CHECK(k.type == core::ConfigKeyType::PathFile);
        }
    }
    // Every section has at least one key.
    for (const core::ConfigSectionDef& s : core::ConfigSections()) {
        CAPTURE(s.name);
        CHECK(std::any_of(core::ConfigKeys().begin(), core::ConfigKeys().end(),
                          [&](const core::ConfigKeyDef& k) { return std::string(k.section) == s.name; }));
        CHECK(std::string(s.label.de).size() > 1);
        CHECK(std::string(s.label.en).size() > 1);
    }
}

TEST_CASE("schema: the M3c [render] keys of the shared contract (item 5)") {
    const core::ConfigKeyDef* resolution = core::FindConfigKey("render", "resolution");
    const core::ConfigKeyDef* scale = core::FindConfigKey("render", "render_scale");
    const core::ConfigKeyDef* msaa = core::FindConfigKey("render", "msaa");
    const core::ConfigKeyDef* a2c = core::FindConfigKey("render", "alpha_to_coverage");
    const core::ConfigKeyDef* aniso = core::FindConfigKey("render", "anisotropy");
    REQUIRE(resolution != nullptr);
    REQUIRE(scale != nullptr);
    REQUIRE(msaa != nullptr);
    REQUIRE(a2c != nullptr);
    REQUIRE(aniso != nullptr);
    CHECK(resolution->type == core::ConfigKeyType::Resolution);
    CHECK(std::string(resolution->defaultValue) == "auto");
    CHECK(scale->type == core::ConfigKeyType::Float);
    CHECK(scale->min == 1.0);
    CHECK(scale->max == 2.0);
    CHECK(scale->step == 0.25);
    CHECK(std::string(core::ConfigChoiceList(*msaa)) == "off|2|4|8");
    CHECK(std::string(core::ConfigChoiceList(*aniso)) == "off|2|4|8|16");
    CHECK(std::string(aniso->defaultValue) == "16");
    CHECK(a2c->type == core::ConfigKeyType::Bool);
    bool on = false;
    CHECK(core::ParseConfigBool(a2c->defaultValue, on));
    CHECK(on);
    CHECK(std::string(msaa->label.de) == "Kantenglättung (MSAA)");
    for (const core::ConfigKeyDef* k : {resolution, scale, msaa, a2c, aniso}) {
        CHECK(k->tab == core::ConfigUiTab::Graphics);
        CHECK_FALSE(k->Has(core::kConfigAdvanced));
    }
    // "Classic" (M3 regression vs xemu) is expressible.
    core::Config cfg;
    std::string error;
    CHECK_MESSAGE(LoadWith(L" --set=render.resolution=640x480 --set=render.render_scale=1 --set=render.msaa=off "
                           L"--set=render.anisotropy=off --set=render.alpha_to_coverage=off",
                           cfg, error),
                  error);
    // The renderer's parsers and defaults agree with the schema (gfx/gfx_config.h).
    const gfx::Settings defaults = gfx::SettingsFromConfig(core::Config{});
    CHECK(defaults.resolutionWidth == 0u);
    CHECK(defaults.renderScale == 1.0f);
    CHECK(defaults.msaa == 4u);
    CHECK(defaults.alphaToCoverage);
    CHECK(defaults.anisotropy == 16u);
    for (const core::ConfigChoice& c : msaa->choices) {
        std::uint32_t samples = 0;
        CHECK(gfx::ParseMsaa(c.value, &samples));
    }
    for (const core::ConfigChoice& c : aniso->choices) {
        std::uint32_t level = 0;
        CHECK(gfx::ParseAnisotropy(c.value, &level));
    }
}

TEST_CASE("schema: the configuration accepts exactly the schema keys") {
    for (const core::ConfigKeyDef& k : core::ConfigKeys()) {
        CAPTURE(FullName(k));
        CHECK(core::IsKnownConfigKey(k.section, k.name));
        CHECK(core::IsKnownConfigKey(Lower(k.section), std::string(k.name)));
        CHECK(core::FindConfigKey(k.section, k.name) == &k);
        // A plausible value for every key passes the whole configuration loader.
        std::string value = k.defaultValue;
        if (value.empty()) {
            value = k.type == core::ConfigKeyType::PathFile || k.type == core::ConfigKeyType::PathDir ? "C:\\x" : "x";
        }
        if (std::string(k.section) == "saves" && std::string(k.name) == "key_mode") {
            value = "fixed";
        }
        if (std::string(k.section) == "saves" && std::string(k.name) != "key_mode") {
            value = "00112233445566778899aabbccddeeff";
        }
        if (std::string(k.section) == "paths" && std::string(k.name) == "userdata") {
            value = "C:\\fut_schema_test";
        }
        core::Config cfg;
        std::string error;
        const std::wstring set =
            L" \"--set=" + core::Utf8ToWide(FullName(k)) + L"=" + core::Utf8ToWide(value) + L"\"";
        CHECK_MESSAGE(LoadWith(set, cfg, error), error);
        // ConfigKeyChoices (config.h) = the schema's choice list.
        const char* list = core::ConfigKeyChoices(k.section, k.name);
        if (k.type == core::ConfigKeyType::Choice) {
            REQUIRE(list != nullptr);
            CHECK(std::string(list) == core::ConfigChoiceList(k));
            CHECK(std::string(core::ConfigKeyDefault(k.section, k.name)) == k.defaultValue);
        } else {
            CHECK(list == nullptr);
        }
    }
    core::Config cfg;
    std::string error;
    CHECK_FALSE(LoadWith(L" --set=render.msaa_level=4", cfg, error));
    CHECK(error.find("unknown config key 'msaa_level'") != std::string::npos);
    CHECK_FALSE(core::IsKnownConfigKey("render", "display_index"));
    CHECK(core::IsKnownConfigKey("render", "monitor"));  // M3c integration
    CHECK(core::ConfigKeyDefault("render", "monitor") == "primary");
    CHECK(core::ConfigKeyDefault("render", "nonexistent").empty());
    CHECK(core::ConfigKeyDefault("", "msaa").empty());  // section-less lookups have no default
}

TEST_CASE("schema: every key the host sources read has a row, and every row is read") {
    const fs::path root = SourceRoot();
    if (root.empty()) {
        MESSAGE("skipped: source tree not found next to " << __FILE__);
        return;
    }
    std::string sections;
    for (const core::ConfigSectionDef& s : core::ConfigSections()) {
        sections += (sections.empty() ? "" : "|") + std::string(s.name);
    }
    // cfg.GetU32("render", "frame_rate", ...), GetU32Value(config, "settings", "game_region", ...), ...
    const std::regex pair("\"(" + sections + ")\",\\s*\"([a-z0-9_]+)\"");
    std::set<std::string> read;
    std::string allSources;
    for (const auto& entry : fs::recursive_directory_iterator(root / "src" / "host")) {
        const fs::path& p = entry.path();
        if (!entry.is_regular_file() || (p.extension() != ".cpp" && p.extension() != ".h")) {
            continue;
        }
        const std::string name = p.filename().string();
        if (name.rfind("config_schema", 0) == 0 || name.rfind("cli_tools", 0) == 0) {
            continue;  // the table itself and the JSON writer
        }
        const std::string text = ReadText(p);
        allSources += text;
        for (std::sregex_iterator it(text.begin(), text.end(), pair), end; it != end; ++it) {
            const std::string key = (*it)[1].str() + "." + (*it)[2].str();
            CAPTURE(p.string());
            CAPTURE(key);
            CHECK_MESSAGE(core::FindConfigKey((*it)[1].str(), (*it)[2].str()) != nullptr,
                          "a module reads a key without a schema row: add it to core/config_schema.cpp");
            read.insert(key);
        }
    }
    REQUIRE(read.size() > 30u);
    // Reverse: a row nobody reads would be a setting without effect. Keys read through a variable
    // (gfx_config.cpp FrameList(cfg, "screenshot_frames"), ParseOrWarn(cfg, "msaa", ...)) appear as their quoted
    // name at least.
    for (const core::ConfigKeyDef& k : core::ConfigKeys()) {
        CAPTURE(FullName(k));
        const bool asPair = read.count(FullName(k)) != 0;
        const bool asName = allSources.find("\"" + std::string(k.name) + "\"") != std::string::npos;
        CHECK_MESSAGE((asPair || asName), "schema row without a reader in src/host");
    }
}

TEST_CASE("schema: value validation by type") {
    const core::ConfigKeyDef& vsync = *core::FindConfigKey("render", "vsync");
    const core::ConfigKeyDef& timeout = *core::FindConfigKey("debug", "timeout");
    const core::ConfigKeyDef& scale = *core::FindConfigKey("render", "render_scale");
    const core::ConfigKeyDef& resolution = *core::FindConfigKey("render", "resolution");
    const core::ConfigKeyDef& language = *core::FindConfigKey("settings", "language");
    const core::ConfigKeyDef& stack = *core::FindConfigKey("kernel", "min_guest_stack");
    const core::ConfigKeyDef& windowScale = *core::FindConfigKey("render", "window_scale");
    std::string e;
    for (const char* good : {"1", "0", "TRUE", "off", " yes "}) {
        CHECK(core::ValidateConfigValue(vsync, good, e));
    }
    CHECK_FALSE(core::ValidateConfigValue(vsync, "maybe", e));
    CHECK(e.find("invalid boolean for [render] vsync") != std::string::npos);
    CHECK(core::ValidateConfigValue(timeout, "0x3C", e));
    CHECK(core::ValidateConfigValue(timeout, "100000", e));  // range is a UI hint (kConfigRangeIsHint)
    CHECK_FALSE(core::ValidateConfigValue(timeout, "60s", e));
    CHECK(e.find("invalid number for [debug] timeout") != std::string::npos);
    CHECK_FALSE(core::ValidateConfigValue(timeout, "-1", e));
    // Enforced ranges: the kernel rejects such a stack anyway (exit 2 before M3c, now exit 1).
    CHECK(core::ValidateConfigValue(stack, "0x400000", e));
    CHECK_FALSE(core::ValidateConfigValue(stack, "0x1000", e));
    CHECK(e.find("value out of range for [kernel] min_guest_stack") != std::string::npos);
    CHECK(core::ValidateConfigValue(windowScale, "12", e));  // gfx warns and uses 2 (hint only)
    for (const char* good : {"1", "1.0", "1.25", "1.5", "2", "2.0", " 1.75 "}) {
        CAPTURE(good);
        CHECK(core::ValidateConfigValue(scale, good, e));
    }
    for (const char* bad : {"0.5", "2.25", "1,5", "abc", "", "nan", "inf", "1e0", "+1.5"}) {
        CAPTURE(bad);
        CHECK_FALSE(core::ValidateConfigValue(scale, bad, e));
    }
    for (const char* good : {"auto", "AUTO", "640x480", "1920X1440", " 2880 x 2160 ", "160x160", "16384x16384"}) {
        CAPTURE(good);
        CHECK(core::ValidateConfigValue(resolution, good, e));
    }
    for (const char* bad : {"", "640", "640x", "x480", "159x480", "640x16385", "640*480", "-640x480", "1e3x480"}) {
        CAPTURE(bad);
        CHECK_FALSE(core::ValidateConfigValue(resolution, bad, e));
        CHECK(e.find("invalid resolution for [render] resolution") != std::string::npos);
    }
    core::ConfigResolution r;
    REQUIRE(core::ParseConfigResolution("1920x1440", r));
    CHECK_FALSE(r.isAuto);
    CHECK(r.width == 1920u);
    CHECK(r.height == 1440u);
    REQUIRE(core::ParseConfigResolution("Auto", r));
    CHECK(r.isAuto);
    CHECK(r.width == 0u);
    // Choice: visible + hidden values and the range of the language key.
    for (const char* good : {"english", "German", "tchinese", "1", "9"}) {
        CAPTURE(good);
        CHECK(core::ValidateConfigValue(language, good, e));
    }
    CHECK_FALSE(core::ValidateConfigValue(language, "10", e));
    CHECK(e.find("invalid value for [settings] language") != std::string::npos);
    CHECK(std::string(core::ConfigChoiceList(language)) ==
          "english|german|french|spanish|italian|japanese|korean|tchinese|portuguese|1..9");
    // log.level accepts log.cpp's alias "warning" (hidden).
    CHECK(core::ValidateConfigValue(*core::FindConfigKey("log", "level"), "warning", e));
    CHECK_FALSE(core::ValidateConfigValue(*core::FindConfigKey("log", "level"), "verbose", e));
}

TEST_CASE("schema: defaults agree with the modules that read the keys") {
    auto def = [](const char* section, const char* key) {
        return std::string(core::ConfigKeyDefault(section, key));
    };
    auto u32 = [&](const char* section, const char* key) {
        std::uint32_t v = 0;
        REQUIRE(core::ParseConfigU32(def(section, key), v));
        return v;
    };
    auto boolean = [&](const char* section, const char* key) {
        bool v = false;
        REQUIRE(core::ParseConfigBool(def(section, key), v));
        return v;
    };
    // gfx (gfx/gfx_config.h Settings)
    const gfx::Settings g = gfx::SettingsFromConfig(core::Config{});
    CHECK(u32("render", "window_scale") == g.windowScale);
    CHECK(boolean("render", "vsync") == g.vsync);
    CHECK(boolean("render", "fullscreen") == g.fullscreen);
    CHECK(boolean("render", "load_textures") == g.loadTextures);
    CHECK(boolean("render", "validate") == g.validate);
    CHECK(u32("render", "frame_rate") == g.frameRate);
    CHECK(def("render", "backend") == "d3d11");
    CHECK(g.backend == gfx::RenderBackend::D3D11);
    CHECK(def("render", "scaling") == "linear");
    CHECK(g.scaling == gfx::rhi::PresentScaling::Linear);
    CHECK(def("render", "pacing") == "auto");
    CHECK(g.pacing == platform::pacing::Mode::Auto);
    CHECK(def("render", "d3d_traps") == "abort");
    CHECK(g.trapMode == gfx::TrapMode::Abort);
    CHECK(def("render", "adapter") == "hardware");
    CHECK_FALSE(g.softwareAdapter);
    // kernel (kernel/kernel_settings.h KernelSettings)
    const krnl::KernelSettings ks{};
    CHECK(u32("settings", "game_region") == ks.gameRegion);
    CHECK(u32("settings", "av_region") == ks.avRegion);
    CHECK(u32("settings", "video_flags") == ks.videoFlags);
    CHECK(u32("settings", "audio_flags") == ks.audioFlags);
    CHECK(u32("settings", "av_pack") == ks.avPack);
    CHECK(u32("kernel", "min_guest_stack") == ks.minGuestStack);
    CHECK(std::string(krnl::LanguageName(ks.language)) == def("settings", "language"));
    CHECK(def("saves", "key_mode") == "fixed");
    // timing (xapi/game_timing.h)
    CHECK(u32("game", "tick_rate") == xapi::kDefaultTickRate);
    CHECK(def("game", "frame_clock") == "lockstep");
    // core (core/config.cpp, core/crash.cpp, kernel/mm_kernel_stack.cpp)
    core::Config cfg;
    std::string error;
    REQUIRE_MESSAGE(LoadWith(L"", cfg, error), error);
    CHECK(def("log", "level") == cfg.logLevel);
    CHECK(boolean("log", "console") == cfg.logConsole);
    CHECK(boolean("log", "debugger") == cfg.logDebugger);
    CHECK(boolean("debug", "selftest") == cfg.selftest);
    CHECK(boolean("kernel", "strict") == cfg.strict);
    CHECK(boolean("debug", "minidump"));
    CHECK(boolean("debug", "mmio_trap"));
    CHECK(u32("kernel", "fiber_stack") == 0x80000u);
}

TEST_CASE("schema: --print-config-schema JSON follows the shared contract (item 1)") {
    const std::string text = core::cli::ConfigSchemaJson(L"C:\\nowhere\\futurama.exe");
    testjson::Value doc;
    std::string error;
    REQUIRE_MESSAGE(testjson::Parse(text, doc, error), error);
    REQUIRE(doc.IsObject());
    CHECK(doc["schema_version"].IsInteger());
    CHECK(doc["schema_version"].number == 1.0);
    CHECK(doc["build"].IsString());
    CHECK_FALSE(doc["build"].text.empty());
    CHECK(doc["config"]["file_name"].text == "futurama.ini");
    CHECK(doc["config"]["portable_marker"].text == "portable.txt");
    CHECK(doc["config"]["mode"].text == "default");
    REQUIRE(doc["sections"].IsArray());
    std::set<std::string> seen;
    for (const testjson::Value& section : doc["sections"].items) {
        REQUIRE(section["name"].IsString());
        CHECK(core::IsConfigSection(section["name"].text));
        CHECK(section["label"]["de"].IsString());
        CHECK(section["label"]["en"].IsString());
        REQUIRE(section["keys"].IsArray());
        for (const testjson::Value& key : section["keys"].items) {
            const std::string full = section["name"].text + "." + key["name"].text;
            CAPTURE(full);
            CHECK(seen.insert(full).second);
            const core::ConfigKeyDef* k = core::FindConfigKey(section["name"].text, key["name"].text);
            REQUIRE(k != nullptr);
            CHECK(key["type"].text == core::ConfigKeyTypeName(k->type));
            static const std::set<std::string> kTypes = {"choice", "bool",     "int",      "float",
                                                         "string", "path_file", "path_dir", "resolution"};
            CHECK(kTypes.count(key["type"].text) == 1);
            // default typed by the key type
            const testjson::Value& d = key["default"];
            switch (k->type) {
            case core::ConfigKeyType::Bool:
                CHECK(d.IsBool());
                break;
            case core::ConfigKeyType::Int:
                CHECK(d.IsInteger());
                break;
            case core::ConfigKeyType::Float:
                CHECK(d.IsNumber());
                break;
            default:
                CHECK(d.IsString());
                CHECK(d.text == k->defaultValue);
            }
            for (const char* lang : {"de", "en"}) {
                CHECK(key["label"][lang].IsString());
                CHECK_FALSE(key["label"][lang].text.empty());
                CHECK(key["help"][lang].IsString());
            }
            CHECK(key["advanced"].IsBool());
            CHECK(key["restart_required"].IsBool());
            CHECK(key["developer"].IsBool());
            CHECK(key["dynamic_default"].IsBool());
            // M4 (input agent): tab "controls", widgets "binding" and "slider" (src/INTERFACES.md 4.26).
            // M5 (tooling/launcher agent): tab "audio", widget "audio_device" (src/INTERFACES.md 4.29).
            static const std::set<std::string> kTabs = {"game", "graphics", "paths", "advanced", "controls", "audio"};
            static const std::set<std::string> kWidgets = {"combo",   "check",   "spin",   "dspin",
                                                           "text",    "file",    "dir",    "resolution",
                                                           "monitor", "binding", "slider", "audio_device",
                                                           "color",   "fraction"};
            if (k->unit != nullptr) {
                CHECK(key["unit"].text == k->unit);
            }
            CHECK(kTabs.count(key["ui"]["tab"].text) == 1);
            CHECK(key["ui"]["order"].IsInteger());
            CHECK(kWidgets.count(key["ui"]["widget"].text) == 1);
            if (k->type == core::ConfigKeyType::Choice) {
                REQUIRE(key["choices"].IsArray());
                std::vector<std::string> values;
                for (const testjson::Value& c : key["choices"].items) {
                    CHECK(c["value"].IsString());
                    CHECK(c["label"]["de"].IsString());
                    CHECK(c["label"]["en"].IsString());
                    values.push_back(c["value"].text);
                }
                for (const testjson::Value& h : key["hidden_choices"].items) {
                    values.push_back(h.text);
                }
                // visible + hidden = everything the validation accepts
                std::string joined;
                for (const std::string& v : values) {
                    joined += (joined.empty() ? "" : "|") + v;
                }
                CHECK(joined == core::ConfigChoiceList(*k));
            }
            if (k->ranged) {
                CHECK(key["min"].IsNumber());
                CHECK(key["max"].IsNumber());
                CHECK(key["step"].IsNumber());
                CHECK(key["range_enforced"].IsBool());
            } else {
                CHECK_FALSE(key.Has("min"));
            }
        }
    }
    CHECK(seen.size() == core::ConfigKeys().size());
    // Spot checks of the contract example and item 5.
    const testjson::Value* render = nullptr;
    for (const testjson::Value& s : doc["sections"].items) {
        if (s["name"].text == "render") {
            render = &s;
        }
    }
    REQUIRE(render != nullptr);
    std::map<std::string, const testjson::Value*> keys;
    for (const testjson::Value& k : (*render)["keys"].items) {
        keys[k["name"].text] = &k;
    }
    REQUIRE(keys.count("msaa") == 1);
    const testjson::Value& msaa = *keys["msaa"];
    CHECK(msaa["type"].text == "choice");
    CHECK(msaa["choices"][0]["value"].text == "off");
    CHECK(msaa["choices"][0]["label"]["de"].text == "Aus");
    CHECK(msaa["choices"][0]["label"]["en"].text == "Off");
    CHECK(msaa["label"]["de"].text == "Kantenglättung (MSAA)");
    CHECK(msaa["ui"]["tab"].text == "graphics");
    CHECK(msaa["ui"]["widget"].text == "combo");
    CHECK(msaa["restart_required"].boolean);
    const testjson::Value& scale = *keys["render_scale"];
    CHECK(scale["type"].text == "float");
    CHECK(scale["default"].number == 1.0);
    CHECK(scale["min"].number == 1.0);
    CHECK(scale["max"].number == 2.0);
    CHECK(scale["step"].number == 0.25);
    CHECK(scale["ui"]["widget"].text == "dspin");
    CHECK((*keys["resolution"])["default"].text == "auto");
    CHECK((*keys["resolution"])["ui"]["widget"].text == "resolution");
    CHECK((*keys["alpha_to_coverage"])["default"].boolean);
    CHECK((*keys["anisotropy"])["default"].text == "16");
}
