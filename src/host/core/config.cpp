// Configuration loading (INI + command line). See config.h.
#include "core/config.h"

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

bool ParseBool(std::string_view text, bool& out) {
    const std::string v = Lower(Trim(text));
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        out = false;
        return true;
    }
    return false;
}

bool ParseU32(std::string_view text, std::uint32_t& out) {
    const std::string v(Trim(text));
    if (v.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long n = std::strtoull(v.c_str(), &end, 0);
    if (end == nullptr || *end != 0 || n > 0xFFFFFFFFull) {
        return false;
    }
    out = static_cast<std::uint32_t>(n);
    return true;
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

// Every key the host reads (review finding 8). A module that reads a new key adds it here. A key
// that is not listed is a configuration error in a known section (typos such as "stop-at" or
// "timeout = 60s" must not be ignored silently); an unknown section only produces a warning.
// Choice (M2 review finding 10): the value must be one of the '|'-separated words (case-insensitive) or
// lie in a numeric range "a..b" of the list; otherwise the configuration error (exit code 1) is raised
// here instead of a stage failure (exit code 2) when the module parses the key.
enum class KeyType : std::uint8_t { String, Bool, U32, List, Path, Choice };
struct KnownKey {
    const char* section;
    const char* key;
    KeyType type;
    const char* choices = nullptr;  // KeyType::Choice only
};
constexpr KnownKey kKnownKeys[] = {
    {"paths", "iso", KeyType::Path},
    {"paths", "xbe", KeyType::Path},
    {"paths", "userdata", KeyType::Path},
    {"paths", "disc_dir", KeyType::Path},
    {"debug", "stop_at", KeyType::String},
    {"debug", "selftest", KeyType::Bool},
    {"debug", "trace", KeyType::List},
    {"debug", "msgbox", KeyType::Bool},
    {"debug", "timeout", KeyType::U32},
    {"debug", "crash_test", KeyType::Bool},
    {"debug", "exit_hang_test", KeyType::Bool},  // M2 review finding 7: an exit reporter that never returns
    {"debug", "minidump", KeyType::Bool},
    {"debug", "mmio_trap", KeyType::Bool},
    {"debug", "frames", KeyType::U32},  // gfx (M2): end orderly after N displayed frames, 0 = off
    {"kernel", "strict", KeyType::Bool},
    {"kernel", "min_guest_stack", KeyType::U32},
    {"kernel", "fiber_stack", KeyType::U32},  // fiber (M2): MmCreateKernelStack minimum, default 0x80000
    {"log", "level", KeyType::String},
    {"log", "console", KeyType::Bool},
    {"log", "debugger", KeyType::Bool},
    {"hooks", "disable", KeyType::List},
    // same names as kLanguages in kernel/kernel_settings.cpp (unit test "config: choice keys ...")
    {"settings", "language", KeyType::Choice,
     "english|japanese|german|french|spanish|italian|korean|tchinese|portuguese|1..9"},
    {"settings", "game_region", KeyType::U32},
    {"settings", "av_region", KeyType::U32},
    {"settings", "video_flags", KeyType::U32},
    {"settings", "audio_flags", KeyType::U32},
    {"settings", "av_pack", KeyType::U32},
    {"saves", "key_mode", KeyType::Choice, "fixed|xbox"},
    {"saves", "signature_key", KeyType::String},
    {"saves", "xbox_cert_key", KeyType::String},
    // gfx (M2, src/host/gfx/gfx_config.h): [render] = null renderer and D3D shim/traps
    {"render", "frame_rate", KeyType::U32},       // DisplayFrame pacing in Hz, 0 = unthrottled (default 50)
    {"render", "d3d_traps", KeyType::Choice, "abort|log"},  // default abort
    {"render", "load_textures", KeyType::Bool},   // slot 65 reads texture files for their size (default 1)
    // xdk (M2, src/host/audio/dsound_stubs.h): DirectSoundCreateStream returns stub streams instead of NULL
    {"audio", "dummy_streams", KeyType::Bool},  // default 0
};

// True if `value` is one of the '|'-separated choices (words case-insensitive, "a..b" numeric ranges).
bool MatchesChoice(std::string_view value, std::string_view choices) {
    const std::string v = Lower(Trim(value));
    std::uint32_t number = 0;
    const bool isNumber = ParseU32(v, number);
    std::size_t start = 0;
    while (start <= choices.size()) {
        std::size_t bar = choices.find('|', start);
        if (bar == std::string_view::npos) {
            bar = choices.size();
        }
        const std::string_view choice = choices.substr(start, bar - start);
        const std::size_t dots = choice.find("..");
        if (dots != std::string_view::npos) {
            std::uint32_t lo = 0, hi = 0;
            if (isNumber && ParseU32(choice.substr(0, dots), lo) && ParseU32(choice.substr(dots + 2), hi) &&
                number >= lo && number <= hi) {
                return true;
            }
        } else if (v == choice) {
            return true;
        }
        start = bar + 1;
    }
    return false;
}

bool IsKnownSection(std::string_view section) {
    for (const KnownKey& k : kKnownKeys) {
        if (section == k.section) {
            return true;
        }
    }
    return false;
}

// Section-less keys (outside any [section]) match the key name in any section.
const KnownKey* FindKnownKey(std::string_view section, std::string_view key) {
    for (const KnownKey& k : kKnownKeys) {
        if (key == k.key && (section.empty() || section == k.section)) {
            return &k;
        }
    }
    return nullptr;
}

// Checks every stored "section.key" against kKnownKeys and its type. Unknown sections -> warnings.
bool ValidateKeys(const std::map<std::string, std::string, std::less<>>& values,
                  std::vector<std::string>& warnings, std::string& error) {
    for (const auto& [fullKey, value] : values) {
        const size_t dot = fullKey.find('.');
        const std::string_view full(fullKey);
        const std::string_view section = dot == std::string::npos ? std::string_view() : full.substr(0, dot);
        const std::string_view key = dot == std::string::npos ? full : full.substr(dot + 1);
        if (!section.empty() && !IsKnownSection(section)) {
            warnings.push_back("unknown config section [" + std::string(section) + "] (key '" +
                               std::string(key) + "' ignored)");
            continue;
        }
        const KnownKey* known = FindKnownKey(section, key);
        if (known == nullptr) {
            error = section.empty() ? "unknown config key '" + std::string(key) + "' outside any section"
                                    : "unknown config key '" + std::string(key) + "' in [" +
                                          std::string(section) + "] (known keys: see core/config.h)";
            return false;
        }
        const std::string where = "[" + std::string(known->section) + "] " + known->key;
        bool b = false;
        std::uint32_t u = 0;
        if (known->type == KeyType::Bool && !ParseBool(value, b)) {
            error = "invalid boolean for " + where + ": '" + value +
                    "' (expected 0/1, true/false, yes/no, on/off)";
            return false;
        }
        if (known->type == KeyType::U32 && !ParseU32(value, u)) {
            error = "invalid number for " + where + ": '" + value + "' (expected decimal or 0x hex, no unit)";
            return false;
        }
        if (known->type == KeyType::Choice && known->choices != nullptr &&
            !MatchesChoice(Unquote(value), known->choices)) {
            error = "invalid value for " + where + ": '" + value + "' (expected " + known->choices + ")";
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
            if (eq == std::string::npos || eq == 0) {
                error = "--set expects <section>.<key>=<value>: " + arg;
                LocalFree(argv);
                return false;
            }
            overrides[Lower(v.substr(0, eq))] = v.substr(eq + 1);
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

    // 2. INI file.
    if (!explicitConfig.empty()) {
        if (!std::filesystem::exists(explicitConfig, ec)) {
            error = "config file not found: " + WideToUtf8(explicitConfig.wstring());
            return false;
        }
        out.configFile = explicitConfig;
    } else {
        const std::filesystem::path def = out.exeDir / L"futurama.ini";
        if (std::filesystem::exists(def, ec)) {
            out.configFile = def;
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
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
        if (n == 0 || n >= std::size(buf)) {
            error = "%LOCALAPPDATA% is not set; configure [paths] userdata";
            return false;
        }
        out.userdata = std::filesystem::path(buf) / L"FuturamaPC";
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
    return FindKnownKey(Lower(section), Lower(key)) != nullptr;
}

const char* ConfigKeyChoices(std::string_view section, std::string_view key) {
    const KnownKey* k = FindKnownKey(Lower(section), Lower(key));
    return k != nullptr && k->type == KeyType::Choice ? k->choices : nullptr;
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
