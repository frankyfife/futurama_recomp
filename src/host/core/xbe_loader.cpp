// XBE loading and mapping (design 1.4 steps 1-4 and 7). Owned by the core agent.
//
// Evidence: design 1.4 (section table, header checks), kernel.md 1 (thunk table 0x293560 after the
// retail XOR key), cpu.md 5/6 item 1 (every section's VirtualSize - RawSize tail must be zero and
// writable, $$XTIMAGE/.XTLID are mapped although not preloaded), launcher_probe.md (VirtualProtect of
// the image pages to RWX works; the placeholder spans 0x11000..0x31B000).
#include "core/xbe_loader.h"

#include "core/log.h"
#include "shared/xbe_format.h"
#include "vfs/vfs.h"

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace core {
namespace {

// The verified file. Allocated once and never freed: kernel HLE (XeLoadSection) and self-tests may
// read it until the process ends, independent of static destruction order.
std::vector<std::uint8_t>* g_xbe = nullptr;

struct MappedSection {
    guest::GuestAddr va;
    std::uint32_t virtualSize;
    std::uint32_t rawAddress;
    std::uint32_t rawSize;
};
MappedSection g_sections[guest::kXbeSectionCount] = {};
bool g_sectionsValid = false;

bool ReadWholeFile(const std::filesystem::path& path, std::vector<std::uint8_t>& out, std::string& error) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "cannot open (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != 0;
    if (!ok || size.QuadPart <= 0 || size.QuadPart > 64ll * 1024 * 1024) {
        CloseHandle(h);
        error = ok ? "implausible file size " + std::to_string(size.QuadPart) : "GetFileSizeEx failed";
        return false;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 20));
        if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) {
            ok = false;
            break;
        }
        done += got;
    }
    CloseHandle(h);
    if (!ok) {
        error = "read error (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

bool Sha1Hex(std::span<const std::uint8_t> data, std::string& hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::uint8_t digest[20] = {};
    bool ok =
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
        BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) >= 0 &&
        BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (alg != nullptr) {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    if (!ok) {
        return false;
    }
    char buf[41];
    for (int i = 0; i < 20; ++i) {
        std::snprintf(buf + 2 * i, 3, "%02x", digest[i]);
    }
    hex.assign(buf, 40);
    return true;
}

template <class T>
bool FileRead(std::span<const std::uint8_t> file, std::uint32_t offset, T& out) {
    if (offset > file.size() || file.size() - offset < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, file.data() + offset, sizeof(T));
    return true;
}

// Header address -> file offset (only the header area is addressable this way).
bool HeaderOffset(guest::GuestAddr va, std::uint32_t& offset) {
    if (va < guest::kXbeBase || va - guest::kXbeBase >= guest::kXbeSizeOfHeaders) {
        return false;
    }
    offset = va - guest::kXbeBase;
    return true;
}

std::string HeaderString(std::span<const std::uint8_t> file, guest::GuestAddr va) {
    std::uint32_t off = 0;
    if (!HeaderOffset(va, off)) {
        return {};
    }
    std::string s;
    while (off < guest::kXbeSizeOfHeaders && off < file.size() && file[off] != 0 && s.size() < 32) {
        s.push_back(static_cast<char>(file[off++]));
    }
    return s;
}

bool Fail(const char* what, const std::string& detail = {}) {
    FUT_LOGE(Core, "XBE check failed: %s%s%s", what, detail.empty() ? "" : " - ", detail.c_str());
    return false;
}

// Design 1.4 step 2 plus the section table of step 4.
bool CheckXbe(std::span<const std::uint8_t> file, MappedSection (&sections)[guest::kXbeSectionCount]) {
    char buf[160];
    xbe::ImageHeader h{};
    if (!FileRead(file, 0, h)) {
        return Fail("file shorter than the image header");
    }
    if (h.Magic != xbe::kMagic) {
        return Fail("magic is not XBEH");
    }
    const std::uint32_t entry = h.EntryPoint ^ xbe::kEntryXorRetail;
    const std::uint32_t thunks = h.KernelImageThunkAddress ^ xbe::kThunkXorRetail;
    struct Expect {
        const char* name;
        std::uint32_t actual;
        std::uint32_t expected;
    };
    const Expect expects[] = {
        {"BaseAddress", h.BaseAddress, guest::kXbeBase},
        {"SizeOfHeaders", h.SizeOfHeaders, guest::kXbeSizeOfHeaders},
        {"SizeOfImage", h.SizeOfImage, guest::kXbeSizeOfImage},
        {"SizeOfImageHeader", h.SizeOfImageHeader, sizeof(xbe::ImageHeader)},
        {"EntryPoint (retail XOR)", entry, guest::kEntryPoint},
        {"KernelImageThunkAddress (retail XOR)", thunks, guest::kKernelThunkTable},
        {"CertificateAddress", h.CertificateAddress, guest::kCertificate},
        {"NumberOfSections", h.NumberOfSections, guest::kXbeSectionCount},
        {"SectionHeadersAddress", h.SectionHeadersAddress, guest::kSectionHeaders},
        {"TlsAddress", h.TlsAddress, guest::kTlsDirectory},
    };
    for (const Expect& e : expects) {
        if (e.actual != e.expected) {
            std::snprintf(buf, sizeof(buf), "%s = 0x%08X, expected 0x%08X", e.name, e.actual, e.expected);
            return Fail("header field", buf);
        }
    }
    if (file.size() < h.SizeOfHeaders) {
        return Fail("file shorter than SizeOfHeaders");
    }
    for (std::uint32_t i = 0; i < guest::kXbeSectionCount; ++i) {
        const guest::SectionLayout& want = guest::kSections[i];
        std::uint32_t off = 0;
        xbe::SectionHeader s{};
        if (!HeaderOffset(guest::kSectionHeaders + i * sizeof(xbe::SectionHeader), off) ||
            !FileRead(file, off, s)) {
            return Fail("section header outside the header area");
        }
        const std::string name = HeaderString(file, s.SectionNameAddress);
        if (name != want.name || s.VirtualAddress != want.va || s.VirtualSize != want.virtualSize ||
            s.RawSize != want.rawSize) {
            std::snprintf(buf, sizeof(buf),
                          "#%u '%s' va 0x%08X vsize 0x%X raw 0x%X (expected '%s' 0x%08X 0x%X 0x%X)", i,
                          name.c_str(), s.VirtualAddress, s.VirtualSize, s.RawSize, want.name, want.va,
                          want.virtualSize, want.rawSize);
            return Fail("section table", buf);
        }
        if (s.RawSize > s.VirtualSize || s.RawAddress > file.size() ||
            file.size() - s.RawAddress < s.RawSize) {
            std::snprintf(buf, sizeof(buf), "section %s raw 0x%X+0x%X outside the file", want.name,
                          s.RawAddress, s.RawSize);
            return Fail("section raw data", buf);
        }
        if (s.VirtualAddress < guest::kXbeSectionsBegin ||
            s.VirtualAddress + s.VirtualSize > guest::kXbeSpaceEnd) {
            std::snprintf(buf, sizeof(buf), "section %s 0x%08X+0x%X outside 0x%08X..0x%08X", want.name,
                          s.VirtualAddress, s.VirtualSize, guest::kXbeSectionsBegin, guest::kXbeSpaceEnd);
            return Fail("section placement", buf);
        }
        sections[i] = MappedSection{s.VirtualAddress, s.VirtualSize, s.RawAddress, s.RawSize};
    }
    return true;
}

void LogCertificate(std::span<const std::uint8_t> file) {
    xbe::Certificate cert{};
    std::uint32_t off = 0;
    if (!HeaderOffset(guest::kCertificate, off) || !FileRead(file, off, cert)) {
        return;
    }
    std::string title;
    for (wchar_t c : cert.TitleName) {
        if (c == 0) {
            break;
        }
        title.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    FUT_LOGI(Core, "XBE certificate: title id 0x%08X '%s', allowed media 0x%X, region 0x%X, version 0x%X",
             cert.TitleId, title.c_str(), cert.AllowedMediaTypes, cert.GameRegion, cert.Version);
}

}  // namespace

bool StageLoadXbe(const Config& config) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    std::string source;
    if (!config.xbe.empty()) {
        source = WideToUtf8(config.xbe.wstring());
        if (!ReadWholeFile(config.xbe, bytes, error)) {
            FUT_LOGE(Core, "cannot read XBE %s: %s", source.c_str(), error.c_str());
            return false;
        }
    } else if (!config.iso.empty()) {
        source = WideToUtf8(config.iso.wstring()) + " :: default.xbe";
        try {
            if (!vfs::ReadIsoFile(config.iso, "default.xbe", bytes, error)) {
                FUT_LOGE(Core, "cannot read default.xbe from the disc image %s: %s",
                         WideToUtf8(config.iso.wstring()).c_str(), error.c_str());
                return false;
            }
        } catch (const std::exception& e) {
            FUT_LOGE(Core, "cannot read default.xbe from the disc image: exception %s", e.what());
            return false;
        }
    } else {
        FUT_LOGE(Core, "no XBE source: set [paths] xbe or [paths] iso (or --xbe= / --iso=)");
        return false;
    }

    // Design 1.4 step 1: all addresses are build specific -> exact file required.
    std::string sha1;
    if (!Sha1Hex(bytes, sha1)) {
        FUT_LOGE(Core, "SHA-1 computation failed (BCrypt)");
        return false;
    }
    if (bytes.size() != guest::kXbeFileSize || sha1 != guest::kXbeSha1) {
        FUT_LOGE(Core, "wrong XBE %s: %zu bytes, SHA-1 %s (expected %u bytes, SHA-1 %s = retail EU)",
                 source.c_str(), bytes.size(), sha1.c_str(), guest::kXbeFileSize, guest::kXbeSha1);
        return false;
    }
    MappedSection sections[guest::kXbeSectionCount] = {};
    if (!CheckXbe(bytes, sections)) {
        return false;
    }
    g_xbe = new std::vector<std::uint8_t>(std::move(bytes));
    std::memcpy(g_sections, sections, sizeof(g_sections));
    g_sectionsValid = true;
    FUT_LOGI(Core, "XBE %s: %zu bytes, SHA-1 %s, entry 0x%08X, kernel thunks 0x%08X, %u sections",
             source.c_str(), g_xbe->size(), sha1.c_str(), guest::kEntryPoint, guest::kKernelThunkTable,
             guest::kXbeSectionCount);
    LogCertificate(*g_xbe);
    return true;
}

