// Unit tests of the kernel HLE (kernel agent; design 5.1 test "sha1" and the M1 kernel exports).
// Run without the XBE: guest structures are host buffers in the same address space.
#include "doctest.h"

#include "core/config.h"
#include "core/guest_thread.h"
#include "kernel/io_apc.h"
#include "kernel/ke_thread.h"
#include "kernel/ke_types.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "kernel/kernel_hle.h"
#include "kernel/kernel_internal.h"
#include "kernel/kernel_settings.h"
#include "kernel/ob_handles.h"
#include "kernel/xc_crypto.h"
#include "shared/launcher_info.h"
#include "shared/xbe_format.h"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string Hex(const std::uint8_t* p, std::size_t n) {
    std::string s;
    char b[3];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02x", p[i]);
        s += b;
    }
    return s;
}

// Fake launcher info with real TlsAlloc indices (the tests run without the launcher).
const LauncherInfo& KernelTestLauncherInfo() {
    static LauncherInfo info = [] {
        LauncherInfo li{};
        li.size = sizeof(li);
        li.version = FUT_LAUNCHER_INFO_VERSION;
        for (auto& slot : li.tlsSlots) {
            slot = TlsAlloc();
        }
        li.xbeSpace = FUT_XBE_SPACE_ADDRESS;
        li.xbeSpaceSize = FUT_XBE_SPACE_SIZE;
        li.imageBase = FUT_LAUNCHER_IMAGE_BASE;
        return li;
    }();
    core::SetLauncherInfo(&info);
    return info;
}

