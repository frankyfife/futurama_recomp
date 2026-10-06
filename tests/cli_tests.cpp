// Unit tests of the tool modes of futurama.exe (M3c, core/cli_tools.h): JSON writer, command-line handling,
// --check-iso on the real images (read-only; skipped when missing) and on synthetic/fake files, --query-gpu,
// --print-paths and the player configuration location (shared contract item 4), and an end-to-end run of the
// built futurama.exe (skipped when it is not next to fut_tests.exe).
//
// Images: %FUT_TEST_ISO% (retail EU, default below), %FUT_TEST_PROTO_ISO% (NTSC prototype .bin), XBEs:
// %FUT_TEST_XBE% (retail EU default.xbe), %FUT_TEST_PROTO_XBE%. Temporary files: %FUT_TEST_TMP% or
// <exe dir>\fut_tests_tmp (never %LOCALAPPDATA%; tests that need it point the variable at a temporary directory).
#include "doctest.h"

#include "cli_json.h"

#include "core/cli_tools.h"
#include "core/config.h"
#include "core/guest.h"
#include "shared/xbe_format.h"

#include <winioctl.h>

#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kDefaultIso[] =
    L"Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso";
constexpr wchar_t kDefaultProtoIso[] = L"Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Xbox - Futurama.bin";
constexpr wchar_t kDefaultXbe[] = L"D:\\FuturamaDecomp\\orig\\retail_eu\\default.xbe";
constexpr wchar_t kDefaultProtoXbe[] = L"D:\\FuturamaDecomp\\orig\\proto\\default.xbe";

fs::path EnvPath(const wchar_t* name, const wchar_t* fallback) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        return fs::path(buf);
    }
    return fallback != nullptr ? fs::path(fallback) : fs::path();
}

fs::path ExistingFile(const wchar_t* env, const wchar_t* fallback) {
    const fs::path p = EnvPath(env, fallback);
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? p : fs::path();
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// Fresh, empty temporary directory for one test case.
fs::path TempDir(const wchar_t* name) {
    fs::path base = EnvPath(L"FUT_TEST_TMP", nullptr);
    if (base.empty()) {
        base = ExeDir() / "fut_tests_tmp";
    }
    const fs::path dir = (base / name).make_preferred();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::uint8_t> ReadBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                              static_cast<std::streamsize>(bytes.size()));
}

testjson::Value ParseFile(const fs::path& p) {
    testjson::Value v;
    std::string error;
    const std::string text = ReadText(p);
    REQUIRE_MESSAGE(testjson::Parse(text, v, error), (error + " in " + p.string()));
    return v;
}

// Runs the tool dispatcher like RunHost does (in-process).
int Tool(const std::wstring& args, bool* handled = nullptr) {
    const std::wstring cmd = L"\"C:\\nowhere\\futurama.exe\" " + args;
    int code = -1;
    const bool h = core::cli::RunToolIfRequested(cmd.c_str(), L"C:\\nowhere\\futurama.exe", code);
    if (handled != nullptr) {
        *handled = h;
    }
    return h ? code : -1;
}

std::wstring Quoted(const std::wstring& option, const fs::path& p) {
    return L"\"" + option + L"=" + p.wstring() + L"\"";
}

// ---- synthetic XDVDFS images (layout: tools/xdvdfs.py, vfs/xdvdfs.cpp) -------------------------------------------
constexpr std::size_t kSector = 2048;
constexpr std::uint64_t kRedumpOffset = 0x18300000ull;

void PutU16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
void PutU32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

