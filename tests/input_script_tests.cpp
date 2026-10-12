// Unit tests of the input script (input/input_script.h, M4 test automation): parser, tick timing of the player,
// conditions, loops, failures, the shared pad merge of input/input_api.h and the checked-in example scripts
// (tools\input_scripts\*.txt must parse).
#include "doctest.h"

#include "core/log.h"
#include "input/input_api.h"
#include "input/input_script.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace inp;
using namespace inp::script;

namespace {

class FakeEnv final : public Environment {
public:
    std::map<CondKind, bool> values;
    std::string context = "Intro";
    std::vector<std::uint64_t> logMatches;
    std::uint64_t frames = 0;
    std::vector<std::string> messages;
    std::vector<std::string> errors;
    std::vector<std::string> screenshots;
    int quitCode = -1;

    bool Test(const Condition& c) noexcept override {
        if (c.kind == CondKind::Context) {
            return c.argument == context;
        }
        const auto it = values.find(c.kind);
        return it != values.end() && it->second;
    }
    std::uint64_t LogMatches(int pattern) noexcept override {
        return static_cast<std::size_t>(pattern) < logMatches.size() ? logMatches[static_cast<std::size_t>(pattern)]
                                                                     : 0;
    }
    std::uint64_t Frames() noexcept override {
        return frames;
    }
    void Message(bool error, const std::string& line) noexcept override {
        (error ? errors : messages).push_back(line);
    }
    void Screenshot(const std::string& name) noexcept override {
        screenshots.push_back(name);
    }
    void Quit(int code, const std::string&) noexcept override {
        quitCode = code;
    }
    bool Unplug(bool unplugged) noexcept override {
        unplugCalls.push_back(unplugged);
        return supportsUnplug;
    }
    std::vector<bool> unplugCalls;
    bool supportsUnplug = true;
};

Program MustParse(const std::string& text, std::uint32_t rate = 60) {
    Program p;
    std::string error;
    const bool ok = Parse(text, rate, p, &error);
    INFO(error);
    REQUIRE(ok);
    return p;
}

std::string ParseError(const std::string& text) {
    Program p;
    std::string error;
    CHECK_FALSE(Parse(text, 60, p, &error));
    return error;
}

// Steps the player n ticks (one frame per tick, like the lockstep clock at 60 fps) and returns the pads.
std::vector<PadState> Run(Player& player, FakeEnv& env, int n) {
    std::vector<PadState> pads;
    for (int i = 0; i < n; ++i) {
        pads.push_back(player.Step());
        ++env.frames;
    }
    return pads;
}

bool A(const PadState& p) {
    return p.analog[kAnalogA] == 255;
}

fs::path SourceRoot() {
    return fs::path(__FILE__).parent_path().parent_path();
}

}  // namespace

TEST_CASE("input_api: MergePad ORs buttons, takes the larger analog value and stick deflection") {
    PadState a;
    a.buttons = kButtonStart;
    a.analog[kAnalogA] = 40;
    a.thumb[kAxisLX] = -20000;
    a.thumb[kAxisLY] = 100;
    PadState b;
    b.buttons = kButtonDpadUp;
    b.analog[kAnalogA] = 255;
    b.thumb[kAxisLX] = 10000;
    b.thumb[kAxisLY] = -32768;
    MergePad(a, b);
    CHECK(a.buttons == (kButtonStart | kButtonDpadUp));
    CHECK(a.analog[kAnalogA] == 255);
    CHECK(a.thumb[kAxisLX] == -20000);
    CHECK(a.thumb[kAxisLY] == -32768);
    CHECK_FALSE(a.Neutral());
    CHECK(PadState{}.Neutral());
}

