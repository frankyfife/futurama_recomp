// Game observer probes (M2 integration, design 6 M2 exit criteria 1-5 and 7): FUT_PROBEs on game code
// that log what the M2 exit check (tools/m2_check.py) needs on log level info, without changing guest
// behaviour, plus the disc error trap. Implementation and evidence: xapi/game_probes.cpp.
//
//   probe:Game::ArchiveMount   FUN_0013FE70  "archive: mounted sysPAL.img as #0 ..." / "... not mounted"
//   probe:Game::ArchiveLookup  FUN_0013FE10  vfs_trace.log line per asset lookup (archive hit or loose)
//   probe:Game::Init           FUN_000A26A0  "game: video standard ...", "game: language ...",
//                                            "game: BUILD.TXT = ...", "game: archives mounted: ..."
//   probe:CApp::Frame          FUN_000D2630  "game: context Intro -> Frontend ..." on every change
//   probe:XAPILIB::XGetLanguage 0xF6440      "XGetLanguage() = 3 (german)" when the value changes
//   trap:Game::DiscErrorScreen 0xF2FD0       TRAP + core::Fatal (the original loops forever)
//
// The exit reporter "game" logs the summary line "game summary: ..." at every orderly process end.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xapi {

struct GameObserverStats {
    std::uint32_t archivesMounted = 0;   // successful mounts (FUN_0013FE70 returned TRUE)
    std::uint32_t archiveMountFailures = 0;
    std::uint64_t archiveLookups = 0;    // FUN_0013FE10 calls with the archive search enabled
    std::uint64_t archiveHits = 0;
    std::uint64_t frames = 0;            // CApp::Frame calls
    std::uint32_t contextSwitches = 0;   // changes of CApp+0xBC seen after CApp::Frame
    std::uint32_t discErrorCalls = 0;    // trap:Game::DiscErrorScreen (fatal unless disabled)
    std::uint32_t language = 0;          // last XGetLanguage() result, 0 = never called
    std::uint32_t languageCalls = 0;
    std::int32_t videoStandard = -1;     // CApp+0xE8 after Game::Init (0 PAL, 1 NTSC), -1 = not yet
    bool gameInitDone = false;
};

GameObserverStats GetGameObserverStats() noexcept;
// Names of the mounted archives in mount (= search) order, as the game passed them.
std::vector<std::string> MountedArchiveNames();
// Context names seen by the CApp::Frame probe, in order (first = context after Game::Init).
std::vector<std::string> ContextHistory();
// "english", "japanese", "german", "french", "spanish", "italian", "korean", "chinese", "portuguese"
// for the XC_LANGUAGE values 1..9 (XGetLanguage, design 2.1 settings row); "?" otherwise.
const char* XboxLanguageName(std::uint32_t value) noexcept;
// Logs "game summary: ..." (GAME/info). Exit reporter "game".
void LogGameSummary() noexcept;

}  // namespace xapi
