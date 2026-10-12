// Tool modes of futurama.exe (see cli_tools.h).
#include "core/cli_tools.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "core/guest.h"
#include "kernel/xc_crypto.h"
#include "platform/pacer_display.h"
#include "shared/xbe_format.h"
#include "vfs/vfs.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <utility>

namespace core::cli {
namespace {

using Microsoft::WRL::ComPtr;

static_assert(sizeof(guest::kXbeSha1) == sizeof(kProtoNtscXbeSha1), "SHA-1 hex strings have 40 digits");

std::string Utf8(const std::filesystem::path& p) {
    return WideToUtf8(p.wstring());
}

// Error line of a tool: stderr (pipe/console/file) + OutputDebugString, never a message box (the launcher runs
// the tools hidden).
void ReportToolError(const std::string& text) noexcept {
    try {
        const std::string line = "futurama: " + text + "\n";
        OutputDebugStringA(line.c_str());
        const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (err != nullptr && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) {
            DWORD written = 0;
            WriteFile(err, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        }
    } catch (...) {
    }
}

bool WriteAll(HANDLE h, const std::string& text) noexcept {
    const char* data = text.data();
    std::size_t size = text.size();
    while (size > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
        if (!WriteFile(h, data, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

std::string Win32ErrorText(DWORD code) {
    return "Win32 error " + std::to_string(code);
}

// Writes `text` to `target` ("-" = stdout) via "<target>.tmp" + rename, so a reader never sees a partial file.
bool WriteOutput(const std::wstring& target, const std::string& text, std::string& error) {
    if (target == L"-") {
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == nullptr || out == INVALID_HANDLE_VALUE || GetFileType(out) == FILE_TYPE_UNKNOWN) {
            error = "no standard output (pass a file name instead of '-')";
            return false;
        }
        if (!WriteAll(out, text)) {
            error = "writing to standard output failed (" + Win32ErrorText(GetLastError()) + ")";
            return false;
        }
        return true;
    }
    std::error_code ec;
    std::filesystem::path file = std::filesystem::absolute(std::filesystem::path(target), ec);
    if (ec) {
        file = std::filesystem::path(target);
    }
    const std::filesystem::path tmp = std::filesystem::path(file.wstring() + L".tmp");
    const HANDLE h =
        CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "cannot create " + Utf8(tmp) + " (" + Win32ErrorText(GetLastError()) + ")";
        return false;
    }
    const bool written = WriteAll(h, text);
    const DWORD writeError = GetLastError();
    CloseHandle(h);
    if (!written) {
        DeleteFileW(tmp.c_str());
        error = "cannot write " + Utf8(tmp) + " (" + Win32ErrorText(writeError) + ")";
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        DeleteFileW(tmp.c_str());
        error = "cannot replace " + Utf8(file) + " (" + Win32ErrorText(moveError) + ")";
        return false;
    }
    return true;
}

void WriteText(JsonWriter& w, const ConfigText& text) {
    w.BeginObject(true);
    w.Key("de").String(text.de);
    w.Key("en").String(text.en);
    w.EndObject();
}

// ---- schema --------------------------------------------------------------------------------------------------

void WriteDefault(JsonWriter& w, const ConfigKeyDef& k) {
    switch (k.type) {
    case ConfigKeyType::Bool: {
        bool b = false;
        ParseConfigBool(k.defaultValue, b);
        w.Bool(b);
        return;
    }
    case ConfigKeyType::Int: {
        std::uint32_t u = 0;
        ParseConfigU32(k.defaultValue, u);
        w.UInt(u);
        return;
    }
    case ConfigKeyType::Float: {
        double d = 0.0;
        ParseConfigFloat(k.defaultValue, d);
        w.Number(d, true);
        return;
    }
    case ConfigKeyType::Choice:
    case ConfigKeyType::String:
    case ConfigKeyType::PathFile:
    case ConfigKeyType::PathDir:
    case ConfigKeyType::Resolution:
        w.String(k.defaultValue);
        return;
    }
    w.String(k.defaultValue);
}

void WriteLimit(JsonWriter& w, const ConfigKeyDef& k, double v) {
    if (k.type == ConfigKeyType::Float) {
        w.Number(v, true);
    } else {
        w.UInt(static_cast<std::uint64_t>(v));
    }
}

void WriteKey(JsonWriter& w, const ConfigKeyDef& k, const ConfigLocation& location) {
    w.BeginObject();
    w.Key("name").String(k.name);
    w.Key("type").String(ConfigKeyTypeName(k.type));
    w.Key("default");
    WriteDefault(w, k);
    bool anyHidden = false;
    if (!k.choices.empty()) {
        w.Key("choices");
        w.BeginArray();
        for (const ConfigChoice& c : k.choices) {
            if (c.hidden) {
                anyHidden = true;
                continue;
            }
            w.BeginObject(true);
            w.Key("value").String(c.value);
            w.Key("label");
            WriteText(w, c.label);
            w.EndObject();
        }
        w.EndArray();
    }
    if (anyHidden) {
        w.Key("hidden_choices");
        w.BeginArray(true);
        for (const ConfigChoice& c : k.choices) {
            if (c.hidden) {
                w.String(c.value);
            }
        }
        w.EndArray();
    }
    if (k.ranged) {
        w.Key("min");
        WriteLimit(w, k, k.min);
        w.Key("max");
        WriteLimit(w, k, k.max);
        w.Key("step");
        WriteLimit(w, k, k.step > 0.0 ? k.step : 1.0);
        w.Key("range_enforced").Bool(!k.Has(kConfigRangeIsHint));
    }
    w.Key("label");
    WriteText(w, k.label);
    w.Key("help");
    WriteText(w, k.help);
    w.Key("advanced").Bool(k.Has(kConfigAdvanced));
    w.Key("developer").Bool(k.Has(kConfigDeveloper));
    w.Key("dynamic_default").Bool(k.Has(kConfigDynamicDefault));
    w.Key("restart_required").Bool(true);  // the host reads the configuration once at startup
    const std::string_view section(k.section);
    const std::string_view name(k.name);
    if (section == "paths" && name == "userdata" && !location.defaultUserdata.empty()) {
        w.Key("placeholder").String(Utf8(location.defaultUserdata));
    } else if (section == "render" && name == "screenshot_dir" && !location.defaultUserdata.empty()) {
        w.Key("placeholder").String(Utf8(location.defaultUserdata / L"screenshots"));
    }
    if (k.unit != nullptr) {
        w.Key("unit").String(k.unit);  // M4 (input agent): slider unit, src/INTERFACES.md 4.26
    }
    w.Key("ui");
    w.BeginObject(true);
    w.Key("tab").String(ConfigUiTabName(k.tab));
    w.Key("order").Int(k.order);
    w.Key("widget").String(ConfigUiWidgetName(k.widget));
    if (k.fileFilter != nullptr) {
        w.Key("filter").String(k.fileFilter);
    }
    w.EndObject();
    w.EndObject();
}

// ---- disc image check ----------------------------------------------------------------------------------------

constexpr std::uint32_t kMaxXbeBytes = 64u << 20;

struct ReasonText {
    const char* code;
    const char* de;
    const char* en;
};
// Player texts of IsoCheckResult (launcher wizard).
constexpr ReasonText kReasons[] = {
    {"supported", "Futurama erkannt (europäische Verkaufsversion). Diese Version wird unterstützt.",
     "Futurama detected (European retail release). This release is supported."},
    {"not_found", "Die Datei wurde nicht gefunden.", "The file was not found."},
    {"not_a_file", "Das ist keine Datei, sondern ein Ordner oder Gerät.", "This is not a file but a folder or device."},
    {"unreadable", "Die Datei lässt sich nicht lesen.", "The file cannot be read."},
    {"no_xdvdfs", "Das ist kein Xbox-Disc-Abbild (kein Xbox-Dateisystem gefunden).",
     "This is not an Xbox disc image (no Xbox file system found)."},
    {"corrupt", "Das Xbox-Dateisystem im Abbild ist beschädigt.", "The Xbox file system in the image is damaged."},
    {"no_default_xbe", "Im Abbild fehlt das Spielprogramm default.xbe.",
     "The game program default.xbe is missing from the image."},
    {"bad_xbe", "Das Spielprogramm default.xbe im Abbild ist beschädigt.",
     "The game program default.xbe in the image is damaged."},
    {"prototype",
     "Das ist der NTSC-Prototyp von Futurama (17. Juni 2003). Unterstützt wird nur die europäische "
     "Verkaufsversion.",
     "This is the Futurama NTSC prototype (June 17, 2003). Only the European retail release is supported."},
    {"other_version",
     "Diese Version von Futurama wird nicht unterstützt (zum Beispiel die US-Version). Unterstützt wird nur die "
     "europäische Verkaufsversion.",
     "This release of Futurama is not supported (for example the US version). Only the European retail release is "
     "supported."},
    {"other_game", "Das ist kein Futurama-Abbild.", "This is not a Futurama disc image."},
};

void SetReason(IsoCheckResult& r, const char* code, std::string reason) {
    r.reasonCode = code;
    r.reason = std::move(reason);
    for (const ReasonText& t : kReasons) {
        if (std::strcmp(t.code, code) == 0) {
            r.messageDe = t.de;
            r.messageEn = t.en;
        }
    }
}

std::string HexDigest(const std::uint8_t* digest, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        s += kHex[digest[i] >> 4];
        s += kHex[digest[i] & 15];
    }
    return s;
}

// Title name and id from the XBE certificate; false if the header is not a plausible XBE header.
bool ReadXbeCertificate(const std::vector<std::uint8_t>& x, std::string& title, std::uint32_t& titleId) {
    if (x.size() < sizeof(xbe::ImageHeader)) {
        return false;
    }
    xbe::ImageHeader h{};
    std::memcpy(&h, x.data(), sizeof(h));
    if (h.Magic != xbe::kMagic || h.CertificateAddress < h.BaseAddress) {
        return false;
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(h.CertificateAddress) - h.BaseAddress;
    constexpr std::uint64_t kNeeded = offsetof(xbe::Certificate, AlternateTitleIds);
    if (offset + kNeeded > x.size() || offset + kNeeded > h.SizeOfHeaders) {
        return false;
    }
    xbe::Certificate c{};
    std::memcpy(&c, x.data() + offset, static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(c), x.size() - offset)));
    titleId = c.TitleId;
    std::size_t n = 0;
    while (n < std::size(c.TitleName) && c.TitleName[n] != 0) {
        ++n;
    }
    title = WideToUtf8(std::wstring_view(c.TitleName, n));
    return true;
}

bool ContainsFuturama(const std::string& title) {
    std::string lower(title);
    for (char& c : lower) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return lower.find("futurama") != std::string::npos;
}

// ---- GPU query -------------------------------------------------------------------------------------------------

// D3DKMT (gdi32, Windows Vista+; d3dkmthk.h): exact 64-bit segment sizes. DXGI_ADAPTER_DESC1::DedicatedVideoMemory
// is a SIZE_T and saturates in this 32-bit process (a 32 GB card would read as 4095 MB).
using KmtHandle = UINT;
struct KmtOpenAdapterFromLuid {
    LUID adapterLuid;
    KmtHandle adapter;
};
struct KmtQueryAdapterInfo {
    KmtHandle adapter;
    UINT type;  // KMTQUERYADAPTERINFOTYPE
    void* privateData;
    UINT privateDataSize;
};
struct KmtSegmentSizeInfo {
    ULONGLONG dedicatedVideoMemorySize;
    ULONGLONG dedicatedSystemMemorySize;
    ULONGLONG sharedSystemMemorySize;
};
struct KmtCloseAdapter {
    KmtHandle adapter;
};
constexpr UINT kKmtQaiTypeGetSegmentSize = 3;  // KMTQAITYPE_GETSEGMENTSIZE

bool KmtDedicatedVideoMemory(const LUID& luid, std::uint64_t& bytes) noexcept {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (gdi == nullptr) {
        gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (gdi == nullptr) {
        return false;
    }
    using OpenFn = LONG(APIENTRY*)(KmtOpenAdapterFromLuid*);
    using QueryFn = LONG(APIENTRY*)(const KmtQueryAdapterInfo*);
    using CloseFn = LONG(APIENTRY*)(const KmtCloseAdapter*);
    const auto open = reinterpret_cast<OpenFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid")));
    const auto query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo")));
    const auto close = reinterpret_cast<CloseFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTCloseAdapter")));
    if (open == nullptr || query == nullptr || close == nullptr) {
        return false;
    }
    KmtOpenAdapterFromLuid o{};
    o.adapterLuid = luid;
    if (open(&o) < 0) {
        return false;
    }
    KmtSegmentSizeInfo sizes{};
    KmtQueryAdapterInfo q{};
    q.adapter = o.adapter;
    q.type = kKmtQaiTypeGetSegmentSize;
    q.privateData = &sizes;
    q.privateDataSize = sizeof(sizes);
    const bool ok = query(&q) >= 0;
    KmtCloseAdapter c{};
    c.adapter = o.adapter;
    close(&c);
    if (!ok) {
        return false;
    }
    bytes = sizes.dedicatedVideoMemorySize;
    return true;
}

const char* FeatureLevelName(D3D_FEATURE_LEVEL level) noexcept {
    switch (level) {
    case D3D_FEATURE_LEVEL_12_2:
        return "12_2";
    case D3D_FEATURE_LEVEL_12_1:
        return "12_1";
    case D3D_FEATURE_LEVEL_12_0:
        return "12_0";
    case D3D_FEATURE_LEVEL_11_1:
        return "11_1";
    case D3D_FEATURE_LEVEL_11_0:
        return "11_0";
    case D3D_FEATURE_LEVEL_10_1:
        return "10_1";
    case D3D_FEATURE_LEVEL_10_0:
        return "10_0";
    default:
        return "9_x";
    }
}

// Creates a D3D11 device on `adapter` (feature level 10_0+, like the renderer: shader model 4) and checks the
// MSAA sample counts the renderer needs: B8G8R8A8 back buffer and R8G8B8A8 render-to-texture targets (multisample
// render target + resolve), D24S8 depth.
void ProbeD3D11(IDXGIAdapter1* adapter, GpuAdapterInfo& a) {
    a.msaa = {1};
    static constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, kLevels,
                                   static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, &device, &level, nullptr);
    if (hr == E_INVALIDARG) {  // runtime without 11_1
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, kLevels + 1,
                               static_cast<UINT>(std::size(kLevels) - 1), D3D11_SDK_VERSION, &device, &level, nullptr);
    }
    if (FAILED(hr) || device == nullptr) {
        return;
    }
    a.featureLevel = FeatureLevelName(level);
    a.maxAnisotropy = level >= D3D_FEATURE_LEVEL_9_2 ? D3D11_MAX_MAXANISOTROPY : 2u;
    for (const UINT count : {2u, 4u, 8u}) {
        bool ok = true;
        for (const DXGI_FORMAT format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM}) {
            UINT quality = 0;
            UINT support = 0;
            constexpr UINT kNeeded = D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
            ok = ok && SUCCEEDED(device->CheckMultisampleQualityLevels(format, count, &quality)) && quality > 0 &&
                 SUCCEEDED(device->CheckFormatSupport(format, &support)) && (support & kNeeded) == kNeeded;
        }
        UINT depthQuality = 0;
        ok = ok && SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, count, &depthQuality)) &&
             depthQuality > 0;
        if (ok) {
            a.msaa.push_back(count);
        }
    }
}