// Sector 32 volume descriptor, sector 33 root directory with ONE file `name`, file data from sector 34.
std::vector<std::uint8_t> BuildXdvdfs(const char* name, const std::vector<std::uint8_t>& content) {
    const std::size_t dataSectors = (content.size() + kSector - 1) / kSector;
    std::vector<std::uint8_t> img((34 + dataSectors) * kSector, 0);
    std::memcpy(img.data() + 32 * kSector, "MICROSOFT*XBOX*MEDIA", 20);
    PutU32(img, 32 * kSector + 20, 33);
    PutU32(img, 32 * kSector + 24, static_cast<std::uint32_t>(kSector));
    std::memcpy(img.data() + 32 * kSector + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    std::fill(img.begin() + 33 * kSector, img.begin() + 34 * kSector, std::uint8_t{0xFF});
    const std::size_t entry = 33 * kSector;
    const std::size_t n = std::strlen(name);
    PutU16(img, entry, 0);
    PutU16(img, entry + 2, 0);
    PutU32(img, entry + 4, 34);
    PutU32(img, entry + 8, static_cast<std::uint32_t>(content.size()));
    img[entry + 12] = 0x20;  // FILE_ATTRIBUTE_ARCHIVE
    img[entry + 13] = static_cast<std::uint8_t>(n);
    std::memcpy(img.data() + entry + 14, name, n);
    if (!content.empty()) {
        std::memcpy(img.data() + 34 * kSector, content.data(), content.size());
    }
    return img;
}

// The image at `offset` of a sparse file (redump layout without writing 400 MB). False if sparse files are not
// supported by the temporary directory's file system.
bool WriteSparseImage(const fs::path& file, std::uint64_t offset, const std::vector<std::uint8_t>& img) {
    const HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr) != FALSE;
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    DWORD written = 0;
    ok = ok && SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) &&
         WriteFile(h, img.data(), static_cast<DWORD>(img.size()), &written, nullptr) && written == img.size();
    CloseHandle(h);
    return ok;
}

// Minimal XBE: header (base 0x10000, headers 0x1000 bytes) + certificate at 0x180 with title id and name.
std::vector<std::uint8_t> FakeXbe(const wchar_t* title, std::uint32_t titleId) {
    std::vector<std::uint8_t> x(0x2000, 0x5A);
    xbe::ImageHeader h{};
    h.Magic = xbe::kMagic;
    h.BaseAddress = 0x10000;
    h.SizeOfHeaders = 0x1000;
    h.SizeOfImage = 0x2000;
    h.SizeOfImageHeader = 0x180;
    h.CertificateAddress = 0x10180;
    std::memcpy(x.data(), &h, sizeof(h));
    xbe::Certificate c{};
    c.SizeOfCertificate = sizeof(c);
    c.TitleId = titleId;
    std::wcsncpy(c.TitleName, title, std::size(c.TitleName) - 1);
    std::memcpy(x.data() + 0x180, &c, sizeof(c));
    return x;
}

// Sets an environment variable for the lifetime of the object (restores the old value).
class ScopedEnv {
public:
    ScopedEnv(const wchar_t* name, const std::wstring& value) : name_(name) {
        wchar_t buf[2048];
        const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
        had_ = n > 0 && n < std::size(buf);
        if (had_) {
            old_.assign(buf, n);
        }
        SetEnvironmentVariableW(name, value.c_str());
    }
    ~ScopedEnv() { SetEnvironmentVariableW(name_, had_ ? old_.c_str() : nullptr); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const wchar_t* name_;
    std::wstring old_;
    bool had_ = false;
};

testjson::Value CheckIsoDoc(const fs::path& image, const fs::path& dir, int* exitCode) {
    const fs::path result = dir / "result (check).json";
    *exitCode = Tool(Quoted(L"--check-iso", image) + L" " + Quoted(L"--result", result));
    return ParseFile(result);
}

// Runs a process and returns its exit code (-1 on failure or timeout).
int RunProcess(std::wstring commandLine, DWORD timeoutMs = 60000) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi)) {
        return -1;
    }
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 99);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace

