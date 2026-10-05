// Tracing (design 5.4): the kernel trace ring buffer (--trace=krnl) and the VFS trace file
// (--trace=vfs). Owned by the io agent (M2).
//
// Kernel trace (--trace=krnl, also "all"): every log line of the KRNL channel - with --trace=krnl the
// channel runs at level trace, so this is every HLE call that logs its decoded arguments (paths,
// handles, NTSTATUS names) - is copied into a fixed ring buffer of the last kKrnlRingEntries lines.
// The ring is fed through core::SetLogTap (log.h) and dumped by a crash reporter
// (core::AddCrashReporter "kernel trace ring") when the process dies in core::Fatal / KeBugCheck /
// an unhandled exception, so the calls leading to a crash are in the crash report even when the
// log level of the run hides them.
//
// VFS trace (--trace=vfs, also "all"): <userdata>\logs\vfs_trace.log gets one line per open,
// attribute query and symbolic link of the guest (kernel/io_file.cpp, io_info.cpp,
// vfs/vfs_namespace.cpp): guest path -> device path : result. The result is "hit", "missing
// (documented: ...)", "missing UNDOCUMENTED" or "error" (M2 exit criterion 7: every opened path is a
// hit or a documented miss). The file is rewritten on every start and written line by line without
// buffering, so it is complete after a crash or TerminateProcess.
//
// Usage:
//   core::trace::Init(config);                       // vfs::StageInitVfs (HostMain stage 11); idempotent
//   if (core::trace::VfsEnabled()) core::trace::VfsLine("NtCreateFile \"%s\" -> %s : hit", ...);
//   core::trace::DumpKrnlRing();                     // crash reporter (also callable for diagnostics)
// All functions are noexcept and thread-safe.
#pragma once

#include "core/config.h"

#include <sal.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace core::trace {

// Number of lines kept by the kernel trace ring and the maximum stored length of one line.
inline constexpr std::size_t kKrnlRingEntries = 1024;
inline constexpr std::size_t kKrnlRingLineBytes = 240;
inline constexpr wchar_t kVfsTraceFileName[] = L"vfs_trace.log";

struct Options {
    bool krnl = false;                // ring buffer + crash reporter
    bool vfs = false;                 // vfs_trace.log
    std::filesystem::path directory;  // directory of vfs_trace.log (normally <userdata>\logs)
};

// Options from [debug] trace / --trace= ("krnl", "vfs", "all"; other channel names are ignored here).
Options OptionsFromConfig(const Config& config);
// Applies `options`: opens (truncates) <directory>\vfs_trace.log, installs the log tap and registers
// the crash reporter (once per process). Calling it again reconfigures (unit tests); disabling closes
// the file and removes the tap. Returns false if the VFS trace file cannot be created (logged; the
// run continues without it).
bool Configure(const Options& options) noexcept;
// Configure(OptionsFromConfig(config)) - called by vfs::StageInitVfs.
bool Init(const Config& config) noexcept;

namespace detail {
extern std::atomic<bool> g_krnlEnabled;
extern std::atomic<bool> g_vfsEnabled;
}  // namespace detail
inline bool KrnlEnabled() noexcept {
    return detail::g_krnlEnabled.load(std::memory_order_relaxed);
}
inline bool VfsEnabled() noexcept {
    return detail::g_vfsEnabled.load(std::memory_order_relaxed);
}

// ---- kernel trace ring --------------------------------------------------------------------------
// Appends one line (normally called by the log tap; a trailing newline is stripped, longer lines are
// truncated to kKrnlRingLineBytes - 1). No-op while the kernel trace is disabled.
void RecordKrnl(const char* line, std::size_t size) noexcept;
// Lines currently in the ring, oldest first.
std::vector<std::string> KrnlRingSnapshot();
// Total number of lines recorded since the last Configure (including overwritten ones).
std::uint64_t KrnlRecordCount() noexcept;
// Logs the ring (oldest first) on KRNL/error. The crash reporter; safe inside the fatal hook.
void DumpKrnlRing() noexcept;

// ---- VFS trace file -----------------------------------------------------------------------------
// Writes "[<seconds since process start>] <thread id> <text>\n" to vfs_trace.log. No-op while the
// VFS trace is disabled.
void VfsLine(_Printf_format_string_ const char* fmt, ...) noexcept;
// Path of the open vfs_trace.log (empty if disabled) and the number of lines written to it.
std::filesystem::path VfsTracePath();
std::uint64_t VfsLineCount() noexcept;

}  // namespace core::trace