std::uint32_t MonitorDpi(HMONITOR monitor) noexcept {
    static HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (shcore == nullptr) {
        return 0;
    }
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    const auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")));
    UINT x = 0;
    UINT y = 0;
    return fn != nullptr && SUCCEEDED(fn(monitor, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) ? x : 0u;
}

void ReadModes(IDXGIOutput* output, GpuOutputInfo& o) {
    for (const DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
        UINT count = 0;
        if (FAILED(output->GetDisplayModeList(format, 0, &count, nullptr)) || count == 0) {
            continue;
        }
        std::vector<DXGI_MODE_DESC> modes(count);
        if (FAILED(output->GetDisplayModeList(format, 0, &count, modes.data()))) {
            continue;
        }
        modes.resize(count);
        for (const DXGI_MODE_DESC& m : modes) {
            o.modes.emplace_back(m.Width, m.Height);
        }
        break;
    }
    if (o.width > 0 && o.height > 0) {
        o.modes.emplace_back(static_cast<std::uint32_t>(o.width), static_cast<std::uint32_t>(o.height));
    }
    std::sort(o.modes.begin(), o.modes.end());
    o.modes.erase(std::unique(o.modes.begin(), o.modes.end()), o.modes.end());
}

// HKCU\Software\Microsoft\DirectX\UserGpuPreferences DirectXUserGlobalSettings "VRROptimizeEnable=1;..." (Windows
// graphics settings "Variable refresh rate"). Informational only: the flip-model swap chain with tearing support
// reaches VRR without it.
std::string ReadWindowsVrrSetting() {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
                     L"DirectXUserGlobalSettings", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return "unset";
    }
    const std::wstring_view s(buf);
    constexpr std::wstring_view kKey = L"VRROptimizeEnable=";
    const std::size_t at = s.find(kKey);
    if (at == std::wstring_view::npos || at + kKey.size() >= s.size()) {
        return "unset";
    }
    const wchar_t v = s[at + kKey.size()];
    return v == L'1' ? "on" : v == L'0' ? "off" : "unset";
}

