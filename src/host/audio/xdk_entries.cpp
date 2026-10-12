// XDK entry table, stub registry, call tracing, guest dummy objects and self-tests (see xdk_entries.h).
#include "audio/xdk_entries.h"

#include "audio/dsound_hle.h"

#include "core/config.h"
#include "core/crash.h"
#include "core/guest.h"
#include "core/hooks.h"
#include "core/host_main.h"
#include "core/log.h"
#include "core/selftest.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace xdk {
namespace {

constexpr EntryInfo kEntries[] = {
#define XDK_ENTRY(id, address, lib, hookName, purgeBytes, flags, sitesGame, sitesXmv, ghidraName, callers)   \
    {Entry::id, address, Library::lib, hookName, purgeBytes, flags, sitesGame, sitesXmv, ghidraName, callers},
#include "gen/xdk_entries.gen.inc"
};
static_assert(sizeof(kEntries) / sizeof(kEntries[0]) == kEntryCount);

constexpr bool TableConsistent() {
    for (std::uint32_t i = 0; i < kEntryCount; ++i) {
        if (static_cast<std::uint32_t>(kEntries[i].entry) != i || kEntries[i].purgeBytes % 4 != 0 ||
            (i > 0 && kEntries[i - 1].address >= kEntries[i].address)) {
            return false;
        }
    }
    return true;
}
static_assert(TableConsistent(), "generated XDK table: index order, sorted addresses, purge % 4");

// ---- stub registry and call statistics ----------------------------------------------------------
const void* g_stubs[kEntryCount] = {};
std::uint32_t g_stubRegistrations[kEntryCount] = {};
std::atomic<std::uint32_t> g_calls[kEntryCount] = {};
std::atomic<std::uint32_t> g_sites[kEntryCount] = {};

std::mutex& SiteMutex() {
    static std::mutex m;
    return m;
}
std::unordered_set<std::uint64_t>& SiteSet() {
    static std::unordered_set<std::uint64_t> sites;
    return sites;
}

void CrashReport() noexcept {
    LogCallSummary(core::Level::Error);
}
const bool g_crashReporterRegistered = (core::AddCrashReporter("xdk", &CrashReport), true);
// Exit reporter (core/host_main.h): call counts per library at every orderly process end.
void ExitReport() noexcept {
    LogCallSummary(core::Level::Info);
}
const bool g_exitReporterRegistered = (core::AddExitReporter("xdk", &ExitReport), true);

// ---- guest object pool ----------------------------------------------------------------------------
constexpr std::uint32_t kPoolBytes = 0x10000;  // one allocation granule
constexpr std::uint32_t kPoolSlots = kPoolBytes / kGuestObjectSize;

struct Pool {
    std::mutex mutex;
    std::uint8_t* base = nullptr;
    bool failed = false;
    std::uint32_t used[kPoolSlots / 32] = {};
};
Pool& GuestPool() {
    static Pool pool;
    return pool;
}

bool EnsurePool(Pool& pool) noexcept {
    if (pool.base != nullptr || pool.failed) {
        return pool.base != nullptr;
    }
    void* p = VirtualAlloc(nullptr, kPoolBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p == nullptr || static_cast<std::uint64_t>(guest::Addr(p)) + kPoolBytes > guest::kGuestMemoryLimit) {
        FUT_LOGE(Snd, "XDK guest object pool: no memory below 0x80000000 (%p)", p);
        if (p != nullptr) {
            VirtualFree(p, 0, MEM_RELEASE);
        }
        pool.failed = true;
        return false;
    }
    pool.base = static_cast<std::uint8_t*>(p);
    return true;
}

bool SlotOf(const Pool& pool, const void* object, std::uint32_t& slot) noexcept {
    if (pool.base == nullptr || object == nullptr) {
        return false;
    }
    const auto p = reinterpret_cast<std::uintptr_t>(object);
    const auto b = reinterpret_cast<std::uintptr_t>(pool.base);
    if (p < b || p >= b + kPoolBytes || (p - b) % kGuestObjectSize != 0) {
        return false;
    }
    slot = static_cast<std::uint32_t>((p - b) / kGuestObjectSize);
    return true;
}

// ---- trap vtable --------------------------------------------------------------------------------
// The game passes the XDK dummy handles only to hooked C functions (avi.md 2.3: "Buffer-Zeiger sind fuer
// den Spielcode opak"; IDirectSound is used only through the C API, checked at every use of
// SoundManager+0x48 in 0xC9000..0xCD000). A virtual call would mean an unknown code path: abort loudly.
// The convention of such a call is unknown, so the trap never returns.
template <int N>
void __stdcall TrapSlot() noexcept {
    const guest::GuestAddr from = guest::Addr(_ReturnAddress());
    char where[96] = "?";
    try {
        std::snprintf(where, sizeof(where), "%s", guest::Symbolize(from).c_str());
    } catch (...) {
    }
    core::Fatal("TRAP XDK dummy object: virtual call of vtable slot %d (+0x%02X) from %s", N, N * 4, where);
}

#define XDK_TRAP4(n)                                                                                         \
    reinterpret_cast<std::uintptr_t>(&TrapSlot<(n)>), reinterpret_cast<std::uintptr_t>(&TrapSlot<(n) + 1>),  \
        reinterpret_cast<std::uintptr_t>(&TrapSlot<(n) + 2>),                                                \
        reinterpret_cast<std::uintptr_t>(&TrapSlot<(n) + 3>)
const std::uintptr_t g_trapVtable[kTrapVtableSlots] = {XDK_TRAP4(0), XDK_TRAP4(4), XDK_TRAP4(8),
                                                       XDK_TRAP4(12)};
#undef XDK_TRAP4

// ---- purge probe ---------------------------------------------------------------------------------
std::atomic<DWORD> g_probeThreadId{0};
std::atomic<std::uint32_t> g_probeEntryEsp{0};
constexpr std::uint32_t kProbeStackCommit = 0x10000;
constexpr std::uint32_t kProbeFrameDepth = 0x2000;  // probe frame below the new thread's initial esp
constexpr std::uint32_t kProbeSpareDwords = 16;
constexpr DWORD kProbeTimeoutMs = 5000;

// Reached by the measured function's `ret N` (never called): its own return-address slot is the first
// DWORD the callee did not pop; the exit code is the number of popped argument bytes.
[[noreturn]] void __cdecl ProbeLanding() noexcept {
    const std::uint32_t esp = guest::Addr(_AddressOfReturnAddress());
    ExitThread(static_cast<DWORD>(esp - g_probeEntryEsp.load() - 4));
}

DWORD WINAPI ProbeThreadStart(void*) {
    return 0xFFFFFFFFu;  // not reached: the context is replaced before the thread first runs
}

}  // namespace