template <class T>
T SlotValue(std::uint32_t slot) {
    return reinterpret_cast<T>(krnl::GetBinding(slot).address);
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// SHA-1 / HMAC / Xc*
// ------------------------------------------------------------------------------------------------
TEST_CASE("kernel: SHA-1 FIPS 180-1 vectors (one-shot and incremental)") {
    std::uint8_t d[20];
    krnl::xc::Sha1("abc", 3, d);
    CHECK(Hex(d, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    krnl::xc::Sha1("", 0, d);
    CHECK(Hex(d, 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    krnl::xc::Sha1(m, std::strlen(m), d);
    CHECK(Hex(d, 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    // one million 'a', fed in odd chunk sizes
    std::vector<std::uint8_t> a(1000000, 'a');
    krnl::xc::ShaContext ctx;
    krnl::xc::ShaInit(ctx);
    std::size_t pos = 0, step = 1;
    while (pos < a.size()) {
        const std::size_t n = std::min(step, a.size() - pos);
        krnl::xc::ShaUpdate(ctx, a.data() + pos, n);
        pos += n;
        step = step * 3 % 1031 + 1;
    }
    krnl::xc::ShaFinal(ctx, d);
    CHECK(Hex(d, 20) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("kernel: HMAC-SHA1 RFC 2202 vectors") {
    std::uint8_t d[20];
    std::uint8_t key1[20];
    std::memset(key1, 0x0B, sizeof(key1));
    krnl::xc::HmacSha1(key1, sizeof(key1), "Hi There", 8, d);
    CHECK(Hex(d, 20) == "b617318655057264e28bc0b6fb378c8ef146be00");
    const char* data2 = "what do ya want for nothing?";
    krnl::xc::HmacSha1("Jefe", 4, data2, std::strlen(data2), d);
    CHECK(Hex(d, 20) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    std::uint8_t key80[80];
    std::memset(key80, 0xAA, sizeof(key80));
    const char* data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    krnl::xc::HmacSha1(key80, sizeof(key80), data6, std::strlen(data6), d);
    CHECK(Hex(d, 20) == "aa4ae5e15272d00e95705637ce8a3b55ed402112");
}

TEST_CASE("kernel: XcSHA* keep the state inside the 0x74-byte guest buffer") {
    std::uint8_t ctx[krnl::xc::kGuestShaContextSize + 16];
    std::memset(ctx, 0xCD, sizeof(ctx));
    std::uint8_t digest[20] = {};
    krnl::XcSHAInit(ctx);
    krnl::XcSHAUpdate(ctx, reinterpret_cast<const unsigned char*>("ab"), 2);
    krnl::XcSHAUpdate(ctx, reinterpret_cast<const unsigned char*>("c"), 1);
    krnl::XcSHAFinal(ctx, digest);
    CHECK(Hex(digest, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    for (std::size_t i = sizeof(krnl::xc::ShaContext); i < sizeof(ctx); ++i) {
        CHECK(ctx[i] == 0xCD);  // nothing written behind the 0x5C-byte state
    }
}

// Re-implements the XAPI HMAC construction (FUN_000f6f26 ipad + XcSHAUpdate, FUN_000f6f88 opad:
// XcSHAFinal, XcSHAInit on the SAME context, XcSHAUpdate over opad||inner = 0x54 bytes, XcSHAFinal)
// and checks it equals HMAC-SHA1 - i.e. the context semantics fit XCalculateSignature*.
TEST_CASE("kernel: XAPI HMAC sequence over XcSHA* equals HMAC-SHA1 (save signatures)") {
    const std::uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::vector<std::uint8_t> data(0xFEC);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    std::uint8_t ctx[krnl::xc::kGuestShaContextSize];
    std::uint32_t ipad[16] = {};
    std::memcpy(ipad, key, sizeof(key));
    for (auto& w : ipad) {
        w ^= 0x36363636;
    }
    krnl::XcSHAInit(ctx);
    krnl::XcSHAUpdate(ctx, reinterpret_cast<const unsigned char*>(ipad), 0x40);
    krnl::XcSHAUpdate(ctx, data.data(), static_cast<ULONG>(data.size()));
    struct {
        std::uint32_t opad[16];
        std::uint8_t inner[20];
    } outer = {};
    static_assert(sizeof(outer) == 0x54);
    std::memcpy(outer.opad, key, sizeof(key));
    for (auto& w : outer.opad) {
        w ^= 0x5C5C5C5C;
    }
    krnl::XcSHAFinal(ctx, outer.inner);
    krnl::XcSHAInit(ctx);
    krnl::XcSHAUpdate(ctx, reinterpret_cast<const unsigned char*>(&outer), 0x54);
    std::uint8_t sig[20];
    krnl::XcSHAFinal(ctx, sig);
    std::uint8_t expected[20];
    krnl::xc::HmacSha1(key, sizeof(key), data.data(), data.size(), expected);
    CHECK(Hex(sig, 20) == Hex(expected, 20));
    CHECK(krnl::XcVerifyPKCS1Signature(nullptr, nullptr, nullptr) == TRUE);
}

// ------------------------------------------------------------------------------------------------
// Rtl
// ------------------------------------------------------------------------------------------------
TEST_CASE("kernel: RtlNtStatusToDosError matches ntdll and the Win32 conventions") {
    CHECK(krnl::RtlNtStatusToDosError(STATUS_SUCCESS) == 0);
    CHECK(krnl::RtlNtStatusToDosError(STATUS_OBJECT_NAME_NOT_FOUND) == ERROR_FILE_NOT_FOUND);
    CHECK(krnl::RtlNtStatusToDosError(STATUS_OBJECT_PATH_NOT_FOUND) == ERROR_PATH_NOT_FOUND);
    CHECK(krnl::RtlNtStatusToDosError(STATUS_OBJECT_NAME_COLLISION) == ERROR_ALREADY_EXISTS);
    CHECK(krnl::RtlNtStatusToDosError(STATUS_END_OF_FILE) == ERROR_HANDLE_EOF);
    CHECK(krnl::RtlNtStatusToDosError(STATUS_PENDING) == ERROR_IO_PENDING);
    CHECK(krnl::RtlNtStatusToDosError(STATUS_DISK_FULL) == ERROR_DISK_FULL);
    CHECK(krnl::RtlNtStatusToDosError(STATUS_NO_MORE_FILES) == ERROR_NO_MORE_FILES);
    CHECK(krnl::RtlNtStatusToDosError(static_cast<NTSTATUS>(0xC0070005)) == 5);  // FACILITY_NTWIN32

    using NoTebFn = ULONG(NTAPI*)(NTSTATUS);
    auto fn = reinterpret_cast<NoTebFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosErrorNoTeb"));
    REQUIRE(fn != nullptr);
    for (std::uint32_t i = 0; i < krnl::StatusTableSize(); ++i) {
        std::uint32_t status = 0, error = 0;
        krnl::StatusTableEntry(i, status, error);
        CAPTURE(status);
        CHECK(fn(static_cast<NTSTATUS>(status)) == error);
    }
    // a code outside the table falls back to ntdll
    CHECK(krnl::RtlNtStatusToDosError(STATUS_DLL_NOT_FOUND) == fn(STATUS_DLL_NOT_FOUND));
}

TEST_CASE("kernel: RtlInitAnsiString / RtlEqualString / RtlCompareMemoryUlong") {
    xbox::ANSI_STRING a{}, b{};
    krnl::RtlInitAnsiString(&a, "$$XTIMAGE");
    CHECK(a.Length == 9);
    CHECK(a.MaximumLength == 10);
    CHECK(std::strcmp(a.Buffer, "$$XTIMAGE") == 0);
    krnl::RtlInitAnsiString(&b, nullptr);
    CHECK(b.Length == 0);
    CHECK(b.MaximumLength == 0);
    CHECK(b.Buffer == nullptr);

    krnl::RtlInitAnsiString(&b, "$$xtimage");
    CHECK(krnl::RtlEqualString(&a, &b, TRUE) == TRUE);
    CHECK(krnl::RtlEqualString(&a, &b, FALSE) == FALSE);
    krnl::RtlInitAnsiString(&b, "$$XTIMAGE2");
    CHECK(krnl::RtlEqualString(&a, &b, TRUE) == FALSE);

    std::uint32_t buf[8];
    for (auto& v : buf) {
        v = 0xFEEEFEEE;
    }
    CHECK(krnl::RtlCompareMemoryUlong(buf, sizeof(buf), 0xFEEEFEEE) == sizeof(buf));
    CHECK(krnl::RtlCompareMemoryUlong(buf, 15, 0xFEEEFEEE) == 12);  // rounded down to whole ULONGs
    buf[5] = 0;
    CHECK(krnl::RtlCompareMemoryUlong(buf, sizeof(buf), 0xFEEEFEEE) == 20);
    CHECK(krnl::RtlCompareMemoryUlong(buf, 0, 0xFEEEFEEE) == 0);
}

TEST_CASE("kernel: RtlUnwind and RtlRaiseException slots are ntdll's own functions (design 1.11)") {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    CHECK(krnl::GetBinding(krnl::exp::RtlUnwind::slot).address ==
          reinterpret_cast<std::uintptr_t>(GetProcAddress(ntdll, "RtlUnwind")));
    CHECK(krnl::GetBinding(krnl::exp::RtlRaiseException::slot).address ==
          reinterpret_cast<std::uintptr_t>(GetProcAddress(ntdll, "RtlRaiseException")));
}

TEST_CASE("kernel: critical sections map guest addresses to host locks and mirror the Xbox fields") {
    KernelTestLauncherInfo();
    krnl::guestdef::RTL_CRITICAL_SECTION cs;
    std::memset(&cs, 0xAB, sizeof(cs));
    krnl::RtlInitializeCriticalSection(&cs);
    CHECK(cs.Event.Type == 1);
    CHECK(cs.Event.Size == 4);
    CHECK(cs.LockCount == -1);
    CHECK(cs.RecursionCount == 0);
    CHECK(cs.OwningThread == 0);

    krnl::RtlEnterCriticalSection(&cs);
    krnl::RtlEnterCriticalSection(&cs);
    CHECK(cs.RecursionCount == 2);
    CHECK(cs.LockCount == 1);

    std::atomic<int> stage{0};
    std::thread other([&] {
        stage = 1;
        krnl::RtlEnterCriticalSection(&cs);  // blocks until the main thread left twice
        stage = 2;
        krnl::RtlLeaveCriticalSection(&cs);
    });
    while (stage.load() == 0) {
        Sleep(1);
    }
    Sleep(50);
    CHECK(stage.load() == 1);
    krnl::RtlLeaveCriticalSection(&cs);
    Sleep(20);
    CHECK(stage.load() == 1);  // still held once
    krnl::RtlLeaveCriticalSection(&cs);
    other.join();
    CHECK(stage.load() == 2);
    CHECK(cs.LockCount == -1);
    CHECK(cs.RecursionCount == 0);

    // a lock never passed to RtlInitializeCriticalSection (statically initialized guest lock)
    krnl::guestdef::RTL_CRITICAL_SECTION lazy{};
    krnl::RtlEnterCriticalSection(&lazy);
    CHECK(lazy.RecursionCount == 1);
    krnl::RtlLeaveCriticalSection(&lazy);
    CHECK(lazy.RecursionCount == 0);
    krnl::RtlLeaveCriticalSection(&lazy);  // not owned: logged and ignored, no crash
    CHECK(lazy.RecursionCount == 0);
}

// ------------------------------------------------------------------------------------------------
// Memory
// ------------------------------------------------------------------------------------------------
TEST_CASE("kernel: Nt*VirtualMemory keep NT in/out semantics below 0x80000000 (design 1.8)") {
    void* base = nullptr;
    SIZE_T size = 0x10000;
    REQUIRE(krnl::NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE) == STATUS_SUCCESS);
    REQUIRE(base != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(base) + size <= 0x80000000u);
    CHECK(size == 0x10000);

    void* commit = static_cast<std::uint8_t*>(base) + 0x1234;  // rounded down to the page
    SIZE_T commitSize = 0x100;
    REQUIRE(krnl::NtAllocateVirtualMemory(&commit, 0, &commitSize, MEM_COMMIT, PAGE_READWRITE) ==
            STATUS_SUCCESS);
    CHECK(commit == static_cast<std::uint8_t*>(base) + 0x1000);
    CHECK(commitSize == 0x1000);
    static_cast<std::uint8_t*>(commit)[0] = 1;

    krnl::guestdef::MEMORY_BASIC_INFORMATION mbi{};
    REQUIRE(krnl::NtQueryVirtualMemory(commit, &mbi) == STATUS_SUCCESS);
    CHECK(mbi.BaseAddress == reinterpret_cast<std::uintptr_t>(commit));
    CHECK(mbi.State == MEM_COMMIT);
    CHECK(mbi.RegionSize == 0x1000);
    REQUIRE(krnl::NtQueryVirtualMemory(static_cast<std::uint8_t*>(base) + 0x2000, &mbi) == STATUS_SUCCESS);
    CHECK(mbi.State == MEM_RESERVE);

    SIZE_T decommit = 0x1000;
    void* d = commit;
    CHECK(krnl::NtFreeVirtualMemory(&d, &decommit, MEM_DECOMMIT) == STATUS_SUCCESS);
    // XAPI releases with the reserved size (FUN_000f3e12: NtFreeVirtualMemory(&p, &0x10000, MEM_RELEASE))
    SIZE_T release = 0x10000;
    CHECK(krnl::NtFreeVirtualMemory(&base, &release, MEM_RELEASE) == STATUS_SUCCESS);
    SIZE_T again = 0;
    CHECK(krnl::NtFreeVirtualMemory(&base, &again, MEM_RELEASE) != STATUS_SUCCESS);
}

TEST_CASE("kernel: contiguous and system memory (design 1.8)") {
    void* p = krnl::MmAllocateContiguousMemoryEx(0x1801, 0, 0x7FFFFFF, 0x4000, 0x404);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 0x4000 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(p) < 0x80000000u);
    CHECK(krnl::MmQueryAllocationSize(p) == 0x2000);
    std::memset(p, 0x5A, 0x1801);
    krnl::MmPersistContiguousMemory(p, 0x1801, TRUE);
    krnl::MmFreeContiguousMemory(p);

    void* big = krnl::MmAllocateContiguousMemoryEx(0x3000, 0, 0xFFFFFFFF, 0x40000, PAGE_READWRITE);
    REQUIRE(big != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(big) % 0x40000 == 0);
    krnl::MmFreeContiguousMemory(big);

    void* page = krnl::MmAllocateContiguousMemory(0x1000);  // LaunchDataPage size (FUN_000f81bb)
    REQUIRE(page != nullptr);
    CHECK(krnl::MmQueryAllocationSize(page) == 0x1000);
    krnl::MmFreeContiguousMemory(page);
    krnl::MmFreeContiguousMemory(page);  // second free: warning only

    void* sys = krnl::MmAllocateSystemMemory(0x10000, PAGE_READWRITE);
    REQUIRE(sys != nullptr);
    CHECK(krnl::MmQueryAddressProtect(sys) == PAGE_READWRITE);
    krnl::MmSetAddressProtect(sys, 0x1000, PAGE_READONLY);
    CHECK(krnl::MmQueryAddressProtect(sys) == PAGE_READONLY);
    CHECK(krnl::MmFreeSystemMemory(sys, 0x10000) == 0x10);
    CHECK(krnl::MmQueryAddressProtect(sys) == 0);
}

// ------------------------------------------------------------------------------------------------
// Settings and data exports
// ------------------------------------------------------------------------------------------------
TEST_CASE("kernel: ExQueryNonVolatileSetting answers the documented indices") {
    krnl::KernelSettings s{};
    s.language = 3;  // german
    krnl::SetSettings(s);
    ULONG type = 0, value = 0xFFFFFFFF, result = 0;
    CHECK(krnl::ExQueryNonVolatileSetting(7, &type, &value, 4, &result) == STATUS_SUCCESS);
    CHECK(type == 4);
    CHECK(value == 3);
    CHECK(result == 4);
    CHECK(krnl::ExQueryNonVolatileSetting(0xA, &type, &value, 4, nullptr) == STATUS_SUCCESS);
    CHECK(value == 0);  // parental control MUST be 0 (XapiInitProcess 0xF93E8)
    CHECK(krnl::ExQueryNonVolatileSetting(0x11, &type, &value, 4, nullptr) == STATUS_SUCCESS);
    CHECK(value == 0);  // AutoPowerDown off
    CHECK(krnl::ExQueryNonVolatileSetting(0x104, &type, &value, 4, nullptr) == STATUS_SUCCESS);
    CHECK(value == 4);
    CHECK(krnl::ExQueryNonVolatileSetting(9, &type, &value, 4, nullptr) == STATUS_SUCCESS);
    CHECK(value == 0);
    std::uint8_t eeprom[0x100];
    CHECK(krnl::ExQueryNonVolatileSetting(0xFFFF, &type, eeprom, sizeof(eeprom), nullptr) ==
          STATUS_OBJECT_NAME_NOT_FOUND);
    std::uint16_t small = 0;
    CHECK(krnl::ExQueryNonVolatileSetting(7, &type, &small, 2, &result) == STATUS_BUFFER_TOO_SMALL);
    krnl::SetSettings(krnl::KernelSettings{});
}

TEST_CASE("kernel: [settings]/[saves]/[kernel] parsing") {
    core::Config c;
    krnl::KernelSettings s;
    std::string err;
    REQUIRE(krnl::LoadKernelSettings(c, s, err));
    CHECK(s.language == 1);
    CHECK(s.avPack == 6);
    CHECK(s.minGuestStack == 0x200000);
    CHECK(s.keyMode == krnl::SaveKeyMode::Fixed);

    c.values["settings.language"] = "Italian";
    c.values["settings.game_region"] = "0x1";
    c.values["kernel.min_guest_stack"] = "0x400000";
    c.values["saves.signature_key"] = "00112233445566778899aabbccddeeff";
    REQUIRE(krnl::LoadKernelSettings(c, s, err));
    CHECK(s.language == 6);
    CHECK(s.gameRegion == 1);
    CHECK(s.minGuestStack == 0x400000);
    CHECK(s.hasSignatureKeyOverride);
    CHECK(s.signatureKeyOverride[15] == 0xFF);

    c.values["settings.language"] = "klingon";
    CHECK_FALSE(krnl::LoadKernelSettings(c, s, err));
    c.values["settings.language"] = "5";
    REQUIRE(krnl::LoadKernelSettings(c, s, err));
    CHECK(s.language == 5);
    c.values["saves.key_mode"] = "xbox";  // needs xbox_cert_key
    CHECK_FALSE(krnl::LoadKernelSettings(c, s, err));
    c.values["saves.xbox_cert_key"] = "000102030405060708090A0B0C0D0E0F";
    REQUIRE(krnl::LoadKernelSettings(c, s, err));
    CHECK(s.keyMode == krnl::SaveKeyMode::Xbox);
}

TEST_CASE("kernel: data exports have the design 1.5 values") {
    krnl::KernelSettings s{};
    REQUIRE(krnl::InitDataExports(s));
    const auto* ver = SlotValue<const krnl::guestdef::XBOX_KRNL_VERSION*>(krnl::exp::XboxKrnlVersion::slot);
    CHECK(ver->Major == 1);
    CHECK(ver->Minor == 0);
    CHECK(ver->Build == 5838);
    CHECK(ver->Qfe == 1);
    const auto* hw = SlotValue<const krnl::guestdef::XBOX_HARDWARE_INFO*>(krnl::exp::XboxHardwareInfo::slot);
    CHECK((hw->Flags & 8) == 0);
    CHECK(*SlotValue<const std::uint32_t*>(krnl::exp::LaunchDataPage::slot) == 0);
    CHECK(*SlotValue<const std::uint32_t*>(krnl::exp::HalBootSMCVideoMode::slot) == 6);
    CHECK(*SlotValue<const std::uint32_t*>(krnl::exp::HalDiskCachePartitionCount::slot) == 3);
    const auto* idex = SlotValue<const std::uint8_t*>(krnl::exp::IdexChannelObject::slot);
    CHECK(*reinterpret_cast<const std::uint32_t*>(idex + 0x20) == 0);

    const auto* hd = SlotValue<const std::uint8_t*>(krnl::exp::XboxHDKey::slot);
    bool nonZero = false;
    for (int i = 0; i < 16; ++i) {
        nonZero |= hd[i] != 0;
    }
    CHECK(nonZero);  // FUN_000f6ffa would read the EEPROM otherwise

    // fixed key = SHA-1("FuturamaPC/XboxSignatureKey/fixed/v1")[0..15], stable across runs
    std::uint8_t expected[20];
    const char* label = "FuturamaPC/XboxSignatureKey/fixed/v1";
    krnl::xc::Sha1(label, std::strlen(label), expected);
    const auto* sig = SlotValue<const std::uint8_t*>(krnl::exp::XboxSignatureKey::slot);
    CHECK(std::memcmp(sig, expected, 16) == 0);

    s.hasSignatureKeyOverride = true;
    for (int i = 0; i < 16; ++i) {
        s.signatureKeyOverride[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    REQUIRE(krnl::InitDataExports(s));
    CHECK(sig[0] == 0xA0);
    CHECK(sig[15] == 0xAF);
    REQUIRE(krnl::InitDataExports(krnl::KernelSettings{}));

    // every DATA/ADDR slot is registered and below 0x80000000 (design 1.2)
    for (std::uint32_t slot = 0; slot < krnl::gen::kSlotCount; ++slot) {
        if (krnl::gen::kExports[slot].kind != krnl::gen::Kind::Func) {
            CAPTURE(krnl::gen::kExports[slot].name);
            CHECK(krnl::IsImplemented(slot));
            CHECK(krnl::GetBinding(slot).address < 0x80000000u);
        }
    }
}

TEST_CASE("kernel: [saves] key_mode = xbox derives the keys from the certificate (design 4.5)") {
    // Needs the header page 0x10000 (certificate at 0x10180) - free in the test process normally.
    void* page =
        VirtualAlloc(reinterpret_cast<void*>(0x10000), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (page != reinterpret_cast<void*>(0x10000)) {
        MESSAGE("0x10000 not available in this process - skipped");
        if (page != nullptr) {
            VirtualFree(page, 0, MEM_RELEASE);
        }
        return;
    }
    auto* cert = reinterpret_cast<xbe::Certificate*>(0x10180);
    for (int i = 0; i < 16; ++i) {
        cert->SignatureKey[i] = static_cast<std::uint8_t>(0x30 + i);
        cert->AlternateSignatureKeys[3][i] = static_cast<std::uint8_t>(0x60 + i);
    }
    krnl::KernelSettings s{};
    s.keyMode = krnl::SaveKeyMode::Xbox;
    s.hasXboxCertKey = true;
    for (int i = 0; i < 16; ++i) {
        s.xboxCertKey[i] = static_cast<std::uint8_t>(i * 17);
    }
    krnl::SetSettings(s);
    REQUIRE(krnl::InitDataExports(s));
    krnl::OnXbeMapped();
    std::uint8_t expected[20];
    krnl::xc::HmacSha1(s.xboxCertKey.data(), 16, cert->SignatureKey, 16, expected);
    CHECK(std::memcmp(krnl::DataXboxSignatureKey(), expected, 16) == 0);
    krnl::xc::HmacSha1(s.xboxCertKey.data(), 16, cert->AlternateSignatureKeys[3], 16, expected);
    CHECK(std::memcmp(krnl::DataXboxAlternateSignatureKeys() + 3 * 16, expected, 16) == 0);
    krnl::SetSettings(krnl::KernelSettings{});
    REQUIRE(krnl::InitDataExports(krnl::KernelSettings{}));
    VirtualFree(page, 0, MEM_RELEASE);
}

namespace {
std::uint32_t g_completionErr = 0xFFFFFFFF, g_completionBytes = 0;
void* g_completionOverlapped = nullptr;
void __stdcall FakeCompletion(std::uint32_t err, std::uint32_t bytes, void* overlapped) {
    g_completionErr = err;
    g_completionBytes = bytes;
    g_completionOverlapped = overlapped;
}
}  // namespace

TEST_CASE("kernel: NtUserIoApcDispatcher calls the completion routine with the mapped error") {
    using Dispatcher = void(__stdcall*)(void*, xbox::IO_STATUS_BLOCK*, std::uint32_t);
    auto dispatcher = SlotValue<Dispatcher>(krnl::exp::NtUserIoApcDispatcher::slot);
    REQUIRE(dispatcher == &krnl::NtUserIoApcDispatcher);
    xbox::IO_STATUS_BLOCK iosb{};
    iosb.Status = STATUS_SUCCESS;
    iosb.Information = 0x800;
    dispatcher(reinterpret_cast<void*>(&FakeCompletion), &iosb, 0);
    CHECK(g_completionErr == 0);
    CHECK(g_completionBytes == 0x800);
    CHECK(g_completionOverlapped == &iosb);
    iosb.Status = STATUS_END_OF_FILE;
    iosb.Information = 0;
    dispatcher(reinterpret_cast<void*>(&FakeCompletion), &iosb, 0);
    CHECK(g_completionErr == ERROR_HANDLE_EOF);
    iosb.Status = static_cast<NTSTATUS>(0x80000005);  // warning: not an error -> 0 (BasepIoCompletion)
    dispatcher(reinterpret_cast<void*>(&FakeCompletion), &iosb, 0);
    CHECK(g_completionErr == 0);
}

// ------------------------------------------------------------------------------------------------
// Ke: timers, events, waits, sections
// ------------------------------------------------------------------------------------------------
TEST_CASE("kernel: KeSetTimer remembers, KeCancelTimer reports the previous state") {
    std::uint8_t timer[0x28] = {};
    std::uint8_t dpc[0x20] = {};
    krnl::KeInitializeDpc(dpc, reinterpret_cast<void*>(0xF9B08), nullptr);
    krnl::KeInitializeTimerEx(timer, 0);
    xbox::LARGE_INTEGER due;
    due.QuadPart = -216000000000LL;  // 6 h relative (XAutoPowerDownResetTimer)
    CHECK(krnl::KeSetTimer(timer, due, dpc) == FALSE);
    // as pushed by XAutoPowerDownResetTimer 0xF9AED: Timer, DueTime low, DueTime high, Dpc (purge 16)
    using RawKeSetTimer = BOOLEAN(__stdcall*)(void*, ULONG, ULONG, void*);
    auto raw = SlotValue<RawKeSetTimer>(krnl::exp::KeSetTimer::slot);
    CHECK(raw(timer, 0xB5659000u, 0xFFFFFFCDu, dpc) == TRUE);
    CHECK(krnl::KeCancelTimer(timer) == TRUE);
    CHECK(krnl::KeCancelTimer(timer) == FALSE);
    for (auto b : timer) {
        CHECK(b == 0);  // design 1.7: the guest structure is not filled
    }
}

TEST_CASE("kernel: TimeoutToMilliseconds") {
    xbox::LARGE_INTEGER t;
    CHECK(krnl::TimeoutToMilliseconds(nullptr) == INFINITE);
    t.QuadPart = 0;
    CHECK(krnl::TimeoutToMilliseconds(&t) == 0);
    t.QuadPart = -10000;
    CHECK(krnl::TimeoutToMilliseconds(&t) == 1);
    t.QuadPart = -1;
    CHECK(krnl::TimeoutToMilliseconds(&t) == 1);  // rounded up
    t.QuadPart = LLONG_MIN;                       // SleepEx(INFINITE): {0, 0x80000000}
    CHECK(krnl::TimeoutToMilliseconds(&t) == INFINITE);
    t.QuadPart = 1;  // absolute time in the past
    CHECK(krnl::TimeoutToMilliseconds(&t) == 0);
}

TEST_CASE("kernel: KeSetEvent / KeWaitForSingleObject on a guest KEVENT") {
    krnl::guestdef::DISPATCHER_HEADER ev{};
    ev.Type = 0;  // NotificationEvent, not signaled
    ev.Size = 4;
    xbox::LARGE_INTEGER zero;
    zero.QuadPart = 0;
    CHECK(krnl::KeWaitForSingleObject(&ev, 0, 1, FALSE, &zero) == STATUS_TIMEOUT);
    CHECK(krnl::KeSetEvent(&ev, 1, FALSE) == 0);
    CHECK(ev.SignalState == 1);
    CHECK(krnl::KeWaitForSingleObject(&ev, 0, 1, FALSE, nullptr) == STATUS_SUCCESS);
    CHECK(krnl::KeWaitForSingleObject(&ev, 0, 1, FALSE, &zero) == STATUS_SUCCESS);  // notification stays set

    krnl::guestdef::DISPATCHER_HEADER sync{};
    sync.Type = 1;  // SynchronizationEvent
    CHECK(krnl::KeSetEvent(&sync, 1, FALSE) == 0);
    CHECK(krnl::KeWaitForSingleObject(&sync, 0, 1, FALSE, &zero) == STATUS_SUCCESS);
    CHECK(sync.SignalState == 0);
    CHECK(krnl::KeWaitForSingleObject(&sync, 0, 1, FALSE, &zero) == STATUS_TIMEOUT);
}

TEST_CASE("kernel: alertable KeDelayExecutionThread delivers queued I/O APCs (io_apc.h contract)") {
    xbox::LARGE_INTEGER zero;
    zero.QuadPart = 0;
    CHECK(krnl::KeDelayExecutionThread(1, TRUE, &zero) == STATUS_SUCCESS);  // nothing queued
    xbox::IO_STATUS_BLOCK iosb{};
    g_completionErr = 0xFFFFFFFF;
    REQUIRE(krnl::io::QueueIoApc(&krnl::NtUserIoApcDispatcher, reinterpret_cast<void*>(&FakeCompletion),
                                 &iosb, STATUS_SUCCESS, 0x1000));
    CHECK(krnl::KeDelayExecutionThread(1, FALSE, &zero) == STATUS_SUCCESS);  // not alertable: stays queued
    CHECK(g_completionErr == 0xFFFFFFFF);
    CHECK(krnl::KeDelayExecutionThread(1, TRUE, &zero) == STATUS_USER_APC);
    CHECK(g_completionErr == 0);
    CHECK(g_completionBytes == 0x1000);
    CHECK(iosb.Information == 0x1000);
}

TEST_CASE("kernel: XeLoadSection rejects pointers that are not XBE section headers") {
    xbe::SectionHeader fake{};
    CHECK(krnl::XeLoadSection(&fake) == STATUS_INVALID_PARAMETER);
    CHECK(krnl::XeUnloadSection(&fake) == STATUS_INVALID_PARAMETER);
}

// ------------------------------------------------------------------------------------------------
// Threads
// ------------------------------------------------------------------------------------------------
namespace {
struct ThreadProbe {
    std::uint32_t startRoutine;
    std::uint32_t startContext;
    std::uint32_t kthreadId;
    std::uint32_t tlsDataField;
    std::uint32_t anchor;
    std::uint32_t irql;
    std::uint32_t hostId;
    std::uintptr_t stackReserve;
};
ThreadProbe g_probe{};

// Stands in for XapiThreadStartup 0xF99DA: stdcall (StartRoutine, StartContext), never returns.
void __stdcall FakeSystemRoutine(void* startRoutine, void* startContext) {
    g_probe.startRoutine = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(startRoutine));
    g_probe.startContext = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(startContext));
    const auto* kthread = reinterpret_cast<const xbox::FakeKThread*>(
        static_cast<std::uintptr_t>(core::GetGuestSlot(FUT_TLS_S_KTHREAD)));
    g_probe.kthreadId = kthread ? kthread->ThreadId : 0;
    g_probe.tlsDataField = kthread ? kthread->TlsData : 0;
    g_probe.anchor = core::GetGuestSlot(FUT_TLS_S_TLS);
    g_probe.irql = core::GetGuestSlot(FUT_TLS_S_IRQL);
    g_probe.hostId = GetCurrentThreadId();
    g_probe.stackReserve = kthread ? kthread->StackBase - kthread->StackLimit : 0;
    krnl::PsTerminateSystemThread(static_cast<NTSTATUS>(0x1234));
}
}  // namespace

TEST_CASE(
    "kernel: PsCreateSystemThreadEx runs SystemRoutine(StartRoutine, StartContext) on an adopted thread") {
    KernelTestLauncherInfo();
    krnl::SetSettings(krnl::KernelSettings{});
    REQUIRE(krnl::InitThreading());
    HANDLE handle = nullptr;
    ULONG tid = 0;
    // Called like the guest does through the thunk slot, all arguments as DWORDs (purge 40). CreateThread
    // 0xF9AAB passes (dwCreationFlags >> 2) & 0xFFFFFF01: only the low byte is the BOOLEAN.
    using RawPsCreateSystemThreadEx =
        NTSTATUS(__stdcall*)(HANDLE*, ULONG, ULONG, ULONG, ULONG*, ULONG, ULONG, ULONG, ULONG, ULONG);
    auto raw = SlotValue<RawPsCreateSystemThreadEx>(krnl::exp::PsCreateSystemThreadEx::slot);
    const NTSTATUS st = raw(&handle, 0, 0x10000, 0x24, &tid, 0xF8469, 0x77, 0xFFFFFF00u, 0xFFFFFF00u,
                            static_cast<ULONG>(reinterpret_cast<std::uintptr_t>(&FakeSystemRoutine)));
    REQUIRE(st == STATUS_SUCCESS);
    REQUIRE(handle != nullptr);
    CHECK(krnl::NtWaitForSingleObject(handle, FALSE, nullptr) == STATUS_SUCCESS);
    auto thread = krnl::HandleTable::Instance().LookupAs<krnl::ThreadObject>(handle);
    REQUIRE(thread);
    CHECK(thread->HasExited());
    CHECK(thread->ExitStatus() == 0x1234);
    DWORD exitCode = 0;
    CHECK(GetExitCodeThread(thread->HostHandle(), &exitCode));
    CHECK(exitCode == 0x1234);
    thread = {};
    CHECK(g_probe.startRoutine == 0xF8469);
    CHECK(g_probe.startContext == 0x77);
    CHECK(g_probe.hostId == tid);
    CHECK(g_probe.kthreadId == tid);
    CHECK(g_probe.tlsDataField != 0);
    CHECK(g_probe.anchor == g_probe.tlsDataField + 0x24);  // S_TLS = TlsData + TlsDataSize (design 1.6)
    CHECK(g_probe.irql == 0);
    CHECK(g_probe.stackReserve >= 0x200000);  // max(KernelStackSize, 2 MiB)
    CHECK(krnl::NtClose(handle) == STATUS_SUCCESS);
    CHECK(WaitForSingleObject(krnl::GuestThreadsExitedEvent(), 0) == WAIT_OBJECT_0);
    CHECK(krnl::LiveGuestThreadCount() == 0);
}

TEST_CASE("kernel: IRQL emulation uses the S_IRQL TLS slot") {
    KernelTestLauncherInfo();
    core::SetGuestSlot(FUT_TLS_S_IRQL, 0);
    CHECK(krnl::KeRaiseIrqlToDpcLevel() == 0);
    CHECK(krnl::CurrentIrql() == 2);
    CHECK(krnl::KfRaiseIrql(2) == 2);  // already at DISPATCH_LEVEL (no second lock acquisition)
    krnl::KfLowerIrql(1);
    CHECK(krnl::CurrentIrql() == 1);
    // the dispatcher lock is free again: another thread can raise and lower
    std::thread other([] {
        core::SetGuestSlot(FUT_TLS_S_IRQL, 0);
        const xbox::KIRQL old = krnl::KeRaiseIrqlToDpcLevel();
        krnl::KfLowerIrql(old);
    });
    other.join();
    krnl::KfLowerIrql(0);
    CHECK(krnl::CurrentIrql() == 0);
    krnl::KfLowerIrql(2);  // raising via KfLowerIrql is rejected (logged)
    CHECK(krnl::CurrentIrql() == 0);
}
