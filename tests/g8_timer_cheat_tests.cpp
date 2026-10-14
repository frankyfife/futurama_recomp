// Unit tests of the G8 freeze timer cheat (xapi/cheats_timer.h, input/cheat_toggles.h, 2026-10-14): [cheats]
// freeze_timer - schema row and hook conditions, the D-pad sequence (unique, no prefix/suffix overlap with
// the other sequences), the pure decisions (HUD timer counting, countdown sites, the held wait, the m:ss
// display), the probe sites and HUD timer data against the retail XBE (%FUT_TEST_XBE% or
// orig\retail_eu\default.xbe) and the countdown sites against the level scripts (%FUT_TEST_SCRIPTS% or the
// extracted FutPAL01 scripts; both skipped without).
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "input/cheat_toggles.h"
#include "xapi/cheats.h"
#include "xapi/cheats_timer.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace ch = xapi::cheats;
namespace ct = inp::cheattoggles;
namespace fs = std::filesystem;

namespace {

struct Xbe {
    std::vector<std::uint8_t> bytes;
    struct Section {
        std::uint32_t va, vsize, raw, rsize, nameAddr;
    };
    std::vector<Section> sections;
    std::uint32_t base = 0;
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
    std::vector<std::uint8_t> At(std::uint32_t va, std::size_t n) const {
        const auto o = Offset(va);
        if (!o || *o + n > bytes.size()) {
            return {};
        }
        return {bytes.begin() + static_cast<std::ptrdiff_t>(*o),
                bytes.begin() + static_cast<std::ptrdiff_t>(*o + n)};
    }
    std::uint32_t U32(std::uint32_t va) const {
        const auto o = Offset(va);
        return o ? U32At(*o) : 0;
    }
    std::string Str(std::uint32_t va) const {
        std::string s;
        const auto o = Offset(va);
        for (std::size_t i = o ? *o : bytes.size(); i < bytes.size() && bytes[i] != 0 && s.size() < 64; ++i) {
            s += static_cast<char>(bytes[i]);
        }
        return s;
    }
    std::string SectionName(const Section& s) const {
        std::string n;
        for (std::size_t i = s.nameAddr - base; i < bytes.size() && bytes[i] != 0 && n.size() < 16; ++i) {
            n += static_cast<char>(bytes[i]);
        }
        return n;
    }
};

std::optional<Xbe> LoadXbe() {
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
    Xbe x;
    x.bytes.assign(std::istreambuf_iterator<char>(f), {});
    if (x.bytes.size() < 0x200 || std::memcmp(x.bytes.data(), "XBEH", 4) != 0) {
        return std::nullopt;
    }
    x.base = x.U32At(0x104);
    const std::uint32_t count = x.U32At(0x11C);
    const std::uint32_t headers = x.U32At(0x120);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t o = headers - x.base + i * 0x38;
        x.sections.push_back(
            {x.U32At(o + 4), x.U32At(o + 8), x.U32At(o + 12), x.U32At(o + 16), x.U32At(o + 20)});
    }
    return x;
}

// The extracted level scripts (levelN-M.ful), optional.
fs::path ScriptDir() {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(L"FUT_TEST_SCRIPTS", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        return fs::path(buf);
    }
    return fs::path(L"D:\\FuturamaDecomp\\run\\g4_select\\data\\FutPAL01\\scripts");
}

std::vector<std::uint8_t> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return {};
    }
    return {std::istreambuf_iterator<char>(f), {}};
}