PurgeMeasurement MeasureEntryPurge(const void* entry, std::uint32_t stackDwords) noexcept {
    static std::mutex serial;
    PurgeMeasurement m;
    if (entry == nullptr || stackDwords > 32) {
        m.error = "bad arguments";
        return m;
    }
    std::lock_guard<std::mutex> lock(serial);
    DWORD tid = 0;
    HANDLE thread =
        CreateThread(nullptr, kProbeStackCommit, &ProbeThreadStart, nullptr, CREATE_SUSPENDED, &tid);
    if (thread == nullptr) {
        m.error = "CreateThread failed";
        return m;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) {
        m.error = "GetThreadContext failed";
        TerminateThread(thread, 0);
        CloseHandle(thread);
        return m;
    }
    // Probe frame inside the committed part of the new thread's own stack: [esp] = landing,
    // [esp+4..] = zero arguments + spare DWORDs (a callee popping too much still lands on zeros).
    const std::uint32_t esp = ((ctx.Esp - kProbeFrameDepth) & ~0xFu) - 4;
    auto* frame = guest::Ptr<std::uint32_t>(esp);
    std::memset(frame, 0, (1 + stackDwords + kProbeSpareDwords) * sizeof(std::uint32_t));
    frame[0] = guest::Addr(core::detail::CodeAddress(&ProbeLanding));
    ctx.Esp = esp;
    ctx.Eip = guest::Addr(entry);
    ctx.Ebp = 0;
    if (!SetThreadContext(thread, &ctx)) {
        m.error = "SetThreadContext failed";
        TerminateThread(thread, 0);
        CloseHandle(thread);
        return m;
    }
    g_probeEntryEsp.store(esp);
    g_probeThreadId.store(tid);
    ResumeThread(thread);
    DWORD code = 0;
    if (WaitForSingleObject(thread, kProbeTimeoutMs) != WAIT_OBJECT_0) {
        m.error = "probe thread did not finish within 5 s";
        TerminateThread(thread, 0);
        WaitForSingleObject(thread, kProbeTimeoutMs);
    } else if (!GetExitCodeThread(thread, &code)) {
        m.error = "GetExitCodeThread failed";
    } else {
        m.ok = true;
        m.purgeBytes = code;
    }
    g_probeThreadId.store(0);
    CloseHandle(thread);
    return m;
}

