// G4: [game] start_level and [game] play_as (see game_select.h).
#include "xapi/game_select.h"

#include "core/config_schema.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/log.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace xapi::select {
namespace {

char Lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }
    return true;
}

// Game order = order of the cheat codes and of the table 0x2D60BC (g4_select.md 2.1/2.4). The table rows follow
// the game order, not the folder names: Level1-7 before Level1-6, Level2-6 before Level2-2, Level3-6 before 3-4.
constexpr StartLevel kStartLevels[] = {
    {"Level1-1", 0, Character::Fry},     {"Level1-2", 1, Character::Fry},     {"Level1-3", 2, Character::Fry},
    {"Level1-4", 3, Character::Fry},     {"Level1-5", 4, Character::Fry},     {"Level1-7", 5, Character::Fry},
    {"Level1-6", 6, Character::Fry},     {"Level2-1", 7, Character::Bender},  {"Level2-6", 8, Character::Bender},
    {"Level2-2", 9, Character::Bender},  {"Level2-3", 10, Character::Bender}, {"Level2-4", 11, Character::Bender},
    {"Level2-5", 12, Character::Bender}, {"Level3-1", 13, Character::Leela},  {"Level3-2", 14, Character::Leela},
    {"Level3-3", 15, Character::Leela},  {"Level3-6", 16, Character::Leela},  {"Level3-4", 17, Character::Leela},
    {"Level4-1", 18, Character::Rodent}, {"Level5-1", 19, Character::Bender}, {"Level5-2", 20, Character::Leela},
    {"Level5-3", 21, Character::Fry},
};
static_assert(std::size(kStartLevels) == kStartStateRows);

struct BlockedLevel {
    const char* level;
    const char* reason;
};
constexpr BlockedLevel kBlocked[] = {
    {"Level1-6", "Fry's Chicken Walker (FUN_00043D80, only for Fry)"},
    {"Level2-5", "Bender's cannon for the tank fight (FUN_000334D0, only for Bender)"},
    {"Level4-1", "Zoidberg's rat ride (Rodent, FUN_000611B0)"},
    {"Level5-3", "Fry's railgun (WeaponDef::Railgun, fry-5-3 animations)"},
};

// ---- configuration (read once) -------------------------------------------------------------------------------
std::atomic<int> g_playAs{-1};  // -1 = not read yet
const StartLevel* g_startLevel = nullptr;
std::once_flag g_startOnce;

void ReadConfigOnce() {
    std::call_once(g_startOnce, [] {
        g_startLevel = StartLevelFromConfig(core::GetConfig());
        g_playAs.store(static_cast<int>(PlayAsFromConfig(core::GetConfig())));
    });
}

std::string GuestString(guest::GuestAddr addr, std::size_t max = 64) {
    std::string s;
    if (addr == 0) {
        return s;
    }
    const char* p = guest::Ptr<const char>(addr);
    for (std::size_t i = 0; i < max && p[i] != 0; ++i) {
        s.push_back(p[i]);
    }
    return s;
}

std::string CurrentLevelName() {
    const auto level = guest::Read<std::uint32_t>(kLevelPtr);
    if (level == 0) {
        return {};
    }
    return GuestString(guest::Read<std::uint32_t>(level + kLevelNameOffset), 16);
}

// Name mapping of the play_as probes; returns `name` itself when nothing changes.
const char* Mapped(const char* name) {
    ReadConfigOnce();
    const auto playAs = static_cast<PlayAs>(g_playAs.load(std::memory_order_relaxed));
    if (name == nullptr || playAs == PlayAs::Original) {
        return name;
    }
    const std::string_view view(name, strnlen(name, 64));
    if (PlayerDefCharacter(view) == Character::None) {
        return name;  // fast path for every other def
    }
    const char* to = MapPlayerDefName(view, CurrentLevelName(), playAs);
    return to != nullptr ? to : name;
}

// Removes Pickup=/Equip= from a guest state text in place (the text only gets shorter; size var updated).
void StripGuestState(guest::GuestAddr textVar) {
    const auto text = guest::Read<std::uint32_t>(textVar);
    if (text == 0) {
        return;
    }
    const std::uint32_t size = guest::Read<std::uint32_t>(textVar + 4);
    const std::string original = GuestString(text, size != 0 ? size : 4096);
    const std::string stripped = StripInventoryLines(original);
    if (stripped.size() == original.size()) {
        return;
    }
    std::memcpy(guest::Ptr<char>(text), stripped.c_str(), stripped.size() + 1);
    guest::Write<std::uint32_t>(textVar + 4, static_cast<std::uint32_t>(stripped.size() + 1));
    FUT_LOGI(Game, "game: play_as: weapons removed from the start state 0x%08X (%zu -> %zu bytes)", textVar,
             original.size(), stripped.size());
}

