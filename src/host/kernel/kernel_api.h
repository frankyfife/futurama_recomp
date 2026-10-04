// Registration of xboxkrnl HLE implementations (design 1.5, E4).
//
// The 111 thunk slots are described by the generated kernel/gen/kernel_table.gen.h (from
// analysis/loader/kernel_imports.tsv). Every implementation registers itself with ONE macro at
// namespace scope in the .cpp that defines it:
//
//   namespace krnl {
//   // NtClose - slot 26, Ord 187, stdcall, 1 arg (purge 4). Evidence: kernel_imports.tsv; design 4.2.
//   NTSTATUS __stdcall NtClose(HANDLE Handle) noexcept { ... }
//   KRNL_FUNC(NtClose, &NtClose);
//
//   // XboxKrnlVersion - slot 75, Ord 324, DATA. Evidence: design 1.5 table, kernel.md 8.2.
//   XboxKrnlVersionData g_XboxKrnlVersion = {1, 0, 5838, 1};
//   KRNL_DATA(XboxKrnlVersion, &g_XboxKrnlVersion);
//   }
//
// KRNL_FUNC static_asserts at compile time that the implementation's calling convention, stack
// purge and (fastcall) register-argument count match the slot (design 1.5: a wrong purge destroys
// the guest stack). Stack bytes are computed from the parameter types (each rounded up to 4 bytes,
// LARGE_INTEGER by value = 8). Variants:
//   KRNL_FUNC(Name, &impl)          real implementation                         (status impl)
//   KRNL_FUNC_STUB_OK(Name, &impl)  plausible-value stub for replaced libraries   (status stub_ok)
//   KRNL_DATA(Name, &object)        data export: slot receives &object           (kind DATA)
//   KRNL_ADDR(Name, &function)      function whose address is only passed around (kind ADDR)
// Slots without registration get the generated logging stub (FUNC) or a placeholder (DATA/ADDR).
//
// Calling conventions (kernel.md 4): 93 stdcall -> __stdcall; 4 fastcall (KfLowerIrql, KfRaiseIrql,
// IofCompleteRequest, ObfDereferenceObject) -> __fastcall (ecx[, edx]); DbgPrint -> __cdecl variadic.
// Every implementation must be noexcept and preserve ebx/esi/edi/ebp (MSVC ABI does).
#pragma once

#include "kernel/gen/kernel_table.gen.h"
#include "shared/xbox_types.h"

#include <cstdint>
#include <type_traits>

