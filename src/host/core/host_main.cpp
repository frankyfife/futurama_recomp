// HostMain (design 1.3 item 3.4, 1.10). Owned by the core agent.
//
// Stage order (design 1.10); each stage is declared in the header of the module that implements it:
//   1  InitConfigAndLog            core/host_main.cpp (here)     config + log + launcher info
//   2  InstallCrashHandlers        core/crash.h                  VEH ring buffer, filter, minidump
//   3  InitKernel                  kernel/kernel_init.h          kernel agent: data exports, HLE state
//   4  LoadXbe                     core/xbe_loader.h             read + SHA-1 + header checks
//   5  MapXbe                      core/xbe_loader.h             RWX, zero, copy 10 sections
//   6  BuildPolyglotHeader         core/xbe_header_polyglot.h    XBE header + MZ/e_lfanew + NT headers
//   7  ResolveKernelThunks         core/kernel_thunks.h          111 slots from the krnl registry
//   8  ApplyFsPatches              core/fs_patcher.h             38 disp32 retargets
//   9  InstallHooks                core/hooks.h                  XAPI hooks (+ later D3D/DSOUND/...)
//  10  RunSelfTests                core/selftest.h               design 1.4 step 8 (with --selftest)
//  11  InitVfs                     vfs/vfs_init.h                io agent: CdRom0, partition1
//  12  CreateWindow                platform/platform.h           (M3)
//  13  AdoptBootThreadAndRunEntry  core/boot.h                   entry 0xF84DD -> guest main thread
//  14  MessageLoop                 platform/platform.h           until the process ends
#include "core/host_main.h"

#include "core/boot.h"
#include "core/config.h"
#include "core/crash.h"
#include "core/fs_patcher.h"
#include "core/guest.h"
#include "core/hooks.h"
#include "core/kernel_thunks.h"
#include "core/log.h"
#include "core/selftest.h"
#include "core/xbe_header_polyglot.h"
#include "core/xbe_loader.h"
#include "kernel/kernel_init.h"
#include "platform/platform.h"
#include "vfs/vfs_init.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>

extern "C" std::uint32_t __cdecl fut_asm_probe(void);  // asm/asm_probe.asm