std::span<const EntryInfo> Entries() noexcept {
    return kEntries;
}

const EntryInfo& Info(Entry entry) noexcept {
    const auto i = static_cast<std::uint32_t>(entry);
    return kEntries[i < kEntryCount ? i : 0];
}

const char* LibraryName(Library library) noexcept {
    switch (library) {
    case Library::Dsound:
        return "DSOUND";
    case Library::Xmv:
        return "XMV";
    case Library::Xpp:
        return "XPP";
    }
    return "?";
}

core::Channel LibraryChannel(Library library) noexcept {
    switch (library) {
    case Library::Dsound:
        return core::Channel::Snd;
    case Library::Xmv:
        return core::Channel::Vid;
    case Library::Xpp:
        return core::Channel::Inp;
    }
    return core::Channel::Snd;
}

const void* RegisteredStub(Entry entry) noexcept {
    const auto i = static_cast<std::uint32_t>(entry);
    return i < kEntryCount ? g_stubs[i] : nullptr;
}

std::uint32_t CallCount(Entry entry) noexcept {
    const auto i = static_cast<std::uint32_t>(entry);
    return i < kEntryCount ? g_calls[i].load(std::memory_order_relaxed) : 0;
}

std::uint32_t CallSiteCount(Entry entry) noexcept {
    const auto i = static_cast<std::uint32_t>(entry);
    return i < kEntryCount ? g_sites[i].load(std::memory_order_relaxed) : 0;
}

void LogCallSummary(core::Level level) noexcept {
    for (const Library lib : {Library::Dsound, Library::Xmv, Library::Xpp}) {
        std::uint64_t calls = 0;
        std::uint32_t entries = 0, used = 0, sites = 0;
        for (const EntryInfo& e : kEntries) {
            if (e.library != lib) {
                continue;
            }
            ++entries;
            const std::uint32_t n = CallCount(e.entry);
            calls += n;
            sites += CallSiteCount(e.entry);
            used += n != 0 ? 1u : 0u;
        }
        const core::Channel ch = LibraryChannel(lib);
        if (core::LogEnabled(ch, level)) {
            // XPP is HLE since M4 (input/xpp_hle.cpp), DSOUND since M5 (audio/dsound_hle.cpp); XMV stays a
            // stub until M6.
            core::LogWrite(ch, level, "XDK %s %s: %llu calls to %u of %u entries from %u call sites",
                           LibraryName(lib), lib == Library::Xmv ? "stubs" : "HLE",
                           static_cast<unsigned long long>(calls), used, entries, sites);
        }
        for (const EntryInfo& e : kEntries) {
            if (e.library == lib && CallCount(e.entry) != 0 && core::LogEnabled(ch, core::Level::Debug)) {
                core::LogWrite(ch, core::Level::Debug, "XDK   %-44s %8u calls, %u sites", e.hookName,
                               CallCount(e.entry), CallSiteCount(e.entry));
            }
        }
    }
    snd::LogDsoundSummary(level);  // M5: DSOUND HLE objects and packets
}

