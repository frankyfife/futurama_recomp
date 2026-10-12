// Unit tests of G4 (xapi/game_select.h): [game] start_level / play_as parsing, schema rows, the level table against
// the start state table 0x2D60BC of the retail XBE (%FUT_TEST_XBE% or orig\retail_eu\default.xbe; skipped without
// it), the blocked levels, the def name mapping and the inventory filter.
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "core/hooks.h"
#include "xapi/game_select.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace sel = xapi::select;
namespace fs = std::filesystem;

namespace {

// Minimal XBE reader (tools/xbe.py): VA -> file offset through the section headers.
struct XbeFile {
    std::vector<std::uint8_t> bytes;
    std::uint32_t base = 0;
    struct Section {
        std::uint32_t va, vsize, raw, rsize;
    };
    std::vector<Section> sections;

    std::uint32_t U32At(std::size_t off) const {
        std::uint32_t v = 0;
        std::memcpy(&v, bytes.data() + off, 4);
        return v;
    }
    std::optional<std::size_t> Offset(std::uint32_t va) const {
        for (const Section& s : sections) {
            if (va >= s.va && va < s.va + s.rsize) {
                return s.raw + (va - s.va);
            }
        }
        return std::nullopt;
    }
    std::uint32_t U32(std::uint32_t va) const {
        const auto o = Offset(va);
        return o ? U32At(*o) : 0;
    }
    std::string CStr(std::uint32_t va) const {
        const auto o = Offset(va);
        std::string s;
        for (std::size_t i = o.value_or(bytes.size()); i < bytes.size() && bytes[i] != 0; ++i) {
            s.push_back(static_cast<char>(bytes[i]));
        }
        return s;
    }
};

std::optional<XbeFile> LoadRetailXbe() {
    wchar_t buf[1024];
    fs::path p = L"D:\\FuturamaDecomp\\orig\\retail_eu\\default.xbe";
    const DWORD n = GetEnvironmentVariableW(L"FUT_TEST_XBE", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        p = buf;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    XbeFile x;
    x.bytes.assign(std::istreambuf_iterator<char>(f), {});
    if (x.bytes.size() < 0x200 || std::memcmp(x.bytes.data(), "XBEH", 4) != 0) {
        return std::nullopt;
    }
    x.base = x.U32At(0x104);
    const std::uint32_t count = x.U32At(0x11C);
    const std::uint32_t headers = x.U32At(0x120);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t o = headers - x.base + i * 0x38;
        x.sections.push_back({x.U32At(o + 4), x.U32At(o + 8), x.U32At(o + 12), x.U32At(o + 16)});
    }
    return x;
}

}  // namespace

TEST_CASE("G4: [game] start_level / play_as parsing and schema rows") {
    const sel::StartLevel* level = nullptr;
    CHECK(sel::ParseStartLevel("original", level));
    CHECK(level == nullptr);
    CHECK(sel::ParseStartLevel("", level));
    CHECK(level == nullptr);
    REQUIRE(sel::ParseStartLevel("level2-1", level));
    REQUIRE(level != nullptr);
    CHECK(std::string(level->level) == "Level2-1");
    CHECK(level->row == 7);
    CHECK(level->character == sel::Character::Bender);
    CHECK_FALSE(sel::ParseStartLevel("Level3-5", level));  // unreachable level, not offered
    CHECK_FALSE(sel::ParseStartLevel("Level9-9", level));

    sel::PlayAs p = sel::PlayAs::Original;
    CHECK(sel::ParsePlayAs("Bender", p));
    CHECK(p == sel::PlayAs::Bender);
    CHECK(sel::ParsePlayAs("leela", p));
    CHECK(p == sel::PlayAs::Leela);
    CHECK(sel::ParsePlayAs("FRY", p));
    CHECK(p == sel::PlayAs::Fry);
    CHECK(sel::ParsePlayAs("original", p));
    CHECK(p == sel::PlayAs::Original);
    CHECK_FALSE(sel::ParsePlayAs("zoidberg", p));
    CHECK_FALSE(sel::ParsePlayAs("", p));

    CHECK(core::ConfigKeyDefault("game", "start_level") == "original");
    CHECK(core::ConfigKeyDefault("game", "play_as") == "original");
    CHECK(std::string(core::ConfigKeyChoices("game", "play_as")) == "original|fry|bender|leela");

    core::Config cfg;
    CHECK(sel::StartLevelFromConfig(cfg) == nullptr);
    CHECK(sel::PlayAsFromConfig(cfg) == sel::PlayAs::Original);
    cfg.values["game.start_level"] = "Level3-1";
    cfg.values["game.play_as"] = "leela";
    REQUIRE(sel::StartLevelFromConfig(cfg) != nullptr);
    CHECK(std::string(sel::StartLevelFromConfig(cfg)->level) == "Level3-1");
    CHECK(sel::PlayAsFromConfig(cfg) == sel::PlayAs::Leela);
    cfg.values["game.start_level"] = "nonsense";
    cfg.values["game.play_as"] = "nonsense";
    CHECK(sel::StartLevelFromConfig(cfg) == nullptr);
    CHECK(sel::PlayAsFromConfig(cfg) == sel::PlayAs::Original);

    // A plain HookDesc has no install condition (all existing hooks are unconditional).
    const core::HookDesc plain{0x1000, nullptr, core::HookConv::Cdecl, "x", nullptr, true, __FILE__};
    CHECK(plain.condition == nullptr);
}

