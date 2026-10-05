// D3D trap stubs and their registration with the hook engine (see d3d_traps.h).
#include "gfx/d3d_traps.h"

#include "core/guest.h"
#include "core/hooks.h"
#include "core/log.h"
#include "gfx/gfx_abi.h"
#include "gfx/gfx_config.h"
#include "gfx/purge_probe.h"

#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace gfx::d3d {
namespace {

constexpr bool TableConsistent() {
    for (std::uint32_t i = 0; i < kTrapCount; ++i) {
        const TrapRow& r = kTrapRows[i];
        if (r.index != i || r.purgeBytes % 4 != 0 || r.purgeBytes > 0x40 ||
            (r.conv == TrapConv::Fastcall && r.purgeBytes != 0) || (i > 0 && kTrapRows[i - 1].address >= r.address) ||
            r.address < guest::kSections[1].va || r.address >= guest::kSections[1].va + guest::kSections[1].virtualSize) {
            return false;
        }
    }
    return true;
}
static_assert(TableConsistent(), "gen/d3d_traps.gen.inc: index order, sorted addresses inside D3D, purge");

std::atomic<std::uint32_t> g_hits[kTrapCount];
std::mutex g_logMutex;
std::set<std::pair<std::uint32_t, std::uint32_t>> g_loggedSites;  // (trap, return address), log mode

std::string FormatArgs(std::uint32_t ecx, std::uint32_t edx, const std::uint32_t* args, std::uint32_t count,
                       TrapConv conv) {
    char buf[256];
    std::size_t used = 0;
    if (conv == TrapConv::Fastcall) {
        used = static_cast<std::size_t>(std::snprintf(buf, sizeof(buf), "(ecx=0x%08X, edx=0x%08X)", ecx, edx));
        return std::string(buf, used < sizeof(buf) ? used : sizeof(buf) - 1);
    }
    buf[used++] = '(';
    for (std::uint32_t i = 0; i < count && used + 16 < sizeof(buf); ++i) {
        used += static_cast<std::size_t>(
            std::snprintf(buf + used, sizeof(buf) - used, "%s0x%08X", i == 0 ? "" : ", ", args[i]));
    }
    buf[used++] = ')';
    return std::string(buf, used);
}

// Common trap handler: log, then abort (default) or return 0 (log mode).
std::uint32_t TrapHit(std::uint32_t index, void* returnAddress, std::uint32_t ecx, std::uint32_t edx,
                      const std::uint32_t* args, std::uint32_t count) noexcept {
    const TrapRow& row = kTrapRows[index];
    g_hits[index].fetch_add(1);
    const char* name = row.hookName + 5;  // without "trap:"
    const std::uint32_t from = guest::Addr(returnAddress);
    std::string where, argText;
    try {
        where = guest::Symbolize(from);
        argText = FormatArgs(ecx, edx, args, count, row.conv);
    } catch (...) {
    }
    if (GetSettings().trapMode == TrapMode::Abort) {
        FUT_LOGE(Gfx, "TRAP %s @0x%08X from %s %s", name, row.address, where.c_str(), argText.c_str());
        core::Fatal("D3D trap %s @0x%08X hit from %s (design 2.2: only the 8 D3D shim functions may run; "
                    "[render] d3d_traps = log continues with 0)", name, row.address, where.c_str());
    }
    try {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_loggedSites.size() < 256 && g_loggedSites.emplace(index, from).second) {
            FUT_LOGW(Gfx, "TRAP %s @0x%08X from %s %s - [render] d3d_traps = log: returning 0 (logged once per call "
                     "site)", name, row.address, where.c_str(), argText.c_str());
        }
    } catch (...) {
    }
    return 0;
}

template <std::size_t>
using Dword = std::uint32_t;

// stdcall trap with the original purge: callee pops sizeof...(I) DWORDs.
template <std::uint32_t Index, class Seq>
struct StdcallTrap;
template <std::uint32_t Index, std::size_t... I>
struct StdcallTrap<Index, std::index_sequence<I...>> {
    static std::uint32_t __stdcall Call(Dword<I>... args) noexcept {
        if (InPurgeProbe()) {
            return 0;
        }
        const std::uint32_t values[] = {args..., 0u};
        return TrapHit(Index, _ReturnAddress(), 0, 0, values, static_cast<std::uint32_t>(sizeof...(I)));
    }
};

// fastcall trap (D3DDevice_SetRenderState_Simple 0x2357D0: ecx = method header, edx = value, plain ret).
template <std::uint32_t Index>
struct FastcallTrap {
    static std::uint32_t __fastcall Call(std::uint32_t ecx, std::uint32_t edx) noexcept {
        if (InPurgeProbe()) {
            return 0;
        }
        return TrapHit(Index, _ReturnAddress(), ecx, edx, nullptr, 0);
    }
};

template <std::uint32_t Index>
const void* MakeTrapEntry() noexcept {
    constexpr TrapRow row = kTrapRows[Index];
    if constexpr (row.conv == TrapConv::Fastcall) {
        static_assert(row.purgeBytes == 0, "fastcall trap with stack arguments");
        return core::detail::CodeAddress(&FastcallTrap<Index>::Call);
    } else {
        using Trap = StdcallTrap<Index, std::make_index_sequence<row.purgeBytes / 4>>;
        static_assert(abi::StdcallPurge<decltype(&Trap::Call)>() == row.purgeBytes, "trap purge differs");
        return core::detail::CodeAddress(&Trap::Call);
    }
}

template <std::size_t... I>
constexpr auto MakeEntries(std::index_sequence<I...>) noexcept {
    return std::array<const void* (*)() noexcept, sizeof...(I)>{&MakeTrapEntry<static_cast<std::uint32_t>(I)>...};
}

const void* g_entries[kTrapCount];

// Static registration (like FUT_HOOK, but one registrar for the whole generated table).
struct TrapRegistrar {
    TrapRegistrar() noexcept {
        const auto makers = MakeEntries(std::make_index_sequence<kTrapCount>{});
        for (std::uint32_t i = 0; i < kTrapCount; ++i) {
            g_entries[i] = makers[i]();
            core::RegisterHook(core::HookDesc{kTrapRows[i].address, g_entries[i],
                                              kTrapRows[i].conv == TrapConv::Fastcall ? core::HookConv::Fastcall
                                                                                      : core::HookConv::Stdcall,
                                              kTrapRows[i].hookName, nullptr, true, __FILE__});
        }
    }
};
const TrapRegistrar g_registrar;

}  // namespace

const void* TrapEntry(std::uint32_t index) noexcept {
    return index < kTrapCount ? g_entries[index] : nullptr;
}

std::uint32_t TrapHits(std::uint32_t index) noexcept {
    return index < kTrapCount ? g_hits[index].load() : 0;
}

void LogTrapStatistics() noexcept {
    for (std::uint32_t i = 0; i < kTrapCount; ++i) {
        const std::uint32_t n = g_hits[i].load();
        if (n != 0) {
            FUT_LOGI(Gfx, "D3D trap %s @0x%08X hit %u times", kTrapRows[i].hookName + 5, kTrapRows[i].address, n);
        }
    }
}

}  // namespace gfx::d3d
