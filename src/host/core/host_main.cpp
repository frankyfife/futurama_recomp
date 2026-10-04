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

#include <atomic>
#include <cstdio>
#include <functional>
#include <string>

extern "C" std::uint32_t __cdecl fut_asm_probe(void);  // asm/asm_probe.asm

namespace core {
namespace {

void ReportStartupError(const std::string& message) noexcept {
    const std::string line = "futurama_host: " + message + "\n";
    OutputDebugStringA(line.c_str());
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    if (err == nullptr || err == INVALID_HANDLE_VALUE ||
        !WriteFile(err, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        MessageBoxA(nullptr, message.c_str(), "Futurama - startup error", MB_OK | MB_ICONERROR);
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
    FUT_LOGI(Core, "exe       %s", WideToUtf8(c.exePath.wstring()).c_str());
    FUT_LOGI(Core, "config    %s",
             c.configFile.empty() ? "(none; defaults)" : WideToUtf8(c.configFile.wstring()).c_str());
    FUT_LOGI(Core, "userdata  %s", WideToUtf8(c.userdata.wstring()).c_str());
    FUT_LOGI(Core, "iso       %s", c.iso.empty() ? "(not set)" : WideToUtf8(c.iso.wstring()).c_str());
    FUT_LOGI(Core, "xbe       %s", c.xbe.empty() ? "(not set)" : WideToUtf8(c.xbe.wstring()).c_str());
    FUT_LOGI(Core, "options   stop_at=%s selftest=%d strict=%d log=%s",
             c.stopAt.empty() ? "-" : c.stopAt.c_str(), c.selftest ? 1 : 0, c.strict ? 1 : 0,
             c.logLevel.c_str());
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

void ExitHostProcess(int exitCode, const char* reason) noexcept {
    static std::atomic<int> exiting{0};
    if (exiting.fetch_add(1) != 0) {
        // Another thread is already ending the process with its exit code; wait for it.
        for (;;) {
            Sleep(INFINITE);
        }
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
                FUT_LOGE(Core, "stage %s failed - exiting with code %d", stage.name, kExitStageFailed);
                LogFlush();
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