namespace core {
namespace {

// Reports an error that ends the start outside the log file: stderr if the process has one (console,
// pipe, file), otherwise - or additionally with [debug] msgbox = 1 - a message box. A GUI start
// (SUBSYSTEM:WINDOWS, no console) would otherwise end silently (review finding 5).
void ReportStartupError(const std::string& message, bool forceMessageBox = false) noexcept {
    try {
        const std::string line = "futurama: " + message + "\n";
        OutputDebugStringA(line.c_str());
        const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        DWORD written = 0;
        const bool toStderr = err != nullptr && err != INVALID_HANDLE_VALUE &&
                              GetFileType(err) != FILE_TYPE_UNKNOWN &&
                              WriteFile(err, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        if (!toStderr || forceMessageBox) {
            MessageBoxA(nullptr, message.c_str(), "Futurama - startup error", MB_OK | MB_ICONERROR);
        }
    } catch (...) {
    }
}

// Stage failure: the error the stage logged (LogLastError() before the "stage failed" line) plus the
// log file location.
void ReportStageFailure(const char* stage, int exitCode, bool msgbox, const std::string& cause) noexcept {
    try {
        std::string message =
            std::string("stage ") + stage + " failed (exit code " + std::to_string(exitCode) + ")";
        if (!cause.empty()) {
            message += ": " + cause;
        }
        const std::filesystem::path& log = LogFilePath();
        message += log.empty() ? std::string("\nno log file") : "\nlog file: " + WideToUtf8(log.wstring());
        ReportStartupError(message, msgbox);
    } catch (...) {
    }
}

bool InitConfigAndLog(const LauncherInfo* info) {
    if (info == nullptr || info->size < sizeof(LauncherInfo) || info->version != FUT_LAUNCHER_INFO_VERSION) {
        ReportStartupError("launcher/host version mismatch (LauncherInfo)");
        return false;
    }
    SetLauncherInfo(info);
    LogSetThreadName("boot");

    Config cfg;
    std::string error;
    if (!LoadConfig(info->cmdline,
                    info->exePath ? std::filesystem::path(info->exePath) : std::filesystem::path(), cfg,
                    error)) {
        ReportStartupError("configuration error: " + error);
        return false;
    }
    Level level = Level::Info;
    if (!ParseLevel(cfg.logLevel, level)) {
        ReportStartupError("configuration error: unknown [log] level '" + cfg.logLevel + "'");
        return false;
    }
    LogOptions options;
    options.directory = cfg.LogDirectory();
    options.defaultLevel = level;
    options.console = cfg.logConsole;
    options.debugger = cfg.logDebugger;
    std::string logError;
    const bool logOk = LogInit(options, &logError);
    for (const std::string& t : cfg.trace) {
        Channel ch;
        if (t == "all") {
            LogSetAllLevels(Level::Trace);
        } else if (ParseChannel(t, ch)) {
            LogSetLevel(ch, Level::Trace);
        } else {
            FUT_LOGW(Core, "unknown trace channel '%s' ignored", t.c_str());
        }
    }
    SetFatalMessageBox(cfg.msgbox);
    SetConfig(std::move(cfg));
    const Config& c = GetConfig();

#ifdef NDEBUG
    const char* buildType = "release";
#else
    const char* buildType = "debug";
#endif
    FUT_LOGI(Core, "Futurama PC loader (%s build, %s %s)", buildType, __DATE__, __TIME__);
    if (!logOk) {
        FUT_LOGW(Core, "log file unavailable: %s", logError.c_str());
    }
    for (const std::string& w : c.warnings) {
        FUT_LOGW(Core, "config: %s", w.c_str());
    }
    FUT_LOGI(Core, "exe       %s", WideToUtf8(c.exePath.wstring()).c_str());
    FUT_LOGI(Core, "config    %s",
             c.configFile.empty() ? "(none; defaults)" : WideToUtf8(c.configFile.wstring()).c_str());
    FUT_LOGI(Core, "userdata  %s", WideToUtf8(c.userdata.wstring()).c_str());
    FUT_LOGI(Core, "iso       %s", c.iso.empty() ? "(not set)" : WideToUtf8(c.iso.wstring()).c_str());
    FUT_LOGI(Core, "xbe       %s", c.xbe.empty() ? "(not set)" : WideToUtf8(c.xbe.wstring()).c_str());
    FUT_LOGI(Core, "options   stop_at=%s selftest=%d strict=%d log=%s long_paths=%d",
             c.stopAt.empty() ? "-" : c.stopAt.c_str(), c.selftest ? 1 : 0, c.strict ? 1 : 0,
             c.logLevel.c_str(), ProcessLongPathsEnabled() ? 1 : 0);
    FUT_LOGI(Core,
             "launcher  TLS slots S_TLS=%lu S_KTHREAD=%lu S_PRCB=%lu S_IRQL=%lu S_DPC=%lu, XBE space "
             "0x%08lX+0x%lX",
             info->tlsSlots[FUT_TLS_S_TLS], info->tlsSlots[FUT_TLS_S_KTHREAD], info->tlsSlots[FUT_TLS_S_PRCB],
             info->tlsSlots[FUT_TLS_S_IRQL], info->tlsSlots[FUT_TLS_S_DPC], info->xbeSpace,
             info->xbeSpaceSize);
    return true;
}

struct Stage {
    const char* name;
    std::function<bool()> run;
};

}  // namespace

// Example self-test (also proves that MASM objects are linked into the DLL).
FUT_SELFTEST(AsmObjectLinked, Host) {
    const std::uint32_t v = fut_asm_probe();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "fut_asm_probe() = 0x%08X", v);
    detail = buf;
    return v == 0x46555441u;
}

namespace {

struct ExitReporterEntry {
    const char* name;
    ExitReporter fn;
};
std::mutex g_exitReporterLock;  // constexpr constructor: usable from static initializers
ExitReporterEntry g_exitReporters[kMaxExitReporters] = {};
int g_exitReporterCount = 0;

}  // namespace

void AddExitReporter(const char* name, ExitReporter reporter) noexcept {
    if (reporter == nullptr) {
        return;
    }
    const char* n = name != nullptr ? name : "?";
    bool full = false;
    {
        std::lock_guard<std::mutex> lock(g_exitReporterLock);
        for (int i = 0; i < g_exitReporterCount; ++i) {
            if (std::strcmp(g_exitReporters[i].name, n) == 0) {
                return;  // registered already (e.g. a module that registers on first use)
            }
        }
        if (g_exitReporterCount < kMaxExitReporters) {
            g_exitReporters[g_exitReporterCount++] = ExitReporterEntry{n, reporter};
        } else {
            full = true;
        }
    }
    if (full) {
        FUT_LOGW(Core, "exit reporter '%s' ignored (more than %d)", n, kMaxExitReporters);
    }
}

int ExitReporterCount() noexcept {
    std::lock_guard<std::mutex> lock(g_exitReporterLock);
    return g_exitReporterCount;
}

void RunExitReporters() noexcept {
    ExitReporterEntry copy[kMaxExitReporters];
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(g_exitReporterLock);
        count = g_exitReporterCount;
        std::copy(g_exitReporters, g_exitReporters + count, copy);
    }
    std::sort(copy, copy + count, [](const ExitReporterEntry& a, const ExitReporterEntry& b) {
        return std::strcmp(a.name, b.name) < 0;
    });
    for (int i = 0; i < count; ++i) {
        copy[i].fn();  // noexcept by type: a throwing reporter ends in std::terminate (crash path)
    }
}