std::vector<std::uint8_t> Hex(std::initializer_list<int> v) {
    std::vector<std::uint8_t> out;
    for (int b : v) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

// Relative branches (E8/E9 rel32, EB/7x rel8, 0F 8x rel32) anywhere in .text whose target lies strictly
// inside (start, start + stolen) - a raw byte scan, it also decodes data, so 0 hits is the strong statement.
int BranchesInto(const Xbe& x, std::uint32_t start, std::uint32_t stolen) {
    int hits = 0;
    for (const Xbe::Section& s : x.sections) {
        if (x.SectionName(s) != ".text") {
            continue;
        }
        const std::uint8_t* b = x.bytes.data() + s.raw;
        for (std::uint32_t i = 0; i + 6 < s.rsize; ++i) {
            const std::uint32_t at = s.va + i;
            std::int64_t target = -1;
            if (b[i] == 0xE8 || b[i] == 0xE9) {
                std::int32_t rel = 0;
                std::memcpy(&rel, b + i + 1, 4);
                target = static_cast<std::int64_t>(at) + 5 + rel;
            } else if (b[i] == 0xEB || (b[i] >= 0x70 && b[i] <= 0x7F)) {
                target = static_cast<std::int64_t>(at) + 2 + static_cast<std::int8_t>(b[i + 1]);
            } else if (b[i] == 0x0F && b[i + 1] >= 0x80 && b[i + 1] <= 0x8F) {
                std::int32_t rel = 0;
                std::memcpy(&rel, b + i + 2, 4);
                target = static_cast<std::int64_t>(at) + 6 + rel;
            }
            if (target > static_cast<std::int64_t>(start) &&
                target < static_cast<std::int64_t>(start + stolen)) {
                ++hits;
            }
        }
    }
    return hits;
}

inp::PadState Dpad(std::uint16_t bits) {
    inp::PadState s;
    s.buttons = bits;
    return s;
}

std::uint16_t Bit(ct::Dir d) {
    switch (d) {
    case ct::Dir::Up:
        return inp::kButtonDpadUp;
    case ct::Dir::Down:
        return inp::kButtonDpadDown;
    case ct::Dir::Left:
        return inp::kButtonDpadLeft;
    default:
        return inp::kButtonDpadRight;
    }
}

// Presses `seq` (2 ticks held, 3 released each); returns every sequence index that fired, in order.
std::vector<int> PressAll(ct::DpadSequenceDetector& d, const ct::Sequence& seq) {
    std::vector<int> hits;
    for (ct::Dir dir : seq) {
        for (int t = 0; t < 2; ++t) {
            const int r = d.Feed(Dpad(Bit(dir)), true);
            if (r >= 0) {
                hits.push_back(r);
            }
        }
        for (int t = 0; t < 3; ++t) {
            const int r = d.Feed(Dpad(0), true);
            if (r >= 0) {
                hits.push_back(r);
            }
        }
    }
    return hits;
}

// Longest k (1..7) with the last k presses of `a` equal to the first k of `b`.
std::size_t Overlap(const ct::Sequence& a, const ct::Sequence& b) {
    std::size_t best = 0;
    for (std::size_t k = 1; k < a.size(); ++k) {
        bool same = true;
        for (std::size_t i = 0; i < k && same; ++i) {
            same = a[a.size() - k + i] == b[i];
        }
        if (same) {
            best = k;
        }
    }
    return best;
}

}  // namespace

