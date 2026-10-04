// DATA and ADDR exports (design 1.5 table, kernel.md 5) and the save-signature keys (design 4.5).
// Owned by the kernel agent.
//
// The objects live in this DLL's .data (writable). The thunk slot of a DATA export receives the
// object's address (the guest dereferences the slot, e.g. `mov eax,[0x29368c]; movzx ecx,word [eax+4]`).
#include "core/guest.h"
#include "core/log.h"
#include "kernel/ke_types.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_hle.h"
#include "kernel/kernel_internal.h"
#include "kernel/xc_crypto.h"
#include "shared/xbe_format.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace krnl {
namespace {

constexpr std::uint32_t kKeySize = 16;
constexpr std::uint32_t kAlternateKeyCount = 16;

// --- the export objects ---------------------------------------------------------------------------
// slot 18 ExEventObjectType: POBJECT_TYPE, only pushed by XPP (0x28DA06) as the ObjectType argument
// of ObReferenceObjectByHandle (replaced library) -> zeroed dummy (Xbox OBJECT_TYPE is 0x1C bytes).
alignas(16) std::uint8_t g_ExEventObjectType[0x40] = {};
// slot 30 XboxHDKey: 16 bytes, MUST NOT be all zero - otherwise FUN_000f6ffa reads the whole EEPROM
// with XQueryValue(0xFFFF) (kernel.md 5, Verifikation). Only used for non-roamable signatures.
alignas(16) std::uint8_t g_XboxHDKey[kKeySize] = {};
// slot 31 XboxSignatureKey: 16 bytes, HMAC key of the save signatures (XCalculateSignatureBeginEx
// 0xF706F `mov eax,[0x2935dc]`, files.md 4.3).
alignas(16) std::uint8_t g_XboxSignatureKey[kKeySize] = {};
// slot 32 XboxAlternateSignatureKeys: 16 x 16 bytes (`add eax,[0x2935e0]` = &Keys[i], 0xF70C6).
alignas(16) std::uint8_t g_XboxAlternateSignatureKeys[kAlternateKeyCount][kKeySize] = {};
// slot 47 HalDiskCachePartitionCount: DWORD, read only by XapiSelectCachePartition (0xF7DD4/0xF7E2B)
// below XMountUtilityDrive, which is replaced by a hook. ASSUMPTION: 3 (design 1.5).
std::uint32_t g_HalDiskCachePartitionCount = 3;
// slot 53 LaunchDataPage: PLAUNCH_DATA_PAGE variable = NULL; XapiInitProcess reads/clears it
// (0xF9409/0xF9459), FUN_000f81bb sets it before a relaunch.
std::uint32_t g_LaunchDataPage = 0;
// slot 64 XboxHardwareInfo: Flags = 0 (bit 3 MUST be 0, otherwise no D: link and no NV queries,
// files.md 5.5.7; bit 1 selects the kernel-hotfix address in FUN_000f9804), other bytes 0 (design 1.5).
guestdef::XBOX_HARDWARE_INFO g_XboxHardwareInfo = {0, 0, 0, {0, 0}};
// slot 73 XePublicKeyData: only passed to XcVerifyPKCS1Signature by FUN_000f902f -> zeroed dummy
// (an Xbox RSA public key blob is 0x11C bytes).
alignas(16) std::uint8_t g_XePublicKeyData[0x200] = {};
// slot 75 XboxKrnlVersion {1, 0, 5838, 1}: Build must not be 3944/4034/4039/4817/4818, otherwise
// FUN_000f9804 patches "kernel code" at 0x800299A0..0x800398CD (kernel.md 8.2, design 1.5).
guestdef::XBOX_KRNL_VERSION g_XboxKrnlVersion = {1, 0, 5838, 1};
// slot 87 IdexChannelObject: zeroed buffer >= 0x24 bytes; sub_0f9c7f writes +0x10/+0x14 and waits only
// if +0x20 != 0, sub_0f9c63 clears +0x20 (kernel.md 5/8.7). Design 1.5: >= 0x40 bytes.
alignas(16) std::uint8_t g_IdexChannelObject[0x100] = {};
// slot 110 HalBootSMCVideoMode: DWORD returned by XGetAVPack 0x170AA5 (used by XGetAudioFlags in
// DSOUND only): [settings] av_pack, default 6 (design 1.5, ASSUMPTION).
std::uint32_t g_HalBootSMCVideoMode = 6;

std::atomic<bool> g_mappedDone{false};
SRWLOCK g_keyLock = SRWLOCK_INIT;

void DeriveFixedKey(const char* label, std::uint8_t out[kKeySize]) noexcept {
    std::uint8_t digest[xc::kShaDigestSize];
    xc::Sha1(label, std::strlen(label), digest);
    std::memcpy(out, digest, kKeySize);
}

bool AllZero(const std::uint8_t* p, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

void HexKey(const std::uint8_t* key, char out[2 * kKeySize + 1]) noexcept {
    for (std::uint32_t i = 0; i < kKeySize; ++i) {
        std::snprintf(out + 2 * i, 3, "%02X", key[i]);
    }
}

bool BelowGuestLimit(const void* p, std::size_t size) noexcept {
    return static_cast<std::uint64_t>(guest::Addr(p)) + size <= guest::kGuestMemoryLimit;
}

}  // namespace

// ---- registration of the DATA exports (design 1.5 table) ----
KRNL_DATA(ExEventObjectType, &g_ExEventObjectType);
KRNL_DATA(XboxHDKey, &g_XboxHDKey);
KRNL_DATA(XboxSignatureKey, &g_XboxSignatureKey);
KRNL_DATA(XboxAlternateSignatureKeys, &g_XboxAlternateSignatureKeys);
KRNL_DATA(HalDiskCachePartitionCount, &g_HalDiskCachePartitionCount);
KRNL_DATA(LaunchDataPage, &g_LaunchDataPage);
KRNL_DATA(XboxHardwareInfo, &g_XboxHardwareInfo);
KRNL_DATA(XePublicKeyData, &g_XePublicKeyData);
KRNL_DATA(XboxKrnlVersion, &g_XboxKrnlVersion);
KRNL_DATA(IdexChannelObject, &g_IdexChannelObject);
KRNL_DATA(HalBootSMCVideoMode, &g_HalBootSMCVideoMode);

// IoInvalidDeviceRequest - slot 8, Ord 74, ADDR: NTSTATUS stdcall (PDEVICE_OBJECT, PIRP) (PDRIVER_DISPATCH).
// Evidence: kernel.md 5 / startup.md 2.4: the XPP initializer 0x28D254 (runs in _cinit) copies the
// slot value into 10 dispatch slots 0x28C004..0x28C038; XPP is replaced, so it is never called.
// ASSUMPTION: the IRP is not touched (its Xbox layout is not needed); returns
// STATUS_INVALID_DEVICE_REQUEST like the Xbox routine.
NTSTATUS __stdcall IoInvalidDeviceRequest(void* DeviceObject, void* Irp) noexcept {
    FUT_LOGW(Krnl, "IoInvalidDeviceRequest(device %p, irp %p) ret=%p -> STATUS_INVALID_DEVICE_REQUEST",
             DeviceObject, Irp, _ReturnAddress());
    return STATUS_INVALID_DEVICE_REQUEST;
}
KRNL_ADDR(IoInvalidDeviceRequest, &IoInvalidDeviceRequest);

// NtUserIoApcDispatcher - slot 39, Ord 232, ADDR: VOID stdcall (PVOID ApcContext,
// PIO_STATUS_BLOCK IoStatusBlock, ULONG Reserved) (PIO_APC_ROUTINE, ret 0xC).
// Evidence: kernel.md 5/8.5: ReadFileEx 0xF7625 / WriteFileEx 0xF7676 push [0x2935FC] as ApcRoutine,
// ApcContext = lpCompletionRoutine and IoStatusBlock = lpOverlapped (files.md 5.1 item 2); the game's
// completions 0x13F1D0 / 0xE01A0 take (dwErrorCode, dwNumberOfBytesTransfered, lpOverlapped) with
// `ret 0xC`. Error mapping as in Win32 BasepIoCompletion: NT_ERROR(Status) -> RtlNtStatusToDosError,
// else 0 (design 1.7; ASSUMPTION for the Xbox, the game only tests err != 0).
// Marker + callable routine: the io agent's NtReadFile/NtWriteFile compare ApcRoutine with this
// address and queue {ApcContext, IoStatusBlock} for delivery in alertable waits (M2).
void __stdcall NtUserIoApcDispatcher(void* ApcContext, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                     std::uint32_t Reserved) noexcept {
    if (ApcContext == nullptr || IoStatusBlock == nullptr) {
        FUT_LOGE(Krnl, "NtUserIoApcDispatcher(%p, %p, %u): NULL completion routine or IoStatusBlock",
                 ApcContext, static_cast<void*>(IoStatusBlock), Reserved);
        return;
    }
    const NTSTATUS status = IoStatusBlock->Status;
    const bool isError = (static_cast<std::uint32_t>(status) >> 30) == 3;  // NT_ERROR
    const std::uint32_t error = isError ? NtStatusToDosError(status) : 0;
    FUT_LOGT(Krnl,
             "NtUserIoApcDispatcher: completion %p(err %u, bytes 0x%X, overlapped %p) for status 0x%08lX",
             ApcContext, error, IoStatusBlock->Information, static_cast<void*>(IoStatusBlock),
             static_cast<unsigned long>(status));
    using CompletionRoutine = void(__stdcall*)(std::uint32_t dwErrorCode,
                                               std::uint32_t dwNumberOfBytesTransfered, void* lpOverlapped);
    reinterpret_cast<CompletionRoutine>(ApcContext)(error, IoStatusBlock->Information, IoStatusBlock);
}
KRNL_ADDR(NtUserIoApcDispatcher, &NtUserIoApcDispatcher);

bool InitDataExports(const KernelSettings& settings) noexcept {
    AcquireSRWLockExclusive(&g_keyLock);
    // XboxHDKey: fixed project value (only used for non-roamable signatures, which the game never
    // creates; files.md 4.3).
    DeriveFixedKey("FuturamaPC/XboxHDKey/v1", g_XboxHDKey);
    if (AllZero(g_XboxHDKey, kKeySize)) {
        g_XboxHDKey[0] = 1;
    }
    // XboxSignatureKey: design 4.5 key mode "fixed" (default) = constant project key
    // SHA-1("FuturamaPC/XboxSignatureKey/fixed/v1")[0..15] or [saves] signature_key; mode "xbox"
    // derives it from the certificate in OnXbeMapped().
    if (settings.hasSignatureKeyOverride) {
        std::memcpy(g_XboxSignatureKey, settings.signatureKeyOverride.data(), kKeySize);
    } else {
        DeriveFixedKey("FuturamaPC/XboxSignatureKey/fixed/v1", g_XboxSignatureKey);
    }
    for (std::uint32_t i = 0; i < kAlternateKeyCount; ++i) {
        char label[64];
        std::snprintf(label, sizeof(label), "FuturamaPC/XboxAlternateSignatureKeys/fixed/v1/%u", i);
        DeriveFixedKey(label, g_XboxAlternateSignatureKeys[i]);
    }
    g_HalBootSMCVideoMode = settings.avPack;
    g_LaunchDataPage = 0;
    g_mappedDone.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_keyLock);