TEST_CASE("input_script: controls parse to Xbox values") {
    ControlValue v;
    std::string error;
    REQUIRE(ParseControl("A", v, &error));
    CHECK(v.kind == ControlKind::Analog);
    CHECK(v.index == kAnalogA);
    CHECK(v.value == 255);
    REQUIRE(ParseControl("lt=0.5", v, &error));
    CHECK(v.index == kAnalogLeftTrigger);
    CHECK(v.value == 128);
    REQUIRE(ParseControl("LY=-1.0", v, &error));
    CHECK(v.kind == ControlKind::Axis);
    CHECK(v.index == kAxisLY);
    CHECK(v.value == -32767);
    REQUIRE(ParseControl("start", v, &error));
    CHECK(v.kind == ControlKind::Button);
    CHECK(v.button == kButtonStart);
    REQUIRE(ParseControl("DPAD_LEFT", v, &error));
    CHECK(v.button == kButtonDpadLeft);
    REQUIRE(ParseControl("RS", v, &error));
    CHECK(v.button == kButtonRightThumb);
    CHECK_FALSE(ParseControl("LX", v, &error));      // an axis needs a value
    CHECK_FALSE(ParseControl("LX=1.5", v, &error));  // out of range
    CHECK_FALSE(ParseControl("Z", v, &error));
    CHECK_FALSE(ParseControl("START=0.5", v, &error));

    PadState pad;
    REQUIRE(ParseControl("RX=0.25", v, &error));
    ApplyControl(pad, v, true);
    CHECK(pad.thumb[kAxisRX] == 8192);
    ApplyControl(pad, v, false);
    CHECK(pad.thumb[kAxisRX] == 0);
}

TEST_CASE("input_script: parser accepts the documented commands and converts seconds to ticks") {
    const Program p = MustParse(
        "# comment\n"
        "wait context Frontend timeout 40s   # trailing comment\n"
        "press A\n"
        "press A B 5t\n"
        "hold LY=1.0 2s\n"
        "set RX=-0.5\n"
        "release all\n"
        "release RX\n"
        "wait 30t\n"
        "wait 10f\n"
        "wait 250ms\n"
        "at 600t\n"
        "expect not dialog\n"
        "loop timeout 300s until controllable\n"
        "  repeat 2\n"
        "    press START 1t\n"
        "  end\n"
        "end\n"
        "wait log \"game: context Frontend -> Ingame\" timeout 20s\n"
        "wait not log \"x\"\n"
        "log \"M4: done\"\n"
        "screenshot level1-1_start\n"
        "fail \"bad\"\n"
        "quit 3\n");
    REQUIRE(p.code.size() == 23);
    CHECK(p.code[0].op == Op::WaitCond);
    CHECK(p.code[0].condition.kind == CondKind::Context);
    CHECK(p.code[0].condition.argument == "Frontend");
    CHECK(p.code[0].time == Time{2400, Unit::Ticks});
    CHECK(p.code[1].op == Op::Press);
    CHECK(p.code[1].time.value == kDefaultPressTicks);
    CHECK(p.code[2].controls.size() == 2);
    CHECK(p.code[2].time.value == 5);
    CHECK(p.code[3].op == Op::Hold);
    CHECK(p.code[3].hasTime);
    CHECK(p.code[3].time.value == 120);
    CHECK(p.code[4].op == Op::Hold);
    CHECK_FALSE(p.code[4].hasTime);
    CHECK(p.code[5].op == Op::Release);
    CHECK(p.code[5].controls.empty());
    CHECK(p.code[6].controls.size() == 1);
    CHECK(p.code[8].time == Time{10, Unit::Frames});
    CHECK(p.code[9].time == Time{15, Unit::Ticks});
    CHECK(p.code[10].op == Op::At);
    CHECK(p.code[11].condition.negate);
    CHECK(p.code[12].op == Op::Loop);
    CHECK(p.code[12].jump == 16);
    CHECK(p.code[12].time.value == 18000);
    CHECK(p.code[13].op == Op::Repeat);
    CHECK(p.code[13].jump == 15);
    CHECK(p.code[15].jump == 13);
    CHECK(p.code[16].jump == 12);
    CHECK(p.code[17].condition.kind == CondKind::Log);
    CHECK(p.code[17].condition.logPattern == 0);
    CHECK(p.code[18].condition.negate);
    CHECK(p.logPatterns.size() == 2);
    CHECK(p.code[19].text == "M4: done");
    CHECK(p.code[20].text == "level1-1_start");
    CHECK(p.code[21].op == Op::Fail);
    CHECK(p.code[22].op == Op::Quit);
    CHECK(p.code[22].code == 3);
    // 50 Hz tick rate: seconds follow it.
    CHECK(MustParse("wait 2s\n", 50).code[0].time.value == 100);
    // Default timeout of a condition wait.
    CHECK(MustParse("wait main_menu\n").code[0].time.value == 60u * kDefaultWaitTimeoutSeconds);
}

