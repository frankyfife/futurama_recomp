// Configuration loading (INI + command line). See config.h.
#include "core/config.h"

#include "core/config_schema.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace core {
namespace {

Config* g_config = nullptr;
const LauncherInfo* g_launcherInfo = nullptr;
LauncherInfo g_emptyLauncherInfo{};

std::string Lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return r;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string Unquote(std::string_view s) {
    s = Trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return std::string(s);
}

std::string MakeKey(std::string_view section, std::string_view key) {
    return section.empty() ? Lower(key) : Lower(section) + "." + Lower(key);
}

// M3c: the value parsers live with the schema (core/config_schema.h), so the validation and the accessors
// accept exactly the same text.
bool ParseBool(std::string_view text, bool& out) {
    return ParseConfigBool(text, out);
}

bool ParseU32(std::string_view text, std::uint32_t& out) {
    return ParseConfigU32(text, out);
}

std::vector<std::string> SplitList(std::string_view text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        if (comma == std::string_view::npos) {
            comma = text.size();
        }
        std::string item(Trim(text.substr(start, comma - start)));
        if (!item.empty()) {
            out.push_back(std::move(item));
        }
        start = comma + 1;
    }
    return out;
}

// Inline comment rule (review finding 4, same as Python configparser with
// inline_comment_prefixes=(";", "#")): ';' or '#' starts a comment when it is preceded by a space or
// tab and is not inside double quotes. Values that contain " ;" or " #" must be quoted.
std::string_view StripInlineComment(std::string_view line) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && (c == ';' || c == '#') && i > 0 &&
                   (line[i - 1] == ' ' || line[i - 1] == '\t')) {
            return Trim(line.substr(0, i));
        }
    }
    return line;
}

// Every key the host reads (review finding 8) is a row of the schema table (core/config_schema.cpp, M3c; it
// replaced kKnownKeys here). A key that is not listed is a configuration error in a known section (typos such
// as "stop-at" or "timeout = 60s" must not be ignored silently); an unknown section only produces a warning.
// Choice (M2 review finding 10): the value must be one of the '|'-separated words (case-insensitive) or lie in
// a numeric range "a..b" of the list; otherwise the configuration error (exit code 1) is raised here instead of
// a stage failure (exit code 2) when the module parses the key. M3c adds float, resolution and enforced ranges
// (core::ValidateConfigValue).

// Checks every stored "section.key" against the schema and its type. Unknown sections -> warnings.
bool ValidateKeys(const std::map<std::string, std::string, std::less<>>& values,
                  std::vector<std::string>& warnings, std::string& error) {
    for (const auto& [fullKey, value] : values) {
        const size_t dot = fullKey.find('.');
        const std::string_view full(fullKey);
        const std::string_view section = dot == std::string::npos ? std::string_view() : full.substr(0, dot);
        const std::string_view key = dot == std::string::npos ? full : full.substr(dot + 1);
        if (!section.empty() && !IsConfigSection(section)) {
            warnings.push_back("unknown config section [" + std::string(section) + "] (key '" +
                               std::string(key) + "' ignored)");
            continue;
        }
        const ConfigKeyDef* known = FindConfigKey(section, key);
        if (known == nullptr) {
            // Review M3c finding 16 (h): the launcher shows this line to the player - no source reference.
            error = section.empty() ? "unknown config key '" + std::string(key) + "' outside any section"
                                    : "unknown config key '" + std::string(key) + "' in [" +
                                          std::string(section) +
                                          "] - this game build does not know it; remove the line or fix the name";
            return false;
        }
        if (!ValidateConfigValue(*known, known->type == ConfigKeyType::Choice ? Unquote(value) : value, error)) {
            return false;
        }
    }
    return true;
}

std::filesystem::path ResolvePath(const std::string& value, const std::filesystem::path& base) {
    std::filesystem::path p(Utf8ToWide(value));
    if (p.is_relative() && !base.empty()) {
        p = base / p;
    }
    return p.lexically_normal();
}

