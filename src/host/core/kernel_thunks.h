// Kernel thunk resolution (design 1.5). Owned by the core agent; uses the registry of
// kernel/kernel_api.h (krnl::ValidateRegistry, krnl::FindExportByOrdinal, krnl::GetBinding,
// krnl::LogBindingSummary).
#pragma once

namespace core {

// Stage "ResolveKernelThunks": for each of the 111 DWORDs at 0x293560 (0x80000000|ordinal, ends with
// 0 at 0x29371C) look up the ordinal (unknown ordinal -> abort), check the slot index matches the
// generated table, write krnl::GetBinding(slot).address; then log which slots are still stubs.
bool StageResolveKernelThunks();

}  // namespace core