// Per-monitor DPI awareness for the queries on this thread (physical coordinates, real DPI).
class DpiAwareScope {
public:
    DpiAwareScope() noexcept : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~DpiAwareScope() {
        if (previous_ != nullptr) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }
    DpiAwareScope(const DpiAwareScope&) = delete;
    DpiAwareScope& operator=(const DpiAwareScope&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_;
};

void WriteU32Array(JsonWriter& w, const std::vector<std::uint32_t>& values) {
    w.BeginArray(true);
    for (const std::uint32_t v : values) {
        w.UInt(v);
    }
    w.EndArray();
}

// ---- command line --------------------------------------------------------------------------------------------

enum class Tool : std::uint8_t { None, Schema, CheckIso, QueryGpu, Paths };

struct ToolArgs {
    Tool tool = Tool::None;
    int toolOptions = 0;      // number of tool options (exactly one allowed)
    bool anyToolArg = false;  // a tool option or --result= is present
    std::wstring output;      // schema / gpu / paths target
    std::wstring iso;
    std::wstring result;
    bool hasResult = false;
    std::wstring config;
    std::vector<std::wstring> unexpected;
};

// "--name=value" or "--name" (value empty, hasValue false).
bool MatchOption(const std::wstring& arg, std::wstring_view name, std::wstring& value) {
    if (arg.size() < name.size() || arg.compare(0, name.size(), name) != 0) {
        return false;
    }
    if (arg.size() == name.size()) {
        value.clear();
        return true;
    }
    if (arg[name.size()] != L'=') {
        return false;
    }
    value = arg.substr(name.size() + 1);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return true;
}

ToolArgs ParseToolArgs(const wchar_t* commandLine) {
    ToolArgs a;
    if (commandLine == nullptr || *commandLine == 0) {
        return a;
    }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine, &argc);
    if (argv == nullptr) {
        return a;
    }
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    LocalFree(argv);
    for (const std::wstring& arg : args) {
        std::wstring v;
        auto tool = [&](Tool t, std::wstring* target) {
            a.tool = t;
            ++a.toolOptions;
            a.anyToolArg = true;
            *target = v;
        };
        if (MatchOption(arg, L"--print-config-schema", v)) {
            tool(Tool::Schema, &a.output);
        } else if (MatchOption(arg, L"--check-iso", v)) {
            tool(Tool::CheckIso, &a.iso);
        } else if (MatchOption(arg, L"--query-gpu", v)) {
            tool(Tool::QueryGpu, &a.output);
        } else if (MatchOption(arg, L"--print-paths", v)) {
            tool(Tool::Paths, &a.output);
        } else if (MatchOption(arg, L"--result", v)) {
            a.anyToolArg = true;
            a.hasResult = true;
            a.result = v;
        } else if (MatchOption(arg, L"--config", v)) {
            a.config = v;
        } else {
            a.unexpected.push_back(arg);
        }
    }
    return a;
}

