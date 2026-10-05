// Host-owned memory that is handed to the guest (design 5.3: guest-visible memory must lie below
// 0x80000000; the process is LARGEADDRESSAWARE, so ordinary heap blocks may end up above).
//
// - AllocGuestPages: zeroed pages (VirtualAlloc), e.g. the NiPCRenderer vtables, the dummy D3D
//   device and the pixel buffers of the D3D shim textures.
// - AllocGuestBlock: zeroed blocks of up to kGuestBlockSize bytes from 64 KiB slabs, e.g. the
//   0x14/0x18-byte D3D resource headers of the shim (the original takes them from XAPI LocalAlloc).
// Both return nullptr if the memory cannot be placed below 0x80000000. Thread-safe.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gfx {

void* AllocGuestPages(std::size_t size) noexcept;
void FreeGuestPages(void* pages) noexcept;

inline constexpr std::size_t kGuestBlockSize = 64;
void* AllocGuestBlock(std::size_t size) noexcept;
void FreeGuestBlock(void* block) noexcept;
// Number of blocks currently allocated (tests, statistics).
std::size_t GuestBlocksInUse() noexcept;

}  // namespace gfx
