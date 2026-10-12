// Unit tests of the M7 host pieces (save games; analysis/loader/m7_saves.md, docs/m7_report.md):
//  - HostDirDevice directory scans: creation order on writable devices (FATX order of the save lists), name
//  order
//    on read-only devices, snapshot semantics while entries are deleted (XAPI recursive delete),
//  - [saves] free_space_limit (CreateHostDirDevice freeSpaceLimit),
//  - the save container signature computed like XAPI (XCalculateSignatureBegin/Update/End over the kernel's
//    XcSHA exports) against a known value that tools/verify_save.py computes with Python's hmac,
//  - the input script command `game level_end <NextLevel>`.
#include "doctest.h"

#include "input/input_script.h"
#include "kernel/kernel_exports.h"
#include "kernel/xc_crypto.h"
#include "vfs/vfs.h"
#include "vfs/vfs_internal.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path SaveTestDir(const char* name) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wchar_t env[1024];
    const DWORD n = GetEnvironmentVariableW(L"FUT_TEST_TMP", env, static_cast<DWORD>(std::size(env)));
    const fs::path base =
        (n > 0 && n < std::size(env)) ? fs::path(env) : fs::path(buf).parent_path() / "fut_tests_tmp";
    const fs::path dir = (base / name).make_preferred();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

// Creates a file or directory with an explicit creation time (100 ns units since 1601).
void CreateWithTime(const fs::path& p, bool directory, std::int64_t creationTime) {
    if (directory) {
        fs::create_directory(p);
    } else {
        std::ofstream(p, std::ios::binary) << "x";
    }
    const HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(creationTime);
    ft.dwHighDateTime = static_cast<DWORD>(creationTime >> 32);
    REQUIRE(SetFileTime(h, &ft, nullptr, nullptr));
    CloseHandle(h);
}

std::vector<std::string> Scan(vfs::Device& device, std::string_view dir, std::string_view mask = {}) {
    std::unique_ptr<vfs::File> file;
    std::uint32_t action = 0;
    vfs::OpenRequest req;
    req.desiredAccess = 0x100001;
    req.createOptions = vfs::detail::kOptDirectoryFile;
    REQUIRE(device.Open(dir, req, file, action) == STATUS_SUCCESS);
    std::vector<std::string> names;
    vfs::DirEntry e;
    bool restart = true;
    while (file->NextDirEntry(mask, restart, e) == STATUS_SUCCESS) {
        names.push_back(e.name);
        restart = false;
    }
    return names;
}

constexpr std::int64_t kT0 = 133000000000000000LL;  // some time in 2022
constexpr std::int64_t kSecond = 10000000;

}  // namespace

TEST_CASE("vfs (M7): writable host directories list entries in creation order like FATX") {
    const fs::path dir = SaveTestDir("m7_order");
    fs::create_directories(dir / "UDATA");
    // hash-like names created in a non-alphabetical order (the save menus list XFindFirst/NextSaveGame in
    // this order)
    CreateWithTime(dir / "UDATA" / "ED2E0E8B528A", true, kT0 + 1 * kSecond);
    CreateWithTime(dir / "UDATA" / "95EEB682BD97", true, kT0 + 2 * kSecond);
    CreateWithTime(dir / "UDATA" / "TitleMeta.xbx", false, kT0 + 0 * kSecond);
    CreateWithTime(dir / "UDATA" / "B4B8BDB51434", true, kT0 + 3 * kSecond);
    CreateWithTime(dir / "UDATA" / "0A0A0A0A0A0A", true, kT0 + 3 * kSecond);  // same time: name order

    auto writable = vfs::CreateHostDirDevice(dir, false);
    REQUIRE(writable);
    CHECK(Scan(*writable, "\\UDATA") == std::vector<std::string>{"TitleMeta.xbx", "ED2E0E8B528A",
                                                                 "95EEB682BD97", "0A0A0A0A0A0A",
                                                                 "B4B8BDB51434"});
    // the mask still applies (FindFirstFileA "*.xbx")
    CHECK(Scan(*writable, "\\UDATA", "*.xbx") == std::vector<std::string>{"TitleMeta.xbx"});

    // read-only devices (an extracted disc, XDVDFS) keep the host's name order
    auto readOnly = vfs::CreateHostDirDevice(dir, true);
    REQUIRE(readOnly);
    CHECK(Scan(*readOnly, "\\UDATA") == std::vector<std::string>{"0A0A0A0A0A0A", "95EEB682BD97",
                                                                 "B4B8BDB51434", "ED2E0E8B528A",
                                                                 "TitleMeta.xbx"});
}