TEST_CASE("G8 freeze timer: [cheats] schema row, settings and hook conditions") {
    const core::ConfigKeyDef* def = core::FindConfigKey("cheats", "freeze_timer");
    REQUIRE(def != nullptr);
    CHECK(def->type == core::ConfigKeyType::Bool);
    CHECK(core::ConfigKeyDefault("cheats", "freeze_timer") == "0");
    CHECK(def->tab == core::ConfigUiTab::Cheats);
    CHECK(def->widget == core::ConfigUiWidget::Check);
    CHECK(std::string(def->label.de) == "Timer einfrieren (↑ ← ↓ → ↑ ← ↓ →)");
    CHECK(std::string(def->label.en) == "Freeze timer (↑ ← ↓ → ↑ ← ↓ →)");
    // The help names every countdown it holds.
    for (const char* level : {"4-1", "2-4", "3-5", "5-1", "5-2", "5-3"}) {
        CHECK(std::string(def->help.de).find(level) != std::string::npos);
        CHECK(std::string(def->help.en).find(level) != std::string::npos);
    }
    // Before the in-game toggles row (launcher order), whose help lists the new sequence.
    const core::ConfigKeyDef* toggles = core::FindConfigKey("cheats", "ingame_toggles");
    REQUIRE(toggles != nullptr);
    CHECK(def->order < toggles->order);
    CHECK(def->order > core::FindConfigKey("cheats", "infinite_ammo")->order);
    CHECK(std::string(toggles->help.de).find("Timer einfrieren = ↑ ← ↓ → ↑ ← ↓ →") != std::string::npos);
    CHECK(std::string(toggles->help.en).find("freeze timer = ↑ ← ↓ → ↑ ← ↓ →") != std::string::npos);

    core::Config cfg;
    ch::Settings s = ch::SettingsFromConfig(cfg);
    CHECK_FALSE(s.freezeTimer);
    CHECK_FALSE(s.AnyHook());  // default: no hook
    CHECK_FALSE(ch::TimerHooksWanted(cfg));

    cfg.values["cheats.freeze_timer"] = "1";
    CHECK(ch::TimerHooksWanted(cfg));
    s = ch::SettingsFromConfig(cfg);
    CHECK(s.freezeTimer);
    CHECK(s.Enabled(ch::Cheat::FreezeTimer));
    CHECK(s.AnyHook());
    // The other cheats' hooks are not wanted for it.
    CHECK_FALSE(ch::InvulnerableHooksWanted(cfg));
    CHECK_FALSE(ch::OneHitKillHooksWanted(cfg));
    CHECK_FALSE(ch::EnemiesIgnoreHooksWanted(cfg));
    cfg.values["cheats.freeze_timer"] = "0";

    // Other cheats alone: no timer hooks.
    for (const char* key : {"invulnerable", "one_hit_kill", "enemies_ignore", "teleport_freecam", "moon_jump",
                            "infinite_ammo"}) {
        cfg.values[std::string("cheats.") + key] = "1";
        CHECK_FALSE(ch::TimerHooksWanted(cfg));
        cfg.values[std::string("cheats.") + key] = "0";
    }
    cfg.values["cheats.ingame_toggles"] = "1";  // toggles: every hook
    CHECK(ch::TimerHooksWanted(cfg));
    CHECK_FALSE(ch::SettingsFromConfig(cfg).freezeTimer);  // installed, but off until toggled

    CHECK(std::string(ch::CheatKey(ch::Cheat::FreezeTimer)) == "freeze_timer");
    CHECK(std::string(ch::CheatLabelDe(ch::Cheat::FreezeTimer)) == "Timer einfrieren");
    CHECK(ch::kCheatCount == 7);
}

TEST_CASE("G8 freeze timer: the D-pad sequence is unique and does not overlap the others") {
    REQUIRE(ct::kSequences.size() == static_cast<std::size_t>(ch::kCheatCount));
    const std::size_t freeze = static_cast<std::size_t>(ch::Cheat::FreezeTimer);
    CHECK(ct::kSequences[freeze] == ct::kFreezeTimerSequence);
    CHECK(std::string(ct::SequenceText(static_cast<int>(freeze))) == "UP LEFT DOWN RIGHT UP LEFT DOWN RIGHT");
    for (std::size_t a = 0; a < ct::kSequences.size(); ++a) {
        for (std::size_t b = a + 1; b < ct::kSequences.size(); ++b) {
            CHECK(ct::kSequences[a] != ct::kSequences[b]);
        }
    }
    // No prefix/suffix overlap of more than one press with any other sequence (in both directions): a stray
    // tail of another sequence followed by the freeze sequence (or the other way round) never completes a
    // third one.
    for (std::size_t o = 0; o < ct::kSequences.size(); ++o) {
        if (o == freeze) {
            continue;
        }
        CAPTURE(o);
        CHECK(Overlap(ct::kSequences[o], ct::kFreezeTimerSequence) <= 1);
        CHECK(Overlap(ct::kFreezeTimerSequence, ct::kSequences[o]) <= 1);
    }
    // The G7 level skip is A/B, the game's own codes LT + face buttons: the D-pad sequences cannot collide.
    ct::DpadSequenceDetector d;
    d.Configure(60);
    CHECK(PressAll(d, ct::kFreezeTimerSequence) == std::vector<int>{static_cast<int>(freeze)});
    // Each other sequence right before it: both fire, nothing else.
    for (std::size_t o = 0; o < ct::kSequences.size(); ++o) {
        if (o == freeze) {
            continue;
        }
        CAPTURE(o);
        CHECK(PressAll(d, ct::kSequences[o]) == std::vector<int>{static_cast<int>(o)});
        CHECK(PressAll(d, ct::kFreezeTimerSequence) == std::vector<int>{static_cast<int>(freeze)});
    }
    // Half of the moon jump circle (clockwise) and then the freeze sequence: only the freeze sequence fires.
    for (ct::Dir dir : {ct::Dir::Up, ct::Dir::Right, ct::Dir::Down, ct::Dir::Left}) {
        d.Feed(Dpad(Bit(dir)), true);
        d.Feed(Dpad(0), true);
    }
    CHECK(PressAll(d, ct::kFreezeTimerSequence) == std::vector<int>{static_cast<int>(freeze)});
}

