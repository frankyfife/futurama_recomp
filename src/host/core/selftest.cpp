// Self-test registry and runner (design 1.4 step 8). Owned by the core agent.
#include "core/selftest.h"

#include "core/log.h"

#include <exception>
#include <vector>

namespace core {
namespace {

struct Test {
    const char* name;
    SelfTestPhase phase;
    SelfTestFn fn;
    const char* file;
};

std::vector<Test>& Tests() {
    static auto* tests = new std::vector<Test>();
    return *tests;
}

const char* PhaseName(SelfTestPhase p) {
    switch (p) {
    case SelfTestPhase::Host:
        return "host";
    case SelfTestPhase::Mapped:
        return "mapped";
    case SelfTestPhase::AdoptedThread:
        return "adopted-thread";
    }
    return "?";
}

}  // namespace

SelfTestRegistrar::SelfTestRegistrar(const char* name, SelfTestPhase phase, SelfTestFn fn,
                                     const char* file) noexcept {
    try {
        Tests().push_back({name, phase, fn, file});
    } catch (...) {
    }
}

int SelfTestCount(SelfTestPhase phase) noexcept {
    int n = 0;
    for (const Test& t : Tests()) {
        n += t.phase == phase ? 1 : 0;
    }
    return n;
}

namespace {
thread_local bool t_inSelfTest = false;  // set while RunSelfTests runs a test on this thread
}  // namespace

bool InSelfTest() noexcept {
    return t_inSelfTest;
}

int RunSelfTests(SelfTestPhase phase) noexcept {
    if (!GetConfig().selftest) {
        return 0;
    }
    int failures = 0, run = 0;
    for (const Test& t : Tests()) {
        if (t.phase != phase) {
            continue;
        }
        ++run;
        std::string detail;
        bool ok = false;
        t_inSelfTest = true;
        try {
            ok = t.fn(detail);
        } catch (const std::exception& e) {
            detail = std::string("exception: ") + e.what();
        } catch (...) {
            detail = "unknown exception";
        }
        t_inSelfTest = false;
        failures += ok ? 0 : 1;
        LogWrite(Channel::Core, ok ? Level::Info : Level::Error, "SELFTEST %s %s: %s", ok ? "PASS" : "FAIL",
                 t.name, detail.c_str());
    }
    LogWrite(Channel::Core, failures ? Level::Error : Level::Info, "SELFTEST phase %s: %d run, %d failed",
             PhaseName(phase), run, failures);
    return failures;
}

bool StageRunSelfTests(const Config& config) {
    if (!config.selftest) {
        FUT_LOGD(Core, "self-tests disabled (use --selftest)");
        return true;
    }
    // Sequenced on purpose (review finding 7): the order of the operands of '+' is unspecified, and
    // MSVC Debug and RelWithDebInfo evaluated them differently. Host tests need no mapped XBE and run
    // first; a host failure skips the mapped phase.
    int failures = RunSelfTests(SelfTestPhase::Host);
    if (failures != 0) {
        FUT_LOGE(Core, "SELFTEST host phase failed - mapped phase skipped");
        return false;
    }
    failures += RunSelfTests(SelfTestPhase::Mapped);
    return failures == 0;
}

}  // namespace core
