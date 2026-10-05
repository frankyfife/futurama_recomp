// Unit tests of the VFS and the file-I/O kernel exports (io agent; design 5.1 tests "xdvdfs",
// "path_resolve"). Temporary files go to %FUT_TEST_TMP% or <exe dir>\fut_tests_tmp (never
// %LOCALAPPDATA%). Tests that need the user's disc image / XBE read %FUT_TEST_ISO% / %FUT_TEST_XBE%
// (defaults below) and are skipped with a message if the file does not exist. The image is only read.
#include "doctest.h"

#include "core/config.h"
#include "core/trace.h"
#include "kernel/io_apc.h"
#include "kernel/io_exports.h"
#include "kernel/io_types.h"
#include "kernel/ke_wait.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "kernel/ob_handles.h"
#include "vfs/vfs.h"
#include "vfs/vfs_init.h"
#include "vfs/vfs_internal.h"

#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kDefaultIso[] =
    L"Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso";
constexpr wchar_t kDefaultXbe[] = L"D:\\FuturamaDecomp\\orig\\retail_eu\\default.xbe";

fs::path EnvPath(const wchar_t* name, const wchar_t* fallback) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        return fs::path(buf);
    }
    return fallback != nullptr ? fs::path(fallback) : fs::path();
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// Fresh, empty temporary directory for one test case.
fs::path TempDir(const char* name) {
    fs::path base = EnvPath(L"FUT_TEST_TMP", nullptr);
    if (base.empty()) {
        base = ExeDir() / "fut_tests_tmp";
    }
    // make_preferred: FUT_TEST_TMP may use '/', but the "\\?\" paths built by the tests need '\'.
    const fs::path dir = (base / name).make_preferred();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

fs::path IsoPath() {
    const fs::path p = EnvPath(L"FUT_TEST_ISO", kDefaultIso);
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? p : fs::path();
}

std::vector<std::uint8_t> ReadWholeFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Guest-style ANSI_STRING + OBJECT_ATTRIBUTES built on the host (host memory = guest memory).
struct ObjAttr {
    std::string text;
    xbox::ANSI_STRING str{};
    xbox::OBJECT_ATTRIBUTES oa{};
    ObjAttr(std::string name, HANDLE root) : text(std::move(name)) {
        str.Length = static_cast<std::uint16_t>(text.size());
        str.MaximumLength = static_cast<std::uint16_t>(text.size() + 1);
        str.Buffer = text.data();
        oa.RootDirectory = root;
        oa.ObjectName = &str;
        oa.Attributes = xbox::OBJ_CASE_INSENSITIVE;
    }
    ObjAttr(const ObjAttr&) = delete;
    ObjAttr& operator=(const ObjAttr&) = delete;
};

const HANDLE kDos = xbox::ObDosDevicesDirectory;

// ---- synthetic XDVDFS image ----------------------------------------------------------------------
void PutU16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
void PutU32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}
std::size_t PutEntry(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t left, std::uint16_t right,
                     std::uint32_t sector, std::uint32_t size, std::uint8_t attr, const char* name) {
    const std::size_t n = std::strlen(name);
    PutU16(b, off, left);
    PutU16(b, off + 2, right);
    PutU32(b, off + 4, sector);
    PutU32(b, off + 8, size);
    b[off + 12] = attr;
    b[off + 13] = static_cast<std::uint8_t>(n);
    std::memcpy(b.data() + off + 14, name, n);
    return (off + 14 + n + 3) & ~std::size_t{3};
}

constexpr std::size_t kSector = 2048;
constexpr std::uint32_t kAFileSize = 5000;
constexpr std::int64_t kSyntheticTime = 0x01C3420012345678LL;

// sectors: 32 VD, 33 root {Dir, a.txt, zeta.bin}, 34 Dir {Sub File.dat}, 35-37 a.txt, 38 sub file, 39
// zeta.bin
std::vector<std::uint8_t> BuildSyntheticImage() {
    std::vector<std::uint8_t> img(40 * kSector, 0);
    std::uint8_t* vd = img.data() + 32 * kSector;
    std::memcpy(vd, "MICROSOFT*XBOX*MEDIA", 20);
    PutU32(img, 32 * kSector + 20, 33);
    PutU32(img, 32 * kSector + 24, static_cast<std::uint32_t>(kSector));
    std::memcpy(vd + 28, &kSyntheticTime, 8);
    std::memcpy(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);

    std::fill(img.begin() + 33 * kSector, img.begin() + 35 * kSector,
              std::uint8_t{0xFF});  // directory padding
    // root: "Dir" at 0 (left -> a.txt at dword 5, right -> zeta.bin at dword 10)
    std::size_t root = 33 * kSector;
    std::size_t next = PutEntry(img, root, 5, 10, 34, static_cast<std::uint32_t>(kSector), 0x10, "Dir");
    CHECK(next - root == 20);
    next = PutEntry(img, root + 20, 0, 0, 35, kAFileSize, 0x20, "a.txt");
    CHECK(next - root == 40);
    PutEntry(img, root + 40, 0, 0, 39, 10, 0x20, "zeta.bin");
    PutEntry(img, 34 * kSector, 0, 0, 38, 3, 0x20, "Sub File.dat");

    for (std::uint32_t i = 0; i < kAFileSize; ++i) {
        img[35 * kSector + i] = static_cast<std::uint8_t>(i % 251);
    }
    std::memcpy(img.data() + 38 * kSector, "xyz", 3);
    std::memcpy(img.data() + 39 * kSector, "0123456789", 10);
    return img;
}

