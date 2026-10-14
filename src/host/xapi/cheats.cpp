// G8: cheats (see cheats.h).
#include "xapi/cheats.h"

#include "xapi/cheats_player.h"

#include "core/config_schema.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace xapi::cheats {
namespace {

constexpr std::uint32_t kHealthTraceLines = 400;  // "cheat: player health" lines per run (then only the summary)
constexpr std::uint32_t kDetailLines = 40;        // per kind of event, then every kDetailEvery-th
constexpr std::uint32_t kDetailEvery = 100;

std::uint32_t ReadU32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}
float ReadF32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<float>(p) : 0.0f;
}
std::int32_t ReadS32(guest::GuestAddr p) noexcept {
    return static_cast<std::int32_t>(ReadU32(p));
}
std::uint32_t FloatBits(float f) noexcept {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

std::string GuestString(guest::GuestAddr p, std::size_t max = 40) {
    std::string s;
    for (std::size_t i = 0; p >= 0x10000 && i < max && guest::IsReadable(p + static_cast<guest::GuestAddr>(i), 1);
         ++i) {
        const char c = guest::Read<char>(p + static_cast<guest::GuestAddr>(i));
        if (c == 0) {
            break;
        }
        s += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    return s;
}

// "defname/instance" of a character, e.g. "stormtrooper/St1Trooper3".
std::string ActorName(guest::GuestAddr actor) {
    if (actor == 0) {
        return "(none)";
    }
    const std::string def = GuestString(ReadU32(ReadU32(actor + kActorDef) + 0x10));
    const std::string inst = GuestString(ReadU32(actor + kActorName));
    if (inst.empty()) {
        return def.empty() ? "?" : def;
    }
    return def + "/" + inst;
}

bool IsPlayerVtable(std::uint32_t vt) noexcept {
    return std::find(std::begin(kPlayerVtables), std::end(kPlayerVtables), vt) != std::end(kPlayerVtables);
}
bool IsPlayerCharacter(guest::GuestAddr actor) noexcept {
    return actor != 0 && IsPlayerVtable(ReadU32(actor));
}

bool Detail(std::uint32_t count) noexcept {
    return count <= kDetailLines || count % kDetailEvery == 0;
}

// ---- state ------------------------------------------------------------------------------------------------------
std::once_flag g_configureOnce;
Settings g_settings;
std::array<std::atomic<bool>, kCheatCount> g_active{};
std::atomic<int> g_pendingFlash{0};  // 0 none, 1 green (on), 2 red (off)

std::mutex g_statsLock;
Stats g_stats;

std::mutex g_watchLock;
LethalBlockWatch g_lethalWatch;  // invulnerable safety net (player damage probe)

double NowSeconds() noexcept {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// OnTick (guest main thread only).
guest::GuestAddr g_tracePlayer = 0;
float g_traceHealth = 0.0f;
std::int32_t g_traceLives = 0;
std::uint32_t g_traceLines = 0;

const char* OnOff(bool v) noexcept {
    return v ? "on" : "off";
}

void EnsureConfigured() noexcept {
    Configure();
}

// ---- probes -----------------------------------------------------------------------------------------------------

// Player::TakeDamage FUN_00051AD0 (vtables 0x29A1E8/0x29E1E8/0x29D8E0/0x29F0C0/0x2A0DB0 +0x7C), thiscall(player,
// float amount, Actor* attacker) ret 8.
std::uint32_t __fastcall PlayerTakeDamageProbe(std::uint32_t player, std::uint32_t /*edx*/, float amount,
                                               std::uint32_t attacker) noexcept {
    EnsureConfigured();
    const guest::GuestAddr from = guest::Addr(_ReturnAddress());
    const bool invulnerable = Active(Cheat::Invulnerable);
    bool block = BlockPlayerDamage(invulnerable, amount, from);
    if (block) {
        // Safety net: a lethal hit that keeps coming (scripted grab/crush kills wait for the death).
        const float health = ReadF32(player + kActorHealth);
        if (IsLethalHit(amount, health)) {
            bool letThrough = false;
            {
                std::lock_guard<std::mutex> lock(g_watchLock);
                letThrough = g_lethalWatch.OnBlockedLethal(attacker, NowSeconds());
            }
            if (letThrough) {
                block = false;
                std::uint32_t n = 0;
                {
                    std::lock_guard<std::mutex> lock(g_statsLock);
                    n = ++g_stats.safetyNetKills;
                }
                try {
                    FUT_LOGI(Game,
                             "cheat: invulnerable - safety net: lethal damage by %s vt 0x%08X blocked for more than "
                             "%.0f s (%.1f damage, health %.1f, call from 0x%08X): let through as an instant death; the "
                             "level script respawns the player at the checkpoint, no life is lost (%u.)",
                             ActorName(attacker).c_str(), ReadU32(attacker), kLethalBlockLimitSeconds,
                             static_cast<double>(amount), static_cast<double>(health), from - 5, n);
                } catch (...) {
                }
            }
        }
    }
    if (block) {
        std::uint32_t n = 0;
        {
            std::lock_guard<std::mutex> lock(g_statsLock);
            n = ++g_stats.blockedHits;
            g_stats.blockedDamage += static_cast<double>(amount);
        }
        if (Detail(n)) {
            try {
                FUT_LOGI(Game,
                         "cheat: invulnerable - %.1f damage to %s blocked (health stays %.1f; attacker %s vt 0x%08X, "
                         "call from 0x%08X; %u. hit)",
                         static_cast<double>(amount), ActorName(player).c_str(),
                         static_cast<double>(ReadF32(player + kActorHealth)), ActorName(attacker).c_str(),
                         ReadU32(attacker), from - 5, n);
            } catch (...) {
            }
        }
        return 0;
    }
    if (invulnerable && from == kKillFloorReturn) {
        std::uint32_t n = 0;
        {
            std::lock_guard<std::mutex> lock(g_statsLock);
            n = ++g_stats.killFloorDeaths;
        }
        FUT_LOGI(Game,
                 "cheat: invulnerable - instant-death floor (pit, deadly water or lava): %.1f damage passes like in "
                 "the game's god mode; the level script respawns the player at the checkpoint, no life is lost (%u.)",
                 static_cast<double>(amount), n);
    }
    if (invulnerable && from == kTentacleKillReturn) {
        std::uint32_t n = 0;
        {
            std::lock_guard<std::mutex> lock(g_statsLock);
            n = ++g_stats.grabKills;
        }
        if (Detail(n)) {
            try {
                FUT_LOGI(Game,
                         "cheat: invulnerable - %s pulls the player through the pipe (grab kill, %.1f damage every "
                         "tick until death): passes like an instant-death floor; respawn at the checkpoint, no life "
                         "is lost (%u.)",
                         ActorName(attacker).c_str(), static_cast<double>(amount), n);
            } catch (...) {
            }
        }
    }
    if (amount > 0.0f) {
        {
            std::lock_guard<std::mutex> lock(g_watchLock);
            if (g_lethalWatch.Active() && g_lethalWatch.Attacker() == attacker) {
                g_lethalWatch.Reset();  // the series ends with the damage that passes
            }
        }
        if (core::LogEnabled(core::Channel::Game, core::Level::Debug)) {
            try {
                FUT_LOGD(Game, "cheat: player damage %.1f to %s passes (health %.1f; attacker %s vt 0x%08X, return 0x%08X)",
                         static_cast<double>(amount), ActorName(player).c_str(),
                         static_cast<double>(ReadF32(player + kActorHealth)), ActorName(attacker).c_str(),
                         ReadU32(attacker), from);
            } catch (...) {
            }
        }
    }
    return core::CallGuestThiscall(core::HookTrampoline(kPlayerTakeDamage), player, {FloatBits(amount), attacker});
}

// Tentacle update FUN_0008C4F0 (vtable 0x29F790 +0x5C), thiscall(tentacle, arg) ret 4. Enemies ignore me: the
// detection (distance < 12 and cooldown +0x8C4 <= 0, 0x8C675) never fires while the cooldown stays above zero.
std::uint32_t __fastcall TentacleUpdateProbe(std::uint32_t tentacle, std::uint32_t /*edx*/,
                                             std::uint32_t arg) noexcept {
    if (tentacle != 0 && Active(Cheat::EnemiesIgnore)) {
        const guest::GuestAddr p = tentacle + kTentacleCooldown;
        if (guest::IsReadable(p, 4)) {
            const float cooldown = guest::Read<float>(p);
            const float hold = TentacleHoldCooldown(cooldown);
            if (FloatBits(hold) != FloatBits(cooldown)) {
                guest::Write<float>(p, hold);
                if (!(cooldown > 0.0f)) {
                    std::uint32_t n = 0;
                    {
                        std::lock_guard<std::mutex> lock(g_statsLock);
                        n = ++g_stats.tentacleHolds;
                    }
                    if (Detail(n)) {
                        try {
                            FUT_LOGI(Game,
                                     "cheat: enemies ignore me - %s does not look for the player (detection cooldown "
                                     "held at %.1f s; %u.)",
                                     ActorName(tentacle).c_str(), static_cast<double>(hold), n);
                        } catch (...) {
                        }
                    }
                }
            }
        }
    }
    const std::uint32_t r = core::CallGuestThiscall(core::HookTrampoline(kTentacleUpdate), tentacle, {arg});
    // [debug] trace = game: positions for test runs (every 31st tentacle update - rotates over the tentacles of the
    // level, about two lines per second).
    static std::atomic<std::uint32_t> calls{0};
    if (tentacle != 0 && core::LogEnabled(core::Channel::Game, core::Level::Trace) && calls++ % 31 == 0) {
        try {
            const guest::GuestAddr session = ReadU32(kSessionPtr);
            const guest::GuestAddr player = session != 0 ? ReadU32(session + kSessionPlayer) : 0;
            alignas(4) float tp[3] = {};
            alignas(4) float pp[3] = {};
            core::CallGuestThiscall(ReadU32(ReadU32(tentacle) + kActorGetPosition), tentacle, {guest::Addr(tp)});
            if (player != 0) {
                core::CallGuestThiscall(ReadU32(ReadU32(player) + kActorGetPosition), player, {guest::Addr(pp)});
            }
            const float dx = pp[0] - tp[0];
            const float dy = pp[1] - tp[1];
            const float dz = pp[2] - tp[2];
            FUT_LOGT(Game,
                     "tentacle trace: %s pos %.2f %.2f %.2f player %.2f %.2f %.2f dist %.2f cooldown %.2f attack %u "
                     "missed %u killing %u grabber 0x%08X",
                     ActorName(tentacle).c_str(), static_cast<double>(tp[0]), static_cast<double>(tp[1]),
                     static_cast<double>(tp[2]), static_cast<double>(pp[0]), static_cast<double>(pp[1]),
                     static_cast<double>(pp[2]), std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz)),
                     static_cast<double>(ReadF32(tentacle + kTentacleCooldown)),
                     guest::Read<std::uint8_t>(tentacle + 0x508), guest::Read<std::uint8_t>(tentacle + kTentacleGrabMissed),
                     guest::Read<std::uint8_t>(tentacle + kTentacleKilling),
                     player != 0 ? ReadU32(player + kPlayerGrabber) : 0u);
        } catch (...) {
        }
    }
    return r;
}

// Tentacle animation events FUN_0008CAD0 (vtable 0x29F790 +0xE8), thiscall(tentacle, arg, const char* event) ret 8.
// "morph:grab" with invulnerable or enemies ignore me: the game's own miss path (+0x8C0 = 1 -> retract animation).
std::uint32_t __fastcall TentacleAnimEventProbe(std::uint32_t tentacle, std::uint32_t /*edx*/, std::uint32_t arg,
                                                std::uint32_t event) noexcept {
    const bool invulnerable = Active(Cheat::Invulnerable);
    const bool ignore = Active(Cheat::EnemiesIgnore);
    std::string name;
    try {
        name = GuestString(event, 32);
        for (char& c : name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));  // the game compares with _stricmp
        }
    } catch (...) {
    }
    if (tentacle != 0 && (invulnerable || ignore)) {
        const bool skip = SkipTentacleGrab(invulnerable, ignore, name);
        if (skip) {
            guest::Write<std::uint8_t>(tentacle + kTentacleGrabMissed, 1);
            std::uint32_t n = 0;
            {
                std::lock_guard<std::mutex> lock(g_statsLock);
                n = ++g_stats.grabsPrevented;
            }
            if (Detail(n)) {
                try {
                    const guest::GuestAddr session = ReadU32(kSessionPtr);
                    const guest::GuestAddr player = session != 0 ? ReadU32(session + kSessionPlayer) : 0;
                    FUT_LOGI(Game,
                             "cheat: %s - %s grab skipped (morph:grab -> miss, the tentacle retracts; player %s "
                             "grabbed by 0x%08X; %u.)",
                             ignore ? "enemies ignore me" : "invulnerable", ActorName(tentacle).c_str(),
                             ActorName(player).c_str(), player != 0 ? ReadU32(player + kPlayerGrabber) : 0u, n);
                } catch (...) {
                }
            }
            return 0;
        }
    }
    const std::uint32_t r = core::CallGuestThiscall(core::HookTrampoline(kTentacleAnimEvent), tentacle, {arg, event});
    // Evidence: the grab sequence as the game runs it (only with cheat hooks; a few lines per attack).
    if (tentacle != 0 && core::LogEnabled(core::Channel::Game, core::Level::Debug)) {
        try {
            FUT_LOGD(Game, "cheat: tentacle %s event %s", ActorName(tentacle).c_str(), name.c_str());
        } catch (...) {
        }
    }
    if (tentacle != 0 && (name == "morph:grab" || name == "morph:release" || name == "morph:pull_through_pipe")) {
        static std::atomic<std::uint32_t> events{0};
        const std::uint32_t n = ++events;
        if (Detail(n)) {
            try {
                const guest::GuestAddr session = ReadU32(kSessionPtr);
                const guest::GuestAddr player = session != 0 ? ReadU32(session + kSessionPlayer) : 0;
                FUT_LOGI(Game, "cheat: tentacle %s event %s (player grabbed by %s, missed %u, killing %u; %u.)",
                         ActorName(tentacle).c_str(), name.c_str(),
                         ActorName(player != 0 ? ReadU32(player + kPlayerGrabber) : 0u).c_str(),
                         guest::Read<std::uint8_t>(tentacle + kTentacleGrabMissed),
                         guest::Read<std::uint8_t>(tentacle + kTentacleKilling), n);
            } catch (...) {
            }
        }
    }
    return r;
}

