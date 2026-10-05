// Kernel export registry and stub reporting (design 1.5, 5.4). See kernel_api.h.
// Part of the shared kernel_api unit (skeleton-owned, see src/INTERFACES.md).
#include "kernel/kernel_api.h"

#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"

#include <cstdio>
#include <string>
#include <unordered_set>

namespace krnl {
namespace {

struct Entry {
    std::uintptr_t address;
    ImplStatus status;
    std::uint32_t count;
    const char* implName;
    const char* file;
    const char* duplicateFile;  // second registration (error)
};

// Constant-initialized (zero) before any dynamic initializer runs, so registrars in other TUs can
// write here regardless of static initialization order.
Entry g_entries[gen::kSlotCount];
std::uint32_t g_badRegistrations = 0;

SRWLOCK g_seenLock = SRWLOCK_INIT;

std::unordered_set<std::uint64_t>& SeenSet() {
    static auto* set = new std::unordered_set<std::uint64_t>();  // never destroyed (used until exit)
    return *set;
}

const char* MilestoneName(gen::Milestone m) noexcept {
    switch (m) {
    case gen::Milestone::M1:
        return "M1";
    case gen::Milestone::M2:
        return "M2";
    case gen::Milestone::M7:
        return "M7";
    case gen::Milestone::Defensive:
        return "defensive";
    case gen::Milestone::ReplacedLibs:
        return "replaced-libs";
    }
    return "?";
}

const char* RetName(gen::Ret r) noexcept {
    switch (r) {
    case gen::Ret::Void:
        return "void";
    case gen::Ret::Ntstatus:
        return "NTSTATUS";
    case gen::Ret::Pointer:
        return "pointer";
    case gen::Ret::Boolean:
        return "BOOLEAN";
    case gen::Ret::Ulong:
        return "ULONG";
    }
    return "?";
}

const char* KindName(gen::Kind k) noexcept {
    switch (k) {
    case gen::Kind::Func:
        return "FUNC";
    case gen::Kind::Data:
        return "DATA";
    case gen::Kind::Addr:
        return "ADDR";
    }
    return "?";
}

}  // namespace

const gen::ExportInfo& ExportAt(std::uint32_t slot) noexcept {
    return gen::kExports[slot < gen::kSlotCount ? slot : 0];
}

const gen::ExportInfo* FindExportByOrdinal(std::uint32_t ordinal) noexcept {
    for (const auto& e : gen::kExports) {
        if (e.ordinal == ordinal) {
            return &e;
        }
    }
    return nullptr;
}

Binding GetBinding(std::uint32_t slot) noexcept {
    if (slot >= gen::kSlotCount) {
        return Binding{0, ImplStatus::Unimpl, "<invalid slot>", nullptr};
    }
    const Entry& e = g_entries[slot];
    if (e.count > 0) {
        return Binding{e.address, e.status, e.implName, e.file};
    }
    return Binding{gen::StubAddress(slot), ImplStatus::Unimpl, "<generated stub>", nullptr};
}

bool IsImplemented(std::uint32_t slot) noexcept {
    return slot < gen::kSlotCount && g_entries[slot].count > 0;
}

bool ValidateRegistry() noexcept {
    bool ok = g_badRegistrations == 0;
    if (!ok) {
        FUT_LOGE(Krnl, "%u kernel registrations with an invalid slot", g_badRegistrations);
    }
    for (std::uint32_t s = 0; s < gen::kSlotCount; ++s) {
        const Entry& e = g_entries[s];
        if (e.count > 1) {
            ok = false;
            FUT_LOGE(Krnl, "slot %u %s registered %u times (%s and %s)", s, gen::kExports[s].name, e.count,
                     e.file ? e.file : "?", e.duplicateFile ? e.duplicateFile : "?");
        }
        if (e.count > 0 && e.address == 0) {
            ok = false;
            FUT_LOGE(Krnl, "slot %u %s registered with a null address", s, gen::kExports[s].name);
        }
    }
    return ok;
}

std::uint32_t LogBindingSummary() noexcept {
    std::uint32_t impl = 0, stubOk = 0, unimpl = 0, unimplM1 = 0;
    for (std::uint32_t s = 0; s < gen::kSlotCount; ++s) {
        const gen::ExportInfo& x = gen::kExports[s];
        const Entry& e = g_entries[s];
        if (e.count > 0) {
            (e.status == ImplStatus::StubOk ? stubOk : impl)++;
            FUT_LOGD(Krnl, "slot %3u Ord %3u %-32s %s %s (%s)", s, x.ordinal, x.name, KindName(x.kind),
                     e.status == ImplStatus::StubOk ? "stub_ok" : "impl", e.implName);
            continue;
        }
        ++unimpl;
        if (x.milestone == gen::Milestone::M1) {
            ++unimplM1;
            FUT_LOGW(Krnl, "slot %3u Ord %3u %-32s %s still a generated %s (plan M1)", s, x.ordinal, x.name,
                     KindName(x.kind), x.kind == gen::Kind::Func ? "stub" : "placeholder");
        } else {
            FUT_LOGI(Krnl, "slot %3u Ord %3u %-32s %s still a generated %s (plan %s)%s", s, x.ordinal, x.name,
                     KindName(x.kind), x.kind == gen::Kind::Func ? "stub" : "placeholder",
                     MilestoneName(x.milestone), x.stubFatal ? ", aborts when called" : "");
        }
    }
    FUT_LOGI(
        Krnl,
        "kernel exports: %u impl, %u stub_ok, %u generated stubs/placeholders (%u of them planned for M1)",
        impl, stubOk, unimpl, unimplM1);
    return unimpl;
}

namespace detail {

void Register(std::uint32_t slot, std::uintptr_t address, ImplStatus status, const char* implName,
              const char* file) noexcept {
    if (slot >= gen::kSlotCount) {
        ++g_badRegistrations;
        return;
    }
    Entry& e = g_entries[slot];
    if (e.count == 0) {
        e.address = address;
        e.status = status;
        e.implName = implName;
        e.file = file;
    } else if (e.duplicateFile == nullptr) {
        e.duplicateFile = file;
    }
    ++e.count;
}

std::uint32_t UnimplementedResult(gen::Ret ret) noexcept {
    // Design 5.4 (review finding 1): only NTSTATUS exports report STATUS_NOT_IMPLEMENTED. For a
    // pointer/BOOLEAN/ULONG result 0xC0000002 would read as success (non-NULL, al = 2 = TRUE), so the
    // stub returns 0 (NULL/FALSE) and the guest takes its own failure path.
    return ret == gen::Ret::Ntstatus ? static_cast<std::uint32_t>(STATUS_NOT_IMPLEMENTED) : 0u;
}

std::uint32_t ReportUnimplemented(std::uint32_t slot, const std::uint32_t* args, std::uint32_t argCount,
                                  void* returnAddress) noexcept {
    const gen::ExportInfo& x = ExportAt(slot);
    const guest::GuestAddr ret = guest::Addr(returnAddress);
    const std::uint32_t result = UnimplementedResult(x.ret);
    try {
        bool first;
        AcquireSRWLockExclusive(&g_seenLock);
        try {
            first = SeenSet().insert((static_cast<std::uint64_t>(slot) << 32) | ret).second;
        } catch (...) {
            first = true;
        }
        ReleaseSRWLockExclusive(&g_seenLock);
        if (first || core::LogEnabled(core::Channel::Krnl, core::Level::Trace)) {
            char argText[24 * 12] = {};
            size_t pos = 0;
            for (std::uint32_t i = 0; i < argCount && pos + 14 < sizeof(argText); ++i) {
                pos += static_cast<size_t>(
                    std::snprintf(argText + pos, sizeof(argText) - pos, "%s0x%08X", i ? ", " : "", args[i]));
            }
            const std::string sym = guest::Symbolize(ret);
            core::LogWrite(core::Channel::Krnl, first ? core::Level::Warn : core::Level::Trace,
                           "UNIMPLEMENTED Ord %u %s(%s) ret=%s -> 0x%08X (%s)", x.ordinal, x.name, argText,
                           sym.c_str(), result, RetName(x.ret));
        }
    } catch (...) {
    }
    if (x.stubFatal) {
        // No return value lets this caller continue safely (gen_kernel_table.py FATAL_STUBS).
        core::Fatal("UNIMPLEMENTED kernel export Ord %u %s has no safe failure value (ret=0x%08X); "
                    "it is planned for %s",
                    x.ordinal, x.name, ret, MilestoneName(x.milestone));
    }
    if (core::GetConfig().strict) {
        if (IsDebuggerPresent()) {
            __debugbreak();
        }
        core::Fatal("UNIMPLEMENTED kernel export Ord %u %s called with [kernel] strict=1 (ret=0x%08X)",
                    x.ordinal, x.name, ret);
    }
    return result;
}

void ReportAddrPlaceholderCall(std::uint32_t slot, void* returnAddress) noexcept {
    const gen::ExportInfo& x = ExportAt(slot);
    std::string sym;
    try {
        sym = guest::Symbolize(guest::Addr(returnAddress));
    } catch (...) {
    }
    core::Fatal("guest called the placeholder of ADDR export Ord %u %s (no implementation registered) ret=%s",
                x.ordinal, x.name, sym.c_str());
}

}  // namespace detail
}  // namespace krnl