void WriteFileBytes(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// =================================================================================================
TEST_CASE("vfs path helpers") {
    CHECK(vfs::NormalizePath("/a//b\\\\c/") == "\\a\\b\\c\\");
    CHECK(vfs::NormalizePath("D:/Video/x.xmv") == "D:\\Video\\x.xmv");
    CHECK(vfs::IEquals("sysPAL.img", "SysPAL.IMG"));
    CHECK_FALSE(vfs::IEquals("sysPAL.img", "sysPAL.im"));
    CHECK(vfs::IStartsWith("\\Device\\Harddisk0\\Partition1\\x", "\\device\\harddisk0\\partition1"));
    CHECK(vfs::detail::SplitPath("\\a\\\\b/c\\") == std::vector<std::string>{"a", "b", "c"});
    CHECK(vfs::detail::SplitPath("\\").empty());
    CHECK(vfs::detail::JoinPath({}) == "\\");
    CHECK(vfs::detail::JoinPath({"a", "b"}) == "\\a\\b");

    CHECK(vfs::MatchFileMask("anything", ""));
    CHECK(vfs::MatchFileMask("SaveMeta.xbx", "*"));
    CHECK(vfs::MatchFileMask("SaveMeta.xbx", "*.XBX"));
    CHECK(vfs::MatchFileMask("SaveMeta.xbx", "save????.xbx"));
    CHECK_FALSE(vfs::MatchFileMask("SaveMeta.xbx", "*.dat"));
    CHECK(vfs::MatchFileMask("a.b.c", "*.c"));
    CHECK(vfs::MatchFileMask("abc", "a*b*c"));
    CHECK_FALSE(vfs::MatchFileMask("abcd", "a*b*c"));
    CHECK(vfs::MatchFileMask("FutPAL03.img", "<.img"));  // DOS wildcard form

    std::string guestName;
    CHECK(vfs::detail::WideToGuest(L"caf\u00E9", guestName));
    CHECK(guestName == "caf\xE9");
    CHECK(vfs::detail::GuestToWide("caf\xE9") == L"caf\u00E9");
    CHECK_FALSE(vfs::detail::WideToGuest(L"\u4E2D", guestName));
}

// =================================================================================================
TEST_CASE("vfs namespace: devices, symbolic links, resolution") {
    vfs::ResetNamespace();
    const fs::path dir = TempDir("namespace");
    auto hdd = vfs::CreateHostDirDevice(dir / "p1", false);
    auto hdd10 = vfs::CreateHostDirDevice(dir / "p10", false);
    REQUIRE(hdd);
    REQUIRE(hdd10);
    REQUIRE(vfs::AddDevice(vfs::kPartition1, hdd));
    REQUIRE(vfs::AddDevice("\\Device\\Harddisk0\\partition10", hdd10));
    CHECK_FALSE(vfs::AddDevice("\\DEVICE\\HARDDISK0\\PARTITION1", hdd));  // duplicate (case-insensitive)
    CHECK(vfs::FindDevice("\\device\\harddisk0\\Partition1") == hdd);

    CHECK(vfs::CreateSymbolicLink("\\??\\U:", "\\Device\\Harddisk0\\partition1\\UDATA\\46490002\\") ==
          STATUS_SUCCESS);
    CHECK(vfs::CreateSymbolicLink("\\??\\u:", "\\Device\\CdRom0") == STATUS_OBJECT_NAME_COLLISION);
    CHECK(vfs::CreateSymbolicLink("\\??\\X:", "\\??\\U:\\sub") == STATUS_SUCCESS);  // link to a link
    std::string target;
    REQUIRE(vfs::QuerySymbolicLink("\\??\\U:", target));
    CHECK(target == "\\Device\\Harddisk0\\partition1\\UDATA\\46490002");

    vfs::ResolvedPath r;
    REQUIRE(vfs::Resolve("\\??\\u:\\TitleMeta.xbx", r) == STATUS_SUCCESS);
    CHECK(r.device == hdd);
    CHECK(r.devicePath == "\\Device\\Harddisk0\\partition1");
    CHECK(r.relativePath == "\\UDATA\\46490002\\TitleMeta.xbx");

    REQUIRE(vfs::Resolve("\\??\\U:\\", r) == STATUS_SUCCESS);
    CHECK(r.relativePath == "\\UDATA\\46490002");
    REQUIRE(vfs::Resolve("\\??\\X:/a/b", r) == STATUS_SUCCESS);
    CHECK(r.relativePath == "\\UDATA\\46490002\\sub\\a\\b");
    REQUIRE(vfs::Resolve("\\Device\\Harddisk0\\partition1\\", r) == STATUS_SUCCESS);
    CHECK(r.relativePath == "\\");
    // longest prefix at a component boundary: partition10 is not partition1 + "0"
    REQUIRE(vfs::Resolve("\\Device\\Harddisk0\\partition10\\x", r) == STATUS_SUCCESS);
    CHECK(r.device == hdd10);
    CHECK(r.relativePath == "\\x");

    CHECK(vfs::Resolve("\\??\\Q:", r) == STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(vfs::Resolve("\\??\\Q:\\file", r) == STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(vfs::Resolve("\\??\\Ux:\\file", r) == STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(vfs::Resolve("relative", r) == STATUS_OBJECT_NAME_INVALID);

    // loops are detected
    CHECK(vfs::CreateSymbolicLink("\\??\\L1:", "\\??\\L2:") == STATUS_SUCCESS);
    CHECK(vfs::CreateSymbolicLink("\\??\\L2:", "\\??\\L1:") == STATUS_SUCCESS);
    CHECK(vfs::Resolve("\\??\\L1:\\x", r) == STATUS_OBJECT_NAME_INVALID);

    CHECK(vfs::DeleteSymbolicLink("\\??\\U:") == STATUS_SUCCESS);
    CHECK(vfs::DeleteSymbolicLink("\\??\\U:") == STATUS_OBJECT_NAME_NOT_FOUND);
    vfs::ResetNamespace();
}

// =================================================================================================
TEST_CASE("vfs: host directories beyond MAX_PATH (review finding 11)") {
    const fs::path dir = TempDir("longpath");
    fs::path deep = dir;
    for (int i = 0; i < 8; ++i) {
        deep /= std::wstring(40, static_cast<wchar_t>(L'a' + i));  // > 330 characters in total
    }
    REQUIRE(deep.wstring().size() > MAX_PATH);
    std::string error;
    REQUIRE_MESSAGE(vfs::CreateHostDirectories(deep / "hdd" / "partition1" / "TDATA", error), error);
    CHECK(vfs::CreateHostDirectories(deep / "hdd" / "partition1" / "TDATA", error));  // already exists
    auto hdd = vfs::CreateHostDirDevice(deep / "hdd" / "partition1", false);
    REQUIRE(hdd);
    const std::wstring native = L"\\\\?\\" + (deep / "hdd" / "partition1" / "TDATA").wstring();
    const DWORD attrs = GetFileAttributesW(native.c_str());
    CHECK(attrs != INVALID_FILE_ATTRIBUTES);
    CHECK((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    // a file in the way is reported, not ignored
    const std::wstring file = L"\\\\?\\" + (deep / "file").wstring();
    const HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    CloseHandle(h);
    CHECK_FALSE(vfs::CreateHostDirectories(deep / "file" / "sub", error));
    CHECK_FALSE(error.empty());
    std::error_code ec;
    fs::remove_all(fs::path(L"\\\\?\\" + dir.wstring()), ec);
}

// =================================================================================================
TEST_CASE("xdvdfs: synthetic image (offset 0)") {
    const fs::path dir = TempDir("xdvdfs_synthetic");
    const fs::path imagePath = dir / "synthetic.iso";
    WriteFileBytes(imagePath, BuildSyntheticImage());

    vfs::XdvdfsImage img;
    std::string error;
    REQUIRE_MESSAGE(img.Open(imagePath, error), error);
    CHECK(img.PartitionOffset() == 0);
    CHECK(img.EntryCount() == 4);
    CHECK(img.VolumeTimestamp() == kSyntheticTime);
    CHECK(img.PartitionSize() == 40 * kSector);

    std::vector<vfs::XdvdfsImage::Entry> list;
    REQUIRE(img.ListDirectory("\\", list));
    REQUIRE(list.size() == 3);
    CHECK(list[0].name == "a.txt");  // in-order = sorted
    CHECK(list[1].name == "Dir");
    CHECK(list[1].IsDirectory());
    CHECK(list[2].name == "zeta.bin");

    vfs::XdvdfsImage::Entry e;
    REQUIRE(img.Lookup("DIR/sub file.DAT", e));
    CHECK(e.size == 3);
    CHECK_FALSE(img.Lookup("\\Dir\\missing", e));
    CHECK_FALSE(img.Lookup("a.txt\\x", e));
    REQUIRE(img.Lookup("\\", e));
    CHECK(e.IsDirectory());

    REQUIRE(img.Lookup("A.TXT", e));
    std::vector<std::uint8_t> buf(4096, 0xEE);
    std::uint32_t got = 0;
    // a 2048-aligned read that runs past the end of the file is clipped (files.md 2.4)
    REQUIRE(img.Read(e, 4096, buf.data(), 2048, got));
    CHECK(got == kAFileSize - 4096);
    CHECK(buf[0] == static_cast<std::uint8_t>(4096 % 251));
    CHECK(buf[got] == 0xEE);  // nothing written beyond the clipped length
    REQUIRE(img.Read(e, kAFileSize, buf.data(), 16, got));
    CHECK(got == 0);

    std::vector<std::uint8_t> whole;
    REQUIRE(vfs::ReadIsoFile(imagePath, "zeta.bin", whole, error));
    CHECK(std::string(whole.begin(), whole.end()) == "0123456789");
    CHECK_FALSE(vfs::ReadIsoFile(imagePath, "nope.bin", whole, error));
    CHECK_FALSE(vfs::ReadIsoFile(imagePath, "Dir", whole, error));
}

TEST_CASE("xdvdfs: truncated image is zero-padded, garbage is rejected") {
    const fs::path dir = TempDir("xdvdfs_truncated");
    std::vector<std::uint8_t> bytes = BuildSyntheticImage();
    bytes.resize(39 * kSector + 4);  // zeta.bin (10 bytes) only partially present
    WriteFileBytes(dir / "trunc.iso", bytes);
    vfs::XdvdfsImage img;
    std::string error;
    REQUIRE_MESSAGE(img.Open(dir / "trunc.iso", error), error);
    vfs::XdvdfsImage::Entry e;
    REQUIRE(img.Lookup("zeta.bin", e));
    char buf[16] = {};
    std::uint32_t got = 0;
    REQUIRE(img.Read(e, 0, buf, 16, got));
    CHECK(got == 10);
    CHECK(std::string(buf, 4) == "0123");
    CHECK(buf[4] == 0);

    WriteFileBytes(dir / "garbage.iso", std::vector<std::uint8_t>(64 * kSector, 0x5A));
    vfs::XdvdfsImage bad;
    CHECK_FALSE(bad.Open(dir / "garbage.iso", error));
    CHECK_FALSE(bad.Open(dir / "does_not_exist.iso", error));
}

TEST_CASE("xdvdfs: redump layout (partition at 0x18300000, sparse file)") {
    const fs::path dir = TempDir("xdvdfs_redump");
    const fs::path p = dir / "redump.iso";
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    DWORD ret = 0;
    const bool sparse = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr) != FALSE;
    if (!sparse) {
        CloseHandle(h);
        MESSAGE("skipped: the temporary directory does not support sparse files");
        return;
    }
    const std::vector<std::uint8_t> image = BuildSyntheticImage();
    LARGE_INTEGER pos;
    pos.QuadPart = 0x18300000LL;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    DWORD written = 0;
    REQUIRE(WriteFile(h, image.data(), static_cast<DWORD>(image.size()), &written, nullptr));
    CloseHandle(h);

    vfs::XdvdfsImage img;
    std::string error;
    REQUIRE_MESSAGE(img.Open(p, error), error);
    CHECK(img.PartitionOffset() == 0x18300000ull);
    vfs::XdvdfsImage::Entry e;
    REQUIRE(img.Lookup("Dir\\Sub File.dat", e));
    char buf[8] = {};
    std::uint32_t got = 0;
    REQUIRE(img.Read(e, 0, buf, 8, got));
    CHECK(got == 3);
    CHECK(std::string(buf, 3) == "xyz");
    std::error_code ec;
    fs::remove(p, ec);
}

// =================================================================================================
TEST_CASE("xdvdfs: retail EU disc image (read-only)") {
    const fs::path iso = IsoPath();
    if (iso.empty()) {
        MESSAGE("skipped: disc image not found (set FUT_TEST_ISO)");
        return;
    }
    vfs::XdvdfsImage img;
    std::string error;
    REQUIRE_MESSAGE(img.Open(iso, error), error);
    CHECK(img.PartitionOffset() == 0);  // trimmed XISO (files.md header)
    CHECK(img.EntryCount() == 59);      // files.md 5.2

    std::vector<vfs::XdvdfsImage::Entry> root;
    REQUIRE(img.ListDirectory("\\", root));
    CHECK(root.size() == 9);  // 7 files + video + SaveGameIcons

    vfs::XdvdfsImage::Entry e;
    REQUIRE(img.Lookup("default.xbe", e));
    CHECK(e.size == 2973696u);
    REQUIRE(img.Lookup("sysPAL.img", e));  // disc name is SysPAL.img (files.md 1.2)
    CHECK(e.size == 7742704u);
    CHECK(e.name == "SysPAL.img");
    REQUIRE(img.Lookup("Video/Intro-PAL.xmv", e));  // '/' and case
    CHECK(e.size == 2519040u);
    REQUIRE(img.Lookup("\\SAVEGAMEICONS\\saveimage.xbx", e));
    CHECK(e.size == 4096u);
    CHECK_FALSE(img.Lookup("FutPAL05.img", e));
    CHECK_FALSE(img.Lookup("sysNTSC.img", e));
    CHECK_FALSE(img.Lookup("fonts\\kanjii.fnt", e));
    std::vector<vfs::XdvdfsImage::Entry> video;
    REQUIRE(img.ListDirectory("video", video));
    CHECK(video.size() == 47);

    // SysPAL.img header: u32 header_size = 0xCF7 (files.md 3.4); last sector read runs past EOF.
    REQUIRE(img.Lookup("SysPAL.img", e));
    std::uint32_t headerSize = 0, got = 0;
    REQUIRE(img.Read(e, 0, &headerSize, 4, got));
    CHECK(got == 4);
    CHECK(headerSize == 0xCF7u);
    std::vector<std::uint8_t> chunk(0x20000);
    const std::uint64_t lastAligned = (e.size - 1) & ~std::uint64_t{0x7FF};
    REQUIRE(img.Read(e, lastAligned, chunk.data(), static_cast<std::uint32_t>(chunk.size()), got));
    CHECK(got == e.size - lastAligned);

    // default.xbe from the image == the extracted XBE (design 1.4 step 1)
    std::vector<std::uint8_t> xbe;
    REQUIRE_MESSAGE(vfs::ReadIsoFile(iso, "default.xbe", xbe, error), error);
    REQUIRE(xbe.size() == 2973696u);
    CHECK(std::memcmp(xbe.data(), "XBEH", 4) == 0);
    const fs::path extracted = EnvPath(L"FUT_TEST_XBE", kDefaultXbe);
    std::error_code ec;
    if (fs::is_regular_file(extracted, ec)) {
        CHECK(ReadWholeFile(extracted) == xbe);
    } else {
        MESSAGE("extracted XBE not found - byte comparison skipped");
    }
}

// =================================================================================================
// Kernel layer: replays the XAPI call sequences of XapiInitProcess / XapiMapLetterToDirectory /
// XGetDiskClusterSizeA / CreateFileA / FindFirstFileA / the recursive delete with the Xbox argument lists.
namespace {

struct KernelFixture {
    fs::path userdata;
    std::size_t baseHandles = 0;

    explicit KernelFixture(const char* name) {
        vfs::ResetNamespace();
        userdata = TempDir(name);
        core::Config cfg;
        cfg.userdata = userdata;
        cfg.iso = IsoPath();
        REQUIRE(vfs::StageInitVfs(cfg));
        baseHandles = krnl::HandleTable::Instance().Count();
    }
    ~KernelFixture() { vfs::ResetNamespace(); }
    KernelFixture(const KernelFixture&) = delete;
    KernelFixture& operator=(const KernelFixture&) = delete;

    fs::path Partition1() const { return userdata / "hdd" / "partition1"; }
};

NTSTATUS Create(HANDLE* h, std::uint32_t access, const std::string& name, HANDLE root, std::uint32_t attrs,
                std::uint32_t share, std::uint32_t disposition, std::uint32_t options,
                xbox::IO_STATUS_BLOCK* iosb = nullptr) {
    ObjAttr oa(name, root);
    xbox::IO_STATUS_BLOCK local{};
    return krnl::NtCreateFile(h, access, &oa.oa, iosb != nullptr ? iosb : &local, nullptr, attrs, share,
                              disposition, options);
}

NTSTATUS Open(HANDLE* h, std::uint32_t access, const std::string& name, HANDLE root, std::uint32_t share,
              std::uint32_t options) {
    ObjAttr oa(name, root);
    xbox::IO_STATUS_BLOCK iosb{};
    return krnl::NtOpenFile(h, access, &oa.oa, &iosb, share, options);
}

// XGetDiskClusterSizeA 0xF8081: NtOpenFile(0x100001, share 3, 0x800021) + class 3, product.
std::uint32_t ClusterSize(const std::string& dosPath) {
    HANDLE h = nullptr;
    if (!NT_SUCCESS(Open(&h, 0x100001, dosPath, kDos, 3, 0x800021))) {
        return 0;
    }
    krnl::io::GuestFileFsSizeInformation s{};
    xbox::IO_STATUS_BLOCK iosb{};
    const NTSTATUS st = krnl::NtQueryVolumeInformationFile(h, &iosb, &s, sizeof(s), 3);
    krnl::NtClose(h);
    return NT_SUCCESS(st) ? s.SectorsPerAllocationUnit * s.BytesPerSector : 0;
}

std::uint64_t EndOfFile(HANDLE h) {
    krnl::io::GuestFileNetworkOpenInformation n{};
    xbox::IO_STATUS_BLOCK iosb{};
    REQUIRE(krnl::NtQueryInformationFile(h, &iosb, &n, sizeof(n), 0x22) == STATUS_SUCCESS);
    CHECK(iosb.Information == sizeof(n));
    return static_cast<std::uint64_t>(n.EndOfFile.QuadPart);
}

std::vector<std::string> ListDir(HANDLE dir, const char* mask, NTSTATUS* firstStatus = nullptr) {
    std::vector<std::string> names;
    alignas(8) std::uint8_t buf[0x148];
    xbox::ANSI_STRING m{};
    std::string maskText = mask != nullptr ? mask : "";
    m.Length = static_cast<std::uint16_t>(maskText.size());
    m.MaximumLength = m.Length;
    m.Buffer = maskText.data();
    bool first = true;
    for (int guard = 0; guard < 1000; ++guard) {
        xbox::IO_STATUS_BLOCK iosb{};
        const NTSTATUS s = krnl::NtQueryDirectoryFile(dir, nullptr, nullptr, nullptr, &iosb, buf, sizeof(buf),
                                                      1, first ? &m : nullptr, FALSE);
        if (first && firstStatus != nullptr) {
            *firstStatus = s;
        }
        first = false;
        if (!NT_SUCCESS(s)) {
            break;
        }
        const auto* d = reinterpret_cast<const krnl::io::GuestFileDirectoryInformation*>(buf);
        names.emplace_back(d->FileName, d->FileNameLength);
        CHECK(iosb.Information == 0x40 + d->FileNameLength);
    }
    return names;
}

std::atomic<int> g_apcCalls{0};
void* g_apcContext = nullptr;
xbox::IO_STATUS_BLOCK* g_apcIosb = nullptr;
void __stdcall TestApcRoutine(void* context, xbox::IO_STATUS_BLOCK* iosb, std::uint32_t) {
    ++g_apcCalls;
    g_apcContext = context;
    g_apcIosb = iosb;
}

// LPOVERLAPPED_COMPLETION_ROUTINE as the game's 0x13F1D0 (stdcall, ret 0xC).
std::atomic<int> g_completionCalls{0};
std::uint32_t g_completionError = 0xFFFFFFFF;
std::uint32_t g_completionBytes = 0;
void* g_completionOverlapped = nullptr;
void __stdcall TestCompletion(std::uint32_t error, std::uint32_t bytes, void* overlapped) {
    ++g_completionCalls;
    g_completionError = error;
    g_completionBytes = bytes;
    g_completionOverlapped = overlapped;
}

}  // namespace

TEST_CASE("io: XapiInitProcess drive setup on partition1 (T:, U:, cluster size, TitleMeta)") {
    KernelFixture fx("io_init");

    // FUN_000f8ae2: "\Device\Harddisk0\partition1\" must report a 0x4000 cluster.
    HANDLE h = nullptr;
    REQUIRE(Open(&h, 0x100001, "\\Device\\Harddisk0\\partition1\\", nullptr, 3, 0x800021) == STATUS_SUCCESS);
    krnl::io::GuestFileFsSizeInformation fsSize{};
    xbox::IO_STATUS_BLOCK iosb{};
    REQUIRE(krnl::NtQueryVolumeInformationFile(h, &iosb, &fsSize, sizeof(fsSize), 3) == STATUS_SUCCESS);
    CHECK(iosb.Information == 0x18);
    CHECK(fsSize.SectorsPerAllocationUnit * fsSize.BytesPerSector == 0x4000u);
    CHECK(static_cast<std::uint64_t>(fsSize.AvailableAllocationUnits.QuadPart) * 0x4000u < 0x100000000ull);
    CHECK(fsSize.TotalAllocationUnits.QuadPart >= fsSize.AvailableAllocationUnits.QuadPart);
    CHECK(krnl::NtClose(h) == STATUS_SUCCESS);

    // XapiInitProcess: \??\D: -> \Device\CdRom0 (a second call reports the collision)
    {
        ObjAttr link("\\??\\D:", nullptr), dev("\\Device\\CdRom0", nullptr);
        CHECK(krnl::IoCreateSymbolicLink(&link.str, &dev.str) == STATUS_SUCCESS);
        CHECK(krnl::IoCreateSymbolicLink(&link.str, &dev.str) == STATUS_OBJECT_NAME_COLLISION);
    }

    // XapiMapLetterToDirectory(T:/U:): FILE_OPEN_IF of TDATA, then of the title directory.
    for (const char* letter : {"T", "U"}) {
        const std::string base = std::string("\\Device\\Harddisk0\\partition1\\") + letter + "DATA";
        REQUIRE(Create(&h, 0x100001, base, nullptr, 0x80, 3, 3, 0x4021, &iosb) == STATUS_SUCCESS);
        CHECK(iosb.Information == 1);  // FILE_OPENED (StageInitVfs created it)
        krnl::NtClose(h);
        REQUIRE(Create(&h, 0x120117, base + "\\46490002", nullptr, 0x80, 3, 3, 0x4021, &iosb) ==
                STATUS_SUCCESS);
        CHECK(iosb.Information == 2);  // FILE_CREATED
        krnl::NtClose(h);
        ObjAttr link(std::string("\\??\\") + letter + ":", nullptr), dir(base + "\\46490002", nullptr);
        CHECK(krnl::IoCreateSymbolicLink(&link.str, &dir.str) == STATUS_SUCCESS);
    }
    CHECK(fs::is_directory(fx.Partition1() / "TDATA" / "46490002"));
    CHECK(fs::is_directory(fx.Partition1() / "UDATA" / "46490002"));

    // TitleMeta.xbx: NtCreateFile(GENERIC_WRITE|SYNCHRONIZE, SYSTEM, share 1, FILE_OPEN_IF, 0x22),
    // class 0x22 EndOfFile == 0, NtWriteFile with ByteOffset NULL on the synchronous handle.
    const std::string meta = "\\Device\\Harddisk0\\partition1\\UDATA\\46490002\\TitleMeta.xbx";
    REQUIRE(Create(&h, 0x40100000, meta, nullptr, 4, 1, 3, 0x22, &iosb) == STATUS_SUCCESS);
    CHECK(iosb.Information == 2);
    CHECK(EndOfFile(h) == 0);
    const wchar_t text[] = L"\xFEFFTitleName=Futurama\r\n";
    const ULONG bytes = static_cast<ULONG>(std::wcslen(text) * 2);
    REQUIRE(krnl::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, text, 10, nullptr) == STATUS_SUCCESS);
    CHECK(iosb.Information == 10);
    REQUIRE(krnl::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, reinterpret_cast<const char*>(text) + 10,
                              bytes - 10, nullptr) == STATUS_SUCCESS);  // continues at the file pointer
    CHECK(EndOfFile(h) == bytes);
    krnl::NtClose(h);
    REQUIRE(Create(&h, 0x40100000, meta, nullptr, 4, 1, 3, 0x22, &iosb) == STATUS_SUCCESS);
    CHECK(iosb.Information == 1);  // FILE_OPENED -> XAPI does not rewrite it
    CHECK(EndOfFile(h) == bytes);
    krnl::NtClose(h);
    const std::vector<std::uint8_t> onDisk =
        ReadWholeFile(fx.Partition1() / "UDATA" / "46490002" / "TitleMeta.xbx");
    REQUIRE(onDisk.size() == bytes);
    CHECK(std::memcmp(onDisk.data(), text, bytes) == 0);

    // XGetDiskClusterSizeA("U:\") in _cinit (files.md 4.4): never 0, 0x4000.
    CHECK(ClusterSize("U:\\") == 0x4000u);
    CHECK(ClusterSize("u:\\") == 0x4000u);
    CHECK(ClusterSize("T:\\") == 0x4000u);
    CHECK(ClusterSize("Q:\\") == 0u);  // unknown drive: open fails

    CHECK(krnl::HandleTable::Instance().Count() == fx.baseHandles);
}

TEST_CASE("io: CreateFileA dispositions, information classes, file pointer, delete") {
    KernelFixture fx("io_files");
    {
        ObjAttr link("\\??\\U:", nullptr), dir("\\Device\\Harddisk0\\partition1\\UDATA", nullptr);
        REQUIRE(krnl::IoCreateSymbolicLink(&link.str, &dir.str) == STATUS_SUCCESS);
    }
    HANDLE h = nullptr;
    xbox::IO_STATUS_BLOCK iosb{};
    // CreateFileA(CREATE_NEW): FILE_CREATE; DesiredAccess | 0x100080; options SYNC|NON_DIRECTORY.
    REQUIRE(Create(&h, 0xC0100080, "U:\\save.dat", kDos, 0x80, 0, 2, 0x60, &iosb) == STATUS_SUCCESS);
    CHECK(iosb.Information == 2);
    std::vector<std::uint8_t> data(0x1000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 7);
    }
    xbox::LARGE_INTEGER off{};
    REQUIRE(krnl::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, data.data(), 0x1000, &off) ==
            STATUS_SUCCESS);
    CHECK(iosb.Information == 0x1000);

    // FilePositionInformation follows the synchronous writes; SetFilePointer(FILE_BEGIN) sets it.
    krnl::io::GuestFilePositionInformation pos{};
    REQUIRE(krnl::NtQueryInformationFile(h, &iosb, &pos, 8, 0xE) == STATUS_SUCCESS);
    CHECK(pos.CurrentByteOffset.QuadPart == 0x1000);
    pos.CurrentByteOffset.QuadPart = 0x10;
    REQUIRE(krnl::NtSetInformationFile(h, &iosb, &pos, 8, 0xE) == STATUS_SUCCESS);
    std::uint8_t small[4] = {};
    REQUIRE(krnl::NtReadFile(h, nullptr, nullptr, nullptr, &iosb, small, 4, nullptr) == STATUS_SUCCESS);
    CHECK(small[0] == static_cast<std::uint8_t>(0x10 * 7));
    REQUIRE(krnl::NtQueryInformationFile(h, &iosb, &pos, 8, 0xE) == STATUS_SUCCESS);
    CHECK(pos.CurrentByteOffset.QuadPart == 0x14);

    // read at EOF -> STATUS_END_OF_FILE (XAPI ReadFile: TRUE with 0 bytes), past EOF -> clipped
    off.QuadPart = 0x1000;
    CHECK(krnl::NtReadFile(h, nullptr, nullptr, nullptr, &iosb, small, 4, &off) == STATUS_END_OF_FILE);
    std::vector<std::uint8_t> big(0x800);
    off.QuadPart = 0xC00;
    REQUIRE(krnl::NtReadFile(h, nullptr, nullptr, nullptr, &iosb, big.data(), 0x800, &off) == STATUS_SUCCESS);
    CHECK(iosb.Information == 0x400);

    // SetEndOfFile 0xF7358: query 0xE, set 0x14 and 0x13 to the position.
    krnl::io::GuestFileEndOfFileInformation eof{};
    eof.EndOfFile.QuadPart = 0x800;
    REQUIRE(krnl::NtSetInformationFile(h, &iosb, &eof, 8, 0x14) == STATUS_SUCCESS);
    REQUIRE(krnl::NtSetInformationFile(h, &iosb, &eof, 8, 0x13) == STATUS_SUCCESS);
    CHECK(EndOfFile(h) == 0x800);

    // unsupported classes are refused, not crashed on
    std::uint8_t junk[64] = {};
    CHECK(krnl::NtQueryInformationFile(h, &iosb, junk, sizeof(junk), 0x30) == STATUS_INVALID_INFO_CLASS);
    CHECK(krnl::NtQueryInformationFile(h, &iosb, junk, 4, 0x22) == STATUS_INFO_LENGTH_MISMATCH);
    krnl::NtClose(h);

    // CREATE_NEW again -> collision; CREATE_ALWAYS (FILE_OVERWRITE_IF) -> FILE_OVERWRITTEN + truncated
    CHECK(Create(&h, 0xC0100080, "U:\\save.dat", kDos, 0x80, 0, 2, 0x60) == STATUS_OBJECT_NAME_COLLISION);
    REQUIRE(Create(&h, 0x40100080, "U:\\SAVE.DAT", kDos, 0x80, 0, 5, 0x60, &iosb) == STATUS_SUCCESS);
    CHECK(iosb.Information == 3);
    CHECK(EndOfFile(h) == 0);
    // a write on a handle opened for reading only is refused
    krnl::NtClose(h);
    REQUIRE(Create(&h, 0x80100080, "U:\\save.dat", kDos, 0x80, 1, 1, 0x60) == STATUS_SUCCESS);
    CHECK(krnl::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, data.data(), 4, &off) ==
          STATUS_ACCESS_DENIED);
    krnl::NtClose(h);
    CHECK(Create(&h, 0x80100080, "U:\\missing.dat", kDos, 0x80, 1, 1, 0x60) == STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(Create(&h, 0x80100080, "U:\\nodir\\missing.dat", kDos, 0x80, 1, 1, 0x60) ==
          STATUS_OBJECT_PATH_NOT_FOUND);

    // FATX / safety rules
    CHECK(Create(&h, 0xC0100080, "U:\\..\\escape.dat", kDos, 0x80, 0, 2, 0x60) == STATUS_OBJECT_NAME_INVALID);
    CHECK(Create(&h, 0xC0100080, "U:\\" + std::string(43, 'x'), kDos, 0x80, 0, 2, 0x60) ==
          STATUS_OBJECT_NAME_INVALID);
    CHECK(Create(&h, 0xC0100080, "U:\\a:b", kDos, 0x80, 0, 2, 0x60) == STATUS_OBJECT_NAME_INVALID);
    // reserved DOS names are ordinary files below "\\?\" (never the console device)
    REQUIRE(Create(&h, 0xC0110080, "U:\\CON", kDos, 0x80, 0, 5, 0x60) == STATUS_SUCCESS);
    xbox::LARGE_INTEGER zero{};
    REQUIRE(krnl::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, "con", 3, &zero) == STATUS_SUCCESS);
    CHECK(EndOfFile(h) == 3);
    std::uint8_t delCon = 1;
    REQUIRE(krnl::NtSetInformationFile(h, &iosb, &delCon, 1, 0xD) == STATUS_SUCCESS);
    krnl::NtClose(h);

    // CreateDirectoryA 0xF85AA: FILE_CREATE + DIRECTORY; twice -> collision
    REQUIRE(Create(&h, 0x100001, "U:\\ABCDEF012345", kDos, 0x80, 3, 2, 0x4021) == STATUS_SUCCESS);
    krnl::NtClose(h);
    CHECK(Create(&h, 0x100001, "U:\\ABCDEF012345", kDos, 0x80, 3, 2, 0x4021) == STATUS_OBJECT_NAME_COLLISION);
    // directory vs. file options
    CHECK(Create(&h, 0x80100080, "U:\\ABCDEF012345", kDos, 0x80, 1, 1, 0x60) == STATUS_FILE_IS_A_DIRECTORY);
    CHECK(Create(&h, 0x100001, "U:\\save.dat", kDos, 0x80, 3, 1, 0x4021) == STATUS_NOT_A_DIRECTORY);

    // SetFileAttributesA 0xF7B6C: NtOpenFile(0x100100, share 7, 0x4020) + class 4 (attributes at +0x20)
    REQUIRE(Open(&h, 0x100100, "U:\\save.dat", kDos, 7, 0x4020) == STATUS_SUCCESS);
    krnl::io::GuestFileBasicInformation basic{};
    basic.FileAttributes = 0x01 | 0x80;  // read-only
    REQUIRE(krnl::NtSetInformationFile(h, &iosb, &basic, 0x28, 4) == STATUS_SUCCESS);
    krnl::NtClose(h);
    CHECK((GetFileAttributesW((fx.Partition1() / "UDATA" / "save.dat").c_str()) & FILE_ATTRIBUTE_READONLY) !=
          0);
    {
        ObjAttr oa("U:\\save.dat", kDos);
        krnl::io::GuestFileNetworkOpenInformation n{};
        REQUIRE(krnl::NtQueryFullAttributesFile(&oa.oa, &n) == STATUS_SUCCESS);
        CHECK((n.FileAttributes & 0x01) != 0);
    }
    // RemoveDirectoryA-style delete of a read-only file is refused until the bit is cleared
    REQUIRE(Open(&h, 0x110100, "U:\\save.dat", kDos, 7, 0x4060) == STATUS_SUCCESS);
    std::uint8_t del = 1;
    CHECK(krnl::NtSetInformationFile(h, &iosb, &del, 1, 0xD) == STATUS_CANNOT_DELETE);
    basic.FileAttributes = 0x80;
    REQUIRE(krnl::NtSetInformationFile(h, &iosb, &basic, 0x28, 4) == STATUS_SUCCESS);
    REQUIRE(krnl::NtSetInformationFile(h, &iosb, &del, 1, 0xD) == STATUS_SUCCESS);
    krnl::NtClose(h);
    CHECK_FALSE(fs::exists(fx.Partition1() / "UDATA" / "save.dat"));

    CHECK(krnl::HandleTable::Instance().Count() == fx.baseHandles);
}