// Script native FsDeductLives FUN_000C1240 (opcode 0x149, the level scripts' respawn routine), cdecl, no args.
std::uint32_t __cdecl DeductLivesProbe() noexcept {
    EnsureConfigured();
    if (Active(Cheat::Invulnerable)) {
        std::uint32_t n = 0;
        {
            std::lock_guard<std::mutex> lock(g_statsLock);
            n = ++g_stats.livesKept;
        }
        FUT_LOGI(Game, "cheat: invulnerable - life kept (FsDeductLives skipped, lives stay %d; %u.)",
                 ReadS32(kLivesVar), n);
        return 0;
    }
    return core::CallGuest(core::HookTrampoline(kDeductLives));
}

// Protected targets are counted once, in the catch-all (every class ends there).
bool CountsProtected(guest::GuestAddr hook) noexcept {
    return hook == kActorTakeDamage;
}

// One-hit kill: the +0x7C functions of the enemy classes (entry, before their "killing blow" check) and the actor
// base FUN_000354B0 (catch-all), all thiscall(actor, float amount, Actor* attacker) ret 8.
template <guest::GuestAddr Address>
std::uint32_t __fastcall EnemyTakeDamageProbe(std::uint32_t actor, std::uint32_t /*edx*/, float amount,
                                              std::uint32_t attacker) noexcept {
    EnsureConfigured();
    float use = amount;
    if (Active(Cheat::OneHitKill) && actor != 0) {
        const bool attackerIsPlayer = IsPlayerCharacter(attacker);
        const bool targetIsPlayer = IsPlayerCharacter(actor);
        const float health = ReadF32(actor + kActorHealth);
        const float maxHealth = ReadF32(actor + kActorMaxHealth);
        const HitDecision d = OneHitDamage(amount, health, maxHealth, attackerIsPlayer, targetIsPlayer);
        if (d.kind != HitKind::Unchanged) {
            use = d.amount;
            std::uint32_t n = 0;
            {
                std::lock_guard<std::mutex> lock(g_statsLock);
                n = d.kind == HitKind::Lethal ? ++g_stats.lethalHits : ++g_stats.bossHits;
            }
            if (Detail(n)) {
                try {
                    FUT_LOGI(Game,
                             "cheat: one-hit kill - %s hit by %s: %.1f -> %.1f damage (health %.1f of %.1f%s; "
                             "0x%08X, %u.)",
                             ActorName(actor).c_str(), ActorName(attacker).c_str(), static_cast<double>(amount),
                             static_cast<double>(use), static_cast<double>(health), static_cast<double>(maxHealth),
                             d.kind == HitKind::Boss ? ", boss: a quarter of the maximum per hit" : "", Address, n);
                } catch (...) {
                }
            }
        } else if (CountsProtected(Address) && attackerIsPlayer && !targetIsPlayer && amount > 0.0f &&
                   health > 0.0f && amount < health) {
            std::uint32_t n = 0;
            {
                std::lock_guard<std::mutex> lock(g_statsLock);
                n = ++g_stats.protectedHits;
            }
            if (Detail(n)) {
                try {
                    FUT_LOGI(Game,
                             "cheat: one-hit kill - %s is protected (health %.1f of %.1f): %.1f damage unchanged "
                             "(%u.)",
                             ActorName(actor).c_str(), static_cast<double>(health), static_cast<double>(maxHealth),
                             static_cast<double>(amount), n);
                } catch (...) {
                }
            }
        }
        if (core::LogEnabled(core::Channel::Game, core::Level::Debug)) {
            try {
                FUT_LOGD(Game,
                         "cheat: damage 0x%08X %s vt 0x%08X health %.1f/%.1f amount %.1f -> %.1f attacker %s vt "
                         "0x%08X from 0x%08X",
                         Address, ActorName(actor).c_str(), ReadU32(actor), static_cast<double>(health),
                         static_cast<double>(maxHealth), static_cast<double>(amount), static_cast<double>(use),
                         ActorName(attacker).c_str(), ReadU32(attacker), guest::Addr(_ReturnAddress()) - 5);
            } catch (...) {
            }
        }
    }
    return core::CallGuestThiscall(core::HookTrampoline(Address), actor, {FloatBits(use), attacker});
}

