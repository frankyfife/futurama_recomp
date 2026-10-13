// G8: cheats - [cheats] invulnerable, one_hit_kill, enemies_ignore, ingame_toggles (analysis/loader/g8_cheats.md,
// docs/g8_cheats_report.md). Default: all off, no hook installed (FUT_PROBE_IF, core::HookDesc::condition).
//
// Invulnerable (probe:Player::TakeDamage 0x51AD0, probe:Script::FsDeductLives 0xC1240):
//   Every character's damage goes through its vtable slot +0x7C, thiscall(this, float amount, Actor* attacker)
//   ret 8. The five player classes (0x29A1E8 base, Fry 0x29E1E8, Bender 0x29D8E0, Leela 0x29F0C0, Rodent 0x2A0DB0)
//   share FUN_00051AD0, which calls the actor base Actor::TakeDamage FUN_000354B0 (health +0x4F4 -= amount; dead at
//   <= 0). The probe skips the call: no health loss, no hit reaction, no rumble - for enemies, projectiles,
//   explosions, damaging floors (material flag 4, 0x38ACE) and script damage (FsDamageCharacter, opcode 0x70). This
//   covers Fry's Chickenwalker (1-6), Bender's cannon (2-5) and Zoidberg's rat (4-1): their hits reach the player
//   object. Only the instant-death floors (material flag 8: bottomless pits, deadly water/lava; call 0x38C46, return
//   0x38C49, amount = full health) still kill - exactly like the game's own god mode (cheat code 3853856705, health
//   55555, FUN_000354B0 ignores everything but an amount of 55555). The level script then respawns the player at the
//   last checkpoint; the life it deducts with FsDeductLives (opcode 0x149, FUN_000C1240) is kept.
//
// One-hit kill (probe:Actor::TakeDamage 0x354B0 and the enemy classes' own +0x7C functions 0x47240, 0x60080,
//   0x80300, 0x86520, 0x88530, 0x90570): a hit by a player character on a non-player character becomes lethal
//   (amount = current health) before the class's own "killing blow" check (pickup/score event FUN_000C2EE0 etc.,
//   called when amount >= health) runs. Excluded (amount unchanged): characters with the game's invulnerability
//   marker health 55555 (scripts' SetBigHealth, protected defs like sungod, igner_boss, statuehead, crocodile,
//   plants) and maximum health >= 50000 (tentacle_dude 55000). Bosses (maximum health >= 1000: Destructor 10000,
//   chickenwalker 2000, tank 1000) take a quarter of their maximum health per hit instead (four hits) so that
//   scripted boss phases still run one after the other.
//
// Enemies ignore me (probe:AI::BrainUpdate 0x2ECB0): the game's AI already has a "hold fire" flag, byte 0x2DBAA0,
//   written by the player's own brain every tick (= player input disabled, +0x630 == 0: cut scenes). While it is
//   set, the perception FUN_0002E2E0 finds no target and the attack state FUN_0002F720 does not attack a player
//   character. The probe sets it to 1 after every brain update. Enemies that the player hits still turn towards him
//   (FUN_0002DEC0) but do not attack; the bosses' own attack code (Destructor +0x774 == 0xE, sun god 0xC) does not
//   read the flag and keeps fighting.
//
// In-game toggles ([cheats] ingame_toggles): the D-pad sequences of input/cheat_toggles.h flip the three switches at
// run time (Toggle); with the option on, all hooks are installed (each passes through while its switch is off).
// Feedback: the game's own cheat confirmation (camera colour flash FUN_00013130 on [[0x2DABC4]+0x8C], green = on,
// red = off; run at the next tick heartbeat) plus a short rumble pulse (input side) and the log line
// "cheat: <name> on|off ...".
#pragma once

#include "core/config.h"
#include "core/guest.h"

#include <cstdint>
#include <string_view>