namespace krnl {

enum class ImplStatus : std::uint8_t { Unimpl, StubOk, Impl };

struct Binding {
    std::uintptr_t address;  // value to write into the guest thunk slot
    ImplStatus status;       // Unimpl => generated stub / placeholder
    const char* implName;    // registered expression (e.g. "&NtClose") or "<generated stub>"
    const char* file;        // registering source file (nullptr for generated stubs)
};

// --- resolution API (used by the core thunk stage) ---------------------------------------------
// Export description by slot (0..110) / by ordinal (nullptr if unknown).
const gen::ExportInfo& ExportAt(std::uint32_t slot) noexcept;
const gen::ExportInfo* FindExportByOrdinal(std::uint32_t ordinal) noexcept;
// Registered implementation, or the generated stub / placeholder for the slot.
Binding GetBinding(std::uint32_t slot) noexcept;
bool IsImplemented(std::uint32_t slot) noexcept;  // registered (impl or stub_ok)
// Logs duplicate registrations (error) and returns false if any exist.
bool ValidateRegistry() noexcept;
// Logs one line per slot that is still a generated stub (warn for M1 slots, info otherwise) plus a
// summary; returns the number of unimplemented slots.
std::uint32_t LogBindingSummary() noexcept;

// --- internals used by the macros and the generated stubs ---------------------------------------
namespace detail {

inline constexpr std::uint32_t kDataPlaceholderSize = 0x400;

void Register(std::uint32_t slot, std::uintptr_t address, ImplStatus status, const char* implName,
              const char* file) noexcept;

struct Registrar {
    Registrar(std::uint32_t slot, std::uintptr_t address, ImplStatus status, const char* implName,
              const char* file) noexcept {
        Register(slot, address, status, implName, file);
    }
};

// Called by every generated stub: logs "UNIMPLEMENTED Ord <n> <Name>(args) ret=<caller>" once per
// return address (warn), honours [kernel] strict, returns STATUS_NOT_IMPLEMENTED.
std::uint32_t ReportUnimplemented(std::uint32_t slot, const std::uint32_t* args, std::uint32_t argCount,
                                  void* returnAddress) noexcept;
// Called if the guest calls an ADDR placeholder: logs and aborts (convention unknown).
[[noreturn]] void ReportAddrPlaceholderCall(std::uint32_t slot, void* returnAddress) noexcept;

// ---- compile-time calling-convention traits over function pointer types ----
struct ConvInfo {
    gen::Conv conv;
    std::uint32_t purgeBytes;  // callee stack cleanup
    std::uint32_t regArgs;     // fastcall: arguments passed in ecx/edx
};

template <class T>
constexpr std::uint32_t StackBytes() {
    return (static_cast<std::uint32_t>(sizeof(T)) + 3u) & ~3u;
}
template <class T>
constexpr bool FastcallRegisterEligible() {
    return (std::is_integral_v<T> || std::is_pointer_v<T> || std::is_enum_v<T> || std::is_null_pointer_v<T>) &&
           sizeof(T) <= 4;
}
template <class... A>
constexpr std::uint32_t StdcallPurge() {
    return (0u + ... + StackBytes<A>());
}
// MSVC __fastcall: the first two DWORD-or-smaller integral/pointer arguments (left to right) go to
// ecx/edx, all others are pushed; the callee cleans the stack part.
template <class... A>
constexpr ConvInfo FastcallInfo() {
    constexpr bool eligible[] = {FastcallRegisterEligible<A>()..., false};
    constexpr std::uint32_t bytes[] = {StackBytes<A>()..., 0u};
    std::uint32_t regs = 0, stack = 0;
    for (std::size_t i = 0; i < sizeof...(A); ++i) {
        if (regs < 2 && eligible[i]) {
            ++regs;
        } else {
            stack += bytes[i];
        }
    }
    return ConvInfo{gen::Conv::Fastcall, stack, regs};
}

template <class F>
struct FnTraits {
    static constexpr bool valid = false;
    static constexpr ConvInfo info{gen::Conv::None, 0, 0};
};
#define KRNL_DETAIL_FN_TRAITS(NOEXCEPT)                                                                  \
    template <class R, class... A>                                                                       \
    struct FnTraits<R(__stdcall*)(A...) NOEXCEPT> {                                                      \
        static constexpr bool valid = true;                                                              \
        static constexpr ConvInfo info{gen::Conv::Stdcall, StdcallPurge<A...>(), 0};                     \
    };                                                                                                   \
    template <class R, class... A>                                                                       \
    struct FnTraits<R(__fastcall*)(A...) NOEXCEPT> {                                                     \
        static constexpr bool valid = true;                                                              \
        static constexpr ConvInfo info = FastcallInfo<A...>();                                           \
    };                                                                                                   \
    template <class R, class... A>                                                                       \
    struct FnTraits<R(__cdecl*)(A...) NOEXCEPT> {                                                        \
        static constexpr bool valid = true;                                                              \
        static constexpr ConvInfo info{gen::Conv::Cdecl, 0, 0};                                          \
    };                                                                                                   \
    template <class R, class... A>                                                                       \
    struct FnTraits<R(__cdecl*)(A..., ...) NOEXCEPT> {                                                   \
        static constexpr bool valid = true;                                                              \
        static constexpr ConvInfo info{gen::Conv::Cdecl, 0, 0};                                          \
    };
KRNL_DETAIL_FN_TRAITS(noexcept(false))
KRNL_DETAIL_FN_TRAITS(noexcept(true))
#undef KRNL_DETAIL_FN_TRAITS

template <class F>
using FnTraitsOf = FnTraits<std::decay_t<F>>;

}  // namespace detail
}  // namespace krnl