// AI brain update FUN_0002ECB0, thiscall(brain) -> al, plain ret (called once per brain and tick, 0x2FF43). The
// player's brain writes the hold-fire flag 0x2DBAA0 (= player input disabled) at its start; afterwards it is set.
std::uint32_t __fastcall BrainUpdateProbe(std::uint32_t brain, std::uint32_t /*edx*/) noexcept {
    const std::uint32_t r = core::CallGuestThiscall(core::HookTrampoline(kAiBrainUpdate), brain, {});
    if (Active(Cheat::EnemiesIgnore)) {
        if (guest::Read<std::uint8_t>(kAiHoldFireVar) == 0) {
            std::lock_guard<std::mutex> lock(g_statsLock);
            ++g_stats.holdFireTicks;
        }
        guest::Write<std::uint8_t>(kAiHoldFireVar, 1);
    }
    return r;
}

}  // namespace

// ---- options ----------------------------------------------------------------------------------------------------

const char* CheatKey(Cheat c) noexcept {
    switch (c) {
        case Cheat::Invulnerable:
            return "invulnerable";
        case Cheat::OneHitKill:
            return "one_hit_kill";
        case Cheat::EnemiesIgnore:
            return "enemies_ignore";
        case Cheat::TeleportFreecam:
            return "teleport_freecam";
        case Cheat::MoonJump:
            return "moon_jump";
        case Cheat::InfiniteAmmo:
            return "infinite_ammo";
        default:
            return "?";
    }
}