bool ReadIni(const std::filesystem::path& file, std::map<std::string, std::string, std::less<>>& values,
             std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open config file " + WideToUtf8(file.wstring());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    const std::filesystem::path iniDir = file.parent_path();
    std::string section;
    std::istringstream lines(text);
    std::string rawLine;
    int lineNo = 0;
    while (std::getline(lines, rawLine)) {
        ++lineNo;
        std::string_view line = Trim(rawLine);
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        line = StripInlineComment(line);
        if (line.front() == '[') {
            if (line.back() != ']') {
                error = WideToUtf8(file.wstring()) + ":" + std::to_string(lineNo) + ": bad section header";
                return false;
            }
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            error = WideToUtf8(file.wstring()) + ":" + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        const std::string key = MakeKey(section, Trim(line.substr(0, eq)));
        std::string value = Unquote(line.substr(eq + 1));
        if (section == "paths" && !value.empty()) {
            value = WideToUtf8(ResolvePath(value, iniDir).wstring());
        }
        values[key] = std::move(value);
    }
    return true;
}

}  // namespace

// --- Config accessors ---------------------------------------------------------------------------

bool Config::Has(std::string_view section, std::string_view key) const {
    return values.find(MakeKey(section, key)) != values.end() || values.find(Lower(key)) != values.end();
}

std::string Config::GetString(std::string_view section, std::string_view key, std::string_view def) const {
    auto it = values.find(MakeKey(section, key));
    if (it == values.end()) {
        it = values.find(Lower(key));
    }
    return it != values.end() ? it->second : std::string(def);
}

bool Config::GetBool(std::string_view section, std::string_view key, bool def) const {
    bool v = def;
    if (Has(section, key) && ParseBool(GetString(section, key), v)) {
        return v;
    }
    return def;
}

std::uint32_t Config::GetU32(std::string_view section, std::string_view key, std::uint32_t def) const {
    std::uint32_t v = def;
    if (Has(section, key) && ParseU32(GetString(section, key), v)) {
        return v;
    }
    return def;
}

std::vector<std::string> Config::GetList(std::string_view section, std::string_view key) const {
    return SplitList(GetString(section, key));
}

std::filesystem::path Config::GetPath(std::string_view section, std::string_view key,
                                      const std::filesystem::path& def) const {
    if (!Has(section, key)) {
        return def;
    }
    const std::string v = GetString(section, key);
    if (v.empty()) {
        return def;
    }
    return ResolvePath(v, configFile.empty() ? exeDir : configFile.parent_path());
}

// --- loading ----------------------------------------------------------------------------------

