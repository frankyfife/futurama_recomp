// Unit tests of the fiber support (fiber agent, M2; design 1.7 "Fibers", E9, R2):
// kernel/mm_kernel_stack.* (MmCreateKernelStack/MmDeleteKernelStack), asm/fiber_switch.asm +
// xapi/xapi_fiber.cpp (SwitchToFiber replacement). They run without the XBE: the guest fiber API is
// mirrored in host code exactly as the XAPI disassembly lays it out (CreateFiber 0x1708B0,
// ConvertThreadToFiber 0x17098E, trampoline 0x17086B), _tls_index is a host variable holding -9.
// Each scenario runs on its own std::thread so the doctest thread's TEB/TLS state stays untouched.
// The SEHOP scenarios start this executable again as a child process with the SEHOP mitigation
// enforced (PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY), which only affects that child.
#include "doctest.h"

#include "core/config.h"
#include "core/guest.h"
#include "core/guest_thread.h"
#include "kernel/kernel_api.h"
#include "kernel/mm_kernel_stack.h"
#include "shared/launcher_info.h"
#include "shared/xbox_types.h"
#include "xapi/xapi_fiber.h"

#include <intrin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr std::uint32_t kChainEnd = 0xFFFFFFFFu;
constexpr unsigned long kTebExceptionList = 0x00;
constexpr unsigned long kTebStackBase = 0x04;
constexpr unsigned long kTebStackLimit = 0x08;
constexpr unsigned long kTebDeallocationStack = 0xE0C;
constexpr const char* kChildEnv = "FUT_FIBER_TEST_CHILD";
constexpr DWORD kUnhandledExitCode = 0x77;

struct Teb {
    std::uint32_t exceptionList, base, limit, dealloc;
    bool operator==(const Teb&) const = default;
};
Teb ReadTeb() noexcept {
    return Teb{static_cast<std::uint32_t>(__readfsdword(kTebExceptionList)),
               static_cast<std::uint32_t>(__readfsdword(kTebStackBase)),
               static_cast<std::uint32_t>(__readfsdword(kTebStackLimit)),
               static_cast<std::uint32_t>(__readfsdword(kTebDeallocationStack))};
}

std::uint32_t Esp() noexcept {
    return guest::Addr(_AddressOfReturnAddress());
}

// Launcher info with real TlsAlloc indices (the tests run without the launcher).
void SetFiberTestLauncherInfo() {
    static LauncherInfo info = [] {
        LauncherInfo li{};
        li.size = sizeof(li);
        li.version = FUT_LAUNCHER_INFO_VERSION;
        for (auto& slot : li.tlsSlots) {
            slot = TlsAlloc();
        }
        li.xbeSpace = FUT_XBE_SPACE_ADDRESS;
        li.xbeSpaceSize = FUT_XBE_SPACE_SIZE;
        li.imageBase = FUT_LAUNCHER_IMAGE_BASE;
        return li;
    }();
    core::SetLauncherInfo(&info);
}

// The calling thread as a guest thread after XapiThreadStartup + ConvertThreadToFiber(param):
// S_TLS anchor = TlsData + 0x24, [TlsData] = TLS block = TlsData + 4, block+0x10 = thread fiber
// {param, KTHREAD.StackBase, 0}, block+0xC = &thread fiber (0x1709A1..0x1709B9).
struct GuestFiberThread {
    std::int32_t tlsIndex = xbox::kTlsIndex;
    guest::GuestAddr previousTlsIndexVar = 0;
    core::GuestThread thread{};
    std::uint32_t block = 0;
    std::uint32_t mainFiber = 0;
    bool ok = false;