const char* CheatLabelDe(Cheat c) noexcept {
    switch (c) {
        case Cheat::Invulnerable:
            return "Unverwundbar";
        case Cheat::OneHitKill:
            return "Gegner sterben mit einem Treffer";
        case Cheat::EnemiesIgnore:
            return "Feinde ignorieren mich";
        case Cheat::TeleportFreecam:
            return "Zur Freecam teleportieren";
        case Cheat::MoonJump:
            return "Mondsprung";
        case Cheat::InfiniteAmmo:
            return "Unendlich Munition";
        default:
            return "?";
    }
}

bool Settings::Enabled(Cheat c) const noexcept {
    switch (c) {
        case Cheat::Invulnerable:
            return invulnerable;
        case Cheat::OneHitKill:
            return oneHitKill;
        case Cheat::EnemiesIgnore:
            return enemiesIgnore;
        case Cheat::TeleportFreecam:
            return teleportFreecam;
        case Cheat::MoonJump:
            return moonJump;
        case Cheat::InfiniteAmmo:
            return infiniteAmmo;
        default:
            return false;
    }
}

Settings SettingsFromConfig(const core::Config& config) noexcept {
    Settings s;
    auto get = [&config](const char* key) {
        try {
            return config.GetBool("cheats", key, core::ConfigKeyDefault("cheats", key) == "1");
        } catch (...) {
            return false;
        }
    };
    s.invulnerable = get("invulnerable");
    s.oneHitKill = get("one_hit_kill");
    s.enemiesIgnore = get("enemies_ignore");
    s.teleportFreecam = get("teleport_freecam");
    s.moonJump = get("moon_jump");
    s.infiniteAmmo = get("infinite_ammo");
    s.jumpDistanceMetres = JumpDistanceFromConfig(config);
    s.ingameToggles = get("ingame_toggles");
    return s;
}