// ---- probes ----------------------------------------------------------------------------------------------------
// FUN_000A2340 CApp::SetLevelName (thiscall ecx = CApp, [esp+4] = name, ret 4).
std::uint32_t __fastcall SetLevelNameProbe(std::uint32_t self, std::uint32_t /*edx*/, std::uint32_t name) noexcept {
    ReadConfigOnce();
    const guest::GuestAddr from = guest::Addr(_ReturnAddress());
    std::uint32_t useName = name;
    if (from == kNewGameSetLevelReturn && g_startLevel != nullptr) {
        const auto state = guest::Read<std::uint32_t>(kStartStateTable + 8 * g_startLevel->row);
        const std::string text = GuestString(state, 512);
        const std::string expect = std::string("Level=") + g_startLevel->level + "\n";
        if (text.size() < expect.size() || !IEquals(std::string_view(text).substr(0, expect.size()), expect)) {
            FUT_LOGW(Game, "game: start_level %s: table row %u reads '%.20s' - starting Level1-1 as usual",
                     g_startLevel->level, g_startLevel->row, text.c_str());
        } else {
            core::CallGuest(kSetPendingState, {state, static_cast<std::uint32_t>(text.size() + 1)});
            static char levelName[16] = {};
            std::memcpy(levelName, g_startLevel->level, std::strlen(g_startLevel->level) + 1);
            useName = guest::Addr(levelName);
            std::string flat = text;
            for (char& c : flat) {
                c = c == '\n' ? ';' : c;
            }
            FUT_LOGI(Game, "game: start_level: \"Neues Spiel\" starts %s (%s) instead of Level1-1, start state "
                           "of table row %u: %s",
                     g_startLevel->level, CharacterName(g_startLevel->character), g_startLevel->row, flat.c_str());
        }
    }
    return core::CallGuestThiscall(core::HookTrampoline(kSetLevelName), self, {useName});
}

// FUN_0005E580 PlayerDef ctor (thiscall ecx = def, [esp+4] = name, ret 4, returns the def).
std::uint32_t __fastcall PlayerDefCtorProbe(std::uint32_t self, std::uint32_t /*edx*/, std::uint32_t name) noexcept {
    ReadConfigOnce();
    const char* original = guest::Ptr<const char>(name);
    const char* to = Mapped(original);
    const std::string level = CurrentLevelName();
    if (to != original) {
        FUT_LOGI(Game, "game: play_as %s: %s player def \"%s\" -> \"%s\" (Chars\\%s.ucf)",
                 PlayAsName(static_cast<PlayAs>(g_playAs.load())), level.c_str(), original, to, to);
        StripGuestState(kPendingStateVar);
        StripGuestState(kCheckpointStateVar);
    } else if (original != nullptr) {
        const char* reason = PlayAsBlockedReason(level);
        FUT_LOGI(Game, "game: play_as %s: %s keeps its player def \"%s\"%s%s",
                 PlayAsName(static_cast<PlayAs>(g_playAs.load())), level.c_str(), original,
                 reason != nullptr ? " - blocked level, needs " : "", reason != nullptr ? reason : "");
    }
    return core::CallGuestThiscall(core::HookTrampoline(kPlayerDefCtor), self, {guest::Addr(to)});
}

// FUN_000C3E80 Level::FindDef / FUN_000C3F20 Level::FindCharDef (thiscall ecx = level, [esp+4] = name, ret 4).
std::uint32_t __fastcall FindDefProbe(std::uint32_t self, std::uint32_t /*edx*/, std::uint32_t name) noexcept {
    const char* to = Mapped(guest::Ptr<const char>(name));
    return core::CallGuestThiscall(core::HookTrampoline(kLevelFindDef), self, {guest::Addr(to)});
}
std::uint32_t __fastcall FindCharDefProbe(std::uint32_t self, std::uint32_t /*edx*/, std::uint32_t name) noexcept {
    const char* to = Mapped(guest::Ptr<const char>(name));
    return core::CallGuestThiscall(core::HookTrampoline(kLevelFindCharDef), self, {guest::Addr(to)});
}