TEST_CASE("G4: the start_level choices are the level table, in game order") {
    const char* choices = core::ConfigKeyChoices("game", "start_level");
    REQUIRE(choices != nullptr);
    std::string expect = "original";
    for (const sel::StartLevel& l : sel::StartLevels()) {
        expect += std::string("|") + l.level;
    }
    CHECK(std::string(choices) == expect);
    REQUIRE(sel::StartLevels().size() == sel::kStartStateRows);
    for (std::uint32_t i = 0; i < sel::StartLevels().size(); ++i) {
        CHECK(sel::StartLevels()[i].row == i);
    }
    // Game order (cheat codes 38538585NN): 1-7 before 1-6, 2-6 after 2-1, 3-6 before 3-4.
    CHECK(std::string(sel::StartLevels()[5].level) == "Level1-7");
    CHECK(std::string(sel::StartLevels()[6].level) == "Level1-6");
    CHECK(std::string(sel::StartLevels()[8].level) == "Level2-6");
    CHECK(std::string(sel::StartLevels()[16].level) == "Level3-6");
    CHECK(std::string(sel::StartLevels()[21].level) == "Level5-3");
}

TEST_CASE("G4: level table and probe prologues agree with the retail XBE") {
    const auto xbe = LoadRetailXbe();
    if (!xbe) {
        MESSAGE("skipped: retail XBE not found (FUT_TEST_XBE)");
        return;
    }
    for (const sel::StartLevel& l : sel::StartLevels()) {
        CAPTURE(l.level);
        const std::string state = xbe->CStr(xbe->U32(sel::kStartStateTable + 8 * l.row));
        CHECK(state.rfind(std::string("Level=") + l.level + "\n", 0) == 0);
        CHECK(state.find("Health=100.0\n") != std::string::npos);
    }
    // Fry's levels 1-2 .. 1-7 start with weapons, the others without.
    CHECK(xbe->CStr(xbe->U32(sel::kStartStateTable + 8 * 1)).find("Equip=Pistol") != std::string::npos);
    CHECK(xbe->CStr(xbe->U32(sel::kStartStateTable + 8 * 7)).find("Pickup=") == std::string::npos);
    // "Neues Spiel": push 0x2940C0 ("Level1-1"); call FUN_000A2340 returns to 0x200B8.
    const auto at = [&](std::uint32_t va, std::size_t n) {
        const auto o = xbe->Offset(va);
        REQUIRE(o.has_value());
        return std::vector<std::uint8_t>(xbe->bytes.begin() + static_cast<std::ptrdiff_t>(*o),
                                         xbe->bytes.begin() + static_cast<std::ptrdiff_t>(*o + n));
    };
    CHECK(at(0x200AE, 10) == std::vector<std::uint8_t>{0x68, 0xC0, 0x40, 0x29, 0x00, 0xE8, 0x88, 0x22, 0x08, 0x00});
    CHECK(0x200B3 + 5 + 0x82288 == sel::kSetLevelName);
    CHECK(xbe->CStr(0x2940C0) == "Level1-1");
    CHECK(at(sel::kSetLevelName, 6) == std::vector<std::uint8_t>{0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0});
    CHECK(at(sel::kPlayerDefCtor, 5) == std::vector<std::uint8_t>{0x8B, 0x44, 0x24, 0x04, 0x56});
    CHECK(at(sel::kLevelFindDef, 5) == std::vector<std::uint8_t>{0x53, 0x8B, 0x5C, 0x24, 0x08});
    CHECK(at(sel::kLevelFindCharDef, 5) == std::vector<std::uint8_t>{0x53, 0x8B, 0x5C, 0x24, 0x08});
}