TEST_CASE("cli: JsonWriter writes valid, escaped JSON") {
    core::cli::JsonWriter w;
    w.BeginObject();
    w.Key("quote \"\\ path").String("C:\\a \"b\"\\c");
    w.Key("control").String(std::string("tab\tnl\ncr\r\x01\x1f end"));
    w.Key("umlaut").String("Kantenglättung × 2");
    w.Key("numbers");
    w.BeginArray(true);
    w.Int(-5);
    w.UInt(4294967295u);
    w.Number(0.25);
    w.Number(1.0, true);
    w.Number(2.0);
    w.Number(std::numeric_limits<double>::infinity());
    w.EndArray();
    w.Key("empty_object");
    w.BeginObject();
    w.EndObject();
    w.Key("empty_array");
    w.BeginArray();
    w.EndArray();
    w.Key("nested");
    w.BeginArray();
    w.BeginArray(true);
    w.UInt(640);
    w.UInt(480);
    w.EndArray();
    w.BeginObject(true);
    w.Key("de").String("Aus");
    w.Key("en").Null();
    w.EndObject();
    w.EndArray();
    w.Key("flag").Bool(false);
    w.EndObject();
    const std::string text = w.Take();
    CHECK(text.back() == '\n');
    CHECK(text.find("[640, 480]") != std::string::npos);  // inline containers stay on one line
    testjson::Value v;
    std::string error;
    REQUIRE_MESSAGE(testjson::Parse(text, v, error), (error + "\n" + text));
    CHECK(v["quote \"\\ path"].text == "C:\\a \"b\"\\c");
    CHECK(v["control"].text == std::string("tab\tnl\ncr\r\x01\x1f end"));
    CHECK(v["umlaut"].text == "Kantenglättung × 2");
    REQUIRE(v["numbers"].size() == 6u);
    CHECK(v["numbers"][0].number == -5.0);
    CHECK(v["numbers"][1].number == 4294967295.0);
    CHECK(v["numbers"][2].number == 0.25);
    CHECK(v["numbers"][3].text == "1.0");
    CHECK(v["numbers"][4].text == "2");
    CHECK(v["numbers"][5].IsNull());
    CHECK(v["empty_object"].IsObject());
    CHECK(v["empty_array"].IsArray());
    CHECK(v["nested"][0][1].number == 480.0);
    CHECK(v["nested"][1]["de"].text == "Aus");
    CHECK(v["nested"][1]["en"].IsNull());
    CHECK(v["flag"].IsBool());
    CHECK(core::cli::JsonWriter::Escape("\x7f") == "\"\\u007f\"");
}

TEST_CASE("cli: only tool options start a tool mode; usage errors exit with 1") {
    bool handled = true;
    CHECK(Tool(L"--config=x.ini --selftest --stop-at=main", &handled) == -1);
    CHECK_FALSE(handled);
    CHECK(Tool(L"", &handled) == -1);
    CHECK_FALSE(handled);
    int code = 0;
    CHECK_FALSE(core::cli::RunToolIfRequested(nullptr, L"C:\\nowhere\\futurama.exe", code));
    CHECK_FALSE(core::cli::RunToolIfRequested(L"futurama.exe", L"C:\\nowhere\\futurama.exe", code));

    const fs::path dir = TempDir(L"cli usage (a,b)");
    const fs::path out = dir / "out.json";
    // Missing file names, two tools, foreign options, --result without --check-iso: usage errors, nothing written.
    for (const std::wstring& bad :
         {std::wstring(L"--query-gpu"), std::wstring(L"--print-config-schema="),
          Quoted(L"--print-config-schema", out) + L" " + Quoted(L"--query-gpu", out),
          Quoted(L"--print-config-schema", out) + L" --selftest", Quoted(L"--check-iso", out),
          Quoted(L"--result", out), Quoted(L"--query-gpu", out) + L" " + Quoted(L"--result", out),
          Quoted(L"--print-config-schema", dir / "missing dir" / "x.json")}) {
        CAPTURE(core::WideToUtf8(bad));
        CHECK(Tool(bad, &handled) == core::cli::kToolExitUsage);
        CHECK(handled);
        std::error_code ec;
        CHECK_FALSE(fs::exists(out, ec));
    }
    // --config may accompany any tool (the launcher passes it habitually).
    CHECK(Tool(Quoted(L"--print-config-schema", out) + L" --config=whatever.ini") == core::cli::kToolExitOk);
    CHECK(fs::exists(out));
}

TEST_CASE("cli: --print-config-schema writes the schema atomically (paths with spaces, commas, parentheses)") {
    const fs::path dir = TempDir(L"cli schema (En,Fr) out");
    const fs::path file = dir / "schema file (1).json";
    std::ofstream(file, std::ios::binary) << "old content";
    REQUIRE(Tool(Quoted(L"--print-config-schema", file)) == core::cli::kToolExitOk);
    const testjson::Value doc = ParseFile(file);
    CHECK(doc["schema_version"].number == 1.0);
    CHECK(doc["sections"].size() >= 8u);
    std::error_code ec;
    CHECK_FALSE(fs::exists(fs::path(file.wstring() + L".tmp"), ec));
    // Same text as the in-process document.
    CHECK(ReadText(file) == core::cli::ConfigSchemaJson(L"C:\\nowhere\\futurama.exe"));
}

