// RtlNtStatusToDosError (slot 24) and the status table. Owned by the kernel agent.
#include "core/log.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_hle.h"
#include "kernel/kernel_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>

namespace krnl {
namespace {

struct StatusMapping {
    std::uint32_t status;
    std::uint32_t error;
};

// Every NTSTATUS the kernel HLE (Ke/Mm/Rtl/Ps/Xe), the VFS layer (Nt*File, Io*) or the forwarded
// ntdll memory functions can hand to the guest, with the Win32 error the guest sees through
// XapiSetLastNTError 0xF6EAA / XQueryValue 0xF8AC0 / GetOverlappedResult / the APC dispatcher.
// Values generated from ntdll!RtlNtStatusToDosErrorNoTeb (Windows 11 26200) and cross-checked at
// run time by the self-test KernelStatusTableMatchesNtdll; the Xbox kernel uses the NT table
// (ASSUMPTION: no Xbox-specific deviations for these codes). Sorted by status for binary search.
constexpr StatusMapping kStatusTable[] = {
    {0x00000000, 0},     // STATUS_SUCCESS
    {0x00000001, 731},   // STATUS_WAIT_1
    {0x000000C0, 737},   // STATUS_USER_APC
    {0x00000101, 739},   // STATUS_ALERTED
    {0x00000102, 1460},  // STATUS_TIMEOUT
    {0x00000103, 997},   // STATUS_PENDING -> ERROR_IO_PENDING
    {0x40000000, 698},   // STATUS_OBJECT_NAME_EXISTS
    {0x40000024, 721},   // STATUS_NO_YIELD_PERFORMED
    {0x80000005, 234},   // STATUS_BUFFER_OVERFLOW -> ERROR_MORE_DATA
    {0x80000006, 18},    // STATUS_NO_MORE_FILES
    {0x80000011, 170},   // STATUS_DEVICE_BUSY
    {0x8000001A, 259},   // STATUS_NO_MORE_ENTRIES
    {0xC0000001, 31},    // STATUS_UNSUCCESSFUL -> ERROR_GEN_FAILURE
    {0xC0000002, 1},     // STATUS_NOT_IMPLEMENTED -> ERROR_INVALID_FUNCTION
    {0xC0000003, 87},    // STATUS_INVALID_INFO_CLASS
    {0xC0000004, 24},    // STATUS_INFO_LENGTH_MISMATCH
    {0xC0000005, 998},   // STATUS_ACCESS_VIOLATION -> ERROR_NOACCESS
    {0xC0000008, 6},     // STATUS_INVALID_HANDLE
    {0xC000000C, 541},   // STATUS_TIMER_NOT_CANCELED
    {0xC000000D, 87},    // STATUS_INVALID_PARAMETER
    {0xC000000E, 433},   // STATUS_NO_SUCH_DEVICE
    {0xC000000F, 2},     // STATUS_NO_SUCH_FILE
    {0xC0000010, 1},     // STATUS_INVALID_DEVICE_REQUEST
    {0xC0000011, 38},    // STATUS_END_OF_FILE -> ERROR_HANDLE_EOF
    {0xC0000013, 21},    // STATUS_NO_MEDIA_IN_DEVICE -> ERROR_NOT_READY
    {0xC0000014, 1785},  // STATUS_UNRECOGNIZED_MEDIA
    {0xC0000017, 8},     // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
    {0xC0000018, 487},   // STATUS_CONFLICTING_ADDRESSES -> ERROR_INVALID_ADDRESS
    {0xC0000019, 487},   // STATUS_NOT_MAPPED_VIEW
    {0xC000001A, 87},    // STATUS_UNABLE_TO_FREE_VM
    {0xC000001C, 1},     // STATUS_INVALID_SYSTEM_SERVICE
    {0xC0000022, 5},     // STATUS_ACCESS_DENIED
    {0xC0000023, 122},   // STATUS_BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER
    {0xC0000024, 6},     // STATUS_OBJECT_TYPE_MISMATCH
    {0xC000002D, 487},   // STATUS_NOT_COMMITTED
    {0xC0000030, 87},    // STATUS_INVALID_PARAMETER_MIX
    {0xC0000032, 1393},  // STATUS_DISK_CORRUPT_ERROR
    {0xC0000033, 123},   // STATUS_OBJECT_NAME_INVALID -> ERROR_INVALID_NAME
    {0xC0000034, 2},     // STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
    {0xC0000035, 183},   // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
    {0xC0000039, 161},   // STATUS_OBJECT_PATH_INVALID -> ERROR_BAD_PATHNAME
    {0xC000003A, 3},     // STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
    {0xC000003B, 161},   // STATUS_OBJECT_PATH_SYNTAX_BAD
    {0xC000003C, 1117},  // STATUS_DATA_OVERRUN
    {0xC000003E, 23},    // STATUS_DATA_ERROR -> ERROR_CRC
    {0xC000003F, 23},    // STATUS_CRC_ERROR
    {0xC0000043, 32},    // STATUS_SHARING_VIOLATION
    {0xC0000045, 87},    // STATUS_INVALID_PAGE_PROTECTION
    {0xC0000046, 288},   // STATUS_MUTANT_NOT_OWNED
    {0xC0000047, 298},   // STATUS_SEMAPHORE_LIMIT_EXCEEDED
    {0xC0000054, 33},    // STATUS_FILE_LOCK_CONFLICT
    {0xC0000055, 33},    // STATUS_LOCK_NOT_GRANTED
    {0xC0000056, 5},     // STATUS_DELETE_PENDING
    {0xC000007B, 193},   // STATUS_INVALID_IMAGE_FORMAT
    {0xC000007F, 112},   // STATUS_DISK_FULL
    {0xC0000086, 154},   // STATUS_INVALID_VOLUME_LABEL
    {0xC0000095, 534},   // STATUS_INTEGER_OVERFLOW
    {0xC000009A, 1450},  // STATUS_INSUFFICIENT_RESOURCES
    {0xC000009C, 23},    // STATUS_DEVICE_DATA_ERROR
    {0xC000009F, 487},   // STATUS_FREE_VM_NOT_AT_BASE
    {0xC00000A0, 487},   // STATUS_MEMORY_NOT_ALLOCATED
    {0xC00000A2, 19},    // STATUS_MEDIA_WRITE_PROTECTED
    {0xC00000A3, 21},    // STATUS_DEVICE_NOT_READY
    {0xC00000B5, 121},   // STATUS_IO_TIMEOUT
    {0xC00000BA, 5},     // STATUS_FILE_IS_A_DIRECTORY
    {0xC00000BB, 50},    // STATUS_NOT_SUPPORTED
    {0xC00000C0, 55},    // STATUS_DEVICE_DOES_NOT_EXIST
    {0xC00000D4, 17},    // STATUS_NOT_SAME_DEVICE
    {0xC00000E5, 1359},  // STATUS_INTERNAL_ERROR
    {0xC00000E8, 1784},  // STATUS_INVALID_USER_BUFFER
    {0xC00000E9, 1117},  // STATUS_UNEXPECTED_IO_ERROR
    {0xC00000EF, 87},    // STATUS_INVALID_PARAMETER_1
    {0xC00000F0, 87},    // STATUS_INVALID_PARAMETER_2
    {0xC00000F1, 87},    // STATUS_INVALID_PARAMETER_3
    {0xC00000F2, 87},    // STATUS_INVALID_PARAMETER_4
    {0xC00000F3, 87},    // STATUS_INVALID_PARAMETER_5
    {0xC0000101, 145},   // STATUS_DIRECTORY_NOT_EMPTY
    {0xC0000102, 1392},  // STATUS_FILE_CORRUPT_ERROR
    {0xC0000103, 267},   // STATUS_NOT_A_DIRECTORY -> ERROR_DIRECTORY
    {0xC0000106, 206},   // STATUS_NAME_TOO_LONG
    {0xC000010A, 5},     // STATUS_PROCESS_IS_TERMINATING
    {0xC000011F, 4},     // STATUS_TOO_MANY_OPENED_FILES
    {0xC0000120, 995},   // STATUS_CANCELLED -> ERROR_OPERATION_ABORTED
    {0xC0000121, 5},     // STATUS_CANNOT_DELETE
    {0xC0000123, 5},     // STATUS_FILE_DELETED
    {0xC0000128, 6},     // STATUS_FILE_CLOSED
    {0xC0000141, 59},    // STATUS_INVALID_ADDRESS
    {0xC0000144, 574},   // STATUS_UNHANDLED_EXCEPTION
    {0xC0000148, 124},   // STATUS_INVALID_LEVEL
    {0xC000014F, 1005},  // STATUS_UNRECOGNIZED_VOLUME
    {0xC0000156, 1381},  // STATUS_TOO_MANY_SECRETS (FUN_000f902f, DVD check)
    {0xC0000184, 22},    // STATUS_INVALID_DEVICE_STATE
    {0xC0000185, 1117},  // STATUS_IO_DEVICE_ERROR
    {0xC0000190, 1790},  // STATUS_TRUST_FAILURE (FUN_000f902f, DVD check)
    {0xC0000225, 1168},  // STATUS_NOT_FOUND
    {0xC000026E, 21},    // STATUS_VOLUME_DISMOUNTED
};

constexpr bool IsSorted() {
    for (std::size_t i = 1; i < std::size(kStatusTable); ++i) {
        if (kStatusTable[i - 1].status >= kStatusTable[i].status) {
            return false;
        }
    }
    return true;
}
static_assert(IsSorted(), "kStatusTable must be sorted by status (binary search)");

using NoTebFn = ULONG(NTAPI*)(NTSTATUS);

NoTebFn NtdllNoTeb() noexcept {
    static NoTebFn fn = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NoTebFn>(GetProcAddress(ntdll, "RtlNtStatusToDosErrorNoTeb"))
                     : nullptr;
    }();
    return fn;
}

}  // namespace

