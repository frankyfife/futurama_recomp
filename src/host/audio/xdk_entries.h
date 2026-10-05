// XDK library entry points (DSOUND, XMV, XPP) and the registration of their host replacements
// (design E7, 2.1, Anhang B "M2"; analysis/loader/avi.md 2.2, 3.1, 4.1). Shared by audio/, video/
// and input/ (namespace xdk); the per-library stubs live in audio/dsound_stubs.cpp,
// video/xmv_stubs.cpp and input/xpp_stubs.cpp.
//
// The entry table is generated from analysis/loader/avi_api_surface.tsv by
// tools/gen/gen_xdk_entries.py (gen/xdk_entries.gen.inc): address, library, hook name, stack purge
// (`ret N` of the original, re-verified with capstone against the XBE), flags, call-site counts.
//
// Registration (namespace scope, next to the definition):
//
//   // DSOUND::DirectSoundCreate 0x24935C - stdcall, 3 args (ret 12). Evidence: ...
//   HRESULT __stdcall DirectSoundCreateHle(const void* guid, void** ppDS, void* outer) noexcept { ... }
//   XDK_HOOK(DirectSoundCreate, &DirectSoundCreateHle);
//
// XDK_HOOK static_asserts that the replacement is `__stdcall noexcept`, that its argument block equals
// the generated purge (a wrong purge destroys the guest stack, design 3.3) and that the return value fits
// into eax, then registers a normal FUT_HOOK (5-byte jmp; hook name "<LIB>::<id>", so
// [hooks] disable = DSOUND::* switches a whole library back to the original code).
//
// Call tracing (design 5.4 style, "each stub logs once per call site"): every stub ends with
//   XDK_TRACE_CALL(DirectSoundCreate, "(guid=0x%08X, ppDS=0x%08X) -> 0x%08lX", ...);
// which counts the call and logs one line on the library's channel (SND/VID/INP) at debug level the
// first time a call site (guest return address) is seen. xdk::LogCallSummary() and the crash
// reporter "xdk" print the per-entry call counts.
#pragma once

#include "core/guest.h"
#include "core/hooks.h"
#include "core/log.h"

#include <intrin.h>
#include <sal.h>

#include <cstdint>
#include <span>
#include <type_traits>

namespace xdk {

enum class Library : std::uint8_t { Dsound, Xmv, Xpp };

inline constexpr std::uint32_t kFlagJmpThunk = 1;     // entry is a 5-byte jmp to the implementation
inline constexpr std::uint32_t kFlagNameAssumed = 2;  // XMV: name derived from behaviour (avi.md 3.1)

// One generated entry (row of gen/xdk_entries.gen.inc).
enum class Entry : std::uint32_t {
#define XDK_ENTRY(id, address, lib, hookName, purgeBytes, flags, sitesGame, sitesXmv, ghidraName, callers) id,
#include "gen/xdk_entries.gen.inc"
    Count
};
inline constexpr std::uint32_t kEntryCount = static_cast<std::uint32_t>(Entry::Count);

struct EntryInfo {
    Entry entry;
    guest::GuestAddr address;
    Library library;
    const char* hookName;      // "DSOUND::DirectSoundCreate" ([hooks] disable name)
    std::uint32_t purgeBytes;  // ret N
    std::uint32_t flags;       // kFlag*
    std::uint32_t sitesGame;   // call sites in game code (avi_rawcalls.out)
    std::uint32_t sitesXmv;    // call sites inside the XMV library (only DSOUND entries)
    const char* ghidraName;
    const char* callers;  // calling functions (TSV column, "nofunc@" = code outside Ghidra functions)
};

// Compile-time view of each entry: xdk::gen::DirectSoundCreate::address, ::purgeBytes, ...
namespace gen {
#define XDK_ENTRY(id, address_, lib_, hookName_, purgeBytes_, flags_, sitesGame_, sitesXmv_, ghidraName_,    \
                  callers_)                                                                                  \
    struct id {                                                                                              \
        static constexpr Entry entry = Entry::id;                                                            \
        static constexpr guest::GuestAddr address = address_;                                                \
        static constexpr Library library = Library::lib_;                                                    \
        static constexpr const char* hookName = hookName_;                                                   \
        static constexpr std::uint32_t purgeBytes = purgeBytes_;                                             \
        static constexpr std::uint32_t flags = flags_;                                                       \
    };
#include "gen/xdk_entries.gen.inc"
}  // namespace gen

// XMediaObject vtable of a DSOUND stream (guest vtable guest::kDsoundStreamVtable): slot number and
// purge of each method, e.g. xdk::stream::Process::slot == 4, ::purgeBytes == 12.
namespace stream {
#define XDK_STREAM_METHOD(id, slot_, purgeBytes_, inTsv_)                                                    \
    struct id {                                                                                              \
        static constexpr std::uint32_t slot = slot_;                                                         \
        static constexpr std::uint32_t purgeBytes = purgeBytes_;                                             \
        static constexpr bool calledByGame = (inTsv_) != 0;                                                  \
    };
#include "gen/xdk_entries.gen.inc"
inline constexpr std::uint32_t kMethodCount = 7;
}  // namespace stream

std::span<const EntryInfo> Entries() noexcept;  // kEntryCount rows, sorted by address
const EntryInfo& Info(Entry entry) noexcept;
const char* LibraryName(Library library) noexcept;       // "DSOUND", "XMV", "XPP"
core::Channel LibraryChannel(Library library) noexcept;  // Snd, Vid, Inp

