// M3b game timing (xapi/game_timing.*): [game] tick_rate, the 60 Hz patch table of Game::Init 0xA26A0
// (analysis/loader/timing60.md 2.3/2.4), the patch engine on a fake image, the read-back expectations and
// the steps-per-frame histogram. The XBE file is optional: with orig/retail_eu/default.xbe (or FUT_TEST_XBE)
// every row is also compared with the file.
#include "doctest.h"

#include "core/config.h"
#include "core/guest.h"
#include "shared/xbe_format.h"
#include "xapi/game_timing.h"

#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---- helpers ------------------------------------------------------------------------------------------

std::vector<std::uint8_t> FromHex(const char* hex) {
    std::vector<std::uint8_t> out;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (const char* p = hex; *p != '\0';) {
        if (nibble(*p) < 0) {
            ++p;
            continue;
        }
        const int hi = nibble(p[0]);
        const int lo = nibble(p[1]);
        REQUIRE(lo >= 0);
        out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        p += 2;
    }
    return out;
}

std::vector<std::uint8_t> Bytes(const std::array<std::uint8_t, xapi::kMaxPatchBytes>& a, std::uint32_t n) {
    return std::vector<std::uint8_t>(a.begin(), a.begin() + n);
}

const xapi::TimingPatch* FindRow(std::span<const xapi::TimingPatch> rows, guest::GuestAddr address) {
    for (const auto& r : rows) {
        if (r.address == address) {
            return &r;
        }
    }
    return nullptr;
}

std::filesystem::path TestIni(const char* name, const std::string& text) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("fut_timing_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream(file, std::ios::binary) << text;
    return file;
}

bool LoadIni(const std::filesystem::path& ini, core::Config& cfg, std::string& error, const std::wstring& extra = L"") {
    const std::wstring cmd = L"futurama.exe \"--config=" + ini.wstring() + L"\"" + extra;
    return core::LoadConfig(cmd.c_str(), L"C:\\nowhere\\futurama.exe", cfg, error);
}

// Image as the guest sees it: section contents at their virtual addresses (XBE file, if present).
struct XbeFile {
    std::vector<std::uint8_t> bytes;
    std::vector<xbe::SectionHeader> sections;

    static XbeFile Load() {
        XbeFile x;
        wchar_t buf[1024];
        const DWORD n = GetEnvironmentVariableW(L"FUT_TEST_XBE", buf, static_cast<DWORD>(std::size(buf)));
        const std::filesystem::path path = n > 0 && n < std::size(buf)
                                               ? std::filesystem::path(buf)
                                               : std::filesystem::path(L"D:\\FuturamaDecomp\\orig\\retail_eu\\default.xbe");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            return x;
        }
        std::ifstream in(path, std::ios::binary);
        x.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (x.bytes.size() < sizeof(xbe::ImageHeader)) {
            x.bytes.clear();
            return x;
        }
        xbe::ImageHeader h{};
        std::memcpy(&h, x.bytes.data(), sizeof(h));
        for (std::uint32_t i = 0; i < h.NumberOfSections; ++i) {
            xbe::SectionHeader s{};
            const std::uint32_t off = h.SectionHeadersAddress - h.BaseAddress + i * sizeof(s);
            if (off + sizeof(s) > x.bytes.size()) {
                break;
            }
            std::memcpy(&s, x.bytes.data() + off, sizeof(s));
            x.sections.push_back(s);
        }
        return x;
    }
    bool Read(guest::GuestAddr va, void* out, std::uint32_t size) const {
        for (const auto& s : sections) {
            if (va >= s.VirtualAddress && va + size <= s.VirtualAddress + s.RawSize &&
                s.RawAddress + (va - s.VirtualAddress) + size <= bytes.size()) {
                std::memcpy(out, bytes.data() + s.RawAddress + (va - s.VirtualAddress), size);
                return true;
            }
        }
        return false;
    }
};

// Fake image for the patch engine: byte map for the current image and one for the file.
class FakeImage final : public xapi::PatchMemory {
public:
    std::map<guest::GuestAddr, std::uint8_t> current, file;
    std::uint32_t writes = 0;

    void Put(std::map<guest::GuestAddr, std::uint8_t>& m, guest::GuestAddr va, const std::uint8_t* b, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) {
            m[va + i] = b[i];
        }
    }
    // Both maps hold the original bytes of every row.
    void LoadOriginals(std::span<const xapi::TimingPatch> rows) {
        for (const auto& r : rows) {
            Put(current, r.address, r.original.data(), r.length);
            Put(file, r.address, r.original.data(), r.length);
        }
    }
    static bool Get(const std::map<guest::GuestAddr, std::uint8_t>& m, guest::GuestAddr va, void* out,
                    std::uint32_t n) noexcept {
        auto* dst = static_cast<std::uint8_t*>(out);
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto it = m.find(va + i);
            if (it == m.end()) {
                return false;
            }
            dst[i] = it->second;
        }
        return true;
    }
    bool ReadCurrent(guest::GuestAddr va, void* out, std::uint32_t size) noexcept override {
        return Get(current, va, out, size);
    }
    bool ReadOriginal(guest::GuestAddr va, void* out, std::uint32_t size) noexcept override {
        return Get(file, va, out, size);
    }
    bool Write(guest::GuestAddr va, const void* data, std::uint32_t size) noexcept override {
        ++writes;
        for (std::uint32_t i = 0; i < size; ++i) {
            current[va + i] = static_cast<const std::uint8_t*>(data)[i];
        }
        return true;
    }
};

// Tiny interpreter for exactly the instructions of the time base block of Game::Init (timing60.md 2.1).
struct X87Block {
    std::map<guest::GuestAddr, std::uint32_t> mem;  // dword-addressed memory (byte stores keep the low byte)
    std::vector<float> st;                          // st[back] = st0
    std::uint32_t eax = 0, ebx = 1, ecx = 0;
    std::string error;