    GuestFiberThread() {
        SetFiberTestLauncherInfo();
        if (!core::AdoptCurrentThread(xbox::kTlsDataSize, "fiber-test", &thread)) {
            return;
        }
        previousTlsIndexVar = xapi::detail::SetTlsIndexAddressForTests(guest::Addr(&tlsIndex));
        const std::uint32_t tlsData = guest::Addr(thread.tlsData);
        block = tlsData + 4;
        guest::Write<std::uint32_t>(tlsData, block);
        mainFiber = block + xapi::kTlsBlockThreadFiber;
        guest::Write<std::uint32_t>(mainFiber + xapi::kFiberParameter, 0);
        guest::Write<std::uint32_t>(mainFiber + xapi::kFiberStackBase, thread.kthread->StackBase);
        guest::Write<std::uint32_t>(mainFiber + xapi::kFiberStackLimit, 0);
        guest::Write<std::uint32_t>(block + xapi::kTlsBlockCurrentFiber, mainFiber);
        ok = true;
    }
    ~GuestFiberThread() {
        if (previousTlsIndexVar != 0) {
            xapi::detail::SetTlsIndexAddressForTests(previousTlsIndexVar);
        }
        core::ReleaseCurrentThread();
    }
    GuestFiberThread(const GuestFiberThread&) = delete;
    GuestFiberThread& operator=(const GuestFiberThread&) = delete;

    std::uint32_t CurrentFiber() const {
        return guest::Read<std::uint32_t>(block + xapi::kTlsBlockCurrentFiber);
    }
};

GuestFiberThread* t_env = nullptr;  // set per scenario thread (one scenario at a time)

// Mirror of the fiber trampoline 0x17086B: entered by the first switch's `ret 4` with
// [esp] = [top-0x18] and [esp+4] = start ([top-0x14]); calls start(currentFiber->param).
void __cdecl TestFiberTrampoline(std::uint32_t start) noexcept {
    const std::uint32_t fiber = t_env->CurrentFiber();
    reinterpret_cast<void(__stdcall*)(std::uint32_t)>(start)(
        guest::Read<std::uint32_t>(fiber + xapi::kFiberParameter));
    // The guest trampoline would KeBugCheck(0) here (0x1708A9).
    std::fputs("fiber start routine returned\n", stderr);
    std::abort();
}

// Mirror of XAPI CreateFiber 0x1708B0 (initial frame 0x1708F1..0x170934), with the host trampoline
// in place of 0x17086B. `top` 0 = use MmCreateKernelStack, else a foreign stack top (fallback test).
std::uint32_t TestCreateFiber(std::uint32_t size, void(__stdcall* start)(std::uint32_t), std::uint32_t param,
                              std::uint32_t foreignTop = 0) {
    size = size < 0x3000 ? 0x3000 : size;
    size = (size + 0xFFF) & ~0xFFFu;
    const std::uint32_t top =
        foreignTop != 0 ? foreignTop : guest::Addr(krnl::MmCreateKernelStack(size, FALSE));
    if (top == 0) {
        return 0;
    }
    const std::uint32_t fiber = top - 0x10;
    const std::uint32_t trampoline = guest::Addr(reinterpret_cast<const void*>(&TestFiberTrampoline));
    guest::Write<std::uint32_t>(fiber + xapi::kFiberParameter, param);
    guest::Write<std::uint32_t>(fiber + xapi::kFiberStackBase, top);
    guest::Write<std::uint32_t>(fiber + xapi::kFiberStackLimit, top - size);
    guest::Write<std::uint32_t>(top - 0x14, guest::Addr(reinterpret_cast<const void*>(start)));
    guest::Write<std::uint32_t>(top - 0x18, trampoline);
    guest::Write<std::uint32_t>(top - 0x1C, 0);
    guest::Write<std::uint32_t>(top - 0x20, trampoline);
    for (std::uint32_t off = 0x24; off <= 0x30; off += 4) {
        guest::Write<std::uint32_t>(top - off, 0);
    }
    guest::Write<std::uint32_t>(top - 0x34, kChainEnd);
    guest::Write<std::uint32_t>(fiber + xapi::kFiberSavedEsp, top - 0x34);
    return fiber;
}

// Runs `body` on a fresh thread that is set up as a guest fiber thread.
template <class F>
void OnGuestFiberThread(F body) {
    std::thread worker([&] {
        GuestFiberThread env;
        CHECK(env.ok);
        if (!env.ok) {
            return;
        }
        t_env = &env;
        try {
            body(env);  // a failed REQUIRE throws; it is already recorded
        } catch (...) {
        }
        t_env = nullptr;
    });
    worker.join();
}

// ---- code that runs ON a fiber ----------------------------------------------------------------

