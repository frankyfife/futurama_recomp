// Host configuration: futurama.ini (next to futurama.exe, or --config=<file>) + command line.
//
// INI format: UTF-8, [section] headers, key = value, comment lines start with ';' or '#',
// values may be quoted ("..."). Section and key names are case-insensitive. Relative paths in the
// INI are resolved against the INI file's directory, on the command line against the current
// directory. Pass paths with spaces/commas/parentheses (e.g. the ISO) through the INI file.
//
//   [paths]
//   iso      = Y:\xbox games\...\Futurama (Europe) (En,Fr,De,Es,It).iso   ; disc image (D:, default.xbe)
//   xbe      = D:\FuturamaDecomp\orig\retail_eu\default.xbe              ; optional extracted XBE
//   userdata = D:\FuturamaDecomp\run\myagent         ; default %LOCALAPPDATA%\FuturamaPC
//   [debug]
//   stop_at  = main            ; or 0x<guest address>; hook logs and exits with code 0
//   selftest = 1               ; run the self-tests of design 1.4 step 8
//   trace    = krnl,vfs        ; channels raised to level trace ("all" = every channel)
//   msgbox   = 0               ; message box on fatal errors (default: only without stderr)
//   [kernel]
//   strict   = 1               ; UNIMPLEMENTED kernel stub -> debug break + abort
//   [log]
//   level    = info            ; default level of all channels
//   console  = 1               ; stderr/parent console (default 1 in Debug builds)
//   debugger = 1               ; OutputDebugString (default 1 in Debug builds)
//   [hooks]
//   disable  = 0x13F250,NiUDSStream::*   ; design 3.2 (consumed by core/hooks)
//
// Command line (wins over the INI):
//   --config=<file> --userdata=<dir> --iso=<file> --xbe=<file> --stop-at=<main|0xADDR> --selftest
//   --trace=<channels> --strict --log-level=<level> --set=<section>.<key>=<value> (any key)
//
// Other modules read their own keys generically, e.g.
//   core::GetConfig().GetString("settings", "language", "english")
// without changing this header.
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
    std::filesystem::path exePath;     // futurama.exe
    std::filesystem::path exeDir;
    std::filesystem::path configFile;  // INI actually loaded (empty if none)

    // [paths]
    std::filesystem::path iso;       // empty if not configured
    std::filesystem::path xbe;       // empty if not configured
    std::filesystem::path userdata;  // always set (default %LOCALAPPDATA%\FuturamaPC)

    // [debug]
    std::string stopAt;              // "", "main" or "0x..." (validated by core)
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

    // Every key from INI and command line: "section.key" (lower case) -> raw value.
    // Keys outside any section are stored without prefix ("key").
    std::map<std::string, std::string, std::less<>> values;

    // Generic access ("section.key", falling back to a section-less "key").
    bool Has(std::string_view section, std::string_view key) const;
    std::string GetString(std::string_view section, std::string_view key, std::string_view def = {}) const;
    bool GetBool(std::string_view section, std::string_view key, bool def) const;
    std::uint32_t GetU32(std::string_view section, std::string_view key, std::uint32_t def) const;  // dec or 0x
    std::vector<std::string> GetList(std::string_view section, std::string_view key) const;  // comma separated
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