    static std::uint32_t U32(const std::uint8_t* p) {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    float Pop() {
        const float v = st.back();
        st.pop_back();
        return v;
    }
    // Runs the byte sequence; false (with `error`) on an unknown instruction.
    bool Run(const std::vector<std::uint8_t>& code) {
        std::size_t i = 0;
        while (i < code.size()) {
            const std::uint8_t* p = code.data() + i;
            const std::size_t left = code.size() - i;
            if (left >= 6 && p[0] == 0xD9 && p[1] == 0x05) {  // fld m32
                st.push_back(std::bit_cast<float>(mem[U32(p + 2)]));
                i += 6;
            } else if (left >= 6 && p[0] == 0xD9 && p[1] == 0x15) {  // fst m32
                mem[U32(p + 2)] = std::bit_cast<std::uint32_t>(st.back());
                i += 6;
            } else if (left >= 6 && p[0] == 0xD9 && p[1] == 0x1D) {  // fstp m32
                mem[U32(p + 2)] = std::bit_cast<std::uint32_t>(Pop());
                i += 6;
            } else if (left >= 10 && p[0] == 0xC7 && p[1] == 0x05) {  // mov m32, imm32
                mem[U32(p + 2)] = U32(p + 6);
                i += 10;
            } else if (left >= 5 && p[0] == 0xA1) {  // mov eax, [m32]
                eax = mem[U32(p + 1)];
                i += 5;
            } else if (left >= 5 && p[0] == 0xA3) {  // mov [m32], eax
                mem[U32(p + 1)] = eax;
                i += 5;
            } else if (left >= 6 && p[0] == 0x8A && p[1] == 0x0D) {  // mov cl, [m32]
                ecx = (ecx & ~0xFFu) | (mem[U32(p + 2)] & 0xFFu);
                i += 6;
            } else if (left >= 6 && p[0] == 0x88 && p[1] == 0x0D) {  // mov [m32], cl
                mem[U32(p + 2)] = ecx & 0xFFu;
                i += 6;
            } else if (left >= 6 && p[0] == 0x88 && p[1] == 0x1D) {  // mov [m32], bl
                mem[U32(p + 2)] = ebx & 0xFFu;
                i += 6;
            } else if (left >= 2 && p[0] == 0xD9 && p[1] == 0xC9) {  // fxch st(1)
                std::swap(st[st.size() - 1], st[st.size() - 2]);
                i += 2;
            } else if (left >= 2 && p[0] == 0x8B && p[1] == 0xCE) {  // mov ecx, esi (no effect here)
                i += 2;
            } else if (left >= 2 && p[0] == 0xEB) {  // jmp to the tail (the caller appends the tail)
                i += 2;
            } else {
                error = "unknown instruction byte " + std::to_string(p[0]) + " at offset " + std::to_string(i);
                return false;
            }
        }
        return true;
    }
    xapi::TimingGlobals Globals() {
        xapi::TimingGlobals g;
        g.fps = mem[guest::kGameFpsVar];
        g.dt = mem[guest::kGameFrameTimeVar];
        g.dtMin = mem[guest::kGameDtMinVar];
        g.dtMax = mem[guest::kGameDtMaxVar];
        g.fpsCopy1 = mem[guest::kGameFpsCopy1Var];
        g.fpsCopy2 = mem[guest::kGameFpsCopy2Var];
        g.frameTime = mem[guest::kGameVideoFrameTimeVar];
        g.fixedStep = mem[guest::kGameFixedStepVar];
        g.videoStandard = 0;
        const std::uint32_t tag = mem[guest::kGameMovieTagVar];
        std::memcpy(g.movieTag.data(), &tag, 4);
        return g;
    }
};

// "NTSC" string and its terminator read by the NTSC branch (`mov eax,[0x2A1864]`, `mov cl,[0x2A1868]`,
// timing60.md 2.1); only the model below needs them, the loader never touches them.
constexpr guest::GuestAddr kNtscTagString = 0x002A1864;
constexpr guest::GuestAddr kNtscTagTerminator = 0x002A1868;

// Machine state before the block: the .rdata constants and the NTSC tag string.
X87Block FreshBlock() {
    X87Block m;
    m.mem[guest::kRdataFloat50] = 0x42480000;
    m.mem[guest::kRdataFloat0p02] = 0x3CA3D70A;
    m.mem[guest::kRdataFloat60] = 0x42700000;
    m.mem[guest::kRdataFloatOneSixtieth] = 0x3C888889;
    m.mem[kNtscTagString] = 0x4353544E;  // "NTSC"
    m.mem[kNtscTagTerminator] = 0;
    return m;
}

// Literal listings of analysis/loader/timing60.md 2.1 (independent of the table in game_timing.cpp).
constexpr const char* kPalBranchOriginal =
    "d905ac552900 c7056c7b2e0000004842 d915607a2e00 c7055c7a2e000ad7a33c d905c0422900 c705687b2e008542213d "
    "c705707b2e0050414c00";
constexpr const char* kNtscBranch =
    "d905cc422900 a164182a00 8a0d68182a00 d915607a2e00 d90590e32900 c7056c7b2e0000007042 c7055c7a2e008988883c "
    "c705687b2e008a4b063d a3707b2e00 880d747b2e00 eb3a";
constexpr const char* kTail = "d915587a2e00 881d4c672d00 d9c9 8bce d91d44672d00 d91d48672d00";
constexpr guest::GuestAddr kTailEnd = 0x000A276A;

// Bytes of the table (patched or original) from 0xA2714 to the end of the tail; rows must be contiguous.
std::vector<std::uint8_t> PalBranchFromTable(bool patched, std::string& gap) {
    std::map<guest::GuestAddr, const xapi::TimingPatch*> rows;
    for (const auto& r : xapi::TimingPatches(xapi::kTickRateNtsc)) {
        rows[r.address] = &r;
    }
    for (const auto& r : xapi::TimingReferenceRows()) {
        rows[r.address] = &r;
    }
    std::vector<std::uint8_t> out;
    guest::GuestAddr at = guest::kGameInitPalFpsLoad;
    while (at < kTailEnd) {
        const auto it = rows.find(at);
        if (it == rows.end()) {
            gap = "no table row at 0x" + std::to_string(at);
            return {};
        }
        const xapi::TimingPatch& r = *it->second;
        const auto& b = patched ? r.patched : r.original;
        out.insert(out.end(), b.begin(), b.begin() + r.length);
        at += r.length;
    }
    return out;
}

}  // namespace