bool StartLevelActive(const core::Config& config) {
    return StartLevelFromConfig(config) != nullptr;
}
bool PlayAsActive(const core::Config& config) {
    return PlayAsFromConfig(config) != PlayAs::Original;
}

}  // namespace

std::span<const StartLevel> StartLevels() noexcept {
    return kStartLevels;
}

bool ParseStartLevel(std::string_view text, const StartLevel*& out) noexcept {
    out = nullptr;
    if (text.empty() || IEquals(text, "original")) {
        return true;
    }
    for (const StartLevel& l : kStartLevels) {
        if (IEquals(text, l.level)) {
            out = &l;
            return true;
        }
    }
    return false;
}

bool ParsePlayAs(std::string_view text, PlayAs& out) noexcept {
    constexpr PlayAs kAll[] = {PlayAs::Original, PlayAs::Fry, PlayAs::Bender, PlayAs::Leela};
    for (PlayAs p : kAll) {
        if (IEquals(text, PlayAsName(p))) {
            out = p;
            return true;
        }
    }
    return false;
}

const char* PlayAsName(PlayAs value) noexcept {
    switch (value) {
    case PlayAs::Fry:
        return "fry";
    case PlayAs::Bender:
        return "bender";
    case PlayAs::Leela:
        return "leela";
    case PlayAs::Original:
        break;
    }
    return "original";
}

const char* CharacterName(Character value) noexcept {
    switch (value) {
    case Character::Fry:
        return "Fry";
    case Character::Bender:
        return "Bender";
    case Character::Leela:
        return "Leela";
    case Character::Rodent:
        return "Zoidberg";
    case Character::None:
        break;
    }
    return "-";
}

Character PlayerDefCharacter(std::string_view defName) noexcept {
    // FUN_0005E620 / FUN_0005D660: "fry" (0x29A664), "railgunfry", "fry-1-1".."fry-1-6", "fry-5-3".
    constexpr const char* kFry[] = {"fry",     "railgunfry", "fry-1-1", "fry-1-2", "fry-1-3",
                                    "fry-1-4", "fry-1-5",    "fry-1-6", "fry-5-3"};
    for (const char* f : kFry) {
        if (IEquals(defName, f)) {
            return Character::Fry;
        }
    }
    if (IEquals(defName, "bender")) {
        return Character::Bender;
    }
    if (IEquals(defName, "leela")) {
        return Character::Leela;
    }
    if (IEquals(defName, "rodent")) {
        return Character::Rodent;
    }
    return Character::None;
}

const char* PlayAsBlockedReason(std::string_view levelName) noexcept {
    for (const BlockedLevel& b : kBlocked) {
        if (IEquals(levelName, b.level)) {
            return b.reason;
        }
    }
    return nullptr;
}

const char* MapPlayerDefName(std::string_view defName, std::string_view levelName, PlayAs playAs) noexcept {
    if (playAs == PlayAs::Original || IsPlayAsBlockedLevel(levelName)) {
        return nullptr;
    }
    const Character from = PlayerDefCharacter(defName);
    if (from != Character::Fry && from != Character::Bender && from != Character::Leela) {
        return nullptr;
    }
    const Character to = playAs == PlayAs::Fry      ? Character::Fry
                         : playAs == PlayAs::Bender ? Character::Bender
                                                    : Character::Leela;
    if (from == to) {
        return nullptr;  // the level's own variant (e.g. fry-1-3 with its weapon animations) stays
    }
    return PlayAsName(playAs);  // def names "fry" (full model fry.ucf), "bender", "leela"
}

std::string StripInventoryLines(std::string_view state) {
    std::string out;
    out.reserve(state.size());
    std::size_t pos = 0;
    while (pos < state.size()) {
        std::size_t end = state.find('\n', pos);
        end = end == std::string_view::npos ? state.size() : end + 1;
        const std::string_view line = state.substr(pos, end - pos);
        if (line.rfind("Pickup=", 0) != 0 && line.rfind("Equip=", 0) != 0) {
            out.append(line);
        }
        pos = end;
    }
    return out;
}

const StartLevel* FindStartLevel(std::string_view level) noexcept {
    for (const StartLevel& l : kStartLevels) {
        if (IEquals(level, l.level)) {
            return &l;
        }
    }
    return nullptr;
}