void* AllocGuestObject(std::uint32_t size) noexcept {
    if (size == 0 || size > kGuestObjectSize) {
        return nullptr;
    }
    Pool& pool = GuestPool();
    std::lock_guard<std::mutex> lock(pool.mutex);
    if (!EnsurePool(pool)) {
        return nullptr;
    }
    for (std::uint32_t slot = 0; slot < kPoolSlots; ++slot) {
        std::uint32_t& word = pool.used[slot / 32];
        const std::uint32_t bit = 1u << (slot % 32);
        if ((word & bit) == 0) {
            word |= bit;
            std::uint8_t* p = pool.base + static_cast<std::size_t>(slot) * kGuestObjectSize;
            std::memset(p, 0, kGuestObjectSize);
            return p;
        }
    }
    FUT_LOGE(Snd, "XDK guest object pool exhausted (%u objects)", kPoolSlots);
    return nullptr;
}

void FreeGuestObject(void* object) noexcept {
    if (object == nullptr) {
        return;
    }
    const void* const* trapTable = TrapVtable();  // before locking: may allocate from the pool
    Pool& pool = GuestPool();
    std::lock_guard<std::mutex> lock(pool.mutex);
    std::uint32_t slot = 0;
    if (!SlotOf(pool, object, slot)) {
        return;
    }
    // Poison the freed block (the trap vtable at +0): a stale guest handle aborts instead of reading
    // reused state.
    std::memset(object, 0, kGuestObjectSize);
    const std::uintptr_t trap = reinterpret_cast<std::uintptr_t>(trapTable);
    std::memcpy(object, &trap, sizeof(trap));
    pool.used[slot / 32] &= ~(1u << (slot % 32));
}

bool IsGuestObject(const void* object) noexcept {
    Pool& pool = GuestPool();
    std::lock_guard<std::mutex> lock(pool.mutex);
    std::uint32_t slot = 0;
    return SlotOf(pool, object, slot) && (pool.used[slot / 32] & (1u << (slot % 32))) != 0;
}

const void* const* TrapVtable() noexcept {
    // Guest-visible copy in the pool (below 0x80000000); the DLL copy is only a fallback.
    static const void* const* const table = []() noexcept {
        static_assert(sizeof(g_trapVtable) == kGuestObjectSize);
        void* copy = AllocGuestObject(kGuestObjectSize);
        if (copy == nullptr) {
            return reinterpret_cast<const void* const*>(&g_trapVtable[0]);
        }
        std::memcpy(copy, g_trapVtable, sizeof(g_trapVtable));
        return static_cast<const void* const*>(copy);
    }();
    return table;
}

namespace detail {

bool InPurgeProbe() noexcept {
    const DWORD id = g_probeThreadId.load(std::memory_order_relaxed);
    return id != 0 && id == GetCurrentThreadId();
}

void RegisterStub(Entry entry, const void* replacement) noexcept {
    const auto i = static_cast<std::uint32_t>(entry);
    if (i < kEntryCount) {
        g_stubs[i] = replacement;
        ++g_stubRegistrations[i];
    }
}

bool NoteCall(Entry entry, const void* returnAddress) noexcept {
    const auto i = static_cast<std::uint32_t>(entry);
    if (i >= kEntryCount || InPurgeProbe()) {
        return false;
    }
    g_calls[i].fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t key = (static_cast<std::uint64_t>(i) << 32) | guest::Addr(returnAddress);
    bool fresh = false;
    try {
        std::lock_guard<std::mutex> lock(SiteMutex());
        fresh = SiteSet().insert(key).second;
    } catch (...) {
        return false;
    }
    if (!fresh) {
        return false;
    }
    g_sites[i].fetch_add(1, std::memory_order_relaxed);
    return core::LogEnabled(LibraryChannel(kEntries[i].library), core::Level::Debug);
}

void LogCall(Entry entry, const void* returnAddress, const char* fmt, ...) noexcept {
    const EntryInfo& e = Info(entry);
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    char where[96] = "?";
    try {
        std::snprintf(where, sizeof(where), "%s", guest::Symbolize(guest::Addr(returnAddress)).c_str());
    } catch (...) {
    }
    core::LogWrite(LibraryChannel(e.library), core::Level::Debug,
                   "XDK %s%s ret=%s (stub, first call from this site)", e.hookName, text, where);
}

}  // namespace detail