struct FiberView {
    std::uint32_t switchBackTo = 0;
    std::uint32_t noAccess = 0;
    std::uint32_t nextFiber = 0;  // nested test: switch here first
    int entries = 0;              // times the fiber body ran (resume counts)
    Teb teb[3]{};
    std::uint32_t esp[3]{};
    std::uint32_t chainLast[3]{}, chainSecondLast[3]{};
    bool chainInRange[3]{};
    bool sehCaught = false;
    bool cppCaught = false;
    bool raiseUnhandled = false;  // child "unhandled": AV without any handler on the fiber
};

// SEH in host code on the fiber (no C++ objects in this function: C2712).
bool HostSehOnCurrentStack(std::uint32_t noAccess) noexcept {
    __try {
        *guest::Ptr<volatile std::uint32_t>(noAccess) = 1;
    } __except (GetExceptionCode() == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION)
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}

void RecordFiberState(FiberView* v, int i) noexcept {
    v->teb[i] = ReadTeb();
    v->esp[i] = Esp();
    v->chainInRange[i] = true;
    std::uint32_t prevEnd = v->teb[i].limit, last = 0, secondLast = 0, n = 0;
    for (std::uint32_t r = v->teb[i].exceptionList; r != kChainEnd && n < 64; ++n) {
        if (r < prevEnd || r >= v->teb[i].base - 8 || (r & 3) != 0) {
            v->chainInRange[i] = false;
            break;
        }
        secondLast = last;
        last = r;
        prevEnd = r + 8;
        r = guest::Read<std::uint32_t>(r);
    }
    v->chainLast[i] = last;
    v->chainSecondLast[i] = secondLast;
}

void __stdcall FiberBody(std::uint32_t param) noexcept {
    auto* v = guest::Ptr<FiberView>(param);
    for (;;) {
        const int i = v->entries < 3 ? v->entries : 2;
        ++v->entries;
        RecordFiberState(v, i);
        if (v->noAccess != 0 && i == 0) {
            v->sehCaught = HostSehOnCurrentStack(v->noAccess);
            try {
                throw std::runtime_error("fiber");
            } catch (const std::exception& e) {
                v->cppCaught = std::strcmp(e.what(), "fiber") == 0;
            }
        }
        if (v->raiseUnhandled) {
            *guest::Ptr<volatile std::uint32_t>(v->noAccess) = 2;  // nobody handles this
        }
        if (v->nextFiber != 0) {
            const std::uint32_t next = v->nextFiber;
            v->nextFiber = 0;
            fut_switch_to_fiber(next);
        } else {
            fut_switch_to_fiber(v->switchBackTo);
        }
    }
}

void FreeFiber(std::uint32_t fiber) {
    krnl::MmDeleteKernelStack(guest::Ptr<void>(guest::Read<std::uint32_t>(fiber + xapi::kFiberStackBase)),
                              guest::Ptr<void>(guest::Read<std::uint32_t>(fiber + xapi::kFiberStackLimit)));
}

