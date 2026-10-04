// XeLoadSection / XeUnloadSection (slots 85, 86). Owned by the kernel agent.
//
// The argument is a pointer to an XBE section header in the mapped header page (0x1036C + n*0x38,
// 10 headers, cpu.md/kernel.md 7.5; FUN_000f9b8e looks them up by name). The only caller chain is
// XapiMapLetterToDirectory 0xF8C41 / FUN_000f8b5a -> XLoadSectionByHandle 0xF9BF5 (returns
// header->VirtualAddress) -> NtWriteFile(VirtualSize bytes = XGetSectionSize, header+8) ->
// XFreeSectionByHandle 0xF9C16 (only checks the status). It loads "$$XTIMAGE" (not PRELOAD, flags
// 0x38) to write TitleImage.xbx; "$$XTINFO"/"$$XSIMAGE" do not exist in this XBE.
//
// All 10 sections are pre-mapped by the loader (design 1.4 step 4), so loading only maintains the
// Xbox bookkeeping in the guest header like the Xbox kernel: SectionReferenceCount (+0x18) and, on
// the first reference, the WORD counters at HeadSharedPageRefCountAddress/TailSharedPageRefCountAddress
// (+0x1C/+0x20, inside the header page 0x1059C..0x105AF). No XAPI code reads these counters
// (only +0x04, +0x08, +0x14 are read). For a non-PRELOAD section the first load additionally
// verifies the mapped bytes against the XBE file and restores them if they differ (the Xbox kernel
// reads them from the file at this point). Unloading never frees memory.
#include "core/guest.h"
#include "core/log.h"
#include "core/xbe_loader.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "shared/xbe_format.h"

#include <cstring>
#include <string>

