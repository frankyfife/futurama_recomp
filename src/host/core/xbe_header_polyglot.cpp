// PE/XBE polyglot header (design 1.4 step 5, E2). Owned by the core agent.
//
// The header page 0x10000 holds the launcher's PE headers, which Windows keeps reading at runtime
// (RtlImageNtHeader for CreateThread's default stack size, SEH/debugger/module APIs). The guest needs
// the XBE header at the same address but reads it only from 0x10108 on (design_header_refs.out), so
// both fit: XBE header 0x10000..0x10968 with 'MZ' over the magic and e_lfanew (0x1003C, inside the
// never-read RSA signature) pointing at a copy of the launcher's NT headers at 0x10970.
#include "core/xbe_header_polyglot.h"

#include "core/guest.h"
#include "core/log.h"
#include "core/xbe_loader.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace core {
namespace {

constexpr std::uint32_t kPageSize = 0x1000;
constexpr std::uint32_t kMaxLauncherLfanew = 0x400;  // launcher SizeOfHeaders

}  // namespace

bool StageBuildPolyglotHeader() {
    const auto file = XbeFileBytes();
    if (file.size() < guest::kXbeSizeOfHeaders) {
        FUT_LOGE(Core, "polyglot: no verified XBE");
        return false;
    }

    // 1. Save the launcher's NT headers + section table (still intact at 0x10000).
    const auto* dos = guest::Ptr<const IMAGE_DOS_HEADER>(guest::kXbeBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<std::uint32_t>(dos->e_lfanew) > kMaxLauncherLfanew) {
        FUT_LOGE(Core, "polyglot: launcher DOS header at 0x%08X is not intact (magic 0x%04X, e_lfanew 0x%lX)",
                 guest::kXbeBase, dos->e_magic, static_cast<unsigned long>(dos->e_lfanew));
        return false;
    }
    const guest::GuestAddr ntAddr = guest::kXbeBase + static_cast<std::uint32_t>(dos->e_lfanew);
    const auto* nt = guest::Ptr<const IMAGE_NT_HEADERS32>(ntAddr);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.ImageBase != guest::kXbeBase) {
        FUT_LOGE(Core, "polyglot: launcher NT headers at 0x%08X are not a PE32 image at 0x%08X", ntAddr,
                 guest::kXbeBase);
        return false;
    }
    const std::uint32_t ntSize =
        static_cast<std::uint32_t>(offsetof(IMAGE_NT_HEADERS32, OptionalHeader)) +
        nt->FileHeader.SizeOfOptionalHeader +
        nt->FileHeader.NumberOfSections * static_cast<std::uint32_t>(sizeof(IMAGE_SECTION_HEADER));
    const std::uint32_t room = guest::kPolyglotFreeEnd - guest::kPolyglotNtHeaders;
    if (ntSize > room) {
        FUT_LOGE(Core, "polyglot: launcher NT headers need 0x%X bytes, only 0x%X free at 0x%08X", ntSize,
                 room, guest::kPolyglotNtHeaders);
        return false;
    }
    static_assert(guest::kPolyglotNtHeaders >= guest::kXbeBase + guest::kXbeSizeOfHeaders,
                  "NT header copy must not overlap the XBE header");
    static_assert(guest::kPolyglotLfanewAddr + 4 <= guest::kFirstGuestReadHeaderAddr &&
                      guest::kPolyglotMzAddr + 2 <= guest::kFirstGuestReadHeaderAddr,
                  "MZ/e_lfanew must lie in header bytes the guest never reads");

    // 2. Build the new page off-line, then replace the page in one copy (keeps the window in which
    //    RtlImageNtHeader would see an invalid header minimal).
    std::vector<std::uint8_t> page(kPageSize, 0);
    std::memcpy(page.data(), file.data(), guest::kXbeSizeOfHeaders);  // XBE header
    const std::uint16_t mz = IMAGE_DOS_SIGNATURE;                     // 'MZ'
    std::memcpy(&page[guest::kPolyglotMzAddr - guest::kXbeBase], &mz, sizeof(mz));
    const std::uint32_t lfanew = guest::kPolyglotNtHeaders - guest::kXbeBase;  // 0x970
    std::memcpy(&page[guest::kPolyglotLfanewAddr - guest::kXbeBase], &lfanew, sizeof(lfanew));
    std::memcpy(&page[lfanew], nt, ntSize);  // NT headers + sections

    DWORD oldProtect = 0;
    if (!VirtualProtect(guest::Ptr<void>(guest::kXbeBase), kPageSize, PAGE_READWRITE, &oldProtect)) {
        FUT_LOGE(Core, "polyglot: VirtualProtect(0x%08X) failed (error %lu)", guest::kXbeBase,
                 GetLastError());
        return false;
    }
    std::memcpy(guest::Ptr<void>(guest::kXbeBase), page.data(), kPageSize);
    // Design 1.2: the header page stays RW (entry may write the certificate size at 0xF84F3).

    // 3. Verify with the loader's own parser.
    using RtlImageNtHeaderFn = PIMAGE_NT_HEADERS(NTAPI*)(PVOID);
    const auto rtlImageNtHeader = reinterpret_cast<RtlImageNtHeaderFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlImageNtHeader"));
    const PIMAGE_NT_HEADERS check =
        rtlImageNtHeader != nullptr ? rtlImageNtHeader(guest::Ptr<void>(guest::kXbeBase)) : nullptr;
    if (check != guest::Ptr<IMAGE_NT_HEADERS>(guest::kPolyglotNtHeaders)) {
        FUT_LOGE(Core, "polyglot: RtlImageNtHeader(0x%08X) = %p, expected 0x%08X", guest::kXbeBase,
                 static_cast<void*>(check), guest::kPolyglotNtHeaders);
        return false;
    }
    if (std::memcmp(guest::Ptr<const void>(guest::kFirstGuestReadHeaderAddr),
                    file.data() + (guest::kFirstGuestReadHeaderAddr - guest::kXbeBase),
                    guest::kXbeBase + guest::kXbeSizeOfHeaders - guest::kFirstGuestReadHeaderAddr) != 0) {
        FUT_LOGE(Core, "polyglot: XBE header bytes 0x%08X.. differ from the file",
                 guest::kFirstGuestReadHeaderAddr);
        return false;
    }
    FUT_LOGI(Core,
             "polyglot header: XBE header 0x%X bytes at 0x%08X, 'MZ' + e_lfanew 0x%X, launcher NT headers "
             "(0x%X bytes, %u sections) at 0x%08X",
             guest::kXbeSizeOfHeaders, guest::kXbeBase, lfanew, ntSize,
             static_cast<unsigned>(check->FileHeader.NumberOfSections), guest::kPolyglotNtHeaders);
    return true;
}

}  // namespace core