TEST_CASE("cli: --check-iso detects the retail EU image (real ISO, read-only)") {
    const fs::path iso = ExistingFile(L"FUT_TEST_ISO", kDefaultIso);
    if (iso.empty()) {
        MESSAGE("skipped: retail EU image not found (set FUT_TEST_ISO)");
        return;
    }
    const fs::path dir = TempDir(L"cli iso retail");
    int code = -1;
    const testjson::Value r = CheckIsoDoc(iso, dir, &code);
    CHECK(code == core::cli::kToolExitOk);
    CHECK(r["ok"].boolean);
    CHECK(r["supported"].boolean);
    CHECK(r["xdvdfs"].boolean);
    CHECK(r["version"].text == "retail_eu");
    CHECK(r["xbe_sha1"].text == guest::kXbeSha1);
    CHECK(r["reason_code"].text == "supported");
    CHECK((r["layout"].text == "xiso" || r["layout"].text == "redump"));
    CHECK(r["size"].number == static_cast<double>(fs::file_size(iso)));
    CHECK(r["xbe_size"].number == static_cast<double>(guest::kXbeFileSize));
    CHECK(r["title"].text.find("Futurama") != std::string::npos);
    CHECK_FALSE(r["message"]["de"].text.empty());
    CHECK_FALSE(r["message"]["en"].text.empty());
    MESSAGE("retail image: layout " << r["layout"].text << ", title \"" << r["title"].text << "\", title id "
                                    << r["title_id"].text);
    // The in-process result is the same.
    const core::cli::IsoCheckResult direct = core::cli::CheckIso(iso);
    CHECK(direct.supported);
    CHECK(direct.partitionOffset == static_cast<std::uint64_t>(r["partition_offset"].number));
}

TEST_CASE("cli: --check-iso recognises the NTSC prototype and rejects it (real image, read-only)") {
    const fs::path bin = ExistingFile(L"FUT_TEST_PROTO_ISO", kDefaultProtoIso);
    if (bin.empty()) {
        MESSAGE("skipped: prototype image not found (set FUT_TEST_PROTO_ISO)");
        return;
    }
    const fs::path dir = TempDir(L"cli iso proto");
    int code = -1;
    const testjson::Value r = CheckIsoDoc(bin, dir, &code);
    CHECK(code == core::cli::kToolExitNotSupported);
    CHECK(r["ok"].boolean);
    CHECK_FALSE(r["supported"].boolean);
    CHECK(r["xdvdfs"].boolean);
    CHECK(r["version"].text == "proto_ntsc");
    CHECK(r["xbe_sha1"].text == core::cli::kProtoNtscXbeSha1);
    CHECK(r["reason_code"].text == "prototype");
    MESSAGE("prototype image: layout " << r["layout"].text << ", title \"" << r["title"].text << "\"");
}

