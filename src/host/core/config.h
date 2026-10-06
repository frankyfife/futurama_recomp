// Host configuration: futurama.ini + command line.
//
// Location of the player configuration (M3c, shared contract item 4; ResolveConfigLocation below):
//   default   %LOCALAPPDATA%\FuturamaPC\futurama.ini, userdata %LOCALAPPDATA%\FuturamaPC
//   portable  a file "portable.txt" lies next to futurama.exe: <exe dir>\futurama.ini, userdata = <exe dir>
//             (config and user data side by side as in the default mode; design 4.1 had ".\userdata")
//   explicit  --config=<file> always wins (the file must exist; the Qt launcher always passes it); the userdata
//             default then follows the mode above (portable.txt -> <exe dir>, else %LOCALAPPDATA%\FuturamaPC)
// A missing INI in default/portable mode means built-in defaults. [paths] userdata overrides the userdata
// default in every mode. Until M3c the default INI was <exe dir>\futurama.ini; such a file is now only read in
// portable mode (without portable.txt and --config it is ignored with a warning in the log).
//
// INI format: UTF-8, [section] headers, key = value. Section and key names are case-insensitive.
// Comments: a line that starts with ';' or '#', and an inline comment that starts with ';' or '#'
// preceded by a space or tab outside double quotes (the rule of Python configparser with
// inline_comment_prefixes, used by tools/m1_check.py). Values may be quoted ("..."); quote a value
// that itself contains " ;" or " #". Relative paths in the INI are resolved against the INI file's
// directory, on the command line against the current directory. Pass paths with spaces/commas/
// parentheses (e.g. the ISO) through the INI file.
//
// Every key is checked against the schema table of core/config_schema.cpp (M3c; before: kKnownKeys in
// config.cpp): an unknown key in a known section and a value that is not a valid boolean/number/choice/
// resolution or lies outside an enforced range are configuration errors (exit code 1); an unknown section
// only produces a warning in the log. The same table produces `futurama.exe --print-config-schema=<file>`
// (core/cli_tools.h) with types, defaults, choices and de/en texts for the launcher. Known keys:
//
//   [paths]
//   iso      = Y:\xbox games\...\Futurama (Europe) (En,Fr,De,Es,It).iso   ; disc image (D:, default.xbe)
//   xbe      = D:\FuturamaDecomp\orig\retail_eu\default.xbe              ; optional extracted XBE
//   userdata = D:\FuturamaDecomp\run\myagent         ; default %LOCALAPPDATA%\FuturamaPC
//   disc_dir = D:\extracted_disc                     ; optional D: from a directory (without iso)
//   [debug]
//   stop_at  = main            ; or 0x<guest address>; hook logs and exits with code 0
//   selftest = 1               ; run the self-tests of design 1.4 step 8
//   trace    = krnl,vfs        ; channels raised to level trace ("all" = every channel)
//   msgbox   = 0               ; message box on fatal and stage errors (default: only without stderr)
//   timeout  = 60              ; watchdog in seconds, 0 = off (exit code 5)
//   minidump = 1               ; minidump on fatal errors (default 1)
//   mmio_trap = 1              ; reserve the Xbox MMIO range 0xFD000000.. as NOACCESS (default 1)
//   crash_test = 0             ; unhandled guest AV right before entry (tests the crash path)
//   exit_hang_test = 0         ; (M2 review) an exit reporter that never returns: the watchdog must end
//                              ; the orderly exit with exit code 5 (tests the watchdog during the exit)
//   frames   = 3000            ; (gfx, M2) end with exit code 0 after N displayed frames, 0 = off
//   [kernel]
//   strict   = 1               ; UNIMPLEMENTED kernel stub -> debug break + abort
//   min_guest_stack = 0x200000 ; stack reserve of guest threads (design 1.7)
//   fiber_stack = 0x80000      ; (fiber, M2) minimum committed size of MmCreateKernelStack fiber stacks
//                              ; (design 1.7/E9; 0x10000..0x4000000, out-of-range values are clamped)
//   [log]
//   level    = info            ; default level of all channels
//   console  = 1               ; stderr/parent console (default 1 in Debug builds)
//   debugger = 1               ; OutputDebugString (default 1 in Debug builds)
//   [hooks]
//   disable  = 0x13F250,NiUDSStream::*   ; design 3.2: name, prefix* or address (any 0x form)
//   [settings]
//   language = english         ; english|japanese|german|french|spanish|italian|korean|tchinese|
//                              ; portuguese or 1..9 (anything else: exit code 1)
//   game_region / av_region / video_flags / audio_flags / av_pack = <number>
//   [saves]
//   key_mode = fixed           ; fixed | xbox (design 4.5; anything else: exit code 1)
//   signature_key / xbox_cert_key = <32 hex digits>
//   [render]                   ; (gfx module M2, src/host/gfx/gfx_config.h)
//   frame_rate = 60            ; game frames per second (M3b: default = [game] tick_rate, was 50; 0 = like
//                              ; pacing = off); another value than the tick rate logs a warning
//   pacing = auto              ; (M3b, platform/frame_pacer.h) auto | vsync | timer | off: auto = Present(k)
//                              ; on a refresh of k x frame_rate, else timer + tearing (VRR) or one frame per
//                              ; refresh, free-running timer in the background; vsync/timer/off override it
//   pacing_trace = <file>      ; (M3b) CSV with one line per frame (pacing diagnosis), default off
//   resolution = auto          ; (M3c, renderer) auto | <W>x<H>: internal render size; auto = the largest 4:3
//                              ; size inside the window/display (pillar-/letterbox)
//   render_scale = 1.0         ; (M3c) 1.0..2.0 supersampling factor on top (UI step 0.25)
//   msaa = 4                   ; (M3c) off | 2 | 4 | 8 (clamped to the device's support, logged)
//   alpha_to_coverage = on     ; (M3c) boolean, only effective with MSAA
//   anisotropy = 16            ; (M3c) off | 2 | 4 | 8 | 16
//                              ; "classic" = resolution 640x480, render_scale 1, msaa off, anisotropy off (M3)
//   d3d_traps = abort          ; abort | log (a hit D3D trap logs and returns 0 with the right purge;
//                              ; anything else: exit code 1)
//   load_textures = 1          ; NiPCRenderer slot 65 reads texture files like FUN_0015E010 (0: such
//                              ; textures get the 8x8 default texture)
//   backend = d3d11            ; (M3) d3d11 | null - d3d11 = window + D3D11 (default since the M3
//                              ; integration), null = M2 null renderer (no window, nothing drawn)
//   vsync = 1                  ; (M3) Present with sync interval 1
//   debug_layer = auto         ; (M3) auto | on | off (auto: on in Debug builds if installed)
//   adapter = hardware         ; (M3) hardware | warp
//   window_scale = 2           ; (M3) initial client area = 640x480 * N (1..8)
//   fullscreen = 0             ; (M3) borderless window on the whole monitor
//   screenshot_dir = <dir>     ; (M3) PNG screenshots, default <userdata>\screenshots
//   screenshot_frames = 100, 900 ; (M3) automatic screenshots at these displayed frames
//   trace_frames = 900         ; (M3) log every draw call of these displayed frames (GFX/info)
//   validate = 0               ; (M3 phase 2) the null renderer runs ni_extract/texconv on every call and
//                              ; logs result counts at the end (real-data check without D3D)
//   [audio]                    ; (M2, src/host/audio/dsound_stubs.h)
//   dummy_streams = 0          ; 1: DirectSoundCreateStream returns stub streams (packets complete after
//                              ;    their play time) instead of NULL; 0 = design default *ppStream = NULL
//   [game]                     ; (M3b, src/host/xapi/game_timing.h)
//   tick_rate = 60             ; 60 | 50 (anything else: exit code 1). 60 = the NTSC timing values of
//                              ; Game::Init 0xA26A0 with PAL content (CApp+0xE8 = 0, movie tag "PAL"),
//                              ; 50 = original PAL timing (comparisons with the PAL xemu captures)
//   frame_clock = lockstep     ; lockstep | realtime (anything else: exit code 1). Time samples of
//                              ; Game::Frame: lockstep = +1 step per frame while the frames keep the tick
//                              ; rate (whole-step corrections beyond 3/4 step), realtime = original QPC
//
// Command line (wins over the INI):
//   --config=<file> --userdata=<dir> --iso=<file> --xbe=<file> --stop-at=<main|0xADDR> --selftest
//   --trace=<channels> --strict --log-level=<level> --set=<section>.<key>=<value> (any known key)
//   --frames=<N> (= [debug] frames, gfx M2)
//
// Tool modes (M3c, core/cli_tools.h; handled before the configuration is loaded, the XBE is never mapped):
//   --print-config-schema=<file.json>   --check-iso=<image> --result=<file.json>   --query-gpu=<file.json>
//   --print-paths=<file.json> [--config=<file>]
//
// Other modules read their keys generically, e.g.
//   core::GetConfig().GetString("settings", "language", "english")
// A new key must be added to the schema table (core/config_schema.cpp); core::ConfigKeyDefault(section, key)
// returns its default.
#pragma once