// ---- configuration ----------------------------------------------------------------------------------------

TEST_CASE("timing: [game] tick_rate is 60 or 50 (default 60), anything else is a configuration error") {
    core::Config cfg;
    std::string error;
    const auto ini = TestIni("plain.ini", "[debug]\nselftest = 0\n");
    REQUIRE_MESSAGE(LoadIni(ini, cfg, error), error);
    CHECK(xapi::ConfiguredTickRate(cfg) == 60u);
    for (const wchar_t* good : {L" --set=game.tick_rate=60", L" --set=game.tick_rate=50"}) {
        CAPTURE(core::WideToUtf8(good));
        REQUIRE_MESSAGE(LoadIni(ini, cfg, error, good), error);
        CHECK(xapi::ConfiguredTickRate(cfg) == (std::wstring(good).ends_with(L"60") ? 60u : 50u));
    }
    REQUIRE_MESSAGE(LoadIni(TestIni("ini50.ini", "[game]\ntick_rate = 50   ; PAL comparison\n"), cfg, error), error);
    CHECK(xapi::ConfiguredTickRate(cfg) == 50u);
    for (const wchar_t* bad : {L" --set=game.tick_rate=55", L" --set=game.tick_rate=0", L" --set=game.tick_rate=120",
                               L" --set=game.tick_rate=60hz", L" --set=game.tick_rate=0x3C", L" --set=game.tick_rate=",
                               L" --set=game.tick_rate=060", L" --set=game.tickrate=60"}) {
        CAPTURE(core::WideToUtf8(bad));
        CHECK_FALSE(LoadIni(ini, cfg, error, bad));
        CHECK(error.find("tick") != std::string::npos);
    }
    REQUIRE(core::ConfigKeyChoices("game", "tick_rate") != nullptr);
    CHECK(std::string(core::ConfigKeyChoices("game", "tick_rate")) == "60|50");
}

TEST_CASE("timing: ParseTickRate accepts exactly the choices of [game] tick_rate") {
    CHECK(xapi::ParseTickRate("60") == 60u);
    CHECK(xapi::ParseTickRate(" 50\t") == 50u);
    CHECK(xapi::ParseTickRate("") == 0u);
    CHECK(xapi::ParseTickRate("59.94") == 0u);
    CHECK(xapi::ParseTickRate("6O") == 0u);
    CHECK(xapi::ParseTickRate("100") == 0u);
    // Every word of the config choice list parses, and nothing else is a valid rate.
    const std::string choices = core::ConfigKeyChoices("game", "tick_rate");
    std::size_t start = 0;
    while (start <= choices.size()) {
        std::size_t bar = choices.find('|', start);
        bar = bar == std::string::npos ? choices.size() : bar;
        CHECK(xapi::ParseTickRate(choices.substr(start, bar - start)) != 0u);
        start = bar + 1;
    }
    CHECK(xapi::kDefaultTickRate == 60u);
}

// ---- patch table --------------------------------------------------------------------------------------------

TEST_CASE("timing: the 60 Hz rows are the patch table of timing60.md 2.3/2.4, 50 Hz patches nothing") {
    struct Expected {
        guest::GuestAddr address;
        const char* original;
        const char* patched;
        xapi::TimingPatchKind kind;
    };
    const Expected expected[] = {
        {0x000A2714, "d905ac552900", "d905cc422900", xapi::TimingPatchKind::Code},
        {0x000A271A, "c7056c7b2e0000004842", "c7056c7b2e0000007042", xapi::TimingPatchKind::Code},
        {0x000A272A, "c7055c7a2e000ad7a33c", "c7055c7a2e008988883c", xapi::TimingPatchKind::Code},
        {0x000A2734, "d905c0422900", "d90590e32900", xapi::TimingPatchKind::Code},
        {0x000A273A, "c705687b2e008542213d", "c705687b2e008a4b063d", xapi::TimingPatchKind::Code},
        {0x002D6744, "00004842", "00007042", xapi::TimingPatchKind::Data},
        {0x002D6748, "0ad7a33c", "8988883c", xapi::TimingPatchKind::Data},
    };
    const auto rows = xapi::TimingPatches(60);
    REQUIRE(rows.size() == std::size(expected));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CAPTURE(rows[i].name);
        CHECK(rows[i].address == expected[i].address);
        CHECK(rows[i].kind == expected[i].kind);
        CHECK(Bytes(rows[i].original, rows[i].length) == FromHex(expected[i].original));
        CHECK(Bytes(rows[i].patched, rows[i].length) == FromHex(expected[i].patched));
    }
    CHECK(xapi::TimingPatches(50).empty());
    CHECK(xapi::TimingPatches(0).empty());
    // The tag store 0xA2744 ("PAL") is a reference row, never a patch row; so is the CApp+0xE8 branch.
    CHECK(FindRow(rows, guest::kGameInitPalMovieTagStore) == nullptr);
    const xapi::TimingPatch* tag = FindRow(xapi::TimingReferenceRows(), guest::kGameInitPalMovieTagStore);
    REQUIRE(tag != nullptr);
    CHECK(Bytes(tag->original, tag->length) == FromHex("c705707b2e0050414c00"));
    const xapi::TimingPatch* branch = FindRow(xapi::TimingReferenceRows(), guest::kGameInitVideoStandardBranch);
    REQUIRE(branch != nullptr);
    CHECK(Bytes(branch->original, branch->length) == FromHex("8b86e8000000"));
    // The new operands are the NTSC branch's own constants (its fld instructions are reference rows).
    const xapi::TimingPatch* ntscFps = FindRow(xapi::TimingReferenceRows(), guest::kGameInitNtscFpsLoad);
    const xapi::TimingPatch* ntscDt = FindRow(xapi::TimingReferenceRows(), guest::kGameInitNtscDtLoad);
    REQUIRE(ntscFps != nullptr);
    REQUIRE(ntscDt != nullptr);
    CHECK(Bytes(ntscFps->original, 6) == Bytes(FindRow(rows, guest::kGameInitPalFpsLoad)->patched, 6));
    CHECK(Bytes(ntscDt->original, 6) == Bytes(FindRow(rows, guest::kGameInitPalDtLoad)->patched, 6));
}