bool PlayAsReplacesCharacter(Character character, std::string_view levelName, PlayAs playAs) noexcept {
    const char* def = character == Character::Fry      ? "fry"
                      : character == Character::Bender ? "bender"
                      : character == Character::Leela  ? "leela"
                                                       : nullptr;
    return def != nullptr && MapPlayerDefName(def, levelName, playAs) != nullptr;
}

namespace {

constexpr int kAmmoSlots = 5;  // FUN_000C5340 / FUN_000C26E0: 5 ammo slots

std::vector<std::string> SplitLines(std::string_view s) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t end = s.find('\n', pos);
        if (end == std::string_view::npos) {
            end = s.size();
        }
        if (end > pos) {
            lines.emplace_back(s.substr(pos, end - pos));
        }
        pos = end + 1;
    }
    return lines;
}

bool StartsWith(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// "Pickup=<name>" / "Equip=<name>" -> name, else empty. `equip` = Equip line.
std::string_view WeaponOf(std::string_view line, bool& equip) noexcept {
    equip = StartsWith(line, "Equip=");
    if (equip) {
        return line.substr(6);
    }
    return StartsWith(line, "Pickup=") ? line.substr(7) : std::string_view();
}

// Ammo lines as FUN_000C5340 reads them: "Ammo=<v>" sets all slots, "Ammo=<slot>|<v>" one slot (atol / atof+ftol).
// Returns false if the text has no Ammo line.
bool ParseAmmo(const std::vector<std::string>& lines, std::array<long, kAmmoSlots>& out) {
    out.fill(0);
    bool any = false;
    for (const std::string& line : lines) {
        if (!StartsWith(line, "Ammo=")) {
            continue;
        }
        any = true;
        const std::string value = line.substr(5);
        const std::size_t bar = value.find('|');
        if (bar == std::string::npos) {
            out.fill(static_cast<long>(std::atof(value.c_str())));
        } else {
            const long slot = std::atol(value.substr(0, bar).c_str());
            if (slot >= 0 && slot < kAmmoSlots) {
                out[static_cast<std::size_t>(slot)] = static_cast<long>(std::atof(value.c_str() + bar + 1));
            }
        }
    }
    return any;
}

}  // namespace

WeaponMerge MergeStartWeapons(std::string_view state, std::string_view table) {
    WeaponMerge result;
    std::vector<std::string> lines = SplitLines(state);
    const std::vector<std::string> tableLines = SplitLines(table);

    bool hasEquip = false;
    for (const std::string& line : lines) {
        bool equip = false;
        if (!WeaponOf(line, equip).empty() && equip) {
            hasEquip = true;
        }
    }
    auto findWeapon = [&lines](std::string_view name) -> std::string* {
        for (std::string& line : lines) {
            bool equip = false;
            const std::string_view w = WeaponOf(line, equip);
            if (!w.empty() && IEquals(w, name)) {
                return &line;
            }
        }
        return nullptr;
    };

    std::vector<std::string> added;
    for (const std::string& tline : tableLines) {
        bool tableEquip = false;
        const std::string_view name = WeaponOf(tline, tableEquip);
        if (name.empty()) {
            continue;
        }
        bool inAdded = false;
        for (const std::string& a : added) {
            bool e = false;
            inAdded = inAdded || IEquals(WeaponOf(a, e), name);
        }
        if (inAdded) {
            continue;
        }
        if (std::string* own = findWeapon(name)) {
            if (tableEquip && !hasEquip) {
                *own = "Equip=" + std::string(name);  // the player's own weapon becomes the equipped one
                hasEquip = true;
                result.equipped = std::string(name);
            }
            continue;
        }
        const bool equipIt = tableEquip && !hasEquip;
        added.push_back((equipIt ? "Equip=" : "Pickup=") + std::string(name));
        if (equipIt) {
            hasEquip = true;
            result.equipped = std::string(name);
        }
        result.added += (result.added.empty() ? "" : ",") + std::string(name);
    }

    if (!added.empty()) {
        // After the last weapon line, else before the trailing counters, else at the end (FUN_000C26E0 order).
        std::size_t at = lines.size();
        bool afterWeapon = false;
        for (std::size_t i = lines.size(); i-- > 0;) {
            bool e = false;
            if (!WeaponOf(lines[i], e).empty()) {
                at = i + 1;
                afterWeapon = true;
                break;
            }
        }
        if (!afterWeapon) {
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (StartsWith(lines[i], "Lives=") || StartsWith(lines[i], "AgeSeconds=") ||
                    StartsWith(lines[i], "AgeHours=")) {
                    at = i;
                    break;
                }
            }
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), added.begin(), added.end());

        std::array<long, kAmmoSlots> have{};
        std::array<long, kAmmoSlots> want{};
        const bool stateAmmo = ParseAmmo(lines, have);
        if (ParseAmmo(tableLines, want)) {
            std::array<long, kAmmoSlots> merged = have;
            for (std::size_t s = 0; s < merged.size(); ++s) {
                if (want[s] > merged[s]) {
                    merged[s] = want[s];
                    result.ammoRaised = true;
                }
            }
            if (result.ammoRaised) {
                std::size_t ammoAt = lines.size();
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    if (StartsWith(lines[i], "Ammo=")) {
                        ammoAt = std::min(ammoAt, i);
                    }
                }
                if (!stateAmmo) {
                    // No Ammo line: before the first weapon line (FUN_000C26E0 writes Ammo before the weapons).
                    for (std::size_t i = 0; i < lines.size(); ++i) {
                        bool e = false;
                        if (!WeaponOf(lines[i], e).empty()) {
                            ammoAt = i;
                            break;
                        }
                    }
                }
                std::erase_if(lines, [](const std::string& l) { return StartsWith(l, "Ammo="); });
                ammoAt = std::min(ammoAt, lines.size());
                std::vector<std::string> ammo;
                for (int s = 0; s < kAmmoSlots; ++s) {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "Ammo=%d|%ld.000000", s, merged[static_cast<std::size_t>(s)]);
                    ammo.emplace_back(buf);
                }
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(ammoAt), ammo.begin(), ammo.end());
            }
        }
    }

    if (!result.Changed()) {
        result.text = std::string(state);
        return result;
    }
    for (const std::string& line : lines) {
        result.text += line;
        result.text += '\n';
    }
    return result;
}