TEST_CASE("cli: --check-iso on missing, foreign and synthetic files") {
    const fs::path dir = TempDir(L"cli iso fake (x,y)");
    int code = -1;
    SUBCASE("missing file") {
        const testjson::Value r = CheckIsoDoc(dir / "no such image.iso", dir, &code);
        CHECK(code == core::cli::kToolExitNotSupported);
        CHECK_FALSE(r["ok"].boolean);
        CHECK(r["reason_code"].text == "not_found");
        CHECK(r["layout"].text == "unknown");
        CHECK(r["version"].text == "unknown");
        CHECK(r["size"].number == 0.0);
    }
    SUBCASE("directory") {
        const testjson::Value r = CheckIsoDoc(dir, dir, &code);
        CHECK(r["reason_code"].text == "not_a_file");
        CHECK_FALSE(r["ok"].boolean);
    }
    SUBCASE("random data") {
        std::vector<std::uint8_t> junk(300 * 1024);
        std::mt19937 rng(1234);
        for (auto& b : junk) {
            b = static_cast<std::uint8_t>(rng());
        }
        WriteBytes(dir / "junk.iso", junk);
        const testjson::Value r = CheckIsoDoc(dir / "junk.iso", dir, &code);
        CHECK(code == core::cli::kToolExitNotSupported);
        CHECK_FALSE(r["ok"].boolean);
        CHECK_FALSE(r["xdvdfs"].boolean);
        CHECK(r["reason_code"].text == "no_xdvdfs");
        CHECK(r["size"].number == static_cast<double>(junk.size()));
    }
    SUBCASE("empty file") {
        WriteBytes(dir / "empty.iso", {});
        const testjson::Value r = CheckIsoDoc(dir / "empty.iso", dir, &code);
        CHECK(r["reason_code"].text == "no_xdvdfs");
    }
    SUBCASE("XISO without default.xbe") {
        WriteBytes(dir / "noxbe.iso", BuildXdvdfs("readme.txt", {'h', 'i'}));
        const testjson::Value r = CheckIsoDoc(dir / "noxbe.iso", dir, &code);
        CHECK(r["xdvdfs"].boolean);
        CHECK(r["layout"].text == "xiso");
        CHECK(r["reason_code"].text == "no_default_xbe");
        CHECK_FALSE(r["ok"].boolean);
    }
    SUBCASE("XISO with a damaged default.xbe") {
        WriteBytes(dir / "badxbe.iso", BuildXdvdfs("default.xbe", std::vector<std::uint8_t>(0x800, 0x11)));
        const testjson::Value r = CheckIsoDoc(dir / "badxbe.iso", dir, &code);
        CHECK(r["reason_code"].text == "bad_xbe");
        CHECK_FALSE(r["ok"].boolean);
        CHECK(r["xbe_sha1"].text.size() == 40u);
    }
    SUBCASE("XISO of another game") {
        WriteBytes(dir / "other.iso", BuildXdvdfs("DEFAULT.XBE", FakeXbe(L"Some Other Game", 0x41420001)));
        const testjson::Value r = CheckIsoDoc(dir / "other.iso", dir, &code);
        CHECK(code == core::cli::kToolExitNotSupported);
        CHECK(r["ok"].boolean);
        CHECK_FALSE(r["supported"].boolean);
        CHECK(r["reason_code"].text == "other_game");
        CHECK(r["title"].text == "Some Other Game");
        CHECK(r["title_id"].text == "41420001");
        CHECK(r["version"].text == "unknown");
    }
    SUBCASE("XISO of another Futurama release") {
        WriteBytes(dir / "us.iso", BuildXdvdfs("default.xbe", FakeXbe(L"Futurama", 0x56550004)));
        const testjson::Value r = CheckIsoDoc(dir / "us.iso", dir, &code);
        CHECK(r["reason_code"].text == "other_version");
        CHECK(r["ok"].boolean);
        CHECK_FALSE(r["supported"].boolean);
    }
    SUBCASE("redump layout (partition at 0x18300000, sparse)") {
        const fs::path file = dir / "redump.iso";
        if (!WriteSparseImage(file, kRedumpOffset, BuildXdvdfs("default.xbe", FakeXbe(L"Sparse Test", 1)))) {
            MESSAGE("skipped: the temporary directory does not support sparse files");
            return;
        }
        const testjson::Value r = CheckIsoDoc(file, dir, &code);
        CHECK(r["xdvdfs"].boolean);
        CHECK(r["layout"].text == "redump");
        CHECK(r["partition_offset"].number == static_cast<double>(kRedumpOffset));
        CHECK(r["title"].text == "Sparse Test");
        CHECK(r["size"].number > static_cast<double>(kRedumpOffset));
    }
    SUBCASE("synthetic images with the real XBEs (retail EU / prototype, XISO and redump)") {
        const fs::path retail = ExistingFile(L"FUT_TEST_XBE", kDefaultXbe);
        const fs::path proto = ExistingFile(L"FUT_TEST_PROTO_XBE", kDefaultProtoXbe);
        if (retail.empty() || proto.empty()) {
            MESSAGE("skipped: orig\\retail_eu\\default.xbe or orig\\proto\\default.xbe not found");
            return;
        }
        WriteBytes(dir / "retail.iso", BuildXdvdfs("default.xbe", ReadBytes(retail)));
        testjson::Value r = CheckIsoDoc(dir / "retail.iso", dir, &code);
        CHECK(code == core::cli::kToolExitOk);
        CHECK(r["supported"].boolean);
        CHECK(r["version"].text == "retail_eu");
        CHECK(r["layout"].text == "xiso");
        if (WriteSparseImage(dir / "retail redump.iso", kRedumpOffset, BuildXdvdfs("default.xbe", ReadBytes(retail)))) {
            r = CheckIsoDoc(dir / "retail redump.iso", dir, &code);
            CHECK(code == core::cli::kToolExitOk);
            CHECK(r["layout"].text == "redump");
            CHECK(r["version"].text == "retail_eu");
        }
        WriteBytes(dir / "proto.iso", BuildXdvdfs("default.xbe", ReadBytes(proto)));
        r = CheckIsoDoc(dir / "proto.iso", dir, &code);
        CHECK(code == core::cli::kToolExitNotSupported);
        CHECK(r["version"].text == "proto_ntsc");
        CHECK(r["reason_code"].text == "prototype");
    }
}

