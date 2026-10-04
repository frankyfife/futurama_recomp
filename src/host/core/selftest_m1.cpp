// Self-tests of design 1.4 step 8 (M1, enabled with --selftest / [debug] selftest=1). Owned by the
// core agent. Phases (core/selftest.h): Host before the XBE is touched, Mapped after mapping,
// polyglot, thunks, fs patches and hooks, AdoptedThread on the boot thread right before entry.
#include "core/crash.h"
#include "core/fs_patcher.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/guest_thread.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/selftest.h"
#include "core/xbe_loader.h"
#include "kernel/kernel_api.h"
#include "shared/xbe_format.h"
#include "shared/xbox_types.h"

#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace core {
namespace {

std::string Format(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

// Host functions with known conventions, called through the MASM guest-call helpers.
std::uint32_t __stdcall ProbeStdcall(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    return a * 100 + b * 10 + c;
}
std::uint32_t __cdecl ProbeCdecl(std::uint32_t a, std::uint32_t b) {
    return a - b;
}
std::uint32_t __fastcall ProbeFastcall(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    return (a << 16) | (b << 8) | c;
}
std::uint64_t __stdcall ProbeStdcall64(std::uint32_t lo, std::uint32_t hi) {
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

guest::GuestAddr CodeOf(const void* p) {
    return guest::Addr(p);
}

}  // namespace

// ---- Host phase -----------------------------------------------------------------------------

// Design 1.4 step 8 item 1 / cpu.md 2.4 A step 1: TlsSetValue(k) and fs:[0xE10+4k] are the same DWORD.
FUT_SELFTEST(TlsSlotsReadBack, Host) {
    static const char* const kNames[FUT_TLS_SLOT_COUNT] = {"S_TLS", "S_KTHREAD", "S_PRCB", "S_IRQL", "S_DPC"};
    std::string ok;
    for (int s = 0; s < FUT_TLS_SLOT_COUNT; ++s) {
        const DWORD index = TlsSlotIndex(static_cast<FutTlsSlot>(s));
        if (index >= FUT_TEB_TLS_SLOTS_INLINE) {
            detail = Format("%s has TlsAlloc index %lu >= 64", kNames[s], index);
            return false;
        }
        void* const old = TlsGetValue(index);
        const std::uint32_t pattern = 0xA5C30000u | (static_cast<std::uint32_t>(s) << 8) | index;
        TlsSetValue(index, guest::Ptr<void>(pattern));
        const unsigned long viaFs = __readfsdword(FUT_TEB_TLS_SLOTS_OFFSET + 4 * index);
        TlsSetValue(index, old);
        if (viaFs != pattern ||
            FsPatchDisplacement(static_cast<std::uint32_t>(s)) != FUT_TEB_TLS_SLOTS_OFFSET + 4 * index) {
            detail = Format("%s index %lu: fs:[0x%lX] = 0x%08lX, expected 0x%08X", kNames[s], index,
                            FUT_TEB_TLS_SLOTS_OFFSET + 4 * index, viaFs, pattern);
            return false;
        }
        ok +=
            Format("%s%s=fs:[0x%lX]", ok.empty() ? "" : " ", kNames[s], FUT_TEB_TLS_SLOTS_OFFSET + 4 * index);
    }
    detail = ok;
    return true;
}

// The MASM host->guest call helpers (asm/guest_call.asm) pass arguments and restore esp correctly
// for callee-cleanup, caller-cleanup and register conventions.
FUT_SELFTEST(GuestCallThunks, Host) {
    const std::uint32_t r1 = CallGuest(CodeOf(reinterpret_cast<const void*>(&ProbeStdcall)), {1, 2, 3});
    const std::uint32_t r2 = CallGuest(CodeOf(reinterpret_cast<const void*>(&ProbeCdecl)), {50, 8});
    const std::uint32_t r3 =
        CallGuestFastcall(CodeOf(reinterpret_cast<const void*>(&ProbeFastcall)), 0x11, 0x22, {0x33});
    const std::uint64_t r4 =
        CallGuest64(CodeOf(reinterpret_cast<const void*>(&ProbeStdcall64)), {0x89ABCDEF, 0x01234567});
    detail = Format("stdcall %u, cdecl %u, fastcall 0x%06X, edx:eax 0x%016llX", r1, r2, r3,
                    static_cast<unsigned long long>(r4));
    return r1 == 123 && r2 == 42 && r3 == 0x112233 && r4 == 0x0123456789ABCDEFull;
}

// ---- Mapped phase ---------------------------------------------------------------------------

// Design 1.4 step 8 item 2: RtlImageNtHeader(GetModuleHandle(NULL)) != NULL after the polyglot.
FUT_SELFTEST(PolyglotNtHeader, Mapped) {
    using RtlImageNtHeaderFn = PIMAGE_NT_HEADERS(NTAPI*)(PVOID);
    const auto rtlImageNtHeader = reinterpret_cast<RtlImageNtHeaderFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlImageNtHeader"));
    const HMODULE exe = GetModuleHandleW(nullptr);
    const PIMAGE_NT_HEADERS nt = rtlImageNtHeader != nullptr ? rtlImageNtHeader(exe) : nullptr;
    HMODULE handlerModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       guest::Ptr<const wchar_t>(guest::kCrtExceptHandler3), &handlerModule);
    const std::uint32_t magicArea = guest::Read<std::uint32_t>(guest::kXbeBase + 0x104);  // XBE BaseAddress
    detail = Format(
        "GetModuleHandle(NULL) = %p, RtlImageNtHeader = %p, ImageBase 0x%08lX, SizeOfStackReserve 0x%lX, "
        "_except_handler3 module %p, XBE BaseAddress field 0x%08X",
        static_cast<void*>(exe), static_cast<void*>(nt), nt ? nt->OptionalHeader.ImageBase : 0ul,
        nt ? nt->OptionalHeader.SizeOfStackReserve : 0ul, static_cast<void*>(handlerModule), magicArea);
    return exe == guest::Ptr<void>(guest::kXbeBase) &&
           nt == guest::Ptr<IMAGE_NT_HEADERS>(guest::kPolyglotNtHeaders) &&
           nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.ImageBase == guest::kXbeBase &&
           handlerModule == exe && magicArea == guest::kXbeBase;
}

namespace {
struct ThreadProbe {
    std::uint32_t stackBase;
    std::uint32_t reservationBase;
};
DWORD WINAPI ZeroStackThread(void* param) {
    auto* probe = static_cast<ThreadProbe*>(param);
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(&mbi, &mbi, sizeof(mbi));
    probe->stackBase = guest::Addr(tib->StackBase);
    probe->reservationBase = guest::Addr(mbi.AllocationBase);
    return 0x600D;
}
}  // namespace

// Design 1.4 step 8 item 3: CreateThread with stack size 0 from the host DLL (BaseCreateStack reads
// SizeOfStackReserve through the polyglot NT headers).
FUT_SELFTEST(CreateThreadZeroStack, Mapped) {
    ThreadProbe probe{};
    DWORD tid = 0;
    const HANDLE h = CreateThread(nullptr, 0, &ZeroStackThread, &probe, 0, &tid);
    if (h == nullptr) {
        detail = Format("CreateThread failed (error %lu)", GetLastError());
        return false;
    }
    const DWORD wait = WaitForSingleObject(h, 10000);
    DWORD code = 0;
    GetExitCodeThread(h, &code);
    CloseHandle(h);
    const std::uint32_t reserve = probe.stackBase - probe.reservationBase;
    detail = Format("thread exit 0x%lX, stack reservation 0x%X", code, reserve);
    return wait == WAIT_OBJECT_0 && code == 0x600D && reserve >= 0x10000;
}

// Design 1.4 step 8 item 5: the mapped image equals the file (sections) and is zero elsewhere (BSS
// tails, gaps), except for the bytes the loader patched on purpose (thunk table, fs disp32, hooks).
FUT_SELFTEST(MappingMatchesFile, Mapped) {
    const auto file = XbeFileBytes();
    if (file.empty()) {
        detail = "no XBE loaded";
        return false;
    }
    const std::uint32_t begin = guest::kXbeSectionsBegin,
                        size = guest::kXbeSpaceEnd - guest::kXbeSectionsBegin;
    std::vector<std::uint8_t> expected(size, 0);
    for (std::uint32_t i = 0; i < guest::kXbeSectionCount; ++i) {
        xbe::SectionHeader s{};
        std::memcpy(&s, file.data() + (guest::kSectionHeaders - guest::kXbeBase) + i * sizeof(s), sizeof(s));
        std::memcpy(&expected[s.VirtualAddress - begin], file.data() + s.RawAddress, s.RawSize);
    }
    // Intentional patches: copy the live bytes over the expectation.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> patched;  // [address, length)
    patched.emplace_back(guest::kKernelThunkTable, 4 * guest::kKernelThunkCount);
    for (const auto& site : guest::FsPatchSites()) {
        patched.emplace_back(site.address + site.dispOffset, 4u);
    }
    for (const auto& hook : InstalledHookList()) {
        patched.emplace_back(hook.address, 5u);
    }
    for (const auto& [address, length] : patched) {
        std::memcpy(&expected[address - begin], guest::Ptr<const void>(address), length);
    }
    const auto* live = guest::Ptr<const std::uint8_t>(begin);
    std::uint32_t mismatches = 0, first = 0;
    for (std::uint32_t off = 0; off < size; ++off) {
        if (live[off] != expected[off]) {
            if (mismatches++ == 0) {
                first = begin + off;
            }
        }
    }
    // Header bytes the guest reads (0x10108..0x10968) must equal the file as well.
    const std::uint32_t hdr = guest::kFirstGuestReadHeaderAddr;
    const bool headerOk = std::memcmp(guest::Ptr<const void>(hdr), file.data() + (hdr - guest::kXbeBase),
                                      guest::kXbeBase + guest::kXbeSizeOfHeaders - hdr) == 0;
    detail =
        Format("0x%08X..0x%08X: %u differing bytes%s (excluded %zu patched ranges), header %s", begin,
               guest::kXbeSpaceEnd, mismatches, mismatches ? Format(", first at 0x%08X", first).c_str() : "",
               patched.size(), headerOk ? "ok" : "DIFFERS");
    return mismatches == 0 && headerOk;
}

// The 38 fs patch sites now address TEB.TlsSlots (design 1.6), all other instruction bytes unchanged.
FUT_SELFTEST(FsPatchesApplied, Mapped) {
    std::uint32_t good = 0;
    for (const auto& site : guest::FsPatchSites()) {
        std::uint8_t expected[8];
        std::memcpy(expected, site.original, sizeof(expected));
        const std::uint32_t disp = FsPatchDisplacement(site.tlsSlot);
        std::memcpy(&expected[site.dispOffset], &disp, sizeof(disp));
        if (std::memcmp(guest::Ptr<const void>(site.address), expected, site.length) != 0) {
            detail = Format("site 0x%08X (%s) is not patched as expected", site.address, site.function);
            return false;
        }
        ++good;
    }
    detail = Format("%u/%zu sites retargeted", good, guest::FsPatchSites().size());
    return good == 38;
}

// All 111 thunk slots hold the binding of the kernel registry (design 1.5), terminator 0.
FUT_SELFTEST(KernelThunksResolved, Mapped) {
    std::uint32_t impl = 0;
    for (std::uint32_t slot = 0; slot < guest::kKernelThunkCount; ++slot) {
        const std::uint32_t value = guest::Read<std::uint32_t>(guest::kKernelThunkTable + 4 * slot);
        const krnl::Binding b = krnl::GetBinding(slot);
        if (value == 0 || value != b.address) {
            detail = Format("slot %u (%s) = 0x%08X, binding 0x%08X", slot, krnl::ExportAt(slot).name, value,
                            static_cast<std::uint32_t>(b.address));
            return false;
        }
        impl += b.status != krnl::ImplStatus::Unimpl ? 1 : 0;
    }
    const std::uint32_t terminator = guest::Read<std::uint32_t>(guest::kKernelThunkTerminator);
    detail =
        Format("111 slots resolved (%u registered implementations), terminator 0x%08X", impl, terminator);
    return terminator == 0;
}

// Every installed hook is a 5-byte E9 rel32 to its replacement (design 3.1). Probe hooks: the
// trampoline holds the original stolen bytes (XBE file) followed by E9 rel32 to address+stolen.
FUT_SELFTEST(HooksInstalled, Mapped) {
    const auto hooks = InstalledHookList();
    std::size_t probes = 0;
    for (const auto& h : hooks) {
        const auto* p = guest::Ptr<const std::uint8_t>(h.address);
        std::uint32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        if (p[0] != 0xE9 || h.address + 5 + rel != guest::Addr(h.replacement)) {
            detail = Format("hook %s at 0x%08X does not jump to %p", h.name, h.address, h.replacement);
            return false;
        }
        if (h.trampolineBytes == 0) {
            continue;
        }
        ++probes;
        std::uint8_t original[16] = {};
        const auto* t = guest::Ptr<const std::uint8_t>(h.trampoline);
        std::uint32_t back = 0;
        if (h.trampoline == 0 || h.trampolineBytes > sizeof(original) ||
            !ReadOriginalXbeBytes(h.address, original, h.trampolineBytes)) {
            detail = Format("probe %s at 0x%08X has no valid trampoline", h.name, h.address);
            return false;
        }
        std::memcpy(&back, t + h.trampolineBytes + 1, sizeof(back));
        if (std::memcmp(t, original, h.trampolineBytes) != 0 || t[h.trampolineBytes] != 0xE9 ||
            h.trampoline + h.trampolineBytes + 5 + back != h.address + h.trampolineBytes) {
            detail = Format("probe %s at 0x%08X: trampoline 0x%08X does not mirror the original", h.name,
                            h.address, h.trampoline);
            return false;
        }
    }
    detail = Format("%zu hooks verified (%zu probes with trampoline)", hooks.size(), probes);
    return true;
}

// ---- AdoptedThread phase (boot thread, before entry) ------------------------------------------

// Design 1.6 "Boot-Thread": KTHREAD.TlsData = 0, but S_TLS leads to a valid dummy TLS block;
// S_KTHREAD/S_PRCB/S_IRQL/S_DPC as in the table (KPRCB+0x250 = 0).
FUT_SELFTEST(BootThreadSlots, AdoptedThread) {
    const GuestThread* t = CurrentGuestThread();
    if (t == nullptr) {
        detail = "boot thread not adopted";
        return false;
    }
    const std::uint32_t anchor = GetGuestSlot(FUT_TLS_S_TLS);
    const std::uint32_t blockPtrAddr =
        anchor - static_cast<std::uint32_t>(-xbox::kTlsIndex * 4);  // [anchor - 0x24]
    const std::uint32_t block =
        guest::IsReadable(blockPtrAddr, 4) ? guest::Read<std::uint32_t>(blockPtrAddr) : 0;
    const auto* prcb = guest::Ptr<const xbox::FakeKPrcb>(GetGuestSlot(FUT_TLS_S_PRCB));
    detail = Format("KTHREAD 0x%08X TlsData 0x%08X ThreadId %s, anchor 0x%08X -> block 0x%08X, PRCB 0x%08X, "
                    "IRQL %u, DPC %u",
                    GetGuestSlot(FUT_TLS_S_KTHREAD), t->kthread->TlsData,
                    t->kthread->ThreadId == GetCurrentThreadId() ? "= host id" : "WRONG", anchor, block,
                    guest::Addr(prcb), GetGuestSlot(FUT_TLS_S_IRQL), GetGuestSlot(FUT_TLS_S_DPC));
    return GetGuestSlot(FUT_TLS_S_KTHREAD) == guest::Addr(t->kthread) && t->kthread->TlsData == 0 &&
           t->kthread->ThreadId == GetCurrentThreadId() && block != 0 && guest::IsReadable(block, 0x20) &&
           prcb != nullptr && prcb->DebugMonitorData == 0 && GetGuestSlot(FUT_TLS_S_IRQL) == 0 &&
           GetGuestSlot(FUT_TLS_S_DPC) == 0;
}

// Guest GetCurrentThreadId 0xF9AE0 reads [fs:[0x28]+0x12C] through a patched site (cpu.md 2.3).
FUT_SELFTEST(GuestGetCurrentThreadId, AdoptedThread) {
    const auto fn = guest::Func<DWORD(__stdcall*)()>(guest::kXapiGetCurrentThreadId);
    const DWORD id = fn();
    const bool match = id == GetCurrentThreadId();
    detail = match ? "guest id == host id" : Format("guest %lu, host %lu", id, GetCurrentThreadId());
    return match;
}

// Guest SetLastError/GetLastError (0xF6E7C/0xF6E54) go through the patched IRQL and TLS sites and the
// boot thread's dummy TLS block (cpu.md 2.2 Nachtrag). They index the anchor with _tls_index
// (0x2F3D18), which entry sets to -9 only later (0xF852F); the test sets the same value temporarily
// and restores the BSS value afterwards, so entry starts from the original state.
FUT_SELFTEST(GuestLastErrorRoundTrip, AdoptedThread) {
    const auto setLastError = guest::Func<void(__stdcall*)(DWORD)>(guest::kXapiSetLastError);
    const auto getLastError = guest::Func<DWORD(__stdcall*)()>(guest::kXapiGetLastError);
    const std::int32_t savedIndex = guest::Read<std::int32_t>(guest::kTlsIndexVar);
    guest::Write<std::int32_t>(guest::kTlsIndexVar, xbox::kTlsIndex);
    setLastError(0x1234ABCD);
    const DWORD v = getLastError();
    setLastError(0);
    const DWORD cleared = getLastError();
    guest::Write<std::int32_t>(guest::kTlsIndexVar, savedIndex);
    detail =
        Format("GetLastError after SetLastError(0x1234ABCD) = 0x%08lX, after reset 0x%lX (_tls_index %d)", v,
               cleared, xbox::kTlsIndex);
    return v == 0x1234ABCD && cleared == 0;
}

// Design 1.4 step 8 item 4: XAPILIB::IsBadReadPtr(0, 4) (0x1709C9, stdcall 2) returns TRUE.
// Note (disassembly 0x1709DC..0x1709E1): NULL is rejected before any memory access, so this case
// raises no exception; GuestSehIsBadReadPtr below covers the SEH path.
FUT_SELFTEST(GuestIsBadReadPtrNull, AdoptedThread) {
    const auto isBadReadPtr = guest::Func<BOOL(__stdcall*)(const void*, UINT_PTR)>(guest::kXapiIsBadReadPtr);
    const BOOL r = isBadReadPtr(nullptr, 4);
    detail = Format("IsBadReadPtr(0, 4) = %d", r);
    return r == TRUE;
}

// The real guest SEH path (cpu.md 2.5 item 4, design R1): IsBadReadPtr on a PAGE_NOACCESS page faults
// at 0x1709EF inside __SEH_prolog's frame; Windows must dispatch to _except_handler3 0x168DCC in the
// launcher image (polyglot headers, /SAFESEH:NO), which unwinds via RtlUnwind (slot 105) into the
// __except block -> TRUE. The VEH must have recorded the first-chance AV in the ring buffer. A
// readable buffer must give FALSE.
FUT_SELFTEST(GuestSehIsBadReadPtr, AdoptedThread) {
    const auto isBadReadPtr = guest::Func<BOOL(__stdcall*)(const void*, UINT_PTR)>(guest::kXapiIsBadReadPtr);
    void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (page == nullptr) {
        detail = Format("VirtualAlloc(PAGE_NOACCESS) failed (error %lu)", GetLastError());
        return false;
    }
    const std::uint64_t before = GuestExceptionCount();
    const BOOL bad = isBadReadPtr(page, 4);
    const std::uint64_t after = GuestExceptionCount();
    GuestException last{};
    const bool recorded = RecentGuestExceptions(&last, 1) == 1 && last.code == EXCEPTION_ACCESS_VIOLATION &&
                          last.info1 == guest::Addr(page) && guest::InXbeImage(last.eip);
    VirtualFree(page, 0, MEM_RELEASE);
    const std::uint32_t readable[4] = {1, 2, 3, 4};
    const BOOL good = isBadReadPtr(readable, sizeof(readable));
    std::string where;
    try {
        where = guest::Symbolize(last.eip);
    } catch (...) {
    }
    detail = Format(
        "IsBadReadPtr(NOACCESS page, 4) = %d, IsBadReadPtr(stack buffer, 16) = %d, first-chance AV "
        "recorded: %s at %s (%llu new)",
        bad, good, recorded ? "yes" : "no", where.c_str(), static_cast<unsigned long long>(after - before));
    return bad == TRUE && good == FALSE && recorded && after > before;
}

}  // namespace core