// One complete scenario: fresh fiber with SEH + C++ EH, back, resume, back, delete. Returns true if
// everything matched (also used by the SEHOP child process).
bool RunSwitchScenario(GuestFiberThread& env, std::string& why) {
    void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    FiberView view{};
    view.switchBackTo = env.mainFiber;
    view.noAccess = guest::Addr(page);
    const std::uint32_t fiber = TestCreateFiber(0x4000, &FiberBody, guest::Addr(&view));
    krnl::KernelStackInfo stack{};
    if (fiber == 0 || !krnl::FindKernelStack(fiber + 0x10, &stack)) {
        why = "CreateFiber mirror / MmCreateKernelStack failed";
        return false;
    }
    const Teb before = ReadTeb();
    const xapi::FiberStats s0 = xapi::GetFiberStats();
    fut_switch_to_fiber(fiber);
    const Teb afterFirst = ReadTeb();
    const std::uint32_t currentAfterFirst = env.CurrentFiber();
    fut_switch_to_fiber(fiber);  // resume after its switch back
    const Teb afterSecond = ReadTeb();
    const xapi::FiberStats s1 = xapi::GetFiberStats();
    FreeFiber(fiber);
    VirtualFree(page, 0, MEM_RELEASE);

    const Teb expectedOnFiber{0, stack.stackBase, stack.stackLimit, stack.allocationBase};
    for (int i = 0; i < 2; ++i) {
        if (view.teb[i].base != expectedOnFiber.base || view.teb[i].limit != expectedOnFiber.limit ||
            view.teb[i].dealloc != expectedOnFiber.dealloc) {
            why = "TEB stack fields on the fiber differ from the kernel stack bookkeeping";
            return false;
        }
        if (view.esp[i] < stack.stackLimit || view.esp[i] >= stack.stackBase) {
            why = "ESP not on the fiber stack";
            return false;
        }
        if (!view.chainInRange[i] ||
            view.chainLast[i] != stack.stackBase - krnl::kKernelStackTerminalRecord ||
            view.chainSecondLast[i] != stack.exceptionList) {
            why = "SEH chain on the fiber does not end with fiber-top record -> terminal record";
            return false;
        }
    }
    if (view.entries != 2 || !view.sehCaught || !view.cppCaught) {
        why = "fiber body did not run twice / SEH or C++ EH on the fiber failed";
        return false;
    }
    if (!(afterFirst == before) || !(afterSecond == before) || currentAfterFirst != env.mainFiber ||
        env.CurrentFiber() != env.mainFiber) {
        why = "thread TEB values / ExceptionList / current fiber not restored";
        return false;
    }
    if (s1.switches - s0.switches != 4 || s1.chainFixups - s0.chainFixups != 1 ||
        s1.toThreadFiber - s0.toThreadFiber != 2) {
        why = "switch counters wrong";
        return false;
    }
    return true;
}

// Negative control for the SEHOP child: a fiber on a foreign stack keeps ExceptionList -1 (no fixup);
// a handled exception there is fine without SEHOP but kills the process when SEHOP is enforced.
void RunForeignStackException(GuestFiberThread& env) {
    const std::uint32_t size = 0x40000;
    void* mem = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    FiberView view{};
    view.switchBackTo = env.mainFiber;
    view.noAccess = guest::Addr(page);
    const std::uint32_t top = guest::Addr(mem) + size - 0x100;
    const std::uint32_t fiber = TestCreateFiber(size - 0x1000, &FiberBody, guest::Addr(&view), top);
    fut_switch_to_fiber(fiber);
    std::printf("foreign stack: SEH caught %d\n", view.sehCaught ? 1 : 0);
}

DWORD RunChild(const char* mode, bool enforceSehop, bool& started) {
    started = false;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd =
        L"\"" + std::wstring(exe) + L"\" --test-case=\"fiber child process body\" --minimal=true";
    std::string env = std::string(kChildEnv) + "=" + mode;
    _putenv(env.c_str());
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si.StartupInfo);
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::string attrBuf(attrSize, '\0');
    DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_SEHOP_ENABLE;
    DWORD flags = 0;
    if (enforceSehop) {
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize) ||
            !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
                                       &policy, sizeof(policy), nullptr, nullptr)) {
            _putenv((std::string(kChildEnv) + "=").c_str());
            return 0;
        }
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr,
                                        &si.StartupInfo, &pi);
    _putenv((std::string(kChildEnv) + "=").c_str());
    if (enforceSehop) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
    }
    if (!created) {
        return 0;
    }
    started = true;
    DWORD code = 0xFFFFFFFFu;
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 0xDEAD);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

LONG WINAPI ChildTopLevelFilter(EXCEPTION_POINTERS*) {
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), kUnhandledExitCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// MmCreateKernelStack / MmDeleteKernelStack
// ------------------------------------------------------------------------------------------------

TEST_CASE("fiber: MmCreateKernelStack/MmDeleteKernelStack are registered for slots 107/108") {
    CHECK(krnl::IsImplemented(krnl::exp::MmCreateKernelStack::slot));
    CHECK(krnl::IsImplemented(krnl::exp::MmDeleteKernelStack::slot));
    CHECK(krnl::GetBinding(krnl::exp::MmCreateKernelStack::slot).address ==
          reinterpret_cast<std::uintptr_t>(&krnl::MmCreateKernelStack));
    CHECK(krnl::GetBinding(krnl::exp::MmDeleteKernelStack::slot).address ==
          reinterpret_cast<std::uintptr_t>(&krnl::MmDeleteKernelStack));
    CHECK(krnl::ConfiguredFiberStackBytes() == krnl::kDefaultFiberStackBytes);
}

