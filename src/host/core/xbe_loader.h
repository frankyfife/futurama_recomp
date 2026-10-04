// XBE loading and mapping (design 1.4 steps 1-4, 7). Owned by the core agent.
#pragma once

#include "core/config.h"
#include "core/guest.h"

#include <cstdint>
#include <span>

namespace core {

// Stage "LoadXbe": reads default.xbe from [paths] xbe or, if that is not set, from the ISO
// ([paths] iso, via vfs::ReadIsoFile), verifies size and SHA-1 (guest::kXbeSha1) and the header
// (magic, base 0x10000, SizeOfHeaders 0x968, SizeOfImage 0x30A300, entry 0xF84DD and thunk table
// 0x293560 after the retail XOR keys, 10 sections matching guest::kSections).
bool StageLoadXbe(const Config& config);

// Stage "MapXbe": VirtualProtect(0x10000, 0x30B000, RWX), zero 0x11000..0x31B000, copy all 10
// sections (incl. $$XTIMAGE and .XTLID). The header page is written by the polyglot stage.
bool StageMapXbe();

// The verified XBE file (valid after StageLoadXbe; used by self-tests and XeLoadSection checks).
std::span<const std::uint8_t> XbeFileBytes() noexcept;

// File offset of the byte that is mapped at guest address `va` (header or section raw data);
// false for BSS, gaps and addresses outside the image or before StageLoadXbe.
bool XbeFileOffset(guest::GuestAddr va, std::uint32_t& offset) noexcept;
// Copies `size` ORIGINAL bytes (as in the XBE file, before any thunk/fs/hook patch) of guest address
// `va`. BSS bytes read as 0. False if any byte lies outside the image or the file is not loaded.
bool ReadOriginalXbeBytes(guest::GuestAddr va, void* out, std::uint32_t size) noexcept;

}  // namespace core
