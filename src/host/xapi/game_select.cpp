// G4: [game] start_level and [game] play_as (see game_select.h).
#include "xapi/game_select.h"

#include "core/config_schema.h"
#include "core/guest_call.h"
#include "core/hooks.h"
#include "core/log.h"

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

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