TEST_CASE("cli: --query-gpu lists adapters, outputs and MSAA levels") {
    const core::cli::GpuInfo info = core::cli::QueryGpu();
    if (!info.error.empty()) {
        MESSAGE("DXGI unavailable: " << info.error);
    }
    const fs::path dir = TempDir(L"cli gpu");
    REQUIRE(Tool(Quoted(L"--query-gpu", dir / "gpu.json")) == core::cli::kToolExitOk);
    const testjson::Value doc = ParseFile(dir / "gpu.json");
    REQUIRE(doc["adapters"].IsArray());
    REQUIRE(doc["outputs"].IsArray());
    REQUIRE(doc["msaa"].IsArray());
    CHECK(doc["msaa"][0].number == 1.0);
    CHECK(doc["max_anisotropy"].IsInteger());
    CHECK(doc["tearing"].IsBool());
    static const std::set<std::string> kHints = {"possible", "unavailable", "unknown"};
    CHECK(kHints.count(doc["vrr_hint"].text) == 1);
    if (info.adapters.empty()) {
        return;
    }
    for (const testjson::Value& a : doc["adapters"].items) {
        CHECK(a["name"].IsString());
        CHECK(a["vendor_id"].IsInteger());
        CHECK(a["dedicated_mb"].IsInteger());
        REQUIRE(a["msaa"].IsArray());
        CHECK(a["msaa"][0].number == 1.0);
        double previous = 0.0;
        for (const testjson::Value& s : a["msaa"].items) {
            CHECK(s.number > previous);  // ascending
            CHECK((s.number == 1.0 || s.number == 2.0 || s.number == 4.0 || s.number == 8.0));
            previous = s.number;
        }
        MESSAGE("adapter " << a["name"].text << ": " << a["dedicated_mb"].number << " MB ("
                           << a["dedicated_mb_source"].text << "), feature level " << a["feature_level"].text
                           << ", MSAA up to " << previous << "x");
    }
    CHECK(doc["default_adapter"].number == 0.0);
    CHECK(doc["msaa"].size() == doc["adapters"][0]["msaa"].size());
    bool primary = false;
    for (const testjson::Value& o : doc["outputs"].items) {
        CHECK(o["device"].text.rfind("\\\\.\\DISPLAY", 0) == 0);
        CHECK(o["rect"].size() == 4u);
        CHECK(o["refresh"]["den"].number >= 1.0);
        REQUIRE(o["modes"].IsArray());
        for (const testjson::Value& m : o["modes"].items) {
            CHECK(m.size() == 2u);
        }
        primary = primary || o["primary"].boolean;
        MESSAGE("output " << o["device"].text << " \"" << o["name"].text << "\": " << o["rect"][2].number << "x"
                          << o["rect"][3].number << " @ " << o["refresh"]["num"].number << "/"
                          << o["refresh"]["den"].number << " Hz (" << o["refresh_source"].text << "), "
                          << o["modes"].size() << " modes, dpi " << o["dpi"].number);
    }
    if (!doc["outputs"].items.empty()) {
        CHECK(primary);
    }
}

