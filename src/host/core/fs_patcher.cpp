// fs: segment retargeting (design 1.6, strategy A of cpu.md 2.4). Owned by the core agent.
//
// Every one of the 38 sites (gen/fs_patch_table.gen.inc, generated from fs_sites.tsv rows with
// offset != 0) is an instruction with an absolute disp32 relative to fs (encodings 64 A1, 64 8B 0D,
// 64 8B 3D, 64 0F B6 05; cpu.md 2.3). Only the disp32 is replaced by 0xE10 + 4*k, the address of
// TEB.TlsSlots[k] for the TlsAlloc index k of the slot the original KPCR offset maps to:
//   0x04 -> S_TLS, 0x20 -> S_PRCB, 0x24 -> S_IRQL (byte read = low byte), 0x28 -> S_KTHREAD, 0x58 -> S_DPC.
#include "core/fs_patcher.h"

#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"

#include <cstring>

namespace core {
namespace {

std::uint32_t ExpectedSlotForDisp(std::uint32_t disp) noexcept {
    switch (disp) {
    case 0x04:
        return FUT_TLS_S_TLS;
    case 0x20:
        return FUT_TLS_S_PRCB;
    case 0x24:
        return FUT_TLS_S_IRQL;
    case 0x28:
        return FUT_TLS_S_KTHREAD;
    case 0x58:
        return FUT_TLS_S_DPC;
    default:
        return FUT_TLS_SLOT_COUNT;
    }
}

}  // namespace

std::uint32_t FsPatchDisplacement(std::uint32_t tlsSlot) noexcept {
    return FUT_TEB_TLS_SLOTS_OFFSET + 4 * TlsSlotIndex(static_cast<FutTlsSlot>(tlsSlot));
}

bool StageApplyFsPatches() {
    const auto sites = guest::FsPatchSites();
    std::uint32_t perSlot[FUT_TLS_SLOT_COUNT] = {};
    // Validate everything first so that a mismatch leaves the image untouched.
    for (const guest::FsPatchSite& s : sites) {
        if (s.length < 6 || s.length > sizeof(s.original) || s.dispOffset + 4u > s.length ||
            s.tlsSlot >= FUT_TLS_SLOT_COUNT || ExpectedSlotForDisp(s.originalDisp) != s.tlsSlot) {
            FUT_LOGE(Core, "fs patch table row 0x%08X is malformed (len %u, disp@%u, disp 0x%X, slot %u)",
                     s.address, s.length, s.dispOffset, s.originalDisp, s.tlsSlot);
            return false;
        }
        if (s.original[0] != 0x64) {
            FUT_LOGE(Core, "fs patch site 0x%08X: table bytes do not start with the fs prefix", s.address);
            return false;
        }
        std::uint32_t disp = 0;
        std::memcpy(&disp, &s.original[s.dispOffset], sizeof(disp));
        if (disp != s.originalDisp) {
            FUT_LOGE(Core, "fs patch site 0x%08X: disp32 in the table is 0x%X, expected 0x%X", s.address,
                     disp, s.originalDisp);
            return false;
        }
        if (std::memcmp(guest::Ptr<const void>(s.address), s.original, s.length) != 0) {
            FUT_LOGE(Core,
                     "fs patch site 0x%08X (%s, %s): original bytes differ - wrong XBE or already patched",
                     s.address, s.function, s.library);
            return false;
        }
        const std::uint32_t index = TlsSlotIndex(static_cast<FutTlsSlot>(s.tlsSlot));
        if (index >= FUT_TEB_TLS_SLOTS_INLINE) {
            FUT_LOGE(Core, "TLS slot %u has index %u >= 64 (not addressable through fs:[0xE10+4k])",
                     s.tlsSlot, index);
            return false;
        }
    }
    for (const guest::FsPatchSite& s : sites) {
        const std::uint32_t newDisp = FsPatchDisplacement(s.tlsSlot);
        if (!guest::WriteBytes(s.address + s.dispOffset, &newDisp, sizeof(newDisp))) {
            FUT_LOGE(Core, "fs patch site 0x%08X: write failed", s.address);
            return false;
        }
        ++perSlot[s.tlsSlot];
        FUT_LOGT(Core, "fs patch 0x%08X %-40s fs:[0x%02X] -> fs:[0x%03X]", s.address, s.function,
                 s.originalDisp, newDisp);
    }
    FUT_LOGI(
        Core,
        "fs patches: %zu sites retargeted (S_TLS %u -> fs:[0x%X], S_KTHREAD %u -> fs:[0x%X], S_PRCB %u -> "
        "fs:[0x%X], S_IRQL %u -> fs:[0x%X], S_DPC %u -> fs:[0x%X])",
        sites.size(), perSlot[FUT_TLS_S_TLS], FsPatchDisplacement(FUT_TLS_S_TLS), perSlot[FUT_TLS_S_KTHREAD],
        FsPatchDisplacement(FUT_TLS_S_KTHREAD), perSlot[FUT_TLS_S_PRCB], FsPatchDisplacement(FUT_TLS_S_PRCB),
        perSlot[FUT_TLS_S_IRQL], FsPatchDisplacement(FUT_TLS_S_IRQL), perSlot[FUT_TLS_S_DPC],
        FsPatchDisplacement(FUT_TLS_S_DPC));
    return true;
}

}  // namespace core