TEST_CASE("G8 freeze timer: HUD timer state, countdown sites, held wait and display") {
    CHECK(ch::HudTimerCounting(true, false));
    CHECK_FALSE(ch::HudTimerCounting(true, true));    // "pause"
    CHECK_FALSE(ch::HudTimerCounting(false, false));  // "stop" / never played
    CHECK_FALSE(ch::HudTimerCounting(false, true));

    // Sites: level (case-insensitive like the game's _stricmp), code offset and signature must all match.
    for (const ch::ScriptCountdownSite& s : ch::kScriptCountdowns) {
        CAPTURE(s.level);
        CHECK(ch::FindCountdownSite(s.level, s.code, s.words) == &s);
        std::string lower(s.level);
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        CHECK(ch::FindCountdownSite(lower, s.code, s.words) == &s);  // the session holds "level2-3"
        CHECK(ch::FindCountdownSite(s.level, s.code + 4, s.words) == nullptr);
        CHECK(ch::FindCountdownSite("Level1-1", s.code, s.words) == nullptr);
        const std::uint32_t other[3] = {s.words[0], s.words[1] ^ 1u, s.words[2]};
        CHECK(ch::FindCountdownSite(s.level, s.code, other) == nullptr);
        // Every signature ends in a wait opcode (0x18, top byte) of the same source line as its push.
        CHECK((s.words[2] >> 24) == ch::kScriptOpWait);
        CHECK(((s.words[0] >> 8) & 0xFFFFu) == ((s.words[2] >> 8) & 0xFFFFu));
        CHECK(((s.words[0] >> 24) == 0x08u || (s.words[0] >> 24) == 0x05u));  // push_imm / push_var
    }
    CHECK(std::size(ch::kScriptCountdowns) == 5);
    // 2-4: Wait(60.0); 3-5: Wait(1.0) per step; 5-x: Wait(m_Timeout) = push_var this+0x28.
    CHECK(ch::kScriptCountdowns[0].words[1] == 0x42700000u);
    CHECK(ch::kScriptCountdowns[1].words[1] == 0x3F800000u);
    CHECK(ch::kScriptCountdowns[2].words[1] == 0x28u);

    // Held wait: the scheduler's subtraction leaves it unchanged; dt 0 (cut scenes) and invalid input
    // unchanged.
    for (float wait : {59.98f, 1.0f, 4.5f, 0.016f, 0.0f}) {
        for (float dt : {1.0f / 60.0f, 0.02f}) {
            const float before = ch::HeldWaitBeforeStep(wait, dt);
            CHECK(before == doctest::Approx(wait + dt));
            CHECK(before - dt == doctest::Approx(wait).epsilon(1e-5));
            CHECK(before - dt > (wait > 0.0f ? 0.0f : -1e-6f));  // a positive wait never reaches zero
        }
    }
    CHECK(ch::HeldWaitBeforeStep(3.0f, 0.0f) == 3.0f);
    CHECK(ch::HeldWaitBeforeStep(3.0f, -0.1f) == 3.0f);
    CHECK(ch::HeldWaitBeforeStep(3.0f, std::nanf("")) == 3.0f);
    CHECK(std::isnan(ch::HeldWaitBeforeStep(std::nanf(""), 0.1f)));

    // The display: "%d:%02d" of the truncated seconds (FUN_000EC870).
    char t[16];
    ch::FormatHudTime(120.0f, t);
    CHECK(std::string(t) == "2:00");
    ch::FormatHudTime(119.99f, t);
    CHECK(std::string(t) == "1:59");
    ch::FormatHudTime(60.0f, t);
    CHECK(std::string(t) == "1:00");
    ch::FormatHudTime(55.0f, t);
    CHECK(std::string(t) == "0:55");
    ch::FormatHudTime(0.5f, t);
    CHECK(std::string(t) == "0:00");
    ch::FormatHudTime(-3.0f, t);
    CHECK(std::string(t) == "0:00");
    ch::FormatHudTime(std::nanf(""), t);
    CHECK(std::string(t) == "0:00");
}

