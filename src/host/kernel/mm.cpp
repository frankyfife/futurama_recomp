// Memory manager exports (design 1.8): Nt*VirtualMemory, Mm*ContiguousMemory, MmQueryAllocationSize,
// Mm*AddressProtect, Mm*SystemMemory. Owned by the kernel agent.
//
// Rule (design 1.2): memory handed to the guest lies below 0x80000000 (checked after every
// allocation; violations are released again and reported as STATUS_NO_MEMORY / NULL with an error).
#include "core/guest.h"
#include "core/log.h"
#include "kernel/ke_types.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "shared/nt_status.h"

#include <unordered_map>

// ntdll native API (ntdll.lib is linked by src/host/CMakeLists.txt). The Xbox functions are the NT
// ones without the ProcessHandle argument, so forwarding keeps NT's in/out rounding and status codes.
extern "C" {
NTSYSAPI NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits,
                                                PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
NTSYSAPI NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize,
                                            ULONG FreeType);
NTSYSAPI NTSTATUS NTAPI NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                                             int MemoryInformationClass, PVOID MemoryInformation,
                                             SIZE_T MemoryInformationLength, PSIZE_T ReturnLength);
}

namespace krnl {
namespace {

constexpr ULONG kPageSize = 0x1000;
constexpr ULONG kAllocationGranularity = 0x10000;
// Xbox-only flags that Windows rejects or interprets differently.
constexpr ULONG kXboxMemNoZero = 0x800000;  // ASSUMPTION: Xbox MEM_NOZERO (Windows: MEM_ROTATE)
constexpr ULONG kWindowsAllocTypes = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN;
constexpr ULONG kCacheProtectFlags = PAGE_NOCACHE | PAGE_WRITECOMBINE;  // 0x200 | 0x400 (D3D: 0x404)
constexpr int kMemoryBasicInformation = 0;                              // MEMORY_INFORMATION_CLASS

inline const char* StatusName(NTSTATUS s) noexcept {
    const char* n = xbox::NtStatusName(s);
    return n ? n : "";
}

inline bool BelowGuestLimit(const void* base, SIZE_T size) noexcept {
    const std::uint64_t end = static_cast<std::uint64_t>(guest::Addr(base)) + size;
    return end <= guest::kGuestMemoryLimit;
}

ULONG HostProtect(ULONG protect) noexcept {
    const ULONG p = protect & ~kCacheProtectFlags;
    return p != 0 ? p : PAGE_READWRITE;
}

// --- contiguous / system memory bookkeeping ---
struct HostAllocation {
    void* allocationBase;  // what VirtualFree needs
    SIZE_T size;           // page-rounded size handed to the guest
};

SRWLOCK g_allocLock = SRWLOCK_INIT;

std::unordered_map<std::uint32_t, HostAllocation>& Allocations() {
    static auto* map = new std::unordered_map<std::uint32_t, HostAllocation>();  // process lifetime
    return *map;
}

// Allocates committed memory below 0x80000000 with the requested alignment (VirtualAlloc gives
// 64 KiB granularity, which covers every alignment the guest uses: <= 0x4000, kernel.md 7.1).
void* AllocateTracked(SIZE_T bytes, ULONG_PTR alignment, ULONG protect, const char* what) {
    if (bytes == 0) {
        return nullptr;
    }
    const SIZE_T size = (bytes + kPageSize - 1) & ~static_cast<SIZE_T>(kPageSize - 1);
    SIZE_T reserve = size;
    if (alignment > kAllocationGranularity) {
        reserve += alignment;
    }
    void* base = VirtualAlloc(nullptr, reserve, MEM_RESERVE | MEM_COMMIT, HostProtect(protect));
    if (base == nullptr) {
        FUT_LOGE(Krnl, "%s(0x%lX): VirtualAlloc failed (%lu)", what, static_cast<unsigned long>(bytes),
                 GetLastError());
        return nullptr;
    }
    std::uintptr_t user = reinterpret_cast<std::uintptr_t>(base);
    if (alignment > kAllocationGranularity) {
        user = (user + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    }
    if (!BelowGuestLimit(reinterpret_cast<void*>(user), size)) {
        FUT_LOGE(Krnl, "%s(0x%lX): host memory %p lies above 0x80000000 (design 1.2) - failing", what,
                 static_cast<unsigned long>(bytes), base);
        VirtualFree(base, 0, MEM_RELEASE);
        return nullptr;
    }
    AcquireSRWLockExclusive(&g_allocLock);
    try {
        Allocations()[static_cast<std::uint32_t>(user)] = HostAllocation{base, size};
    } catch (...) {
        ReleaseSRWLockExclusive(&g_allocLock);
        VirtualFree(base, 0, MEM_RELEASE);
        throw;
    }
    ReleaseSRWLockExclusive(&g_allocLock);
    return reinterpret_cast<void*>(user);
}

bool FindTracked(const void* p, HostAllocation& out) noexcept {
    AcquireSRWLockShared(&g_allocLock);
    bool found = false;
    const auto& map = Allocations();
    if (auto it = map.find(guest::Addr(p)); it != map.end()) {
        out = it->second;
        found = true;
    }
    ReleaseSRWLockShared(&g_allocLock);
    return found;
}

bool FreeTracked(const void* p, SIZE_T* sizeOut) noexcept {
    HostAllocation a{};
    AcquireSRWLockExclusive(&g_allocLock);
    bool found = false;
    auto& map = Allocations();
    if (auto it = map.find(guest::Addr(p)); it != map.end()) {
        a = it->second;
        map.erase(it);
        found = true;
    }
    ReleaseSRWLockExclusive(&g_allocLock);
    if (found) {
        VirtualFree(a.allocationBase, 0, MEM_RELEASE);
        guest::NoteMemoryReleased();  // M3d review finding 6: readable-page caches
        if (sizeOut) {
            *sizeOut = a.size;
        }
    }
    return found;
}

}  // namespace

// NtAllocateVirtualMemory - slot 12, Ord 184, stdcall, 5 args (purge 20):
// NTSTATUS (PVOID* BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect)
// Xbox signature without ProcessHandle (kernel.md 4). Evidence: kernel_imports.tsv (11 sites, all
// Protect 4); XAPI heap FUN_000f3e12/FUN_000f3fe6/FUN_000f4959/FUN_000f4a77/FUN_000f4ea8/FUN_000f52cc
// reserve (0x2000) then commit (0x1000) and read *BaseAddress / *RegionSize back; design 1.8 (in/out
// parameters, assertion < 0x80000000).
NTSTATUS __stdcall NtAllocateVirtualMemory(void** BaseAddress, ULONG_PTR ZeroBits, SIZE_T* RegionSize,
                                           ULONG AllocationType, ULONG Protect) noexcept {
    if (BaseAddress == nullptr || RegionSize == nullptr) {
        FUT_LOGE(Krnl, "NtAllocateVirtualMemory: NULL BaseAddress/RegionSize pointer");
        return STATUS_ACCESS_VIOLATION;
    }
    const void* requestedBase = *BaseAddress;
    const SIZE_T requestedSize = *RegionSize;
    ULONG type = AllocationType & ~kXboxMemNoZero;
    if ((type & ~kWindowsAllocTypes) != 0) {
        FUT_LOGW(Krnl, "NtAllocateVirtualMemory: unsupported AllocationType bits 0x%lX ignored",
                 type & ~kWindowsAllocTypes);
        type &= kWindowsAllocTypes;
    }
    NTSTATUS status = ::NtAllocateVirtualMemory(GetCurrentProcess(), BaseAddress, ZeroBits, RegionSize, type,
                                                HostProtect(Protect));
    if (NT_SUCCESS(status) && !BelowGuestLimit(*BaseAddress, *RegionSize)) {
        FUT_LOGE(Krnl,
                 "NtAllocateVirtualMemory: region %p+0x%lX lies above 0x80000000 (design 1.2) - failing",
                 *BaseAddress, static_cast<unsigned long>(*RegionSize));
        if (requestedBase == nullptr) {
            SIZE_T zero = 0;
            ::NtFreeVirtualMemory(GetCurrentProcess(), BaseAddress, &zero, MEM_RELEASE);
        }
        *BaseAddress = const_cast<void*>(requestedBase);
        *RegionSize = requestedSize;
        status = STATUS_NO_MEMORY;
    }
    if (!NT_SUCCESS(status) || core::LogEnabled(core::Channel::Krnl, core::Level::Trace)) {
        core::LogWrite(
            core::Channel::Krnl, NT_SUCCESS(status) ? core::Level::Trace : core::Level::Warn,
            "NtAllocateVirtualMemory(base %p, size 0x%lX, type 0x%lX, protect 0x%lX) -> 0x%08lX %s "
            "[%p, 0x%lX]",
            requestedBase, static_cast<unsigned long>(requestedSize), AllocationType, Protect,
            static_cast<unsigned long>(status), StatusName(status), *BaseAddress,
            static_cast<unsigned long>(*RegionSize));
    }
    return status;
}
KRNL_FUNC(NtAllocateVirtualMemory, &NtAllocateVirtualMemory);

// NtFreeVirtualMemory - slot 11, Ord 199, stdcall, 3 args (purge 12):
// NTSTATUS (PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType) - Xbox without ProcessHandle.
// Evidence: kernel_imports.tsv (6 XAPI sites; FreeType 0x8000 MEM_RELEASE with the reserved size
// and 0x4000 MEM_DECOMMIT in FUN_000f4bfe); design 1.8.
NTSTATUS __stdcall NtFreeVirtualMemory(void** BaseAddress, SIZE_T* RegionSize, ULONG FreeType) noexcept {
    if (BaseAddress == nullptr || RegionSize == nullptr) {
        FUT_LOGE(Krnl, "NtFreeVirtualMemory: NULL BaseAddress/RegionSize pointer");
        return STATUS_ACCESS_VIOLATION;
    }
    const void* base = *BaseAddress;
    const SIZE_T size = *RegionSize;
    const NTSTATUS status = ::NtFreeVirtualMemory(GetCurrentProcess(), BaseAddress, RegionSize, FreeType);
    if (NT_SUCCESS(status)) {
        guest::NoteMemoryReleased();  // release or decommit (M3d review finding 6: readable-page caches)
    }
    if (!NT_SUCCESS(status) || core::LogEnabled(core::Channel::Krnl, core::Level::Trace)) {
        core::LogWrite(core::Channel::Krnl, NT_SUCCESS(status) ? core::Level::Trace : core::Level::Warn,
                       "NtFreeVirtualMemory(base %p, size 0x%lX, type 0x%lX) -> 0x%08lX %s", base,
                       static_cast<unsigned long>(size), FreeType, static_cast<unsigned long>(status),
                       StatusName(status));
    }
    return status;
}
KRNL_FUNC(NtFreeVirtualMemory, &NtFreeVirtualMemory);

// NtQueryVirtualMemory - slot 15, Ord 217, stdcall, 2 args (purge 8):
// NTSTATUS (PVOID BaseAddress, PMEMORY_BASIC_INFORMATION MemoryInformation) - Xbox without
// ProcessHandle/class/length (kernel.md 4). Evidence: kernel_imports.tsv; heap FUN_000f4ea8 reads
// BaseAddress, RegionSize and State of the 0x1C-byte result (guestdef::MEMORY_BASIC_INFORMATION).
NTSTATUS __stdcall NtQueryVirtualMemory(void* BaseAddress,
                                        guestdef::MEMORY_BASIC_INFORMATION* MemoryInformation) noexcept {
    if (MemoryInformation == nullptr) {
        return STATUS_ACCESS_VIOLATION;
    }
    const NTSTATUS status = ::NtQueryVirtualMemory(GetCurrentProcess(), BaseAddress, kMemoryBasicInformation,
                                                   MemoryInformation, sizeof(*MemoryInformation), nullptr);
    FUT_LOGT(Krnl, "NtQueryVirtualMemory(%p) -> 0x%08lX [base 0x%08X size 0x%X state 0x%X protect 0x%X]",
             BaseAddress, static_cast<unsigned long>(status), MemoryInformation->BaseAddress,
             MemoryInformation->RegionSize, MemoryInformation->State, MemoryInformation->Protect);
    return status;
}
KRNL_FUNC(NtQueryVirtualMemory, &NtQueryVirtualMemory);

// MmAllocateContiguousMemoryEx - slot 56, Ord 166, stdcall, 5 args (purge 20):
// PVOID (SIZE_T NumberOfBytes, ULONG_PTR LowestAcceptableAddress, ULONG_PTR HighestAcceptableAddress,
// ULONG_PTR Alignment, ULONG ProtectionType). Evidence: kernel_imports.tsv; startup.md 5.1 table
// (XPhysicalAlloc 0xF8449 used by the movie player with PhysicalAddress -1, D3D with Protect 0x404);
// design 1.2/1.8: ordinary memory, no 0x80000000 window, address limits and WRITECOMBINE ignored.
void* __stdcall MmAllocateContiguousMemoryEx(SIZE_T NumberOfBytes, ULONG_PTR LowestAcceptableAddress,
                                             ULONG_PTR HighestAcceptableAddress, ULONG_PTR Alignment,
                                             ULONG ProtectionType) noexcept {
    void* p = nullptr;
    FUT_HLE_TRY {
        p = AllocateTracked(NumberOfBytes, Alignment, ProtectionType, "MmAllocateContiguousMemoryEx");
    }
    FUT_HLE_CATCH("MmAllocateContiguousMemoryEx")
    FUT_LOGD(
        Krnl,
        "MmAllocateContiguousMemoryEx(0x%lX, low 0x%08lX, high 0x%08lX, align 0x%lX, protect 0x%lX) -> %p",
        static_cast<unsigned long>(NumberOfBytes), static_cast<unsigned long>(LowestAcceptableAddress),
        static_cast<unsigned long>(HighestAcceptableAddress), static_cast<unsigned long>(Alignment),
        ProtectionType, p);
    return p;
}
KRNL_FUNC(MmAllocateContiguousMemoryEx, &MmAllocateContiguousMemoryEx);

// MmAllocateContiguousMemory - slot 52, Ord 165, stdcall, 1 arg (purge 4): PVOID (SIZE_T NumberOfBytes).
// Evidence: kernel_imports.tsv; FUN_000f81bb (LaunchDataPage, 0x1000 bytes), XPP 0x28CBC9;
// design 1.8. Same as the Ex variant with page alignment and PAGE_READWRITE.
void* __stdcall MmAllocateContiguousMemory(SIZE_T NumberOfBytes) noexcept {
    void* p = nullptr;
    FUT_HLE_TRY {
        p = AllocateTracked(NumberOfBytes, kPageSize, PAGE_READWRITE, "MmAllocateContiguousMemory");
    }
    FUT_HLE_CATCH("MmAllocateContiguousMemory")
    FUT_LOGD(Krnl, "MmAllocateContiguousMemory(0x%lX) -> %p", static_cast<unsigned long>(NumberOfBytes), p);
    return p;
}
KRNL_FUNC(MmAllocateContiguousMemory, &MmAllocateContiguousMemory);

// MmFreeContiguousMemory - slot 54, Ord 171, stdcall, 1 arg (purge 4): VOID (PVOID BaseAddress).
// Evidence: kernel_imports.tsv (17 sites); XapiInitProcess frees the LaunchDataPage (0xF9459),
// game FUN_000ee080 via XPhysicalFree 0xF8463, D3D stub 0x2417B0; design 1.8.
void __stdcall MmFreeContiguousMemory(void* BaseAddress) noexcept {
    SIZE_T size = 0;
    if (BaseAddress == nullptr) {
        FUT_LOGW(Krnl, "MmFreeContiguousMemory(NULL) ignored");
        return;
    }
    if (FreeTracked(BaseAddress, &size)) {
        FUT_LOGD(Krnl, "MmFreeContiguousMemory(%p) freed 0x%lX bytes", BaseAddress,
                 static_cast<unsigned long>(size));
    } else {
        FUT_LOGW(Krnl, "MmFreeContiguousMemory(%p): not a contiguous allocation - ignored", BaseAddress);
    }
}
KRNL_FUNC(MmFreeContiguousMemory, &MmFreeContiguousMemory);

// MmQueryAllocationSize - slot 57, Ord 180, stdcall, 1 arg (purge 4): SIZE_T (PVOID BaseAddress).
// Evidence: kernel_imports.tsv (only D3D/DSOUND, replaced libraries); kernel.md 7.1 (size from the
// own allocation table, fallback VirtualQuery).
SIZE_T __stdcall MmQueryAllocationSize(void* BaseAddress) noexcept {
    HostAllocation a{};
    if (FindTracked(BaseAddress, a)) {
        return a.size;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (BaseAddress != nullptr && VirtualQuery(BaseAddress, &mbi, sizeof(mbi)) == sizeof(mbi) &&
        mbi.State == MEM_COMMIT) {
        FUT_LOGD(Krnl, "MmQueryAllocationSize(%p): untracked, VirtualQuery region 0x%lX", BaseAddress,
                 static_cast<unsigned long>(mbi.RegionSize));
        return mbi.RegionSize;
    }
    FUT_LOGW(Krnl, "MmQueryAllocationSize(%p): unknown address -> 0", BaseAddress);
    return 0;
}
KRNL_FUNC(MmQueryAllocationSize, &MmQueryAllocationSize);

// MmPersistContiguousMemory - slot 51, Ord 178, stdcall, 3 args (purge 12):
// VOID (PVOID BaseAddress, SIZE_T NumberOfBytes, BOOLEAN Persist). Evidence: kernel_imports.tsv;
// FUN_000f81bb (LaunchDataPage before HalReturnToFirmware), D3D; design 1.8: no-op (memory never
// survives a "reboot" on the PC).
void __stdcall MmPersistContiguousMemory(void* BaseAddress, SIZE_T NumberOfBytes, BOOLEAN Persist) noexcept {
    FUT_LOGD(Krnl, "MmPersistContiguousMemory(%p, 0x%lX, %u): no-op (design 1.8)", BaseAddress,
             static_cast<unsigned long>(NumberOfBytes), static_cast<unsigned>(Persist));
}
KRNL_FUNC(MmPersistContiguousMemory, &MmPersistContiguousMemory);

// MmAllocateSystemMemory - slot 44, Ord 167, stdcall, 2 args (purge 8): PVOID (SIZE_T NumberOfBytes,
// ULONG Protect). Evidence: kernel_imports.tsv (XAPI copy routine FUN_000f786b, 64 KiB buffer,
// Protect 4); design 1.8 (VirtualAlloc). Planned for M7; implemented early because it is trivial.
void* __stdcall MmAllocateSystemMemory(SIZE_T NumberOfBytes, ULONG Protect) noexcept {
    void* p = nullptr;
    FUT_HLE_TRY {
        p = AllocateTracked(NumberOfBytes, kPageSize, Protect, "MmAllocateSystemMemory");
    }
    FUT_HLE_CATCH("MmAllocateSystemMemory")
    FUT_LOGD(Krnl, "MmAllocateSystemMemory(0x%lX, 0x%lX) -> %p", static_cast<unsigned long>(NumberOfBytes),
             Protect, p);
    return p;
}
KRNL_FUNC(MmAllocateSystemMemory, &MmAllocateSystemMemory);

// MmFreeSystemMemory - slot 42, Ord 172, stdcall, 2 args (purge 8): ULONG (PVOID BaseAddress,
// SIZE_T NumberOfBytes) -> number of pages freed. Evidence: kernel_imports.tsv (FUN_000f786b);
// design 1.8.
ULONG __stdcall MmFreeSystemMemory(void* BaseAddress, SIZE_T NumberOfBytes) noexcept {
    SIZE_T size = 0;
    if (!FreeTracked(BaseAddress, &size)) {
        FUT_LOGW(Krnl, "MmFreeSystemMemory(%p, 0x%lX): unknown allocation - ignored", BaseAddress,
                 static_cast<unsigned long>(NumberOfBytes));
        return 0;
    }
    FUT_LOGD(Krnl, "MmFreeSystemMemory(%p, 0x%lX)", BaseAddress, static_cast<unsigned long>(NumberOfBytes));
    return static_cast<ULONG>(size / kPageSize);
}
KRNL_FUNC(MmFreeSystemMemory, &MmFreeSystemMemory);

// MmQueryAddressProtect - slot 59, Ord 179, stdcall, 1 arg (purge 4): ULONG (PVOID VirtualAddress).
// Evidence: kernel_imports.tsv; FUN_000f9804 (kernel hotfix, not reached with build 5838) and XPP
// FUN_0028eaa7; design 1.8 (VirtualQuery). Returns 0 for addresses that are not committed.
ULONG __stdcall MmQueryAddressProtect(void* VirtualAddress) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    ULONG protect = 0;
    if (VirtualQuery(VirtualAddress, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT) {
        protect = mbi.Protect;
    }
    FUT_LOGD(Krnl, "MmQueryAddressProtect(%p) -> 0x%lX", VirtualAddress, protect);
    return protect;
}
KRNL_FUNC(MmQueryAddressProtect, &MmQueryAddressProtect);

// MmSetAddressProtect - slot 58, Ord 182, stdcall, 3 args (purge 12):
// VOID (PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect). Evidence: kernel_imports.tsv;
// FUN_000f9804 (MmSetAddressProtect(p, 4, 4) and back), XPP FUN_0028eaa7 (0x20 bytes); design 1.8.
// Inside the XBE image execute permission is kept (design 1.4 step 7: M1 keeps the image RWX).
void __stdcall MmSetAddressProtect(void* BaseAddress, ULONG NumberOfBytes, ULONG NewProtect) noexcept {
    ULONG protect = HostProtect(NewProtect);
    const guest::GuestAddr addr = guest::Addr(BaseAddress);
    if (addr < guest::kXbeSpaceEnd && addr + NumberOfBytes > guest::kXbeBase) {
        if (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY) {
            protect = PAGE_EXECUTE_READWRITE;
        } else if (protect == PAGE_READONLY) {
            protect = PAGE_EXECUTE_READ;
        }
    }
    DWORD old = 0;
    if (!VirtualProtect(BaseAddress, NumberOfBytes, protect, &old)) {
        FUT_LOGW(Krnl, "MmSetAddressProtect(%p, 0x%lX, 0x%lX): VirtualProtect failed (%lu)", BaseAddress,
                 NumberOfBytes, NewProtect, GetLastError());
        return;
    }
    FUT_LOGD(Krnl, "MmSetAddressProtect(%p, 0x%lX, 0x%lX) host 0x%lX (was 0x%lX)", BaseAddress, NumberOfBytes,
             NewProtect, protect, old);
}
KRNL_FUNC(MmSetAddressProtect, &MmSetAddressProtect);

}  // namespace krnl