TEST_CASE("io: directory enumeration and recursive delete (FindFirstFileA, XDeleteSaveGame)") {
    KernelFixture fx("io_dirs");
    {
        ObjAttr link("\\??\\U:", nullptr), dir("\\Device\\Harddisk0\\partition1\\UDATA", nullptr);
        REQUIRE(krnl::IoCreateSymbolicLink(&link.str, &dir.str) == STATUS_SUCCESS);
    }
    HANDLE h = nullptr;
    REQUIRE(Create(&h, 0x100001, "U:\\0123456789AB", kDos, 0x80, 3, 2, 0x4021) == STATUS_SUCCESS);
    krnl::NtClose(h);
    for (const char* f : {"SaveMeta.xbx", "Futurama.dat", "saveimage.xbx"}) {
        REQUIRE(Create(&h, 0x40100080, std::string("U:\\0123456789AB\\") + f, kDos, 0x80, 0, 5, 0x60) ==
                STATUS_SUCCESS);
        krnl::NtClose(h);
    }
    REQUIRE(Create(&h, 0x100001, "U:\\EmptyDir", kDos, 0x80, 3, 2, 0x4021) == STATUS_SUCCESS);
    krnl::NtClose(h);

    // FindFirstFileA 0xF8961: NtOpenFile(dir, 0x100001, 3, 0x4021) + class 1 with the mask
    REQUIRE(Open(&h, 0x100001, "U:\\0123456789AB", kDos, 3, 0x4021) == STATUS_SUCCESS);
    std::vector<std::string> all = ListDir(h, "");
    CHECK(all.size() == 3);
    krnl::NtClose(h);
    REQUIRE(Open(&h, 0x100001, "U:\\0123456789AB", kDos, 3, 0x4021) == STATUS_SUCCESS);
    std::vector<std::string> xbx = ListDir(h, "*.xbx");
    CHECK(xbx.size() == 2);
    krnl::NtClose(h);
    REQUIRE(Open(&h, 0x100001, "U:\\", kDos, 3, 0x4021) == STATUS_SUCCESS);
    std::vector<std::string> top = ListDir(h, "*");
    CHECK(top.size() == 2);  // 0123456789AB, EmptyDir (no "." / "..")
    krnl::NtClose(h);
    NTSTATUS first = 0;
    REQUIRE(Open(&h, 0x100001, "U:\\EmptyDir", kDos, 3, 0x4021) == STATUS_SUCCESS);
    CHECK(ListDir(h, "*", &first).empty());
    CHECK(first == STATUS_NO_SUCH_FILE);
    krnl::NtClose(h);

    // directory with entries cannot get the delete disposition
    REQUIRE(Open(&h, 0x110101, "U:\\0123456789AB", kDos, 3, 0x4021) == STATUS_SUCCESS);
    std::uint8_t del = 1;
    xbox::IO_STATUS_BLOCK iosb{};
    CHECK(krnl::NtSetInformationFile(h, &iosb, &del, 1, 0xD) == STATUS_DIRECTORY_NOT_EMPTY);

    // recursive delete FUN_000f8709: RestartScan TRUE, then children opened RELATIVE to the handle
    alignas(8) std::uint8_t buf[0x146];
    BOOLEAN restart = TRUE;
    int deleted = 0;
    for (int guard = 0; guard < 10; ++guard) {
        const NTSTATUS s =
            krnl::NtQueryDirectoryFile(h, nullptr, nullptr, nullptr, &iosb, buf, 0x146, 1, nullptr, restart);
        restart = FALSE;
        if (!NT_SUCCESS(s)) {
            CHECK((s == STATUS_NO_MORE_FILES || s == STATUS_NO_SUCH_FILE));
            break;
        }
        const auto* d = reinterpret_cast<const krnl::io::GuestFileDirectoryInformation*>(buf);
        HANDLE child = nullptr;
        REQUIRE(Open(&child, 0x110100, std::string(d->FileName, d->FileNameLength), h, 7, 0x4040) ==
                STATUS_SUCCESS);
        REQUIRE(krnl::NtSetInformationFile(child, &iosb, &del, 1, 0xD) == STATUS_SUCCESS);
        krnl::NtClose(child);
        ++deleted;
    }
    CHECK(deleted == 3);
    // FUN_000f8709 resets the directory's attributes (attr & 0x3126 | 0x80) before deleting it
    krnl::io::GuestFileBasicInformation dirBasic{};
    dirBasic.FileAttributes = 0x80;
    CHECK(krnl::NtSetInformationFile(h, &iosb, &dirBasic, 0x28, 4) == STATUS_SUCCESS);
    REQUIRE(krnl::NtSetInformationFile(h, &iosb, &del, 1, 0xD) == STATUS_SUCCESS);
    krnl::NtClose(h);
    CHECK_FALSE(fs::exists(fx.Partition1() / "UDATA" / "0123456789AB"));

    // absolute "\??\U:\..." (XDeleteSaveGame form)
    REQUIRE(Open(&h, 0x110101, "\\??\\U:\\EmptyDir", nullptr, 3, 0x4021) == STATUS_SUCCESS);
    REQUIRE(krnl::NtSetInformationFile(h, &iosb, &del, 1, 0xD) == STATUS_SUCCESS);
    krnl::NtClose(h);
    CHECK_FALSE(fs::exists(fx.Partition1() / "UDATA" / "EmptyDir"));

    CHECK(krnl::HandleTable::Instance().Count() == fx.baseHandles);
}