    struct Obj {
        const char* name;
        const void* p;
        std::size_t size;
    };
    const Obj objects[] = {
        {"ExEventObjectType", g_ExEventObjectType, sizeof(g_ExEventObjectType)},
        {"XboxHDKey", g_XboxHDKey, sizeof(g_XboxHDKey)},
        {"XboxSignatureKey", g_XboxSignatureKey, sizeof(g_XboxSignatureKey)},
        {"XboxAlternateSignatureKeys", g_XboxAlternateSignatureKeys, sizeof(g_XboxAlternateSignatureKeys)},
        {"HalDiskCachePartitionCount", &g_HalDiskCachePartitionCount, sizeof(g_HalDiskCachePartitionCount)},
        {"LaunchDataPage", &g_LaunchDataPage, sizeof(g_LaunchDataPage)},
        {"XboxHardwareInfo", &g_XboxHardwareInfo, sizeof(g_XboxHardwareInfo)},
        {"XePublicKeyData", g_XePublicKeyData, sizeof(g_XePublicKeyData)},
        {"XboxKrnlVersion", &g_XboxKrnlVersion, sizeof(g_XboxKrnlVersion)},
        {"IdexChannelObject", g_IdexChannelObject, sizeof(g_IdexChannelObject)},
        {"HalBootSMCVideoMode", &g_HalBootSMCVideoMode, sizeof(g_HalBootSMCVideoMode)},
    };
    bool ok = true;
    for (const Obj& o : objects) {
        if (!BelowGuestLimit(o.p, o.size)) {
            FUT_LOGE(Krnl, "data export %s at %p lies above 0x80000000 (design 1.2)", o.name, o.p);
            ok = false;
        }
    }
    return ok;
}