TEST_CASE("fiber: MmCreateKernelStack layout (guard page, committed stack, SEH records, bookkeeping)") {
    const krnl::KernelStackStats s0 = krnl::GetKernelStackStats();
    const std::uint32_t top = guest::Addr(krnl::MmCreateKernelStack(0x4000, FALSE));
    REQUIRE(top != 0);
    krnl::KernelStackInfo info{};
    REQUIRE(krnl::FindKernelStack(top, &info));
    CHECK(info.top == top);
    CHECK(info.stackBase - top == krnl::kKernelStackTopReserve);
    CHECK(info.stackLimit == info.allocationBase + krnl::kKernelStackGuardBytes);
    CHECK(info.stackBase - info.stackLimit >= krnl::kDefaultFiberStackBytes);  // max(0x4000, 512 KiB)
    CHECK(info.requestedBytes == 0x4000u);
    CHECK(static_cast<std::uint64_t>(info.stackBase) <= guest::kGuestMemoryLimit);
    CHECK((top & 0xF) == 0);

    MEMORY_BASIC_INFORMATION mbi{};
    REQUIRE(VirtualQuery(guest::Ptr<void>(info.allocationBase), &mbi, sizeof(mbi)) != 0);
    CHECK(mbi.State == MEM_COMMIT);
    CHECK(mbi.Protect == (PAGE_READWRITE | PAGE_GUARD));
    CHECK(mbi.RegionSize == krnl::kKernelStackGuardBytes);
    REQUIRE(VirtualQuery(guest::Ptr<void>(info.stackLimit), &mbi, sizeof(mbi)) != 0);
    CHECK(mbi.State == MEM_COMMIT);
    CHECK(mbi.Protect == PAGE_READWRITE);
    CHECK(guest::Addr(mbi.BaseAddress) + mbi.RegionSize == info.stackBase);  // fully committed

    // SEH records above top: fiber-top -> terminal -> -1, 8 zero bytes at the very top (SEHOP bound).
    const std::uint32_t terminal = info.stackBase - krnl::kKernelStackTerminalRecord;
    CHECK(info.exceptionList == info.stackBase - krnl::kKernelStackFiberTopRecord);
    CHECK(guest::Read<std::uint32_t>(info.exceptionList) == terminal);
    CHECK(guest::Read<std::uint32_t>(info.exceptionList + 4) != 0);
    CHECK(guest::Read<std::uint32_t>(terminal) == kChainEnd);
    CHECK(guest::Read<std::uint32_t>(terminal + 4) == krnl::KernelStackTerminalHandler());
    CHECK(guest::Read<std::uint64_t>(info.stackBase - 8) == 0u);
    HMODULE handlerModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       guest::Ptr<const wchar_t>(krnl::KernelStackTerminalHandler()), &handlerModule);
    CHECK((handlerModule == GetModuleHandleW(L"ntdll.dll")) == krnl::KernelStackTerminalHandlerIsNtdll());
    const std::string handlerKind =
        krnl::KernelStackTerminalHandlerIsNtdll() ? "ntdll final handler" : "host (SEHOP disabled)";
    MESSAGE("terminal SEH handler: " << handlerKind);

    krnl::KernelStackInfo containing{};
    CHECK(krnl::FindKernelStackContaining(top - 0x100, &containing));
    CHECK(containing.top == top);
    CHECK_FALSE(krnl::FindKernelStackContaining(info.allocationBase, nullptr));  // guard page

    const krnl::KernelStackStats s1 = krnl::GetKernelStackStats();
    CHECK(s1.created == s0.created + 1);
    CHECK(s1.live == s0.live + 1);
    CHECK(s1.peakLive >= s1.live);

    krnl::MmDeleteKernelStack(guest::Ptr<void>(top), guest::Ptr<void>(top - 0x4000));
    CHECK_FALSE(krnl::FindKernelStack(top, nullptr));
    REQUIRE(VirtualQuery(guest::Ptr<void>(info.stackLimit), &mbi, sizeof(mbi)) != 0);
    CHECK(mbi.State == MEM_FREE);
    const krnl::KernelStackStats s2 = krnl::GetKernelStackStats();
    CHECK(s2.deleted == s0.deleted + 1);
    CHECK(s2.live == s0.live);
}