TEST_CASE("timing: the tables are structurally valid and lie where their kind belongs") {
    std::string error;
    CHECK_MESSAGE(xapi::ValidateTimingPatchTable(xapi::TimingPatches(60), xapi::TimingReferenceRows(), error), error);
    CHECK_MESSAGE(xapi::ValidateTimingPatchTable(xapi::TimingPatches(50), xapi::TimingReferenceRows(), error), error);
    const guest::SectionLayout& text = guest::kSections[0];
    const guest::SectionLayout& rdata = guest::kSections[5];
    const guest::SectionLayout& data = guest::kSections[6];
    REQUIRE(std::string(rdata.name) == ".rdata");
    REQUIRE(std::string(data.name) == ".data");
    auto inRaw = [](const guest::SectionLayout& s, guest::GuestAddr a, std::uint32_t n) {
        return a >= s.va && a + n <= s.va + s.rawSize;  // file-backed part (the file holds the original)
    };
    for (const auto& r : xapi::TimingPatches(60)) {
        CAPTURE(r.name);
        if (r.kind == xapi::TimingPatchKind::Code) {
            CHECK(inRaw(text, r.address, r.length));
            const guest::FunctionInfo* f = guest::FindFunctionContaining(r.address);
            REQUIRE(f != nullptr);
            CHECK(f->address == guest::kGameInit);
            // Outside the bytes the Game::Init probe steals.
            CHECK(r.address >= guest::kGameInit + 6);
        } else {
            CHECK(inRaw(data, r.address, r.length));
        }
    }
    for (const auto& r : xapi::TimingReferenceRows()) {
        CAPTURE(r.name);
        CHECK((inRaw(text, r.address, r.length) || inRaw(rdata, r.address, r.length)));
    }
}

TEST_CASE("timing: ValidateTimingPatchTable rejects malformed rows") {
    const xapi::TimingPatch good = xapi::TimingPatches(60)[0];  // fld operand row
    const xapi::TimingPatch movRow = xapi::TimingPatches(60)[1];
    std::string error;
    {
        xapi::TimingPatch p = good;
        p.patched[1] = 0x15;  // opcode change (fld -> fst)
        CHECK_FALSE(xapi::ValidateTimingPatchTable({&p, 1}, {}, error));
        CHECK(error.find("outside the operand") != std::string::npos);
    }
    {
        xapi::TimingPatch p = movRow;
        p.patched[2] ^= 0x01;  // destination address of mov m32, imm32
        CHECK_FALSE(xapi::ValidateTimingPatchTable({&p, 1}, {}, error));
    }
    {
        xapi::TimingPatch p = good;
        p.patched = p.original;  // no change
        CHECK_FALSE(xapi::ValidateTimingPatchTable({&p, 1}, {}, error));
    }
    {
        xapi::TimingPatch p = good;
        p.length = 0;
        CHECK_FALSE(xapi::ValidateTimingPatchTable({&p, 1}, {}, error));
    }
    {
        xapi::TimingPatch p = good;
        p.original[0] = 0x90;  // unsupported instruction
        p.patched[0] = 0x90;
        CHECK_FALSE(xapi::ValidateTimingPatchTable({&p, 1}, {}, error));
    }
    {
        xapi::TimingPatch p = xapi::TimingPatches(60)[5];  // data row
        p.length = 3;
        CHECK_FALSE(xapi::ValidateTimingPatchTable({&p, 1}, {}, error));
    }
    {
        xapi::TimingPatch ref = xapi::TimingReferenceRows()[0];
        ref.patched[0] ^= 0xFF;  // a reference row that changes bytes
        CHECK_FALSE(xapi::ValidateTimingPatchTable({}, {&ref, 1}, error));
        xapi::TimingPatch asPatch = good;
        asPatch.kind = xapi::TimingPatchKind::Reference;  // kind does not fit the patch table
        CHECK_FALSE(xapi::ValidateTimingPatchTable({&asPatch, 1}, {}, error));
    }
    {
        xapi::TimingPatch rows[2] = {good, good};
        rows[1].address += 3;  // overlap
        CHECK_FALSE(xapi::ValidateTimingPatchTable(rows, {}, error));
        CHECK(error.find("overlap") != std::string::npos);
    }
    {
        REQUIRE_FALSE(guest::FsPatchSites().empty());
        xapi::TimingPatch p = good;
        p.address = guest::FsPatchSites()[0].address;
        CHECK_FALSE(xapi::ValidateTimingPatchTable({&p, 1}, {}, error));
        CHECK(error.find("fs patch site") != std::string::npos);
    }
}