void OnXbeMapped() noexcept {
    if (g_mappedDone.load(std::memory_order_acquire)) {
        return;
    }
    AcquireSRWLockExclusive(&g_keyLock);
    if (!g_mappedDone.load(std::memory_order_relaxed)) {
        const KernelSettings& s = Settings();
        if (s.keyMode == SaveKeyMode::Xbox) {
            if (!guest::IsReadable(guest::kCertificate, sizeof(xbe::Certificate))) {
                FUT_LOGE(
                    Krnl,
                    "[saves] key_mode = xbox: XBE certificate at 0x%08X not mapped - keeping the fixed keys",
                    guest::kCertificate);
            } else {
                // ASSUMPTION (design 4.5, after Cxbx/xemu): key = HMAC-SHA1(XboxCERTKey,
                // cert.SignatureKey)[0..15], alternate keys likewise from cert.AlternateSignatureKeys[i].
                const auto* cert = guest::Ptr<const xbe::Certificate>(guest::kCertificate);
                std::uint8_t digest[xc::kShaDigestSize];
                xc::HmacSha1(s.xboxCertKey.data(), kKeySize, cert->SignatureKey, kKeySize, digest);
                std::memcpy(g_XboxSignatureKey, digest, kKeySize);
                for (std::uint32_t i = 0; i < kAlternateKeyCount; ++i) {
                    xc::HmacSha1(s.xboxCertKey.data(), kKeySize, cert->AlternateSignatureKeys[i], kKeySize,
                                 digest);
                    std::memcpy(g_XboxAlternateSignatureKeys[i], digest, kKeySize);
                }
                char hex[2 * kKeySize + 1];
                HexKey(g_XboxSignatureKey, hex);
                FUT_LOGI(Krnl, "XboxSignatureKey derived from XboxCERTKey and the certificate: %s", hex);
            }
        }
        g_mappedDone.store(true, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_keyLock);
}

void LogDataExports() noexcept {
    char sig[2 * kKeySize + 1];
    char hd[2 * kKeySize + 1];
    HexKey(g_XboxSignatureKey, sig);
    HexKey(g_XboxHDKey, hd);
    FUT_LOGI(
        Krnl,
        "data exports: XboxKrnlVersion %u.%u.%u.%u, XboxHardwareInfo.Flags 0x%X, HalBootSMCVideoMode %u, "
        "HalDiskCachePartitionCount %u, LaunchDataPage 0x%08X",
        g_XboxKrnlVersion.Major, g_XboxKrnlVersion.Minor, g_XboxKrnlVersion.Build, g_XboxKrnlVersion.Qfe,
        g_XboxHardwareInfo.Flags, g_HalBootSMCVideoMode, g_HalDiskCachePartitionCount, g_LaunchDataPage);
    FUT_LOGI(Krnl, "save keys: mode %s, XboxSignatureKey %s, XboxHDKey %s",
             Settings().keyMode == SaveKeyMode::Xbox ? "xbox (derived after mapping)" : "fixed", sig, hd);
    FUT_LOGD(Krnl, "data export addresses: ExEventObjectType %p, XePublicKeyData %p, IdexChannelObject %p",
             static_cast<void*>(g_ExEventObjectType), static_cast<void*>(g_XePublicKeyData),
             static_cast<void*>(g_IdexChannelObject));
}

const std::uint8_t* DataXboxHDKey() noexcept {
    return g_XboxHDKey;
}
const std::uint8_t* DataXboxSignatureKey() noexcept {
    return g_XboxSignatureKey;
}
const std::uint8_t* DataXboxAlternateSignatureKeys() noexcept {
    return &g_XboxAlternateSignatureKeys[0][0];
}

}  // namespace krnl