bool InvulnerableHooksWanted(const core::Config& config) noexcept {
    const Settings s = SettingsFromConfig(config);
    return s.invulnerable || s.ingameToggles;
}
bool OneHitKillHooksWanted(const core::Config& config) noexcept {
    const Settings s = SettingsFromConfig(config);
    return s.oneHitKill || s.ingameToggles;
}
bool EnemiesIgnoreHooksWanted(const core::Config& config) noexcept {
    const Settings s = SettingsFromConfig(config);
    return s.enemiesIgnore || s.ingameToggles;
}

// ---- pure decisions ---------------------------------------------------------------------------------------------

bool TentacleHooksWanted(const core::Config& config) noexcept {
    const Settings s = SettingsFromConfig(config);
    return s.invulnerable || s.enemiesIgnore || s.ingameToggles;
}

bool BlockPlayerDamage(bool invulnerable, float amount, guest::GuestAddr returnAddress) noexcept {
    return invulnerable && amount > 0.0f && returnAddress != kKillFloorReturn && returnAddress != kTentacleKillReturn;
}

bool IsLethalHit(float amount, float health) noexcept {
    return health > 0.0f && amount >= health;
}

bool SkipTentacleGrab(bool invulnerable, bool enemiesIgnore, std::string_view event) noexcept {
    constexpr std::string_view kGrab = "morph:grab";
    if (!(invulnerable || enemiesIgnore) || event.size() != kGrab.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kGrab.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(event[i])) != kGrab[i]) {
            return false;
        }
    }
    return true;
}