TEST_CASE("io: D: from the disc image, APC completion, IOCTLs") {
    const fs::path iso = IsoPath();
    if (iso.empty()) {
        MESSAGE("skipped: disc image not found (set FUT_TEST_ISO)");
        return;
    }
    KernelFixture fx("io_disc");
    {
        ObjAttr link("\\??\\D:", nullptr), dev("\\Device\\CdRom0", nullptr);
        REQUIRE(krnl::IoCreateSymbolicLink(&link.str, &dev.str) == STATUS_SUCCESS);
    }
    HANDLE h = nullptr;
    xbox::IO_STATUS_BLOCK iosb{};
    // NiUDSStream 0x13F250: CreateFileA("D:\sysPAL.img", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
    // 0x68000000) -> asynchronous handle (no FILE_SYNCHRONOUS_IO_*), FILE_NON_DIRECTORY_FILE.
    REQUIRE(Create(&h, 0x80100080, "D:\\sysPAL.img", kDos, 0, 1, 1, 0x4C, &iosb) == STATUS_SUCCESS);
    CHECK(iosb.Information == 1);
    CHECK(EndOfFile(h) == 7742704u);

    // ReadFileEx 0xF7603: ApcRoutine + ApcContext + IoStatusBlock = OVERLAPPED, offset from OVERLAPPED.
    xbox::OVERLAPPED ovl{};
    ovl.Offset = static_cast<std::uint32_t>((7742704u - 1) & ~0x7FFu);  // last 2048-aligned chunk
    xbox::LARGE_INTEGER off{};
    off.LowPart = ovl.Offset;
    std::vector<std::uint8_t> ring(0x20000);
    auto* ovlIosb = reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl);
    void* completion =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x13F1D0));  // ApcContext (not called)
    g_apcCalls = 0;
    const NTSTATUS s =
        krnl::NtReadFile(h, nullptr, &TestApcRoutine, completion, ovlIosb, ring.data(), 0x20000, &off);
    CHECK(s == STATUS_PENDING);
    CHECK(ovl.Internal == 0);  // IoStatusBlock is written at delivery (design 1.7)
    CHECK(krnl::io::PendingIoApcCount() == 1);
    CHECK(g_apcCalls == 0);
    CHECK(krnl::io::DeliverIoApcs() == 1);
    CHECK(g_apcCalls == 1);
    CHECK(g_apcContext == completion);
    CHECK(g_apcIosb == ovlIosb);
    CHECK(ovl.Internal == static_cast<std::uint32_t>(STATUS_SUCCESS));
    CHECK(ovl.InternalHigh == 7742704u - ovl.Offset);  // clipped at the end of the file
    CHECK(krnl::io::PendingIoApcCount() == 0);
    CHECK(krnl::io::DeliverIoApcs() == 0);
    // end to end through the kernel's NtUserIoApcDispatcher (slot 39): completion(err, bytes, overlapped)
    const auto dispatcher = reinterpret_cast<krnl::io::IoApcRoutine>(
        krnl::GetBinding(krnl::exp::NtUserIoApcDispatcher::slot).address);
    if (krnl::IsImplemented(krnl::exp::NtUserIoApcDispatcher::slot)) {
        g_completionCalls = 0;
        xbox::OVERLAPPED ovl2{};
        off.QuadPart = 0;
        REQUIRE(krnl::NtReadFile(h, nullptr, dispatcher, reinterpret_cast<void*>(&TestCompletion),
                                 reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl2), ring.data(), 0x800,
                                 &off) == STATUS_PENDING);
        CHECK(krnl::io::DeliverIoApcs() == 1);
        CHECK(g_completionCalls == 1);
        CHECK(g_completionError == 0);
        CHECK(g_completionBytes == 0x800);
        CHECK(g_completionOverlapped == &ovl2);
    } else {
        MESSAGE("NtUserIoApcDispatcher not registered - end-to-end APC check skipped");
    }
    // a read that fails synchronously queues nothing
    off.QuadPart = 7742704 + 0x800;
    CHECK(krnl::NtReadFile(h, nullptr, &TestApcRoutine, completion, ovlIosb, ring.data(), 0x800, &off) ==
          STATUS_END_OF_FILE);
    CHECK(krnl::io::PendingIoApcCount() == 0);
    // the disc is read-only
    CHECK(krnl::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, ring.data(), 16, &off) ==
          STATUS_ACCESS_DENIED);
    krnl::NtClose(h);

    CHECK(Create(&h, 0x80100080, "D:\\FutPAL05.img", kDos, 0, 1, 1, 0x4C) == STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(Create(&h, 0x80100080, "D:\\fonts\\kanjii.fnt", kDos, 0, 1, 1, 0x4C) ==
          STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(Create(&h, 0x80100080, "D:\\Video\\Intro-PAL.txt", kDos, 0, 1, 1, 0x4C) ==
          STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(Create(&h, 0xC0100080, "D:\\new.dat", kDos, 0x80, 0, 2, 0x60) == STATUS_MEDIA_WRITE_PROTECTED);
    CHECK(Create(&h, 0xC0100080, "D:\\default.xbe", kDos, 0x80, 0, 1, 0x60) == STATUS_ACCESS_DENIED);
    REQUIRE(Create(&h, 0x80100080, "d:\\VIDEO/intro-pal.XMV", kDos, 0, 1, 1, 0x4C) == STATUS_SUCCESS);
    CHECK(EndOfFile(h) == 2519040u);
    krnl::NtClose(h);

    // DVD check FUN_000f90e6: NtOpenFile("\Device\CdRom0", 0x80100000, share 1, 0x20) + IOCTL 0x4D014 fails
    REQUIRE(Open(&h, 0x80100000, "\\Device\\CdRom0", nullptr, 1, 0x20) == STATUS_SUCCESS);
    std::uint8_t ioctlIn[0x2C] = {};
    CHECK(krnl::NtDeviceIoControlFile(h, nullptr, nullptr, nullptr, &iosb, 0x4D014, ioctlIn, 0x2C, nullptr,
                                      0) == STATUS_INVALID_DEVICE_REQUEST);
    CHECK(krnl::NtFsControlFile(h, nullptr, nullptr, nullptr, &iosb, 0x90020, nullptr, 0, nullptr, 0) ==
          STATUS_SUCCESS);
    krnl::NtClose(h);
    CHECK(krnl::NtDeviceIoControlFile(reinterpret_cast<HANDLE>(0x7777), nullptr, nullptr, nullptr, &iosb,
                                      0x4D014, nullptr, 0, nullptr, 0) == STATUS_INVALID_HANDLE);

    // XGetDiskSectorSizeA("D:\") / volume size of the disc
    REQUIRE(Open(&h, 0x100001, "D:\\", kDos, 3, 0x800021) == STATUS_SUCCESS);
    krnl::io::GuestFileFsSizeInformation fsSize{};
    REQUIRE(krnl::NtQueryVolumeInformationFile(h, &iosb, &fsSize, sizeof(fsSize), 3) == STATUS_SUCCESS);
    CHECK(fsSize.BytesPerSector == 2048u);
    CHECK(fsSize.AvailableAllocationUnits.QuadPart == 0);
    std::vector<std::string> rootNames = ListDir(h, "*.img");
    CHECK(rootNames.size() == 6);  // SysPAL + FutPAL00..04
    krnl::NtClose(h);

    {
        ObjAttr oa("D:\\default.xbe", kDos);
        krnl::io::GuestFileNetworkOpenInformation n{};
        REQUIRE(krnl::NtQueryFullAttributesFile(&oa.oa, &n) == STATUS_SUCCESS);
        CHECK(n.EndOfFile.QuadPart == 2973696);
        CHECK((n.FileAttributes & 0x10) == 0);
    }
    CHECK(krnl::HandleTable::Instance().Count() == fx.baseHandles);
}

TEST_CASE("io: CdRom0 without a disc image has no files") {
    vfs::ResetNamespace();
    core::Config cfg;
    cfg.userdata = TempDir("io_nodisc");
    REQUIRE(vfs::StageInitVfs(cfg));
    HANDLE h = nullptr;
    REQUIRE(Open(&h, 0x80100000, "\\Device\\CdRom0", nullptr, 1, 0x20) == STATUS_SUCCESS);
    krnl::NtClose(h);
    CHECK(Open(&h, 0x80100000, "\\Device\\CdRom0\\default.xbe", nullptr, 1, 0x60) ==
          STATUS_OBJECT_NAME_NOT_FOUND);
    vfs::ResetNamespace();
}

// =================================================================================================
// M2 (io agent): directory enumeration on both device types, Event parameter, APC paths, A8 / disc
// error handler path, VFS trace. The disc tests use the synthetic XDVDFS image (no ISO needed).
namespace {

// Namespace with \Device\CdRom0 = synthetic image (root {a.txt 5000, Dir {Sub File.dat}, zeta.bin}) and
// partition1 under a fresh userdata.
struct SyntheticDiscFixture {
    fs::path dir;
    std::size_t baseHandles = 0;

    explicit SyntheticDiscFixture(const char* name, std::vector<std::string> trace = {}) {
        vfs::ResetNamespace();
        dir = TempDir(name);
        WriteFileBytes(dir / "synthetic.iso", BuildSyntheticImage());
        core::Config cfg;
        cfg.userdata = dir / "userdata";
        cfg.iso = dir / "synthetic.iso";
        cfg.trace = std::move(trace);
        REQUIRE(vfs::StageInitVfs(cfg));
        baseHandles = krnl::HandleTable::Instance().Count();
    }
    ~SyntheticDiscFixture() {
        core::trace::Configure(core::trace::Options{});  // tracing off again
        vfs::ResetNamespace();
    }
    SyntheticDiscFixture(const SyntheticDiscFixture&) = delete;
    SyntheticDiscFixture& operator=(const SyntheticDiscFixture&) = delete;

    fs::path Partition1() const { return dir / "userdata" / "hdd" / "partition1"; }
};

struct DirRecord {
    std::string name;
    std::uint32_t attributes = 0;
    std::int64_t endOfFile = 0;
};

// One NtQueryDirectoryFile call (class 1, 0x148-byte buffer like FindFirstFileA).
NTSTATUS QueryOne(HANDLE dir, const char* mask, BOOLEAN restart, DirRecord* out,
                  xbox::IO_STATUS_BLOCK* iosbOut = nullptr, ULONG length = 0x148, HANDLE event = nullptr) {
    alignas(8) std::uint8_t buf[0x148] = {};
    xbox::ANSI_STRING m{};
    std::string maskText = mask != nullptr ? mask : "";
    m.Length = static_cast<std::uint16_t>(maskText.size());
    m.MaximumLength = m.Length;
    m.Buffer = maskText.data();
    xbox::IO_STATUS_BLOCK iosb{};
    const NTSTATUS s = krnl::NtQueryDirectoryFile(dir, event, nullptr, nullptr, &iosb, buf, length, 1,
                                                  mask != nullptr ? &m : nullptr, restart);
    if ((NT_SUCCESS(s) || s == STATUS_BUFFER_OVERFLOW) && out != nullptr) {
        const auto* d = reinterpret_cast<const krnl::io::GuestFileDirectoryInformation*>(buf);
        const std::uint32_t room = length - 0x40;
        out->name.assign(d->FileName, std::min(room, d->FileNameLength));
        out->attributes = d->FileAttributes;
        out->endOfFile = d->EndOfFile.QuadPart;
    }
    if (iosbOut != nullptr) {
        *iosbOut = iosb;
    }
    return s;
}

std::vector<std::string> Sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

krnl::io::IoApcRoutine DispatcherRoutine() {
    return reinterpret_cast<krnl::io::IoApcRoutine>(
        krnl::GetBinding(krnl::exp::NtUserIoApcDispatcher::slot).address);
}

std::vector<std::string> ReadLines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p);
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

bool AnyLineContains(const std::vector<std::string>& lines, std::initializer_list<const char*> parts) {
    for (const std::string& l : lines) {
        bool all = true;
        for (const char* p : parts) {
            if (l.find(p) == std::string::npos) {
                all = false;
                break;
            }
        }
        if (all) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("io: NtQueryDirectoryFile on the disc image (masks, attributes, restart, buffer, errors)") {
    SyntheticDiscFixture fx("io_dir_disc");
    HANDLE h = nullptr;
    // FindFirstFileA 0xF8961: NtOpenFile(dir, 0x100001, share 3, 0x4021 = SYNC_NONALERT|DIRECTORY|BACKUP)
    REQUIRE(Open(&h, 0x100001, "\\Device\\CdRom0\\", nullptr, 3, 0x4021) == STATUS_SUCCESS);
    CHECK(Sorted(ListDir(h, "")) == std::vector<std::string>{"Dir", "a.txt", "zeta.bin"});
    krnl::NtClose(h);
    REQUIRE(Open(&h, 0x100001, "\\Device\\CdRom0\\", nullptr, 3, 0x4021) == STATUS_SUCCESS);
    CHECK(ListDir(h, "*.TXT") == std::vector<std::string>{"a.txt"});  // case-insensitive mask
    krnl::NtClose(h);
    REQUIRE(Open(&h, 0x100001, "\\Device\\CdRom0\\", nullptr, 3, 0x4021) == STATUS_SUCCESS);
    CHECK(ListDir(h, "?eta.*") == std::vector<std::string>{"zeta.bin"});
    krnl::NtClose(h);
    NTSTATUS first = 0;
    REQUIRE(Open(&h, 0x100001, "\\Device\\CdRom0\\", nullptr, 3, 0x4021) == STATUS_SUCCESS);
    CHECK(ListDir(h, "*.xyz", &first).empty());
    CHECK(first == STATUS_NO_SUCH_FILE);  // nothing matched since the scan started
    krnl::NtClose(h);

    // attributes and sizes; one entry per call; RestartScan rewinds; the end is STATUS_NO_MORE_FILES
    REQUIRE(Open(&h, 0x100001, "\\Device\\CdRom0\\", nullptr, 3, 0x4021) == STATUS_SUCCESS);
    std::vector<DirRecord> recs;
    DirRecord r;
    xbox::IO_STATUS_BLOCK iosb{};
    REQUIRE(QueryOne(h, "*", FALSE, &r, &iosb) == STATUS_SUCCESS);
    CHECK(iosb.Information == 0x40 + r.name.size());
    const std::string firstName = r.name;
    recs.push_back(r);
    while (QueryOne(h, nullptr, FALSE, &r) == STATUS_SUCCESS) {
        recs.push_back(r);
    }
    REQUIRE(recs.size() == 3);
    CHECK(QueryOne(h, nullptr, FALSE, &r) == STATUS_NO_MORE_FILES);
    for (const DirRecord& d : recs) {
        CHECK((d.attributes & 0x01) != 0);  // the disc is read-only
        if (d.name == "Dir") {
            CHECK((d.attributes & 0x10) != 0);
            CHECK(d.endOfFile == 0);
        } else {
            CHECK((d.attributes & 0x10) == 0);
            CHECK(d.endOfFile == (d.name == "a.txt" ? kAFileSize : 10));
        }
    }
    REQUIRE(QueryOne(h, nullptr, TRUE, &r) == STATUS_SUCCESS);  // RestartScan
    CHECK(r.name == firstName);
    // a buffer too small for the name: STATUS_BUFFER_OVERFLOW, the entry is consumed (NT semantics)
    REQUIRE(QueryOne(h, nullptr, TRUE, &r) == STATUS_SUCCESS);
    CHECK(QueryOne(h, nullptr, FALSE, &r, &iosb, 0x42) == STATUS_BUFFER_OVERFLOW);
    CHECK(iosb.Information == 0x42);
    CHECK(r.name.size() == 2);
    CHECK(QueryOne(h, nullptr, FALSE, &r) == STATUS_SUCCESS);
    CHECK(QueryOne(h, nullptr, FALSE, &r) == STATUS_NO_MORE_FILES);
    CHECK(QueryOne(h, nullptr, FALSE, &r, nullptr, 0x3F) == STATUS_INFO_LENGTH_MISMATCH);
    {
        alignas(8) std::uint8_t buf[0x148];
        CHECK(krnl::NtQueryDirectoryFile(h, nullptr, nullptr, nullptr, &iosb, buf, sizeof(buf), 2, nullptr,
                                         FALSE) == STATUS_INVALID_INFO_CLASS);
    }
    // Event parameter: reset at the start, signalled on completion; a bad handle starts nothing
    const HANDLE ev = krnl::CreateEventHandle(krnl::EventObject::Type::Notification, false);
    REQUIRE(ev != nullptr);
    REQUIRE(QueryOne(h, nullptr, TRUE, &r, nullptr, 0x148, ev) == STATUS_SUCCESS);
    CHECK(krnl::LookupEvent(ev)->TryWait());
    CHECK(QueryOne(h, nullptr, FALSE, &r, nullptr, 0x148, reinterpret_cast<HANDLE>(0x7777)) ==
          STATUS_INVALID_HANDLE);
    CHECK(QueryOne(h, nullptr, FALSE, &r, nullptr, 0x148, h) == STATUS_OBJECT_TYPE_MISMATCH);
    REQUIRE(QueryOne(h, nullptr, FALSE, &r) == STATUS_SUCCESS);  // 2nd entry: the bad calls did not advance
    CHECK(r.name != firstName);
    krnl::NtClose(ev);
    krnl::NtClose(h);

    // subdirectory, opened relative to the root handle like FUN_000f8709
    HANDLE root = nullptr;
    REQUIRE(Open(&root, 0x100001, "\\Device\\CdRom0\\", nullptr, 3, 0x4021) == STATUS_SUCCESS);
    REQUIRE(Open(&h, 0x100001, "dir", root, 3, 0x4021) == STATUS_SUCCESS);
    CHECK(ListDir(h, "*") == std::vector<std::string>{"Sub File.dat"});
    krnl::NtClose(h);
    krnl::NtClose(root);
    // a file is not a directory
    REQUIRE(Open(&h, 0x80100080, "\\Device\\CdRom0\\a.txt", nullptr, 1, 0x40) == STATUS_SUCCESS);
    CHECK(QueryOne(h, "*", FALSE, &r) == STATUS_INVALID_PARAMETER);
    krnl::NtClose(h);
    CHECK(krnl::HandleTable::Instance().Count() == fx.baseHandles);
}

TEST_CASE("io: NtQueryDirectoryFile on a host directory (partition1) and with an APC") {
    SyntheticDiscFixture fx("io_dir_host");
    const fs::path title = fx.Partition1() / "UDATA" / "46490002";
    fs::create_directories(title / "0123456789AB");
    {
        std::ofstream(title / "TitleMeta.xbx", std::ios::binary) << "abcd";
    }
    {
        std::ofstream(title / "0123456789AB" / "SaveMeta.xbx", std::ios::binary) << "x";
    }
    HANDLE h = nullptr;
    REQUIRE(Open(&h, 0x100001, "\\Device\\Harddisk0\\partition1\\UDATA\\46490002", nullptr, 3, 0x4021) ==
            STATUS_SUCCESS);
    std::vector<DirRecord> recs;
    DirRecord r;
    for (NTSTATUS s = QueryOne(h, "*", FALSE, &r); s == STATUS_SUCCESS; s = QueryOne(h, nullptr, FALSE, &r)) {
        recs.push_back(r);
    }
    REQUIRE(recs.size() == 2);  // no "." / ".."
    for (const DirRecord& d : recs) {
        if (d.name == "0123456789AB") {
            CHECK((d.attributes & 0x10) != 0);
        } else {
            CHECK(d.name == "TitleMeta.xbx");
            CHECK((d.attributes & 0x10) == 0);
            CHECK(d.endOfFile == 4);
        }
    }
    krnl::NtClose(h);

    // asynchronous directory handle (no FILE_SYNCHRONOUS_IO_*) + NtUserIoApcDispatcher: STATUS_PENDING,
    // the IoStatusBlock is written when the APC is delivered by an alertable user-mode wait
    REQUIRE(Open(&h, 0x100001, "\\Device\\Harddisk0\\partition1\\UDATA\\46490002", nullptr, 3, 0x4001) ==
            STATUS_SUCCESS);
    alignas(8) std::uint8_t buf[0x148];
    xbox::OVERLAPPED ovl{};
    xbox::ANSI_STRING mask{};
    char maskText[] = "*.xbx";
    mask.Length = 5;
    mask.MaximumLength = 6;
    mask.Buffer = maskText;
    g_completionCalls = 0;
    CHECK(krnl::NtQueryDirectoryFile(h, nullptr, DispatcherRoutine(),
                                     reinterpret_cast<void*>(&TestCompletion),
                                     reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl), buf, sizeof(buf), 1,
                                     &mask, FALSE) == STATUS_PENDING);
    CHECK(ovl.InternalHigh == 0);
    xbox::LARGE_INTEGER zero;
    zero.QuadPart = 0;
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    CHECK(g_completionCalls == 1);
    CHECK(g_completionError == 0);
    CHECK(g_completionBytes == 0x40 + 13);  // "TitleMeta.xbx"
    CHECK(ovl.InternalHigh == 0x40 + 13);
    // no match left: STATUS_NO_MORE_FILES is a warning (0x80000006), not an error - completed like a
    // success (NT IopCompleteRequest), dwErrorCode 0 (BasepIoCompletion), the status in the OVERLAPPED
    CHECK(krnl::NtQueryDirectoryFile(h, nullptr, DispatcherRoutine(),
                                     reinterpret_cast<void*>(&TestCompletion),
                                     reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl), buf, sizeof(buf), 1,
                                     nullptr, FALSE) == STATUS_PENDING);
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    CHECK(g_completionCalls == 2);
    CHECK(g_completionError == 0);
    CHECK(ovl.Internal == static_cast<std::uint32_t>(STATUS_NO_MORE_FILES));
    // an error is returned synchronously: nothing is queued
    CHECK(krnl::NtQueryDirectoryFile(h, nullptr, DispatcherRoutine(),
                                     reinterpret_cast<void*>(&TestCompletion),
                                     reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl), buf, sizeof(buf), 7,
                                     nullptr, FALSE) == STATUS_INVALID_INFO_CLASS);
    CHECK(krnl::io::PendingIoApcCount() == 0);
    krnl::NtClose(h);

    // synchronous handle + APC: the final status is returned, the APC is still queued (NT)
    REQUIRE(Open(&h, 0x100001, "\\Device\\Harddisk0\\partition1\\UDATA\\46490002", nullptr, 3, 0x4021) ==
            STATUS_SUCCESS);
    xbox::OVERLAPPED ovl2{};
    CHECK(krnl::NtQueryDirectoryFile(h, nullptr, DispatcherRoutine(),
                                     reinterpret_cast<void*>(&TestCompletion),
                                     reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl2), buf, sizeof(buf), 1,
                                     nullptr, FALSE) == STATUS_SUCCESS);
    CHECK(ovl2.InternalHigh != 0);  // written at once on a synchronous handle
    CHECK(krnl::io::PendingIoApcCount() == 1);
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    CHECK(g_completionCalls == 3);
    krnl::NtClose(h);
    CHECK(krnl::HandleTable::Instance().Count() == fx.baseHandles);
}