std::uint32_t StatusTableSize() noexcept {
    return static_cast<std::uint32_t>(std::size(kStatusTable));
}
void StatusTableEntry(std::uint32_t index, std::uint32_t& status, std::uint32_t& error) noexcept {
    status = kStatusTable[index].status;
    error = kStatusTable[index].error;
}

std::uint32_t NtStatusToDosError(NTSTATUS status) noexcept {
    const auto s = static_cast<std::uint32_t>(status);
    const auto* it = std::lower_bound(std::begin(kStatusTable), std::end(kStatusTable), s,
                                      [](const StatusMapping& m, std::uint32_t v) { return m.status < v; });
    if (it != std::end(kStatusTable) && it->status == s) {
        return it->error;
    }
    // FACILITY_NTWIN32 (0x?007xxxx) carries the Win32 code in the low word (NT rule).
    if ((s & 0x0FFF0000u) == 0x00070000u) {
        return s & 0xFFFFu;
    }
    std::uint32_t result = 317;  // ERROR_MR_MID_NOT_FOUND, NT's answer for unknown codes
    if (NoTebFn fn = NtdllNoTeb()) {
        result = fn(status);
    }
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 32) {
        FUT_LOGD(Krnl, "RtlNtStatusToDosError: status 0x%08X not in the table -> %u (ntdll)", s, result);
    }
    return result;
}

// RtlNtStatusToDosError - slot 24, Ord 301, stdcall, 1 arg (purge 4): ULONG (NTSTATUS Status).
// Evidence: kernel_imports.tsv; callers XapiSetLastNTError 0xF6EAA (every XAPI failure path),
// XQueryValue 0xF8AC0 (result = return value, FUN_000f9b1a tests it against 0), XCreateSaveGame
// paths (0xF6BD0); kernel.md 7 (ntdll-identical).
ULONG __stdcall RtlNtStatusToDosError(NTSTATUS Status) noexcept {
    const std::uint32_t error = NtStatusToDosError(Status);
    FUT_LOGT(Krnl, "RtlNtStatusToDosError(0x%08lX) -> %u", static_cast<unsigned long>(Status), error);
    return error;
}
KRNL_FUNC(RtlNtStatusToDosError, &RtlNtStatusToDosError);

}  // namespace krnl