TEST_CASE("input_script: parser errors name the line") {
    CHECK(ParseError("press A\nbogus\n").find("line 2") == 0);
    CHECK(ParseError("press\n").find("at least one control") != std::string::npos);
    CHECK(ParseError("press A 0t\n").find("at least one tick") != std::string::npos);
    CHECK(ParseError("press A 5f\n").find("frames") != std::string::npos);
    CHECK(ParseError("loop until main_menu\n").find("without end") != std::string::npos);
    CHECK(ParseError("end\n").find("without loop") != std::string::npos);
    CHECK(ParseError("wait sometimes\n").find("unknown condition") != std::string::npos);
    CHECK(ParseError("wait context\n").find("one argument") != std::string::npos);
    CHECK(ParseError("log \"open\n").find("quote") != std::string::npos);
    CHECK(ParseError("quit 999\n").find("0..255") != std::string::npos);
    CHECK(ParseError("screenshot a/b\n").find("names") != std::string::npos);
    CHECK(ParseError("wait main_menu timeout\n").find("timeout needs") != std::string::npos);
    CHECK(ParseError("loop timeout 5s main_menu\nend\n").find("loop syntax") != std::string::npos);
}

TEST_CASE("input_script: press holds n ticks, then one released tick before the next command") {
    FakeEnv env;
    Player player(MustParse("press A 3t\npress A 2t\n"), env);
    const std::vector<PadState> pads = Run(player, env, 9);
    // ticks: 0 1 2 = A, 3 = released (gap), 4 5 = A, 6 = released, 7 = finished
    const bool expected[] = {true, true, true, false, true, true, false, false, false};
    for (int i = 0; i < 9; ++i) {
        INFO("tick " << i);
        CHECK(A(pads[static_cast<std::size_t>(i)]) == expected[i]);
    }
    CHECK(player.State() == PlayerState::Finished);
    CHECK(player.Presses() == 2);
    CHECK(env.errors.empty());
}

TEST_CASE("input_script: hold with a time releases without a gap; set/release keep values") {
    FakeEnv env;
    Player player(MustParse("hold LY=1.0 2t\nhold LY=-0.5 1t\nset RX=1\nwait 2t\nrelease RX\n"), env);
    const std::vector<PadState> pads = Run(player, env, 6);
    CHECK(pads[0].thumb[kAxisLY] == 32767);
    CHECK(pads[1].thumb[kAxisLY] == 32767);
    CHECK(pads[2].thumb[kAxisLY] == -16384);  // next hold starts in the tick the first one ends
    CHECK(pads[3].thumb[kAxisLY] == 0);
    CHECK(pads[3].thumb[kAxisRX] == 32767);
    CHECK(pads[4].thumb[kAxisRX] == 32767);
    CHECK(pads[5].thumb[kAxisRX] == 0);
}