constexpr char kUsage[] =
    "usage: futurama.exe --print-config-schema=<file.json> | --check-iso=<image> --result=<file.json> | "
    "--query-gpu=<file.json> | --print-paths=<file.json> [--config=<file>]  ('-' = stdout)";

int RunTool(const ToolArgs& a, const std::filesystem::path& exePath) {
    if (a.toolOptions != 1) {
        ReportToolError(a.toolOptions == 0 ? std::string("--result needs --check-iso; ") + kUsage
                                           : std::string("only one tool option per call; ") + kUsage);
        return kToolExitUsage;
    }
    if (!a.unexpected.empty()) {
        ReportToolError("option '" + WideToUtf8(a.unexpected.front()) + "' cannot be combined with a tool option; " +
                        kUsage);
        return kToolExitUsage;
    }
    if (a.tool == Tool::CheckIso && (a.iso.empty() || !a.hasResult || a.result.empty())) {
        ReportToolError(std::string("--check-iso needs an image path and --result=<file.json>; ") + kUsage);
        return kToolExitUsage;
    }
    if (a.tool != Tool::CheckIso && a.hasResult) {
        ReportToolError(std::string("--result belongs to --check-iso; ") + kUsage);
        return kToolExitUsage;
    }
    if (a.tool != Tool::CheckIso && a.output.empty()) {
        ReportToolError(std::string("missing output file; ") + kUsage);
        return kToolExitUsage;
    }
    std::error_code ec;
    std::string text;
    std::wstring target = a.output;
    int okCode = kToolExitOk;
    switch (a.tool) {
    case Tool::Schema:
        text = ConfigSchemaJson(exePath);
        break;
    case Tool::CheckIso: {
        const IsoCheckResult r = CheckIso(std::filesystem::path(a.iso));
        text = IsoCheckJson(r);
        target = a.result;
        okCode = r.supported ? kToolExitOk : kToolExitNotSupported;
        break;
    }
    case Tool::QueryGpu:
        text = GpuInfoJson(QueryGpu());
        break;
    case Tool::Paths: {
        std::filesystem::path config;
        if (!a.config.empty()) {
            config = std::filesystem::absolute(std::filesystem::path(a.config), ec);
            if (ec) {
                config = std::filesystem::path(a.config);
            }
        }
        text = PathsJson(exePath, config.lexically_normal());
        break;
    }
    case Tool::None:
        return kToolExitUsage;
    }
    std::string error;
    if (!WriteOutput(target, text, error)) {
        ReportToolError(error);
        return kToolExitUsage;
    }
    return okCode;
}

}  // namespace