#include "shared/launcher_info.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace core {

// ---- player configuration location (M3c, shared contract item 4) ----------------------------------------------
enum class ConfigLocationMode : std::uint8_t { Default, Portable, Explicit };

inline constexpr wchar_t kConfigFileName[] = L"futurama.ini";
inline constexpr wchar_t kPortableMarkerName[] = L"portable.txt";  // next to futurama.exe -> portable mode
inline constexpr wchar_t kUserdataFolderName[] = L"FuturamaPC";    // below %LOCALAPPDATA%

struct ConfigLocation {
    ConfigLocationMode mode = ConfigLocationMode::Default;
    bool portable = false;                  // portable.txt lies next to futurama.exe (also with --config)
    std::filesystem::path configFile;       // INI to read (default/portable: may not exist)
    std::filesystem::path defaultUserdata;  // userdata unless [paths] userdata is set (empty: %LOCALAPPDATA% unset)
};

// Where the configuration and the user data live for an exe in `exeDir`; `explicitConfig` = --config (absolute,
// empty if not given). Pure lookup: checks for portable.txt and reads %LOCALAPPDATA%, touches no other file.
ConfigLocation ResolveConfigLocation(const std::filesystem::path& exeDir, const std::filesystem::path& explicitConfig);
const char* ConfigLocationModeName(ConfigLocationMode mode) noexcept;  // "default" | "portable" | "explicit"