TEST_CASE("vfs (M7): a directory scan is a snapshot - deleting returned entries does not end it early") {
    const fs::path dir = SaveTestDir("m7_snapshot");
    fs::create_directories(dir / "save");
    for (int i = 0; i < 5; ++i) {
        CreateWithTime(dir / "save" / ("f" + std::to_string(i)), false, kT0 + i * kSecond);
    }
    auto device = vfs::CreateHostDirDevice(dir, false);
    REQUIRE(device);
    std::unique_ptr<vfs::File> d;
    std::uint32_t action = 0;
    vfs::OpenRequest req;
    req.desiredAccess = 0x100001;
    req.createOptions = vfs::detail::kOptDirectoryFile;
    REQUIRE(device->Open("\\save", req, d, action) == STATUS_SUCCESS);
    std::vector<std::string> seen;
    vfs::DirEntry e;
    for (bool restart = true; d->NextDirEntry({}, restart, e) == STATUS_SUCCESS; restart = false) {
        seen.push_back(e.name);
        std::error_code ec;
        fs::remove(dir / "save" / e.name, ec);  // like FUN_000F8709: delete each entry as it is returned
        CHECK_FALSE(ec);
    }
    CHECK(seen == std::vector<std::string>{"f0", "f1", "f2", "f3", "f4"});
    CHECK(fs::is_empty(dir / "save"));
    // RestartScan re-reads the (now empty) directory
    CHECK(d->NextDirEntry({}, true, e) == STATUS_NO_MORE_FILES);
}

TEST_CASE("vfs (M7): [saves] free_space_limit caps the free space of partition1") {
    const fs::path dir = SaveTestDir("m7_free");
    auto unlimited = vfs::CreateHostDirDevice(dir, false);
    auto limited = vfs::CreateHostDirDevice(dir, false, 100000);
    REQUIRE(unlimited);
    REQUIRE(limited);
    vfs::VolumeSizeInfo a, b;
    REQUIRE(unlimited->QueryVolumeSize(a) == STATUS_SUCCESS);
    REQUIRE(limited->QueryVolumeSize(b) == STATUS_SUCCESS);
    CHECK(b.sectorsPerAllocationUnit * b.bytesPerSector == 0x4000);
    CHECK(b.availableAllocationUnits == 100000 / 0x4000);  // 6 clusters of 16 KiB
    CHECK(b.totalAllocationUnits == a.totalAllocationUnits);
    CHECK(a.availableAllocationUnits * 0x4000 <= 0xFFFF0000ull);  // below 4 GiB (files.md 4.4)
}