bool LoadConfig(const wchar_t* commandLine, const std::filesystem::path& exePath, Config& out,
                std::string& error) {
    out = Config{};
    out.exePath = exePath;
    out.exeDir = exePath.parent_path();

    // 1. Command line -> overrides (applied after the INI).
    std::map<std::string, std::string, std::less<>> overrides;
    std::filesystem::path explicitConfig;
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    int argc = 0;
    LPWSTR* argv = (commandLine && *commandLine) ? CommandLineToArgvW(commandLine, &argc) : nullptr;
    for (int i = 1; argv != nullptr && i < argc; ++i) {
        const std::string arg = WideToUtf8(argv[i]);
        auto valueOf = [&](std::string_view prefix, std::string& value) {
            if (arg.size() > prefix.size() && arg.compare(0, prefix.size(), prefix) == 0) {
                value = Unquote(std::string_view(arg).substr(prefix.size()));
                return true;
            }
            return false;
        };
        std::string v;
        if (valueOf("--config=", v)) {
            explicitConfig = ResolvePath(v, cwd);
        } else if (valueOf("--userdata=", v)) {
            overrides["paths.userdata"] = WideToUtf8(ResolvePath(v, cwd).wstring());
        } else if (valueOf("--iso=", v)) {
            overrides["paths.iso"] = WideToUtf8(ResolvePath(v, cwd).wstring());
        } else if (valueOf("--xbe=", v)) {
            overrides["paths.xbe"] = WideToUtf8(ResolvePath(v, cwd).wstring());
        } else if (valueOf("--stop-at=", v)) {
            overrides["debug.stop_at"] = v;
        } else if (arg == "--selftest") {
            overrides["debug.selftest"] = "1";
        } else if (valueOf("--trace=", v)) {
            overrides["debug.trace"] = v;
        } else if (valueOf("--frames=", v)) {  // gfx (M2), design 5.4
            overrides["debug.frames"] = v;
        } else if (arg == "--strict") {
            overrides["kernel.strict"] = "1";
        } else if (valueOf("--log-level=", v)) {
            overrides["log.level"] = v;
        } else if (valueOf("--set=", v)) {
            const size_t eq = v.find('=');
            // Key and value as in the INI file: blanks trimmed, the value unquoted (review M3b finding 10: a
            // value " timer" passed the choice check but did not match in the readers).
            const std::string key =
                eq == std::string::npos ? std::string() : Lower(Trim(std::string_view(v).substr(0, eq)));
            if (key.empty()) {
                error = "--set expects <section>.<key>=<value>: " + arg;
                LocalFree(argv);
                return false;
            }
            overrides[key] = Unquote(std::string_view(v).substr(eq + 1));
        } else {
            error = "unknown command line option '" + arg +
                    "' (known: --config= --userdata= --iso= --xbe= --stop-at= --selftest --trace= --strict "
                    "--log-level= --frames= --set=section.key=value)";
            LocalFree(argv);
            return false;
        }
    }
    if (argv != nullptr) {
        LocalFree(argv);
    }

    // 2. INI file (M3c, shared contract item 4): --config, else portable (<exe dir>) or default
    // (%LOCALAPPDATA%\FuturamaPC); see ResolveConfigLocation.
    const ConfigLocation location = ResolveConfigLocation(out.exeDir, explicitConfig);
    out.configMode = location.mode;
    out.portable = location.portable;
    if (location.mode == ConfigLocationMode::Explicit) {
        if (!std::filesystem::exists(explicitConfig, ec)) {
            error = "config file not found: " + WideToUtf8(explicitConfig.wstring());
            return false;
        }
        out.configFile = explicitConfig;
    } else {
        if (!location.configFile.empty() && std::filesystem::is_regular_file(location.configFile, ec)) {
            out.configFile = location.configFile;
        }
        if (location.mode == ConfigLocationMode::Default && !out.exeDir.empty() &&
            std::filesystem::exists(out.exeDir / kConfigFileName, ec)) {
            // Before M3c this was the default INI; keep users from wondering why their settings are gone.
            out.warnings.push_back(WideToUtf8((out.exeDir / kConfigFileName).wstring()) +
                                   " is ignored: the configuration is read from " +
                                   (location.configFile.empty() ? std::string("(none: %LOCALAPPDATA% is not set)")
                                                                : WideToUtf8(location.configFile.wstring())) +
                                   " (put a file " + WideToUtf8(kPortableMarkerName) +
                                   " next to futurama.exe for portable mode, or pass --config=<file>)");
        }
    }
    if (!out.configFile.empty() && !ReadIni(out.configFile, out.values, error)) {
        return false;
    }
    for (auto& [k, v] : overrides) {
        out.values[k] = v;
    }

    // 3. Typed fields.
    out.iso = out.GetPath("paths", "iso");
    out.xbe = out.GetPath("paths", "xbe");
    out.userdata = out.GetPath("paths", "userdata");
    if (out.userdata.empty()) {
        if (location.defaultUserdata.empty()) {
            error = "%LOCALAPPDATA% is not set; configure [paths] userdata (or use portable mode)";
            return false;
        }
        out.userdata = location.defaultUserdata;
    }
    // Review finding 11: the VFS uses "\\?\" paths, but the log, minidumps and std::filesystem users
    // rely on Win32 long path support (launcher manifest longPathAware + LongPathsEnabled = 1).
    // Without it, keep every derived path below MAX_PATH instead of failing later.
    {
        std::error_code absEc;
        const std::filesystem::path abs = std::filesystem::absolute(out.userdata, absEc);
        const size_t length = (absEc ? out.userdata : abs).wstring().size();
        if (!ProcessLongPathsEnabled() && length > kMaxUserdataWithoutLongPaths) {
            error =
                "[paths] userdata is " + std::to_string(length) + " characters long; without Windows long " +
                "path support at most " + std::to_string(kMaxUserdataWithoutLongPaths) +
                " are allowed (logs, minidumps and saves lie up to 57 characters deeper). Choose a shorter " +
                "directory or set HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem LongPathsEnabled = 1";
            return false;
        }
    }
    out.stopAt = Lower(out.GetString("debug", "stop_at"));
    out.selftest = out.GetBool("debug", "selftest", false);
    out.trace = out.GetList("debug", "trace");
    for (auto& t : out.trace) {
        t = Lower(t);
    }
    out.strict = out.GetBool("kernel", "strict", false);
    out.logLevel = Lower(out.GetString("log", "level", "info"));
#ifdef NDEBUG
    constexpr bool kDebugBuild = false;
#else
    constexpr bool kDebugBuild = true;
#endif
    out.logConsole = out.GetBool("log", "console", kDebugBuild);
    out.logDebugger = out.GetBool("log", "debugger", kDebugBuild);
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    const bool hasStderr =
        err != nullptr && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN;
    out.msgbox = out.GetBool("debug", "msgbox", !hasStderr);
    out.hooksDisable = out.GetList("hooks", "disable");

    return ValidateKeys(out.values, out.warnings, error);
}

