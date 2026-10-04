// fs: segment retargeting (design 1.6, strategy A of cpu.md 2.4). Owned by the core agent.
// Patch list: guest::FsPatchSites() (generated from fs_sites.tsv, 38 sites).
#pragma once

#include <cstdint>

namespace core {

// Stage "ApplyFsPatches": for every site compare the original bytes, then replace the disp32 at
// site.dispOffset with 0xE10 + 4 * core::TlsSlotIndex(site.tlsSlot). Any mismatch aborts (and is
// detected before the first byte is written).
bool StageApplyFsPatches();

// New disp32 of a patched site for the given FutTlsSlot: 0xE10 + 4 * TlsAlloc index.
std::uint32_t FsPatchDisplacement(std::uint32_t tlsSlot) noexcept;

}  // namespace core