namespace {

std::uint32_t SafeReadU32(guest::GuestAddr p) noexcept {
    return (p >= 0x10000 && guest::IsReadable(p, 4)) ? guest::Read<std::uint32_t>(p) : 0;
}

std::string Flat(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == 0)) {
        text.pop_back();
    }
    for (char& c : text) {
        c = c == '\n' ? ';' : c;
    }
    return text;
}

constexpr guest::GuestAddr kBreakdownPtr = 0x002E8E4C;  // statistics screen "Breakdown"; byte +1 = open
bool g_startStateArmed = false;  // a level existed while a start state was pending (guest main thread)

}  // namespace

void EnsureStartWeapons(std::string_view nextLevel) noexcept {
    try {
        ReadConfigOnce();
        const StartLevel* target = FindStartLevel(nextLevel);
        if (target == nullptr) {
            return;
        }
        const std::string table = GuestString(SafeReadU32(kStartStateTable + 8 * target->row), 512);
        if (StripInventoryLines(table).size() == table.size()) {
            return;  // the level starts without weapons (Bender, Leela, Zoidberg levels, Level1-1)
        }
        const auto playAs = static_cast<PlayAs>(g_playAs.load(std::memory_order_relaxed));
        if (PlayAsReplacesCharacter(target->character, target->level, playAs)) {
            FUT_LOGI(Game, "cheat: level skip to %s: no start weapons added - play_as %s replaces %s", target->level,
                     PlayAsName(playAs), CharacterName(target->character));
            return;
        }
        const guest::GuestAddr text = SafeReadU32(kPendingStateVar);
        const std::uint32_t size = SafeReadU32(kPendingStateVar + 4);
        const std::string state = text != 0 ? GuestString(text, size != 0 ? size : 4096) : std::string();
        const WeaponMerge merge = MergeStartWeapons(state, table);
        if (!merge.Changed()) {
            FUT_LOGI(Game, "cheat: level skip to %s: the player already has the start weapons of the level (%s)",
                     target->level, Flat(state).c_str());
            return;
        }
        // FUN_000C1D30 frees the old text and copies the new one (host memory is guest-addressable).
        core::CallGuest(kSetPendingState,
                        {guest::Addr(merge.text.c_str()), static_cast<std::uint32_t>(merge.text.size() + 1)});
        FUT_LOGI(Game,
                 "cheat: level skip to %s: start weapons of table row %u added [%s]%s%s%s - state now: %s",
                 target->level, target->row, merge.added.c_str(), merge.equipped.empty() ? "" : ", equipped ",
                 merge.equipped.c_str(), merge.ammoRaised ? ", ammo raised to the table value" : "",
                 Flat(merge.text).c_str());
    } catch (...) {
    }
}

