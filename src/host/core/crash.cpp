// Crash diagnostics (design 1.11, 5.4). Owned by the core agent.
//
// Evidence: design 1.11 (VEH ring buffer without interfering, KeBugCheck output: code, last ring
// entry, EBP backtrace with symbols from the generated function table, minidump), cpu.md 2.5/6.6
// (IsBadReadPtr and other guest __try blocks raise first-chance exceptions on purpose), cpu.md 4
// (MMIO trap option), startup.md 8 (terminate -> KeBugCheck(0xC0000144)).
#include "core/crash.h"

#include "core/guest.h"
#include "core/log.h"

#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>
#include <vector>

namespace core {
namespace {

// ---- per-thread ring buffer of first-chance guest exceptions ---------------------------------
constexpr std::uint32_t kRingSize = 16;
struct Ring {
    GuestException entries[kRingSize];
    std::uint32_t next;
    std::uint32_t count;
};
thread_local Ring t_ring{};
std::atomic<std::uint32_t> g_sequence{0};
std::atomic<std::uint64_t> g_guestExceptions{0};

// Exception being reported by the unhandled-exception filter (read by the fatal hook).
thread_local EXCEPTION_POINTERS* t_fatalException = nullptr;

bool g_minidumpEnabled = true;
std::wstring* g_dumpDirectory = nullptr;  // leaked on purpose (used until the process dies)
bool g_symbolsInitialized = false;
bool g_dumpImageAdded = false;

struct Reporter {
    const char* name;
    CrashReporter fn;
};
constexpr int kMaxReporters = 8;
Reporter g_reporters[kMaxReporters] = {};
std::atomic<int> g_reporterCount{0};

constexpr std::uint32_t kStackScanBytes = 0x4000;
constexpr int kStackScanMaxHits = 24;
constexpr int kBacktraceMaxFrames = 64;
constexpr std::uint32_t kMaxFrameSize = 0x100000;

bool InGuestImage(std::uint32_t a) noexcept {
    return a >= guest::kXbeBase && a < guest::kXbeSpaceEnd;
}
bool InGuestCode(std::uint32_t a) noexcept {
    return a >= guest::kXbeCodeBegin && a < guest::kXbeCodeEnd;
}
bool InMmioTrap(std::uint32_t a) noexcept {
    return a >= guest::kMmioTrapBase && a - guest::kMmioTrapBase < guest::kMmioTrapSize;
}

// Reads memory that may be invalid without raising an exception.
bool SafeRead(std::uint32_t address, void* out, std::size_t size) noexcept {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), guest::Ptr<const void>(address), out, size, &got) != 0 &&
           got == size;
}

const char* ExceptionName(DWORD code) noexcept {
    switch (code) {
    case ExceptionCode(EXCEPTION_ACCESS_VIOLATION):
        return "access violation";
    case ExceptionCode(EXCEPTION_ILLEGAL_INSTRUCTION):
        return "illegal instruction";
    case ExceptionCode(EXCEPTION_PRIV_INSTRUCTION):
        return "privileged instruction";
    case ExceptionCode(EXCEPTION_INT_DIVIDE_BY_ZERO):
        return "integer divide by zero";
    case ExceptionCode(EXCEPTION_INT_OVERFLOW):
        return "integer overflow";
    case ExceptionCode(EXCEPTION_STACK_OVERFLOW):
        return "stack overflow";
    case ExceptionCode(EXCEPTION_BREAKPOINT):
        return "breakpoint";
    case ExceptionCode(EXCEPTION_SINGLE_STEP):
        return "single step";
    case ExceptionCode(EXCEPTION_IN_PAGE_ERROR):
        return "in-page error";
    case ExceptionCode(EXCEPTION_GUARD_PAGE):
        return "guard page";
    case ExceptionCode(EXCEPTION_FLT_DIVIDE_BY_ZERO):
        return "float divide by zero";
    case ExceptionCode(EXCEPTION_FLT_INVALID_OPERATION):
        return "float invalid operation";
    case ExceptionCode(EXCEPTION_DATATYPE_MISALIGNMENT):
        return "datatype misalignment";
    case ExceptionCode(EXCEPTION_NONCONTINUABLE_EXCEPTION):
        return "noncontinuable exception";
    case 0xE06D7363u:
        return "C++ exception";
    case 0xC0000409u:
        return "stack buffer overrun / fail fast";
    default:
        return "exception";
    }
}