TEST_CASE("timing: the patched PAL branch computes exactly the NTSC time base with the PAL tag (x87 model)") {
    // The table covers 0xA2714..0xA276A without gaps; its originals equal the listing of timing60.md 2.1.
    std::string gap;
    const std::vector<std::uint8_t> palOriginal = PalBranchFromTable(false, gap);
    REQUIRE_MESSAGE(!palOriginal.empty(), gap);
    std::vector<std::uint8_t> literal = FromHex(kPalBranchOriginal);
    const std::vector<std::uint8_t> tail = FromHex(kTail);
    literal.insert(literal.end(), tail.begin(), tail.end());
    CHECK(palOriginal == literal);
    const std::vector<std::uint8_t> palPatched = PalBranchFromTable(true, gap);
    REQUIRE(palPatched.size() == palOriginal.size());

    X87Block pal = FreshBlock();
    REQUIRE_MESSAGE(pal.Run(palOriginal), pal.error);
    X87Block patched = FreshBlock();
    REQUIRE_MESSAGE(patched.Run(palPatched), patched.error);
    std::vector<std::uint8_t> ntscCode = FromHex(kNtscBranch);
    ntscCode.insert(ntscCode.end(), tail.begin(), tail.end());
    X87Block ntsc = FreshBlock();
    REQUIRE_MESSAGE(ntsc.Run(ntscCode), ntsc.error);
    CHECK(pal.st.empty());
    CHECK(patched.st.empty());
    CHECK(ntsc.st.empty());

    CHECK(xapi::DiffTimingGlobals(pal.Globals(), xapi::ExpectedTimingGlobals(50)).empty());
    CHECK(xapi::DiffTimingGlobals(patched.Globals(), xapi::ExpectedTimingGlobals(60)).empty());
    // Every time global equals the NTSC branch; only the movie tag differs ("PAL" vs "NTSC").
    const guest::GuestAddr globals[] = {guest::kGameFpsVar,      guest::kGameFrameTimeVar, guest::kGameDtMinVar,
                                        guest::kGameDtMaxVar,    guest::kGameFpsCopy1Var,  guest::kGameFpsCopy2Var,
                                        guest::kGameVideoFrameTimeVar, guest::kGameFixedStepVar};
    for (const guest::GuestAddr g : globals) {
        CAPTURE(g);
        CHECK(patched.mem[g] == ntsc.mem[g]);
    }
    CHECK(std::bit_cast<float>(patched.mem[guest::kGameFpsVar]) == 60.0f);
    CHECK(patched.mem[guest::kGameFrameTimeVar] == 0x3C888889u);
    CHECK(patched.mem[guest::kGameMovieTagVar] == 0x004C4150u);  // "PAL"
    CHECK(ntsc.mem[guest::kGameMovieTagVar] == 0x4353544Eu);     // "NTSC"
    // The shared .rdata constants are untouched by construction (only operands change).
    CHECK(patched.mem[guest::kRdataFloat50] == 0x42480000u);
    CHECK(patched.mem[guest::kRdataFloat0p02] == 0x3CA3D70Au);
}

TEST_CASE("timing: the rows match the XBE file (skipped without orig/retail_eu/default.xbe)") {
    const XbeFile x = XbeFile::Load();
    if (x.bytes.empty()) {
        MESSAGE("XBE not found (set FUT_TEST_XBE) - file comparison skipped");
        return;
    }
    REQUIRE(x.bytes.size() == guest::kXbeFileSize);
    for (const auto& rows : {xapi::TimingPatches(60), xapi::TimingReferenceRows()}) {
        for (const auto& r : rows) {
            CAPTURE(r.name);
            std::uint8_t file[xapi::kMaxPatchBytes] = {};
            REQUIRE(x.Read(r.address, file, r.length));
            CHECK(std::vector<std::uint8_t>(file, file + r.length) == Bytes(r.original, r.length));
        }
    }
    // The NTSC tag string the NTSC branch would use (not used by the patch, model input above).
    std::uint32_t ntsc = 0;
    REQUIRE(x.Read(kNtscTagString, &ntsc, 4));
    CHECK(ntsc == 0x4353544Eu);
    // Unchanged shared constants.
    std::uint32_t v = 0;
    REQUIRE(x.Read(guest::kRdataFloat50, &v, 4));
    CHECK(v == 0x42480000u);
    REQUIRE(x.Read(guest::kRdataFloat0p02, &v, 4));
    CHECK(v == 0x3CA3D70Au);
    // probe:Game::FixedStep prologue.
    std::uint8_t prologue[7] = {};
    REQUIRE(x.Read(guest::kGameFixedStepHelper, prologue, sizeof(prologue)));
    CHECK(std::vector<std::uint8_t>(prologue, prologue + 7) == FromHex("55 8B 2D A4 6F 2E 00"));
    // Game::Frame calls it at kGameFrameFixedStepCall: E8 rel32.
    std::uint8_t call[5] = {};
    REQUIRE(x.Read(guest::kGameFrameFixedStepCall, call, sizeof(call)));
    CHECK(call[0] == 0xE8);
    std::int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    CHECK(guest::kGameFrameFixedStepCall + 5 + static_cast<std::uint32_t>(rel) == guest::kGameFixedStepHelper);
}

// ---- patch engine --------------------------------------------------------------------------------------------

