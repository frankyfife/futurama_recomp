// G4: start level and player character selection ([game] start_level, [game] play_as;
// analysis/loader/g4_select.md, docs/g4_report.md).
//
// The game has a hidden debug level select (cheat codes, FUN_0002C890 -> FUN_0002C7D0 -> FUN_0002C530): for code i
// < 22 it takes the start state text of row i of the table 0x2D60BC (8-byte rows {char* state, char* label}, e.g.
// "Level=Level1-2\nHealth=100.0\nAmmo=100.0\nPickup=Hammer\nEquip=Pistol\n"), extracts the level with FUN_000C1D80,
// sets it with FUN_000A2340 (thiscall CApp [0x2E7B84], strncpy 8 into CApp+0xAC) and stores the state text with
// FUN_000C1D30 (cdecl(text, size incl. NUL) -> DAT_002EA988, applied to the player by FUN_000C5340 after loading).
//
// [game] start_level = <LevelN-M>: probe hook on FUN_000A2340 (prologue 8B 44 24 04 85 C0); only the call of the
//   main menu entry "Neues Spiel" (FUN_00020090, push 0x2940C0 "Level1-1" at 0x200AE, return address 0x200B8) is
//   changed: the state text of the chosen table row goes to FUN_000C1D30 and its level name to the original
//   FUN_000A2340 - the same two calls as the cheat path. Every other caller (cheat, load game, level end) passes.
// [game] play_as = fry|bender|leela (experimental): the player character is data, not code. defs.nif has a node
//   "CharacterDef::<name>"; the def loader FUN_000C5B40 builds a PlayerDef (ctor FUN_0005E580, model
//   "Chars\<name>.ucf") and its Spawn FUN_0005E620 picks the player class by that name (Fry 0x29E1E8, Bender
//   0x29D8E0, Leela 0x29F0C0, Rodent 0x2A0DB0). The stage instance "<def>|<idx>::PLAYER" finds its def with
//   FindDef FUN_000C3E80 (list [lvl+0x34]); the fallback of Stage::Load uses FindCharDef FUN_000C3F20 ("fry").
//   Three probe hooks rename the player def: the PlayerDef ctor (prologue 8B 44 24 04 56) and both lookups
//   (prologue 53 8B 5C 24 08) map a player def name of another character to "fry", "bender" or "leela".
//   Levels whose design needs a character-bound object keep the original character (IsPlayAsBlockedLevel).
//   While a def is renamed, Pickup=/Equip= lines (Fry's weapons) are removed from the pending state text.
//
// Both keys default to "original": then the four hooks are not installed at all (core::HookDesc::condition),
// the game is unchanged and the hook lines of the log are identical to a run without these keys.
#pragma once

#include "core/config.h"
#include "core/guest.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace xapi::select {

// Guest addresses (retail EU XBE; g4_select.md 2-4).
inline constexpr guest::GuestAddr kSetLevelName = 0x000A2340;       // thiscall(CApp, const char*) ret 4
inline constexpr guest::GuestAddr kNewGameSetLevelReturn = 0x000200B8;  // FUN_00020090 after push 0x2940C0
inline constexpr guest::GuestAddr kSetPendingState = 0x000C1D30;    // cdecl(const char* text, size incl. NUL)
inline constexpr guest::GuestAddr kStartStateTable = 0x002D60BC;    // 22 rows {char* state, char* label}
inline constexpr std::uint32_t kStartStateRows = 22;
inline constexpr guest::GuestAddr kPlayerDefCtor = 0x0005E580;      // thiscall(def, const char* name) ret 4
inline constexpr guest::GuestAddr kLevelFindDef = 0x000C3E80;       // thiscall(level, const char* name) ret 4
inline constexpr guest::GuestAddr kLevelFindCharDef = 0x000C3F20;   // thiscall(level, const char* name) ret 4
inline constexpr guest::GuestAddr kLevelPtr = 0x002DABC4;           // level object; +0xC char* level name
inline constexpr std::uint32_t kLevelNameOffset = 0x0C;
inline constexpr guest::GuestAddr kPendingStateVar = 0x002EA988;    // char* state text, +4 size
inline constexpr guest::GuestAddr kCheckpointStateVar = 0x002EA990;  // char* copy for restarts, +4 size
// FUN_000C26E0 Level::BuildStateText thiscall(level, char** out) ret 4: out == 0 -> the player's state (Health,
// Ammo, Pickup/Equip, Lives, Age...) replaces DAT_002EA988 (level end, FsChangeLevel); out != 0 -> "Level=<CApp+0xAC>\n"
// plus the pending text if one exists, else plus the player's state; *out = a FUN_000A4900 block (save game path).
inline constexpr guest::GuestAddr kBuildStateText = 0x000C26E0;
inline constexpr guest::GuestAddr kGuestFree = 0x000A3880;  // cdecl(void*) - the game's free (FUN_000A4900 blocks)
inline constexpr std::uint32_t kLevelPlayerOffset = 0x94;   // [level+0x94] player object (0 while loading)

