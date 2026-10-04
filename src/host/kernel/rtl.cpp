// Rtl string/memory helpers and the SEH forwards (slots 3, 13, 16, 84, 105). Owned by the kernel agent.
#include "core/guest.h"
#include "core/log.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"

#include <cstring>

namespace krnl {
namespace {

// ASSUMPTION: the Xbox kernel upcases ANSI characters without NLS tables: ASCII a-z and the
// Latin-1 range 0xE0..0xFE except 0xF7 (division sign). The only guest use compares XBE section
// names ("$$XTIMAGE", ...), which are pure ASCII (FUN_000f9b8e).
inline unsigned char UpperChar(unsigned char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) {
        return static_cast<unsigned char>(c - 0x20);
    }
    return c;
}

}  // namespace

// RtlInitAnsiString - slot 3, Ord 289, stdcall, 2 args (purge 8):
// VOID (PANSI_STRING DestinationString, PCSZ SourceString).
// Evidence: kernel_imports.tsv; 22 sites, e.g. CreateFileA 0xF76E0 (object name), FUN_000f9b8e
// (section lookup), XapiMapLetterToDirectory 0xF8C41; kernel.md 7 (ntdll-identical semantics:
// Length = strlen (max 0xFFFE), MaximumLength = Length + 1, Buffer = SourceString; NULL -> 0/0/NULL).
void __stdcall RtlInitAnsiString(xbox::ANSI_STRING* DestinationString, const char* SourceString) noexcept {
    if (DestinationString == nullptr) {
        FUT_LOGE(Krnl, "RtlInitAnsiString(NULL, %p)", static_cast<const void*>(SourceString));
        return;
    }
    DestinationString->Buffer = const_cast<char*>(SourceString);
    if (SourceString != nullptr) {
        std::size_t length = std::strlen(SourceString);
        if (length > 0xFFFE) {
            length = 0xFFFE;
        }
        DestinationString->Length = static_cast<std::uint16_t>(length);
        DestinationString->MaximumLength = static_cast<std::uint16_t>(length + 1);
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
    }
    FUT_LOGT(Krnl, "RtlInitAnsiString(%p, \"%s\")", static_cast<void*>(DestinationString),
             SourceString ? SourceString : "(null)");
}
KRNL_FUNC(RtlInitAnsiString, &RtlInitAnsiString);

// RtlCompareMemoryUlong - slot 13, Ord 269, stdcall, 3 args (purge 12):
// SIZE_T (PVOID Source, SIZE_T Length, ULONG Pattern). Returns the number of bytes (multiple of 4)
// that match Pattern before the first mismatch; Length is rounded down to whole ULONGs (NT).
// Evidence: kernel_imports.tsv; XAPI heap free-fill checks with Pattern 0xFEEEFEEE in FUN_000f4171,
// FUN_000f452c and FUN_000f5c77 (_text_000f0000.c; the result is compared with the length).
SIZE_T __stdcall RtlCompareMemoryUlong(const void* Source, SIZE_T Length, ULONG Pattern) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(Source);
    SIZE_T matched = 0;
    for (SIZE_T count = Length / sizeof(ULONG); count > 0; --count) {
        ULONG v;
        std::memcpy(&v, p + matched, sizeof(v));
        if (v != Pattern) {
            break;
        }
        matched += sizeof(ULONG);
    }
    return matched;
}
KRNL_FUNC(RtlCompareMemoryUlong, &RtlCompareMemoryUlong);

// RtlEqualString - slot 84, Ord 279, stdcall, 3 args (purge 12):
// BOOLEAN (PSTRING String1, PSTRING String2, BOOLEAN CaseInSensitive).
// Evidence: kernel_imports.tsv; single caller FUN_000f9b8e (section lookup by name for
// XapiMapLetterToDirectory, CaseInSensitive = 1, result read as char); kernel.md 7.
BOOLEAN __stdcall RtlEqualString(const xbox::ANSI_STRING* String1, const xbox::ANSI_STRING* String2,
                                 BOOLEAN CaseInSensitive) noexcept {
    if (String1 == nullptr || String2 == nullptr) {
        FUT_LOGE(Krnl, "RtlEqualString(%p, %p): NULL string", static_cast<const void*>(String1),
                 static_cast<const void*>(String2));
        return FALSE;
    }
    if (String1->Length != String2->Length) {
        return FALSE;
    }
    const auto* a = reinterpret_cast<const unsigned char*>(String1->Buffer);
    const auto* b = reinterpret_cast<const unsigned char*>(String2->Buffer);
    for (std::uint32_t i = 0; i < String1->Length; ++i) {
        unsigned char ca = a[i];
        unsigned char cb = b[i];
        if (CaseInSensitive) {
            ca = UpperChar(ca);
            cb = UpperChar(cb);
        }
        if (ca != cb) {
            return FALSE;
        }
    }
    return TRUE;
}
KRNL_FUNC(RtlEqualString, &RtlEqualString);

// RtlUnwind - slot 105, Ord 312, stdcall, 4 args (purge 16):
// VOID (PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue).
// Evidence: kernel_imports.tsv; callers LIBC __global_unwind2 0x1668CC.. (TargetIp 0x16694E) and
// _UnwindNestedFrames (0x166C6C); kernel.md 7.5; design 1.11: forwarded DIRECTLY to ntdll.
// The slot receives ntdll's own RtlUnwind (no host wrapper): RtlUnwind captures the context of its
// caller and resumes there via NtContinue, and it removes SEH registrations from fs:[0] - a host
// wrapper with its own EH frame would be unwound by it. fs:[0] is the Windows SEH chain (E3).
KRNL_FUNC(RtlUnwind, &::RtlUnwind);

// RtlRaiseException - slot 16, Ord 302, stdcall, 1 arg (purge 4): VOID (PEXCEPTION_RECORD).
// Evidence: kernel_imports.tsv; callers XAPI heap (0xF52CC, 0xF5C77; HEAP_GENERATE_EXCEPTIONS) and
// XAPILIB::RaiseException 0xF98ED; kernel.md 7.5; design 1.11: forwarded DIRECTLY to ntdll for the
// same reason as RtlUnwind (context capture of the guest caller, guest SEH handlers on fs:[0]).
// First-chance exceptions with EIP in the XBE are logged by the core VEH ring buffer.
KRNL_FUNC(RtlRaiseException, &::RtlRaiseException);

}  // namespace krnl