float TentacleHoldCooldown(float cooldown) noexcept {
    return cooldown >= kTentacleHoldCooldown ? cooldown : kTentacleHoldCooldown;  // NaN -> the hold value
}

bool LethalBlockWatch::OnBlockedLethal(std::uint32_t attacker, double now) noexcept {
    if (!active_ || attacker != attacker_ || now < last_ || now - last_ > kLethalBlockGapSeconds) {
        active_ = true;
        attacker_ = attacker;
        first_ = now;
    }
    last_ = now;
    if (now - first_ > kLethalBlockLimitSeconds) {
        active_ = false;
        return true;
    }
    return false;
}

HitDecision OneHitDamage(float amount, float health, float maxHealth, bool attackerIsPlayer,
                         bool targetIsPlayer) noexcept {
    HitDecision d{HitKind::Unchanged, amount};
    if (!attackerIsPlayer || targetIsPlayer || !(amount > 0.0f) || !(health > 0.0f)) {
        return d;
    }
    // The game's invulnerability marker (FUN_000354B0 ignores every amount but 55555) and near-invulnerable defs.
    if (health == kProtectedHealth || maxHealth == kProtectedHealth || maxHealth >= kProtectedMaxHealth ||
        health >= kProtectedMaxHealth) {
        return d;
    }
    if (maxHealth >= kBossMaxHealth) {
        const float bossHit = maxHealth * kBossHitFraction;
        if (amount < bossHit) {
            d.kind = HitKind::Boss;
            d.amount = bossHit;
        }
        return d;
    }
    if (amount < health) {
        d.kind = HitKind::Lethal;
        d.amount = health;
    }
    return d;
}

// ---- run time ---------------------------------------------------------------------------------------------------

void Configure() noexcept {
    std::call_once(g_configureOnce, [] {
        try {
            g_settings = SettingsFromConfig(core::GetConfig());
        } catch (...) {
            g_settings = Settings{};
        }
        for (int i = 0; i < kCheatCount; ++i) {
            g_active[static_cast<std::size_t>(i)].store(g_settings.Enabled(static_cast<Cheat>(i)),
                                                        std::memory_order_release);
        }
        if (!g_settings.AnyHook()) {
            FUT_LOGI(Game, "cheats: all off (no hooks)");
            return;
        }
        FUT_LOGI(Game,
                 "cheats: invulnerable %s, one-hit kill %s, enemies ignore me %s, teleport to the freecam %s, moon "
                 "jump %s, jump distance +%.1f m, infinite ammo %s; in-game toggles %s%s",
                 OnOff(g_settings.invulnerable), OnOff(g_settings.oneHitKill), OnOff(g_settings.enemiesIgnore),
                 OnOff(g_settings.teleportFreecam), OnOff(g_settings.moonJump),
                 static_cast<double>(g_settings.jumpDistanceMetres), OnOff(g_settings.infiniteAmmo),
                 OnOff(g_settings.ingameToggles),
                 g_settings.ingameToggles ? " (D-pad: invulnerable UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT, one-hit "
                                            "kill LEFT RIGHT LEFT RIGHT UP DOWN UP DOWN, enemies ignore DOWN UP DOWN "
                                            "UP RIGHT LEFT RIGHT LEFT, teleport RIGHT RIGHT LEFT LEFT UP UP DOWN DOWN, "
                                            "moon jump UP RIGHT DOWN LEFT UP RIGHT DOWN LEFT, infinite ammo DOWN DOWN "
                                            "UP UP LEFT LEFT RIGHT RIGHT)"
                                          : "");
        core::AddExitReporter("cheats", &LogSummary);
    });
}

const Settings& ActiveSettings() noexcept {
    Configure();
    return g_settings;
}