TEST_CASE("timing: ApplyTimingPatches writes only after every row matched image and file") {
    const auto patches = xapi::TimingPatches(60);
    const auto refs = xapi::TimingReferenceRows();
    std::string error;
    SUBCASE("success: patched bytes in the image, file unchanged, references untouched") {
        FakeImage img;
        img.LoadOriginals(patches);
        img.LoadOriginals(refs);
        REQUIRE_MESSAGE(xapi::ApplyTimingPatches(patches, refs, img, error), error);
        CHECK(img.writes == patches.size());
        for (const auto& r : patches) {
            std::uint8_t cur[xapi::kMaxPatchBytes] = {}, file[xapi::kMaxPatchBytes] = {};
            REQUIRE(img.ReadCurrent(r.address, cur, r.length));
            REQUIRE(img.ReadOriginal(r.address, file, r.length));
            CHECK(std::vector<std::uint8_t>(cur, cur + r.length) == Bytes(r.patched, r.length));
            CHECK(std::vector<std::uint8_t>(file, file + r.length) == Bytes(r.original, r.length));
        }
        // A second application is refused as "already patched" and writes nothing.
        img.writes = 0;
        CHECK_FALSE(xapi::ApplyTimingPatches(patches, refs, img, error));
        CHECK(error.find("already patched") != std::string::npos);
        CHECK(img.writes == 0u);
    }
    SUBCASE("a different XBE build is refused before any write") {
        FakeImage img;
        img.LoadOriginals(patches);
        img.LoadOriginals(refs);
        img.file[patches.back().address] ^= 0xFF;  // last row differs in the file only
        CHECK_FALSE(xapi::ApplyTimingPatches(patches, refs, img, error));
        CHECK(error.find("XBE file") != std::string::npos);
        CHECK(img.writes == 0u);
    }
    SUBCASE("a changed reference row (e.g. a hook on the tail) is refused before any write") {
        FakeImage img;
        img.LoadOriginals(patches);
        img.LoadOriginals(refs);
        img.current[guest::kGameInitTimeBaseTail] = 0xE9;  // jmp written over the tail
        CHECK_FALSE(xapi::ApplyTimingPatches(patches, refs, img, error));
        CHECK(error.find("another patch or hook") != std::string::npos);
        CHECK(img.writes == 0u);
    }
    SUBCASE("unreadable bytes are refused") {
        FakeImage img;
        img.LoadOriginals(patches);  // references missing from both maps
        CHECK_FALSE(xapi::ApplyTimingPatches(patches, refs, img, error));
        CHECK(img.writes == 0u);
    }
    SUBCASE("an empty table (50 Hz) writes nothing and succeeds") {
        FakeImage img;
        img.LoadOriginals(refs);
        CHECK_MESSAGE(xapi::ApplyTimingPatches(xapi::TimingPatches(50), refs, img, error), error);
        CHECK(img.writes == 0u);
    }
}

// ---- read-back expectations -------------------------------------------------------------------------------------

TEST_CASE("timing: expected globals after Game::Init (60 Hz = NTSC values with PAL tag, 50 Hz = PAL)") {
    const xapi::TimingGlobals e60 = xapi::ExpectedTimingGlobals(60);
    CHECK(std::bit_cast<float>(e60.fps) == 60.0f);
    CHECK(std::bit_cast<float>(e60.dt) == 1.0f / 60.0f);
    CHECK(e60.dtMin == e60.dt);
    CHECK(e60.dtMax == e60.dt);
    CHECK(e60.fpsCopy1 == e60.fps);
    CHECK(e60.fpsCopy2 == e60.fps);
    CHECK(e60.frameTime == 0x3D064B8Au);
    CHECK(e60.fixedStep == 1u);
    CHECK(e60.videoStandard == 0u);
    CHECK(std::string(e60.movieTag.data()) == "PAL");
    const xapi::TimingGlobals e50 = xapi::ExpectedTimingGlobals(50);
    CHECK(std::bit_cast<float>(e50.fps) == 50.0f);
    CHECK(std::bit_cast<float>(e50.dt) == 0.02f);
    CHECK(e50.frameTime == 0x3D214285u);
    CHECK(std::string(e50.movieTag.data()) == "PAL");
    CHECK(xapi::DiffTimingGlobals(e60, e60).empty());
    CHECK(xapi::DiffTimingGlobals(e60, e50) == "fps, dt, dt min, dt max, fps copy 0x2E7A60, fps copy 0x2E7B6C, 0x2E7B68");
    xapi::TimingGlobals ntscTag = e60;
    ntscTag.videoStandard = 1;
    ntscTag.movieTag = {'N', 'T', 'S', 'C', '\0', '\0', '\0', '\0'};
    CHECK(xapi::DiffTimingGlobals(ntscTag, e60) == "CApp+0xE8, movie tag");
}

TEST_CASE("timing: stage state is empty without the loader (unit test process)") {
    CHECK(xapi::ActiveTickRate() == 0u);
    CHECK(xapi::AppliedTimingPatches().empty());
    CHECK_FALSE(xapi::GetGameTimingReadBack().valid);
    const xapi::GameTickStats t = xapi::GetGameTickStats();
    CHECK(t.steps == 0u);
    CHECK(t.stepsPerSecond == 0.0);
    CHECK(xapi::ActiveFrameClock() == xapi::FrameClockMode::Realtime);  // the probe passes QPC through
    CHECK(xapi::GetFrameClockStats().clock.samples == 0u);
}

// ---- steps per frame ---------------------------------------------------------------------------------------------

TEST_CASE("timing: StepHistogram counts fixed steps per CApp::Frame, frames without a step as 0") {
    xapi::StepHistogram h;
    CHECK(h.Snapshot(10) == std::array<std::uint64_t, 4>{0, 0, 0, 0});  // no step yet: nothing counted
    // frame 1: 1 step, 2: 1, 3: none, 4: 2, 5: 1, 6: 6 (cap after a stall), 7: in progress with 1 step
    for (const std::uint64_t f : {1, 2, 4, 4, 5, 6, 6, 6, 6, 6, 6, 7}) {
        h.OnStep(f);
    }
    CHECK(h.MaxStepsPerFrame() == 6u);
    // frame 7 is still running (6 completed): it is not counted yet
    CHECK(h.Snapshot(6) == std::array<std::uint64_t, 4>{1, 3, 1, 1});
    // frame 7 completed, frames 8 and 9 without a step
    CHECK(h.Snapshot(9) == std::array<std::uint64_t, 4>{3, 4, 1, 1});
    // a step of an older frame number (never happens) is counted in the current frame
    h.OnStep(3);
    CHECK(h.Snapshot(7) == std::array<std::uint64_t, 4>{1, 3, 2, 1});
}