const char* AccessName(std::uint32_t type) noexcept {
    switch (type) {
    case 0:
        return "read";
    case 1:
        return "write";
    case 8:
        return "execute (DEP)";
    default:
        return "access";
    }
}

// "0x0013F5D5 FUN_0013f590+0x45" for guest code, "0x6A1B2C3D futurama_host.dll!krnl::Foo+0x12" or
// "0x6A1B2C3D futurama_host.dll+0x12C3D" for host code.
void DescribeAddress(std::uint32_t address, char* out, std::size_t size) noexcept {
    if (InGuestImage(address)) {
        try {
            std::snprintf(out, size, "%s", guest::Symbolize(address).c_str());
            return;
        } catch (...) {
        }
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           guest::Ptr<const wchar_t>(address), &module) &&
        module != nullptr) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* base = std::wcsrchr(path, L'\\');
        base = base != nullptr ? base + 1 : path;
        if (g_symbolsInitialized) {
            alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = {};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
                std::snprintf(out, size, "0x%08X %ls!%s+0x%llX", address, base, symbol->Name,
                              static_cast<unsigned long long>(displacement));
                return;
            }
        }
        std::snprintf(out, size, "0x%08X %ls+0x%X", address, base,
                      address - static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(module)));
        return;
    }
    std::snprintf(out, size, "0x%08X", address);
}

// True if the bytes in front of `v` end with a call instruction (v is a plausible return address).
bool LooksLikeReturnAddress(std::uint32_t v) noexcept {
    std::uint8_t b[8];  // b[8 - k] = byte at v - k
    if (!SafeRead(v - 8, b, sizeof(b))) {
        return false;
    }
    if (b[3] == 0xE8)
        return true;  // call rel32
    if (b[2] == 0xFF && (b[3] == 0x15 || ((b[3] & 0xF8) == 0x90 && b[3] != 0x94)))
        return true;  // call [disp32] / [r+d32]
    if (b[1] == 0xFF && b[2] == 0x94)
        return true;  // call [sib+d32]
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0x50 && b[6] != 0x54)
        return true;  // call [r+d8]
    if (b[4] == 0xFF && b[5] == 0x54)
        return true;  // call [sib+d8]
    if (b[5] == 0xFF && b[6] == 0x14)
        return true;  // call [sib]
    if (b[6] == 0xFF && ((b[7] & 0xF8) == 0xD0 || ((b[7] & 0xF8) == 0x10 && b[7] != 0x14 && b[7] != 0x15)))
        return true;  // call r / [r]
    return false;
}

void LogContext(const CONTEXT& c) noexcept {
    LogWrite(Channel::Core, Level::Error,
             "  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eflags=%08lX",
             c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp, c.EFlags);
}

