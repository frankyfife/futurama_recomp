// Crash diagnostics (design 1.11). Owned by the core agent.
//  - VEH: records first-chance exceptions with EIP in the XBE range into a per-thread ring buffer,
//    without interfering (IsBadReadPtr and other guest __try blocks rely on guest SEH).
//  - SetUnhandledExceptionFilter, std::set_terminate, CRT invalid-parameter/purecall handlers ->
//    KeBugCheck-style abort through core::Fatal (exit code 3).
//  - core::SetFatalHook: ring buffer dump, EBP backtrace symbolized with guest::Symbolize
//    ("0x0013F5D5 FUN_0013f590+0x45"), guest return-address stack scan, registered reporters,
//    minidump into <userdata>\logs.
//  - Optional MMIO trap reservation 0xFD000000..0xFEFFFFFF ([debug] mmio_trap, default 1).
#pragma once

#include "core/config.h"
#include "shared/win32.h"

#include <cstddef>
#include <cstdint>

namespace core {

// Exception code as the DWORD of EXCEPTION_RECORD::ExceptionCode. With WIN32_NO_STATUS + <ntstatus.h>
// (shared/win32.h) the EXCEPTION_* macros are negative NTSTATUS values; comparing them with a DWORD
// or using them as case labels of a DWORD switch is a sign mismatch / narrowing for clang-cl.
template <class T>
constexpr DWORD ExceptionCode(T code) noexcept {
    return static_cast<DWORD>(code);
}

// HostMain stage (right after config/log).
bool StageInstallCrashHandlers(const Config& config);

// Logs an EBP-chain backtrace starting at `ebp` (guest frames symbolized). Safe on bad frames
// (all stack reads go through ReadProcessMemory).
void LogBacktrace(std::uint32_t ebp, std::uint32_t eip) noexcept;
// Logs stack slots between `esp` and the end of its stack region (at most 16 KiB) that look like
// guest return addresses (value inside the XBE code range, preceded by a call instruction). Useful
// when guest frames omit the frame pointer.
void LogGuestStackScan(std::uint32_t esp) noexcept;
// Logs the first-chance guest exceptions recorded for the calling thread (newest first).
void LogGuestExceptionRing() noexcept;
// Suspends every other thread of the process briefly, captures its registers and logs EIP,
// backtrace and stack scan (hang diagnostics, e.g. the --timeout watchdog).
void LogAllThreads(const char* reason) noexcept;
// Writes <userdata>\logs\futurama_<tag>_<date>_<time>_<pid>.dmp (exception pointers may be null).
// Includes the XBE image 0x10000..0x31B000. Returns false if disabled ([debug] minidump=0) or on error.
bool WriteMiniDump(EXCEPTION_POINTERS* exception, const char* tag) noexcept;

// One recorded first-chance exception of guest code.
struct GuestException {
    std::uint32_t sequence;  // process-wide running number (1, 2, ...)
    std::uint32_t code;      // ExceptionCode
    std::uint32_t eip, esp, ebp;
    std::uint32_t info0, info1;  // AV: access type (0 read, 1 write, 8 DEP) and address
};
// Copies up to `max` entries of the calling thread's ring buffer, newest first; returns the count.
std::size_t RecentGuestExceptions(GuestException* out, std::size_t max) noexcept;
// Number of first-chance guest exceptions seen in the whole process.
std::uint64_t GuestExceptionCount() noexcept;

// Extra crash reporters (e.g. the kernel trace ring buffer of design 5.4): called by the fatal hook
// after the backtrace, before the minidump. Must not throw; may log. At most 8 reporters.
using CrashReporter = void (*)() noexcept;
void AddCrashReporter(const char* name, CrashReporter reporter) noexcept;

}  // namespace core