// ---- self-tests (design 1.4 step 8; --selftest) ---------------------------------------------------

// Every generated entry has exactly one XDK_HOOK replacement (design Anhang B: all 54 entries are
// stubbed in M2). Host phase: no XBE needed.
FUT_SELFTEST(XdkStubsRegistered, Host) {
    std::uint32_t missing = 0, duplicate = 0;
    std::string names;
    for (const EntryInfo& e : kEntries) {
        const auto i = static_cast<std::uint32_t>(e.entry);
        if (g_stubs[i] == nullptr) {
            ++missing;
            names += std::string(" missing:") + e.hookName;
        } else if (g_stubRegistrations[i] != 1) {
            ++duplicate;
            names += std::string(" duplicate:") + e.hookName;
        }
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%u entries, %u missing, %u registered twice", kEntryCount, missing,
                  duplicate);
    detail = buf + names;
    return missing == 0 && duplicate == 0;
}

// The dummy handles handed to the guest lie below 0x80000000 (design 1.2) and the pool round-trips.
FUT_SELFTEST(XdkGuestObjectPool, Host) {
    void* a = AllocGuestObject(kGuestObjectSize);
    void* b = AllocGuestObject(4);
    const bool ok = a != nullptr && b != nullptr && a != b && IsGuestObject(a) && IsGuestObject(b) &&
                    static_cast<std::uint64_t>(guest::Addr(a)) + kGuestObjectSize <= guest::kGuestMemoryLimit;
    FreeGuestObject(a);
    FreeGuestObject(b);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "objects %p/%p, freed: %s", a, b,
                  !IsGuestObject(a) && !IsGuestObject(b) ? "yes" : "NO");
    detail = buf;
    return ok && !IsGuestObject(a) && !IsGuestObject(b);
}

// After StageInstallHooks: each entry is patched with a jmp to its stub unless [hooks] disable names it.
FUT_SELFTEST(XdkHooksInstalled, Mapped) {
    const core::Config& cfg = core::GetConfig();
    std::uint32_t installed = 0, disabled = 0, failed = 0;
    std::uint32_t perLib[3] = {};
    std::string names;
    for (const EntryInfo& e : kEntries) {
        const core::HookDesc d{
            e.address, RegisteredStub(e.entry), core::HookConv::Stdcall, e.hookName, nullptr, true, __FILE__};
        if (core::IsHookInstalled(e.address)) {
            ++installed;
            ++perLib[static_cast<int>(e.library)];
        } else if (core::IsHookDisabled(d, cfg)) {
            ++disabled;
        } else {
            ++failed;
            names += std::string(" ") + e.hookName;
        }
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "%u/%u hooked (DSOUND %u, XMV %u, XPP %u), %u disabled by config, %u missing", installed,
                  kEntryCount, perLib[0], perLib[1], perLib[2], disabled, failed);
    detail = buf + names;
    return failed == 0;
}

// Runtime proof of design 3.3 for every installed XDK hook: calling the guest entry address (5-byte jmp
// -> stub) pops exactly the `ret N` of the original (generated purge, verified against the XBE by
// gen_xdk_entries.py). Runs each stub once with all-zero arguments on a probe thread.
FUT_SELFTEST(XdkEntryPurgeMeasured, Mapped) {
    std::uint32_t measured = 0, wrong = 0;
    std::string names;
    for (const EntryInfo& e : kEntries) {
        if (!core::IsHookInstalled(e.address)) {
            continue;  // disabled by config (XdkHooksInstalled reports missing ones)
        }
        const PurgeMeasurement m = MeasureEntryPurge(guest::Ptr<const void>(e.address), e.purgeBytes / 4);
        ++measured;
        if (!m.ok || m.purgeBytes != e.purgeBytes) {
            ++wrong;
            char buf[128];
            std::snprintf(buf, sizeof(buf), " %s:%s%u!=%u", e.hookName, m.ok ? "" : m.error, m.purgeBytes,
                          e.purgeBytes);
            names += buf;
        }
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "%u hooked entries called through the guest address, %u with a wrong purge", measured,
                  wrong);
    detail = buf + names;
    return wrong == 0;
}

}  // namespace xdk