enum class Character : std::uint8_t { None, Fry, Bender, Leela, Rodent };
enum class PlayAs : std::uint8_t { Original, Fry, Bender, Leela };

// One selectable start level, in game order (the order of the cheat codes 38538585NN).
struct StartLevel {
    const char* level;      // folder/level name, e.g. "Level1-7" (value of [game] start_level)
    std::uint32_t row;      // row of the table 0x2D60BC
    Character character;    // the level's own player character
};

// The 22 levels the game reaches, in game order (Level3-5 is unreachable and not offered).
std::span<const StartLevel> StartLevels() noexcept;
// "original" (or empty) -> true with out = nullptr; a level name (case-insensitive) -> its row.
bool ParseStartLevel(std::string_view text, const StartLevel*& out) noexcept;

bool ParsePlayAs(std::string_view text, PlayAs& out) noexcept;  // original|fry|bender|leela, case-insensitive
const char* PlayAsName(PlayAs value) noexcept;
const char* CharacterName(Character value) noexcept;

// Character of a player def name as the game's Spawn FUN_0005E620 decides it (case-insensitive): fry, railgunfry,
// fry-1-1 .. fry-1-6, fry-5-3 -> Fry; bender; leela; rodent; anything else -> None.
Character PlayerDefCharacter(std::string_view defName) noexcept;
// Levels that keep their character with play_as (level-bound objects; g4_select.md 4.3/4.4): Level1-6 (Fry's
// Chicken Walker), Level2-5 (Bender's cannon), Level4-1 (Zoidberg's rat), Level5-3 (Fry's railgun). Returns the
// reason or nullptr.
const char* PlayAsBlockedReason(std::string_view levelName) noexcept;
inline bool IsPlayAsBlockedLevel(std::string_view levelName) noexcept {
    return PlayAsBlockedReason(levelName) != nullptr;
}
// The def name that replaces `defName` in level `levelName` for `playAs`, or nullptr (keep it): only player defs of
// Fry, Bender or Leela whose character differs from the target, and only outside the blocked levels.
const char* MapPlayerDefName(std::string_view defName, std::string_view levelName, PlayAs playAs) noexcept;
// The state text without its Pickup= and Equip= lines (weapons of Fry; other lines unchanged).
std::string StripInventoryLines(std::string_view state);

// The start level of a level name (case-insensitive), nullptr if it is not one of the 22.
const StartLevel* FindStartLevel(std::string_view level) noexcept;
// True if play_as replaces the player character `character` in level `levelName` (its weapons are then stripped).
bool PlayAsReplacesCharacter(Character character, std::string_view levelName, PlayAs playAs) noexcept;

// G7 level skip: the state text FsChangeLevel built from the player's inventory, completed with the weapons of the
// target level's start state (table row): weapons the player has stay; table Pickup=/Equip= weapons that are missing
// are added (names case-insensitive, no duplicates); the table's Equip= becomes Equip= only if the state has no
// Equip= line (if the player owns that weapon unequipped, his line becomes Equip=), else Pickup=. New lines go after
// the last weapon line, else before Lives=/AgeSeconds=/AgeHours=, else at the end. If a weapon was added, every
// ammo slot gets at least the table's Ammo= value (FUN_000C5340 sets missing slots to 0, a pistol without ammo is no
// weapon); the state's Ammo lines are then rewritten as "Ammo=<slot>|<value>" for the 5 slots.
struct WeaponMerge {
    std::string text;
    std::string added;    // names of added weapons, comma separated ("" = none)
    std::string equipped; // weapon the merge made the equipped one ("" = none)
    bool ammoRaised = false;
    bool Changed() const noexcept { return !added.empty() || !equipped.empty() || ammoRaised; }
};
WeaponMerge MergeStartWeapons(std::string_view state, std::string_view table);

// Guest side (guest main thread). After the level skip's FsChangeLevel(nextLevel): merges the table weapons of
// `nextLevel` into the pending state DAT_002EA988 (FUN_000C1D30) unless play_as replaces that level's character.
void EnsureStartWeapons(std::string_view nextLevel) noexcept;
// The player's current state as the game would save it (FUN_000C26E0 with an output buffer), "" without a player or
// while a start state is still pending.
std::string PlayerStateText();
// Called once per logic tick (xapi::RunPendingGameActions): when the pending start state of a level was consumed by
// FUN_000C5340, logs the player's resulting state once ("game: start state applied in <Level>: ...").
void ObserveStartState() noexcept;

// [game] start_level / play_as of the configuration (invalid values: a warning, then "original").
const StartLevel* StartLevelFromConfig(const core::Config& config) noexcept;
PlayAs PlayAsFromConfig(const core::Config& config) noexcept;

}  // namespace xapi::select