TEST_CASE("G4: play_as - characters, blocked levels and the def name mapping") {
    CHECK(sel::PlayerDefCharacter("fry-1-1") == sel::Character::Fry);
    CHECK(sel::PlayerDefCharacter("Fry") == sel::Character::Fry);
    CHECK(sel::PlayerDefCharacter("RailgunFry") == sel::Character::Fry);
    CHECK(sel::PlayerDefCharacter("fry-5-3") == sel::Character::Fry);
    CHECK(sel::PlayerDefCharacter("Bender") == sel::Character::Bender);
    CHECK(sel::PlayerDefCharacter("leela") == sel::Character::Leela);
    CHECK(sel::PlayerDefCharacter("Rodent") == sel::Character::Rodent);
    CHECK(sel::PlayerDefCharacter("nibbler_1-6") == sel::Character::None);
    CHECK(sel::PlayerDefCharacter("Pistol") == sel::Character::None);
    CHECK(sel::PlayerDefCharacter("fry-1-11") == sel::Character::None);

    for (const char* blocked : {"Level1-6", "level2-5", "LEVEL4-1", "Level5-3"}) {
        CAPTURE(blocked);
        CHECK(sel::IsPlayAsBlockedLevel(blocked));
        CHECK(sel::MapPlayerDefName("fry", blocked, sel::PlayAs::Bender) == nullptr);
        CHECK(sel::MapPlayerDefName("Bender", blocked, sel::PlayAs::Fry) == nullptr);
        CHECK(sel::MapPlayerDefName("fry-5-3", blocked, sel::PlayAs::Leela) == nullptr);
    }
    for (const char* open : {"Level1-1", "Level1-7", "Level2-1", "Level3-5", "Level5-1", "Level5-2"}) {
        CHECK_FALSE(sel::IsPlayAsBlockedLevel(open));
    }

    auto map = [](const char* def, const char* level, sel::PlayAs p) {
        const char* to = sel::MapPlayerDefName(def, level, p);
        return std::string(to != nullptr ? to : "-");
    };
    CHECK(map("fry-1-1", "Level1-1", sel::PlayAs::Bender) == "bender");
    CHECK(map("fry-1-1", "Level1-1", sel::PlayAs::Leela) == "leela");
    CHECK(map("fry-1-1", "Level1-1", sel::PlayAs::Fry) == "-");  // own variant stays (weapon animations)
    CHECK(map("Fry", "Level1-7", sel::PlayAs::Fry) == "-");
    CHECK(map("Bender", "Level2-1", sel::PlayAs::Leela) == "leela");
    CHECK(map("Bender", "Level2-1", sel::PlayAs::Fry) == "fry");
    CHECK(map("Leela", "Level3-1", sel::PlayAs::Bender) == "bender");
    CHECK(map("Leela", "Level3-1", sel::PlayAs::Original) == "-");
    CHECK(map("Rodent", "Level3-1", sel::PlayAs::Bender) == "-");  // never mapped
    CHECK(map("nibbler_2", "Level2-1", sel::PlayAs::Fry) == "-");
    CHECK(map("fry", "", sel::PlayAs::Leela) == "leela");  // unknown level name: not blocked
}

TEST_CASE("G4: inventory lines are removed from the start state") {
    CHECK(sel::StripInventoryLines("Level=Level1-4\nHealth=100.0\nAmmo=100.0\nPickup=Hammer\nPickup=Pistol\n"
                                   "Equip=Shotgun\n") == "Level=Level1-4\nHealth=100.0\nAmmo=100.0\n");
    CHECK(sel::StripInventoryLines("Health=50.000000\nAmmo=0|12.000000\nEquip=Pistol\nLives=3\nAgeSeconds=1.0\n") ==
          "Health=50.000000\nAmmo=0|12.000000\nLives=3\nAgeSeconds=1.0\n");
    CHECK(sel::StripInventoryLines("Health=1\nPickup=Hammer") == "Health=1\n");
    CHECK(sel::StripInventoryLines("") == "");
    CHECK(sel::StripInventoryLines("Lives=5\n") == "Lives=5\n");
}