std::string PlayerStateText() {
    const guest::GuestAddr level = SafeReadU32(kLevelPtr);
    if (level == 0 || SafeReadU32(level + kLevelPlayerOffset) == 0 || SafeReadU32(kPendingStateVar) != 0) {
        return {};  // with a pending text FUN_000C26E0 would return that text (not NUL-terminated)
    }
    std::uint32_t out = 0;
    core::CallGuestThiscall(kBuildStateText, level, {guest::Addr(&out)});
    if (out == 0) {
        return {};
    }
    std::string text = GuestString(out, 4096);
    core::CallGuest(kGuestFree, {out});
    return text;
}

void ObserveStartState() noexcept {
    try {
        const guest::GuestAddr level = SafeReadU32(kLevelPtr);
        if (level == 0) {
            return;  // loading: keep the armed state
        }
        if (SafeReadU32(kPendingStateVar) != 0) {
            g_startStateArmed = true;
            return;
        }
        if (!g_startStateArmed) {
            return;
        }
        const guest::GuestAddr breakdown = SafeReadU32(kBreakdownPtr);
        if (breakdown != 0 && guest::IsReadable(breakdown + 1, 1) && guest::Read<std::uint8_t>(breakdown + 1) != 0) {
            g_startStateArmed = false;  // dropped on the statistics screen (FUN_000C1200), not applied
            return;
        }
        if (SafeReadU32(level + kLevelPlayerOffset) == 0) {
            return;
        }
        // FUN_000C5340 applied (and freed) the start state once the player existed.
        g_startStateArmed = false;
        FUT_LOGI(Game, "game: start state applied in %s: %s", CurrentLevelName().c_str(),
                 Flat(PlayerStateText()).c_str());
    } catch (...) {
    }
}

const StartLevel* StartLevelFromConfig(const core::Config& config) noexcept {
    try {
        const std::string text =
            config.GetString("game", "start_level", core::ConfigKeyDefault("game", "start_level"));
        const StartLevel* level = nullptr;
        if (!ParseStartLevel(text, level)) {
            FUT_LOGW(Game, "game: invalid [game] start_level '%s' (expected original or Level1-1 .. Level5-3) - "
                           "using original",
                     text.c_str());
            return nullptr;
        }
        return level;
    } catch (...) {
        return nullptr;
    }
}

PlayAs PlayAsFromConfig(const core::Config& config) noexcept {
    try {
        const std::string text = config.GetString("game", "play_as", core::ConfigKeyDefault("game", "play_as"));
        PlayAs value = PlayAs::Original;
        if (!text.empty() && !ParsePlayAs(text, value)) {
            FUT_LOGW(Game, "game: invalid [game] play_as '%s' (expected original, fry, bender or leela) - using "
                           "original",
                     text.c_str());
            return PlayAs::Original;
        }
        return value;
    } catch (...) {
        return PlayAs::Original;
    }
}

}  // namespace xapi::select

// Installed only when the option is not "original" (core::HookDesc::condition). Prologues checked with capstone
// (g4_select.md 4.1/5.2): whole instructions, no branch into them.
FUT_PROBE_IF(xapi::select::kSetLevelName, &xapi::select::SetLevelNameProbe, Thiscall, "probe:CApp::SetLevelName",
             "8B 44 24 04 85 C0", 6, &xapi::select::StartLevelActive);
FUT_PROBE_IF(xapi::select::kPlayerDefCtor, &xapi::select::PlayerDefCtorProbe, Thiscall, "probe:PlayerDef::Ctor",
             "8B 44 24 04 56", 5, &xapi::select::PlayAsActive);
FUT_PROBE_IF(xapi::select::kLevelFindDef, &xapi::select::FindDefProbe, Thiscall, "probe:Level::FindDef",
             "53 8B 5C 24 08", 5, &xapi::select::PlayAsActive);
FUT_PROBE_IF(xapi::select::kLevelFindCharDef, &xapi::select::FindCharDefProbe, Thiscall, "probe:Level::FindCharDef",
             "53 8B 5C 24 08", 5, &xapi::select::PlayAsActive);
