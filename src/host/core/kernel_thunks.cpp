// Kernel thunk resolution (design 1.5, E4). Owned by the core agent.
//
// Evidence: kernel.md 1 (111 DWORDs 0x80000000|ordinal at 0x293560, terminator 0 at 0x29371C),
// kernel_imports.tsv (slot <-> ordinal), design 1.5 ("unknown ordinal aborts; there is none").
#include "core/kernel_thunks.h"

#include "core/guest.h"
#include "core/log.h"
#include "core/xbe_loader.h"
#include "kernel/kernel_api.h"

namespace core {

static_assert(guest::kKernelThunkTable == krnl::gen::kThunkTableVa,
              "thunk table address: guest.h vs generated table");
static_assert(guest::kKernelThunkTerminator == krnl::gen::kThunkTableEnd,
              "thunk table end: guest.h vs generated table");
static_assert(guest::kKernelThunkCount == krnl::gen::kSlotCount, "slot count: guest.h vs generated table");
static_assert(guest::kKernelThunkTable + 4 * guest::kKernelThunkCount == guest::kKernelThunkTerminator);

bool StageResolveKernelThunks() {
    if (!krnl::ValidateRegistry()) {
        FUT_LOGE(Core, "kernel export registry is inconsistent (see KRNL errors above)");
        return false;
    }
    // Read every slot from the ORIGINAL file bytes (the mapped table is rewritten below).
    std::uint32_t original[guest::kKernelThunkCount + 1] = {};
    if (!ReadOriginalXbeBytes(guest::kKernelThunkTable, original, sizeof(original))) {
        FUT_LOGE(Core, "thunk table 0x%08X not readable from the XBE file", guest::kKernelThunkTable);
        return false;
    }
    if (original[guest::kKernelThunkCount] != 0) {
        FUT_LOGE(Core, "thunk table terminator at 0x%08X is 0x%08X, expected 0",
                 guest::kKernelThunkTerminator, original[guest::kKernelThunkCount]);
        return false;
    }
    std::uint32_t resolved[guest::kKernelThunkCount] = {};
    std::uint32_t impl = 0, stubOk = 0, generated = 0;
    for (std::uint32_t slot = 0; slot < guest::kKernelThunkCount; ++slot) {
        const guest::GuestAddr slotAddr = guest::kKernelThunkTable + 4 * slot;
        const std::uint32_t value = original[slot];
        if ((value & guest::kKernelThunkOrdinalFlag) == 0) {
            FUT_LOGE(Core, "thunk slot %u at 0x%08X = 0x%08X is not an ordinal import", slot, slotAddr,
                     value);
            return false;
        }
        const std::uint32_t ordinal = value & ~guest::kKernelThunkOrdinalFlag;
        const krnl::gen::ExportInfo* exp = krnl::FindExportByOrdinal(ordinal);
        if (exp == nullptr) {
            FUT_LOGE(Core, "thunk slot %u at 0x%08X imports unknown ordinal %u - aborting (design 1.5)", slot,
                     slotAddr, ordinal);
            return false;
        }
        if (exp->slot != slot) {
            FUT_LOGE(Core, "thunk slot %u imports Ord %u %s, but the generated table expects it in slot %u",
                     slot, ordinal, exp->name, exp->slot);
            return false;
        }
        const krnl::Binding b = krnl::GetBinding(slot);
        if (b.address == 0) {
            FUT_LOGE(Core, "thunk slot %u Ord %u %s has no address (not even a generated stub)", slot,
                     ordinal, exp->name);
            return false;
        }
        resolved[slot] = static_cast<std::uint32_t>(b.address);
        switch (b.status) {
        case krnl::ImplStatus::Impl:
            ++impl;
            break;
        case krnl::ImplStatus::StubOk:
            ++stubOk;
            break;
        case krnl::ImplStatus::Unimpl:
            ++generated;
            break;
        }
        FUT_LOGT(Core, "thunk slot %3u Ord %3u %-32s -> 0x%08X (%s)", slot, ordinal, exp->name,
                 resolved[slot], b.implName);
    }
    if (!guest::WriteBytes(guest::kKernelThunkTable, resolved, sizeof(resolved))) {
        FUT_LOGE(Core, "cannot write the thunk table at 0x%08X", guest::kKernelThunkTable);
        return false;
    }
    FUT_LOGI(Core, "kernel thunks: %u slots resolved (%u impl, %u stub_ok, %u generated stubs/placeholders)",
             guest::kKernelThunkCount, impl, stubOk, generated);
    krnl::LogBindingSummary();
    return true;
}

}  // namespace core