TEST_CASE("cli: player configuration location - default, portable, explicit (contract item 4)") {
    const fs::path dir = TempDir(L"cli location (a,b)");
    const fs::path appData = dir / "Local AppData";
    const fs::path exeDir = dir / "Game Dir";
    fs::create_directories(appData);
    fs::create_directories(exeDir);
    const fs::path exe = exeDir / "futurama.exe";
    const ScopedEnv env(L"LOCALAPPDATA", appData.wstring());
    const fs::path defaultDir = appData / "FuturamaPC";

    // default mode
    core::ConfigLocation loc = core::ResolveConfigLocation(exeDir, {});
    CHECK(loc.mode == core::ConfigLocationMode::Default);
    CHECK_FALSE(loc.portable);
    CHECK(loc.configFile == defaultDir / "futurama.ini");
    CHECK(loc.defaultUserdata == defaultDir);
    core::Config cfg;
    std::string error;
    REQUIRE_MESSAGE(core::LoadConfig(L"futurama.exe", exe, cfg, error), error);
    CHECK(cfg.configFile.empty());  // no INI yet: built-in defaults
    CHECK(cfg.userdata == defaultDir);
    CHECK(cfg.configMode == core::ConfigLocationMode::Default);
    fs::create_directories(defaultDir);
    std::ofstream(defaultDir / "futurama.ini", std::ios::binary) << "[render]\nmsaa = 8\n";
    REQUIRE_MESSAGE(core::LoadConfig(L"futurama.exe", exe, cfg, error), error);
    CHECK(cfg.configFile == defaultDir / "futurama.ini");
    CHECK(cfg.GetString("render", "msaa") == "8");
    CHECK(cfg.warnings.empty());
    // A pre-M3c INI next to the exe is ignored with a warning.
    std::ofstream(exeDir / "futurama.ini", std::ios::binary) << "[render]\nmsaa = 2\n";
    REQUIRE_MESSAGE(core::LoadConfig(L"futurama.exe", exe, cfg, error), error);
    CHECK(cfg.GetString("render", "msaa") == "8");
    REQUIRE(cfg.warnings.size() == 1u);
    CHECK(cfg.warnings[0].find("is ignored") != std::string::npos);
    CHECK(cfg.warnings[0].find("portable.txt") != std::string::npos);

    // portable mode: INI and user data next to the exe (the exe directory is the user data directory)
    std::ofstream(exeDir / "portable.txt", std::ios::binary) << "";
    loc = core::ResolveConfigLocation(exeDir, {});
    CHECK(loc.mode == core::ConfigLocationMode::Portable);
    CHECK(loc.portable);
    CHECK(loc.configFile == exeDir / "futurama.ini");
    CHECK(loc.defaultUserdata == exeDir);
    REQUIRE_MESSAGE(core::LoadConfig(L"futurama.exe", exe, cfg, error), error);
    CHECK(cfg.configFile == exeDir / "futurama.ini");
    CHECK(cfg.GetString("render", "msaa") == "2");
    CHECK(cfg.userdata == exeDir);
    CHECK(cfg.portable);
    CHECK(cfg.warnings.empty());

    // explicit --config wins; userdata default follows portable.txt; [paths] userdata wins over both
    const fs::path other = dir / "other (1).ini";
    std::ofstream(other, std::ios::binary) << "[render]\nmsaa = off\n";
    const std::wstring cmd = L"futurama.exe \"--config=" + other.wstring() + L"\"";
    REQUIRE_MESSAGE(core::LoadConfig(cmd.c_str(), exe, cfg, error), error);
    CHECK(cfg.configMode == core::ConfigLocationMode::Explicit);
    CHECK(cfg.configFile == other);
    CHECK(cfg.GetString("render", "msaa") == "off");
    CHECK(cfg.userdata == exeDir);
    fs::remove(exeDir / "portable.txt");
    REQUIRE_MESSAGE(core::LoadConfig(cmd.c_str(), exe, cfg, error), error);
    CHECK(cfg.userdata == defaultDir);
    CHECK(cfg.warnings.empty());  // the exe-dir INI is not mentioned with --config
    std::ofstream(other, std::ios::binary) << "[paths]\nuserdata = my data\n";
    REQUIRE_MESSAGE(core::LoadConfig(cmd.c_str(), exe, cfg, error), error);
    CHECK(cfg.userdata == dir / "my data");  // relative to the INI
    CHECK_FALSE(core::LoadConfig(L"futurama.exe \"--config=C:\\no such dir\\x.ini\"", exe, cfg, error));
    CHECK(error.find("config file not found") != std::string::npos);

    // --print-paths reports the same
    const std::string paths = core::cli::PathsJson(exe, other);
    testjson::Value doc;
    REQUIRE_MESSAGE(testjson::Parse(paths, doc, error), error);
    CHECK(doc["ok"].boolean);
    CHECK(doc["mode"].text == "explicit");
    CHECK(doc["config"].text == core::WideToUtf8(other.wstring()));
    CHECK(doc["config_exists"].boolean);
    CHECK(doc["userdata"].text == core::WideToUtf8((dir / "my data").wstring()));
    CHECK(doc["userdata_configured"].boolean);
    CHECK(doc["logs"].text == core::WideToUtf8((dir / "my data" / "logs").wstring()));
    CHECK(doc["saves"].text == core::WideToUtf8((dir / "my data" / "hdd" / "partition1" / "UDATA").wstring()));
    CHECK(doc["screenshots"].text == core::WideToUtf8((dir / "my data" / "screenshots").wstring()));
    REQUIRE_MESSAGE(testjson::Parse(core::cli::PathsJson(exe, {}), doc, error), error);
    CHECK(doc["mode"].text == "default");
    CHECK(doc["config"].text == core::WideToUtf8((defaultDir / "futurama.ini").wstring()));
    CHECK(doc["userdata"].text == core::WideToUtf8(defaultDir.wstring()));
    CHECK_FALSE(doc["userdata_configured"].boolean);
    CHECK(doc["warnings"].size() == 1u);  // the ignored exe-dir INI
    // A broken INI: ok = false with the error, locations still reported.
    std::ofstream(other, std::ios::binary) << "[render]\nmsaa = 3\n";
    REQUIRE_MESSAGE(testjson::Parse(core::cli::PathsJson(exe, other), doc, error), error);
    CHECK_FALSE(doc["ok"].boolean);
    CHECK(doc["error"].text.find("msaa") != std::string::npos);
    CHECK(doc["userdata"].text == core::WideToUtf8(defaultDir.wstring()));
    // The schema names the default location of this exe.
    REQUIRE_MESSAGE(testjson::Parse(core::cli::ConfigSchemaJson(exe), doc, error), error);
    CHECK(doc["config"]["file"].text == core::WideToUtf8((defaultDir / "futurama.ini").wstring()));
    CHECK(doc["config"]["default_userdata"].text == core::WideToUtf8(defaultDir.wstring()));
}

