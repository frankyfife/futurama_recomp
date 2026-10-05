// M2 integration (phase 2): core exit reporters (core/host_main.h) and the game observer probes
// (xapi/game_probes.*). The probes themselves run only against the mapped XBE (tools/m2_check.py);
// here: their targets in the generated function table, the language names and the exit reporter list.
#include "doctest.h"

#include "core/guest.h"
#include "core/host_main.h"
#include "vfs/vfs.h"
#include "vfs/vfs_internal.h"
#include "xapi/game_probes.h"

#include <string>
#include <vector>

namespace {

std::vector<std::string>& Calls() {
    static std::vector<std::string> calls;
    return calls;
}
void ReporterB() noexcept {
    Calls().push_back("b");
}
void ReporterA() noexcept {
    Calls().push_back("a");
}
void ReporterADuplicate() noexcept {
    Calls().push_back("a-duplicate");
}

}  // namespace

TEST_CASE("core: exit reporters run once each, sorted by name, first registration of a name wins") {
    const int before = core::ExitReporterCount();
    // The modules register theirs statically ("fibers", "io apc", "xdk"); "~" sorts after them.
    core::AddExitReporter("~test reporter b", &ReporterB);
    core::AddExitReporter("~test reporter a", &ReporterA);
    core::AddExitReporter("~test reporter a", &ReporterADuplicate);  // ignored: same name
    core::AddExitReporter("~test reporter null", nullptr);           // ignored
    CHECK(core::ExitReporterCount() == before + 2);
    Calls().clear();
    core::RunExitReporters();
    CHECK(Calls() == std::vector<std::string>{"a", "b"});
    Calls().clear();
    core::RunExitReporters();  // callable again (only ExitHostProcess runs them once)
    CHECK(Calls().size() == 2u);
}

TEST_CASE("xapi: XC_LANGUAGE names of the XGetLanguage probe (design 2.1, files.md 3.2)") {
    CHECK(std::string(xapi::XboxLanguageName(1)) == "english");
    CHECK(std::string(xapi::XboxLanguageName(3)) == "german");
    CHECK(std::string(xapi::XboxLanguageName(4)) == "french");
    CHECK(std::string(xapi::XboxLanguageName(5)) == "spanish");
    CHECK(std::string(xapi::XboxLanguageName(6)) == "italian");
    CHECK(std::string(xapi::XboxLanguageName(9)) == "portuguese");
    CHECK(std::string(xapi::XboxLanguageName(0)) == "?");
    CHECK(std::string(xapi::XboxLanguageName(10)) == "?");
}

TEST_CASE("xapi: game probe targets are function starts with room for their stolen bytes") {
    struct Target {
        guest::GuestAddr address;
        std::uint32_t patchedBytes;  // probe: stolen bytes; trap: the 5-byte jmp
        const char* name;
    };
    const Target targets[] = {{guest::kGameArchiveMount, 7, "FUN_0013fe70"},
                              {guest::kGameArchiveLookup, 5, "FUN_0013fe10"},
                              {guest::kGameInit, 6, "FUN_000a26a0"},
                              {guest::kCAppFrame, 6, "FUN_000d2630"},
                              {guest::kGamePoolMaintenance, 5, "FUN_000a48c0"},
                              {guest::kXapiXGetLanguage, 5, "XAPILIB::XGetLanguage"},
                              {guest::kGameDiscErrorScreen, 5, "DiscErrorScreen_vf0"}};
    for (const Target& t : targets) {
        CAPTURE(t.address);
        const guest::FunctionInfo* f = guest::FindFunctionStart(t.address);
        REQUIRE(f != nullptr);
        CHECK(std::string(f->name) == t.name);
        CHECK(f->size >= t.patchedBytes);
        for (const auto& site : guest::FsPatchSites()) {
            CHECK_FALSE(
                (site.address < t.address + t.patchedBytes && t.address < site.address + site.length));
        }
    }
    // The disc error screen is not a Ghidra function: extra_function_starts.tsv, 0xF2FD0..0xF30A9.
    const guest::FunctionInfo* disc = guest::FindFunctionStart(guest::kGameDiscErrorScreen);
    REQUIRE(disc != nullptr);
    CHECK(disc->flags == guest::kFunctionExtraStart);
    CHECK(disc->size == 0xD9u);
    // probe:Game::ContextSwitch logs only the call from Game::Frame (return address after `call 0xA48C0`)
    const guest::FunctionInfo* frame = guest::FindFunctionContaining(guest::kGameFrameContextSwitchReturn);
    REQUIRE(frame != nullptr);
    CHECK(frame->address == guest::kGameFrame);
}

TEST_CASE("xapi: game observer statistics start empty") {
    const xapi::GameObserverStats s = xapi::GetGameObserverStats();
    CHECK(s.archivesMounted == 0u);
    CHECK(s.frames == 0u);
    CHECK(s.discErrorCalls == 0u);
    CHECK(s.contextSwitches == 0u);
    CHECK(s.switchesSeenAfterFrame == 0u);
    CHECK(s.videoStandard == -1);
    CHECK_FALSE(s.gameInitDone);
    CHECK(xapi::MountedArchiveNames().empty());
    CHECK(xapi::ContextHistory().empty());
}

// M2 review finding 8: only save directories (12 hex digits) and the five save files count as documented
// misses on partition1; everything else there (TitleMeta.xbx, typos) is UNDOCUMENTED.
TEST_CASE("vfs: documented misses on partition1 are limited to save directories and save files") {
    const char* p1 = vfs::kPartition1;
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\0123456789AB)") != nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\0123456789ab\Futurama.dat)") != nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\udata\46490002\A1B2C3D4E5F6\SaveMeta.xbx)") != nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\A1B2C3D4E5F6\options.dat)") != nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\A1B2C3D4E5F6\Unlock.dat)") != nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\A1B2C3D4E5F6\saveimage.xbx)") != nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\TitleMeta.xbx)") == nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\TitleImage.xbx)") == nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\ZZZZZZZZZZZZ\Futurama.dat)") == nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\0123456789AB\Futurama.txt)") == nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\UDATA\46490002\0123456789A\Futurama.dat)") == nullptr);
    CHECK(vfs::detail::DocumentedMissReason(p1, R"(\TDATA\46490002\0123456789AB)") == nullptr);
    CHECK(vfs::detail::DocumentedMissReason(vfs::kCdRom0, R"(\FutPAL05.img)") != nullptr);
}