struct Config {
    std::filesystem::path exePath;  // futurama.exe
    std::filesystem::path exeDir;
    std::filesystem::path configFile;  // INI actually loaded (empty if none)
    // (M3c) how configFile / the userdata default were chosen (ResolveConfigLocation)
    ConfigLocationMode configMode = ConfigLocationMode::Default;
    bool portable = false;

    // [paths]
    std::filesystem::path iso;       // empty if not configured
    std::filesystem::path xbe;       // empty if not configured
    std::filesystem::path userdata;  // always set (default %LOCALAPPDATA%\FuturamaPC)

    // [debug]
    std::string stopAt;  // "", "main" or "0x..." (validated by core)
    bool selftest = false;
    std::vector<std::string> trace;  // lower-case channel names or "all"
    bool msgbox = false;

    // [kernel]
    bool strict = false;

    // [log]
    std::string logLevel = "info";
    bool logConsole = false;
    bool logDebugger = false;

    // [hooks]
    std::vector<std::string> hooksDisable;

    // Warnings found while loading (unknown sections); logged by HostMain once the log is open.
    std::vector<std::string> warnings;

    // Every key from INI and command line: "section.key" (lower case) -> raw value.
    // Keys outside any section are stored without prefix ("key").
    std::map<std::string, std::string, std::less<>> values;

    // Generic access ("section.key", falling back to a section-less "key").
    bool Has(std::string_view section, std::string_view key) const;
    std::string GetString(std::string_view section, std::string_view key, std::string_view def = {}) const;
    bool GetBool(std::string_view section, std::string_view key, bool def) const;
    std::uint32_t GetU32(std::string_view section, std::string_view key,
                         std::uint32_t def) const;  // dec or 0x
    std::vector<std::string> GetList(std::string_view section,
                                     std::string_view key) const;  // comma separated
    // Relative values are resolved against the INI directory (or the exe directory without INI).
    std::filesystem::path GetPath(std::string_view section, std::string_view key,
                                  const std::filesystem::path& def = {}) const;

    // Derived locations (design 4.1).
    std::filesystem::path LogDirectory() const { return userdata / "logs"; }
    std::filesystem::path Partition1Directory() const { return userdata / "hdd" / "partition1"; }
    std::filesystem::path CacheDirectory() const { return userdata / "cache"; }
};

// Parses command line and INI into `out`. Returns false with a message on errors (unknown option,
// missing --config file, unreadable INI, bad value). The INI is found with ResolveConfigLocation.
bool LoadConfig(const wchar_t* commandLine, const std::filesystem::path& exePath, Config& out,
                std::string& error);

// True if [section] key is in the schema table (core/config_schema.h; before M3c: config.cpp kKnownKeys).
bool IsKnownConfigKey(std::string_view section, std::string_view key);
// (M2 review) The allowed values of a choice key ("fixed|xbox", "english|...|1..9"), nullptr for keys of
// other types. Tests compare them with the modules that parse the key.
const char* ConfigKeyChoices(std::string_view section, std::string_view key);

// Win32 long path support of this process (LongPathsEnabled = 1 and the launcher manifest's
// longPathAware). Without it LoadConfig rejects a userdata path longer than this many characters,
// because log files, minidumps and saves lie up to 57 characters deeper (review finding 11).
bool ProcessLongPathsEnabled() noexcept;
inline constexpr std::size_t kMaxUserdataWithoutLongPaths = 200;

// Process-wide configuration, valid after HostMain stage 1 (core/host_main.cpp).
const Config& GetConfig() noexcept;
void SetConfig(Config config);

// Launcher handover (valid for the whole process lifetime after HostMain entry).
void SetLauncherInfo(const LauncherInfo* info) noexcept;
const LauncherInfo& GetLauncherInfo() noexcept;
// TlsAlloc index of the given slot (< 64); the guest reads it as fs:[0xE10 + 4*index].
std::uint32_t TlsSlotIndex(FutTlsSlot slot) noexcept;

// UTF-8 <-> UTF-16 helpers (Windows APIs, never throw on bad input: invalid sequences become U+FFFD).
std::string WideToUtf8(std::wstring_view text);
std::wstring Utf8ToWide(std::string_view text);

}  // namespace core