// Host replacement registered with XDK_HOOK for an entry (nullptr if none).
const void* RegisteredStub(Entry entry) noexcept;
// Number of calls of an entry since process start (all threads).
std::uint32_t CallCount(Entry entry) noexcept;
// Distinct call sites seen for an entry.
std::uint32_t CallSiteCount(Entry entry) noexcept;
// Logs one summary line per library (calls, entries used, sites) at `level` and one line per used
// entry at debug. Used by the crash reporter "xdk"; callable at an orderly end (e.g. --frames).
void LogCallSummary(core::Level level) noexcept;

// ---- guest-visible dummy objects ----------------------------------------------------------------
// Small zeroed blocks below 0x80000000 (design 1.2) for opaque XDK handles (IDirectSound, XMV decoder,
// stub streams). At most kGuestObjectSize bytes each; nullptr when the pool (1024 objects) is full.
inline constexpr std::uint32_t kGuestObjectSize = 64;
void* AllocGuestObject(std::uint32_t size) noexcept;
void FreeGuestObject(void* object) noexcept;
bool IsGuestObject(const void* object) noexcept;  // allocated and not yet freed
// Vtable whose kTrapVtableSlots entries all end the process with "TRAP" + caller (core::Fatal): +0 of
// every dummy handle the game must only pass to the C API (it never calls them virtually, avi.md 2.3).
inline constexpr std::uint32_t kTrapVtableSlots = 16;
const void* const* TrapVtable() noexcept;

// ---- runtime purge measurement -------------------------------------------------------------------
// Calls `entry` (a guest entry address with an installed hook, or a host stub) once on a suspended host
// thread whose context is set to eip = entry, esp -> [landing][stackDwords zero DWORDs][16 spare] and
// returns the number of argument bytes the callee popped (its `ret N`), measured when its return lands
// in a host function. All XDK stubs accept all-zero (NULL) arguments; calls on the probe thread are
// neither counted nor logged and skip one-time side effects (detail::InPurgeProbe). Serialized, at most
// 5 s per call. Same method as gfx/purge_probe (merge candidate after phase 1).
struct PurgeMeasurement {
    bool ok = false;
    std::uint32_t purgeBytes = 0;
    const char* error = "";
};
PurgeMeasurement MeasureEntryPurge(const void* entry, std::uint32_t stackDwords) noexcept;

namespace detail {

// True only on the thread of a running MeasureEntryPurge.
bool InPurgeProbe() noexcept;

template <class R>
constexpr bool ReturnFitsEax() {
    if constexpr (std::is_void_v<R>) {
        return true;
    } else {
        return sizeof(R) <= 4 && !std::is_floating_point_v<R>;
    }
}

template <class F>
struct StdcallFn {
    static constexpr bool valid = false;
    static constexpr std::uint32_t purgeBytes = 0;
    static constexpr bool returnFitsEax = false;
};
template <class R, class... A>
struct StdcallFn<R(__stdcall*)(A...) noexcept> {
    static constexpr bool valid = true;
    static constexpr std::uint32_t purgeBytes =
        (0u + ... + ((static_cast<std::uint32_t>(sizeof(A)) + 3u) & ~3u));
    static constexpr bool returnFitsEax = ReturnFitsEax<R>();
};
template <class F>
using StdcallFnOf = StdcallFn<std::decay_t<F>>;

void RegisterStub(Entry entry, const void* replacement) noexcept;
struct StubRegistrar {
    StubRegistrar(Entry entry, const void* replacement) noexcept { RegisterStub(entry, replacement); }
};

// Counts the call; true if `returnAddress` is a new call site of `entry` and debug logging is enabled
// on the entry's channel (then the caller formats the line with LogCall).
bool NoteCall(Entry entry, const void* returnAddress) noexcept;
void LogCall(Entry entry, const void* returnAddress, _Printf_format_string_ const char* fmt, ...) noexcept;

}  // namespace detail
}  // namespace xdk

#define XDK_DETAIL_CAT2(a, b) a##b
#define XDK_DETAIL_CAT(a, b) XDK_DETAIL_CAT2(a, b)

// Registers `Fn` (address of a `__stdcall noexcept` host function) as the replacement of entry `Id`.
#define XDK_HOOK(Id, Fn)                                                                                     \
    static_assert(::xdk::detail::StdcallFnOf<decltype(Fn)>::valid,                                           \
                  "XDK_HOOK(" #Id "): pass the address of a __stdcall noexcept function");                   \
    static_assert(::xdk::detail::StdcallFnOf<decltype(Fn)>::purgeBytes == ::xdk::gen::Id::purgeBytes,        \
                  "XDK_HOOK(" #Id "): argument bytes differ from `ret N` in avi_api_surface.tsv");           \
    static_assert(::xdk::detail::StdcallFnOf<decltype(Fn)>::returnFitsEax,                                   \
                  "XDK_HOOK(" #Id "): the return value must fit into eax");                                  \
    namespace {                                                                                              \
    const ::xdk::detail::StubRegistrar                                                                       \
        XDK_DETAIL_CAT(xdkStubRegistrar_, Id)(::xdk::Entry::Id, ::core::detail::CodeAddress(Fn));            \
    }                                                                                                        \
    FUT_HOOK(::xdk::gen::Id::address, Fn, Stdcall, ::xdk::gen::Id::hookName)

// Counts the call and logs it once per call site (debug level, channel of the entry's library).
// Use inside the stub itself (_ReturnAddress() must be the guest call site).
#define XDK_TRACE_CALL(Id, ...)                                                                              \
    do {                                                                                                     \
        const void* const xdkReturnAddress_ = _ReturnAddress();                                              \
        if (::xdk::detail::NoteCall(::xdk::Entry::Id, xdkReturnAddress_)) {                                  \
            ::xdk::detail::LogCall(::xdk::Entry::Id, xdkReturnAddress_, __VA_ARGS__);                        \
        }                                                                                                    \
    } while (0)
