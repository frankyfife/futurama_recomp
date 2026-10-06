// Tool modes of futurama.exe for the Qt launcher (M3c, shared contract items 1-4). Owned by the host-CLI/schema
// agent.
//
// core::RunHost (core/host_main.cpp) calls RunToolIfRequested first: the tool modes run in the host DLL after the
// launcher stub's TLS/placeholder checks and LoadLibrary, but BEFORE the configuration, the log, the crash
// handlers and any XBE mapping. They never start the game and never write to the user data directory.
//
//   futurama.exe --print-config-schema=<file.json>
//       Writes the configuration schema (core/config_schema.h) and exits 0.
//       {"schema_version":1,"build":"..","config":{..},"sections":[{"name","label":{"de","en"},"keys":[{"name",
//        "type","default","choices":[{"value","label"}],"min","max","step","label","help","advanced",
//        "restart_required","ui":{"tab","order","widget"}}]}]}
//       Extensions beyond the contract (all optional for readers): per key "developer" (test/diagnosis knob, hide
//       from players), "dynamic_default" (default depends on build/environment/other keys: write the key only if
//       the player changes it), "hidden_choices" (accepted values not offered in the UI), "range_enforced"
//       (false: min/max are UI limits only), "placeholder" (shown in empty path fields: the effective default
//       path), "ui":{"filter"} (file dialog patterns); top level "config" {"file_name", "portable_marker",
//       "mode", "file", "default_userdata"} = where this exe reads its INI without --config.
//   futurama.exe --check-iso=<image> --result=<file.json>
//       {"ok","reason","xdvdfs","layout":"xiso|redump|unknown","xbe_sha1","version":"retail_eu|proto_ntsc|unknown",
//        "supported","title","size"} + extensions "reason_code", "message":{"de","en"}, "partition_offset",
//       "xbe_size", "title_id". ok = a readable Xbox disc with a valid default.xbe (the version is known);
//       supported = the retail EU release (default.xbe SHA-1 guest::kXbeSha1). Exit 0 if supported, else 2.
//   futurama.exe --query-gpu=<file.json>
//       {"adapters":[{"name","vendor_id","dedicated_mb"}],"outputs":[{"name","device","primary","rect":[x,y,w,h],
//        "refresh":{"num","den"},"modes":[[w,h]...]}],"msaa":[1,2,4,8],"max_anisotropy":16,"tearing","vrr_hint"}
//       msaa / max_anisotropy are those of the default hardware adapter (the one [render] adapter = hardware
//       uses) for the formats of the renderer (B8G8R8A8 and R8G8B8A8 render targets incl. resolve, D24S8 depth).
//       vrr_hint: "possible" (tearing supported: a VRR display can follow the presents; the frame pacer confirms
//       it at runtime), "unavailable" (no tearing support), "unknown" (DXGI 1.5 missing). Extensions: per adapter
//       "device_id", "software", "feature_level", "msaa", "max_anisotropy", "dedicated_mb_source"; per output
//       "adapter" (index), "hz", "refresh_source", "dpi", "attached"; top level "default_adapter", "vrr" {..}.
//   futurama.exe --print-paths=<file.json> [--config=<file>]   (extension)
//       Where this exe reads its configuration and keeps user data (core::ResolveConfigLocation + the INI):
//       {"ok","error","mode":"default|portable|explicit","portable","exe_dir","config","config_exists",
//        "userdata","userdata_configured","logs","hdd","saves","screenshots","cache"}.
// An output file name "-" writes to stdout. Files are written to "<file>.tmp" and renamed (no partial JSON).
// Only `--config=<file>` may accompany a tool option; anything else is a usage error. Exit codes: 0 ok,
// 1 usage error or the output could not be written (message on stderr, never a message box), 2 --check-iso
// result written but the image is not supported.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace core::cli {

inline constexpr int kToolExitOk = 0;
inline constexpr int kToolExitUsage = 1;
inline constexpr int kToolExitNotSupported = 2;

inline constexpr int kSchemaVersion = 1;

// SHA-1 of default.xbe of the NTSC prototype (Jun 17, 2003): orig/proto/default.xbe, verified with sha1sum
// (the retail EU XBE is guest::kXbeSha1 = 58f76de5...).
inline constexpr char kProtoNtscXbeSha1[] = "f443d255eab39584dcd5dcea6f7c057182c4b854";

// Returns true and sets exitCode if `commandLine` asks for a tool mode (the tool has run); false for a normal
// game start (nothing done). Never throws, never shows a message box.
bool RunToolIfRequested(const wchar_t* commandLine, const std::filesystem::path& exePath, int& exitCode) noexcept;