void ExitHostProcess(int exitCode, const char* reason) noexcept {
    static std::atomic<int> exiting{0};
    if (exiting.fetch_add(1) != 0) {
        // Another thread is already ending the process with its exit code; wait for it.
        for (;;) {
            Sleep(INFINITE);
        }
    }
    if (exitCode != kExitWatchdog) {
        // Not after a watchdog timeout: a hung guest thread might hold a lock a reporter takes, and the
        // watchdog has already dumped every thread.
        RunExitReporters();
    }
    FUT_LOGI(Core, "process exit: %s (exit code %d; %llu first-chance guest exceptions)",
             reason ? reason : "-", exitCode, static_cast<unsigned long long>(GuestExceptionCount()));
    LogFlush();
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
    ExitProcess(static_cast<UINT>(exitCode));  // not reached
}

int RunHost(const LauncherInfo* info) noexcept {
    try {
        if (!InitConfigAndLog(info)) {
            return kExitStartupError;
        }
        const Config& cfg = GetConfig();
        const Stage stages[] = {
            {"InstallCrashHandlers", [&] { return StageInstallCrashHandlers(cfg); }},
            {"InitKernel", [&] { return krnl::StageInitKernel(cfg); }},
            {"LoadXbe", [&] { return StageLoadXbe(cfg); }},
            {"MapXbe", [] { return StageMapXbe(); }},
            {"BuildPolyglotHeader", [] { return StageBuildPolyglotHeader(); }},
            {"ResolveKernelThunks", [] { return StageResolveKernelThunks(); }},
            {"ApplyFsPatches", [] { return StageApplyFsPatches(); }},
            {"InstallHooks", [&] { return StageInstallHooks(cfg); }},
            {"RunSelfTests", [&] { return StageRunSelfTests(cfg); }},
            {"InitVfs", [&] { return vfs::StageInitVfs(cfg); }},
            {"CreateWindow", [&] { return platform::StageCreateWindow(cfg); }},
            {"AdoptBootThreadAndRunEntry", [&] { return StageAdoptBootThreadAndRunEntry(cfg); }},
        };
        int index = 0;
        for (const Stage& stage : stages) {
            ++index;
            FUT_LOGI(Core, "stage %2d/%zu %s", index, std::size(stages) + 1, stage.name);
            if (!stage.run()) {
                const std::string cause = LogLastError();
                FUT_LOGE(Core, "stage %s failed - exiting with code %d", stage.name, kExitStageFailed);
                LogFlush();
                // The log line above is now the last error; report the cause logged by the stage.
                ReportStageFailure(stage.name, kExitStageFailed, cfg.msgbox, cause);
                return kExitStageFailed;
            }
        }
        FUT_LOGI(Core, "stage %2zu/%zu MessageLoop", std::size(stages) + 1, std::size(stages) + 1);
        const int rc = platform::StageMessageLoop();
        FUT_LOGI(Core, "HostMain returns %d", rc);
        LogFlush();
        return rc;
    } catch (const std::exception& e) {
        Fatal("C++ exception in HostMain: %s", e.what());
    } catch (...) {
        Fatal("unknown C++ exception in HostMain");
    }
}

}  // namespace core

extern "C" int __cdecl HostMain(const LauncherInfo* info) {
    return core::RunHost(info);
}