bool StageMapXbe() {
    if (g_xbe == nullptr || !g_sectionsValid) {
        FUT_LOGE(Core, "MapXbe: no verified XBE (LoadXbe failed?)");
        return false;
    }
    const LauncherInfo& li = GetLauncherInfo();
    if (li.xbeSpace != guest::kXbeSectionsBegin || li.xbeSpace + li.xbeSpaceSize < guest::kXbeSpaceEnd) {
        FUT_LOGE(Core, "MapXbe: launcher placeholder 0x%08lX+0x%lX does not cover 0x%08X..0x%08X",
                 li.xbeSpace, li.xbeSpaceSize, guest::kXbeSectionsBegin, guest::kXbeSpaceEnd);
        return false;
    }
    // Design 1.4 step 3: one protection change over header page + placeholder (one image allocation).
    DWORD oldProtect = 0;
    if (!VirtualProtect(guest::Ptr<void>(guest::kXbeBase), guest::kXbeProtectSize, PAGE_EXECUTE_READWRITE,
                        &oldProtect)) {
        FUT_LOGE(Core, "MapXbe: VirtualProtect(0x%08X, 0x%X, RWX) failed (error %lu)", guest::kXbeBase,
                 guest::kXbeProtectSize, GetLastError());
        return false;
    }
    // Zero everything first: BSS tails of all sections and the gaps between them (cpu.md 5/6).
    std::memset(guest::Ptr<void>(guest::kXbeSectionsBegin), 0,
                guest::kXbeSpaceEnd - guest::kXbeSectionsBegin);
    // Design 1.4 step 4: all 10 sections, including the non-preloaded $$XTIMAGE and .XTLID.
    for (std::uint32_t i = 0; i < guest::kXbeSectionCount; ++i) {
        const MappedSection& s = g_sections[i];
        std::memcpy(guest::Ptr<void>(s.va), g_xbe->data() + s.rawAddress, s.rawSize);
        FUT_LOGD(Core, "mapped %-10s 0x%08X..0x%08X raw 0x%06X+0x%06X bss 0x%X", guest::kSections[i].name,
                 s.va, s.va + s.virtualSize, s.rawAddress, s.rawSize, s.virtualSize - s.rawSize);
    }
    FlushInstructionCache(GetCurrentProcess(), guest::Ptr<const void>(guest::kXbeSectionsBegin),
                          guest::kXbeSpaceEnd - guest::kXbeSectionsBegin);
    FUT_LOGI(Core, "XBE mapped: 10 sections into 0x%08X..0x%08X (RWX, BSS and gaps zeroed)",
             guest::kXbeSectionsBegin, guest::kXbeSectionsEnd);
    return true;
}

std::span<const std::uint8_t> XbeFileBytes() noexcept {
    return g_xbe != nullptr ? std::span<const std::uint8_t>(*g_xbe) : std::span<const std::uint8_t>();
}

bool XbeFileOffset(guest::GuestAddr va, std::uint32_t& offset) noexcept {
    if (g_xbe == nullptr || !g_sectionsValid) {
        return false;
    }
    if (HeaderOffset(va, offset)) {
        return true;
    }
    for (const MappedSection& s : g_sections) {
        if (va >= s.va && va - s.va < s.rawSize) {
            offset = s.rawAddress + (va - s.va);
            return true;
        }
    }
    return false;
}

bool ReadOriginalXbeBytes(guest::GuestAddr va, void* out, std::uint32_t size) noexcept {
    if (g_xbe == nullptr || !guest::InXbeImage(va, size)) {
        return false;
    }
    auto* dst = static_cast<std::uint8_t*>(out);
    for (std::uint32_t i = 0; i < size; ++i) {
        std::uint32_t off = 0;
        dst[i] = XbeFileOffset(va + i, off) ? (*g_xbe)[off] : 0;  // BSS / gaps are zero after MapXbe
    }
    return true;
}

}  // namespace core