TEST_CASE("timing: probe:Game::FixedStep target is a hookable function start called from Game::Frame") {
    const guest::FunctionInfo* f = guest::FindFunctionStart(guest::kGameFixedStepHelper);
    REQUIRE(f != nullptr);
    CHECK(std::string(f->name) == "FUN_0002c890");
    CHECK(f->size >= 7u);
    for (const auto& site : guest::FsPatchSites()) {
        CHECK_FALSE((site.address < guest::kGameFixedStepHelper + 7 && guest::kGameFixedStepHelper < site.address + site.length));
    }
    const guest::FunctionInfo* frame = guest::FindFunctionContaining(guest::kGameFrameFixedStepCall);
    REQUIRE(frame != nullptr);
    CHECK(frame->address == guest::kGameFrame);
}

// ---- frame clock ([game] frame_clock, probe:Game::Timer) --------------------------------------------------------

namespace {

// Game::Frame's fixed-step accumulator as the binary computes it (timing60.md 6, pacing60.md 1.4):
// sec(t) = t * f32(1/QPF) in x87 precision, cap at f32(6 * dt), `while sec(elapsed) >= dt` (at most 100 steps)
// with elapsed -= trunc(QPF * dt), then last = now - elapsed.
struct GuestAccumulator {
    std::int64_t qpf;
    float dt;
    std::int64_t last;
    int Frame(std::int64_t now) {
        const double inv = static_cast<double>(static_cast<float>(1.0 / static_cast<double>(qpf)));
        std::int64_t elapsed = now - last;
        const float cap = dt * 6.0f;
        if (static_cast<double>(cap) < static_cast<double>(elapsed) * inv) {
            elapsed = xapi::GuestTimerTicks(qpf, cap);
        }
        const std::int64_t step = xapi::GuestTimerTicks(qpf, dt);
        int steps = 0;
        for (int budget = 100; budget > 0 && !(static_cast<double>(elapsed) * inv < static_cast<double>(dt));
             --budget) {
            ++steps;
            elapsed -= step;
        }
        last = now - elapsed;
        return steps;
    }
};

// Deterministic jitter in [-amplitude, +amplitude] (LCG).
struct Jitter {
    std::uint32_t state = 12345;
    std::int64_t Next(std::int64_t amplitude) {
        state = state * 1664525u + 1013904223u;
        return static_cast<std::int64_t>(state % static_cast<std::uint32_t>(2 * amplitude + 1)) - amplitude;
    }
};

// Frames presented at exactly `period` ticks (+ sampling jitter); the first frame follows a 0.2 s stall, so the
// guest's cap leaves the remainder at the step boundary (pacing60.md 2.6). Returns {frames with 1 step, others}.
std::pair<int, int> Simulate(bool lockstep, double period, std::int64_t jitterTicks, int frames) {
    constexpr std::int64_t kQpf = 10'000'000;
    GuestAccumulator guest{kQpf, 1.0f / 60.0f, 0};
    xapi::LockstepClock clock;
    Jitter jitter;
    const std::int64_t start = 50'000'000;
    guest.last = start - 2'000'000;  // stall before the first frame
    int one = 0, other = 0;
    for (int k = 0; k < frames; ++k) {
        const std::int64_t real = start + static_cast<std::int64_t>(k * period) + jitter.Next(jitterTicks);
        const std::int64_t now = lockstep ? clock.Advance(real, xapi::GuestTimerTicks(kQpf, guest.dt)) : real;
        const int steps = guest.Frame(now);
        if (k == 0) {
            continue;  // the stall frame itself (6 steps, cap)
        }
        (steps == 1 ? one : other) += 1;
    }
    return {one, other};
}

}  // namespace

TEST_CASE("timing: [game] frame_clock is lockstep (default) or realtime") {
    xapi::FrameClockMode mode = xapi::FrameClockMode::Realtime;
    CHECK(xapi::ParseFrameClock("lockstep", mode));
    CHECK(mode == xapi::FrameClockMode::Lockstep);
    CHECK(xapi::ParseFrameClock(" RealTime ", mode));
    CHECK(mode == xapi::FrameClockMode::Realtime);
    CHECK_FALSE(xapi::ParseFrameClock("display", mode));
    CHECK_FALSE(xapi::ParseFrameClock("", mode));
    core::Config cfg;
    std::string error;
    const auto ini = TestIni("clock.ini", "[debug]\nselftest = 0\n");
    REQUIRE_MESSAGE(LoadIni(ini, cfg, error), error);
    REQUIRE(xapi::ConfiguredFrameClock(cfg, mode));
    CHECK(mode == xapi::FrameClockMode::Lockstep);
    REQUIRE_MESSAGE(LoadIni(ini, cfg, error, L" --set=game.frame_clock=realtime"), error);
    REQUIRE(xapi::ConfiguredFrameClock(cfg, mode));
    CHECK(mode == xapi::FrameClockMode::Realtime);
    CHECK_FALSE(LoadIni(ini, cfg, error, L" --set=game.frame_clock=vsync"));
    CHECK(error.find("frame_clock") != std::string::npos);
    CHECK(std::string(core::ConfigKeyChoices("game", "frame_clock")) == "lockstep|realtime");
    CHECK(std::string(xapi::FrameClockName(xapi::FrameClockMode::Lockstep)) == "lockstep");
}

TEST_CASE("timing: GuestTimerTicks truncates QPF * dt like FUN_000D2B50") {
    CHECK(xapi::GuestTimerTicks(10'000'000, 1.0f / 60.0f) == 166666);   // 60.00024 Hz (timing60.md 6)
    CHECK(xapi::GuestTimerTicks(10'000'000, 0.02f) == 199999);          // PAL
    CHECK(xapi::GuestTimerTicks(10'000'000, (1.0f / 60.0f) * 6.0f) == 1'000'000);  // cap 6 dt
    CHECK(xapi::GuestTimerTicks(733'333'333, 1.0f / 60.0f) == 12'222'222);   // Xbox QPF (timing60.md 6)
    CHECK(xapi::GuestTimerTicks(0, 1.0f / 60.0f) == 0);
    CHECK(xapi::GuestTimerTicks(10'000'000, 0.0f) == 0);
}