TEST_CASE("G8 freeze timer: probe sites and HUD timer data agree with the retail XBE") {
    const auto xbe = LoadXbe();
    if (!xbe) {
        MESSAGE("skipped: retail XBE not found (FUT_TEST_XBE)");
        return;
    }
    // probe:HudTimer::Update: mov al,[ecx+0x2C]; test al,al (stolen) | je; mov al,[ecx+0x2D]; ...
    CHECK(xbe->At(ch::kHudTimerUpdate, 9) == Hex({0x8A, 0x41, 0x2C, 0x84, 0xC0, 0x74, 0x39, 0x8A, 0x41}));
    CHECK(xbe->At(ch::kHudTimerUpdate + 9, 1) == Hex({static_cast<int>(ch::kHudTimerPaused)}));
    // seconds -= [0x2D6748] (tick dt); at <= 0: 0, and for the swamp variant (+0x2E) the expired flag +0x2F
    // = 1.
    CHECK(xbe->At(0x000EC6DE, 12) ==
          Hex({0xD9, 0x41, 0x28, 0xD8, 0x25, 0x48, 0x67, 0x2D, 0x00, 0xD9, 0x51, 0x28}));
    CHECK(xbe->At(0x000EC6F7, 3) == Hex({0x8A, 0x41, static_cast<int>(ch::kHudTimerSwamp)}));
    CHECK(xbe->At(0x000EC70C, 4) == Hex({0xC6, 0x41, static_cast<int>(ch::kHudTimerExpired), 0x01}));
    CHECK(xbe->At(0x000EC710, 1) == Hex({0xC3}));  // plain ret
    CHECK(BranchesInto(*xbe, ch::kHudTimerUpdate, 5) == 0);
    // vtable 0x2A55F8: +0x4 update, +0x10 command handler, +0x14 show/hide.
    CHECK(xbe->U32(ch::kHudTimerVtable + 0x4) == ch::kHudTimerUpdate);
    CHECK(xbe->U32(ch::kHudTimerVtable + 0x10) == ch::kHudTimerCommand);
    CHECK(xbe->U32(ch::kHudTimerVtable + 0x14) == 0x000ECB30u);
    CHECK(xbe->U32(ch::kHudTimerVtable + 0x8) == 0x000EC920u);
    // Commands: "play" "pause" "set" "add" "stop" (strncmp 4).
    CHECK(xbe->At(0x000EC726, 7) == Hex({0x6A, 0x04, 0x68, 0xF0, 0x55, 0x2A, 0x00}));
    CHECK(xbe->Str(0x002A55F0) == "play");
    CHECK(xbe->Str(0x002A55E8) == "pause");
    CHECK(xbe->Str(0x002A55E4) == "set");
    CHECK(xbe->Str(0x002A2064) == "add");
    CHECK(xbe->Str(0x002A55DC) == "stop");
    CHECK(xbe->Str(0x002A5614) == "%d:%02d");
    // "set": seconds +0x28 = argument, expired flag +0x2F = 0 for a positive time.
    CHECK(xbe->At(0x000EC77D, 7) == Hex({0x8B, 0x44, 0x24, 0x10, 0x89, 0x46, 0x28}));
    CHECK(xbe->At(0x000EC796, 4) == Hex({0xC6, 0x46, 0x2F, 0x00}));
    // The element "timer" (0x29A668) and the script native 0x184 FUN_000B09E0: the HUD (session +0x118) looks
    // the element up by name (FUN_000E5890) and returns its expired flag.
    CHECK(xbe->Str(0x0029A668) == "timer");
    CHECK(xbe->At(0x000B09E5, 11) == Hex({0x8B, 0x88, 0x18, 0x01, 0x00, 0x00, 0x68, 0x68, 0xA6, 0x29, 0x00}));
    CHECK(xbe->At(0x000B09F5, 3) == Hex({0x8A, 0x40, static_cast<int>(ch::kHudTimerExpired)}));
    // The 4-1 variant (FUN_000ECB70(1)) loads the swamp bar, clock and flag.
    CHECK(xbe->Str(0x002A561C) == "HUD\\swamp_fl.nif");
    CHECK(xbe->Str(0x002A5630) == "HUD\\swamp_cl.nif");
    CHECK(xbe->Str(0x002A5644) == "HUD\\swamp_bar.nif");

    // probe:Script::RunThreads FUN_000BA060: push ecx; push edi; mov edi,ecx; mov ecx,[0x2EA218] (stolen).
    CHECK(xbe->At(ch::kScriptRunThreads, 10) ==
          Hex({0x51, 0x57, 0x8B, 0xF9, 0x8B, 0x0D, 0x18, 0xA2, 0x2E, 0x00}));
    CHECK(BranchesInto(*xbe, ch::kScriptRunThreads, 10) == 0);
    // Thread list +0x10, next +0x3C, done +0x25, error +0x24, wait +0x20 -= dt ([esp+0x10]), ret 4.
    CHECK(xbe->At(0x000BA07B, 3) == Hex({0x8B, 0x4F, static_cast<int>(ch::kScriptThreadList)}));
    CHECK(xbe->At(0x000BA083, 3) == Hex({0x8A, 0x41, static_cast<int>(ch::kThreadDone)}));
    CHECK(xbe->At(0x000BA088, 3) == Hex({0x8B, 0x71, static_cast<int>(ch::kThreadNext)}));
    CHECK(xbe->At(0x000BA08D, 3) == Hex({0x8A, 0x41, static_cast<int>(ch::kThreadError)}));
    CHECK(xbe->At(0x000BA094, 10) == Hex({0xD9, 0x41, static_cast<int>(ch::kThreadWait), 0xD8, 0x64, 0x24,
                                          0x10, 0xD9, 0x51, static_cast<int>(ch::kThreadWait)}));
    CHECK(xbe->At(0x000BA09E, 3) == Hex({0xD9, 0x41, static_cast<int>(ch::kThreadTimer)}));
    CHECK(xbe->At(0x000BA109, 3) == Hex({0xC2, 0x04, 0x00}));
    // Called by the in-game update with the tick dt for the level script (session +0xA8): 0xC8DC8 mov edx,
    // [0x2D6748]; push edx; lea ecx,[esi+0xA8]; call 0xBA060.
    CHECK(xbe->At(0x000C8DC8, 13) ==
          Hex({0x8B, 0x15, 0x48, 0x67, 0x2D, 0x00, 0x52, 0x8D, 0x8E, 0xA8, 0x00, 0x00, 0x00}));
    CHECK(xbe->At(0x000C8DD5, 1) == Hex({0xE8}));
    std::int32_t rel = 0;
    const auto call = xbe->At(0x000C8DD6, 4);
    REQUIRE(call.size() == 4);
    std::memcpy(&rel, call.data(), 4);
    CHECK(static_cast<std::uint32_t>(0x000C8DDA + rel) == ch::kScriptRunThreads);
}

TEST_CASE("G8 freeze timer: the countdown sites agree with the level scripts") {
    const fs::path dir = ScriptDir();
    int checked = 0;
    for (const ch::ScriptCountdownSite& s : ch::kScriptCountdowns) {
        std::string file(s.level);
        for (char& c : file) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const std::vector<std::uint8_t> ful = ReadFile(dir / (file + ".ful"));
        if (ful.empty()) {
            continue;
        }
        CAPTURE(s.level);
        REQUIRE(ful.size() > s.code);
        CHECK(std::memcmp(ful.data(), "L2iM", 4) == 0);
        std::uint32_t w[3] = {};
        std::memcpy(w, ful.data() + s.code - 12, 12);
        CHECK(w[0] == s.words[0]);
        CHECK(w[1] == s.words[1]);
        CHECK(w[2] == s.words[2]);
        ++checked;
    }
    if (checked == 0) {
        MESSAGE("skipped: level scripts not found (FUT_TEST_SCRIPTS)");
    }
}