TEST_CASE("fiber: MmCreateKernelStack sizes and failures") {
    const krnl::KernelStackStats s0 = krnl::GetKernelStackStats();
    // larger than the configured minimum: the request wins (page-rounded)
    const std::uint32_t big = guest::Addr(krnl::MmCreateKernelStack(0x100800, FALSE));
    REQUIRE(big != 0);
    krnl::KernelStackInfo info{};
    REQUIRE(krnl::FindKernelStack(big, &info));
    CHECK(info.stackBase - info.stackLimit == 0x101000u);
    krnl::MmDeleteKernelStack(guest::Ptr<void>(big), guest::Ptr<void>(big - 0x100800));
    // too large -> NULL (CreateFiber then fails with ERROR_NOT_ENOUGH_MEMORY, 0x1708E6)
    CHECK(krnl::MmCreateKernelStack(krnl::kMaxFiberStackBytes + 0x1000, FALSE) == nullptr);
    // unknown stack -> refused, nothing freed
    std::uint32_t local = 0;
    krnl::MmDeleteKernelStack(&local, &local);
    const krnl::KernelStackStats s1 = krnl::GetKernelStackStats();
    CHECK(s1.failed == s0.failed + 2);
    CHECK(s1.live == s0.live);
}

// ------------------------------------------------------------------------------------------------
// SwitchToFiber replacement
// ------------------------------------------------------------------------------------------------

TEST_CASE("fiber: switch into a fresh fiber, SEH and C++ EH on the fiber stack, resume, delete") {
    OnGuestFiberThread([](GuestFiberThread& env) {
        std::string why;
        const bool ok = RunSwitchScenario(env, why);
        CHECK_MESSAGE(ok, why);
    });
}

TEST_CASE("fiber: switch to the current fiber is a no-op") {
    OnGuestFiberThread([](GuestFiberThread& env) {
        const Teb before = ReadTeb();
        const std::uint64_t n0 = xapi::FiberSwitchCount();
        fut_switch_to_fiber(env.mainFiber);
        CHECK(ReadTeb() == before);
        CHECK(env.CurrentFiber() == env.mainFiber);
        CHECK(xapi::FiberSwitchCount() == n0 + 1);
    });
}

TEST_CASE("fiber: fiber A switches to fiber B, B back to the thread fiber, A and B resume") {
    OnGuestFiberThread([](GuestFiberThread& env) {
        FiberView a{}, b{};
        a.switchBackTo = env.mainFiber;
        b.switchBackTo = env.mainFiber;
        const std::uint32_t fa = TestCreateFiber(0x3000, &FiberBody, guest::Addr(&a));
        const std::uint32_t fb = TestCreateFiber(0x3000, &FiberBody, guest::Addr(&b));
        REQUIRE(fa != 0);
        REQUIRE(fb != 0);
        krnl::KernelStackInfo sa{}, sb{};
        REQUIRE(krnl::FindKernelStack(fa + 0x10, &sa));
        REQUIRE(krnl::FindKernelStack(fb + 0x10, &sb));
        const Teb before = ReadTeb();
        a.nextFiber = fb;
        fut_switch_to_fiber(fa);  // main -> A -> B -> main
        CHECK(ReadTeb() == before);
        CHECK(a.entries == 1);
        CHECK(b.entries == 1);
        CHECK(a.teb[0].base == sa.stackBase);
        CHECK(b.teb[0].base == sb.stackBase);
        CHECK(b.teb[0].limit == sb.stackLimit);
        CHECK(b.teb[0].dealloc == sb.allocationBase);
        CHECK(b.chainLast[0] == sb.stackBase - krnl::kKernelStackTerminalRecord);
        fut_switch_to_fiber(fa);  // resume A inside its switch to B; A then switches to main
        fut_switch_to_fiber(fb);  // resume B inside its switch to main; B switches to main again
        CHECK(ReadTeb() == before);
        CHECK(a.entries == 2);
        CHECK(b.entries == 2);
        CHECK(a.teb[1].base == sa.stackBase);
        CHECK(b.teb[1].base == sb.stackBase);
        FreeFiber(fa);
        FreeFiber(fb);
    });
}