namespace krnl {
namespace {

constexpr std::uint32_t kHeaderSize = sizeof(xbe::SectionHeader);  // 0x38
constexpr guest::GuestAddr kHeadersEnd = guest::kSectionHeaders + guest::kXbeSectionCount * kHeaderSize;

SRWLOCK g_sectionLock = SRWLOCK_INIT;

bool IsSectionHeader(const xbe::SectionHeader* section) noexcept {
    const guest::GuestAddr a = guest::Addr(section);
    return a >= guest::kSectionHeaders && a < kHeadersEnd &&
           (a - guest::kSectionHeaders) % kHeaderSize == 0 && guest::IsReadable(a, kHeaderSize);
}

std::string SectionName(const xbe::SectionHeader* section) {
    const guest::GuestAddr p = section->SectionNameAddress;
    if (!guest::InXbeImage(p, 1) || !guest::IsReadable(p, 1)) {
        return "?";
    }
    return std::string(guest::Ptr<const char>(p), strnlen(guest::Ptr<const char>(p), 32));
}

// Shared page counters live in the header page; never write outside of it.
std::uint16_t* SharedPageCounter(std::uint32_t address) noexcept {
    if (address < guest::kXbeBase ||
        address + sizeof(std::uint16_t) > guest::kXbeBase + guest::kXbeSizeOfHeaders) {
        return nullptr;
    }
    return guest::Ptr<std::uint16_t>(address);
}

void AdjustSharedPages(const xbe::SectionHeader* section, int delta) noexcept {
    std::uint16_t* head = SharedPageCounter(section->HeadSharedPageRefCountAddress);
    std::uint16_t* tail = SharedPageCounter(section->TailSharedPageRefCountAddress);
    for (std::uint16_t* c : {head, tail}) {
        if (c == nullptr) {
            continue;
        }
        if (delta > 0) {
            *c = static_cast<std::uint16_t>(*c + 1);
        } else if (*c > 0) {
            *c = static_cast<std::uint16_t>(*c - 1);
        }
    }
}

// Non-PRELOAD sections: the Xbox kernel reads the raw data from the XBE on the first load.
void VerifyAgainstFile(const xbe::SectionHeader* section, const std::string& name) noexcept {
    const auto file = core::XbeFileBytes();
    if (file.empty()) {
        FUT_LOGD(Krnl, "XeLoadSection(%s): XBE file bytes unavailable - mapping not verified", name.c_str());
        return;
    }
    const std::uint32_t raw = section->RawAddress;
    const std::uint32_t rawSize = section->RawSize;
    const std::uint32_t va = section->VirtualAddress;
    const std::uint32_t vsize = section->VirtualSize;
    if (raw > file.size() || rawSize > file.size() - raw || rawSize > vsize ||
        !guest::InXbeImage(va, vsize) || !guest::IsReadable(va, vsize)) {
        FUT_LOGE(Krnl, "XeLoadSection(%s): inconsistent header (raw 0x%X+0x%X, va 0x%X+0x%X)", name.c_str(),
                 raw, rawSize, va, vsize);
        return;
    }
    if (std::memcmp(guest::Ptr<const void>(va), file.data() + raw, rawSize) == 0) {
        FUT_LOGD(Krnl, "XeLoadSection(%s): mapped data at 0x%08X matches the XBE file (0x%X bytes)",
                 name.c_str(), va, rawSize);
        return;
    }
    FUT_LOGW(Krnl,
             "XeLoadSection(%s): mapped data at 0x%08X differs from the XBE file - restoring 0x%X bytes",
             name.c_str(), va, rawSize);
    guest::WriteBytes(va, file.data() + raw, rawSize);
    if (vsize > rawSize) {
        std::memset(guest::Ptr<std::uint8_t>(va + rawSize), 0, vsize - rawSize);
    }
}

}  // namespace

// XeLoadSection - slot 85, Ord 327, stdcall, 1 arg (purge 4): NTSTATUS (PXBE_SECTION_HEADER Section).
// Evidence: kernel_imports.tsv; single site XLoadSectionByHandle 0xF9BF5 (kernel.md 7.5 + review
// correction: $$XTIMAGE is loaded at boot by XapiMapLetterToDirectory); design 1.4 step 4.
NTSTATUS __stdcall XeLoadSection(xbe::SectionHeader* Section) noexcept {
    if (!IsSectionHeader(Section)) {
        FUT_LOGE(Krnl, "XeLoadSection(%p): not one of the %u XBE section headers at 0x%08X",
                 static_cast<void*>(Section), guest::kXbeSectionCount, guest::kSectionHeaders);
        return STATUS_INVALID_PARAMETER;
    }
    std::string name;
    try {
        name = SectionName(Section);
    } catch (...) {
    }
    AcquireSRWLockExclusive(&g_sectionLock);
    const std::uint32_t before = Section->SectionReferenceCount;
    if (before == 0) {
        AdjustSharedPages(Section, +1);
        if ((Section->Flags & xbe::kSectionPreload) == 0) {
            VerifyAgainstFile(Section, name);
        }
    }
    Section->SectionReferenceCount = before + 1;
    ReleaseSRWLockExclusive(&g_sectionLock);
    FUT_LOGD(Krnl, "XeLoadSection(%s @0x%08X, va 0x%08X size 0x%X) refs %u -> %u", name.c_str(),
             guest::Addr(Section), Section->VirtualAddress, Section->VirtualSize, before, before + 1);
    return STATUS_SUCCESS;
}
KRNL_FUNC(XeLoadSection, &XeLoadSection);

// XeUnloadSection - slot 86, Ord 328, stdcall, 1 arg (purge 4): NTSTATUS (PXBE_SECTION_HEADER Section).
// Evidence: kernel_imports.tsv; single site XFreeSectionByHandle 0xF9C16 (status only); kernel.md 7.5.
// ASSUMPTION: like the Xbox kernel, unloading a section with reference count 0 fails with
// STATUS_INVALID_PARAMETER.
NTSTATUS __stdcall XeUnloadSection(xbe::SectionHeader* Section) noexcept {
    if (!IsSectionHeader(Section)) {
        FUT_LOGE(Krnl, "XeUnloadSection(%p): not an XBE section header", static_cast<void*>(Section));
        return STATUS_INVALID_PARAMETER;
    }
    std::string name;
    try {
        name = SectionName(Section);
    } catch (...) {
    }
    AcquireSRWLockExclusive(&g_sectionLock);
    const std::uint32_t before = Section->SectionReferenceCount;
    if (before == 0) {
        ReleaseSRWLockExclusive(&g_sectionLock);
        FUT_LOGW(Krnl, "XeUnloadSection(%s): reference count already 0 -> STATUS_INVALID_PARAMETER",
                 name.c_str());
        return STATUS_INVALID_PARAMETER;
    }
    Section->SectionReferenceCount = before - 1;
    if (before == 1) {
        AdjustSharedPages(Section, -1);
    }
    ReleaseSRWLockExclusive(&g_sectionLock);
    FUT_LOGD(Krnl, "XeUnloadSection(%s) refs %u -> %u (memory stays mapped)", name.c_str(), before,
             before - 1);
    return STATUS_SUCCESS;
}
KRNL_FUNC(XeUnloadSection, &XeUnloadSection);

}  // namespace krnl
