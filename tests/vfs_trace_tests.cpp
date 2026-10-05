// Unit tests of core/trace (io agent, M2; design 5.4): kernel trace ring buffer fed through the log tap,
// crash-report dump, raw VFS trace lines. The end-to-end VFS trace of guest opens is in vfs_tests.cpp.
#include "doctest.h"

#include "core/log.h"
#include "core/trace.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path TraceTempDir(const char* name) {
    wchar_t buf[1024];
    fs::path base;
    const DWORD n = GetEnvironmentVariableW(L"FUT_TEST_TMP", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        base = buf;
    } else {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        base = fs::path(exe).parent_path() / "fut_tests_tmp";
    }
    const fs::path dir = (base / name).make_preferred();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("trace: --trace= options") {
    core::Config cfg;
    cfg.userdata = "C:\\ud";
    cfg.trace = {"krnl"};
    core::trace::Options o = core::trace::OptionsFromConfig(cfg);
    CHECK(o.krnl);
    CHECK_FALSE(o.vfs);
    CHECK(o.directory == cfg.LogDirectory());
    cfg.trace = {"vfs", "gfx"};
    o = core::trace::OptionsFromConfig(cfg);
    CHECK_FALSE(o.krnl);
    CHECK(o.vfs);
    cfg.trace = {"all"};
    o = core::trace::OptionsFromConfig(cfg);
    CHECK((o.krnl && o.vfs));
    cfg.trace = {};
    o = core::trace::OptionsFromConfig(cfg);
    CHECK_FALSE((o.krnl || o.vfs));
}

TEST_CASE("trace: kernel trace ring keeps the last KRNL lines and is fed by the log tap") {
    core::trace::Options o;
    o.krnl = true;
    REQUIRE(core::trace::Configure(o));
    REQUIRE(core::trace::KrnlEnabled());
    CHECK(core::trace::KrnlRecordCount() == 0);

    // raw records: wrap around, oldest first, newline stripped, long lines truncated
    const std::size_t total = core::trace::kKrnlRingEntries + 76;
    for (std::size_t i = 0; i < total; ++i) {
        const std::string line = "line " + std::to_string(i) + "\n";
        core::trace::RecordKrnl(line.c_str(), line.size());
    }
    CHECK(core::trace::KrnlRecordCount() == total);
    std::vector<std::string> ring = core::trace::KrnlRingSnapshot();
    REQUIRE(ring.size() == core::trace::kKrnlRingEntries);
    CHECK(ring.front() == "line 76");
    CHECK(ring.back() == "line " + std::to_string(total - 1));
    const std::string longLine(1000, 'x');
    core::trace::RecordKrnl(longLine.c_str(), longLine.size());
    ring = core::trace::KrnlRingSnapshot();
    CHECK(ring.back().size() == core::trace::kKrnlRingLineBytes - 1);

    // log tap: KRNL lines that pass the level check land in the ring, other channels do not
    const core::Level saved = core::LogGetLevel(core::Channel::Krnl);
    core::LogSetLevel(core::Channel::Krnl, core::Level::Trace);
    FUT_LOGT(Krnl, "NtReadFile(%p, ...) -> %s", reinterpret_cast<void*>(0x1004), "STATUS_PENDING");
    FUT_LOGE(Vfs, "a VFS line that must not enter the kernel ring");
    core::LogSetLevel(core::Channel::Krnl, saved);
    ring = core::trace::KrnlRingSnapshot();
    CHECK(ring.back().find("KRNL TRACE NtReadFile(00001004, ...) -> STATUS_PENDING") != std::string::npos);
    for (const std::string& l : ring) {
        CHECK(l.find("must not enter") == std::string::npos);
    }
    core::trace::DumpKrnlRing();  // the crash reporter; must not deadlock on the tap (logs on CORE)

    // disabled: nothing is recorded, the tap is removed
    REQUIRE(core::trace::Configure(core::trace::Options{}));
    CHECK_FALSE(core::trace::KrnlEnabled());
    core::trace::RecordKrnl("ignored", 7);
    FUT_LOGE(Krnl, "ignored too");
    CHECK(core::trace::KrnlRecordCount() == 0);
    CHECK(core::trace::KrnlRingSnapshot().empty());
}

TEST_CASE("trace: VFS trace file is truncated per run and written line by line") {
    const fs::path dir = TraceTempDir("trace_vfs");
    core::trace::Options o;
    o.vfs = true;
    o.directory = dir / "logs";  // created if missing
    REQUIRE(core::trace::Configure(o));
    REQUIRE(core::trace::VfsEnabled());
    CHECK(core::trace::VfsTracePath() == dir / "logs" / "vfs_trace.log");
    core::trace::VfsLine("%s \"%s\" -> %s : hit", "NtCreateFile", "\\??\\D:\\SysPAL.img",
                         "\\Device\\CdRom0\\SysPAL.img");
    core::trace::VfsLine("second %d", 2);
    CHECK(core::trace::VfsLineCount() == 2);
    {
        // readable while the loader still writes it (FILE_SHARE_READ), complete without a flush
        std::ifstream in(dir / "logs" / "vfs_trace.log");
        std::vector<std::string> lines;
        std::size_t headerLines = 0;
        for (std::string l; std::getline(in, l);) {
            if (l.rfind("# ", 0) == 0) {
                ++headerLines;
                // the header never contains the marker the M2 check greps for
                CHECK(l.find("UNDOCUMENTED") == std::string::npos);
            } else {
                lines.push_back(l);
            }
        }
        CHECK(headerLines >= 2);
        REQUIRE(lines.size() == 2);
        CHECK(lines[0].rfind("[", 0) == 0);
        CHECK(lines[0].find("] ") != std::string::npos);
        CHECK(lines[0].find("NtCreateFile \"\\??\\D:\\SysPAL.img\" -> \\Device\\CdRom0\\SysPAL.img : hit") !=
              std::string::npos);
        CHECK(lines[1].find("second 2") != std::string::npos);
    }
    // a second Configure starts a new file
    REQUIRE(core::trace::Configure(o));
    CHECK(core::trace::VfsLineCount() == 0);
    REQUIRE(core::trace::Configure(core::trace::Options{}));
    CHECK_FALSE(core::trace::VfsEnabled());
    CHECK(core::trace::VfsTracePath().empty());
    core::trace::VfsLine("not written");
    CHECK(core::trace::VfsLineCount() == 0);
    std::ifstream in(dir / "logs" / "vfs_trace.log");
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(all.find("not written") == std::string::npos);
    CHECK(all.find("second 2") == std::string::npos);
}