TEST_CASE("fiber: a stack that is not from MmCreateKernelStack uses the fiber's own limits") {
    OnGuestFiberThread([](GuestFiberThread& env) {
        const std::uint32_t size = 0x20000;
        void* mem = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        REQUIRE(mem != nullptr);
        const std::uint32_t top = guest::Addr(mem) + size - 0x40;
        FiberView v{};
        v.switchBackTo = env.mainFiber;
        const std::uint32_t fiber = TestCreateFiber(0x10000, &FiberBody, guest::Addr(&v), top);
        const xapi::FiberStats s0 = xapi::GetFiberStats();
        fut_switch_to_fiber(fiber);
        const xapi::FiberStats s1 = xapi::GetFiberStats();
        CHECK(v.entries == 1);
        CHECK(v.teb[0].base == top);
        CHECK(v.teb[0].limit == top - 0x10000);
        // No ExceptionList fixup: the chain on the fiber ends in -1 below the fiber's own frames.
        CHECK(v.chainInRange[0]);
        CHECK(v.chainLast[0] < top);
        CHECK(s1.unknownStacks == s0.unknownStacks + 1);
        CHECK(s1.chainFixups == s0.chainFixups);
        VirtualFree(mem, 0, MEM_RELEASE);
    });
}

// ------------------------------------------------------------------------------------------------
// SEHOP (design R2): child processes with the SEHOP mitigation enforced
// ------------------------------------------------------------------------------------------------

// Body of the child processes started below; does nothing in a normal test run.
TEST_CASE("fiber child process body") {
    const char* mode = std::getenv(kChildEnv);
    if (mode == nullptr || *mode == 0) {
        return;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::string m = mode;
    OnGuestFiberThread([&](GuestFiberThread& env) {
        if (m == "positive") {
            std::string why;
            const bool ok = RunSwitchScenario(env, why);
            CHECK_MESSAGE(ok, why);
        } else if (m == "negative") {
            RunForeignStackException(env);
        } else if (m == "unhandled") {
            SetUnhandledExceptionFilter(&ChildTopLevelFilter);
            void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
            FiberView v{};
            v.switchBackTo = env.mainFiber;
            v.noAccess = guest::Addr(page);
            v.raiseUnhandled = true;
            const std::uint32_t fiber = TestCreateFiber(0x4000, &FiberBody, guest::Addr(&v));
            fut_switch_to_fiber(
                fiber);  // does not come back: fiber-top record -> top-level filter -> exit 0x77
            FAIL("unhandled exception on the fiber did not reach the top-level filter");
        }
    });
}

TEST_CASE("fiber: SEH on fiber stacks works with SEHOP enforced (child processes)") {
    bool started = false;
    // Negative control: proves that the child really enforces SEHOP (a chain ending in -1 on a switched
    // stack kills the process with the access violation instead of reaching the __except).
    const DWORD negative = RunChild("negative", true, started);
    if (!started) {
        MESSAGE("could not start a child with the SEHOP mitigation policy - skipped");
        return;
    }
    CHECK(negative == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
    // Without enforcement (Windows 11 default: SEHOP audit only) the same code is handled.
    CHECK(RunChild("negative", false, started) == 0u);
    // The real fibers: fiber-top record -> terminal record from RtlInitializeExceptionChain.
    CHECK(RunChild("positive", true, started) == 0u);
    CHECK(RunChild("positive", false, started) == 0u);
    // An exception no frame handles reaches the process's top-level filter through the fiber-top record.
    CHECK(RunChild("unhandled", true, started) == kUnhandledExitCode);
    CHECK(RunChild("unhandled", false, started) == kUnhandledExitCode);
}