TEST_CASE("timing: LockstepClock advances one step per frame and corrects only beyond 3/4 step") {
    constexpr std::int64_t P = 166666;
    xapi::LockstepClock c;
    CHECK(c.Advance(1'000'000, P) == 1'000'000);  // first sample: real time
    std::int64_t v = 1'000'000;
    // frames at the tick rate with +-5 ms jitter: always +1 step
    const std::int64_t jitter[] = {0, 50'000, -50'000, 40'000, -45'000, 10'000};
    for (int k = 1; k <= 6; ++k) {
        v += P;
        CHECK(c.Advance(1'000'000 + k * P + jitter[k - 1], P) == v);
    }
    CHECK(c.GetStats().single == 6u);
    // a dropped frame (2 periods): catch up by 2 steps
    v += 2 * P;
    CHECK(c.Advance(1'000'000 + 8 * P, P) == v);
    // a 1 s stall: catch up so that the phase is within (-1/4, 3/4] step
    const std::int64_t real = 1'000'000 + 8 * P + 10'000'000;
    const std::int64_t after = c.Advance(real, P);
    CHECK((after - v) % P == 0);
    CHECK(real - after <= P * 3 / 4);
    CHECK(real - after > -P / 4);
    CHECK(c.GetStats().catchUps == 2u);
    CHECK(c.GetStats().maxCatchUpSteps >= 60);
    // frames twice as fast as the tick rate: hold every other frame, real time is kept
    xapi::LockstepClock fast;
    fast.Advance(0, P);
    for (int k = 1; k <= 120; ++k) {
        const std::int64_t r = k * P / 2;
        const std::int64_t t = fast.Advance(r, P);
        CHECK(r - t <= P * 3 / 4);
        CHECK(r - t >= -P * 3 / 4);
    }
    CHECK(fast.GetStats().holds == 60u);
    CHECK(fast.GetStats().single == 60u);
}

TEST_CASE("timing: a slow drift (59.94 Hz display) gives one catch-up step without flip-flop") {
    constexpr std::int64_t P = 166666;
    const double period = 10'000'000.0 / 59.94;
    xapi::LockstepClock c;
    Jitter jitter;
    std::vector<int> corrections;
    for (int k = 0; k < 3000; ++k) {
        const std::int64_t real = static_cast<std::int64_t>(k * period) + jitter.Next(5'000);
        const auto before = c.GetStats();
        c.Advance(real, P);
        if (k > 0 && c.GetStats().single == before.single) {
            corrections.push_back(k);
        }
    }
    // The drift of 0.1 % crosses 3/4 step after ~750 frames, then one step per ~1000 frames.
    REQUIRE(corrections.size() >= 2u);
    CHECK(corrections.size() <= 4u);
    for (std::size_t i = 1; i < corrections.size(); ++i) {
        CHECK(corrections[i] - corrections[i - 1] > 900);  // never two corrections close together
    }
    CHECK(c.GetStats().holds == 0u);
}

TEST_CASE("timing: lockstep removes the 0/2-step frames of the guest accumulator at the step boundary") {
    const double period = 10'000'000.0 / 60.0;  // exact 60 Hz presentation (120 Hz display, sync interval 2)
    // Realtime: after the stall the remainder sits at the boundary and 0.3 ms sampling jitter decides
    // between 0, 1 and 2 steps (pacing60.md 2.6; timing60.md 6 models 11.6 % each).
    const auto realtime = Simulate(false, period, 3'000, 2000);
    CHECK(realtime.second > 200);
    // Lockstep: exactly one step per frame.
    const auto lockstep = Simulate(true, period, 3'000, 2000);
    CHECK(lockstep.first == 1999);
    CHECK(lockstep.second == 0);
    // Also with large jitter (+-5 ms, e.g. a busy streaming frame).
    const auto noisy = Simulate(true, period, 50'000, 2000);
    CHECK(noisy.second == 0);
}

TEST_CASE("timing: probe:Game::Timer target and the three Game::Frame samples") {
    const guest::FunctionInfo* f = guest::FindFunctionStart(guest::kGameTimer);
    REQUIRE(f != nullptr);
    CHECK(std::string(f->name) == "FUN_000d2b90");
    CHECK(f->size >= 9u);
    for (const guest::GuestAddr ret : {guest::kGameFrameClockSampleReturn, guest::kGameFrameRenderInitSampleReturn,
                                       guest::kGameFrameRenderSampleReturn}) {
        const guest::FunctionInfo* frame = guest::FindFunctionContaining(ret);
        REQUIRE(frame != nullptr);
        CHECK(frame->address == guest::kGameFrame);
    }
    const XbeFile x = XbeFile::Load();
    if (x.bytes.empty()) {
        return;
    }
    std::uint8_t prologue[9] = {};
    REQUIRE(x.Read(guest::kGameTimer, prologue, sizeof(prologue)));
    CHECK(std::vector<std::uint8_t>(prologue, prologue + 9) == FromHex("83 EC 08 8B 0D 4C B4 2E 00"));
    for (const guest::GuestAddr ret : {guest::kGameFrameClockSampleReturn, guest::kGameFrameRenderInitSampleReturn,
                                       guest::kGameFrameRenderSampleReturn}) {
        std::uint8_t call[5] = {};
        REQUIRE(x.Read(ret - 5, call, sizeof(call)));
        CHECK(call[0] == 0xE8);
        std::int32_t rel = 0;
        std::memcpy(&rel, call + 1, 4);
        CHECK(ret + static_cast<std::uint32_t>(rel) == guest::kGameTimer);
    }
}