// ---- JsonWriter ------------------------------------------------------------------------------------------------

void JsonWriter::Indent() {
    out_ += '\n';
    out_.append(stack_.size() * 2, ' ');
}

void JsonWriter::BeforeValue() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (stack_.empty()) {
        return;
    }
    Level& level = stack_.back();
    if (level.count > 0) {
        out_ += level.inlined ? ", " : ",";
    }
    if (!level.inlined) {
        Indent();
    }
    ++level.count;
}

void JsonWriter::Open(char bracket, bool array, bool inlineContainer) {
    BeforeValue();
    out_ += bracket;
    const bool parentInline = !stack_.empty() && stack_.back().inlined;
    stack_.push_back(Level{array, inlineContainer || parentInline, 0});
}

void JsonWriter::Close(char bracket) {
    if (stack_.empty()) {
        return;
    }
    const Level level = stack_.back();
    stack_.pop_back();
    if (!level.inlined && level.count > 0) {
        Indent();
    }
    out_ += bracket;
}

void JsonWriter::BeginObject(bool inlineContainer) {
    Open('{', false, inlineContainer);
}

void JsonWriter::EndObject() {
    Close('}');
}

void JsonWriter::BeginArray(bool inlineContainer) {
    Open('[', true, inlineContainer);
}

void JsonWriter::EndArray() {
    Close(']');
}

JsonWriter& JsonWriter::Key(std::string_view key) {
    BeforeValue();
    out_ += Escape(key);
    out_ += ": ";
    afterKey_ = true;
    return *this;
}

void JsonWriter::String(std::string_view utf8) {
    BeforeValue();
    out_ += Escape(utf8);
}

void JsonWriter::Bool(bool value) {
    BeforeValue();
    out_ += value ? "true" : "false";
}

void JsonWriter::Int(std::int64_t value) {
    BeforeValue();
    out_ += std::to_string(value);
}

void JsonWriter::UInt(std::uint64_t value) {
    BeforeValue();
    out_ += std::to_string(value);
}

void JsonWriter::Number(double value, bool forceFraction) {
    if (!std::isfinite(value)) {
        Null();
        return;
    }
    BeforeValue();
    char buf[64];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    std::string s = ec == std::errc() ? std::string(buf, end) : std::string("0");
    if (forceFraction && s.find_first_of(".eE") == std::string::npos) {
        s += ".0";
    }
    out_ += s;
}

void JsonWriter::Null() {
    BeforeValue();
    out_ += "null";
}

std::string JsonWriter::Take() {
    std::string s = std::move(out_);
    s += '\n';
    out_.clear();
    stack_.clear();
    afterKey_ = false;
    return s;
}

std::string JsonWriter::Escape(std::string_view utf8) {
    std::string r;
    r.reserve(utf8.size() + 2);
    r += '"';
    for (const char ch : utf8) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            r += "\\\"";
            break;
        case '\\':
            r += "\\\\";
            break;
        case '\n':
            r += "\\n";
            break;
        case '\r':
            r += "\\r";
            break;
        case '\t':
            r += "\\t";
            break;
        case '\b':
            r += "\\b";
            break;
        case '\f':
            r += "\\f";
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                r += buf;
            } else {
                r += ch;
            }
        }
    }
    r += '"';
    return r;
}

// ---- public functions --------------------------------------------------------------------------------------------

std::string BuildDescription() {
    // __DATE__ = "Oct  6 2026", __TIME__ = "14:03:12"
    static constexpr char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* date = __DATE__;
    int month = 0;
    for (int m = 0; m < 12; ++m) {
        if (std::strncmp(date, kMonths + 3 * m, 3) == 0) {
            month = m + 1;
        }
    }
    const int day = std::atoi(date + 4);
    const int year = std::atoi(date + 7);
#ifdef NDEBUG
    const char* type = "release";
#else
    const char* type = "debug";
#endif
#if defined(__clang__)
    const std::string compiler = std::string("clang-cl ") + __clang_version__;
#else
    const std::string compiler = "msvc " + std::to_string(_MSC_FULL_VER / 10000000) + "." +
                                 std::to_string(_MSC_FULL_VER / 100000 % 100) + "." +
                                 std::to_string(_MSC_FULL_VER % 100000);
#endif
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s, built %04d-%02d-%02d %s, %s", type, year, month, day, __TIME__,
                  compiler.c_str());
    return buf;
}

