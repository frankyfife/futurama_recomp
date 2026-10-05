// M2 self-tests of the fiber agent (design 6 M2 exit criterion 6, risk R2; m1_report 8 "Host-Frames
// zwischen Gast-Frames" and 9 items 4 and 10). Phase AdoptedThread: they run on the adopted boot
// thread right before entry, after the hooks (SwitchToFiber replacement) are installed.
//
//  GuestSehOnFiberStack       a guest fiber from the original XAPI CreateFiber 0x1708B0 (stack from
//                             the HLE MmCreateKernelStack) runs guest IsBadReadPtr 0x1709C9 on a
//                             PAGE_NOACCESS page; the access violation must be dispatched to the
//                             guest __except (Windows SEH on the fiber stack) and return TRUE.
//  GuestSehThroughHostFrame   a guest exception unwinds through a host noexcept C++ frame that sits
//                             between guest frames (the situation of every probe / host->guest call).
#include "core/crash.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/guest_thread.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/selftest.h"
#include "kernel/mm_kernel_stack.h"
#include "shared/xbox_types.h"
#include "xapi/xapi_fiber.h"

#include <intrin.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace core {
namespace {

std::string Format(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

guest::GuestAddr CodeOf(const void* p) noexcept {
    return guest::Addr(p);
}

constexpr std::uint32_t kChainEnd = 0xFFFFFFFFu;
// 32-bit TEB offsets: NT_TIB.ExceptionList, StackBase, StackLimit; TEB.DeallocationStack.
constexpr unsigned long kTebExceptionList = 0x00;
constexpr unsigned long kTebStackBase = 0x04;
constexpr unsigned long kTebStackLimit = 0x08;
constexpr unsigned long kTebDeallocationStack = 0xE0C;
// Size of the guest _strchr 0x166FC0 (functions.tsv: 195 bytes) for the fault-address check.
constexpr std::uint32_t kCrtStrchrBytes = 195;

struct TebStack {
    std::uint32_t base, limit, dealloc, exceptionList;
};
TebStack ReadTebStack() noexcept {
    return TebStack{static_cast<std::uint32_t>(__readfsdword(kTebStackBase)),
                    static_cast<std::uint32_t>(__readfsdword(kTebStackLimit)),
                    static_cast<std::uint32_t>(__readfsdword(kTebDeallocationStack)),
                    static_cast<std::uint32_t>(__readfsdword(kTebExceptionList))};
}

// =============================================================================================
// GuestSehOnFiberStack
// =============================================================================================

struct FiberProbe {
    // in
    std::uint32_t mainFiber;
    std::uint32_t noAccessPage;
    // out (written on the fiber)
    bool ran;
    TebStack teb;
    std::uint32_t esp;
    std::uint32_t currentFiber;  // [TLS block + 0xC] seen on the fiber
    std::uint32_t chainRecords;  // registration records from fs:[0] to -1
    bool chainInRange;           // every record inside [StackLimit, StackBase - 8), ascending
    std::uint32_t lastRecord, lastHandler, secondLastRecord;
    BOOL badNoAccess, badReadable;
    std::uint64_t guestExceptions;  // first-chance guest exceptions during IsBadReadPtr
    GuestException lastException;
    bool cppCaught;  // host C++ throw/catch on the fiber stack (HLE code from fibers)
};

// Fiber start routine, called by the guest trampoline 0x17086B as start(param) inside its
// __try/__except(UnhandledExceptionFilter). Never returns: it switches back to the main fiber and is
// then abandoned and deleted (returning would end in KeBugCheck(0) at 0x1708A9).
void __stdcall SelfTestFiberProc(FiberProbe* p) noexcept {
    p->ran = true;
    p->teb = ReadTebStack();
    p->esp = guest::Addr(_AddressOfReturnAddress());
    const std::uint32_t anchor = GetGuestSlot(FUT_TLS_S_TLS);
    const std::uint32_t block =
        guest::Read<std::uint32_t>(anchor - static_cast<std::uint32_t>(-xbox::kTlsIndex * 4));
    p->currentFiber = guest::Read<std::uint32_t>(block + xapi::kTlsBlockCurrentFiber);

    // The SEH chain as Windows' dispatcher sees it (RtlDispatchException / RtlpIsValidExceptionChain).
    p->chainInRange = true;
    std::uint32_t previousEnd = p->teb.limit;
    for (std::uint32_t r = p->teb.exceptionList; r != kChainEnd && p->chainRecords < 64; ++p->chainRecords) {
        if (r < previousEnd || r >= p->teb.base - 8 || (r & 3) != 0) {
            p->chainInRange = false;
            break;
        }
        p->secondLastRecord = p->lastRecord;
        p->lastRecord = r;
        p->lastHandler = guest::Read<std::uint32_t>(r + 4);
        previousEnd = r + 8;
        r = guest::Read<std::uint32_t>(r);
    }

    const auto isBadReadPtr = guest::Func<BOOL(__stdcall*)(const void*, UINT_PTR)>(guest::kXapiIsBadReadPtr);
    const std::uint64_t before = GuestExceptionCount();
    p->badNoAccess = isBadReadPtr(guest::Ptr<const void>(p->noAccessPage), 4);
    const std::uint32_t readable[2] = {1, 2};
    p->badReadable = isBadReadPtr(readable, sizeof(readable));
    p->guestExceptions = GuestExceptionCount() - before;
    RecentGuestExceptions(&p->lastException, 1);

    try {
        throw 42;
    } catch (int v) {
        p->cppCaught = v == 42;
    }

    guest::Func<void(__stdcall*)(std::uint32_t)>(guest::kXapiSwitchToFiber)(p->mainFiber);
    Fatal("self-test fiber was resumed after switching back to the main fiber");
}

}  // namespace

// M2 exit criterion 6 / R2 (design 1.7, E9): see the file comment. Also checks the TEB values on the
// fiber (stack bookkeeping of MmCreateKernelStack), the SEHOP-compatible chain end, the restore of the
// thread's real TEB values on the way back, DeleteFiber -> MmDeleteKernelStack and the switch counter.
// Guest state touched: _tls_index (set to -9 like entry does at 0xF852F, restored afterwards) and the
// boot thread's dummy TLS block (+0x0C current fiber, +0x10..+0x1B thread fiber; restored).
FUT_SELFTEST(GuestSehOnFiberStack, AdoptedThread) {
    if (!IsHookInstalled(guest::kXapiSwitchToFiber)) {
        detail =
            "XAPILIB::SwitchToFiber hook is not installed ([hooks] disable?) - SEH on fibers cannot work";
        return false;
    }
    const std::int32_t savedIndex = guest::Read<std::int32_t>(guest::kTlsIndexVar);
    guest::Write<std::int32_t>(guest::kTlsIndexVar, xbox::kTlsIndex);
    const std::uint32_t anchor = GetGuestSlot(FUT_TLS_S_TLS);
    const std::uint32_t block =
        guest::Read<std::uint32_t>(anchor - static_cast<std::uint32_t>(-xbox::kTlsIndex * 4));
    std::uint32_t savedBlock[4] = {};  // +0x0C .. +0x1B
    std::memcpy(savedBlock, guest::Ptr<const void>(block + xapi::kTlsBlockCurrentFiber), sizeof(savedBlock));

    void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    FiberProbe probe{};
    probe.noAccessPage = guest::Addr(page);

    const auto convert =
        guest::Func<std::uint32_t(__stdcall*)(std::uint32_t)>(guest::kXapiConvertThreadToFiber);
    const auto create = guest::Func<std::uint32_t(__stdcall*)(std::uint32_t, std::uint32_t, std::uint32_t)>(
        guest::kXapiCreateFiber);
    const auto switchTo = guest::Func<void(__stdcall*)(std::uint32_t)>(guest::kXapiSwitchToFiber);
    const auto remove = guest::Func<void(__stdcall*)(std::uint32_t)>(guest::kXapiDeleteFiber);

    const krnl::KernelStackStats stacksBefore = krnl::GetKernelStackStats();
    const xapi::FiberStats fibersBefore = xapi::GetFiberStats();
    probe.mainFiber = convert(0);
    const std::uint32_t fiber =
        page != nullptr
            ? create(0x4000, CodeOf(reinterpret_cast<const void*>(&SelfTestFiberProc)), guest::Addr(&probe))
            : 0;
    krnl::KernelStackInfo stack{};
    const bool known = fiber != 0 && krnl::FindKernelStack(
                                         guest::Read<std::uint32_t>(fiber + xapi::kFiberStackBase), &stack);
    const TebStack before = ReadTebStack();
    if (known) {
        switchTo(fiber);  // returns when SelfTestFiberProc switches back
    }
    const TebStack after = ReadTebStack();
    const std::uint32_t currentAfter = guest::Read<std::uint32_t>(block + xapi::kTlsBlockCurrentFiber);
    const xapi::FiberStats fibersAfter = xapi::GetFiberStats();
    if (fiber != 0) {
        remove(fiber);
    }
    const krnl::KernelStackStats stacksAfter = krnl::GetKernelStackStats();
    const bool freed = known && !krnl::FindKernelStack(stack.top, nullptr);

    std::memcpy(guest::Ptr<void>(block + xapi::kTlsBlockCurrentFiber), savedBlock, sizeof(savedBlock));
    guest::Write<std::int32_t>(guest::kTlsIndexVar, savedIndex);
    if (page != nullptr) {
        VirtualFree(page, 0, MEM_RELEASE);
    }

    const bool tebOnFiber = probe.teb.base == stack.stackBase && probe.teb.limit == stack.stackLimit &&
                            probe.teb.dealloc == stack.allocationBase && probe.esp >= stack.stackLimit &&
                            probe.esp < stack.stackBase;
    const bool chainOk = probe.chainInRange &&
                         probe.lastRecord == stack.stackBase - krnl::kKernelStackTerminalRecord &&
                         probe.secondLastRecord == stack.exceptionList &&
                         probe.lastHandler == krnl::KernelStackTerminalHandler();
    const bool restored = after.base == before.base && after.limit == before.limit &&
                          after.dealloc == before.dealloc && after.exceptionList == before.exceptionList &&
                          currentAfter == probe.mainFiber;
    const bool sehOk =
        probe.badNoAccess == TRUE && probe.badReadable == FALSE && probe.guestExceptions >= 1 &&
        probe.lastException.code == ExceptionCode(EXCEPTION_ACCESS_VIOLATION) &&
        probe.lastException.info1 == probe.noAccessPage && guest::InXbeImage(probe.lastException.eip) &&
        probe.lastException.esp >= stack.stackLimit && probe.lastException.esp < stack.stackBase;
    const std::uint64_t switches = fibersAfter.switches - fibersBefore.switches;
    // Both switches count as self-test switches (core::InSelfTest), not as the game's (M2 exit 5).
    const bool countersOk =
        switches == 2 && fibersAfter.selfTestSwitches - fibersBefore.selfTestSwitches == 2 &&
        fibersAfter.chainFixups - fibersBefore.chainFixups == 1 &&
        stacksAfter.created == stacksBefore.created + 1 && stacksAfter.deleted == stacksBefore.deleted + 1 &&
        stacksAfter.live == stacksBefore.live;
    std::string where;
    try {
        where = guest::Symbolize(probe.lastException.eip);
    } catch (...) {
    }
    detail =
        Format("fiber 0x%08X on stack 0x%08X..0x%08X (top 0x%08X): IsBadReadPtr(NOACCESS) = %d, "
               "IsBadReadPtr(readable) = "
               "%d, %llu guest AV at %s (esp 0x%08X); TEB on fiber %s (StackBase 0x%08X StackLimit 0x%08X "
               "DeallocationStack 0x%08X); SEH chain %u records, last 0x%08X handler 0x%08X (%s) %s; C++ EH "
               "on fiber %s; "
               "back on thread fiber: TEB %s, current fiber %s; %llu switches, stack freed %s",
               fiber, stack.stackLimit, stack.stackBase, stack.top, probe.badNoAccess, probe.badReadable,
               static_cast<unsigned long long>(probe.guestExceptions), where.c_str(), probe.lastException.esp,
               tebOnFiber ? "ok" : "WRONG", probe.teb.base, probe.teb.limit, probe.teb.dealloc,
               probe.chainRecords, probe.lastRecord, probe.lastHandler,
               krnl::KernelStackTerminalHandlerIsNtdll() ? "ntdll final" : "host", chainOk ? "ok" : "WRONG",
               probe.cppCaught ? "ok" : "FAILED", restored ? "restored" : "NOT restored",
               currentAfter == probe.mainFiber ? "= main" : "WRONG",
               static_cast<unsigned long long>(switches), freed ? "yes" : "NO");
    return known && probe.ran && probe.currentFiber == fiber && tebOnFiber && chainOk && sehOk &&
           probe.cppCaught && restored && countersOk && freed;
}

namespace {

// =============================================================================================
// GuestSehThroughHostFrame
// =============================================================================================
//
// Call chain (innermost last):
//   fut_seh3_frame_call      guest SEH3 frame: __SEH_prolog 0x167DC8, _except_handler3 0x168DCC,
//                            __try/__except(EXCEPTION_EXECUTE_HANDLER)            [guest __except]
//   HostWrapper              host noexcept C++ frame with a destructor (C++ EH record on fs:[0])
//   ??_L 0x166E00            guest eh vector constructor iterator, __try/__finally [guest __finally]
//   HostElementCtor          host thiscall callback, 2nd element calls ...
//   _strchr 0x166FC0         guest code, faults on a PAGE_NOACCESS string        [fault]
// Windows walks the chain from the fault: the host record (MSVC /EHsc __CxxFrameHandler3 ignores
// non-C++ exceptions), the guest __finally level, then the guest __except filter -> 1. The guest's
// _except_handler3 then runs __global_unwind2 -> RtlUnwind (slot 105) through the host frame and the
// guest frame (whose __finally calls __ArrayUnwind -> HostElementDtor for the one element built so
// far) and enters the __except block. Expected: caught, finally ran once, neither host function
// continued after the fault, fs:[0] restored - and the host frame's destructor did NOT run (/EHsc:
// structured exceptions unwind C++ frames without destructors). That last point is the rule for
// hooks: see the Changelog entry in src/INTERFACES.md and the fiber agent's report.

struct UnwindProbe {
    std::uint32_t noAccessPage;
    std::uint32_t constructed;
    std::uint32_t destroyed;
    std::uint32_t wrapperDestructors;
    std::uint32_t wrapperReturned;
    std::uint32_t ctorContinued;
};
UnwindProbe* g_unwindProbe = nullptr;

struct Element {
    std::uint32_t value;
};

// thiscall without stack arguments == fastcall with ecx = this (edx unused), callee purges nothing.
void __fastcall HostElementCtor(Element* self, void*) noexcept {
    UnwindProbe* p = g_unwindProbe;
    if (p->constructed == 1) {
        guest::Func<const char*(__cdecl*)(const char*, int)>(guest::kCrtStrchr)(
            guest::Ptr<const char>(p->noAccessPage), 'x');
        ++p->ctorContinued;
    }
    self->value = 0xC0FFEEu;
    ++p->constructed;
}

void __fastcall HostElementDtor(Element* self, void*) noexcept {
    self->value = 0;
    ++g_unwindProbe->destroyed;
}

struct WrapperScope {
    WrapperScope() = default;
    WrapperScope(const WrapperScope&) = delete;
    WrapperScope& operator=(const WrapperScope&) = delete;
    ~WrapperScope() { ++g_unwindProbe->wrapperDestructors; }
};

std::uint32_t __cdecl HostWrapper(std::uint32_t elements) noexcept {
    WrapperScope scope;
    using EhVectorCtor = void(__stdcall*)(void*, std::uint32_t, int, std::uint32_t, std::uint32_t);
    guest::Func<EhVectorCtor>(guest::kCrtEhVectorCtorIterator)(
        guest::Ptr<void>(elements), sizeof(Element), 3,
        CodeOf(reinterpret_cast<const void*>(&HostElementCtor)),
        CodeOf(reinterpret_cast<const void*>(&HostElementDtor)));
    ++g_unwindProbe->wrapperReturned;
    return 0;
}

}  // namespace

FUT_SELFTEST(GuestSehThroughHostFrame, AdoptedThread) {
    void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (page == nullptr) {
        detail = Format("VirtualAlloc(PAGE_NOACCESS) failed (error %lu)", GetLastError());
        return false;
    }
    UnwindProbe probe{};
    probe.noAccessPage = guest::Addr(page);
    g_unwindProbe = &probe;
    Element elements[3] = {};
    const std::uint32_t exceptionListBefore = static_cast<std::uint32_t>(__readfsdword(kTebExceptionList));
    const std::uint64_t exceptionsBefore = GuestExceptionCount();
    const std::uint32_t caught =
        fut_seh3_frame_call(guest::kCrtSehProlog, guest::kCrtSehEpilog,
                            CodeOf(reinterpret_cast<const void*>(&HostWrapper)), guest::Addr(elements));
    const std::uint32_t exceptionListAfter = static_cast<std::uint32_t>(__readfsdword(kTebExceptionList));
    const std::uint64_t exceptions = GuestExceptionCount() - exceptionsBefore;
    GuestException last{};
    RecentGuestExceptions(&last, 1);
    g_unwindProbe = nullptr;
    VirtualFree(page, 0, MEM_RELEASE);

    // Control run without a fault: fn returns normally through the same frames.
    UnwindProbe control{};
    control.noAccessPage = 0;
    g_unwindProbe = &control;
    control.constructed = 2;  // skip the faulting branch
    Element controlElements[3] = {};
    const std::uint32_t controlCaught = fut_seh3_frame_call(
        guest::kCrtSehProlog, guest::kCrtSehEpilog, CodeOf(reinterpret_cast<const void*>(&HostWrapper)),
        guest::Addr(controlElements));
    g_unwindProbe = nullptr;

    std::string where;
    try {
        where = guest::Symbolize(last.eip);
    } catch (...) {
    }
    const bool faultInGuest = exceptions >= 1 && last.code == ExceptionCode(EXCEPTION_ACCESS_VIOLATION) &&
                              last.info1 == probe.noAccessPage && last.eip >= guest::kCrtStrchr &&
                              last.eip < guest::kCrtStrchr + kCrtStrchrBytes;
    detail = Format(
        "fault %s at %s; outer guest __except %s; guest __finally destroyed %u of %u built element(s); host "
        "wrapper destructor ran %u time(s) (expected 0: /EHsc unwinds C++ frames without destructors on "
        "SEH), "
        "host code after the fault ran %u time(s); fs:[0] %s; control run without fault: __except %u, "
        "wrapper "
        "destructor %u, %u elements",
        faultInGuest ? "recorded" : "NOT recorded", where.c_str(),
        caught == 1 ? "caught it" : "did NOT catch it", probe.destroyed, probe.constructed,
        probe.wrapperDestructors, probe.wrapperReturned + probe.ctorContinued,
        exceptionListAfter == exceptionListBefore ? "restored" : "WRONG", controlCaught,
        control.wrapperDestructors, control.constructed - 2);
    return caught == 1 && faultInGuest && probe.constructed == 1 && probe.destroyed == 1 &&
           probe.wrapperReturned == 0 && probe.ctorContinued == 0 && probe.wrapperDestructors == 0 &&
           exceptionListAfter == exceptionListBefore && controlCaught == 0 &&
           control.wrapperDestructors == 1 && control.constructed == 5 && control.destroyed == 0 &&
           elements[0].value == 0 && controlElements[2].value == 0xC0FFEEu;
}

}  // namespace core