TEST_CASE("save container (M7): XAPI's XCalculateSignature over XcSHA matches tools/verify_save.py") {
    // Key of [saves] key_mode = fixed: SHA-1("FuturamaPC/XboxSignatureKey/fixed/v1")[0..15]
    // (data_exports.cpp).
    std::uint8_t digest[20];
    const char* label = "FuturamaPC/XboxSignatureKey/fixed/v1";
    krnl::xc::Sha1(label, std::strlen(label), digest);
    std::uint8_t key[16];
    std::memcpy(key, digest, 16);
    static constexpr std::uint8_t kFixedKey[16] = {0x2E, 0x0F, 0xF6, 0x2F, 0xBB, 0xA6, 0xAD, 0x3D,
                                                   0x6B, 0xDA, 0x67, 0x4C, 0x37, 0xDB, 0x8C, 0x66};
    CHECK(std::memcmp(key, kFixedKey, 16) == 0);

    // Container like FUN_000E1390 + FUN_000E03B0: u32 length, payload, zeros, signature at 0xFEC.
    std::vector<std::uint8_t> c(0x1000, 0);
    const char payload[] = "Level=Level1-2\nLives=5\n";  // + the NUL the game counts
    const std::uint32_t len = sizeof(payload);
    std::memcpy(c.data(), &len, 4);
    std::memcpy(c.data() + 4, payload, len);

    // XCalculateSignatureBeginEx -> FUN_000F6F26 (key ^ 0x36 padded to 64, XcSHAInit/Update), Update
    // (XcSHAUpdate), End -> FUN_000F6F88 (XcSHAFinal, XcSHAInit, update with key ^ 0x5C + inner digest as one
    // 0x54-byte block).
    alignas(4) unsigned char ctx[0x74] = {};
    std::array<std::uint8_t, 0x54> outer{};
    std::uint8_t ipad[64] = {};
    std::memcpy(ipad, key, 16);
    std::memcpy(outer.data(), key, 16);
    for (int i = 0; i < 64; ++i) {
        ipad[i] ^= 0x36;
        outer[i] ^= 0x5C;
    }
    krnl::XcSHAInit(ctx);
    krnl::XcSHAUpdate(ctx, ipad, 64);
    krnl::XcSHAUpdate(ctx, c.data(), 0xFEC);
    krnl::XcSHAFinal(ctx, outer.data() + 64);
    krnl::XcSHAInit(ctx);
    krnl::XcSHAUpdate(ctx, outer.data(), 0x54);
    std::uint8_t sig[20];
    krnl::XcSHAFinal(ctx, sig);

    // python: verify_save.make_container(b"Level=Level1-2\nLives=5\n\0", verify_save.fixed_key())[0xFEC:]
    static constexpr std::uint8_t kExpected[20] = {0xC8, 0xE0, 0x77, 0x17, 0x43, 0x12, 0xAC,
                                                   0x26, 0xB6, 0x05, 0xFC, 0xB1, 0xB6, 0x18,
                                                   0x64, 0xD3, 0xB5, 0x3B, 0x92, 0x83};
    CHECK(std::memcmp(sig, kExpected, 20) == 0);
    std::uint8_t hmac[20];
    krnl::xc::HmacSha1(key, 16, c.data(), 0xFEC, hmac);
    CHECK(std::memcmp(sig, hmac, 20) == 0);
}

namespace {

class GameActionEnv final : public inp::script::Environment {
public:
    bool Test(const inp::script::Condition&) noexcept override { return false; }
    std::uint64_t LogMatches(int) noexcept override { return 0; }
    std::uint64_t Frames() noexcept override { return frames; }
    void Message(bool error, const std::string& line) noexcept override {
        if (error) {
            errors.push_back(line);
        }
    }
    void Screenshot(const std::string&) noexcept override {}
    void Quit(int code, const std::string&) noexcept override { quitCode = code; }
    bool GameAction(const std::string& action) noexcept override {
        actions.push_back(action);
        return accept;
    }
    std::vector<std::string> actions;
    std::vector<std::string> errors;
    std::uint64_t frames = 0;
    int quitCode = -1;
    bool accept = true;
};

}  // namespace

TEST_CASE("input_script (M7): game level_end <NextLevel>") {
    using namespace inp::script;
    Program p;
    std::string error;
    REQUIRE(Parse("game level_end Level1-2\ngame LEVEL_END Level3-6\n", 60, p, &error));
    REQUIRE(p.code.size() == 2);
    CHECK(p.code[0].op == Op::Game);
    CHECK(p.code[0].text == "level_end Level1-2");
    CHECK(p.code[1].text == "level_end Level3-6");  // the level name keeps its case

    for (const char* bad : {"game\n", "game level_end\n", "game save_menu\n", "game level_end Level1-2 x\n",
                            "game level_end LevelName1\n"}) {
        Program q;
        std::string e;
        CHECK_FALSE(Parse(bad, 60, q, &e));
        CHECK(e.find("line 1") != std::string::npos);
    }

    GameActionEnv env;
    {
        Player player(p, env);
        player.Step();
        CHECK(env.actions == std::vector<std::string>{"level_end Level1-2", "level_end Level3-6"});
        CHECK(player.State() == PlayerState::Finished);
    }
    // the host refuses (no level running): the script fails
    GameActionEnv refusing;
    refusing.accept = false;
    Program one;
    REQUIRE(Parse("game level_end Level1-2\nquit 0\n", 60, one, &error));
    Player player(one, refusing);
    player.Step();
    CHECK(player.State() == PlayerState::Failing);
    CHECK_FALSE(refusing.errors.empty());
}