bool Active(Cheat c) noexcept {
    const int i = static_cast<int>(c);
    return i >= 0 && i < kCheatCount && g_active[static_cast<std::size_t>(i)].load(std::memory_order_acquire);
}

bool Toggle(Cheat c, int port, std::string_view sequenceText) noexcept {
    Configure();
    const int i = static_cast<int>(c);
    if (i < 0 || i >= kCheatCount) {
        return false;
    }
    std::atomic<bool>& flag = g_active[static_cast<std::size_t>(i)];
    if (!g_settings.ingameToggles) {
        return flag.load();
    }
    const bool on = !flag.load();
    flag.store(on, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_statsLock);
        ++g_stats.toggles;
    }
    g_pendingFlash.store(on ? 1 : 2);
    try {
        const std::string seq(sequenceText);
        FUT_LOGI(Game, "cheat: %s %s (%s \"%s\", %s, port %d)", CheatKey(c), on ? "ON" : "OFF", CheatLabelDe(c),
                 on ? "an" : "aus", seq.c_str(), port + 1);
    } catch (...) {
    }
    return on;
}

void OnTick() noexcept {
    if (!g_settings.AnyHook()) {
        return;
    }
    PlayerCheatsTick();  // teleport, vehicle/charge counters (cheats_player.cpp)
    try {
        const guest::GuestAddr session = ReadU32(kSessionPtr);
        const int flash = g_pendingFlash.exchange(0);
        if (flash != 0) {
            const guest::GuestAddr camera = session != 0 ? ReadU32(session + kSessionCamera) : 0;
            if (camera != 0) {
                // The game's own cheat confirmation (FUN_0002C530 end): colour, alpha 0.5, 0.1 s in, 0.2 s hold,
                // 0.2 s out.
                const float r = flash == 2 ? 1.0f : 0.0f;
                const float g = flash == 1 ? 1.0f : 0.0f;
                core::CallGuestThiscall(kCameraFlash, camera,
                                        {FloatBits(r), FloatBits(g), FloatBits(0.0f), FloatBits(0.5f),
                                         FloatBits(0.1f), FloatBits(0.2f), FloatBits(0.2f)});
            }
        }
        // Health trace of the controlled character (evidence for the cheats; lines only on changes, g_traceHealth = the
        // last logged value).
        const guest::GuestAddr player = session != 0 ? ReadU32(session + kSessionPlayer) : 0;
        const std::int32_t lives = ReadS32(kLivesVar);
        if (player == 0) {
            g_tracePlayer = 0;
            return;
        }
        const float health = ReadF32(player + kActorHealth);
        if (player != g_tracePlayer) {
            g_tracePlayer = player;
            g_traceHealth = health;
            g_traceLives = lives;
            if (g_traceLines++ < kHealthTraceLines) {
                FUT_LOGI(Game, "cheat: player %s health %.1f, lives %d", ActorName(player).c_str(),
                         static_cast<double>(health), lives);
            }
            return;
        }
        // Every loss, gains from 5 points on (the Chickenwalker regenerates a little every tick), lives.
        if (health < g_traceHealth - 0.05f || health > g_traceHealth + 5.0f || lives != g_traceLives) {
            if (g_traceLines++ < kHealthTraceLines) {
                FUT_LOGI(Game, "cheat: player health %.1f -> %.1f, lives %d -> %d", static_cast<double>(g_traceHealth),
                         static_cast<double>(health), g_traceLives, lives);
            }
            g_traceHealth = health;
            g_traceLives = lives;
        }
    } catch (...) {
    }
}

Stats GetStats() noexcept {
    std::lock_guard<std::mutex> lock(g_statsLock);
    return g_stats;
}

std::uint32_t CountPlayerCheat(std::uint32_t Stats::*field) noexcept {
    std::lock_guard<std::mutex> lock(g_statsLock);
    return ++(g_stats.*field);
}

void QueueFlash(bool on) noexcept {
    g_pendingFlash.store(on ? 1 : 2);
}

void LogSummary() noexcept {
    const Stats s = GetStats();
    FUT_LOGI(Game,
             "cheats summary: invulnerable %s (%u hits blocked, %.1f damage, %u instant-death floors, %u grab kills, "
             "%u safety-net deaths, %u lives kept), one-hit kill %s (%u lethal hits, %u boss hits, %u protected), "
             "enemies ignore me %s (hold fire forced %u times, %u tentacle holds), %u tentacle grabs skipped, %u "
             "in-game toggles",
             OnOff(Active(Cheat::Invulnerable)), s.blockedHits, s.blockedDamage, s.killFloorDeaths, s.grabKills,
             s.safetyNetKills, s.livesKept, OnOff(Active(Cheat::OneHitKill)), s.lethalHits, s.bossHits,
             s.protectedHits, OnOff(Active(Cheat::EnemiesIgnore)), s.holdFireTicks, s.tentacleHolds, s.grabsPrevented,
             s.toggles);
    LogPlayerCheatsSummary();
}

}  // namespace xapi::cheats