TEST_CASE("input_script: wait, at and frame-based waits") {
    FakeEnv env;
    Player player(MustParse("wait 5t\npress X 1t\nat 10t\npress Y 1t\nwait 3f\npress B 1t\n"), env);
    std::vector<PadState> pads = Run(player, env, 20);
    auto first = [&](AnalogButton b) {
        for (std::size_t i = 0; i < pads.size(); ++i) {
            if (pads[i].analog[b] != 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    CHECK(first(kAnalogX) == 5);
    CHECK(first(kAnalogY) == 10);
    CHECK(first(kAnalogB) == 15);  // Y 10, gap 11 -> wait 3 frames from 12
}

TEST_CASE("input_script: frames advancing faster than ticks (120 fps) keep tick timing") {
    FakeEnv env;
    Player player(MustParse("wait 4t\npress A 1t\nwait 4f\npress B 1t\n"), env);
    int aTick = -1;
    int bTick = -1;
    for (int i = 0; i < 20; ++i) {
        const PadState p = player.Step();
        env.frames += 2;
        if (A(p) && aTick < 0) {
            aTick = i;
        }
        if (p.analog[kAnalogB] != 0 && bTick < 0) {
            bTick = i;
        }
    }
    CHECK(aTick == 4);
    CHECK(bTick == 8);  // gap at 5 (frames 10), wait 4 frames from tick 6 (frames 12) -> frames 16 = tick 8
}

TEST_CASE("input_script: condition waits continue in the same tick and time out with a failure") {
    FakeEnv env;
    Player player(MustParse("wait context Frontend timeout 10t\npress START 1t\nwait main_menu timeout 5t\n"),
                  env);
    Run(player, env, 3);
    CHECK(player.State() == PlayerState::Running);
    env.context = "Frontend";
    std::vector<PadState> pads = Run(player, env, 1);
    CHECK((pads[0].buttons & kButtonStart) != 0);  // the press starts in the tick the condition became true
    pads = Run(player, env, 7);  // gap tick 4, the main_menu wait from tick 5 times out at tick 10 (5 ticks)
    CHECK(player.State() == PlayerState::Failing);
    REQUIRE(env.errors.size() == 1);
    CHECK(env.errors[0].find("FAILED at line 3: timeout waiting for \"main_menu\"") != std::string::npos);
    CHECK(env.screenshots == std::vector<std::string>{"fail"});
    // The process ends two frames after the failure (screenshot), with exit code 7.
    Run(player, env, 3);
    CHECK(env.quitCode == kExitInputScriptFailed);
    CHECK(player.State() == PlayerState::Quit);
}

TEST_CASE("input_script: expect fails at once; negated conditions") {
    FakeEnv env;
    env.values[CondKind::Dialog] = false;
    Player ok(MustParse("expect no_dialog\nexpect not dialog\nexpect true\n"), env);
    Run(ok, env, 1);
    CHECK(ok.State() == PlayerState::Finished);
    env.values[CondKind::Dialog] = true;
    Player bad(MustParse("expect no_dialog\npress A\n"), env);
    const std::vector<PadState> pads = Run(bad, env, 2);
    CHECK(bad.State() == PlayerState::Failing);
    CHECK_FALSE(A(pads[0]));  // A is never pressed after the failed expect
    CHECK(bad.FailureReason().find("expected \"not dialog\"") != std::string::npos);
}

TEST_CASE("input_script: log conditions consume matches") {
    FakeEnv env;
    env.logMatches = {0};
    Player player(MustParse("wait log \"x\" timeout 100t\npress A 1t\nwait log \"x\" timeout 100t\npress B 1t\n"),
                  env);
    Run(player, env, 3);
    CHECK(player.Line() == 1);
    env.logMatches[0] = 2;  // two lines at once satisfy only the first wait
    std::vector<PadState> pads = Run(player, env, 4);
    CHECK(A(pads[0]));
    CHECK(player.Line() == 3);
    env.logMatches[0] = 3;
    pads = Run(player, env, 1);
    CHECK(pads[0].analog[kAnalogB] == 255);
}

TEST_CASE("input_script: loop until with timeout, repeat counts") {
    FakeEnv env;
    Player player(MustParse("loop timeout 100t until controllable\n  wait 10t\n  press A 1t\nend\n"
                            "repeat 3\n  press X 1t\nend\nquit 0\n"),
                  env);
    int aPresses = 0;
    int xPresses = 0;
    PadState prev;
    for (int i = 0; i < 200 && env.quitCode < 0; ++i) {
        if (i == 35) {
            env.values[CondKind::Controllable] = true;
        }
        const PadState p = player.Step();
        ++env.frames;
        aPresses += (A(p) && !A(prev)) ? 1 : 0;
        xPresses += (p.analog[kAnalogX] != 0 && prev.analog[kAnalogX] == 0) ? 1 : 0;
        prev = p;
    }
    CHECK(aPresses == 3);  // presses at ticks 10, 22, 34; the loop test at 36 sees controllable
    CHECK(xPresses == 3);
    CHECK(env.quitCode == 0);
    CHECK(player.State() == PlayerState::Quit);

    FakeEnv env2;
    Player timeout(MustParse("loop timeout 30t until controllable\n  wait 10t\nend\n"), env2);
    Run(timeout, env2, 40);
    CHECK(timeout.State() != PlayerState::Running);
    CHECK(timeout.FailureReason().find("loop timeout") != std::string::npos);
}

TEST_CASE("input_script: a loop without a wait fails instead of hanging") {
    FakeEnv env;
    Player player(MustParse("loop until false\nend\n"), env);
    Run(player, env, 1);
    CHECK(player.State() == PlayerState::Failing);
    CHECK(player.FailureReason().find("instructions in one tick") != std::string::npos);
}

TEST_CASE("input_script: screenshot waits two frames; log lines carry tick and frame") {
    FakeEnv env;
    Player player(MustParse("screenshot shot1\nlog \"after\"\n"), env);
    Run(player, env, 1);
    CHECK(env.screenshots == std::vector<std::string>{"shot1"});
    CHECK(player.Line() == 1);
    Run(player, env, 2);
    CHECK(player.State() == PlayerState::Finished);
    bool found = false;
    for (const std::string& m : env.messages) {
        found = found || (m.find("input script: line 2: log: after (tick 2, frame 2)") == 0);
    }
    CHECK(found);
}

TEST_CASE("input_script: the checked-in example scripts parse") {
    const fs::path dir = SourceRoot() / "tools" / "input_scripts";
    if (!fs::exists(dir)) {
        MESSAGE("skipped: " << dir.string() << " not found");
        return;
    }
    int n = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".txt") {
            continue;
        }
        std::ifstream f(entry.path(), std::ios::binary);
        std::ostringstream text;
        text << f.rdbuf();
        Program p;
        std::string error;
        INFO(entry.path().string() << ": " << error);
        CHECK(Parse(text.str(), 60, p, &error));
        CHECK_FALSE(p.code.empty());
        ++n;
    }
    CHECK(n > 0);
}

TEST_CASE("input_script: log observers see every logged line") {
    static int seen = 0;
    const core::LogTap observer = [](core::Channel, core::Level, const char* line, std::size_t) noexcept {
        if (std::string_view(line).find("observer-test-line") != std::string_view::npos) {
            ++seen;
        }
    };
    REQUIRE(core::AddLogObserver(observer));
    CHECK(core::AddLogObserver(observer));  // idempotent
    FUT_LOGE(Inp, "observer-test-line %d", 1);
    core::RemoveLogObserver(observer);
    FUT_LOGE(Inp, "observer-test-line %d", 2);
    CHECK(seen == 1);
}

TEST_CASE("input_script: unplug/replug go to the router; without one the script fails") {
    FakeEnv env;
    Player player(MustParse("unplug\nwait 2t\nreplug\n"), env);
    Run(player, env, 4);
    CHECK(env.unplugCalls == std::vector<bool>{true, false});
    CHECK(player.State() == PlayerState::Finished);
    CHECK(ParseError("unplug 2\n").find("no argument") != std::string::npos);

    FakeEnv none;
    none.supportsUnplug = false;
    Player failing(MustParse("unplug\n"), none);
    Run(failing, none, 1);
    CHECK(failing.State() == PlayerState::Failing);
}
