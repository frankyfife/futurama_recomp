// Host configuration: futurama.ini (next to futurama.exe, or --config=<file>) + command line.
//
// INI format: UTF-8, [section] headers, key = value. Section and key names are case-insensitive.
// Comments: a line that starts with ';' or '#', and an inline comment that starts with ';' or '#'
// preceded by a space or tab outside double quotes (the rule of Python configparser with
// inline_comment_prefixes, used by tools/m1_check.py). Values may be quoted ("..."); quote a value
// that itself contains " ;" or " #". Relative paths in the INI are resolved against the INI file's
// directory, on the command line against the current directory. Pass paths with spaces/commas/
// parentheses (e.g. the ISO) through the INI file.
//
// Every key is checked against the table kKnownKeys in config.cpp: an unknown key in a known section
// and a value that is not a valid boolean/number/choice (language, key_mode, d3d_traps) are
// configuration errors (exit code 1); an unknown section only produces a warning in the log. Known keys:
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
//   frame_rate = 50            ; DisplayFrame paces to N Hz like the Xbox vblank wait, 0 = unthrottled
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
//
// Command line (wins over the INI):
//   --config=<file> --userdata=<dir> --iso=<file> --xbe=<file> --stop-at=<main|0xADDR> --selftest
//   --trace=<channels> --strict --log-level=<level> --set=<section>.<key>=<value> (any known key)
//   --frames=<N> (= [debug] frames, gfx M2)
//
// Other modules read their keys generically, e.g.
//   core::GetConfig().GetString("settings", "language", "english")
// A new key must be added to kKnownKeys in config.cpp.
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

struct Config {
    std::filesystem::path exePath;  // futurama.exe
    std::filesystem::path exeDir;
    std::filesystem::path configFile;  // INI actually loaded (empty if none)

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
// missing --config file, unreadable INI, bad value).
bool LoadConfig(const wchar_t* commandLine, const std::filesystem::path& exePath, Config& out,
                std::string& error);

// True if [section] key is in the table of known keys (config.cpp kKnownKeys).
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