TEST_CASE("io: NtReadFile Event parameter, end of file (A8) and the disc error handler path") {
    SyntheticDiscFixture fx("io_read_event");
    REQUIRE(krnl::IsImplemented(krnl::exp::NtUserIoApcDispatcher::slot));
    while (krnl::io::DeliverIoApcs() != 0) {
    }
    HANDLE h = nullptr;
    // NiUDSStream: CreateFileA(.., OPEN_EXISTING, 0x68000000) -> asynchronous handle, options 0x4C
    REQUIRE(Open(&h, 0x80100080, "\\Device\\CdRom0\\a.txt", nullptr, 1, 0x4C) == STATUS_SUCCESS);
    std::vector<std::uint8_t> buf(0x1000, 0xEE);
    xbox::IO_STATUS_BLOCK iosb{};
    xbox::LARGE_INTEGER off{};
    xbox::LARGE_INTEGER zero{};
    const HANDLE ev = krnl::CreateEventHandle(krnl::EventObject::Type::Notification, true);
    REQUIRE(ev != nullptr);

    // plain read with an event: completes at once, IoStatusBlock + event
    off.QuadPart = 0;
    CHECK(krnl::NtReadFile(h, ev, nullptr, nullptr, &iosb, buf.data(), 0x800, &off) == STATUS_SUCCESS);
    CHECK(iosb.Information == 0x800);
    CHECK(krnl::LookupEvent(ev)->TryWait());

    // ReadFileEx-style request with an event: STATUS_PENDING, IoStatusBlock + event at once, APC later
    const krnl::io::IoApcStats before = krnl::io::GetIoApcStats();
    xbox::OVERLAPPED ovl{};
    g_completionCalls = 0;
    off.QuadPart = 4096;  // last 2048-aligned chunk: runs 0x800 - 904 bytes past the end -> clipped
    CHECK(krnl::NtReadFile(h, ev, DispatcherRoutine(), reinterpret_cast<void*>(&TestCompletion),
                           reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl), buf.data(), 0x800,
                           &off) == STATUS_PENDING);
    CHECK(ovl.InternalHigh == kAFileSize - 4096);
    CHECK(krnl::LookupEvent(ev)->TryWait());
    CHECK(g_completionCalls == 0);
    CHECK(krnl::KeDelayExecutionThread(krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    CHECK(g_completionCalls == 1);
    CHECK(g_completionError == 0);  // a read past EOF never reaches the completion as an error
    CHECK(g_completionBytes == kAFileSize - 4096);

    // A8: a read that STARTS at/after the end fails synchronously - with NtUserIoApcDispatcher this is
    // the "ReadFileEx returns FALSE" path of FUN_0013F660 (disc error handler), counted and logged
    off.QuadPart = 6144;
    buf[0] = 0xEE;
    CHECK(krnl::NtReadFile(h, ev, DispatcherRoutine(), reinterpret_cast<void*>(&TestCompletion),
                           reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&ovl), buf.data(), 0x800,
                           &off) == STATUS_END_OF_FILE);
    CHECK(krnl::io::PendingIoApcCount() == 0);
    CHECK_FALSE(krnl::LookupEvent(ev)->TryWait());  // reset at the start, not signalled on failure
    const krnl::io::IoApcStats after = krnl::io::GetIoApcStats();
    CHECK(after.failedRequests - before.failedRequests == 1);
    CHECK(after.errorCompletions == before.errorCompletions);
    CHECK(after.queued - before.queued == 1);
    // synchronous ReadFile at EOF (XAPI turns STATUS_END_OF_FILE into TRUE with 0 bytes)
    CHECK(krnl::NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf.data(), 0x800, &off) ==
          STATUS_END_OF_FILE);

    // bad Event handles: the request is not started
    off.QuadPart = 0;
    CHECK(krnl::NtReadFile(h, reinterpret_cast<HANDLE>(0x7777), nullptr, nullptr, &iosb, buf.data(), 0x800,
                           &off) == STATUS_INVALID_HANDLE);
    CHECK(buf[0] == 0xEE);
    CHECK(krnl::NtReadFile(h, h, nullptr, nullptr, &iosb, buf.data(), 0x800, &off) ==
          STATUS_OBJECT_TYPE_MISMATCH);
    CHECK(buf[0] == 0xEE);
    krnl::NtClose(ev);
    krnl::NtClose(h);

    // NtWriteFile with an event on partition1 (WriteFileEx-style save write: STATUS_PENDING + APC)
    const HANDLE wev = krnl::CreateEventHandle(krnl::EventObject::Type::Synchronization, false);
    REQUIRE(wev != nullptr);
    REQUIRE(Create(&h, 0x40100080, "\\Device\\Harddisk0\\partition1\\TDATA\\save.dat", nullptr, 0x80, 0, 5,
                   0x48) == STATUS_SUCCESS);
    std::vector<std::uint8_t> data(0x1000, 0x5A);
    xbox::OVERLAPPED wovl{};
    off.QuadPart = 0;
    g_completionCalls = 0;
    CHECK(krnl::NtWriteFile(h, wev, DispatcherRoutine(), reinterpret_cast<void*>(&TestCompletion),
                            reinterpret_cast<xbox::IO_STATUS_BLOCK*>(&wovl), data.data(), 0x1000,
                            &off) == STATUS_PENDING);
    CHECK(krnl::NtWaitForSingleObjectEx(wev, krnl::kUserMode, TRUE, &zero) == STATUS_USER_APC);
    CHECK(g_completionCalls == 1);
    CHECK(g_completionBytes == 0x1000);
    CHECK(krnl::NtWaitForSingleObjectEx(wev, krnl::kUserMode, FALSE, &zero) == STATUS_SUCCESS);  // was set
    krnl::NtClose(h);
    krnl::NtClose(wev);
    CHECK(fs::file_size(fx.Partition1() / "TDATA" / "save.dat") == 0x1000);
    CHECK(krnl::HandleTable::Instance().Count() == fx.baseHandles);
}