#define KRNL_DETAIL_CAT2(a, b) a##b
#define KRNL_DETAIL_CAT(a, b) KRNL_DETAIL_CAT2(a, b)

#define KRNL_DETAIL_FUNC(Name, Fn, Status)                                                             \
    static_assert(::krnl::exp::Name::kind == ::krnl::gen::Kind::Func,                                    \
                  "KRNL_FUNC(" #Name "): slot is not a FUNC export (use KRNL_DATA/KRNL_ADDR)");          \
    static_assert(::krnl::detail::FnTraitsOf<decltype(Fn)>::valid,                                     \
                  "KRNL_FUNC(" #Name "): pass a function pointer with explicit __stdcall/__fastcall/__cdecl"); \
    static_assert(::krnl::detail::FnTraitsOf<decltype(Fn)>::info.conv == ::krnl::exp::Name::conv,      \
                  "KRNL_FUNC(" #Name "): calling convention differs from kernel_imports.tsv");           \
    static_assert(::krnl::detail::FnTraitsOf<decltype(Fn)>::info.purgeBytes ==                          \
                      ::krnl::exp::Name::purgeBytes,                                                     \
                  "KRNL_FUNC(" #Name "): stack purge (argument bytes) differs from kernel_imports.tsv"); \
    static_assert(::krnl::detail::FnTraitsOf<decltype(Fn)>::info.regArgs == ::krnl::exp::Name::regArgs, \
                  "KRNL_FUNC(" #Name "): fastcall register arguments differ from kernel_imports.tsv");   \
    namespace {                                                                                          \
    const ::krnl::detail::Registrar KRNL_DETAIL_CAT(krnlRegistrar_, Name)(                              \
        ::krnl::exp::Name::slot, reinterpret_cast<std::uintptr_t>(Fn), Status, #Fn, __FILE__);       \
    }                                                                                                    \
    static_assert(true, "")

#define KRNL_FUNC(Name, Fn) KRNL_DETAIL_FUNC(Name, Fn, ::krnl::ImplStatus::Impl)
#define KRNL_FUNC_STUB_OK(Name, Fn) KRNL_DETAIL_FUNC(Name, Fn, ::krnl::ImplStatus::StubOk)

#define KRNL_DATA(Name, Object)                                                                          \
    static_assert(::krnl::exp::Name::kind == ::krnl::gen::Kind::Data,                                    \
                  "KRNL_DATA(" #Name "): slot is not a DATA export");                                    \
    static_assert(std::is_object_v<std::remove_pointer_t<decltype(Object)>>,                             \
                  "KRNL_DATA(" #Name "): pass the address of a host data object");                       \
    namespace {                                                                                          \
    const ::krnl::detail::Registrar KRNL_DETAIL_CAT(krnlRegistrar_, Name)(                              \
        ::krnl::exp::Name::slot, reinterpret_cast<std::uintptr_t>(Object), ::krnl::ImplStatus::Impl,     \
        #Object, __FILE__);                                                                              \
    }                                                                                                    \
    static_assert(true, "")

#define KRNL_ADDR(Name, Function)                                                                        \
    static_assert(::krnl::exp::Name::kind == ::krnl::gen::Kind::Addr,                                    \
                  "KRNL_ADDR(" #Name "): slot is not an ADDR export");                                   \
    static_assert(::krnl::detail::FnTraitsOf<decltype(Function)>::valid,                                 \
                  "KRNL_ADDR(" #Name "): pass a function pointer with an explicit calling convention");  \
    namespace {                                                                                          \
    const ::krnl::detail::Registrar KRNL_DETAIL_CAT(krnlRegistrar_, Name)(                              \
        ::krnl::exp::Name::slot, reinterpret_cast<std::uintptr_t>(Function), ::krnl::ImplStatus::Impl,   \
        #Function, __FILE__);                                                                            \
    }                                                                                                    \
    static_assert(true, "")