// Installed only when the cheat is on at start or [cheats] ingame_toggles = 1 (core::HookDesc::condition). Prologues
// checked with capstone (g8_cheats.md 5): whole position-independent instructions, no branch into them.
FUT_PROBE_IF(xapi::cheats::kPlayerTakeDamage, &xapi::cheats::PlayerTakeDamageProbe, Thiscall,
             "probe:Player::TakeDamage", "55 8B EC 83 E4 F0", 6, &xapi::cheats::InvulnerableHooksWanted);
FUT_PROBE_IF(xapi::cheats::kDeductLives, &xapi::cheats::DeductLivesProbe, Cdecl, "probe:Script::FsDeductLives",
             "FF 0D DC 67 2D 00", 6, &xapi::cheats::InvulnerableHooksWanted);
FUT_PROBE_IF(xapi::cheats::kActorTakeDamage, &xapi::cheats::EnemyTakeDamageProbe<xapi::cheats::kActorTakeDamage>,
             Thiscall, "probe:Actor::TakeDamage", "64 A1 00 00 00 00", 6, &xapi::cheats::OneHitKillHooksWanted);
FUT_PROBE_IF(xapi::cheats::kNpcTakeDamage, &xapi::cheats::EnemyTakeDamageProbe<xapi::cheats::kNpcTakeDamage>,
             Thiscall, "probe:Npc::TakeDamage", "D9 44 24 04 83 EC 0C", 7, &xapi::cheats::OneHitKillHooksWanted);
FUT_PROBE_IF(xapi::cheats::kKamikazeTakeDamage,
             &xapi::cheats::EnemyTakeDamageProbe<xapi::cheats::kKamikazeTakeDamage>, Thiscall,
             "probe:Kamikaze::TakeDamage", "D9 44 24 04 83 EC 0C", 7, &xapi::cheats::OneHitKillHooksWanted);
FUT_PROBE_IF(xapi::cheats::kAquafishTakeDamage,
             &xapi::cheats::EnemyTakeDamageProbe<xapi::cheats::kAquafishTakeDamage>, Thiscall,
             "probe:Aquafish::TakeDamage", "83 EC 0C 56 8B F1", 6, &xapi::cheats::OneHitKillHooksWanted);
FUT_PROBE_IF(xapi::cheats::kGoonTakeDamage, &xapi::cheats::EnemyTakeDamageProbe<xapi::cheats::kGoonTakeDamage>,
             Thiscall, "probe:Goon::TakeDamage", "83 EC 18 56 8B F1", 6, &xapi::cheats::OneHitKillHooksWanted);
FUT_PROBE_IF(xapi::cheats::kJunkbotTakeDamage,
             &xapi::cheats::EnemyTakeDamageProbe<xapi::cheats::kJunkbotTakeDamage>, Thiscall,
             "probe:Junkbot::TakeDamage", "D9 44 24 04 83 EC 0C", 7, &xapi::cheats::OneHitKillHooksWanted);
FUT_PROBE_IF(xapi::cheats::kStormtrooperTakeDamage,
             &xapi::cheats::EnemyTakeDamageProbe<xapi::cheats::kStormtrooperTakeDamage>, Thiscall,
             "probe:Stormtrooper::TakeDamage", "83 EC 0C 56 8B F1", 6, &xapi::cheats::OneHitKillHooksWanted);
FUT_PROBE_IF(xapi::cheats::kAiBrainUpdate, &xapi::cheats::BrainUpdateProbe, Thiscall, "probe:AI::BrainUpdate",
             "55 8B EC 83 E4 F0", 6, &xapi::cheats::EnemiesIgnoreHooksWanted);
FUT_PROBE_IF(xapi::cheats::kTentacleUpdate, &xapi::cheats::TentacleUpdateProbe, Thiscall, "probe:Tentacle::Update",
             "55 8B EC 83 E4 F0", 6, &xapi::cheats::EnemiesIgnoreHooksWanted);
FUT_PROBE_IF(xapi::cheats::kTentacleAnimEvent, &xapi::cheats::TentacleAnimEventProbe, Thiscall,
             "probe:Tentacle::AnimEvent", "55 8B EC 83 E4 F0", 6, &xapi::cheats::TentacleHooksWanted);