std::string ConfigSchemaJson(const std::filesystem::path& exePath) {
    const ConfigLocation location = ResolveConfigLocation(exePath.parent_path(), {});
    JsonWriter w;
    w.BeginObject();
    w.Key("schema_version").Int(kSchemaVersion);
    w.Key("build").String(BuildDescription());
    w.Key("config");
    w.BeginObject();
    w.Key("file_name").String(WideToUtf8(kConfigFileName));
    w.Key("portable_marker").String(WideToUtf8(kPortableMarkerName));
    w.Key("mode").String(ConfigLocationModeName(location.mode));
    w.Key("file").String(Utf8(location.configFile));
    w.Key("default_userdata").String(Utf8(location.defaultUserdata));
    w.EndObject();
    w.Key("sections");
    w.BeginArray();
    for (const ConfigSectionDef& section : ConfigSections()) {
        w.BeginObject();
        w.Key("name").String(section.name);
        w.Key("label");
        WriteText(w, section.label);
        w.Key("keys");
        w.BeginArray();
        for (const ConfigKeyDef& k : ConfigKeys()) {
            if (std::strcmp(k.section, section.name) == 0) {
                WriteKey(w, k, location);
            }
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return w.Take();
}

IsoCheckResult CheckIso(const std::filesystem::path& imageIn) {
    IsoCheckResult r;
    std::error_code ec;
    std::filesystem::path image = std::filesystem::absolute(imageIn, ec);
    if (ec) {
        image = imageIn;
    }
    const std::filesystem::file_status status = std::filesystem::status(image, ec);
    if (!std::filesystem::exists(status)) {
        SetReason(r, "not_found", "file not found: " + Utf8(image));
        return r;
    }
    if (!std::filesystem::is_regular_file(status)) {
        SetReason(r, "not_a_file", "not a regular file: " + Utf8(image));
        return r;
    }
    r.size = std::filesystem::file_size(image, ec);
    if (ec) {
        r.size = 0;
    }
    vfs::XdvdfsImage disc;
    std::string error;
    if (!disc.Open(image, error)) {
        const char* code = error.rfind("cannot ", 0) == 0          ? "unreadable"
                           : error.rfind("no XDVDFS volume", 0) == 0 ? "no_xdvdfs"
                                                                     : "corrupt";
        r.xdvdfs = std::strcmp(code, "corrupt") == 0;  // the volume descriptor was found
        SetReason(r, code, error);
        return r;
    }
    r.xdvdfs = true;
    r.partitionOffset = disc.PartitionOffset();
    r.layout = r.partitionOffset == 0 ? "xiso" : "redump";
    vfs::XdvdfsImage::Entry entry;
    if (!disc.Lookup("default.xbe", entry) || entry.IsDirectory()) {
        SetReason(r, "no_default_xbe", "default.xbe not found in the disc root");
        return r;
    }
    r.xbeSize = entry.size;
    if (entry.size < sizeof(xbe::ImageHeader) || entry.size > kMaxXbeBytes) {
        SetReason(r, "bad_xbe", "default.xbe has an implausible size of " + std::to_string(entry.size) + " bytes");
        return r;
    }
    std::vector<std::uint8_t> bytes(entry.size);
    std::uint32_t done = 0;
    while (done < entry.size) {
        const std::uint32_t chunk = std::min<std::uint32_t>(entry.size - done, 1u << 20);
        std::uint32_t got = 0;
        if (!disc.Read(entry, done, bytes.data() + done, chunk, got) || got == 0) {
            SetReason(r, "unreadable", "read error in default.xbe at offset " + std::to_string(done));
            return r;
        }
        done += got;
    }
    std::uint8_t digest[krnl::xc::kShaDigestSize];
    krnl::xc::Sha1(bytes.data(), bytes.size(), digest);
    r.xbeSha1 = HexDigest(digest, sizeof(digest));
    const bool header = ReadXbeCertificate(bytes, r.title, r.titleId);
    if (r.xbeSha1 == guest::kXbeSha1) {
        r.version = "retail_eu";
        r.ok = true;
        r.supported = true;
        SetReason(r, "supported", "Futurama retail EU (default.xbe SHA-1 " + r.xbeSha1 + ")");
    } else if (r.xbeSha1 == kProtoNtscXbeSha1) {
        r.version = "proto_ntsc";
        r.ok = true;
        SetReason(r, "prototype", "Futurama NTSC prototype of Jun 17, 2003 (default.xbe SHA-1 " + r.xbeSha1 +
                                      "); only the retail EU release is supported");
    } else if (!header) {
        SetReason(r, "bad_xbe", "default.xbe is not a valid XBE (SHA-1 " + r.xbeSha1 + ")");
    } else {
        r.ok = true;
        const bool futurama = ContainsFuturama(r.title);
        SetReason(r, futurama ? "other_version" : "other_game",
                  std::string(futurama ? "another release of Futurama" : "not Futurama") + " (title \"" + r.title +
                      "\", default.xbe SHA-1 " + r.xbeSha1 + "); only the retail EU release is supported");
    }
    return r;
}

std::string IsoCheckJson(const IsoCheckResult& r) {
    JsonWriter w;
    w.BeginObject();
    w.Key("ok").Bool(r.ok);
    w.Key("reason").String(r.reason);
    w.Key("xdvdfs").Bool(r.xdvdfs);
    w.Key("layout").String(r.layout);
    w.Key("xbe_sha1").String(r.xbeSha1);
    w.Key("version").String(r.version);
    w.Key("supported").Bool(r.supported);
    w.Key("title").String(r.title);
    w.Key("size").UInt(r.size);
    w.Key("reason_code").String(r.reasonCode);
    w.Key("message");
    w.BeginObject(true);
    w.Key("de").String(r.messageDe);
    w.Key("en").String(r.messageEn);
    w.EndObject();
    w.Key("partition_offset").UInt(r.partitionOffset);
    w.Key("xbe_size").UInt(r.xbeSize);
    char id[16];
    std::snprintf(id, sizeof(id), "%08X", r.titleId);
    w.Key("title_id").String(r.titleId != 0 ? id : "");
    w.EndObject();
    return w.Take();
}

GpuInfo QueryGpu() noexcept {
    GpuInfo info;
    try {
        DpiAwareScope dpiScope;
        ComPtr<IDXGIFactory1> factory;
        const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "CreateDXGIFactory1 failed (0x%08lX)", static_cast<unsigned long>(hr));
            info.error = buf;
            return info;
        }
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory.As(&factory5))) {
            BOOL tearing = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
                info.tearing = tearing != FALSE;
                info.vrrHint = info.tearing ? "possible" : "unavailable";
            }
        }
        info.windowsVrrSetting = ReadWindowsVrrSetting();
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || adapter == nullptr) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) {
                continue;
            }
            GpuAdapterInfo a;
            a.name = WideToUtf8(std::wstring_view(desc.Description, wcsnlen(desc.Description, std::size(desc.Description))));
            a.vendorId = desc.VendorId;
            a.deviceId = desc.DeviceId;
            a.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            std::uint64_t bytes = 0;
            if (KmtDedicatedVideoMemory(desc.AdapterLuid, bytes)) {
                a.dedicatedSource = "d3dkmt";
            } else {
                bytes = desc.DedicatedVideoMemory;
                a.dedicatedSource = "dxgi";
            }
            a.dedicatedMb = bytes / (1024u * 1024u);
            ProbeD3D11(adapter.Get(), a);
            const int index = static_cast<int>(info.adapters.size());
            for (UINT j = 0;; ++j) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(j, &output) == DXGI_ERROR_NOT_FOUND || output == nullptr) {
                    break;
                }
                DXGI_OUTPUT_DESC od{};
                if (FAILED(output->GetDesc(&od))) {
                    continue;
                }
                GpuOutputInfo o;
                o.adapter = index;
                o.device = WideToUtf8(std::wstring_view(od.DeviceName, wcsnlen(od.DeviceName, std::size(od.DeviceName))));
                o.attached = od.AttachedToDesktop != FALSE;
                o.x = od.DesktopCoordinates.left;
                o.y = od.DesktopCoordinates.top;
                o.width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
                o.height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                o.primary = od.Monitor != nullptr && GetMonitorInfoW(od.Monitor, &mi) &&
                            (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                const platform::DisplayTiming timing = platform::QueryDisplayTiming(od.Monitor);
                if (timing.valid) {
                    o.refreshNum = timing.numerator;
                    o.refreshDen = timing.denominator != 0 ? timing.denominator : 1;
                    o.hz = timing.hz;
                }
                o.refreshSource = timing.source;
                o.name = timing.name[0] != 0 ? std::string(timing.name) : o.device;
                o.dpi = od.Monitor != nullptr ? MonitorDpi(od.Monitor) : 0u;
                ReadModes(output.Get(), o);
                info.outputs.push_back(std::move(o));
            }
            info.adapters.push_back(std::move(a));
        }
        // D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE) - [render] adapter = hardware - takes the first
        // adapter of EnumAdapters1.
        if (!info.adapters.empty()) {
            info.defaultAdapter = 0;
            info.msaa = info.adapters[0].msaa;
            info.maxAnisotropy = info.adapters[0].maxAnisotropy;
        }
    } catch (...) {
        info.error = "out of memory while querying the graphics adapters";
    }
    return info;
}