// ---- vectored handler: record, never handle ---------------------------------------------------
LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) noexcept {
    if (ep == nullptr || ep->ExceptionRecord == nullptr || ep->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD& r = *ep->ExceptionRecord;
    const CONTEXT& c = *ep->ContextRecord;
    if (!InGuestImage(c.Eip)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    GuestException e{};
    e.sequence = ++g_sequence;
    e.code = r.ExceptionCode;
    e.eip = c.Eip;
    e.esp = c.Esp;
    e.ebp = c.Ebp;
    if (r.NumberParameters >= 2) {
        e.info0 = static_cast<std::uint32_t>(r.ExceptionInformation[0]);
        e.info1 = static_cast<std::uint32_t>(r.ExceptionInformation[1]);
    }
    Ring& ring = t_ring;
    ring.entries[ring.next] = e;
    ring.next = (ring.next + 1) % kRingSize;
    ring.count = ring.count < kRingSize ? ring.count + 1 : kRingSize;
    ++g_guestExceptions;

    const bool mmio = r.ExceptionCode == ExceptionCode(EXCEPTION_ACCESS_VIOLATION) && InMmioTrap(e.info1);
    if (mmio || LogEnabled(Channel::Core, Level::Debug)) {
        char where[192];
        DescribeAddress(e.eip, where, sizeof(where));
        if (mmio) {
            LogWrite(Channel::Core, Level::Error,
                     "MMIO trap: guest %s of 0x%08X (Xbox hardware register) at %s", AccessName(e.info0),
                     e.info1, where);
        } else if (r.ExceptionCode == ExceptionCode(EXCEPTION_ACCESS_VIOLATION)) {
            LogWrite(Channel::Core, Level::Debug,
                     "first-chance guest exception #%u 0x%08lX (%s, %s 0x%08X) at %s", e.sequence,
                     r.ExceptionCode, ExceptionName(r.ExceptionCode), AccessName(e.info0), e.info1, where);
        } else {
            LogWrite(Channel::Core, Level::Debug, "first-chance guest exception #%u 0x%08lX (%s) at %s",
                     e.sequence, r.ExceptionCode, ExceptionName(r.ExceptionCode), where);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- top-level filter and CRT handlers -> core::Fatal -------------------------------------------
LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) noexcept {
    t_fatalException = ep;
    if (ep == nullptr || ep->ExceptionRecord == nullptr || ep->ContextRecord == nullptr) {
        Fatal("unhandled exception (no exception record)");
    }
    const EXCEPTION_RECORD& r = *ep->ExceptionRecord;
    char where[192];
    DescribeAddress(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(r.ExceptionAddress)), where,
                    sizeof(where));
    if (r.ExceptionCode == ExceptionCode(EXCEPTION_ACCESS_VIOLATION) && r.NumberParameters >= 2) {
        const auto type = static_cast<std::uint32_t>(r.ExceptionInformation[0]);
        const auto address = static_cast<std::uint32_t>(r.ExceptionInformation[1]);
        Fatal("unhandled exception 0x%08lX (%s: %s of 0x%08X%s) at %s", r.ExceptionCode,
              ExceptionName(r.ExceptionCode), AccessName(type), address,
              InMmioTrap(address) ? ", Xbox MMIO range" : "", where);
    }
    Fatal("unhandled exception 0x%08lX (%s) at %s", r.ExceptionCode, ExceptionName(r.ExceptionCode), where);
}

[[noreturn]] void TerminateHandler() noexcept {
    // startup.md 8: the guest's terminate ends in KeBugCheck(0xC0000144); the host does the same.
    Fatal("std::terminate (unhandled C++ exception in host code) - KeBugCheck(0xC0000144)-style abort");
}

void __cdecl InvalidParameterHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                                     unsigned int line, std::uintptr_t) noexcept {
    Fatal("CRT invalid parameter: %ls in %ls (%ls:%u)", expression ? expression : L"?",
          function ? function : L"?", file ? file : L"?", line);
}

void __cdecl PurecallHandler() noexcept {
    Fatal("pure virtual function call in host code");
}

void EnsureSymbols() noexcept {
    if (g_symbolsInitialized) {
        return;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
    g_symbolsInitialized = SymInitializeW(GetCurrentProcess(), nullptr, TRUE) != 0;
}

// DbgHelp stack walk: uses the PDB/FPO data of host modules (optimized builds omit frame pointers,
// so the EBP chain alone stops at the first host frame) and falls back to EBP frames for guest code.
void LogStackWalk(const CONTEXT& start) noexcept {
    if (!g_symbolsInitialized) {
        return;
    }
    CONTEXT ctx = start;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp;
    frame.AddrStack.Mode = AddrModeFlat;
    LogWrite(Channel::Core, Level::Error, "backtrace (DbgHelp StackWalk64):");
    char text[256];
    for (int i = 0; i < kBacktraceMaxFrames; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(), &frame, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        DescribeAddress(static_cast<std::uint32_t>(frame.AddrPC.Offset), text, sizeof(text));
        LogWrite(Channel::Core, Level::Error, "  #%-2d %s   (esp 0x%08llX)", i, text,
                 static_cast<unsigned long long>(frame.AddrStack.Offset));
    }
}

void FatalHookImpl(const char* /*message*/) noexcept {
    static std::atomic<int> entered{0};
    if (entered.fetch_add(1) != 0) {
        return;
    }
    EXCEPTION_POINTERS* ep = t_fatalException;
    CONTEXT ctx{};
    if (ep != nullptr && ep->ContextRecord != nullptr) {
        ctx = *ep->ContextRecord;
    } else {
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        RtlCaptureContext(&ctx);
    }
    EnsureSymbols();
    LogWrite(Channel::Core, Level::Error,
             "crash diagnostics: thread %lu, %llu first-chance guest exceptions so far", GetCurrentThreadId(),
             static_cast<unsigned long long>(g_guestExceptions.load()));
    LogContext(ctx);
    LogGuestExceptionRing();
    LogBacktrace(ctx.Ebp, ctx.Eip);
    LogStackWalk(ctx);
    LogGuestStackScan(ctx.Esp);
    const int reporters = std::min(g_reporterCount.load(), kMaxReporters);
    for (int i = 0; i < reporters; ++i) {
        if (g_reporters[i].fn != nullptr) {
            LogWrite(Channel::Core, Level::Error, "crash reporter '%s':", g_reporters[i].name);
            g_reporters[i].fn();
        }
    }
    LogFlush();
    WriteMiniDump(ep, "fatal");
}

BOOL CALLBACK MiniDumpCallback(PVOID, const PMINIDUMP_CALLBACK_INPUT input,
                               PMINIDUMP_CALLBACK_OUTPUT output) {
    if (input == nullptr || output == nullptr) {
        return FALSE;
    }
    switch (input->CallbackType) {
    case MemoryCallback:
        // Add the whole XBE image once (guest code, data and BSS are part of the launcher image).
        if (!g_dumpImageAdded) {
            g_dumpImageAdded = true;
            output->MemoryBase = guest::kXbeBase;
            output->MemorySize = guest::kXbeSpaceEnd - guest::kXbeBase;
            return TRUE;
        }
        return FALSE;
    case CancelCallback:
        output->Cancel = FALSE;
        output->CheckCancel = FALSE;
        return TRUE;
    default:
        return TRUE;
    }
}

}  // namespace

bool StageInstallCrashHandlers(const Config& config) {
    g_minidumpEnabled = config.GetBool("debug", "minidump", true);
    try {
        g_dumpDirectory = new std::wstring(config.LogDirectory().wstring());
    } catch (...) {
        g_dumpDirectory = nullptr;
    }
    if (AddVectoredExceptionHandler(1, &VectoredHandler) == nullptr) {
        FUT_LOGE(Core, "AddVectoredExceptionHandler failed (error %lu)", GetLastError());
        return false;
    }
    SetUnhandledExceptionFilter(&UnhandledFilter);
    std::set_terminate(&TerminateHandler);
    _set_invalid_parameter_handler(&InvalidParameterHandler);
    _set_purecall_handler(&PurecallHandler);
    SetFatalHook(&FatalHookImpl);

    const char* mmio = "off";
    if (config.GetBool("debug", "mmio_trap", true)) {
        void* p = VirtualAlloc(guest::Ptr<void>(guest::kMmioTrapBase), guest::kMmioTrapSize, MEM_RESERVE,
                               PAGE_NOACCESS);
        if (p == guest::Ptr<void>(guest::kMmioTrapBase)) {
            mmio = "0xFD000000..0xFEFFFFFF reserved";
        } else {
            mmio = "unavailable";
            FUT_LOGW(Core, "MMIO trap: cannot reserve 0x%08X+0x%X (error %lu) - continuing without it",
                     guest::kMmioTrapBase, guest::kMmioTrapSize, GetLastError());
        }
    }
    FUT_LOGI(Core,
             "crash handlers: VEH guest ring buffer (%u entries/thread), unhandled filter, terminate, "
             "minidump %s, "
             "MMIO trap %s",
             kRingSize, g_minidumpEnabled ? "on" : "off", mmio);
    return true;
}

void LogBacktrace(std::uint32_t ebp, std::uint32_t eip) noexcept {
    char text[256];
    DescribeAddress(eip, text, sizeof(text));
    LogWrite(Channel::Core, Level::Error, "backtrace (EBP chain):");
    LogWrite(Channel::Core, Level::Error, "  #0  %s", text);
    std::uint32_t frame = ebp;
    for (int i = 1; i <= kBacktraceMaxFrames; ++i) {
        std::uint32_t pair[2] = {};  // [ebp] = caller's ebp, [ebp+4] = return address
        if (frame == 0 || (frame & 3) != 0 || !SafeRead(frame, pair, sizeof(pair))) {
            break;
        }
        if (pair[1] == 0) {
            break;
        }
        DescribeAddress(pair[1], text, sizeof(text));
        LogWrite(Channel::Core, Level::Error, "  #%-2d %s   (frame 0x%08X)", i, text, frame);
        if (pair[0] <= frame || pair[0] - frame > kMaxFrameSize) {
            break;
        }
        frame = pair[0];
    }
}

void LogGuestStackScan(std::uint32_t esp) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    if (esp == 0 || VirtualQuery(guest::Ptr<const void>(esp), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT) {
        return;
    }
    const std::uint64_t regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const std::uint64_t end =
        std::min<std::uint64_t>(regionEnd, static_cast<std::uint64_t>(esp) + kStackScanBytes);
    LogWrite(Channel::Core, Level::Error, "guest return addresses on the stack (scan from esp 0x%08X):", esp);
    int hits = 0;
    char text[256];
    for (std::uint64_t p = esp & ~3u; p + 4 <= end && hits < kStackScanMaxHits; p += 4) {
        std::uint32_t v = 0;
        if (!SafeRead(static_cast<std::uint32_t>(p), &v, sizeof(v))) {
            break;
        }
        if (InGuestCode(v) && LooksLikeReturnAddress(v)) {
            DescribeAddress(v, text, sizeof(text));
            LogWrite(Channel::Core, Level::Error, "  [esp+0x%04X] %s", static_cast<std::uint32_t>(p - esp),
                     text);
            ++hits;
        }
    }
}

void LogGuestExceptionRing() noexcept {
    GuestException entries[kRingSize];
    const std::size_t n = RecentGuestExceptions(entries, kRingSize);
    if (n == 0) {
        LogWrite(Channel::Core, Level::Error, "guest exception ring buffer of thread %lu: empty",
                 GetCurrentThreadId());
        return;
    }
    LogWrite(Channel::Core, Level::Error,
             "guest exception ring buffer of thread %lu (newest first):", GetCurrentThreadId());
    char text[192];
    for (std::size_t i = 0; i < n; ++i) {
        const GuestException& e = entries[i];
        DescribeAddress(e.eip, text, sizeof(text));
        LogWrite(Channel::Core, Level::Error, "  #%u 0x%08X (%s) at %s esp=%08X ebp=%08X info=%08X/%08X",
                 e.sequence, e.code, ExceptionName(e.code), text, e.esp, e.ebp, e.info0, e.info1);
    }
}

void LogAllThreads(const char* reason) noexcept {
    struct Captured {
        DWORD id;
        CONTEXT context;
    };
    std::vector<Captured> captured;
    try {
        const DWORD self = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return;
        }
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL more = Thread32First(snapshot, &te); more; more = Thread32Next(snapshot, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
                continue;
            }
            const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (h == nullptr) {
                continue;
            }
            // Suspend only for the register snapshot; logging happens after resuming (a suspended
            // thread may hold the log or heap lock).
            if (SuspendThread(h) != static_cast<DWORD>(-1)) {
                Captured c{};
                c.id = te.th32ThreadID;
                c.context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                const bool ok = GetThreadContext(h, &c.context) != 0;
                ResumeThread(h);
                if (ok) {
                    captured.push_back(c);
                }
            }
            CloseHandle(h);
        }
        CloseHandle(snapshot);
    } catch (...) {
    }
    EnsureSymbols();
    LogWrite(Channel::Core, Level::Error, "thread dump (%s): %zu other threads", reason ? reason : "-",
             captured.size());
    char text[256];
    for (const Captured& c : captured) {
        DescribeAddress(c.context.Eip, text, sizeof(text));
        LogWrite(Channel::Core, Level::Error, "thread %lu at %s", c.id, text);
        LogContext(c.context);
        LogBacktrace(c.context.Ebp, c.context.Eip);
        LogStackWalk(c.context);
        LogGuestStackScan(c.context.Esp);
    }
}