namespace xapi::cheats {

// ---- guest addresses (retail EU XBE; g8_cheats.md) --------------------------------------------------------------
inline constexpr guest::GuestAddr kPlayerTakeDamage = 0x00051AD0;  // player vtables +0x7C, thiscall ret 8
inline constexpr guest::GuestAddr kActorTakeDamage = 0x000354B0;   // actor base, thiscall(amount, attacker) ret 8
inline constexpr guest::GuestAddr kNpcTakeDamage = 0x00047240;     // NPC base vtable 0x297468 +0x7C (and thunks)
inline constexpr guest::GuestAddr kKamikazeTakeDamage = 0x00060080;  // vtable 0x29B7E8
inline constexpr guest::GuestAddr kAquafishTakeDamage = 0x00080300;  // vtable 0x29D7B0
inline constexpr guest::GuestAddr kGoonTakeDamage = 0x00086520;      // vtables 0x29E3D0/0x29E510/0x29E640/0x29E790
inline constexpr guest::GuestAddr kJunkbotTakeDamage = 0x00088530;   // vtables 0x29ECF8/0x29EE38
// vtables 0x2A00B0 (stormtrooper_mk2) and 0x2A01F0 (stormtrooper)
inline constexpr guest::GuestAddr kStormtrooperTakeDamage = 0x00090570;
inline constexpr guest::GuestAddr kDeductLives = 0x000C1240;       // FsDeductLives, cdecl no args (opcode 0x149)
inline constexpr guest::GuestAddr kAiBrainUpdate = 0x0002ECB0;     // thiscall(brain) -> al, plain ret
inline constexpr guest::GuestAddr kKillFloorReturn = 0x00038C49;   // FUN_00038880: player on a material with flag 8
inline constexpr guest::GuestAddr kAiHoldFireVar = 0x002DBAA0;     // u8, read by 0x2E2E0 (0x2E300) and 0x2F720 (0x2FA84)
inline constexpr guest::GuestAddr kLivesVar = 0x002D67DC;          // s32 (FUN_000C1220/1230)
inline constexpr guest::GuestAddr kCameraFlash = 0x00013130;       // thiscall(camera, r, g, b, a, in, hold, out) ret 0x1C
inline constexpr guest::GuestAddr kSessionPtr = 0x002DABC4;        // level session
inline constexpr std::uint32_t kSessionCamera = 0x8C;
inline constexpr std::uint32_t kSessionPlayer = 0x94;
inline constexpr std::uint32_t kSessionPlayers = 0x98;             // list of player characters, link +0xC4C
inline constexpr std::uint32_t kPlayerNext = 0xC4C;
inline constexpr std::uint32_t kActorHealth = 0x4F4;               // float
inline constexpr std::uint32_t kActorMaxHealth = 0x4FC;            // float (def "maxhealth")
inline constexpr std::uint32_t kActorName = 0x34;                  // char* instance name
inline constexpr std::uint32_t kActorDef = 0x10;                   // def; +0x10 char* def name

// Player classes (g4_select.md 3.2): their +0x7C is kPlayerTakeDamage.
inline constexpr guest::GuestAddr kPlayerVtables[] = {0x0029A1E8, 0x0029E1E8, 0x0029D8E0, 0x0029F0C0, 0x002A0DB0};

// The game's invulnerability marker (god mode cheat, scripts' SetBigHealth, "maxhealth 55555" in chars\*.ucf).
inline constexpr float kProtectedHealth = 55555.0f;
inline constexpr float kProtectedMaxHealth = 50000.0f;  // tentacle_dude 55000 and the like: never killed
inline constexpr float kBossMaxHealth = 1000.0f;        // tank 1000, chickenwalker 2000, Destructor 10000
inline constexpr float kBossHitFraction = 0.25f;        // bosses: a quarter of the maximum health per hit

// ---- options ----------------------------------------------------------------------------------------------------
enum class Cheat : std::uint8_t { Invulnerable, OneHitKill, EnemiesIgnore, Count };
inline constexpr int kCheatCount = static_cast<int>(Cheat::Count);
const char* CheatKey(Cheat c) noexcept;      // INI key: invulnerable | one_hit_kill | enemies_ignore
const char* CheatLabelDe(Cheat c) noexcept;  // "Unverwundbar" ...

struct Settings {
    bool invulnerable = false;
    bool oneHitKill = false;
    bool enemiesIgnore = false;
    bool ingameToggles = false;
    bool Enabled(Cheat c) const noexcept;
    bool AnyHook() const noexcept { return invulnerable || oneHitKill || enemiesIgnore || ingameToggles; }
};
Settings SettingsFromConfig(const core::Config& config) noexcept;
// Hook conditions (FUT_PROBE_IF): a cheat's hooks are installed when it is on at start or can be toggled in game.
bool InvulnerableHooksWanted(const core::Config& config) noexcept;
bool OneHitKillHooksWanted(const core::Config& config) noexcept;
bool EnemiesIgnoreHooksWanted(const core::Config& config) noexcept;

// ---- pure decisions (unit-tested) -------------------------------------------------------------------------------
// Player::TakeDamage while invulnerable: true = skip the call (the damage is lost). Only the instant-death floor
// (return address kKillFloorReturn) and non-positive amounts pass.
bool BlockPlayerDamage(bool invulnerable, float amount, guest::GuestAddr returnAddress) noexcept;

enum class HitKind : std::uint8_t { Unchanged, Lethal, Boss };
struct HitDecision {
    HitKind kind = HitKind::Unchanged;
    float amount = 0.0f;
};
// One-hit kill: the damage a player character's hit on a non-player character should do.
HitDecision OneHitDamage(float amount, float health, float maxHealth, bool attackerIsPlayer,
                         bool targetIsPlayer) noexcept;

// ---- run time ---------------------------------------------------------------------------------------------------
// Reads the options once (idempotent), logs them, registers the exit summary. Called by inp::StartInput; the probes
// call it too.
void Configure() noexcept;
const Settings& ActiveSettings() noexcept;
bool Active(Cheat c) noexcept;
// In-game toggle (input/cheat_toggles.cpp). Flips the switch, logs "cheat: <name> on|off (<sequence>, port N)" and
// queues the screen flash for the next tick. Returns the new state. False (unchanged) if toggles are off.
bool Toggle(Cheat c, int port, std::string_view sequenceText) noexcept;
// Logic tick heartbeat (xapi::RunPendingGameActions): pending flash, player health trace (only with cheat hooks).
void OnTick() noexcept;
// Exit reporter "cheats": "cheats summary: ...".
void LogSummary() noexcept;

struct Stats {
    std::uint32_t blockedHits = 0;      // player damage skipped
    double blockedDamage = 0.0;
    std::uint32_t killFloorDeaths = 0;  // instant-death floor passed through while invulnerable
    std::uint32_t livesKept = 0;        // FsDeductLives skipped
    std::uint32_t lethalHits = 0;       // one-hit kill: hit made lethal
    std::uint32_t bossHits = 0;         // one-hit kill: boss hit raised
    std::uint32_t protectedHits = 0;    // one-hit kill: protected target left unchanged
    std::uint32_t holdFireTicks = 0;    // brain updates with the hold-fire flag forced
    std::uint32_t toggles = 0;
};
Stats GetStats() noexcept;

}  // namespace xapi::cheats
