// Kernel HLE self-tests (run with --selftest, design 1.4 step 8). Owned by the kernel agent.
#include "core/config.h"
#include "core/guest_thread.h"
#include "core/selftest.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_hle.h"
#include "kernel/kernel_internal.h"
#include "kernel/xc_crypto.h"

#include <intrin.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

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

}  // namespace

// Every entry of the RtlNtStatusToDosError table equals ntdll's answer (the table was generated from
// it; this guards against typos and documents the Windows build that was used).
FUT_SELFTEST(KernelStatusTableMatchesNtdll, Host) {
    using NoTebFn = ULONG(NTAPI*)(NTSTATUS);
    auto fn = reinterpret_cast<NoTebFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosErrorNoTeb"));
    if (fn == nullptr) {
        detail = "ntdll!RtlNtStatusToDosErrorNoTeb not found";
        return false;
    }
    const std::uint32_t n = krnl::StatusTableSize();
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t status = 0, error = 0;
        krnl::StatusTableEntry(i, status, error);
        const ULONG nt = fn(static_cast<NTSTATUS>(status));
        if (nt != error) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "status 0x%08X: table %u, ntdll %lu", status, error, nt);
            detail = msg;
            return false;
        }
    }
    detail = std::to_string(n) + " statuses match ntdll";
    return true;
}

// FIPS 180-1 test vectors through the same code path as XcSHAInit/Update/Final.
FUT_SELFTEST(KernelSha1KnownAnswer, Host) {
    std::uint8_t d[20];
    krnl::xc::Sha1("abc", 3, d);
    if (Hex(d, 20) != "a9993e364706816aba3e25717850c26c9cd0d89d") {
        detail = "SHA1(abc) = " + Hex(d, 20);
        return false;
    }
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    krnl::xc::Sha1(m, std::strlen(m), d);
    if (Hex(d, 20) != "84983e441c3bd26ebaae4aa1f95129e5e54670f1") {
        detail = "SHA1(448-bit message) = " + Hex(d, 20);
        return false;
    }
    detail = "FIPS 180-1 vectors ok";
    return true;
}

// Every slot the kernel agent owns for M1 (non-file "M1 - Boot-Pfad" + "Defensiv" + all DATA/ADDR,
// design Anhang A / 1.5) has a registered implementation.
FUT_SELFTEST(KernelSlotsRegistered, Host) {
    static const char* const kNames[] = {
        "KfLowerIrql",
        "KeRaiseIrqlToDpcLevel",
        "RtlInitAnsiString",
        "KeSetEvent",
        "KeWaitForSingleObject",
        "KeInitializeTimerEx",
        "KeInitializeDpc",
        "IoInvalidDeviceRequest",
        "RtlLeaveCriticalSection",
        "RtlEnterCriticalSection",
        "NtFreeVirtualMemory",
        "NtAllocateVirtualMemory",
        "RtlCompareMemoryUlong",
        "RtlInitializeCriticalSection",
        "NtQueryVirtualMemory",
        "RtlRaiseException",
        "ExQueryNonVolatileSetting",
        "ExEventObjectType",
        "KeQuerySystemTime",
        "RtlNtStatusToDosError",
        "XcSHAUpdate",
        "XcSHAInit",
        "XcSHAFinal",
        "XboxHDKey",
        "XboxSignatureKey",
        "XboxAlternateSignatureKeys",
        "NtUserIoApcDispatcher",
        "HalDiskCachePartitionCount",
        "LaunchDataPage",
        "MmFreeContiguousMemory",
        "HalReturnToFirmware",
        "MmSetAddressProtect",
        "MmQueryAddressProtect",
        "KeSetTimer",
        "XboxHardwareInfo",
        "XcVerifyPKCS1Signature",
        "XePublicKeyData",
        "HalRegisterShutdownNotification",
        "XboxKrnlVersion",
        "PsTerminateSystemThread",
        "PsCreateSystemThreadEx",
        "HalInitiateShutdown",
        "RtlEqualString",
        "XeLoadSection",
        "XeUnloadSection",
        "IdexChannelObject",
        "RtlUnwind",
        "KeBugCheck",
        "HalBootSMCVideoMode",
    };
    std::string missing;
    for (const char* name : kNames) {
        bool found = false;
        for (std::uint32_t s = 0; s < krnl::gen::kSlotCount; ++s) {
            if (std::strcmp(krnl::gen::kExports[s].name, name) == 0) {
                found = true;
                if (!krnl::IsImplemented(s)) {
                    missing += missing.empty() ? name : std::string(", ") + name;
                }
            }
        }
        if (!found) {
            missing += std::string(missing.empty() ? "" : ", ") + "?" + name;
        }
    }
    if (!missing.empty()) {
        detail = "not registered: " + missing;
        return false;
    }
    detail = std::to_string(std::size(kNames)) + " kernel-agent slots registered";
    return true;
}

// On the adopted boot thread: IRQL round trip through the TLS slot exactly as the patched guest
// sites read it (byte at fs:[0xE10 + 4*index(S_IRQL)], design 1.6).
FUT_SELFTEST(KernelIrqlRoundTrip, AdoptedThread) {
    const std::uint32_t index = core::TlsSlotIndex(FUT_TLS_S_IRQL);
    const unsigned long offset = 0xE10ul + 4ul * index;
    const xbox::KIRQL old = krnl::KeRaiseIrqlToDpcLevel();
    const unsigned char viaFs = __readfsbyte(offset);
    const xbox::KIRQL raised = krnl::CurrentIrql();
    krnl::KfLowerIrql(old);
    const unsigned char afterFs = __readfsbyte(offset);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "old %u, raised %u (fs:[0x%lX] = %u), lowered fs = %u", old, raised,
                  offset, viaFs, afterFs);
    detail = msg;
    return old == 0 && raised == xbox::kIrqlDispatch && viaFs == xbox::kIrqlDispatch && afterFs == 0;
}