// "debug|release, built <date> <time>, <compiler>" (JSON "build").
std::string BuildDescription();

// ---- minimal JSON writer (UTF-8, 2-space indentation; inline containers stay on one line) ------------------------
class JsonWriter {
public:
    void BeginObject(bool inlineContainer = false);
    void EndObject();
    void BeginArray(bool inlineContainer = false);
    void EndArray();
    JsonWriter& Key(std::string_view key);  // object member name; the next call writes its value
    void String(std::string_view utf8);
    void Bool(bool value);
    void Int(std::int64_t value);
    void UInt(std::uint64_t value);
    void Number(double value, bool forceFraction = false);  // shortest round-trip form; NaN/inf -> null
    void Null();
    // The finished document (with a final newline).
    std::string Take();

    static std::string Escape(std::string_view utf8);  // quoted JSON string

private:
    struct Level {
        bool array = false;
        bool inlined = false;
        int count = 0;
    };
    void BeforeValue();
    void Indent();
    void Open(char bracket, bool array, bool inlineContainer);
    void Close(char bracket);
    std::string out_;
    std::vector<Level> stack_;
    bool afterKey_ = false;
};

// ---- documents (testable without a process) ----------------------------------------------------------------------
// Schema of every configuration key; `exePath` = futurama.exe (default INI location and placeholders).
std::string ConfigSchemaJson(const std::filesystem::path& exePath);

struct IsoCheckResult {
    bool ok = false;               // readable Xbox disc image with a valid default.xbe
    std::string reasonCode;        // supported | not_found | not_a_file | unreadable | no_xdvdfs | no_default_xbe |
                                   // bad_xbe | prototype | other_version | other_game
    std::string reason;            // English sentence
    std::string messageDe;         // player text (launcher)
    std::string messageEn;
    bool xdvdfs = false;
    std::string layout = "unknown";   // xiso (partition at 0) | redump (full disc image) | unknown
    std::uint64_t partitionOffset = 0;
    std::string xbeSha1;           // lower-case hex, "" without default.xbe
    std::uint64_t xbeSize = 0;
    std::string version = "unknown";  // retail_eu | proto_ntsc | unknown
    bool supported = false;
    std::string title;             // XBE certificate title name (UTF-8)
    std::uint32_t titleId = 0;
    std::uint64_t size = 0;        // image file size in bytes
};
IsoCheckResult CheckIso(const std::filesystem::path& image);
std::string IsoCheckJson(const IsoCheckResult& result);

struct GpuAdapterInfo {
    std::string name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedMb = 0;
    const char* dedicatedSource = "dxgi";  // "d3dkmt" (exact, 64-bit) | "dxgi" (SIZE_T: capped in a 32-bit process)
    bool software = false;
    std::string featureLevel;          // "11_1", "" if no D3D11 device could be created
    std::vector<std::uint32_t> msaa;   // sample counts usable for all renderer formats (always starts with 1)
    std::uint32_t maxAnisotropy = 0;
};
struct GpuOutputInfo {
    int adapter = 0;
    std::string name;     // friendly monitor name, else the device name
    std::string device;   // "\\.\DISPLAY1"
    bool primary = false;
    bool attached = false;
    std::int32_t x = 0, y = 0, width = 0, height = 0;  // desktop rectangle (physical pixels)
    std::uint32_t refreshNum = 0;
    std::uint32_t refreshDen = 1;
    double hz = 0.0;
    std::string refreshSource;  // platform::DisplayTiming::source
    std::uint32_t dpi = 0;      // effective DPI (96 = 100 %), 0 if unknown
    std::vector<std::pair<std::uint32_t, std::uint32_t>> modes;  // unique sizes, ascending
};
struct GpuInfo {
    std::vector<GpuAdapterInfo> adapters;
    std::vector<GpuOutputInfo> outputs;
    int defaultAdapter = -1;  // first hardware adapter (index into adapters), -1 if none
    std::vector<std::uint32_t> msaa{1};
    std::uint32_t maxAnisotropy = 0;
    bool tearing = false;
    std::string vrrHint = "unknown";
    std::string windowsVrrSetting = "unset";  // HKCU ...\DirectX\UserGpuPreferences VRROptimizeEnable: on|off|unset
    std::string error;                         // DXGI factory failure (arrays empty)
};
GpuInfo QueryGpu() noexcept;
std::string GpuInfoJson(const GpuInfo& info);

// --print-paths document; `explicitConfig` = --config (absolute or empty).
std::string PathsJson(const std::filesystem::path& exePath, const std::filesystem::path& explicitConfig);

}  // namespace core::cli