std::string GpuInfoJson(const GpuInfo& info) {
    JsonWriter w;
    w.BeginObject();
    w.Key("adapters");
    w.BeginArray();
    for (const GpuAdapterInfo& a : info.adapters) {
        w.BeginObject();
        w.Key("name").String(a.name);
        w.Key("vendor_id").UInt(a.vendorId);
        w.Key("dedicated_mb").UInt(a.dedicatedMb);
        w.Key("device_id").UInt(a.deviceId);
        w.Key("dedicated_mb_source").String(a.dedicatedSource);
        w.Key("software").Bool(a.software);
        w.Key("feature_level").String(a.featureLevel);
        w.Key("msaa");
        WriteU32Array(w, a.msaa);
        w.Key("max_anisotropy").UInt(a.maxAnisotropy);
        w.EndObject();
    }
    w.EndArray();
    w.Key("outputs");
    w.BeginArray();
    for (const GpuOutputInfo& o : info.outputs) {
        w.BeginObject();
        w.Key("name").String(o.name);
        w.Key("device").String(o.device);
        w.Key("primary").Bool(o.primary);
        w.Key("rect");
        w.BeginArray(true);
        w.Int(o.x);
        w.Int(o.y);
        w.Int(o.width);
        w.Int(o.height);
        w.EndArray();
        w.Key("refresh");
        w.BeginObject(true);
        w.Key("num").UInt(o.refreshNum);
        w.Key("den").UInt(o.refreshDen);
        w.EndObject();
        w.Key("modes");
        w.BeginArray();
        for (const auto& [mw, mh] : o.modes) {
            w.BeginArray(true);
            w.UInt(mw);
            w.UInt(mh);
            w.EndArray();
        }
        w.EndArray();
        w.Key("adapter").Int(o.adapter);
        w.Key("hz").Number(o.hz, true);
        w.Key("refresh_source").String(o.refreshSource);
        w.Key("dpi").UInt(o.dpi);
        w.Key("attached").Bool(o.attached);
        w.EndObject();
    }
    w.EndArray();
    w.Key("msaa");
    WriteU32Array(w, info.msaa);
    w.Key("max_anisotropy").UInt(info.maxAnisotropy);
    w.Key("tearing").Bool(info.tearing);
    w.Key("vrr_hint").String(info.vrrHint);
    w.Key("default_adapter").Int(info.defaultAdapter);
    w.Key("vrr");
    w.BeginObject();
    w.Key("possible").Bool(info.vrrHint == "possible");
    w.Key("windows_setting").String(info.windowsVrrSetting);
    w.Key("hint");
    w.BeginObject(true);
    if (info.vrrHint == "possible") {
        // Review M3c finding 17: plain wording for the launcher's graphics tab.
        w.Key("de").String("Die Grafikkarte erlaubt Bildausgabe ohne VSync-Wartezeit. Ein VRR-Bildschirm (G-SYNC, "
                           "FreeSync, HDMI-VRR) kann damit der Bildrate des Spiels folgen; ob VRR aktiv ist, erkennt "
                           "das Spiel erst während des Spielens.");
        w.Key("en").String("The graphics card allows presenting without waiting for vsync. A VRR display (G-SYNC, "
                           "FreeSync, HDMI VRR) can follow the game's frame rate this way; whether VRR is active is "
                           "detected while the game runs.");
    } else if (info.vrrHint == "unavailable") {
        w.Key("de").String("Ohne Unterstützung für Bildrisse im Fenster ist VRR nicht nutzbar; das Spiel koppelt an "
                           "den Bildschirmtakt.");
        w.Key("en").String("Without tearing support in a window VRR cannot be used; the game couples to the display "
                           "refresh.");
    } else {
        w.Key("de").String("Unbekannt (DXGI 1.5 fehlt).");
        w.Key("en").String("Unknown (DXGI 1.5 missing).");
    }
    w.EndObject();
    w.EndObject();
    if (!info.error.empty()) {
        w.Key("error").String(info.error);
    }
    w.EndObject();
    return w.Take();
}

