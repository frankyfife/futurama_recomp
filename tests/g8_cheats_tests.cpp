// Unit tests of G8 (xapi/cheats.h, input/cheat_toggles.h): [cheats] options and hook conditions, the damage
// decisions (invulnerable, one-hit kill incl. protected characters and bosses), the D-pad sequence detector, the
// router's feedback pulse, and the probe sites against the retail XBE (%FUT_TEST_XBE% or orig\retail_eu\default.xbe;
// skipped without it).
#include "doctest.h"

#include "core/config.h"
#include "core/config_schema.h"
#include "input/cheat_toggles.h"
#include "input/host_router.h"
#include "xapi/cheats.h"

#include <windows.h>

#include <algorithm>
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
    std::vector<std::uint8_t> At(std::uint32_t va, std::size_t n) const {
        const auto o = Offset(va);
        if (!o || *o + n > bytes.size()) {
            return {};
        }
        return {bytes.begin() + static_cast<std::ptrdiff_t>(*o), bytes.begin() + static_cast<std::ptrdiff_t>(*o + n)};
    }
    std::uint32_t U32(std::uint32_t va) const {
        const auto o = Offset(va);
        return o ? U32At(*o) : 0;
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

std::vector<std::uint8_t> Hex(std::initializer_list<int> v) {
    std::vector<std::uint8_t> out;
    for (int b : v) {
        out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
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

// Presses `seq` (2 ticks held, 3 released per press); returns the last Feed result != -1 or -1.
int PressAll(ct::DpadSequenceDetector& d, const ct::Sequence& seq, int releaseTicks = 3, bool armed = true) {
    int hit = -1;
    for (ct::Dir dir : seq) {
        for (int t = 0; t < 2; ++t) {
            const int r = d.Feed(Dpad(Bit(dir)), armed);
            hit = r >= 0 ? r : hit;
        }
        for (int t = 0; t < releaseTicks; ++t) {
            const int r = d.Feed(Dpad(0), armed);
            hit = r >= 0 ? r : hit;
        }
    }
    return hit;
}

class MotorSource final : public inp::InputSource {
public:
    const char* Name() const noexcept override { return "pad (test)"; }
    inp::SourceKind Kind() const noexcept override { return inp::SourceKind::Physical; }
    int DefaultPort() const noexcept override { return 0; }
    bool Connected() const noexcept override { return true; }
    bool Sample(int, std::uint64_t, inp::PadState& out) noexcept override {
        out = {};
        return true;
    }
    void SetRumble(int, const inp::RumbleState& r) noexcept override { rumble = r; }
    bool HasMotors() const noexcept override { return true; }
    inp::RumbleState rumble;
};

}  // namespace

TEST_CASE("G8: [cheats] schema rows, settings and hook conditions") {
    for (const char* key : {"invulnerable", "one_hit_kill", "enemies_ignore", "ingame_toggles"}) {
        const core::ConfigKeyDef* def = core::FindConfigKey("cheats", key);
        REQUIRE(def != nullptr);
        CHECK(def->type == core::ConfigKeyType::Bool);
        CHECK(core::ConfigKeyDefault("cheats", key) == "0");
        CHECK(def->tab == core::ConfigUiTab::Game);
    }
    CHECK(core::IsConfigSection("cheats"));

    core::Config cfg;
    ch::Settings s = ch::SettingsFromConfig(cfg);
    CHECK_FALSE(s.AnyHook());  // default: no hook is installed
    CHECK_FALSE(ch::InvulnerableHooksWanted(cfg));
    CHECK_FALSE(ch::OneHitKillHooksWanted(cfg));
    CHECK_FALSE(ch::EnemiesIgnoreHooksWanted(cfg));

    cfg.values["cheats.invulnerable"] = "1";
    CHECK(ch::InvulnerableHooksWanted(cfg));
    CHECK_FALSE(ch::OneHitKillHooksWanted(cfg));
    CHECK_FALSE(ch::EnemiesIgnoreHooksWanted(cfg));
    s = ch::SettingsFromConfig(cfg);
    CHECK(s.Enabled(ch::Cheat::Invulnerable));
    CHECK_FALSE(s.Enabled(ch::Cheat::OneHitKill));

    cfg.values["cheats.invulnerable"] = "0";
    cfg.values["cheats.enemies_ignore"] = "on";
    CHECK(ch::EnemiesIgnoreHooksWanted(cfg));
    CHECK_FALSE(ch::InvulnerableHooksWanted(cfg));

    cfg.values["cheats.enemies_ignore"] = "0";
    cfg.values["cheats.ingame_toggles"] = "1";  // toggles: every cheat's hooks
    CHECK(ch::InvulnerableHooksWanted(cfg));
    CHECK(ch::OneHitKillHooksWanted(cfg));
    CHECK(ch::EnemiesIgnoreHooksWanted(cfg));
    s = ch::SettingsFromConfig(cfg);
    CHECK(s.AnyHook());
    CHECK_FALSE(s.Enabled(ch::Cheat::OneHitKill));

    CHECK(std::string(ch::CheatKey(ch::Cheat::OneHitKill)) == "one_hit_kill");
    CHECK(std::string(ch::CheatLabelDe(ch::Cheat::EnemiesIgnore)) == "Feinde ignorieren mich");
}

TEST_CASE("G8: invulnerable - every player damage is blocked except the instant-death floor") {
    CHECK(ch::BlockPlayerDamage(true, 10.0f, 0x00047300));
    CHECK(ch::BlockPlayerDamage(true, 1000000.0f, 0x00036FBE));  // script FsDamageCharacter(PLAYER, 1e6)
    CHECK(ch::BlockPlayerDamage(true, 5.0f, 0x00038AD1));        // damaging floor (material flag 4)
    CHECK_FALSE(ch::BlockPlayerDamage(true, 100.0f, ch::kKillFloorReturn));  // pit / deadly water / lava
    CHECK_FALSE(ch::BlockPlayerDamage(true, 0.0f, 0x00047300));
    CHECK_FALSE(ch::BlockPlayerDamage(true, -5.0f, 0x00047300));
    CHECK_FALSE(ch::BlockPlayerDamage(false, 10.0f, 0x00047300));
}

TEST_CASE("G8: sewer tentacle - pull-through kill passes, grab skipped, detection held") {
    // The pull-through kill (every update until death) passes like the instant-death floor.
    CHECK_FALSE(ch::BlockPlayerDamage(true, 100.0f, ch::kTentacleKillReturn));
    CHECK(ch::BlockPlayerDamage(true, 2.0f, 0x0008D0DE));  // punch/hit_head while held: blocked
    CHECK(ch::TentacleHooksWanted([] {
        core::Config c;
        c.values["cheats.invulnerable"] = "1";
        return c;
    }()));
    CHECK(ch::TentacleHooksWanted([] {
        core::Config c;
        c.values["cheats.enemies_ignore"] = "1";
        return c;
    }()));
    CHECK_FALSE(ch::TentacleHooksWanted([] {
        core::Config c;
        c.values["cheats.one_hit_kill"] = "1";
        return c;
    }()));
    CHECK_FALSE(ch::TentacleHooksWanted(core::Config{}));

    CHECK(ch::SkipTentacleGrab(true, false, "morph:grab"));
    CHECK(ch::SkipTentacleGrab(false, true, "morph:grab"));
    CHECK(ch::SkipTentacleGrab(true, true, "MORPH:Grab"));  // _stricmp in the game
    CHECK_FALSE(ch::SkipTentacleGrab(false, false, "morph:grab"));
    for (const char* other : {"morph:hit", "morph:punch", "morph:hit_head", "morph:release",
                              "morph:pull_through_pipe", "morph:grab2", "morph:gra", ""}) {
        CHECK_FALSE(ch::SkipTentacleGrab(true, true, other));
    }

    CHECK(ch::TentacleHoldCooldown(0.0f) == doctest::Approx(ch::kTentacleHoldCooldown));
    CHECK(ch::TentacleHoldCooldown(-0.3f) == doctest::Approx(ch::kTentacleHoldCooldown));
    CHECK(ch::TentacleHoldCooldown(0.49f) == doctest::Approx(ch::kTentacleHoldCooldown));
    CHECK(ch::TentacleHoldCooldown(1.0f) == doctest::Approx(1.0f));  // after an attack: unchanged
    CHECK(ch::TentacleHoldCooldown(std::nanf("")) == doctest::Approx(ch::kTentacleHoldCooldown));
}

TEST_CASE("G8: invulnerable safety net - lethal damage blocked for more than 10 s is let through once") {
    CHECK(ch::kLethalBlockLimitSeconds == doctest::Approx(10.0));  // user decision 2026-10-13
    CHECK(ch::IsLethalHit(100.0f, 100.0f));
    CHECK(ch::IsLethalHit(1000000.0f, 100.0f));
    CHECK_FALSE(ch::IsLethalHit(99.0f, 100.0f));
    CHECK_FALSE(ch::IsLethalHit(100.0f, 0.0f));  // already dead

    constexpr std::uint32_t kTentacle = 0x8312345;
    constexpr std::uint32_t kOther = 0x8400000;
    constexpr double kTick = 1.0 / 60.0;
    // Every tick from t = 5 s on (the tentacle's pull-through kill): through after 10 s, not before.
    {
        ch::LethalBlockWatch w;
        int firstThrough = -1;
        for (int i = 0; i <= 11 * 60; ++i) {
            if (w.OnBlockedLethal(kTentacle, 5.0 + i * kTick) && firstThrough < 0) {
                firstThrough = i;
            }
        }
        REQUIRE(firstThrough > 0);
        CHECK(firstThrough * kTick > ch::kLethalBlockLimitSeconds);
        CHECK(firstThrough * kTick < ch::kLethalBlockLimitSeconds + 2 * kTick);
    }
    // 9.9 s: still blocked; a series is reported.
    {
        ch::LethalBlockWatch w;
        bool through = false;
        for (int i = 0; i * kTick < 9.9; ++i) {
            through = through || w.OnBlockedLethal(kTentacle, i * kTick);
        }
        CHECK_FALSE(through);
        CHECK(w.Active());
        CHECK(w.SeriesSeconds() > 9.8);
        CHECK(w.Attacker() == kTentacle);
    }
    // After letting through, the watch starts over (the next series needs another 10 s).
    {
        ch::LethalBlockWatch w;
        double t = 0.0;
        while (!w.OnBlockedLethal(kTentacle, t)) {
            t += kTick;
        }
        CHECK_FALSE(w.Active());
        CHECK_FALSE(w.OnBlockedLethal(kTentacle, t + kTick));
        CHECK_FALSE(w.OnBlockedLethal(kTentacle, t + 5.0 * kTick));
    }
    // A pause longer than the gap or another attacker starts a new series.
    {
        ch::LethalBlockWatch w;
        bool through = false;
        for (int i = 0; i < 8 * 60; ++i) {
            through = through || w.OnBlockedLethal(kTentacle, i * kTick);
        }
        through = through || w.OnBlockedLethal(kTentacle, 8.0 + ch::kLethalBlockGapSeconds + 0.1);  // gap
        for (int i = 1; i < 8 * 60; ++i) {
            through = through || w.OnBlockedLethal(kTentacle, 9.2 + i * kTick);
        }
        CHECK_FALSE(through);  // two series of 8 s each
        ch::LethalBlockWatch v;
        for (int i = 0; i < 12 * 60; ++i) {
            // alternating attackers: each hit starts over
            through = through || v.OnBlockedLethal(i % 2 == 0 ? kTentacle : kOther, i * kTick);
        }
        CHECK_FALSE(through);
        // Script damage (attacker 0) every 0.5 s counts as one continuous series.
        ch::LethalBlockWatch s;
        int hits = 0;
        bool scriptThrough = false;
        for (double t = 0.0; t < 12.0 && !scriptThrough; t += 0.5) {
            scriptThrough = s.OnBlockedLethal(0, t);
            ++hits;
        }
        CHECK(scriptThrough);
        CHECK(hits == 22);  // t = 10.5 s
    }
    // Clock going backwards (should not happen) starts over instead of firing.
    {
        ch::LethalBlockWatch w;
        CHECK_FALSE(w.OnBlockedLethal(kTentacle, 100.0));
        CHECK_FALSE(w.OnBlockedLethal(kTentacle, 50.0));
        CHECK(w.SeriesSeconds() == doctest::Approx(0.0));
    }
}

TEST_CASE("G8: one-hit kill - lethal hits, protected characters, bosses") {
    using K = ch::HitKind;
    // Normal enemy (stormtrooper 200): one hit kills.
    ch::HitDecision d = ch::OneHitDamage(10.0f, 200.0f, 200.0f, true, false);
    CHECK(d.kind == K::Lethal);
    CHECK(d.amount == doctest::Approx(200.0f));
    d = ch::OneHitDamage(10.0f, 35.0f, 200.0f, true, false);  // already hurt: the rest of its health
    CHECK(d.amount == doctest::Approx(35.0f));
    // Already lethal: unchanged (the catch-all hook after an enemy class hook does not count twice).
    d = ch::OneHitDamage(50.0f, 40.0f, 40.0f, true, false);
    CHECK(d.kind == K::Unchanged);
    CHECK(d.amount == doctest::Approx(50.0f));
    // Not the player's hit, a player as target, no damage, already dead.
    CHECK(ch::OneHitDamage(10.0f, 100.0f, 100.0f, false, false).kind == K::Unchanged);
    CHECK(ch::OneHitDamage(10.0f, 100.0f, 100.0f, true, true).kind == K::Unchanged);
    CHECK(ch::OneHitDamage(0.0f, 100.0f, 100.0f, true, false).kind == K::Unchanged);
    CHECK(ch::OneHitDamage(10.0f, 0.0f, 100.0f, true, false).kind == K::Unchanged);
    // The game's invulnerability marker 55555 (SetBigHealth, sungod, igner_boss, plants) and tentacle_dude 55000.
    CHECK(ch::OneHitDamage(10.0f, 55555.0f, 100.0f, true, false).kind == K::Unchanged);
    CHECK(ch::OneHitDamage(10.0f, 55555.0f, 55555.0f, true, false).kind == K::Unchanged);
    CHECK(ch::OneHitDamage(10.0f, 54000.0f, 55000.0f, true, false).kind == K::Unchanged);
    // Bosses: a quarter of the maximum per hit (Destructor 10000, chickenwalker 2000, tank 1000).
    d = ch::OneHitDamage(25.0f, 10000.0f, 10000.0f, true, false);
    CHECK(d.kind == K::Boss);
    CHECK(d.amount == doctest::Approx(2500.0f));
    d = ch::OneHitDamage(25.0f, 600.0f, 1000.0f, true, false);
    CHECK(d.kind == K::Boss);
    CHECK(d.amount == doctest::Approx(250.0f));
    d = ch::OneHitDamage(400.0f, 1000.0f, 1000.0f, true, false);  // a bigger hit stays
    CHECK(d.kind == K::Unchanged);
    // Just below the boss threshold: goonbox 450 dies at once.
    CHECK(ch::OneHitDamage(10.0f, 450.0f, 450.0f, true, false).kind == K::Lethal);
}

TEST_CASE("G8: D-pad sequence detector - the three sequences, releases, other buttons, diagonals, window") {
    CHECK(std::string(ct::SequenceText(0)) == "UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT");
    // The sequences differ (none is another one).
    CHECK(ct::kSequences[0] != ct::kSequences[1]);
    CHECK(ct::kSequences[1] != ct::kSequences[2]);
    CHECK(ct::kSequences[0] != ct::kSequences[2]);

    ct::DpadSequenceDetector d;
    d.Configure(60);
    CHECK(PressAll(d, ct::kInvulnerableSequence) == 0);
    CHECK(PressAll(d, ct::kOneHitKillSequence) == 1);
    CHECK(PressAll(d, ct::kEnemiesIgnoreSequence) == 2);
    // Preceding stray presses do not matter (the last eight count).
    d.Feed(Dpad(inp::kButtonDpadLeft), true);
    d.Feed(Dpad(0), true);
    CHECK(PressAll(d, ct::kInvulnerableSequence) == 0);
    // Held without release counts once: UP held through the first two presses -> no match.
    {
        ct::DpadSequenceDetector h;
        h.Configure(60);
        ct::Sequence s = ct::kInvulnerableSequence;
        int hit = -1;
        for (int t = 0; t < 6; ++t) {
            hit = std::max(hit, h.Feed(Dpad(inp::kButtonDpadUp), true));  // one long UP
        }
        h.Feed(Dpad(0), true);
        for (std::size_t i = 2; i < s.size(); ++i) {
            hit = std::max(hit, h.Feed(Dpad(Bit(s[i])), true));
            h.Feed(Dpad(0), true);
        }
        CHECK(hit == -1);
    }
    // Another button in between starts over.
    {
        ct::DpadSequenceDetector o;
        o.Configure(60);
        ct::Sequence first = {ct::Dir::Up, ct::Dir::Up, ct::Dir::Down, ct::Dir::Down,
                              ct::Dir::Left, ct::Dir::Right, ct::Dir::Left, ct::Dir::Right};
        int hit = -1;
        for (std::size_t i = 0; i < first.size(); ++i) {
            if (i == 4) {
                inp::PadState a;
                a.analog[inp::kAnalogA] = 255;
                o.Feed(a, true);
                o.Feed(Dpad(0), true);
            }
            hit = std::max(hit, o.Feed(Dpad(Bit(first[i])), true));
            o.Feed(Dpad(0), true);
        }
        CHECK(hit == -1);
    }
    // Diagonal (two directions in one tick) starts over.
    {
        ct::DpadSequenceDetector g;
        g.Configure(60);
        int hit = -1;
        for (std::size_t i = 0; i < 8; ++i) {
            const std::uint16_t bits = i == 3 ? static_cast<std::uint16_t>(inp::kButtonDpadDown | inp::kButtonDpadLeft)
                                              : Bit(ct::kInvulnerableSequence[i]);
            hit = std::max(hit, g.Feed(Dpad(bits), true));
            g.Feed(Dpad(0), true);
        }
        CHECK(hit == -1);
    }
    // Pause longer than 1 s and a sequence longer than 5 s fail; not armed fails.
    {
        ct::DpadSequenceDetector p;
        p.Configure(60);
        CHECK(PressAll(p, ct::kInvulnerableSequence, 70) == -1);  // 70 ticks released between presses > 60
        CHECK(PressAll(p, ct::kInvulnerableSequence, 50) == -1);  // gaps ok, but 8 presses take > 300 ticks
        CHECK(PressAll(p, ct::kInvulnerableSequence, 30) == 0);
        CHECK(PressAll(p, ct::kInvulnerableSequence, 3, false) == -1);
    }
    // Sticks are ignored.
    {
        ct::DpadSequenceDetector s;
        s.Configure(60);
        int hit = -1;
        for (ct::Dir dir : ct::kOneHitKillSequence) {
            inp::PadState st = Dpad(Bit(dir));
            st.thumb[inp::kAxisLX] = 30000;
            hit = std::max(hit, s.Feed(st, true));
            inp::PadState rel;
            rel.thumb[inp::kAxisLY] = -20000;
            s.Feed(rel, true);
        }
        CHECK(hit == 1);
    }
    // HasPressEdge only on a new D-pad press.
    ct::DpadSequenceDetector e;
    CHECK(e.HasPressEdge(Dpad(inp::kButtonDpadUp)));
    e.Feed(Dpad(inp::kButtonDpadUp), true);
    CHECK_FALSE(e.HasPressEdge(Dpad(inp::kButtonDpadUp)));
    CHECK(e.HasPressEdge(Dpad(inp::kButtonDpadUp | inp::kButtonDpadLeft)));
}

TEST_CASE("G8: router - feedback pulse overlays the game's rumble, respects rumble = 0 and ends") {
    MotorSource pad;
    inp::HostRouter r;
    r.UseSources({&pad});
    r.SetFocusQuery(nullptr);
    r.Configure({true, 100, false});
    r.PulseRumble(0, {0x8000, 0x8000}, 3);
    CHECK(r.PulseActive(0));
    CHECK(pad.rumble == inp::RumbleState{0x8000, 0x8000});
    r.SetGameRumble(0, {0xFFFF, 0x1000});  // maximum per motor
    CHECK(pad.rumble == inp::RumbleState{0xFFFF, 0x8000});
    r.SetGameRumble(0, {0, 0});
    inp::PadState out;
    REQUIRE(r.SampleGamePort(0, out));
    REQUIRE(r.SampleGamePort(0, out));
    CHECK(pad.rumble == inp::RumbleState{0x8000, 0x8000});
    REQUIRE(r.SampleGamePort(0, out));  // third sample: over
    CHECK_FALSE(r.PulseActive(0));
    CHECK(pad.rumble == inp::RumbleState{});
    // [input] rumble = 0: no pulse; strength scales it.
    r.Configure({false, 100, false});
    r.PulseRumble(0, {0x8000, 0x8000}, 5);
    CHECK(pad.rumble == inp::RumbleState{});
    r.Configure({true, 50, false});
    CHECK(pad.rumble == inp::RumbleState{0x4000, 0x4000});
}

TEST_CASE("G8: probe sites and data agree with the retail XBE") {
    const auto xbe = LoadRetailXbe();
    if (!xbe) {
        MESSAGE("skipped: retail XBE not found (FUT_TEST_XBE)");
        return;
    }
    const auto frame = Hex({0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0});
    CHECK(xbe->At(ch::kPlayerTakeDamage, 6) == frame);
    CHECK(xbe->At(ch::kAiBrainUpdate, 6) == frame);
    // Sewer tentacle: update/animation-event slots, the pull-through kill call and the detection cooldown test.
    CHECK(xbe->At(ch::kTentacleUpdate, 6) == frame);
    CHECK(xbe->At(ch::kTentacleAnimEvent, 6) == frame);
    CHECK(xbe->U32(ch::kTentacleVtable + 0x5C) == ch::kTentacleUpdate);
    CHECK(xbe->U32(ch::kTentacleVtable + 0xE8) == ch::kTentacleAnimEvent);
    CHECK(xbe->U32(ch::kTentacleVtable + 0x7C) == ch::kNpcTakeDamage);
    CHECK(xbe->At(ch::kTentacleKillReturn - 3, 3) == Hex({0xFF, 0x50, 0x7C}));             // call [eax+0x7C]
    CHECK(xbe->At(0x0008C6EA, 6) == Hex({0xD9, 0x87, 0xFC, 0x04, 0x00, 0x00}));            // fld [edi+0x4FC]
    CHECK(xbe->At(0x0008C675, 6) == Hex({0xD9, 0x86, 0xC4, 0x08, 0x00, 0x00}));            // fld [esi+0x8C4]
    CHECK(xbe->At(0x0008C688, 7) == Hex({0xC6, 0x86, 0x08, 0x05, 0x00, 0x00, 0x01}));      // attack flag
    CHECK(xbe->At(0x0008CCF5, 7) == Hex({0xC6, 0x86, 0xC0, 0x08, 0x00, 0x00, 0x01}));      // grab miss
    CHECK(xbe->At(0x0008CAE2, 5) == Hex({0x68, 0xF4, 0xF8, 0x29, 0x00}));                  // push "morph:grab"
    CHECK(xbe->At(0x0029F8F4, 11) == Hex({'m', 'o', 'r', 'p', 'h', ':', 'g', 'r', 'a', 'b', 0}));
    CHECK(xbe->At(0x0008CCF0, 3) == Hex({0xC2, 0x08, 0x00}));                              // ret 8
    CHECK(xbe->At(0x0008C9EF, 3) == Hex({0xC2, 0x04, 0x00}));                              // ret 4
    CHECK(xbe->At(ch::kActorTakeDamage, 6) == Hex({0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}));
    CHECK(xbe->At(ch::kDeductLives, 6) == Hex({0xFF, 0x0D, 0xDC, 0x67, 0x2D, 0x00}));  // dec [0x2D67DC]
    const auto fldSub = Hex({0xD9, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x0C});
    CHECK(xbe->At(ch::kNpcTakeDamage, 7) == fldSub);
    CHECK(xbe->At(ch::kKamikazeTakeDamage, 7) == fldSub);
    CHECK(xbe->At(ch::kJunkbotTakeDamage, 7) == fldSub);
    CHECK(xbe->At(ch::kAquafishTakeDamage, 6) == Hex({0x83, 0xEC, 0x0C, 0x56, 0x8B, 0xF1}));
    CHECK(xbe->At(ch::kStormtrooperTakeDamage, 6) == Hex({0x83, 0xEC, 0x0C, 0x56, 0x8B, 0xF1}));
    CHECK(xbe->At(ch::kGoonTakeDamage, 6) == Hex({0x83, 0xEC, 0x18, 0x56, 0x8B, 0xF1}));
    // vtable slot +0x7C (TakeDamage) of the player classes and the enemy classes.
    for (std::uint32_t vt : ch::kPlayerVtables) {
        CHECK(xbe->U32(vt + 0x7C) == ch::kPlayerTakeDamage);
    }
    CHECK(xbe->U32(0x00295D38 + 0x7C) == ch::kActorTakeDamage);
    CHECK(xbe->U32(0x00297468 + 0x7C) == ch::kNpcTakeDamage);
    CHECK(xbe->U32(0x0029B7E8 + 0x7C) == ch::kKamikazeTakeDamage);
    CHECK(xbe->U32(0x0029D7B0 + 0x7C) == ch::kAquafishTakeDamage);
    for (std::uint32_t vt : {0x0029E3D0u, 0x0029E510u, 0x0029E640u, 0x0029E790u}) {
        CHECK(xbe->U32(vt + 0x7C) == ch::kGoonTakeDamage);
    }
    CHECK(xbe->U32(0x0029ECF8 + 0x7C) == ch::kJunkbotTakeDamage);
    CHECK(xbe->U32(0x002A01F0 + 0x7C) == ch::kStormtrooperTakeDamage);
    // Player TakeDamage calls the actor base (0x51BAB/0x51BDD); the kill-floor call 0x38C46 = call [eax+0x7C].
    CHECK(xbe->At(0x00051BAB, 1) == Hex({0xE8}));
    CHECK(xbe->At(ch::kKillFloorReturn - 3, 3) == Hex({0xFF, 0x50, 0x7C}));
    // FsDeductLives is the only native that decrements the lives (case 0x149 calls it at 0xB1885).
    const auto call = xbe->At(0x000B1885, 5);
    REQUIRE(call.size() == 5);
    std::int32_t rel = 0;
    std::memcpy(&rel, call.data() + 1, 4);
    CHECK(call[0] == 0xE8);
    CHECK(static_cast<std::uint32_t>(0x000B1885 + 5 + rel) == ch::kDeductLives);
    // Hold-fire flag: written by the brain update at 0x2ECE2 (mov [0x2DBAA0], al), read at 0x2E300 and 0x2FA84.
    CHECK(xbe->At(0x0002ECE2, 1) == Hex({0xA2}));
    CHECK(xbe->U32(0x0002ECE3) == ch::kAiHoldFireVar);
    // Camera flash of the game's cheat handler: call 0x13130 at 0x2C7A5.
    const auto flash = xbe->At(0x0002C7A5, 5);
    REQUIRE(flash.size() == 5);
    std::memcpy(&rel, flash.data() + 1, 4);
    CHECK(static_cast<std::uint32_t>(0x0002C7A5 + 5 + rel) == ch::kCameraFlash);
    // God mode marker 55555.0f in Actor::TakeDamage (cmp with 0x47590300).
    float marker = 0.0f;
    const std::uint32_t bits = 0x47590300;
    std::memcpy(&marker, &bits, 4);
    CHECK(marker == ch::kProtectedHealth);
}
