// D3D traps (design 2.1 "D3D8" row, 3.2 "Falle", 5.4): every D3D entry point with external callers
// (d3d_functions.tsv, ext_call_sites > 0) that is not one of the 8 shims gets a detour to a trap stub
// with the same convention and stack purge. A hit logs
//   TRAP <name> @<address> from <return address + symbol> (<arguments>)
// on GFX/error and aborts (core::Fatal, exit code 3); with [render] d3d_traps = log it logs once per call
// site and returns 0 instead. After the NiXBoxRenderer::Create detour nothing should call D3D any more,
// so a trap reports a wrong assumption at once (design 2.2 "Rückfallposition").
//
// The table is generated from analysis/loader/d3d_functions.tsv by tools/gen/gen_d3d_traps.py
// (gen/d3d_traps.gen.inc, purge re-verified with capstone against the XBE). The traps are registered
// through the hook engine (core::RegisterHook = FUT_HOOK machinery: function-start, size, overlap and
// original-byte validation); hook names "trap:<function>", so [hooks] disable = trap:* disables all.
#pragma once

#include <cstdint>
#include <iterator>

namespace gfx::d3d {

enum class TrapConv : std::uint8_t { Stdcall, Fastcall };

struct TrapRow {
    std::uint32_t index;
    std::uint32_t address;     // D3D function start
    std::uint32_t purgeBytes;  // ret N of the original (tail jumps followed)
    TrapConv conv;             // Fastcall only for D3DDevice_SetRenderState_Simple (ecx/edx, d3d.md 3.4)
    std::uint32_t extCallSites;
    const char* hookName;      // "trap:D3D8::..."
};

inline constexpr TrapRow kTrapRows[] = {
#define FUT_D3D_TRAP(index, address, purge, conv, sites, name) {index, address, purge, TrapConv::conv, sites, name},
#include "gen/d3d_traps.gen.inc"
#undef FUT_D3D_TRAP
};
inline constexpr std::uint32_t kTrapCount = static_cast<std::uint32_t>(std::size(kTrapRows));

// Host entry point of trap `index` (the detour target), nullptr past the end.
const void* TrapEntry(std::uint32_t index) noexcept;
// Number of hits of trap `index` (only > 0 in log mode; in abort mode the first hit ends the process).
std::uint32_t TrapHits(std::uint32_t index) noexcept;
// Logs the traps that were hit (GFX/info), nothing if none.
void LogTrapStatistics() noexcept;

}  // namespace gfx::d3d