bool IsKnownConfigKey(std::string_view section, std::string_view key) {
    return FindConfigKey(section, key) != nullptr;
}

const char* ConfigKeyChoices(std::string_view section, std::string_view key) {
    const ConfigKeyDef* k = FindConfigKey(section, key);
    return k != nullptr ? ConfigChoiceList(*k) : nullptr;
}

ConfigLocation ResolveConfigLocation(const std::filesystem::path& exeDir, const std::filesystem::path& explicitConfig) {
    ConfigLocation loc;
    std::error_code ec;
    loc.portable = !exeDir.empty() && std::filesystem::is_regular_file(exeDir / kPortableMarkerName, ec);
    std::filesystem::path appData;
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        appData = std::filesystem::path(buf) / kUserdataFolderName;
    }
    loc.defaultUserdata = loc.portable ? exeDir : appData;
    if (!explicitConfig.empty()) {
        loc.mode = ConfigLocationMode::Explicit;
        loc.configFile = explicitConfig;
    } else if (loc.portable) {
        loc.mode = ConfigLocationMode::Portable;
        loc.configFile = exeDir / kConfigFileName;
    } else {
        loc.mode = ConfigLocationMode::Default;
        loc.configFile = appData.empty() ? std::filesystem::path() : appData / kConfigFileName;
    }
    return loc;
}

const char* ConfigLocationModeName(ConfigLocationMode mode) noexcept {
    switch (mode) {
    case ConfigLocationMode::Default:
        return "default";
    case ConfigLocationMode::Portable:
        return "portable";
    case ConfigLocationMode::Explicit:
        return "explicit";
    }
    return "default";
}

bool ProcessLongPathsEnabled() noexcept {
    // ntdll!RtlAreLongPathsEnabled (Windows 10 1607+): TRUE if the system setting is on AND the
    // process manifest declares longPathAware (src/launcher/futurama.manifest).
    using Fn = BOOLEAN(NTAPI*)();
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto fn =
        ntdll != nullptr ? reinterpret_cast<Fn>(GetProcAddress(ntdll, "RtlAreLongPathsEnabled")) : nullptr;
    return fn != nullptr && fn() != FALSE;
}

const Config& GetConfig() noexcept {
    static const Config empty{};
    return g_config != nullptr ? *g_config : empty;
}

void SetConfig(Config config) {
    if (g_config == nullptr) {
        g_config = new Config(std::move(config));  // intentionally never freed (process lifetime)
    } else {
        *g_config = std::move(config);
    }
}

void SetLauncherInfo(const LauncherInfo* info) noexcept {
    g_launcherInfo = info;
}

const LauncherInfo& GetLauncherInfo() noexcept {
    return g_launcherInfo != nullptr ? *g_launcherInfo : g_emptyLauncherInfo;
}

std::uint32_t TlsSlotIndex(FutTlsSlot slot) noexcept {
    return static_cast<std::uint32_t>(GetLauncherInfo().tlsSlots[slot]);
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

}  // namespace core