TEST_CASE("io: --trace=vfs writes every open to vfs_trace.log (hit / documented miss / undocumented)") {
    SyntheticDiscFixture fx("io_vfs_trace", {"vfs"});
    REQUIRE(core::trace::VfsEnabled());
    const fs::path tracePath = core::trace::VfsTracePath();
    CHECK(tracePath == fx.dir / "userdata" / "logs" / "vfs_trace.log");
    {
        ObjAttr link("\\??\\D:", nullptr), dev("\\Device\\CdRom0", nullptr);
        REQUIRE(krnl::IoCreateSymbolicLink(&link.str, &dev.str) == STATUS_SUCCESS);
        CHECK(krnl::IoCreateSymbolicLink(&link.str, &dev.str) == STATUS_OBJECT_NAME_COLLISION);
    }
    HANDLE h = nullptr;
    REQUIRE(Create(&h, 0x80100080, "D:\\A.TXT", kDos, 0, 1, 1, 0x4C) == STATUS_SUCCESS);
    krnl::NtClose(h);
    CHECK(Create(&h, 0x80100080, "D:\\FutPAL05.img", kDos, 0, 1, 1, 0x4C) == STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(Create(&h, 0x80100080, "D:\\Video\\Intro-PAL.txt", kDos, 0, 1, 1, 0x4C) ==
          STATUS_OBJECT_PATH_NOT_FOUND);
    CHECK(Create(&h, 0x80100080, "D:\\nothing.bin", kDos, 0, 1, 1, 0x4C) == STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(Create(&h, 0xC0100080, "D:\\new.dat", kDos, 0x80, 0, 2, 0x60) == STATUS_MEDIA_WRITE_PROTECTED);
    CHECK(Create(&h, 0x80100080, "Q:\\x", kDos, 0, 1, 1, 0x4C) != STATUS_SUCCESS);  // no Q: link
    REQUIRE(Create(&h, 0x40100080, "\\Device\\Harddisk0\\partition1\\TDATA\\t.bin", nullptr, 0x80, 0, 5,
                   0x60) == STATUS_SUCCESS);
    krnl::NtClose(h);
    {
        ObjAttr oa("D:\\zeta.bin", kDos);
        krnl::io::GuestFileNetworkOpenInformation n{};
        CHECK(krnl::NtQueryFullAttributesFile(&oa.oa, &n) == STATUS_SUCCESS);
    }
    CHECK(core::trace::VfsLineCount() >= 10);

    const std::vector<std::string> lines = ReadLines(tracePath);
    CHECK(AnyLineContains(lines, {"symbolic link", "\\??\\D:", "\\Device\\CdRom0", ": created"}));
    CHECK(AnyLineContains(lines, {"symbolic link", "\\??\\D:", "exists STATUS_OBJECT_NAME_COLLISION"}));
    CHECK(AnyLineContains(lines, {"NtCreateFile", "\"\\??\\D:\\A.TXT\"", "-> \\Device\\CdRom0\\A.TXT",
                                  ": hit FILE_OPENED", "h="}));
    CHECK(AnyLineContains(
        lines, {"FutPAL05.img", ": missing STATUS_OBJECT_NAME_NOT_FOUND (documented: end of the FutPAL"}));
    CHECK(AnyLineContains(lines,
                          {"Intro-PAL.txt", ": missing STATUS_OBJECT_PATH_NOT_FOUND (documented: optional"}));
    CHECK(AnyLineContains(lines, {"nothing.bin", ": missing STATUS_OBJECT_NAME_NOT_FOUND UNDOCUMENTED"}));
    CHECK(AnyLineContains(lines, {"new.dat", ": error STATUS_MEDIA_WRITE_PROTECTED"}));
    CHECK(AnyLineContains(lines, {"\"\\??\\Q:\\x\" -> <unresolved>"}));
    CHECK(AnyLineContains(lines, {"TDATA\\t.bin", ": hit FILE_CREATED"}));
    CHECK(AnyLineContains(lines, {"NtQueryFullAttributesFile", "zeta.bin", ": hit STATUS_SUCCESS"}));
    for (const std::string& l : lines) {
        CHECK((l.empty() || l[0] == '#' || l[0] == '['));
    }
}
