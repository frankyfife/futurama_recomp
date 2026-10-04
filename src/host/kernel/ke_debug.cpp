// KeBugCheck, DbgPrint, DbgBreakPoint (slots 106, 98, 109). Owned by the kernel agent.
#include "core/crash.h"
#include "core/guest.h"
#include "core/log.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_hle.h"
#include "kernel/kernel_internal.h"

#include <intrin.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma intrinsic(_AddressOfReturnAddress)

namespace krnl {
namespace {

const char* BugCheckName(ULONG code) noexcept {
    switch (code) {
    case 0x00000000:
        return "fiber routine returned / CRT doexit";  // 0x1708A9, 0x169FBE
    case 0x0000000A:
        return "IRQL_NOT_LESS_OR_EQUAL (_getptd at IRQL >= 2)";  // 0x1692DE, 0x16936C
    case 0xC0000144:
        return "STATUS_UNHANDLED_EXCEPTION (CRT terminate)";  // terminate 0x169D5E
    default:
        return "unknown code";
    }
}

std::string SymbolizeNoThrow(std::uint32_t addr) noexcept {
    try {
        return guest::Symbolize(addr);
    } catch (...) {
        return {};
    }
}

void __cdecl IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
}

}  // namespace

// KeBugCheck - slot 106, Ord 95, stdcall, 1 arg (purge 4): VOID (ULONG BugCheckCode), no return.
// Evidence: kernel_imports.tsv; sites with codes 0xA (_getptd 0x1692DE/0x16936C), 0xC0000144
// (terminate 0x169D5E via the CRT filter 0x16ED1E), 0 (0x169FBE doexit, fiber trampoline 0x1708A9);
// startup.md 8; design 1.11: log code + backtrace (core crash API), then abort (exit code 3).
// Frame pointers are forced on so that [ebp] of this function is the guest caller's EBP.
#pragma optimize("y", off)
void __stdcall KeBugCheck(ULONG BugCheckCode) noexcept {
    void** returnSlot = static_cast<void**>(_AddressOfReturnAddress());
    const std::uint32_t eip = guest::Addr(*returnSlot);
    std::uint32_t ebp = 0;
    std::memcpy(&ebp, reinterpret_cast<const std::uint8_t*>(returnSlot) - sizeof(std::uint32_t), sizeof(ebp));
    const std::string caller = SymbolizeNoThrow(eip);
    FUT_LOGE(Krnl, "KeBugCheck(0x%08lX: %s) called from %s, IRQL %u, thread %lu", BugCheckCode,
             BugCheckName(BugCheckCode), caller.c_str(), CurrentIrql(), GetCurrentThreadId());
    core::LogBacktrace(ebp, eip);
    core::Fatal("KeBugCheck(0x%08lX: %s) from %s", BugCheckCode, BugCheckName(BugCheckCode), caller.c_str());
}
#pragma optimize("", on)
KRNL_FUNC(KeBugCheck, &KeBugCheck);

// DbgPrint - slot 98, Ord 8, cdecl variadic: ULONG (PCSZ Format, ...) -> STATUS_SUCCESS.
// Evidence: kernel_imports.tsv; single site D3D FUN_0023e600 0x23E627 (1 push, caller cleans with
// `add esp,0x118`, kernel.md 4); kernel.md 6 (only replaced libraries). Formats with the host CRT
// (x86 cdecl va_list layout is identical) and logs on KRNL/info; invalid formats are ignored.
ULONG __cdecl DbgPrint(const char* Format, ...) noexcept {
    if (Format == nullptr) {
        return STATUS_SUCCESS;
    }
    char buffer[512];
    va_list args;
    va_start(args, Format);
    const _invalid_parameter_handler previous =
        _set_thread_local_invalid_parameter_handler(&IgnoreInvalidParameter);
    const int n = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, Format, args);
    _set_thread_local_invalid_parameter_handler(previous);
    va_end(args);
    if (n < 0 && buffer[0] == '\0') {
        std::snprintf(buffer, sizeof(buffer), "(unformattable: %.200s)", Format);
    }
    std::size_t len = std::strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
        buffer[--len] = '\0';
    }
    const std::string caller = SymbolizeNoThrow(guest::Addr(_ReturnAddress()));
    FUT_LOGI(Krnl, "DbgPrint: %s  [ret=%s]", buffer, caller.c_str());
    return STATUS_SUCCESS;
}
KRNL_FUNC(DbgPrint, &DbgPrint);

// DbgBreakPoint - slot 109, Ord 5, stdcall, 0 args (purge 0; push count only, kernel.md 4): VOID.
// Evidence: kernel_imports.tsv; only a tail jump from XAPILIB::DebugBreak. Breaks into an attached
// debugger, otherwise logs and continues (planned M2; trivial).
void __stdcall DbgBreakPoint() noexcept {
    const std::string caller = SymbolizeNoThrow(guest::Addr(_ReturnAddress()));
    if (IsDebuggerPresent()) {
        FUT_LOGW(Krnl, "DbgBreakPoint() from %s - breaking into the debugger", caller.c_str());
        __debugbreak();
        return;
    }
    FUT_LOGW(Krnl, "DbgBreakPoint() from %s - no debugger attached, continuing", caller.c_str());
}
KRNL_FUNC(DbgBreakPoint, &DbgBreakPoint);

}  // namespace krnl