TEST_CASE("cli: futurama.exe runs the tool modes without starting the game (end to end)") {
    const fs::path exe = ExeDir() / "futurama.exe";
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec) || !fs::is_regular_file(ExeDir() / "futurama_host.dll", ec)) {
        MESSAGE("skipped: futurama.exe / futurama_host.dll not next to fut_tests.exe");
        return;
    }
    const fs::path dir = TempDir(L"cli e2e (En,Fr,De) dir");
    // Tool modes never touch the user data: point %LOCALAPPDATA% (inherited by the child) at an empty directory.
    const fs::path appData = dir / "appdata";
    fs::create_directories(appData);
    const ScopedEnv env(L"LOCALAPPDATA", appData.wstring());
    const std::wstring exeQuoted = L"\"" + exe.wstring() + L"\" ";

    CHECK(RunProcess(exeQuoted + Quoted(L"--print-config-schema", dir / "schema.json")) == 0);
    const testjson::Value schema = ParseFile(dir / "schema.json");
    CHECK(schema["schema_version"].number == 1.0);
    CHECK(schema["config"]["file"].text == core::WideToUtf8((appData / "FuturamaPC" / "futurama.ini").wstring()));

    CHECK(RunProcess(exeQuoted + Quoted(L"--query-gpu", dir / "gpu.json")) == 0);
    CHECK(ParseFile(dir / "gpu.json")["msaa"][0].number == 1.0);

    CHECK(RunProcess(exeQuoted + Quoted(L"--print-paths", dir / "paths.json")) == 0);
    CHECK(ParseFile(dir / "paths.json")["mode"].text == "default");

    CHECK(RunProcess(exeQuoted + L"--print-config-schema") == core::cli::kToolExitUsage);

    WriteBytes(dir / "junk.iso", std::vector<std::uint8_t>(4096, 0x42));
    CHECK(RunProcess(exeQuoted + Quoted(L"--check-iso", dir / "junk.iso") + L" " +
                     Quoted(L"--result", dir / "junk.json")) == core::cli::kToolExitNotSupported);
    CHECK(ParseFile(dir / "junk.json")["reason_code"].text == "no_xdvdfs");

    const fs::path iso = ExistingFile(L"FUT_TEST_ISO", kDefaultIso);
    if (!iso.empty()) {
        CHECK(RunProcess(exeQuoted + Quoted(L"--check-iso", iso) + L" " + Quoted(L"--result", dir / "iso.json")) == 0);
        const testjson::Value r = ParseFile(dir / "iso.json");
        CHECK(r["supported"].boolean);
        CHECK(r["version"].text == "retail_eu");
    }
    CHECK_FALSE(fs::exists(appData / "FuturamaPC", ec));  // no log, no user data
}