std::string PathsJson(const std::filesystem::path& exePath, const std::filesystem::path& explicitConfig) {
    const ConfigLocation location = ResolveConfigLocation(exePath.parent_path(), explicitConfig);
    std::wstring commandLine = L"futurama.exe";
    if (!explicitConfig.empty()) {
        commandLine += L" \"--config=" + explicitConfig.wstring() + L"\"";
    }
    Config cfg;
    std::string error;
    const bool ok = LoadConfig(commandLine.c_str(), exePath, cfg, error);
    std::error_code ec;
    const std::filesystem::path userdata = ok ? cfg.userdata : location.defaultUserdata;
    const bool configured = ok && !cfg.GetString("paths", "userdata").empty();
    const std::filesystem::path screenshots =
        ok ? cfg.GetPath("render", "screenshot_dir", userdata / L"screenshots") : userdata / L"screenshots";
    const std::filesystem::path partition1 = userdata / L"hdd" / L"partition1";
    JsonWriter w;
    w.BeginObject();
    w.Key("ok").Bool(ok);
    w.Key("error").String(ok ? std::string() : error);
    w.Key("mode").String(ConfigLocationModeName(location.mode));
    w.Key("portable").Bool(location.portable);
    w.Key("exe_dir").String(Utf8(exePath.parent_path()));
    w.Key("config").String(Utf8(location.configFile));
    w.Key("config_exists").Bool(!location.configFile.empty() && std::filesystem::is_regular_file(location.configFile, ec));
    w.Key("userdata").String(Utf8(userdata));
    w.Key("userdata_configured").Bool(configured);
    w.Key("logs").String(Utf8(userdata / L"logs"));
    w.Key("hdd").String(Utf8(partition1));
    w.Key("saves").String(Utf8(partition1 / L"UDATA"));
    w.Key("screenshots").String(Utf8(screenshots));
    w.Key("cache").String(Utf8(userdata / L"cache"));
    w.Key("warnings");
    w.BeginArray();
    for (const std::string& warning : cfg.warnings) {
        w.String(warning);
    }
    w.EndArray();
    w.EndObject();
    return w.Take();
}

bool RunToolIfRequested(const wchar_t* commandLine, const std::filesystem::path& exePath, int& exitCode) noexcept {
    ToolArgs args;
    try {
        args = ParseToolArgs(commandLine);
    } catch (...) {
        return false;  // out of memory while splitting the command line: let the normal start report it
    }
    if (!args.anyToolArg) {
        return false;
    }
    try {
        exitCode = RunTool(args, exePath);
    } catch (const std::exception& e) {
        ReportToolError(std::string("tool failed: ") + e.what());
        exitCode = kToolExitUsage;
    } catch (...) {
        ReportToolError("tool failed with an unknown exception");
        exitCode = kToolExitUsage;
    }
    return true;
}

}  // namespace core::cli
