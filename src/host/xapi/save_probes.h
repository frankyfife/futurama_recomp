// Save-game observer probes (M7; analysis/loader/m7_saves.md, files.md 4). Observers only: they log what the
// game's save layer does on log level info (channel XAPI, prefix "save: ") and count it for the exit summary,
// so that tools/m7_check.py can follow save, load, damage detection and delete without reading guest memory.
//
// Probes (prologues and log lines: save_probes.cpp, src/INTERFACES.md 4.33):
//   probe:XAPILIB::XCreateSaveGame 0xF6987  "save: XCreateSaveGame(...) -> <error> <directory>"
//   probe:XAPILIB::XDeleteSaveGame 0xF6BEA  "save: XDeleteSaveGame(...) -> 0 (directory removed)"
//   probe:Save::ReadContainer      0xE0210  "save: read <path> -> ok|missing|wrong size|read error"
//   probe:Save::VerifySignature    0xE0340  "save: signature of <path> ok|MISMATCH ..."
//                                           + "save: loaded game data: <text>" for Futurama.dat
//   probe:Save::WriteContainer     0xE0B00  "save: write <file> for <name> (icon ...) -> ok|FAILED"
//                                           + "save: saved game data: <text>" for Futurama.dat
//   probe:Save::OpenSaveMenu       0xE4F70  "save: save menu opened|not opened ..."
//   probe:Save::Dialog             0xDF750  "save: dialog MEMCARD_0xx (MEMHINT_00x)" for every message box
//
// The exit reporter "save" logs "save summary: ...". RequestLevelEnd is the debug action behind the input
// script command `game level_end <NextLevel>`: at the next tick heartbeat it calls the level-script native
// FsChangeLevel 0xB0520, i.e. the game's end-of-level screen "Breakdown" (Speichern / Weiter / Wiederholen /
// Beenden).
#pragma once

#include <cstdint>

namespace xapi {

struct SaveObserverStats {
    std::uint32_t creates = 0;         // XCreateSaveGame calls
    std::uint32_t createFailures = 0;  // ... with a result != 0
    std::uint32_t writes = 0;          // WriteContainer calls
    std::uint32_t writeFailures = 0;
    std::uint32_t reads = 0;          // ReadContainer calls
    std::uint32_t readFailures = 0;   // result != 3 (missing, wrong size, read error)
    std::uint32_t signaturesOk = 0;   // VerifySignature -> 1
    std::uint32_t signaturesBad = 0;  // VerifySignature -> 0
    std::uint32_t deletes = 0;        // XDeleteSaveGame calls
    std::uint32_t deleteFailures = 0;
    std::uint32_t saveMenuOpened = 0;  // OpenSaveMenu -> 1
    std::uint32_t dialogs = 0;         // message boxes of the save layer (SaveDialog ctor)
};

SaveObserverStats GetSaveObserverStats() noexcept;
// Logs "save summary: ..." (XAPI/info). Exit reporter "save".
void LogSaveSummary() noexcept;

// Debug action (input script `game level_end Level1-2`): ends the running level like the level scripts do at
// their end - the native FsChangeLevel(next level) FUN_000B0520 records the level time, sets the next level
// and opens the Breakdown screen, whose entry "Speichern" is the game's only save point (m7_saves.md 1.1).
// Deferred: the call happens on the guest main thread at the next logic tick heartbeat
// (RunPendingGameActions, called by the tick heartbeat probe). Returns false if no level runs (no level
// object) or the name is not 1..8 characters.
bool RequestLevelEnd(const char* nextLevel) noexcept;
// Runs a pending RequestLevelEnd (guest main thread, outside every XPP/router lock).
void RunPendingGameActions() noexcept;

}  // namespace xapi