bool WriteMiniDump(EXCEPTION_POINTERS* exception, const char* tag) noexcept {
    if (!g_minidumpEnabled || g_dumpDirectory == nullptr) {
        return false;
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[160];
    std::swprintf(name, std::size(name), L"\\futurama_%hs_%04u%02u%02u_%02u%02u%02u_%lu.dmp",
                  tag ? tag : "dump", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  GetCurrentProcessId());
    std::wstring path;
    try {
        path = *g_dumpDirectory + name;
    } catch (...) {
        return false;
    }
    CreateDirectoryW(g_dumpDirectory->c_str(), nullptr);
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LogWrite(Channel::Core, Level::Error, "minidump: cannot create %ls (error %lu)", path.c_str(),
                 GetLastError());
        return false;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = exception;
    mei.ClientPointers = FALSE;
    MINIDUMP_CALLBACK_INFORMATION callback{};
    callback.CallbackRoutine = &MiniDumpCallback;
    g_dumpImageAdded = false;
    const auto type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithThreadInfo |
        MiniDumpWithHandleData | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                      exception != nullptr ? &mei : nullptr, nullptr, &callback);
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!ok) {
        LogWrite(Channel::Core, Level::Error, "minidump: MiniDumpWriteDump failed (error 0x%08lX)", error);
        DeleteFileW(path.c_str());
        return false;
    }
    LogWrite(Channel::Core, Level::Error, "minidump written: %ls", path.c_str());
    LogFlush();
    return true;
}

std::size_t RecentGuestExceptions(GuestException* out, std::size_t max) noexcept {
    const Ring& ring = t_ring;
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < ring.count && n < max; ++i) {
        out[n++] = ring.entries[(ring.next + kRingSize - 1 - i) % kRingSize];
    }
    return n;
}

std::uint64_t GuestExceptionCount() noexcept {
    return g_guestExceptions.load();
}

void AddCrashReporter(const char* name, CrashReporter reporter) noexcept {
    const int index = g_reporterCount.fetch_add(1);
    if (index >= kMaxReporters) {
        FUT_LOGW(Core, "crash reporter '%s' ignored (more than %d)", name ? name : "?", kMaxReporters);
        return;
    }
    g_reporters[index] = Reporter{name ? name : "?", reporter};
}

}  // namespace core
