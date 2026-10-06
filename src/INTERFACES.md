# Loader interfaces (phase 2 contract)

Binding design: `docs/loader_design.md` (decisions E1-E12). This file lists every API the module
agents use, who owns which file, and how to build and run. Code and comments are English; design
documents are German.

## 1. Build and run

| What | How |
|---|---|
| Build | `cmd /c "D:\FuturamaDecomp\build.cmd" [builddir] [Debug\|RelWithDebInfo] [target\|all] [msvc\|clang]` (always the FULL path; cmd AutoRun changes the working directory). Default `build\x86-debug Debug`, all targets. Every agent uses **its own** build dir, e.g. `build\core`, `build\kernel`, `build\io`. |
| Targets | `fut_launcher` (futurama.exe), `fut_host` (futurama_host.dll), `fut_game` (static), `fut_tests` (only if `third_party\doctest\doctest.h` exists), `fut_gen` (checks generated files, needs Python). |
| Output | `<builddir>\bin\futurama.exe`, `futurama_host.dll`, PDBs. |
| Presets | `CMakePresets.json`: `x86-debug`, `x86-relwithdebinfo` (Ninja + cl), `x86-clang-debug` (clang-cl 32-bit + ml.exe + link.exe, second build for warnings, design 5.2); all need a vcvars32 environment. `build.cmd <dir> Debug all clang` does the same as the clang preset. Extra cache options via the environment variable `FUT_CMAKE_ARGS` (e.g. `-DFUT_WERROR=ON`). |
| Tests | `ctest` from the build root: `fut_tests`; with `-DFUT_TEST_CONFIG=<test INI>` also `fut_selftest` (`futurama.exe --config=... --selftest --stop-at=main`) and `fut_m1_check` (`tools\m1_check.py`, 10 runs). |
| New source files | Just add them. `src\host\<module>\**\*.cpp/.h/.inc/.asm` (modules core kernel vfs xapi platform asm gen gfx audio video input), `src\game\**`, `tests\*.cpp` are collected with `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)`. No CMake edit needed. |
| Warnings | Host C++: `/std:c++20 /W4 /permissive- /utf-8 /EHsc`, `shared\win32.h` is force-included. Your files must build **without warnings** (`-DFUT_WERROR=ON` turns them into errors). |

Run (use a per-agent user data dir under `D:\FuturamaDecomp\run\<name>\`, never `%LOCALAPPDATA%` in tests):

```ini
; D:\FuturamaDecomp\run\<name>\futurama.ini  - paths with spaces/commas/parentheses go here
[paths]
iso      = Y:\xbox games\Futurama (Jun 17, 2003 prototype)\Futurama (Europe) (En,Fr,De,Es,It).iso
xbe      = D:\FuturamaDecomp\orig\retail_eu\default.xbe
userdata = D:\FuturamaDecomp\run\<name>
[debug]
stop_at  = main
selftest = 1
msgbox   = 0
[kernel]
strict   = 0
[log]
level    = info
```

```sh
# Bash (waits, inherits stderr -> log lines appear on the console):
/d/FuturamaDecomp/build/<dir>/bin/futurama.exe --config=D:/FuturamaDecomp/run/<name>/futurama.ini; echo $?
# PowerShell (futurama.exe is a GUI-subsystem exe, so wait explicitly):
$p = Start-Process D:\FuturamaDecomp\build\<dir>\bin\futurama.exe -ArgumentList '--config=D:\FuturamaDecomp\run\<name>\futurama.ini' -Wait -PassThru -NoNewWindow; $p.ExitCode
```

Log file: `<userdata>\logs\futurama.log` (previous runs: `futurama.1.log`, `futurama.2.log`).
Command line options: `--config= --userdata= --iso= --xbe= --stop-at=<main|0xADDR> --selftest --trace=<krnl,vfs,...|all> --strict --log-level=<error|warn|info|debug|trace> --set=<section>.<key>=<value>`.
M3c tool modes (section 4.21; no game start, no XBE mapping): `--print-config-schema=<file.json>`,
`--check-iso=<image> --result=<file.json>`, `--query-gpu=<file.json>`, `--print-paths=<file.json>`. Without
`--config` the INI is `%LOCALAPPDATA%\FuturamaPC\futurama.ini` (portable mode with `portable.txt` next to the exe:
`<exe dir>\futurama.ini`), no longer `<exe dir>\futurama.ini`.

Exit codes: `0` ok / stop-at reached, `1` startup/config error, `2` a HostMain stage failed,
`3` fatal error (`core::Fatal`, KeBugCheck-style), `10..14` launcher errors (`shared\launcher_info.h`).
Added in phase 2 (core): `4` the guest ended itself (`XLaunchNewImageA` hook, `HalReturnToFirmware`, all
guest threads exited), `5` `[debug] timeout` watchdog (thread dump + minidump in the log directory).
Added in M3c (review finding 15): `6` another game process already runs with the same user data folder (named mutex
per user data folder, taken before the log is opened; stderr line, nothing written).
Core debug keys: `[debug] timeout = <s>` (0 = off), `minidump = 1`, `mmio_trap = 1` (reserve 0xFD000000..0xFEFFFFFF),
`crash_test = 1` (provokes an unhandled guest access violation before entry to check the crash path, exit 3).
M1 boot path verified (core run 2026-10-04, `--stop-at=main --selftest`): all self-tests pass, entry ->
PsCreateSystemThreadEx -> XapiThreadStartup -> 0xF8469 -> XapiInitProcess -> _rtinit -> _cinit -> main hook, exit 0.

**M1 exit check (phase 3 integration)**: `python tools\m1_check.py --exe build\x86-debug\bin\futurama.exe
--config run\integ\futurama.ini --runs 10 --xbe orig\retail_eu\default.xbe [--fresh]` runs the loader N times
(the INI must set `userdata`; the script adds `--selftest --stop-at=main` and fails on a missing log or a different userdata), checks the six M1 exit criteria of design
section 6 per run (self-tests, boot probes, partition1 files, pool + cluster size, main hook + exit 0, log
without error/warn/UNIMPLEMENTED/TRAP) and compares the logs after masking timestamps and 8-digit addresses.
Logs of every run: `<userdata>\m1_check\`. Integration configs: `run\integ` (Debug), `run\integ-rel`
(RelWithDebInfo), `run\integ-main` (no stop-at; before M2 it ended in the MMIO trap of NiXBoxRenderer::Create).

**M2 exit check (M2 phase 2 integration)**: `python tools\m2_check.py --exe build\x86-debug\bin\futurama.exe
--config run\m2\futurama.ini --runs 3 [--fresh]` runs the loader N times past main (the INI must set `userdata`
and must not set `stop_at`; the script adds `--selftest`, and `--frames=3000` / `--trace=vfs` unless the INI sets
`[debug] frames` / `trace`). Per run it checks the seven M2 exit criteria of design section 6 from the log and
`vfs_trace.log`: (1) `archive: mounted sysPAL.img as #0` .. `FutPAL04.img as #5`, `FutPAL05.img not mounted`
+ a documented miss in the trace, `game: video standard CApp+0xE8 = 0 (PAL`; (2) `XGetLanguage() = N` and
`game: language "..."` match `[settings] language` (default english; the game string is compared
case-insensitively - for english the game keeps its default `"English"`); (3) `game: BUILD.TXT = "1.25"` + the
archive lookup; (4) first context Intro, `FRAMES: limit of N frames reached`, exit 0 before the timeout, and from
1000 frames on also Intro -> next context (shorter runs only warn; M3b: from 20 s x frame rate on, i.e. 1200
frames at the default 60 Hz); (5) `fiber summary: N switches (G game, S
self-test` with G > 0 and `fiber: first SwitchToFiber (game)` (the self-test's own switches do not count), `I/O
APC stats` with XBE completions > 0, 0 error completions and 0 `failed APC reads` (failed writes = save layer,
reported only), disc error handler registered + trapped and `disc error screen calls 0`; (6) `SELFTEST PASS
GuestSehOnFiberStack`; (7) every `vfs_trace.log` line a hit, archive hit, documented miss, symlink or mount line
(no UNDOCUMENTED / error; lines ending in `[self-test]` are counted apart), every non-archived asset followed by
its loose open; plus no error/warn/UNIMPLEMENTED/TRAP line. On a failed run the script prints the exe's own
stderr line (not the console copy of the log) or the first error/warn line of the log. 3000 frames at the
default 50 Hz take about 60 s per run (`[debug] timeout` must be larger, e.g. 150). **M3b:** the default is 60 Hz
(`[game] tick_rate`, `[render] frame_rate` follows it), so 3000 frames take about 50 s; the script reads the rate
from `[game] tick_rate` / `[render] frame_rate` (timeout frames / rate + 90 s) and adds the timing criteria
`m2_check.check_timing` (T1: patch table of the tick rate, read-back `game timing: effective after Game::Init:
... - ok` with fps/dt of the rate, CApp+0xE8 = 0, tag "PAL", only `*-PAL.xmv` opened; T2: `game ticks:` steady
rate within 0.2 % of the tick rate and, with the lockstep frame clock and frames at the tick rate, >= 99 % of the
frames with exactly one fixed step).
Logs: `<userdata>\m2_check\run_NN.log`, `run_NN_vfs_trace.log`. Configs: `run\m2` (german, Debug), `run\m2-rel` /
`run\m2-clang` (same settings for the RelWithDebInfo / clang-cl builds), `run\m2-fr` (french, 1000 frames), `run\m2-long` (30000 frames, stability), `run\m2-ctest` (ctest `fut_m2_check`, cache variable
`FUT_M2_TEST_CONFIG`, 3 runs with `--fresh`); after the review fixes `run\m2fix\<de|rel|clang|en|en-explicit|fr|
frames100|long|ctest|ctest-en|exit-hang>` (docs/m2_report.md). Cache variable `FUT_M2_EXTRA_TEST_CONFIGS` (list)
registers one-run variants `fut_m2_check_<directory name>` (e.g. `run\m2fix\ctest-en`: no language key = english).

Skeleton state (end of phase 1): every stage is an empty stub; a run logs all stages, lists the 110
kernel slots that are still generated stubs and stops at `AdoptBootThreadAndRunEntry` with exit code 2.

## 2. File ownership (binding for phase 2)

Edit only files you own. In **shared** files you may only ADD declarations (no renames, no signature
or behaviour changes of existing APIs) and must log each change in the Changelog (section 8).

| Owner | Files |
|---|---|
| **core agent** | `src\launcher\**`, `src\host\core\**` except `log.*`, `config.*`, `guest.h`, `guest.cpp` (those are shared; core may extend them under the shared rule), `src\host\xapi\**`, `src\host\asm\**`, `src\host\platform\**`, the consumer code of `src\host\gen\fs_patch_table.gen.inc` (`core\fs_patcher.*`) |
| **kernel agent** | `src\host\kernel\**` except `kernel\io_*.*`, `kernel\ob_handles.*`, `kernel\kernel_api.*`, `kernel\gen\**` |
| **io agent** | `src\host\vfs\**` except `vfs\vfs.h`, and `src\host\kernel\io_*.*` |
| **shared** (skeleton-owned, add-only + Changelog) | all `CMakeLists.txt`, `CMakePresets.json`, `build.cmd`, `src\shared\**`, `src\host\kernel\kernel_api.h` + `kernel_api.cpp`, `src\host\kernel\ob_handles.*`, `src\host\kernel\gen\**` and `src\host\gen\**` (generated - change the generator in `tools\gen\` and regenerate, never hand-edit), `tools\gen\**`, `src\host\core\log.*`, `src\host\core\config.*`, `src\host\core\guest.h` + `guest.cpp`, `src\host\vfs\vfs.h`, `src\host\futurama_host.def` |
| anyone (new files only) | `tests\*.cpp`, `third_party\` (e.g. `third_party\doctest\doctest.h`) |

**M3 (graphics, phase 2)**: the ownership table of `src\host\gfx\GFX_CONTRACT.md` section 10 applies on top
of this table - backend agent `gfx\rhi\d3d11\**` + `platform\**` (moved from the core agent), texconv agent
`gfx\texconv.*`, extract agent `gfx\ni_extract.*` + `gfx\ni_guest_layouts.h`, shader agent
`gfx\ff_pipeline.*` + `gfx\shaders\**` + the `fut_add_shader` lines in `src\host\CMakeLists.txt`; shared
(add-only + Changelog): `gfx\rhi\rhi.h`, `gfx\rhi\rhi.cpp`, `gfx\gfx_math.h`, `gfx\gfx_config.*`,
`gfx\GFX_CONTRACT.md`; the integration (phase 3) owns `gfx\nipc_renderer.*`.

Notes: `vfs\vfs_stubs.cpp` (skeleton stubs of `vfs.h`) belongs to the io agent, who replaces or deletes
it. `kernel\kernel_init.*` belongs to the kernel agent. `core\guest_thread.*`, `core\hooks.*`,
`core\selftest.*`, `core\host_main.*` belong to the core agent (skeleton versions already work).

## 3. HostMain stages (design 1.10)

`HostMain` (exported from `futurama_host.def`, `__cdecl`) -> `core::RunHost` in `core\host_main.cpp`
runs these stages in order; a stage returning `false` ends the process with exit code 2.

| # | Stage function | Declared in | Implemented by | Content |
|---|---|---|---|---|
| 1 | `InitConfigAndLog` (internal) | `core\host_main.cpp` | done (skeleton) | LauncherInfo check, config, log, banner |
| 2 | `core::StageInstallCrashHandlers(cfg)` | `core\crash.h` | core | VEH ring buffer, unhandled filter, terminate handler, `core::SetFatalHook` (backtrace + minidump) |
| 3 | `krnl::StageInitKernel(cfg)` | `kernel\kernel_init.h` | kernel | data exports from config, HLE state |
| 4 | `core::StageLoadXbe(cfg)` | `core\xbe_loader.h` | core | read `[paths] xbe` or `vfs::ReadIsoFile(iso, "default.xbe")`, SHA-1, header checks |
| 5 | `core::StageMapXbe()` | `core\xbe_loader.h` | core | RWX, zero 0x11000..0x31B000, copy 10 sections |
| 6 | `core::StageBuildPolyglotHeader()` | `core\xbe_header_polyglot.h` | core | design 1.4 step 5 |
| 7 | `core::StageResolveKernelThunks()` | `core\kernel_thunks.h` | core | 111 slots via `krnl::GetBinding`, `krnl::LogBindingSummary()` |
| 8 | `core::StageApplyFsPatches()` | `core\fs_patcher.h` | core | 38 sites from `guest::FsPatchSites()` |
| 8b | `xapi::StageApplyGameTiming(cfg)` | `xapi\game_timing.h` | timing (M3b) | `[game] tick_rate`: 60 Hz = 5 operand patches in Game::Init 0xA26A0 + 2 `.data` initial values (section 4.19); 50 = nothing; reads `[game] frame_clock` for `probe:Game::Timer`. Shifts the log numbers `stage N/14` of the later stages by one |
| 9 | `core::StageInstallHooks(cfg)` | `core\hooks.h` | core (works) | installs all `FUT_HOOK`s |
| 10 | `core::StageRunSelfTests(cfg)` | `core\selftest.h` | core (runner works) | phases Host + Mapped |
| 11 | `vfs::StageInitVfs(cfg)` | `vfs\vfs_init.h` | io | `\Device\CdRom0`, `\Device\Harddisk0\partition1` |
| 12 | `platform::StageCreateWindow(cfg)` | `platform\platform.h` | core | no-op until M3 |
| 13 | `core::StageAdoptBootThreadAndRunEntry(cfg)` | `core\boot.h` | core | `AdoptCurrentThread(0,"boot")`, AdoptedThread self-tests, `--stop-at` hook, call entry 0xF84DD (MASM thunk) |
| 14 | `platform::StageMessageLoop()` -> exit code | `platform\platform.h` | core | wait / message loop until the guest ends the process |

## 4. Shared APIs

### 4.1 Launcher handover - `src\shared\launcher_info.h`, `core\config.h`

- `LauncherInfo { size, version, tlsSlots[5], xbeSpace (0x11000), xbeSpaceSize (0x30A000), imageBase, cmdline, exePath, launcherModule }`.
- TLS slot ids `FUT_TLS_S_TLS, FUT_TLS_S_KTHREAD, FUT_TLS_S_PRCB, FUT_TLS_S_IRQL, FUT_TLS_S_DPC` (enum `FutTlsSlot`). The launcher allocated them before any `LoadLibrary`; each index is < 64.
- `core::GetLauncherInfo()`, `core::TlsSlotIndex(FutTlsSlot)` -> TlsAlloc index; the guest reads the slot as `fs:[0xE10 + 4*index]` after the fs patches.

### 4.2 Logging - `core\log.h`

```cpp
FUT_LOGE(Krnl, "..."); FUT_LOGW(Vfs, ...); FUT_LOGI(Core, ...); FUT_LOGD(Hook, ...); FUT_LOGT(Xapi, ...);
core::LogEnabled(core::Channel::Krnl, core::Level::Trace)   // cheap check before expensive formatting
core::LogSetThreadName("guest-main");                       // thread column of the log
core::Fatal("XBE SHA-1 mismatch: %s", sha);                 // [[noreturn]]: log, fatal hook, exit code 3
NTSTATUS __stdcall NtFoo(...) noexcept { FUT_HLE_TRY { ... } FUT_HLE_CATCH("NtFoo") }   // catch-all
```

Channels `Core Krnl Vfs Xapi Gfx Snd Vid Inp Hook Game`; levels `Error Warn Info Debug Trace`.
`core::LogSeconds()` / `core::LogThreadName()` (M2 review) return the time and thread columns of the log; files
that must line up with `futurama.log` (`vfs_trace.log`) use them.
`core::SetFatalHook(fn)` (crash module) runs inside `Fatal`. printf format strings are checked by MSVC
(`_Printf_format_string_`): use `%lu`/`%lX` for `ULONG`/`DWORD`/`NTSTATUS` casts, `%u`/`%X` for `uint32_t`.

### 4.3 Configuration - `core\config.h`

`core::GetConfig()` returns the process configuration (`iso`, `xbe`, `userdata`, `stopAt`, `selftest`,
`trace`, `strict`, `msgbox`, `logLevel`, `hooksDisable`, plus `LogDirectory()`, `Partition1Directory()`
= `<userdata>\hdd\partition1`, `CacheDirectory()`). Module-specific keys need no header change:
`cfg.GetString("settings", "language", "english")`, `GetBool`, `GetU32` (dec/0x), `GetList`, `GetPath`
(relative to the INI). Any key can be overridden with `--set=section.key=value`.
Phase 5: inline comments (`;`/`#` after a blank, outside quotes; same rule as Python configparser) are
allowed. Every key must be listed in `kKnownKeys` (`core\config.cpp`) - a new module key is added there;
unknown keys in a known section and invalid booleans/numbers are configuration errors (exit code 1),
unknown sections only a warning (`Config::warnings`, logged after the log opens). A userdata path longer
than 200 characters is rejected when the process has no Win32 long path support
(`core::ProcessLongPathsEnabled()`).
M2 review: choice keys (`KeyType::Choice`, `core::ConfigKeyChoices(section, key)`) are validated with the
configuration too, so an invalid `[settings] language` (names of `kernel\kernel_settings.cpp` or 1..9),
`[saves] key_mode` (fixed|xbox) or `[render] d3d_traps` (abort|log) ends with exit code 1 instead of a stage
failure (exit code 2). `[debug] exit_hang_test = 1` registers an exit reporter that never returns (test of the
watchdog during the exit, section 5).
`core::WideToUtf8` / `core::Utf8ToWide` convert strings.
M3c (section 4.21): `kKnownKeys` is replaced by the schema table `core\config_schema.cpp` - a new module key is a
row there (type, default, de/en texts, UI hints); `core::ConfigKeyDefault(section, key)` returns the default,
`core::ParseConfig*` are the shared value parsers. Default INI location `%LOCALAPPDATA%\FuturamaPC\futurama.ini`,
portable mode, `core::ResolveConfigLocation`.

### 4.4 Guest addresses and memory - `core\guest.h`

- Named constants for every M1 address (`guest::kEntryPoint`, `kMainThreadRoutine`, `kXapiThreadStartup`,
  `kXapiInitProcess`, `kGameMain`, `kKernelThunkTable`, `kTlsIndexVar`, `kXapiQueryPerformanceCounter`,
  `kXapiIsBadReadPtr`, `kSectionHeaders`, `kSections[]`, polyglot offsets, ...). Never write a bare guest
  address in code; add a constant with its evidence (shared rule).
- `guest::Ptr<T>(addr)`, `guest::Addr(ptr)`, `guest::Read<T>(addr)`, `guest::Write(addr, v)`,
  `guest::Func<Fn>(addr)` (typed pointer to a guest function), `guest::InXbeImage`, `guest::IsReadable`,
  `guest::WriteBytes` (VirtualProtect + FlushInstructionCache), `guest::ReadAnsiString`.
- Generated tables: `guest::Functions()`, `guest::FindFunctionContaining(addr)`,
  `guest::FindFunctionStart(addr)`, `guest::Symbolize(addr)` -> `"0x0013F5D5 FUN_0013f590+0x45"`,
  `guest::FsPatchSites()` (38 x `guest::FsPatchSite { address, length, original[8], dispOffset,
  originalDisp, tlsSlot, function, library }`).
- `FUT_GLOBAL(type, name, addr)` for fixed-address guest globals (design 3.4).

### 4.5 Guest threads - `core\guest_thread.h` (core-owned, used by the kernel agent)

```cpp
// in the new thread created by PsCreateSystemThreadEx, before calling SystemRoutine:
core::GuestThread t;
if (!core::AdoptCurrentThread(TlsDataSize /*0x24*/, "guest-main", &t)) ...;
// boot thread (core): core::AdoptCurrentThread(0, "boot");   // KTHREAD.TlsData = 0, dummy TLS anchor
core::GetGuestSlot(FUT_TLS_S_IRQL); core::SetGuestSlot(FUT_TLS_S_IRQL, newIrql);   // IRQL emulation
core::CurrentGuestThread(); core::SharedFakePrcb(); core::ReleaseCurrentThread();
```

The fake structures are `xbox::FakeKThread` (0x140: +0x1C StackBase, +0x20 StackLimit, +0x28 TlsData,
+0x12C ThreadId) and `xbox::FakeKPrcb` (+0x250 = 0) in `shared\xbox_types.h`.

### 4.6 Kernel export registration - `kernel\kernel_api.h`

Each implementation registers itself next to its definition (namespace scope):

```cpp
namespace krnl {
// NtCreateFile - slot 25, Ord 190, stdcall, 9 args (purge 36).
// Evidence: kernel_imports.tsv; kernel.md 4 (9 pushes at 0x0F6B09); files.md 2.5; design 4.3.
NTSTATUS __stdcall NtCreateFile(HANDLE* FileHandle, xbox::ACCESS_MASK DesiredAccess,
                                xbox::OBJECT_ATTRIBUTES* ObjectAttributes, xbox::IO_STATUS_BLOCK* IoStatusBlock,
                                xbox::LARGE_INTEGER* AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
                                ULONG CreateDisposition, ULONG CreateOptions) noexcept;
KRNL_FUNC(NtCreateFile, &NtCreateFile);

// KfLowerIrql - slot 0, Ord 161, fastcall, NewIrql in cl.
void __fastcall KfLowerIrql(xbox::KIRQL NewIrql) noexcept;
KRNL_FUNC(KfLowerIrql, &KfLowerIrql);

// DbgPrint - slot 98, Ord 8, cdecl variadic (only D3D).
ULONG __cdecl DbgPrint(const char* Format, ...);
KRNL_FUNC_STUB_OK(DbgPrint, &DbgPrint);

// XboxKrnlVersion - slot 75, DATA.   NtUserIoApcDispatcher - slot 39, ADDR.
KRNL_DATA(XboxKrnlVersion, &g_XboxKrnlVersion);
KRNL_ADDR(NtUserIoApcDispatcher, &NtUserIoApcDispatcher);
}
```

- `KRNL_FUNC` static_asserts kind, calling convention, callee stack purge (sum of parameter sizes rounded
  to 4; `LARGE_INTEGER` by value = 8, e.g. `KeSetTimer(PVOID, LARGE_INTEGER, PVOID)` = 16) and fastcall
  register-argument count against `krnl::exp::<Name>` from `kernel\gen\kernel_table.gen.h`. Verified:
  wrong purge, stdcall instead of fastcall, missing edx argument, `KRNL_DATA` on a FUNC slot all fail to compile.
- Status: `KRNL_FUNC` = impl, `KRNL_FUNC_STUB_OK` = plausible stub for replaced libraries (should only
  log at trace), `KRNL_DATA` / `KRNL_ADDR` = data / address exports.
- Unregistered slots: FUNC -> generated stub with exact convention and purge, logs
  `UNIMPLEMENTED Ord <n> <Name>(0x..., ...) ret=0x000F9C9C FUN_...+0x..` once per return address (KRNL/warn),
  returns the failure value of the export's return kind (phase 5, review finding 1): `STATUS_NOT_IMPLEMENTED`
  only for NTSTATUS exports, 0 (NULL/FALSE) for pointer/BOOLEAN/ULONG/void exports (`gen::ExportInfo::ret`,
  table `RETURN_KIND` in `tools\gen\gen_kernel_table.py`); with `[kernel] strict=1` debug break + `core::Fatal`.
  Slots in `FATAL_STUBS` (`stubFatal`) abort always; the list is empty since M2 (107 MmCreateKernelStack and
  108 MmDeleteKernelStack were in it until the fiber agent implemented them, `kernel\mm_kernel_stack.cpp`).
  DATA -> zeroed writable 0x400-byte placeholder; ADDR -> routine that aborts when called.
- Registration of the same slot twice is reported by `krnl::ValidateRegistry()` (error).
- Resolution API (core): `krnl::ExportAt(slot)`, `krnl::FindExportByOrdinal(ord)`, `krnl::GetBinding(slot)`
  `{ address, status, implName, file }`, `krnl::IsImplemented(slot)`, `krnl::ValidateRegistry()`,
  `krnl::LogBindingSummary()`.
- Address of another export at runtime (e.g. the io agent comparing `ApcRoutine` with
  `NtUserIoApcDispatcher`): `krnl::GetBinding(krnl::exp::NtUserIoApcDispatcher::slot).address`.
- Table: `krnl::gen::kExports[slot]` `{ slot, ordinal, kind, conv, argDwords, regArgs, purgeBytes,
  variadic, onlyReplacedLibs, milestone (design Anhang A), name }`.

### 4.7 Guest handles - `kernel\ob_handles.h`

```cpp
class FileObject final : public krnl::KObject {
public:
    const char* TypeName() const noexcept override { return "File"; }
    NTSTATUS OnHandleClose() noexcept override { return STATUS_SUCCESS; }   // per-handle cleanup
};
auto obj = krnl::MakeObject<FileObject>(...);                  // Ref<FileObject>, refcount 1
HANDLE h = krnl::HandleTable::Instance().Insert(obj);          // 0x1000 + 4n, lowest free reused
krnl::Ref<FileObject> f = krnl::HandleTable::Instance().LookupAs<FileObject>(h);
```

`krnl::NtClose` (slot 26) is implemented and registered in `ob_handles.cpp`: unknown handle ->
`STATUS_INVALID_HANDLE`, pseudo handles (-1/-2/-3) -> `STATUS_SUCCESS`. The object is destroyed when
the last `Ref` goes away (virtual destructor).

### 4.8 VFS - `vfs\vfs.h` (io agent implements; skeleton stubs in `vfs\vfs_stubs.cpp`)

`vfs::File` (positional `Read/Write`, `QueryInfo`, `SetEndOfFile`, `SetTimesAndAttributes`,
`SetDeleteOnClose`, `Flush`, `NextDirEntry`), `vfs::Device` (`Open`, `QueryAttributes`,
`QueryVolumeSize`, `Kind`, `IsReadOnly`), `vfs::XdvdfsImage` (`Open`, `Lookup`, `ListDirectory`, `Read`),
`vfs::ReadIsoFile(iso, "default.xbe", bytes, err)` (used by `core::StageLoadXbe`),
`vfs::CreateIsoDevice`, `vfs::CreateHostDirDevice(root, readOnly)`, namespace functions `vfs::AddDevice`,
`FindDevice`, `CreateSymbolicLink` (`STATUS_OBJECT_NAME_COLLISION` if it exists), `DeleteSymbolicLink`,
`QuerySymbolicLink`, `Resolve(objectPath, ResolvedPath{device, devicePath, relativePath})`, path helpers
`NormalizePath`, `IEquals`, `IStartsWith`. Constants `vfs::kCdRom0`, `vfs::kPartition1`,
`vfs::kDosDevicesPrefix`. The core `XMountUtilityDrive` hook uses `CreateHostDirDevice(cfg.CacheDirectory()
/ "Z", false)` + `AddDevice` + `CreateSymbolicLink("\\??\\Z:", ...)`.

### 4.9 Hooks - `core\hooks.h`

```cpp
// XAPILIB::QueryPerformanceFrequency 0xF642C, stdcall 1 arg. Evidence: kernel.md 8.8, design 1.9.
BOOL __stdcall QueryPerformanceFrequencyHle(LARGE_INTEGER* frequency) noexcept;
FUT_HOOK(guest::kXapiQueryPerformanceFrequency, &QueryPerformanceFrequencyHle, Stdcall,
         "XAPILIB::QueryPerformanceFrequency");
FUT_HOOK_EX(addr, &fn, Thiscall, "Name", "55 8B EC" /*expected bytes or nullptr*/, true /*default on*/);
core::InstallHookNow(core::HookDesc{guest::kGameMain, core::detail::CodeAddress(&StopAtMain),
                     core::HookConv::Cdecl, "main (stop-at)", nullptr, true, __FILE__});
```

Validation (design 3.2): known function start, size >= 5, no overlap with other hooks or fs patch sites,
expected bytes. `[hooks] disable = name, 0xADDR, prefix*`; addresses compare by value (`0xF641B` = `0x000F641B`),
entries without a matching hook are logged as warnings. Member functions: `core::detail::CodeAddress(&C::f)`.

Probe hooks (phase 3, log + call the original; design "Abweichungen in M1"):

```cpp
// XAPILIB::XapiInitProcess 0xF92C0, no args; prologue push ebp; mov ebp,esp; sub esp,0x34 (capstone-checked).
std::uint32_t __cdecl XapiInitProcessProbe() noexcept {
    FUT_LOGI(Xapi, "boot: XapiInitProcess start");
    return core::CallGuest(core::HookTrampoline(guest::kXapiInitProcess));   // original via trampoline
}
FUT_PROBE(guest::kXapiInitProcess, &XapiInitProcessProbe, Cdecl, "probe:XAPILIB::XapiInitProcess",
          "55 8B EC 83 EC 34" /*exactly the stolen bytes*/, 6 /*5..16 whole, position-independent insns*/);
```

The stolen bytes must be whole instructions without relative branches, and no branch may target their
inside (check with capstone). The installer compares them with the XBE file, builds an executable
trampoline (stolen bytes + `jmp address+n`) and writes the usual 5-byte jmp; `HooksInstalled` verifies the
trampolines. M1 probes (`xapi\boot_probes.cpp`, all `probe:*`): XapiInitProcess, _rtinit, _cinit,
XGetDiskClusterSizeA 0xF8081, memory.cpp pool init 0xA4920, XapiBootToDash.

### 4.10 Self-tests - `core\selftest.h`

```cpp
FUT_SELFTEST(TlsSlotsReadBack, Host) { detail = "..."; return ok; }   // phases Host, Mapped, AdoptedThread
```

Run with `--selftest`; result lines `SELFTEST PASS|FAIL <name>: <detail>`. `core::InSelfTest()` (M2 review) is
true while the runner executes a test on the calling thread; the fiber counters and the VFS trace use it to keep
the self-tests' own activity apart from the game's.

### 4.11 Guest structures - `src\shared\`

- `shared\win32.h`: Windows include policy (force-included), `NTSTATUS`, all `STATUS_*` from `<ntstatus.h>`.
- `shared\xbox_types.h` (namespace `xbox`, never `using namespace xbox`): `ANSI_STRING` (= `STRING`,
  `OBJECT_STRING`), `UNICODE_STRING`, Xbox `OBJECT_ATTRIBUTES {RootDirectory, ObjectName (PANSI_STRING),
  Attributes}` (12 bytes, verified at CreateFileA 0xF76E0), `IO_STATUS_BLOCK`, `OVERLAPPED`,
  `LARGE_INTEGER`, `KIRQL`, `ACCESS_MASK`, `ObDosDevicesDirectory` (-3), `OBJ_CASE_INSENSITIVE`,
  `FakeKThread`, `FakeKPrcb`, TLS layout constants (`kTlsDataSize` 0x24, `kTlsIndex` -9), `kIrqlDispatch`.
- `shared\nt_status.h`: `xbox::NtStatusName(status)` for logs.
- `shared\xbe_format.h` (namespace `xbe`): `ImageHeader` (0x180), `SectionHeader` (0x38),
  `Certificate` (0x1EC), `TlsDirectory`, `LibraryVersion`, XOR keys, section flags.

### 4.12 Generated files - `tools\gen\`

| Generator | Input | Output |
|---|---|---|
| `gen_kernel_table.py` | `analysis\loader\kernel_imports.tsv` + design Anhang A | `src\host\kernel\gen\kernel_table.gen.h/.cpp` |
| `gen_fs_patches.py` | `analysis\loader\fs_sites.tsv` (offset != 0x0; cross-checked against the XBE if present) | `src\host\gen\fs_patch_table.gen.inc` |
| `gen_functions.py` | `analysis\retail_eu_export\functions.tsv` + `tools\gen\extra_function_starts.tsv` | `src\host\gen\xbe_functions.gen.inc` |
| `gen_d3d_traps.py` (M2, gfx) | `analysis\loader\d3d_functions.tsv` (`ext_call_sites > 0` minus the 8 shims and the kernel thunk 0x2417B0; tail jumps resolved; `ret N` cross-checked with capstone against the XBE if present) | `src\host\gen\d3d_traps.gen.inc` (X-macro `FUT_D3D_TRAP`, 79 rows, section 4.15) |
| `gen_renderer_slots.py` (M2, gfx) | `analysis\loader\nixboxrenderer_vtable.tsv` + `ret N` table of `renderer_retn.py` (vtable entries and every `ret N` cross-checked with capstone against the XBE if present) | `src\host\gen\renderer_slots.gen.inc` (X-macro `FUT_RENDERER_SLOT`, 75 rows, section 4.15) |
| `gen_xdk_entries.py` (M2, xdk) | `analysis\loader\avi_api_surface.tsv` (54 entries + 5 stream vtable rows; `ret N` of every entry and of the 7 stream vtable slots cross-checked with capstone against the XBE if present) | `src\host\gen\xdk_entries.gen.inc` (X-macros `XDK_ENTRY`, `XDK_STREAM_METHOD`, section 4.13) |

`python tools\gen\gen_all.py` regenerates, `--check` (or target `fut_gen`) verifies. Generated files are
checked in; the build needs no Python. New function starts outside Ghidra go into
`extra_function_starts.tsv` (with evidence) - this also makes them hookable.

### 4.13 XDK stubs (DSOUND, XMV, XPP) - `audio\xdk_entries.h` (M2, xdk agent)

The 54 XDK entry points the game calls (design E7, Anhang B: 40 DSOUND, 7 XMV, 7 XPP) are replaced by
silent stubs in `audio\dsound_stubs.cpp`, `video\xmv_stubs.cpp`, `input\xpp_stubs.cpp` (namespaces `snd`,
`vid`, `inp`; declarations in the matching `.h` for tests). Every stub has a header comment with
address, `ret N`, arguments and the checked game call sites.

```cpp
// DSOUND::DirectSoundCreate 0x24935C - stdcall, 3 args (ret 12). Evidence: ...
std::int32_t __stdcall DirectSoundCreateHle(const void* guid, void** ppDS, void* outer) noexcept { ...
    XDK_TRACE_CALL(DirectSoundCreate, "(guid=0x%08X, ...) -> 0x%08X", ...);  // count + log once per site
    return hr; }
XDK_HOOK(DirectSoundCreate, &DirectSoundCreateHle);  // static_assert stdcall noexcept + purge == ret N
```

- `XDK_HOOK(Id, &fn)` = FUT_HOOK with hook name `<LIB>::<Id>` (`[hooks] disable = DSOUND::*` restores a
  whole library) plus compile-time checks against `xdk::gen::<Id>` (address, `purgeBytes`, flags).
- Table/API: `xdk::Entries()`, `Info(Entry)`, `RegisteredStub`, `CallCount`, `CallSiteCount`,
  `LogCallSummary(level)` (also crash reporter "xdk" and, at Info, exit reporter "xdk" - M2 integration),
  `xdk::stream::<Method>::slot/purgeBytes` (XMediaObject vtable 0x2A5F5C).
- Guest-visible dummy objects (below 0x80000000): `AllocGuestObject/FreeGuestObject/IsGuestObject`
  (64-byte slots), `TrapVtable()` (+0 of every dummy handle; a virtual call ends with `TRAP`).
- `xdk::MeasureEntryPurge(entry, dwords)`: runs an entry once on a probe thread with zero arguments and
  returns the bytes it popped (same method as `gfx/purge_probe`); self-test `XdkEntryPurgeMeasured`
  measures all 54 hooked guest entries (Mapped phase), `XdkHooksInstalled`, `XdkStubsRegistered`,
  `XdkGuestObjectPool`.
- Returned values (evidence per function): DSOUND - `DirectSoundCreate` S_OK + non-NULL dummy device
  (SoundManager::Update 0xCBCB0 needs it to run the voice update), `CreateBuffer`/`CreateStream` S_OK +
  `*pp = NULL` (all uses NULL-checked), `IDirectSoundBuffer_GetStatus` writes 0 always, other HRESULTs
  S_OK. XMV - decoder from the first-packet header (W/H/tracks), `GetNextFrame` result 2 (movie ends on
  the first Update), audio streams NULL. XPP - no devices (`XGetDevices` 0, `XGetDeviceChanges` FALSE,
  `XInputOpen` NULL, `XInputGetState`/`SetState` 0x48F, rumble `dwStatus` never left at 0x3E5).
- `[audio] dummy_streams = 1`: `DirectSoundCreateStream` returns stub streams (XMediaObject vtable;
  `Process` sets E_PENDING synchronously, packets complete after their play time in `DirectSoundDoWork`).
- Log: one INFO line per library on first use (`DSOUND: stub device`, `XMV: stub decoder`, `XPP: stub
  input`), every call site once at debug on SND/VID/INP (`XDK <LIB>::<Id>(args) -> result ret=<site>`).

### 4.14 Fibers - `xapi\xapi_fiber.h`, `kernel\mm_kernel_stack.h` (M2, fiber agent)

Design 1.7 "Fibers", E9, R2. XAPI `CreateFiber` 0x1708B0, `DeleteFiber` 0x17093C, `ConvertThreadToFiber`
0x17098E and the trampoline 0x17086B stay guest code.

- `krnl::MmCreateKernelStack` / `MmDeleteKernelStack` (slots 107/108, stdcall 2/2): VirtualAlloc of
  `max(NumberOfBytes, [kernel] fiber_stack = 0x80000)` committed + one guard page (RW|GUARD) below; returns
  `top = StackBase - 0x20`. Above top: fiber-top SEH record (StackBase-0x18, host handler that calls
  `UnhandledExceptionFilter` = the host crash report) -> terminal record (StackBase-0x10, filled by ntdll
  `RtlInitializeExceptionChain` = ntdll's FinalExceptionHandler unless SEHOP is disabled) -> -1; the top 8
  bytes stay 0 (SEHOP requires every record < StackBase - 8). Bookkeeping: `krnl::FindKernelStack(top)`,
  `FindKernelStackContaining(addr)`, `GetKernelStackStats()`, `ConfiguredFiberStackBytes()`,
  `KernelStackTerminalHandler(IsNtdll)()`.
- `XAPILIB::SwitchToFiber` 0x17094F is detoured (`FUT_HOOK_EX`, name `XAPILIB::SwitchToFiber`) to
  `fut_switch_to_fiber` (`asm\fiber_switch.asm`, stdcall 1, same saved-context layout as the original) +
  `fut_fiber_switch_prepare` (C++): additionally sets `TEB.NtTib.StackBase/StackLimit` (fs:[4]/fs:[8]) and
  `TEB.DeallocationStack` (fs:[0xE0C]) from the bookkeeping (thread fiber `[f+8] = 0`: the real values saved
  when the thread fiber was left) and replaces the saved ExceptionList -1 of a fresh fiber by the fiber-top
  record. Callable from host code (`fut_switch_to_fiber(fiber)`) on an adopted thread.
- Counters: `xapi::GetFiberStats()` `{switches, toThreadFiber, chainFixups, unknownStacks, selfTestSwitches}`,
  `xapi::FiberSwitchCount()`, `xapi::LogFiberSummary()` (XAPI/info `fiber summary: N switches (G game, S
  self-test; ...)`; crash reporter "fibers" after the first switch; exit reporter "fibers" since the M2
  integration). Log evidence: KRNL/info `fiber stacks: ...` (first stack), XAPI/info `fiber: first SwitchToFiber
  (self-test) ...` and `fiber: first SwitchToFiber (game) ...` (M2 exit 5 needs the game line; review finding 5),
  every switch at XAPI/trace.
- Self-tests (AdoptedThread, `core\selftest_m2.cpp`): `GuestSehOnFiberStack` (M2 exit 6) and
  `GuestSehThroughHostFrame` (rule below). Unit tests `tests\fiber_tests.cpp` (incl. child processes with the
  SEHOP mitigation enforced: positive, negative control, unhandled -> top-level filter).
- **Rule for hooks / host frames between guest frames** (verified by `GuestSehThroughHostFrame`, MSVC and
  clang-cl): a guest SEH exception (AV, RtlRaiseException) passes through host C++ frames: the search phase
  skips them, the guest handler's unwind (`__global_unwind2` -> RtlUnwind slot 105) removes them, guest
  `__finally` blocks run - but **no destructor of a host frame runs** and `catch (...)` / `FUT_HLE_CATCH` never
  sees it (/EHsc: `__CxxFrameHandler3` ignores non-C++ exceptions). Hence a host function that calls guest
  code (probe, hook calling an original, HLE delivering a callback) must not hold a lock, an owning object or
  other state that needs cleanup across that call, or must restore it before calling into the guest.

### 4.15 Graphics: NiPCRenderer, D3D shim, D3D traps - `gfx\*.h` (M2, gfx agent)

Design 2.2 (E6): the graphics boundary is the NiRenderer interface. Nothing is drawn in M2.

- **`NiXBoxRenderer::Create` 0x14C170** (cdecl, 8 args; call site 0xD26BE..0xD26E7, `add esp,0x20`) is
  detoured (hook name `NiXBoxRenderer::Create`) to the **NiPCRenderer** null renderer (`gfx\nipc_renderer.h`):
  0x6D0-byte object from the game's operator new 0xA4900 (as the original), built by the original NiRenderer
  base ctor FUN_00140550 (thiscall, verified at 0x14AF18/0x14AF1F: renderer list 0x2D7B18 for the purge /
  create broadcasts), then +0x00 own 75-slot vtable, +0x60 dummy device (`gfx::d3d::DummyDevice()`, 0x2040
  zero bytes), +0x6C0 host state. +0x04/+0x08/+0x0C/+0x10 are the NiRenderer base fields the game writes.
- **Slots** (`gen\renderer_slots.gen.inc` from `tools\gen\gen_renderer_slots.py`: original address and
  `ret N` of all 75 slots, capstone-checked against the XBE): 2..10 = original NiObject entries of 0x2AA7C0;
  all others are host `__fastcall(self, edx, stack args...)` functions with the original purge
  (`static_assert` per handler + runtime measurement). Results: 0 destroys via FUN_001406E0 + operator
  delete 0xA3880; 1 = &NiXBoxRenderer::ms_RTTI 0x30EE00; 15 SetRenderTarget (FALSE in a scene, calls the game's
  screen-offset helpers 0xA1930/0xA1920 like vf15); 16 DisplayFrame (back buffer only: frame counter, pacing,
  `--frames` limit); 21/22 = 640x480 or the rendered texture's size; 23 = 32; 27 SetBackgroundColor TRUE;
  29 GetMaxFogValue float 1.0; 39 PrecacheGeometry FALSE; 40 writes NULL to its out pointer; 45/46 scene
  flag; 57/69/70 TRUE; 65/66 TRUE + renderer data; everything else 0. Each slot's first call is logged on
  GFX/debug (`NiPCRenderer slot N (...) first called from ...`).
- **Texture renderer data** (slots 65/66, chain at NiTexture+0x20): `gfx::NiPCTextureData` (0x70, layout of
  NiXBoxSourceTextureData: +0x10 renderer, +0x3C D3D texture, +0x48/+0x4C size, +0x54, +0x58, +0x5C, +0x60 -
  the offsets the movie player FUN_000EE220 writes) and `NiPCRenderedTextureData` (0x84 + target descriptor).
  Own data vtables (11 / 14 slots) mirror 0x2AB97C / 0x2AB9F4; slot 0 releases +0x3C through the D3D shim,
  deletes the rest of the chain and frees through operator delete. Slot 65 picks the image in the order of
  FUN_0015E010 (M2 review finding 1): the texture file (+0x30) if a path is set, read through the game's
  NiImageConverter (0x30EF64, vf2), else the NiPixelData (+0x34); no image, an unreadable file or a width/height
  that is not a power of two gives the renderer's default texture (8x8, A8R8G8B8 - FUN_0014AB50), silently
  (GFX/debug, counters in the statistics line). `[render] load_textures = 0` skips the file read (default
  texture, counted as "not loaded").
- **D3D shim** (`gfx\d3d_shim.h`, design 2.2 table, stdcall, purge `static_assert`ed): `D3DDevice_AddRef/
  Release` (counter), `CreateTexture2` (Xbox-like header {Common 0x1040001, Data, Lock, Format, Size} + pixel
  buffer, e.g. W*H*2 for YUY2 0x24), `D3DTexture_GetSurfaceLevel2` (surface 0x1050001 holding its parent),
  `D3DResource_AddRef/Release` (rules of 0x2373B0/0x2373F0; NULL/unknown pointers ignored, logged once),
  `SetRenderState_YuvEnable`, `SetScreenSpaceOffset`. Host API: `LookupResource(header)` (pixels for M3/M6),
  `GetShimState()`, `DummyDevice()`. Hook names `D3D8::<function>`.
- **D3D traps** (`gfx\d3d_traps.h`, `gen\d3d_traps.gen.inc` from `tools\gen\gen_d3d_traps.py`): the 79 other
  D3D entries with `ext_call_sites > 0` (88 - 8 shims - the MmFreeContiguousMemory import thunk 0x2417B0),
  registered through `core::RegisterHook` (hook names `trap:D3D8::...`, `[hooks] disable = trap:*`). Stubs
  have the original convention/purge (stdcall, `SetRenderState_Simple` fastcall) and log
  `TRAP <name> @<address> from <return> (<args>)` on GFX/error, then `core::Fatal`; `[render] d3d_traps = log`
  logs once per call site and returns 0.
- **Frames** (design 5.4): `[debug] frames = N` / `--frames=N` ends the process after N back-buffer frames:
  `FRAMES: limit of N frames reached`, `core::ExitHostProcess(0, "frame limit reached")`; the statistics come
  from the exit reporters (section 4.16).
  `[render] frame_rate = 50` (default) paces DisplayFrame like the Xbox vblank wait (PresentationInterval ONE,
  PAL): N frames take N/50 s; `0` = unthrottled (time-based game loops then spin, e.g. 3000 frames in ~2 ms
  before the main loop runs). API: `gfx::FramesDisplayed()`, `SlotCallCount(slot)`, `CurrentRenderer()`,
  `LogRendererStatistics()` (also crash reporter and exit reporter "gfx", registered by the first Create).
- **Purge measurement** `gfx::MeasureCalleePurge(entry, ecx, edx, stackDwords)` (`gfx\purge_probe.h`):
  runs an entry on a suspended-then-resumed host thread whose context returns into a landing function;
  exit code = bytes popped. Entries check `gfx::InPurgeProbe()` first and return without side effects.
- Self-tests (Mapped): `GfxRendererSlotTable`, `GfxRendererThunkPurge` (66 + 11 + 14 thunks measured),
  `GfxD3DShimTrapPurge` (8 + 79). Unit tests: `tests\gfx_tests.cpp`.

### 4.16 Exit reporters and game observer probes - `core\host_main.h`, `xapi\game_probes.h` (M2 integration)

- **Exit reporters** (`core\host_main.h`): `core::AddExitReporter(name, fn)` (`void() noexcept`, at most 16, a
  second registration of a name is ignored) - `core::ExitHostProcess` runs them on the exiting thread before the
  `process exit` line, sorted by name, **not** after a watchdog timeout (exit 5; a hung guest thread may hold a
  lock); `core::RunExitReporters()` / `ExitReporterCount()` for tests. A reporter must not throw, block on guest
  threads or call `ExitHostProcess`. `core::Fatal` runs the crash reporters instead (core\crash.h). Registered:
  `fibers` (`xapi::LogFiberSummary`, static), `game` (`xapi::LogGameSummary`, first archive mount),
  `gfx` (`gfx::LogRendererStatistics`, first `NiXBoxRenderer::Create`), `io apc`
  (`krnl::io::LogIoApcStats("process exit")`, static), `xdk` (`xdk::LogCallSummary(Info)`, static). Every orderly
  end (`--frames`, `--stop-at`, guest shutdown) therefore logs the fiber, APC and XDK lines (M1 logs: 5 lines).
  M2 review finding 7: if the orderly exit itself hangs (a reporter or the log flush blocks), the watchdog calls
  `ExitHostProcess(kExitWatchdog)` again, which then terminates at once (stderr line, exit code 5) instead of
  waiting; `core::ExitInProgress()` tells the watchdog to skip its log-based thread dump. Test knob
  `[debug] exit_hang_test`. The `io apc` line reads `..., error completions N, failed APC reads N (disc error
  path), failed APC writes N (save layer)` (finding 2: `IoApcStats::failedWriteRequests`; a failed WriteFileEx
  is a KRNL/warn, a failed ReadFileEx a KRNL/error).
- **Game observer probes** (`xapi\game_probes.cpp`, FUT_PROBE: log + call the original, design 8.2 B7): the
  evidence of M2 exit criteria 1-5 and 7 on log level info.

| Hook name | Address | Log / trace |
|---|---|---|
| `probe:Game::ArchiveMount` | `FUN_0013FE70` (cdecl 1) | VFS/info `archive: mounted <name> as #<i> (<size> bytes, header 0x.. bytes, stream ..)` / `archive: <name> not mounted (...)`; trace `archive mount "<name>" ... : mounted` / `: not mounted` |
| `probe:Game::ArchiveLookup` | `FUN_0013FE10` (cdecl 3) | trace per asset `archive lookup "<name>" -> <archive>+0x<offset> (<size> bytes) : hit archive #<i>` / `: not archived (loose file D:\ follows)`; VFS/trace in the log |
| `probe:Game::Init` | `FUN_000A26A0` (thiscall) | GAME/info `game: Game::Init (...) -> 1`, `game: video standard CApp+0xE8 = 0 (PAL; 50 fps, fixed time step 0.0200 s, movie tag "PAL")`, `game: language "german" (...)`, `game: BUILD.TXT = "1.25"`, `game: archives mounted: ...`, `game: contexts ...; current "Intro" (...)`, `game: disc error handler [0x0030E68C] = 0x002D707C (... = trap:Game::DiscErrorScreen)` |
| `probe:CApp::Frame` | `FUN_000D2630` (thiscall, every frame) | counts frames; fallback `game: context <from> -> <to> (CApp::Frame #N) [seen after the frame]` if CApp+0xBC changed without the probe below; GAME/warn if it returns FALSE (main loop ends) |
| `probe:Game::ContextSwitch` (M2 review) | `FUN_000A48C0` (cdecl 0, pool maintenance; logs only for the call 0xA2E40 in Game::Frame, return 0xA2E45) | GAME/info `game: context <from> -> <to> (CApp::Frame #N)` at every switch Game::Frame makes, also several within one frame (finding 4) |
| `probe:XAPILIB::XGetLanguage` | 0xF6440 | XAPI/info `XGetLanguage() = 3 (german), called from ...` whenever the value changes |
| `trap:Game::DiscErrorScreen` | 0xF2FD0 (vf0 of the disc error handler 0x2D707C, thiscall 1; extra function start) | GAME/error `TRAP DiscErrorScreen @0x000F2FD0 from ... (stream ...)` + `core::Fatal` (the original loops forever) |

  API: `xapi::GetGameObserverStats()` `{archivesMounted, archiveMountFailures, archiveLookups, archiveHits, frames,
  contextSwitches, switchesSeenAfterFrame, discErrorCalls, language, languageCalls, videoStandard, gameInitDone}`,
  `MountedArchiveNames()`,
  `ContextHistory()`, `XboxLanguageName(value)`, `LogGameSummary()` (GAME/info `game summary: ...`). Guest
  constants in `core\guest.h` (block "added in M2 phase 2"). Unit tests: `tests\m2_integration_tests.cpp`.

### 4.17 Graphics M3: RHI, extraction, texture conversion, fixed function - `gfx\GFX_CONTRACT.md` (M3 phase 1)

The binding contract is `src\host\gfx\GFX_CONTRACT.md` (architecture, verified NetImmerse conventions,
per-module requirements, ownership, exit-criteria evidence). Summary of the phase-1 headers (all compile;
implementations are stubs except where noted):

- `gfx\gfx_math.h` (shared, implemented): `Float2/3/4`, `Matrix4` (D3D row vectors, row-major),
  `IdentityMatrix`, `Multiply`, `TransformPoint/Vector/Float4`, `Transpose`, `Inverse`, `Dot`, `Normalize`,
  `NiTransformToWorld(rotate3x4, translate, scale)` (= FUN_001489A0).
- `gfx\rhi\rhi.h` (shared; `rhi.cpp` implemented: format table `GetFormatInfo`/`RowPitch`/`LevelSize`,
  `HashOf(SamplerDesc|PipelineDesc)`, `BackendName`, `CreateDevice` dispatch): typed handles, `Format`,
  buffer/texture/sampler/blend/depth-stencil/raster/vertex-layout/pipeline descriptors (defaulted `==`),
  `CommandContext` (targets, viewport, scissor, clear with rect, pipeline, streams, 16-bit indices, constant
  buffers, textures, samplers, draws, `UpdateBuffer`, transient ring), `Device` (create/destroy, cached
  `GetSampler`/`GetPipeline`, `Present`, `CaptureBackBuffer`, `ReadTexture`, debug layer info + messages,
  stats). Backend `gfx\rhi\d3d11\d3d11_device.h`: `gfx::rhi::d3d11::CreateDevice` (stub: nullptr).
- `gfx\ni_guest_layouts.h` (namespace `gfx::ni`): verified guest layouts (NiTransform 3x4 rows,
  NiGeometryData and subclasses, NiSkinInstance + `kNiSkinInstanceDeform` 0x14F710, NiPropertyState and the
  properties, NiDynamicEffectState, NiLight, NiPixelFormat/NiPixelData/NiPalette, NiTexturingProperty + Map,
  NiSourceTexture, NiScreenPolygon, NiScreenTexture, NiCamera offsets) with static_asserts and evidence.
- `gfx\ni_extract.h`: `CameraDesc ExtractCamera(CameraInput)`, `DefaultCamera`, `ClearRect`, `ExtractState`,
  `ExtractShape/Strips/Triangles/TriStrip(ExtractContext, GeometryInput, DrawItem*)`, `ExtractScreenPolygon`,
  `ExtractScreenTexture`; `DrawItem {kind, world, geometry streams/indices, RenderStateDesc (D3D8 states),
  LightSetDesc (D3DLIGHT8), TextureEffectSlots}`; `ExtractArena` (implemented); `StateShadow` (D3D state that
  persists between draws, `ExtractContext::shadow`).
- `gfx\texconv.h`: `ConvertPixelData(NiPixelData*, palette, options, HostImage*)`, `ConvertPixels` (host
  form), `MakeDefaultTexture` (FUN_0014AB50), `SelectTextureSource` (slot 65 order of FUN_0015E010).
- `gfx\shaders\ff_shared.h` (C++ and HLSL): constant buffers b0-b3, feature key bits, vertex streams, D3D8
  op/arg/material-source/fog/light codes. `gfx\ff_pipeline.h`: `FixedFunctionPipeline {Init, SetCamera,
  Draw, Stats}` + pure translations (`MakeFeatureKey`, `Make*Desc`, `Fill*Constants`).
- Shaders: `fut_add_shader(<name> <file> <entry> <profile> [defines])` in `src\host\CMakeLists.txt` runs fxc
  at build time into `<build>\src\host\gen_shaders\<name>.h` (`g_<name>`, DXBC, SM 4.0); phase-1 stubs
  `ff_stub_vs`/`ff_stub_ps` from `gfx\shaders\ff_ubershader.hlsl`.
- `platform\window.h` (`MainWindow`, `ShowMainWindow`, `MainWindowClientSize`, `ConsumeHotkeys`),
  `platform\frame_pacer.h` (`PaceFrame`, `NotePresentedFrame`, `GetPresentStatistics`,
  `LogPresentStatistics`), `platform\screenshot.h` (`WritePngFile`, `ScreenshotPath`) - stubs.
- Configuration (`gfx::Settings`, `gfx\gfx_config.h`): `[render] backend` (d3d11|null; default d3d11 since
  the M3 integration, section 4.18), `vsync`, `debug_layer` (auto|on|off), `adapter` (hardware|warp), `window_scale`,
  `fullscreen`, `screenshot_dir`, `screenshot_frames`, `trace_frames`, `validate`; `gfx::WantDebugLayer`.
- Real-data harness (`gfx\nipc_validate.h`, GFX_CONTRACT.md 10): with `[render] validate = 1` the null
  renderer's new slot handlers 47 (SetCameraData), 48-51 (render geometry), 54/55 (screen polygon/texture)
  and slot 65 also run `ni::Extract*` / `tex::ConvertPixelData` on the real guest data and log result counts
  (`validate: ...`); default off = M2 behaviour (the handlers only count, like the former no-op thunks).
  Template `run\m3-iface\validate\futurama.ini`.
- Tests: `tests\gfx_contract_tests.cpp` (math, RHI helpers, arena, constant-buffer sizes).

### 4.18 Graphics M3 integration: NiPCRenderer drawing through D3D11 - `gfx\nipc_gpu.h` (M3 phase 3)

`GFX_CONTRACT.md` section 9 "Phase-3 results" is the reference; summary:

- `[render] backend = d3d11` (default): the `NiXBoxRenderer::Create` detour creates a `gfx::GpuRenderer` for
  `platform::MainWindow()` (`GpuRenderer::Create(CreateParams{window, width, height, debugLayer,
  softwareAdapter, resolveTexture, resolveUser}, &error)`); failure or no window -> GFX/error, null renderer.
  `backend = null` = the M2 null renderer (plus `[render] validate`).
- GpuRenderer API (guest main thread, noexcept): `SetBackBufferTarget`, `SetTextureTarget` (P2: counted
  no-ops), `SetLeftRightSwap`, `ClearBuffer(rect, mode, argb, depth, stencil)`, `BeginScene`,
  `SetCameraData(CameraInput)`, `DrawGeometry(kind, GeometryInput)`, `DrawScreenPolygon`, `DrawScreenTexture`,
  `CountNotInScene`, `DisplayFrame(frame)` (summary, screenshot, Present, `NotePresentedFrame`, arena reset),
  `CreateTexture(HostImage, name)`, `DestroyTexture`, `DefaultTexture` (FUN_0014AB50), `BlackTexture`,
  `LogStatistics`, `Describe`; `DrawOutcome` + `DrawOutcomeName`, `DrawKindName`; `HalfPixelShift(Matrix4*,
  viewportWidth, viewportHeight)` (+0.5-pixel projection shift, design 8.3 B19). Tests: `tests\nipc_gpu_tests.cpp`.
- New slot handler 17 ClearBuffer (ret 8); slots 15, 16, 19, 27, 45, 47-51, 54, 55, 65 extended (section 4.15 keeps
  the M2 rules). `NiPCTextureData` +0x64 `hostTexture`, +0x68 `hostInfo` (`tex::TextureSource` |
  `kHostOwnsTexture`), +0x6C `hostRevision`. Slot 65 = FUN_0015E010 incl. FUN_001572D0 (both backends); the
  resolver = FUN_0015F930 (on-demand data, FUN_0015E260 revision re-upload, black movie/rendered textures).
- Frame pacing: both backends call `platform::PaceFrame([render] frame_rate)` after the frame (the M2 copy in
  nipc_renderer.cpp is removed); the d3d11 path presents with `[render] vsync` first.
- Logs: `draw trace frame N #i: ...` (`trace_frames`), `draw summary frame N: ...` (trace + screenshot frames),
  exit reporter "gfx" adds `NiPCRenderer textures: ...`, `NiPCRenderer slot 65 stack: ...`, `d3d11 draws <Kind>:
  ...`, `d3d11 frames: ...`, `ff pipeline: ...`, `ni_extract: ...`, `d3d11 device: ...`.
- Check: `python tools\m3_check.py --exe build\m3\bin\futurama.exe --config run\m3\futurama.ini [--runs N]
  [--fresh] [--scene NAME=FRAME] [--xemu-ref DIR | --no-xemu-ref] [--min-hz 50]` (M3-1..4 + every M2 criterion;
  logs and screenshot copies in `<userdata>\m3_check\`).
- **M3 phase 5 (review fixes, `docs\m3_report.md`):** `GpuRenderer::PurgeLight(niLight)` (slot 60),
  `GpuRenderer::OnRenderThread()`, `CreateParams::scaling`; `ni::PurgeLight(StateShadow*, niLight)` +
  `ExtractCounters::lightsPurged`; `ni::kNiSourceTextureStatic` (+0x38), `ni::kNiSourceTextureDiscardPixelsHint`
  (+0x3C), `ni::kDiscardPixelsMaxSize`; `guest::kNiPixelDataFreePixels` 0x1511B0, `guest::kNiLightRtti` 0x30EBAC,
  `guest::kGameMemoryPoolUsedOffset/FreeOffset`, `kGameMemoryPoolPeak` 0x2E8D38, `kGameMemoryPoolInitialized`
  0x2E8D35. RHI: `rhi::PresentScaling` + `PresentScalingName`, `DeviceDesc::scaling`, `DeviceDesc::bufferCount`
  = persistent back buffers in rotation (Present semantics in `rhi.h`). Platform: `platform::CloseRequested()`
  (WM_CLOSE no longer ends the process on the UI thread), `platform::NoteFrameWorkDone()` +
  `PresentStatistics::work*`. Config `[render] scaling = linear | point | integer`. New slot handler 60
  (ret 4). Logs: `NiPCRenderer pixel buffers (...)`, `NiPCRenderer slot 60 PurgeEffect: ...`, `game memory pool:
  ...`, `present: frame work ...`, `window closed - ending after frame N`, `draw summary frame N: ...; shows the
  picture of frame M (K draw calls, D drawn)`, `draw trace frame N #i: ... screen (x0, y0)-(x1, y1)`.
  `m3_check`: `--xemu-frontend DIR` (default `run\xemu_ref_steam` if present), `--min-hz` default 49.5,
  `--max-work-ms` 20; `tools\xemu_capture.py --start S`.
- **M3b (verify/integration):** `m3_check` reads `[game] tick_rate` / `[render] frame_rate` (frame rate = tick rate
  unless set) and works in seconds: scenes legal 3.0 s, tech notice 8.0 s, loading 13.6 s, frontend 20.0 s times
  the rate (180/480/816/1200 at 60 Hz, 150/400/680/1000 at 50 Hz; `--scene NAME=FRAME` or `NAME=SECONDSs`),
  `--min-hz` default rate - 0.5, `--max-work-ms` default one tick period, new `--min-on-period` (default 95 % of
  the frame starts within +-5 % of the planned period, line `present: frame start intervals ...`). New criteria
  M3b-5/M3b-6 = `m2_check.check_timing` T1/T2. Reported for every run: the M3b `present:` lines (mode/plan,
  display, intervals, swap chain, on screen, environment). Configs: `run\m3b\{debug,rel,clang}` (60 Hz),
  `run\m3b\pal50` (`[game] tick_rate = 50`, `[render] pacing = timer`), `run\m3b\{m1,m2,m2-null}`; the M3
  configs `run\m3final\*` now set `[game] tick_rate = 50` + `[render] pacing = timer` instead of `[render]
  frame_rate = 50` (that key alone now logs GFX/warn with the 60 Hz tick). Duration comparison of the
  observables of `timing60.md` 8: `python tools\timing_compare.py --run run\m3b\dur50 --run run\m3b\dur60`
  (log stamps, VFS lookups, dense screenshots; `docs\loader_design.md` 8.4).

### 4.19 Game timing: `[game] tick_rate` - `xapi\game_timing.h` (M3b, timing agent)

Evidence: `analysis\loader\timing60.md` (patch table 2.3/2.4, readers 3, tick-counted cosmetics 4.10, NTSC
prototype comparison 5). The EU XBE keeps its PAL content (CApp+0xE8 = 0, `*-PAL.xmv`, PAL archives/language/
splash/legal); only the time base of Game::Init changes.

- Config `[game] tick_rate = 60 | 50` (choice key, default 60, anything else exit code 1).
  `xapi::ParseTickRate(text)`, `xapi::ConfiguredTickRate(cfg)` (60/50, 0 = invalid), `xapi::ActiveTickRate()`
  (set by the stage, 0 before), constants `kTickRateNtsc` 60, `kTickRatePal` 50, `kDefaultTickRate`. Other modules
  (frame pacer) read the rate with `ConfiguredTickRate` or `cfg.GetU32("game", "tick_rate", 60)`.
- Stage 8b `xapi::StageApplyGameTiming(cfg)` (after ApplyFsPatches, before InstallHooks): table
  `xapi::TimingPatches(rate)` (60: 5 Code rows 0xA2714 / 0xA271A / 0xA272A / 0xA2734 / 0xA273A + 2 Data rows
  0x2D6744 / 0x2D6748; 50: empty), 16 unchanged `xapi::TimingReferenceRows()` (CApp+0xE8 branch, NTSC
  instructions, PAL fst and movie tag store 0xA2744, common tail, .rdata 60.0f / 1/60). Engine
  `xapi::ApplyTimingPatches(patches, references, PatchMemory&, error)`: structure check
  (`ValidateTimingPatchTable`: only operand bytes change, no overlap with each other or an fs site), every row
  equal in the mapped image AND the XBE file (`core::ReadOriginalXbeBytes`), then write + read back; nothing is
  written on any mismatch ("already patched", "different XBE build", "changed by another patch or hook").
  `xapi::AppliedTimingPatches()` = rows in the image (self-test `MappingMatchesFile` excludes them).
- Read-back: `probe:Game::Init` calls `xapi::NoteGameInitTiming(app)` after the original (the first fixed step
  does it if that probe is disabled); `xapi::GetGameTimingReadBack()` `{valid, tickRate, values (TimingGlobals:
  fps, dt, dtMin, dtMax, fpsCopy1, fpsCopy2, frameTime, fixedStep, videoStandard, movieTag), matchesExpected, fps,
  dt}`, `xapi::ExpectedTimingGlobals(rate)`, `xapi::DiffTimingGlobals(actual, expected)`.
- `probe:Game::FixedStep` (FUN_0002C890, thiscall, called once per fixed step at 0xA2DD3 in Game::Frame;
  prologue `55 8B 2D A4 6F 2E 00`, 7 stolen bytes): `xapi::GetGameTickStats()` `{steps, frames, wallSeconds,
  gameSeconds, stepsPerSecond, stalls, stallSeconds, steadyStepsPerSecond, stepsPerFrame[0, 1, 2, 3+],
  maxStepsPerFrame}`, `xapi::StepHistogram` (unit-testable). Exit reporter "game timing" -> `LogGameTickSummary`.
- Frame clock `[game] frame_clock = lockstep | realtime` (choice key, default lockstep): `probe:Game::Timer`
  (FUN_000D2B90, cdecl `void(int64* out)`, prologue `83 EC 08 8B 0D 4C B4 2E 00`, 9 stolen bytes) calls the
  original and, for the three samples of Game::Frame (return addresses 0xA2C6F accumulator, 0xA2FA6 / 0xA2FBB
  render time), returns `xapi::LockstepClock` time: +ticks(dt) = trunc(QPF * dt) per frame, whole-step catch-up
  or hold only when real time is more than 3/4 step away (phase stays within +-12.5 ms at 60 Hz, real-time
  semantics and the 6 dt cap unchanged). All other callers keep real QPC. Reason (measured, M3 timer pacer at
  60 Hz on the 120 Hz machine, realtime): 584 frames with 0 and 587 with 2 steps of 5968 (9.8 % each) after
  context switches put the remainder on the step boundary (timing60.md 6, pacing60.md 2.6); with the M3b
  pacer (vblank k = 2, 6000 frames, `run\m3b-timing`): realtime 82 / 76, lockstep 6 / 6 frames with 0 / 2 steps
  (99.75 % exactly one step; the rest are stall catch-ups and their following hold). API:
  `xapi::ParseFrameClock`, `ConfiguredFrameClock(cfg, mode)`, `ActiveFrameClock()`, `FrameClockName`,
  `GuestTimerTicks(qpf, seconds)`, `LockstepClock` (`Advance(real, step)`, `Now`, `GetStats`),
  `GetFrameClockStats()`. Logs `game clock: lockstep - [game] frame_clock = lockstep; ...` (stage) and
  `game clock summary: lockstep, N accumulator samples: +1 step x a, catch-up x b (max +n steps), hold x c;
  phase real - virtual x..y ms; R render samples; ticks(dt) = 166666 at QPF 10000000` (exit). The display-slot
  clock of pacing60.md 4.4 (frame pacer counts missed vblanks) can replace `Advance` later; the hook stays here.
- Self-test `GameTimingPatched` (Mapped): all 23 rows hold their expected bytes, no installed hook overlaps.
- Log lines (GAME/info, parsed by the checks):
  `game timing: 60 Hz (NTSC timing values, PAL content) - [game] tick_rate = 60; 7 patches applied: ...` /
  `game timing: 50 Hz (original PAL timing values) - [game] tick_rate = 50; no patches, ...`;
  `game: video standard CApp+0xE8 = 0 (PAL; 60 fps, fixed time step 0.0167 s, movie tag "PAL")` (unchanged probe
  line); `game timing: effective after Game::Init: tick 60 Hz, fps 60 (0x42700000), dt 0.0166666675 s
  (0x3C888889), fixed step 1, dt clamps ..., CApp+0xE8 = 0, movie tag "PAL" - ok (NTSC timing values, PAL
  content)` (GAME/error `... - MISMATCH in <fields> ...` otherwise); at exit `game ticks: N fixed steps in W s
  (R steps/s at tick rate 60 Hz; game time G s; K stalls > 6 dt, about S s dropped by the guest's cap: steady
  R2 steps/s), steps per CApp::Frame over F frames: 0 x a, 1 x b, 2 x c, 3+ x d (max m)`; GAME/debug one
  `game timing patch 0x... <name>: <original> -> <patched>` line per row.
- Deliberately unchanged (header comment of `game_timing.h`, NTSC parity): tick-counted UI/camera cosmetics run
  like the NTSC version (frontend fade 50 ticks = 0.83 s, input lock 30 ticks, menu debounce 5 ticks, overlay
  fade 25 ticks, camera smoothing and the camera spring's fixed 0.01 s sub-steps), and so do three gameplay
  places found by the M3b review (`timing60.md` 12): rigid-body damping x0.98/x0.9 per tick (FUN_000AFA30), the
  Chicken Walker's fixed 0.02 aim/turn step (FUN_000445A0, +0x64 from 0x43EEC) and level-script loops (every
  loop ends in a compiler-inserted `Wait(0)`: short waits -17 % .. +11 % per iteration). Evidence script
  `analysis\loader\scripts\timing_review.py`. Tests: `tests\timing_tests.cpp` (config, table vs timing60.md, x87 model of the
  patched block = NTSC state with the PAL tag, rows vs the XBE file, engine on a fake image, histogram).

### 4.20 Display-coupled frame pacing: `[render] pacing` - `platform\frame_pacer.h` (M3b, pacing agent)

Design and measurements: `analysis\loader\pacing60.md` (German). Replaces the M2/M3 timer pacer
(`PaceFrame([render] frame_rate)`, default 50 Hz); header comments of `platform\frame_pacer.h`,
`platform\pacer_policy.h` and `platform\pacer_display.h` are the reference.

- Config: `[render] pacing = auto | vsync | timer | off` (choice, default auto), `[render] pacing_trace = <file>`
  (CSV, one line per frame, flushed by the exit reporter "pacing trace"), `[render] frame_rate` now defaults to
  `[game] tick_rate` (60; was 50); 0 keeps the M2 meaning (unthrottled = pacing off) **only with `pacing = auto`**
  (with `timer`/`vsync` the pacer runs at the tick rate and GFX/warn says so; use `pacing = off`), another value
  than the tick rate logs GFX/warn. `pacing = vsync` ignores `[render] vsync = 0` (GFX/warn). `--set` values are
  trimmed and unquoted like INI values (M3b review finding 10). `gfx::Settings::tickRate/pacing/pacingTrace`, `gfx::SettingsFromConfig(cfg)` (tests),
  `gfx::PacingConfigOf(settings)` -> `platform::PacingConfig {mode, vsync, targetHz, tickHz, tracePath}`.
- Per frame (render thread): `GpuRenderer::DisplayFrame` -> `platform::PlanPresent(config, SwapChainInfo
  {frameLatencyWaitable, tearingSupported})` -> `PresentRequest {syncInterval, allowTearing, skipSwapChain}` ->
  `rhi::Device::Present` -> `platform::NoteSwapChainPresent(PresentFeedback)` (+ `NotePresentedFrame` as
  before); `OnFrameDisplayed` -> `platform::EndFrame(config)` (null backend: the headless timer at the target
  rate). `PaceFrame(hz)` stays for tests only.
- Plans (pure functions, `platform\pacer_policy.h`: `SelectPlan`, `ClassifyRefresh`, `PlanHysteresis`,
  `FrameSchedule`, `DisplayLimitDetector`, `VrrDetector`, `IntervalHistogram`, `OnScreenTracker`):
  `vblank k` = refresh is k x target (+-0.2 %, k 1..4; 59.94/119.88 included) -> frame-latency waitable
  (maximum latency 1) + Present(k); `tearing-timer` = otherwise with tearing support -> absolute
  high-resolution timer at the target rate + Present(0, ALLOW_TEARING), VRR probed from the DXGI frame
  statistics (rejected -> `display-rate`); `display-rate` = Present(1), one game frame per refresh; `free` =
  window not in the foreground, minimized, display off, session locked or the display holding the pace ->
  timer, Present(0) only into a free slot (none while hidden); `timer`/`unthrottled` = overrides; `headless`.
  Out of `free` only after 30 frames - except when presenting resumes (restored from minimized: at once, M3b
  review finding 5); a frame without a free slot skips the swap chain (never blocks), the 30th starved one
  presents anyway. `pacing::CadenceGuard` (M3b review finding 6): `timer`/`free` on a refresh of k x the target
  shift their timer grid by half a refresh (alternating sign) when 6 of 60 on-screen durations miss k refreshes; a
  duration visible for its target period (VRR panel following the presents) counts as on time, and the guard
  stops after 4 shifts without a clean window (exit line: `timer cadence: J durations judged (F followed by the
  panel), H half-refresh shifts[ (guard stopped)]`). VRR probe: 2 s warm-up, a rejection is probed once more
  after 10 s (finding 13a).
- Refresh rate: `platform::QueryDisplayTiming(HMONITOR)` (`platform\pacer_display.h`: QueryDisplayConfig
  `targetVideoSignalInfo.vSyncFreq` as a rational, GDI fallback); queried again when `MonitorFromWindow`
  changes or the display epoch moves.
- Window (`platform\window.h`, additive): `platform::GetDisplayEnvironment()` `{displayOn, sessionLocked, epoch}`
  from `GUID_CONSOLE_DISPLAY_STATE` (RegisterPowerSettingNotification), WTS lock/unlock
  (WTSRegisterSessionNotification) and an epoch raised by WM_DISPLAYCHANGE, WM_DPICHANGED, the Alt+Enter
  toggle and those events; `platform::NoteDisplayChange()` (tests).
- RHI (add-only, defaults for other backends): `DeviceDesc::frameLatencyWaitable` (1..3, NiPCRenderer: 1),
  `DeviceInfo::tearingSupported/frameLatency`, `PresentDesc::allowTearing/skipSwapChain`, `PresentTiming`,
  `Device::FrameLatencyWaitable()`, `Device::LastPresentTiming()`. The D3D11 backend sets
  `DXGI_PRESENT_ALLOW_TEARING` only on request now (M3: for every sync interval 0).
- Log: `pacing: display ...`, `pacing: <old> -> <plan>: <reason>` (plans without presents say "no Present"),
  `pacing: VRR confirmed|rejected: ...`, `pacing: VRR probe repeated once ...`, `pacing: timer cadence: ... timer
  grid shifted by +-X ms (half a refresh)`, `pacing: timer cadence: still irregular after 4 shifts ...`; exit reporter "present": the three M3 lines keep their prefix
  (`pacing P Hz` = target rate, `schedule resets` = timer re-anchors + vblank wait timeouts, `S frames longer than
  1.5 periods (frame starts; N Present intervals incl. screenshots)` - since M3b phase 5 counted from the frame
  start intervals, finding 8), new lines `present: mode ...`, `present: display ...`, `present: frame start
  intervals ...`, `present: swap chain ...` (+ `timer cadence: J durations judged (F followed by the panel), H
  half-refresh shifts; V repeated VRR probes`), `present: on screen (DXGI frame statistics) ...` (+ `J jumps over P presents (I off the
  cadence)`, `measured refresh R Hz (fit over N samples, S segments)` = least-squares fit of SyncQPCTime over
  SyncRefreshCount, finding 9), `present: environment ...` (formats in `LogPresentStatistics`).
- `m3_check` M3b-7 (finding 7): foreground >= 95 % of the frames; with `pacing = auto|vsync`, vsync on and a
  refresh of k x the target also >= 95 % of the frames in the vblank plan and >= 99 % of the vblank-coupled frames
  exactly k refreshes on screen (`--min-foreground`, `--min-vblank-frames`, `--min-on-plan`).

### 4.21 Configuration schema, configuration location, tool modes - `core\config_schema.h`, `core\cli_tools.h` (M3c, host-CLI/schema agent)

Shared contract items 1-4 of M3c (Qt launcher `FuturamaLauncher.exe`; `docs\ROADMAP.md` M3c). Header comments of
`core\config_schema.h` and `core\cli_tools.h` are the reference.

- **One definition table** (`core\config_schema.cpp` `kKeys`, 55 rows in 10 sections in phase 1) replaces `kKnownKeys` of
  `core\config.cpp`. Per key: section, name, `core::ConfigKeyType` (bool, int, float, choice, string, path_file,
  path_dir, resolution), default in INI form, choices (`core::ConfigChoice {value, label{de,en}, hidden}`),
  range (`min/max/step`, enforced unless `kConfigRangeIsHint`), label and help text in German and English for
  players, UI tab (game|graphics|paths|advanced), order, widget (combo|check|spin|dspin|text|file|dir|resolution|
  monitor), flags `kConfigAdvanced`, `kConfigDeveloper` (test/diagnosis knob), `kConfigDynamicDefault`, file dialog
  filter. A `consteval` check keeps every section listed and its keys contiguous.
- **Validation** (`core::LoadConfig` -> `core::ValidateConfigValue`): unchanged rules for unknown keys/sections,
  booleans, numbers and choices (same error texts; `ConfigKeyChoices` = `core::ConfigChoiceList`, visible values
  first, e.g. `english|german|french|spanish|italian|japanese|korean|tchinese|portuguese|1..9`); new: float
  (`render_scale`, '.' only), resolution (`auto` or `<W>x<H>`, 160..16384 each), enforced ranges (`render_scale`
  1.0..2.0, `[kernel] min_guest_stack` 0x10000..0x4000000 - was a stage failure, now exit code 1), `[log] level`
  is a choice (`error|warn|info|debug|trace`, hidden `warning`). Ranges that the module clamps stay hints
  (`window_scale`, `fiber_stack`, `timeout`, `frame_rate`).
- **API for readers**: `core::FindConfigKey`, `IsConfigSection`, `ConfigKeys()`, `ConfigSections()`,
  `ConfigKeyDefault(section, key)` (default in INI form - use it instead of repeating literals), the shared value
  parsers `ParseConfigBool`, `ParseConfigU32`, `ParseConfigFloat`, `ParseConfigResolution` (`ConfigResolution
  {isAuto, width, height}`), `MatchesConfigChoice`. `core\config.cpp` uses the same parsers for `GetBool`/`GetU32`.
- **M3c [render] keys** (contract item 5, read by the renderer, `gfx\gfx_config.*`): `resolution = auto`,
  `render_scale = 1.0` (1.0..2.0, step 0.25), `msaa = 4` (off|2|4|8), `alpha_to_coverage = on` (bool),
  `anisotropy = 16` (off|2|4|8|16); the renderer added `screenshot_size = output` (output|internal) through the
  same table. "Classic" (M3 / xemu regression) = `resolution = 640x480`, `render_scale = 1`, `msaa = off`,
  `anisotropy = off`.
- **M3c integration: `[render] monitor = primary | <GDI device name>`** (type string, widget `monitor`, Graphics
  tab order 5; the values are `--query-gpu` `outputs[].device`, e.g. `\\.\DISPLAY2`). Read by `gfx\gfx_config.cpp`
  (`gfx::Settings::monitor`, empty = primary) and used by `platform::CreateMainWindow` through
  `platform::WindowOptions::monitor` / `platform::SelectMonitor(name)` (`MonitorChoice {monitor, primary, found,
  device}`): the window opens on that monitor with its work area and DPI (`GetDpiForMonitor`); a name that is not
  connected falls back to the primary monitor with a Core warning. The `window:` log line ends with
  `monitor <device> (primary)`.
- **Configuration location** (contract item 4, `core::ResolveConfigLocation(exeDir, explicitConfig)` ->
  `ConfigLocation {mode, portable, configFile, defaultUserdata}`, `Config::configMode/portable`): default
  `%LOCALAPPDATA%\FuturamaPC\futurama.ini` + userdata `%LOCALAPPDATA%\FuturamaPC`; portable (`portable.txt` next
  to futurama.exe) `<exe dir>\futurama.ini` + userdata `<exe dir>` (side by side like the default mode; design 4.1
  had `.\userdata`); `--config=<file>` always
  wins (userdata default by the portable rule). `[paths] userdata` overrides the default in every mode. A
  pre-M3c `<exe dir>\futurama.ini` without `portable.txt`/`--config` is ignored with a `config:` warning in the log.
  Log line `config    <file> (explicit|default|portable[, portable.txt])`. All checks pass `--config` (unchanged).
- **Tool modes** (`core::cli::RunToolIfRequested`, called first in `core::RunHost`: after the launcher stub's TLS/
  placeholder checks and `LoadLibrary`, before config, log, crash handlers and XBE mapping; never write user data):

| Command line | Output (UTF-8 JSON, `-` = stdout) | Exit code |
|---|---|---|
| `--print-config-schema=<file.json>` | `{"schema_version":1,"build","config":{file_name, portable_marker, mode, file, default_userdata},"sections":[{"name","label","keys":[{"name","type","default","choices","min","max","step","label","help","advanced","restart_required","ui":{"tab","order","widget"}}]}]}` + per key `developer`, `dynamic_default`, `hidden_choices`, `range_enforced`, `placeholder`, `ui.filter` | 0 |
| `--check-iso=<image> --result=<file.json>` | `{"ok","reason","xdvdfs","layout":"xiso\|redump\|unknown","xbe_sha1","version":"retail_eu\|proto_ntsc\|unknown","supported","title","size"}` + `reason_code` (supported, not_found, not_a_file, unreadable, no_xdvdfs, corrupt, no_default_xbe, bad_xbe, prototype, other_version, other_game), `message{de,en}`, `partition_offset`, `xbe_size`, `title_id` | 0 supported, 2 not supported |
| `--query-gpu=<file.json>` | `{"adapters":[{"name","vendor_id","dedicated_mb"}],"outputs":[{"name","device","primary","rect","refresh":{"num","den"},"modes"}],"msaa","max_anisotropy","tearing","vrr_hint"}` + per adapter `device_id`, `dedicated_mb_source` (d3dkmt = exact 64-bit), `software`, `feature_level`, `msaa`, `max_anisotropy`; per output `adapter`, `hz`, `refresh_source`, `dpi`, `attached`; `default_adapter`, `vrr{possible, windows_setting, hint{de,en}}` | 0 |
| `--print-paths=<file.json> [--config=<file>]` (extension) | `{"ok","error","mode","portable","exe_dir","config","config_exists","userdata","userdata_configured","logs","hdd","saves","screenshots","cache","warnings"}` | 0 |

  Usage errors (two tools, a foreign option, missing file name, `--result` without `--check-iso`, output not
  writable): exit 1, one `futurama: ...` line on stderr, never a message box. `--config=` may accompany any tool.
  Files are written to `<file>.tmp` and renamed. `--check-iso` reads `default.xbe` through `vfs::XdvdfsImage`
  (XISO and redump/XGD2/XGD3 offsets), SHA-1 via `krnl::xc::Sha1`: retail EU = `guest::kXbeSha1`, NTSC prototype
  = `core::cli::kProtoNtscXbeSha1` (`f443d255...`); title/title id from the XBE certificate. `--query-gpu`:
  DXGI 1.1 adapters (dedicated memory via `D3DKMTQueryAdapterInfo(GETSEGMENTSIZE)`, because the SIZE_T of DXGI
  saturates in the 32-bit process), outputs with the refresh fraction of `platform::QueryDisplayTiming`
  (QueryDisplayConfig), unique `GetDisplayModeList` sizes, effective DPI; MSAA = sample counts with quality > 0 and
  multisample render target + resolve support for B8G8R8A8/R8G8B8A8 and D24S8 on a D3D11 device (FL 10_0+) of the
  adapter; top-level `msaa`/`max_anisotropy` = adapter 0 (the one `D3D11CreateDevice(nullptr, HARDWARE)` takes);
  `vrr_hint` possible|unavailable|unknown from `DXGI_FEATURE_PRESENT_ALLOW_TEARING`. Measured (dev machine, Debug):
  schema 27 ms, GPU query 0.27 s (4 adapters, D3D11 device each), ISO check 40 ms (retail ISO and prototype `.bin`,
  both XISO layout).
- **Tests**: `tests\schema_tests.cpp` (row metadata, contract keys, LoadConfig accepts every row, source scan:
  every `"section", "key"` read in `src\host` has a row and every row is read, value validation, defaults vs.
  `gfx::Settings`/`krnl::KernelSettings`/`xapi::kDefaultTickRate`, JSON document), `tests\cli_tests.cpp` (JSON
  writer, usage errors, schema file with spaces/commas/parentheses in the path, `--check-iso` on the real retail
  ISO and prototype `.bin` (read-only, `FUT_TEST_ISO` / `FUT_TEST_PROTO_ISO`), missing/directory/random/empty
  files, synthetic XISO and sparse redump images with fake and real XBEs, `--query-gpu`, configuration location
  with a temporary `%LOCALAPPDATA%`, `--print-paths`, end-to-end runs of the built `futurama.exe`),
  `tests\cli_json.h` (strict JSON reader for the tests).

### 4.22 Render resolution and anti-aliasing: `[render] resolution / render_scale / msaa / ...` - `gfx\nipc_gpu.h` (M3c, renderer agent)

Reference: `src\host\gfx\GFX_CONTRACT.md` section 12 (configuration, coordinate rule, back buffers, ff options, log
lines, evidence). Summary:

- Keys (schema rows in `core\config_schema.cpp`, parsed by `gfx\gfx_config.*`): `resolution = auto | <W>x<H>`
  (default auto = the largest 4:3 rectangle inside the window's view), `render_scale = 1.0..2.0` (1.0),
  `msaa = off | 2 | 4 | 8` (4, clamped to the device), `alpha_to_coverage` (on), `anisotropy = off | 2 | 4 | 8 | 16`
  (16), new `screenshot_size = output | internal` (output), `linear_aa` (off; M3c review finding 3: resolve and
  downsample on linear light). `gfx::Settings::resolutionWidth/Height, renderScale, msaa, alphaToCoverage,
  anisotropy, screenshotSize, linearAa`, defaults `gfx::kDefault*` (= the schema defaults),
  `gfx::ParseRenderResolution / ParseRenderScale / ParseMsaa / ParseAnisotropy` (on top of the schema's value
  parsers). Classic (= M3 bit for bit, the xemu regression): `resolution = 640x480, render_scale = 1, msaa = off,
  anisotropy = off`.
- `gfx::RenderQuality` (+ `RenderQualityOf(settings)`) in `GpuRenderer::CreateParams::quality` (default: classic),
  `CreateParams::viewWidth/viewHeight`; `gfx::RenderSizes` + pure `ComputeRenderSizes`, `ScaleViewport`, `ScaleRect`,
  `kResizeSettleFrames`; `GpuRenderer::Sizes()`, `SampleCount()`, `ResizeForView(w, h)`. The guest keeps its 640x480
  coordinates; only the D3D viewport and the clear rectangles are scaled to the internal size; resolution auto
  follows window resizes after 12 frames.
- Review M3c (findings 2, 3): `RenderQuality::linearAa`; pure `gfx::QualityFallbacks(quality)` -> `QualityLadder`
  (request, render scale 1, MSAA halved down to off, 640x480 without MSAA) and `DescribeQuality`; `GpuRenderer::
  Create` tries the steps while the back buffers cannot be created, `GpuRenderer::Quality()` / `QualityReduced()`;
  the view-following resize steps down too. **Behaviour change:** when even the last step fails,
  `NiXBoxRenderer::Create` ends the game with `core::Fatal` (exit code 3, "Direct3D 11 could not be started") instead
  of the invisible null renderer (headless stays null).
- RHI (add-only): `DeviceDesc::sampleCount / outputWidth / outputHeight / linearResolve`, `DeviceInfo::sampleCount /
  outputWidth / outputHeight / linearResolve`, `BlendDesc::alphaToCoverage`, `RasterDesc::multisample`, `DeviceStats::gpuFramesTimed /
  gpuFrameMsAverage / gpuFrameMsP99 / gpuFrameMsMax`, virtual `Device::CaptureOutput`, `SupportedSampleCount`,
  `ResizeBackBuffers` (defaults for other backends). D3D11: multisampled persistent back buffers + depth, resolve,
  `ResamplePS` downsample (box for integer ratios, Catmull-Rom with a 2x2 anti-ringing clamp otherwise - review
  finding 1; Mitchell-Netravali before), with `linearResolve` TYPELESS targets + sRGB views, present pass 1:1 / scaling /
  downsample, GPU timestamp span per frame; Present flushes the context when it skips the swap chain (before
  M3c nothing submitted such frames until the driver's next implicit flush: frame work p99 15-55 ms with the display
  off).
- `ff::RenderOptions {sampleCount, alphaToCoverage, maxAnisotropy, sharpNearest}` + `FixedFunctionPipeline::
  SetRenderOptions / GetRenderOptions`, `ff::MakeSamplerDesc(stage, maxAnisotropy, screenSpace)`,
  `ff::UsesAlphaToCoverage`, `ff::UsesSharpNearest`, `PipelineStats::alphaToCoverageDraws / alphaTestDiscardDraws /
  anisotropicStages / sharpNearestStages`; review M3c: anisotropy only with linear magnification too (finding 4),
  `ff::ReadsDestinationAlpha(blend)` + `PipelineStats::destAlphaBlendDraws / alphaToCoverageSuspended` (finding 5:
  the first blend that reads the destination alpha suspends alpha-to-coverage); shader layout: `FFDrawConstants::alphaTest.w` = alpha-to-coverage,
  `FFStage::coord.w` flags `FF_STAGE_TEXTURE | FF_STAGE_SHARP_NEAREST`.
- Screenshots: `platform::ScreenshotSource {Internal, Output}`, `platform::SaveScreenshot(device, dir, frame,
  source)` (capture on the render thread, PNG on a worker thread, exit reporter "screenshots" flushes),
  `platform::FlushScreenshots(timeoutMs)`; the M3 overload without a source is unchanged (synchronous).
- Logs: `render: output WxH (resolution ...), internal WxH (...), MSAA Nx (requested Mx; supported: ...), ...`,
  `render: view resized to ...`, exit `render totals: ...`, `d3d11 gpu frame time: ...`, `screenshots: N written,
  M failed`; the device line names samples and output size.
- Check: `python tools\m3c_check.py --exe <futurama.exe> --config <base INI> [--variants classic,hq,hq_noaa,ref,ss,max,frac]
  [--fresh]` - per variant exit/frame limit, paced rate + frame work, debug layer, render configuration (downsample
  none/box/catmull-rom, resolve gamma/linear light, no step down), screenshot
  sizes, every draw + every M2 criterion; classic = the full m3_check incl. xemu; hq downscaled vs xemu (X1); edge
  aliasing MSAA vs no AA against a render_scale 2 + 8x reference (X2). Base config of the renderer agent:
  `run\m3c-renderer\check\futurama.ini`. Tests: `tests\aa_tests.cpp`.

### 4.23 Launcher: `FuturamaLauncher.exe` (Qt 6, x64) - `launcher\**` (M3c, launcher agent)

Separate CMake project (64 bit, Qt 6.8.3 Widgets, MSVC `/W4`, `-DLAUNCHER_WERROR=ON` for `/WX`), not part of the x86
build. Details (German): `launcher\README.md`.

- Build: `cmd /c "D:\FuturamaDecomp\build_launcher.cmd" [builddir] [Debug|Release] [test]` (vcvars64,
  `CMAKE_PREFIX_PATH` = `FUT_QT_DIR` (default `D:\Qt\6.8.3\msvc2022_64`), Ninja, windeployqt into `<builddir>\bin` incl.
  the offscreen platform plugin; Release adds the app-local MSVC runtime). `test` runs ctest: `launcher_tests` (INI round
  trip, schema, validation, paths, CLI, sessions, translations), `launcher_ui_tests` (main window + wizard, offscreen),
  `launcher_screenshots`.
- Uses only the shared contract: `futurama.exe --print-config-schema=`, `--query-gpu=`, `--check-iso= --result=`
  (results cached per build in `launcher_cache\` next to the INI), starts the game with `--config=<ini>` (stdout/stderr
  piped -> no host message boxes; startup errors appear in the launcher's result panel), reads
  `<userdata>\logs\futurama.log`. Optional schema fields it understands: `hidden_choices`, `range_enforced`, `developer`,
  `dynamic_default`, `placeholder`, `ui.filter`; check-iso `message`/`reason_code`; query-gpu `default_adapter`,
  `vrr.hint`. A key with `ui.widget` = `monitor` is shown as a monitor picker (output device names from query-gpu; the
  host has no such key yet). Unknown fields are ignored.
- INI rules = host rules (`core\config.cpp` ReadIni): the launcher writes only what the host reads back unchanged
  (quoting when needed), keeps comments/order/unknown entries, writes atomically (QSaveFile) and reports what the host
  would reject (unknown keys in known sections, broken lines) before "Spielen".
- Automation: `--set section.key=value` (repeatable, schema-validated) `--save`, `--launch [--wait] [--result=<json>]`
  (exit code = the game's; launcher errors 64..70), `--screenshots=<dir>` (all tabs + wizard pages, de/en; with
  `-platform offscreen`), `--schema-file=` / `--gpu-file=` (development data). Test runs: `--config=run\<name>\...`
  keeps everything (INI, `launcher.ini`, cache) out of `%LOCALAPPDATA%`.
- Test double `<launcher build>\fakegame\futurama.exe` (`launcher\fakegame\fake_futurama.cpp`): canned schema/GPU
  data, superficial ISO check, fake game run; never packaged (`tools\package.py` rejects a loader < 3 MB).
- Package: `python tools\package.py --game-build <x86 build> --launcher-build <launcher Release build> [--portable]
  [--zip]` -> `dist\FuturamaPC\` (git-ignored) with an allow-list of files, German `LIESMICH.txt`, checks against game
  data and Debug builds.
- M3c integration: the "Spiel" tab shows the configured image's `--check-iso` verdict (`ui\iso_status.h`
  `IsoStatusBox`, checked in the background on every change; the wizard shares `IsoVerdictHtml`/`IsoDetailLines`);
  the graphics tab names what `resolution = auto` renders (`FitAspect43`, `EstimateWindowSize` in
  `ui\screen_info.h`: the window size at the monitor's `dpi` of `--query-gpu`, or the full-height 4:3 area in
  fullscreen), offers only 4:3 sizes, shows the window size in pixels and disables `alpha_to_coverage` without MSAA;
  numbers follow the UI language (`QLocale::setDefault` in `fl::SetLanguage`).
- M3c review fixes: `GameRunResult::cause()` (the host's `futurama: ...` line, never its `log file:` line; JSON field
  `cause`); exit code 6 = `fl::kGameExitAlreadyRunning` (category `already_running`, the log of the other game is not
  shown); `fl::CleanPathInput` strips one pair of enclosing quotes from every path input (wizard, path editors,
  `--set` of path keys, `SettingsModel::setValue`), a quoted path in the INI is an error (`IssueCode::QuotedPath`);
  `IniDocument::Problem::code` + `IniDocument::ProblemText` (localized), `hasInvalidUtf8()`, localized save errors with
  a hint, `<ini>.bak` before a save of a file with invalid UTF-8 (`SettingsModel::lastBackup`); new keys never after a
  broken line; presets: Maximum = render scale 2 + MSAA 8x + `linear_aa`, both presets step down by the video memory of
  `--query-gpu` (`BackBufferMemoryMb`, `FitsVideoMemory`, `PresetOutputSize`, `PresetValues::videoMemoryMb /
  limitedByMemory`); `EditorContext::dynamicDefault` (frame_rate shows the logic rate); "Spielen" asks before an image
  whose check said unsupported / not Xbox (`IsoStatusBox::currentResult`); the main window fits 90 % of the screen's
  available area. Automation: unexpected positional arguments = exit code 64 (ctest `launcher_cli_positional`), an INI
  with invalid UTF-8 is not overwritten (68), one launcher per INI (`QLockFile` `launcher_cache\<ini>.lock`; GUI: the
  first window comes to the front, automation: exit code 70). Package: `LICENSES\Qt\` (LGPL-3.0, GPL-3.0 from
  `launcher\licenses\`, Qt source note).

## 5. Cross-module contracts

| Contract | Provider -> user |
|---|---|
| `core::AdoptCurrentThread(TlsDataSize, name)` at the start of every guest thread | core -> kernel (`PsCreateSystemThreadEx`) |
| IRQL lives in TLS slot `FUT_TLS_S_IRQL` (`core::Get/SetGuestSlot`) | core -> kernel |
| `core::Fatal`, `core::LogBacktrace` for `KeBugCheck` | core -> kernel |
| `core::XbeFileBytes()` (verified XBE file) e.g. for `XeLoadSection` checks | core -> kernel |
| `vfs::ReadIsoFile` for `default.xbe` when `[paths] xbe` is not set | io -> core |
| `vfs::CreateHostDirDevice/AddDevice/CreateSymbolicLink` for the `XMountUtilityDrive` hook (Z:) | io -> core |
| `krnl::HandleTable` + `krnl::KObject` for files, threads, links | shared -> kernel, io |
| `NtUserIoApcDispatcher` address via `krnl::GetBinding(krnl::exp::NtUserIoApcDispatcher::slot)` | kernel -> io |
| APC queue for `NtReadFile`/`NtWriteFile` (M2): delivered in `KeDelayExecutionThread` / `NtWaitForSingleObjectEx` | kernel <-> io (agree via Changelog) |
| `core::AdoptCurrentThreadWith(kthread, tlsData, tlsDataSize, name)` - same as `AdoptCurrentThread`, but with caller-owned zeroed KTHREAD/TlsData (no allocation) | core -> kernel |
| `core::ExitHostProcess(code, reason)` ends the process from any thread (log + flush + TerminateProcess); `core::kExitGuestShutdown` (4) for `HalReturnToFirmware` | core -> kernel |
| `core/guest_call.h`: `core::CallGuest(addr, {args})`, `CallGuest64`, `CallGuestThiscall`, `CallGuestFastcall`, raw `fut_guest_call*` (MASM, `asm\guest_call.asm`): host -> guest calls with ebx/esi/edi/ebp preserved and esp restored (works for stdcall and cdecl, e.g. `SystemRoutine(StartContext1, StartContext2)`) | core -> kernel, all |
| `core::AddCrashReporter(name, fn)` - fn runs in the fatal hook (after backtrace, before minidump), e.g. kernel trace ring buffer (design 5.4) | core -> kernel, io |
| `core::LogGuestStackScan(esp)`, `core::LogGuestExceptionRing()`, `core::LogAllThreads(reason)`, `core::WriteMiniDump(ep, tag)` (crash.h) | core -> all |
| `core::ReadOriginalXbeBytes(va, out, n)` / `core::XbeFileOffset(va, off)` - original (unpatched) image bytes, e.g. for XeLoadSection | core -> kernel |
| `platform::RequestQuit(exitCode)` makes the boot thread's message loop return; the loop also ends (code 4) when `krnl::GuestThreadsExitedEvent()` is signaled | core <- kernel, all |
| `core::AddExitReporter(name, fn)` - statistics logged by `ExitHostProcess` at every orderly end (not after a watchdog timeout), sorted by name (section 4.16) | core -> all (M2 integration) |
| `core::ExitInProgress()`; a second `ExitHostProcess(kExitWatchdog, ..)` during a hung exit terminates at once (M2 review finding 7) | core -> platform |
| `core::InSelfTest()` - activity of the self-tests is reported apart (fiber switches, `[self-test]` in `vfs_trace.log`) | core -> xapi, trace (M2 review) |

## 6. Conventions (design 5.3)

- Namespaces `core::`, `krnl::`, `vfs::`, `xapi::`, `guest::`, `platform::` (later `gfx:: snd:: vid:: inp:: game::`).
- Every HLE function/hook has a header comment: ordinal/slot or address, convention, args, evidence
  (report + section or address). Guesses are marked `// ASSUMPTION:`.
- HLE and hook entries are `noexcept`; code that can throw is wrapped in `FUT_HLE_TRY/FUT_HLE_CATCH`.
- No inline assembly (MASM files in `src\host\asm\`), no bare guest addresses, guest-visible structs are
  POD with `static_assert(sizeof/offsetof)`, no STL types in guest-visible structs.
- Memory handed to the guest must lie below 0x80000000 (assert).

## 7. Known skeleton limitations

- All HostMain stages except config/log, hooks and the self-test runner are stubs; guest code does not run yet.
  -> phase 2 (core): all core stages are implemented (crash handlers, LoadXbe, MapXbe, polyglot, thunks,
  fs patches, XAPI hooks, self-tests, boot thread + entry, message loop with watchdog).
- `vfs_stubs.cpp` returns "not implemented" for everything.
  -> phase 2 (io): deleted; namespace, XDVDFS reader, IsoDevice, HostDirDevice and the file kernel exports
  are implemented (`vfs\*.cpp`, `kernel\io_*.cpp`, tests in `tests\vfs_tests.cpp`).
- The MMIO trap reservation (design 1.2 debug option) is not part of the launcher; the host can reserve
  0xFD000000..0xFEFFFFFF later if needed.
  -> phase 2 (core): reserved by the host in stage InstallCrashHandlers (`[debug] mmio_trap`, default 1);
  the VEH logs guest accesses to it as "MMIO trap".

## 8. Changelog

Every change to a shared file is listed here: date, agent, file, what was added.

| Date | Agent | File | Change |
|---|---|---|---|
| 2026-10-04 | skeleton | all | initial version |
| 2026-10-04 | io | (claim, no shared edit) | io agent registers these kernel slots in `kernel\io_*.cpp` - please do not define them elsewhere: 22 NtSetInformationFile, 25 NtCreateFile, 34 NtReadFile, 35 NtWriteFile, 36 NtQueryInformationFile, 37 NtQueryVolumeInformationFile, 38 NtFlushBuffersFile, 40 NtFsControlFile, 41 NtDeviceIoControlFile, 43 NtOpenFile, 45 NtQueryFullAttributesFile, 46 NtQueryDirectoryFile, 48 IoCreateSymbolicLink. Not taken by io: 39 NtUserIoApcDispatcher (kernel), 49/50 Nt*SymbolicLinkObject (M7). |
| 2026-10-04 | io | `third_party\doctest\doctest.h`, `tests\test_main.cpp` (new files) | doctest 2.4.11 (MIT) single header; `tests\test_main.cpp` is the ONLY file with `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` - other test files just `#include "doctest.h"`. |
| 2026-10-04 | core | `src\host\core\guest.h` | added constants `kXbeCodeBegin`/`kXbeCodeEnd` (XBE code range 0x11000..0x293560, crash stack scan), `kXapiGetLastError` 0xF6E54, `kXapiSetLastError` 0xF6E7C, `kXapiGetCurrentThreadId` 0xF9AE0 (AdoptedThread self-tests), `kMmioTrapBase` 0xFD000000 / `kMmioTrapSize` 0x02000000 |
| 2026-10-04 | core | (core-owned APIs, listed for the other agents) | `core\guest_thread.h`: `AdoptCurrentThreadWith`; KTHREAD.StackLimit is now the lowest address of the stack reservation. `core\host_main.h`: `ExitHostProcess`, exit codes `kExitGuestShutdown` 4, `kExitWatchdog` 5. New `core\guest_call.h` + `asm\guest_call.asm`. `core\crash.h`: `LogGuestStackScan`, `LogGuestExceptionRing`, `LogAllThreads`, `WriteMiniDump`, `RecentGuestExceptions`, `GuestExceptionCount`, `AddCrashReporter`. `core\xbe_loader.h`: `XbeFileOffset`, `ReadOriginalXbeBytes`. `core\hooks.h`: `InstalledHookList`; hooks now also require the 5 target bytes to equal the XBE file. `core\fs_patcher.h`: `FsPatchDisplacement`. `platform\platform.h`: `RequestQuit`. New tests `tests\test_core.cpp`. |
| 2026-10-04 | io | `src\host\vfs\vfs.h` | added `XdvdfsImage::VolumeTimestamp()`, `PartitionSize()`, `EntryCount()`; free functions `vfs::MatchFileMask(name, mask)` (NtQueryDirectoryFile masks) and `vfs::ResetNamespace()` (unit tests only). `vfs\vfs_stubs.cpp` deleted (real implementation in `vfs\*.cpp`). |
| 2026-10-04 | io | (io-owned APIs, listed for the other agents) | **APC contract (design 1.7)**: `kernel\io_apc.h` - NtReadFile/NtWriteFile with an ApcRoutine do the I/O synchronously, return STATUS_PENDING and queue {ApcRoutine, ApcContext, IoStatusBlock, status, information} for the calling thread. The kernel agent's alertable waits (`KeDelayExecutionThread`/`NtWaitForSingleObjectEx` with Alertable = TRUE) must call `krnl::io::DeliverIoApcs()` and return STATUS_USER_APC (0xC0) if it returned > 0; delivery writes the IoStatusBlock, then calls `ApcRoutine(ApcContext, IoStatusBlock, 0)` (= kernel's NtUserIoApcDispatcher; verified end to end in `tests\vfs_tests.cpp`). Also `PendingIoApcCount()`, `GetIoApcStats()` (M2 exit 5). `kernel\io_exports.h` declares the io kernel exports (callable from host code/tests), `kernel\io_file_object.h` the guest file object (`krnl::io::LookupFile`). New config key `[paths] disc_dir` = extracted disc as read-only `\Device\CdRom0` when no `iso` is set (design 4.1 DirDevice). Without iso/disc_dir, CdRom0 is a device without files. Tests: `tests\vfs_tests.cpp` (ISO via `%FUT_TEST_ISO%`, temp dir `%FUT_TEST_TMP%`, both with defaults). |
| 2026-10-04 | kernel | (claim + kernel-owned APIs, no shared edit) | **Slots registered by the kernel agent** (`kernel\*.cpp` except `io_*`; please do not define them elsewhere): 0 KfLowerIrql, 1 KeRaiseIrqlToDpcLevel, 3 RtlInitAnsiString, 4 KeSetEvent, 5 KeWaitForSingleObject, 6 KeInitializeTimerEx, 7 KeInitializeDpc, 8 IoInvalidDeviceRequest (ADDR), 9/10/14 Rtl*CriticalSection, 11/12/15 Nt*VirtualMemory, 13 RtlCompareMemoryUlong, 16 RtlRaiseException + 105 RtlUnwind (slot = ntdll's own function), 17 ExQueryNonVolatileSetting, 18/30/31/32/47/53/64/73/75/87/110 (all DATA), 20 NtWaitForSingleObjectEx, 21 KeDelayExecutionThread, 23 KeQuerySystemTime, 24 RtlNtStatusToDosError, 27-29 XcSHA*, 33 NtWaitForSingleObject, 39 NtUserIoApcDispatcher (ADDR), 42/44 Mm*SystemMemory, 51/52/54/56/57 Mm*ContiguousMemory*/MmQueryAllocationSize, 55 HalReturnToFirmware, 58/59 Mm*AddressProtect, 60 KeSetTimer, 61 KeCancelTimer, 72 XcVerifyPKCS1Signature, 74 HalRegisterShutdownNotification, 76 PsTerminateSystemThread, 77 PsCreateSystemThreadEx, 78 HalInitiateShutdown, 80 KfRaiseIrql, 84 RtlEqualString, 85/86 XeLoad/UnloadSection, 98 DbgPrint, 106 KeBugCheck, 109 DbgBreakPoint (62 slots). Still generated stubs: 49/50 (M7), 107/108 MmCreate/DeleteKernelStack (M2), the replaced-library slots. **APIs**: `kernel\kernel_hle.h`: `krnl::NtStatusToDosError` (= guest RtlNtStatusToDosError), `TimeoutToMilliseconds`, `CurrentIrql`, `GuestThreadsExitedEvent()` (manual-reset event, signaled when the last PsCreateSystemThreadEx thread called PsTerminateSystemThread - the process does NOT end by itself; the core message loop may wait on it), `LiveGuestThreadCount`, `OnXbeMapped()` (idempotent; derives `[saves] key_mode = xbox` keys from the mapped certificate; PsCreateSystemThreadEx calls it), `NtUserIoApcDispatcher`. `kernel\kernel_exports.h` declares every kernel-agent export (callable from host code/tests); `kernel\ke_thread.h` `krnl::ThreadObject` (guest thread handle object: `HostHandle`, `ThreadId`, `KThread`, `ExitStatus`; owns fake KTHREAD + TlsData, adopted with `core::AdoptCurrentThreadWith`, SystemRoutine called via `core::CallGuest`). **APC contract implemented** (`kernel\ke_wait.cpp`): alertable KeDelayExecutionThread / NtWaitForSingleObject(Ex) call `io::DeliverIoApcs()` before and after waiting and return STATUS_USER_APC if it delivered; waits on file handles return at once (synchronous I/O), thread handles wait on the host thread. **Exit paths**: HalReturnToFirmware -> `core::ExitHostProcess(kExitGuestShutdown)` (routine 4 -> core::Fatal); KeBugCheck -> `core::LogBacktrace` + `core::Fatal`. **Config keys** (`kernel\kernel_settings.h`): `[settings] language` (english\|german\|french\|spanish\|italian\|... or 1..9, default english), `game_region` (4), `av_region` (0x00800300), `video_flags`, `audio_flags` (0), `av_pack` (6); `[saves] key_mode` (fixed\|xbox), `signature_key`, `xbox_cert_key`; `[kernel] min_guest_stack` (0x200000). Self-tests `KernelStatusTableMatchesNtdll`, `KernelSha1KnownAnswer`, `KernelSlotsRegistered` (Host), `KernelIrqlRoundTrip` (AdoptedThread); unit tests `tests\kernel_tests.cpp`. |
| 2026-10-04 | integration (phase 3) | `src\host\core\hooks.h/.cpp`, `src\host\core\selftest_m1.cpp`, new `src\host\xapi\boot_probes.cpp` | Probe hooks: `HookDesc::trampolineBytes` (default 0), `InstalledHook::trampoline/trampolineBytes`, `core::HookTrampoline(addr)`, macro `FUT_PROBE(addr, fn, conv, name, prologueHex, stolenBytes)` (section 4.9). Self-test `HooksInstalled` also verifies the trampolines. Six M1 boot probes (`probe:*`, default on) make exit criteria 2 and 4 visible on log level info: `boot: XapiInitProcess start/done`, `_rtinit`, `_cinit`, `game memory pool ... 0x2600000 bytes (38 MiB)`, `XGetDiskClusterSizeA("U:\") = 0x4000`, `XapiBootToDash(...)` (warn). |
| 2026-10-04 | integration (phase 3) | `src\host\core\guest.h` | added `kXapiXGetDiskClusterSizeA` 0xF8081, `kGameSaveClusterSizeVar` 0x2EDB0C, `kGameMemoryPool` 0x2E7D60 (+ `kGameMemoryPoolBeginOffset` 0x0C / `kGameMemoryPoolEndOffset` 0x10), `kGameMemoryPoolInit` 0xA4920, `kGameMemoryPoolSize` 0x2600000 (evidence in the header). |
| 2026-10-04 | integration (phase 3) | `src\shared\nt_status.h` | `NtStatusName` knows `STATUS_INSUFFICIENT_RESOURCES`, `STATUS_MEDIA_WRITE_PROTECTED`, `STATUS_CANNOT_DELETE` (io agent's open point). |
| 2026-10-04 | integration (phase 3) | `src\host\xapi\xapi_hooks.cpp`, `src\host\kernel\ke_timer.cpp` | formatting only: blocks mangled by clang-format (`FUT_HLE_TRY{` with stray `// namespace` comments) rewritten as `try { ... } FUT_HLE_CATCH(...)` (same expansion). |
| 2026-10-04 | integration (phase 3) | `src\INTERFACES.md` | fixed the escaped paths in the core Changelog row (`core\xbe_loader.h`, `core\fs_patcher.h`); M1 exit check `tools\m1_check.py` (section 1), probe hooks (section 4.9). New tool `tools\m1_check.py`, test `tests\test_core.cpp` case "M1 boot probe targets". |
| 2026-10-05 | phase 5 (review fixes) | `kernel\kernel_api.h/.cpp`, `tools\gen\gen_kernel_table.py`, `kernel\gen\*` | `gen::Ret` + `ExportInfo::ret/stubFatal`, `krnl::exp::<Name>::ret`, `krnl::detail::UnimplementedResult(ret)`; generated stubs return 0 for non-NTSTATUS exports, 107/108 abort (finding 1). |
| 2026-10-05 | phase 5 (review fixes) | `core\config.h/.cpp`, `core\log.h/.cpp`, `core\hooks.h/.cpp`, `core\host_main.cpp`, `core\selftest.cpp`, `core\crash.h` | INI inline comments, `kKnownKeys` validation, `Config::warnings`, `core::IsKnownConfigKey`, `core::ProcessLongPathsEnabled`, `kMaxUserdataWithoutLongPaths`; `core::LogLastError()`; stage failures and `core::Fatal` are reported on stderr / message box with the log path; `core::HookDisableEntryMatches`, `ParseHookAddressEntry`, `UnmatchedHookDisableEntries`; self-test phases run Host then Mapped; `core::ExceptionCode(EXCEPTION_*)` (DWORD form for clang). |
| 2026-10-05 | phase 5 (review fixes) | `vfs\vfs.h`, `vfs\host_dir_device.cpp`, `vfs\vfs_init.cpp`, `launcher\futurama.manifest` | `vfs::CreateHostDirectories(dir, error)` (`\\?\` form, no MAX_PATH limit); launcher manifest `longPathAware`. |
| 2026-10-05 | phase 5 (review fixes) | `CMakeLists.txt`, `CMakePresets.json`, `build.cmd`, `tests\CMakeLists.txt`, `tools\m1_check.py` | `enable_testing()` at the top level, `FUT_TEST_CONFIG` -> tests `fut_selftest`, `fut_m1_check`; preset `x86-clang-debug`, `build.cmd ... clang`, `FUT_CMAKE_ARGS`; m1_check passes `--selftest --stop-at=main`, fails on a missing/empty log and on a userdata mismatch. |
| 2026-10-05 | io (M2 phase 1) | `core\log.h/.cpp` | `core::LogTap` + `core::SetLogTap(tap)`: observer of every line that passes the level check (called outside the log lock, before the write; one tap per process). Used by the kernel trace ring of `core\trace.h`. No behaviour change without a tap. |
| 2026-10-05 | io (M2 phase 1) | new `core\trace.h/.cpp` (io-owned), `vfs\vfs_init.*` | **Tracing (design 5.4)**, started by `vfs::StageInitVfs` from `[debug] trace` / `--trace=`: `krnl` (or `all`) = ring buffer of the last 1024 KRNL log lines (with `--trace=krnl` the channel runs at trace, so every HLE call that logs its decoded arguments), dumped by the crash reporter "kernel trace ring (--trace=krnl)" (`core::AddCrashReporter`, lines `krnl\| ...` on CORE/error after the backtrace). `vfs` (or `all`) = `<userdata>\logs\vfs_trace.log` (truncated per run, unbuffered): one line per open (`NtCreateFile`/`NtOpenFile`), `NtQueryFullAttributesFile` and symbolic link: `[sec] tid api "guest path" -> device path : hit <FILE_OPENED/...> h=.. \| missing <status> (documented: <reason>) \| missing <status> UNDOCUMENTED \| error <status>`. Documented misses: table in `vfs\vfs_known_missing.cpp` (`vfs::detail::DocumentedMissReason`; FutPAL05.img, sysNTSC/FutNTSC*, Video\*.txt, fonts/kanjii*, arial2t20px32x32.*, MusicClip.wav, Mine_Activate.wav, LipsyncMeshes, NiTCache.dat, partition1 `UDATA\46490002\*` before the first save). M2 exit 7: `grep UNDOCUMENTED vfs_trace.log` must be empty (or the miss analysed and added to the table). API: `core::trace::Init/Configure/OptionsFromConfig`, `KrnlEnabled/VfsEnabled`, `RecordKrnl`, `KrnlRingSnapshot`, `KrnlRecordCount`, `DumpKrnlRing`, `VfsLine`, `VfsTracePath`, `VfsLineCount`. NtReadFile/NtWriteFile/NtFlushBuffersFile/NtQueryDirectoryFile per-call lines moved from VFS/trace to KRNL/trace (decoded handle, path, offset, APC, result). |
| 2026-10-05 | io (M2 phase 1) | `kernel\ke_wait.*` (moved from the kernel agent to io for M2, as assigned), `kernel\io_apc.*`, `kernel\io_common.*`, `kernel\io_file.cpp`, `kernel\io_info.cpp`, `kernel\io_control.cpp` | **APC contract (supersedes the 2026-10-04 io/kernel rows)**: delivery writes the IoStatusBlock (= OVERLAPPED) and, for ApcRoutine == NtUserIoApcDispatcher, calls the completion directly: `Completion(dwErr = NT_ERROR ? RtlNtStatusToDosError : 0, Information, lpOverlapped)` through `core::CallGuest` (stdcall ret 0xC verified at 0x13F1D0/0xE01A0; esp restored); other routines `routine(ctx, iosb, 0)`. User APCs are delivered only by `KeDelayExecutionThread` / `NtWaitForSingleObjectEx` with **WaitMode == UserMode (1) and Alertable** (XAPI SleepEx/WaitForSingleObjectEx push 1); `NtWaitForSingleObject` never delivers (ASSUMPTION: Xbox forwards with KernelMode). New `kernel\ke_wait.h`: `krnl::kKernelMode/kUserMode`, `DeliversUserApcs`, `krnl::EventObject` (handle type "Event", Notification/Synchronization), `CreateEventHandle`, `LookupEvent`; waits on event handles. `io_apc.h`: `PendingIoApcCountAllThreads`, `NoteFailedIoApcRequest`, `IoApcStats{queued, delivered, errorCompletions, failedRequests, maxPending}`, `LogIoApcStats(reason)`; `IoApcStats::deliveredXbe` counts completions inside the XBE image; the first one logs `I/O APC: first XBE completion delivered on thread N: 0x0013F1D0 ...(err 0, bytes ..)` (KRNL/info) - **M2 exit 5 evidence** (host completions of self-/unit tests do not count); integration: call `krnl::io::LogIoApcStats("...")` at the orderly `--frames` end. **Event parameter** of NtReadFile/NtWriteFile/NtQueryDirectoryFile/NtDeviceIoControlFile/NtFsControlFile: reset at the start, signalled on completion; unknown handle -> STATUS_INVALID_HANDLE, other type -> STATUS_OBJECT_TYPE_MISMATCH (request not started). Async handle + APC: STATUS_PENDING, IoStatusBlock at delivery (at once if an event is given); synchronous handle + APC: final status, APC still queued. **A8 re-checked**: kept (read starting at/after EOF -> STATUS_END_OF_FILE; past-EOF reads clipped with success). **Disc error handler** = object 0x2D707C (vtable 0x2A59BC, vf0 0xF2FD0, not a Ghidra function start); `DAT_0030E68C` is read only at 0x13F1E3 (completion, dwErr != 0) and 0x13F6B2 (FUN_0013F660, ReadFileEx FALSE) - both paths are counted (`errorCompletions`, `failedRequests`, each also an ERROR log line) and must stay 0. Self-test `GuestReadFileExApc` (AdoptedThread, new `kernel\io_selftest.cpp`): guest WriteFileEx/ReadFileEx/SleepEx(0, TRUE) on a delete-on-close partition1 file -> STATUS_PENDING, OVERLAPPED untouched, SleepEx = 0xC0, completions (0, 0x800, ovl) in FIFO order, read past EOF clipped. `core\guest.h` (shared, additive): `kXapiWriteFileEx` 0xF7654, `kXapiSleepEx` 0xF64BE. Tests: new `tests\apc_tests.cpp`, `tests\vfs_trace_tests.cpp`, new cases in `tests\vfs_tests.cpp` (synthetic-image directory enumeration, host-dir enumeration with APC, Event/A8, end-to-end VFS trace); `TempDir` normalizes `FUT_TEST_TMP` with `make_preferred`. |
| 2026-10-05 | xdk (M2 phase 1) | new `tools\gen\gen_xdk_entries.py`, generated `src\host\gen\xdk_entries.gen.inc`, `tools\gen\gen_all.py` (one `GENERATORS.append` line) | XDK entry table from `analysis\loader\avi_api_surface.tsv` (54 entries, `ret N` + 7 stream vtable slots verified with capstone against the XBE; `--check` works without the XBE). Consumers: `audio\xdk_entries.h` (section 4.13). |
| 2026-10-05 | xdk (M2 phase 1) | `core\guest.h` | `kXppDeviceTypeGamepad` 0x28BF10, `kXppDeviceTypeMemoryUnit` 0x28BFC0 (XPP device type descriptors, compared by address only), `kDsoundStreamVtable` 0x2A5F5C (XMediaObject vtable of CDirectSoundStream). |
| 2026-10-05 | xdk (M2 phase 1) | `core\config.cpp` (`kKnownKeys`), `core\config.h` (doc) | `[audio] dummy_streams` (Bool, default 0): stub streams instead of `*ppStream = NULL`. |
| 2026-10-05 | xdk (M2 phase 1) | (xdk-owned APIs, listed for the other agents) | `src\host\audio\**`, `src\host\video\**`, `src\host\input\**`, `tests\xdk_tests.cpp`: 54 XDK stubs (`XDK_HOOK`), call tracing, dummy guest objects, `xdk::MeasureEntryPurge`, self-tests `XdkStubsRegistered`, `XdkGuestObjectPool` (Host), `XdkHooksInstalled`, `XdkEntryPurgeMeasured` (Mapped); crash reporter "xdk" (call counts). 54 more `HOOK INFO` lines and 3 INFO lines on first use (`DSOUND: stub device`, `XMV: stub decoder`, `XPP: stub input`) in every log. Section 4.13. |
| 2026-10-05 | fiber (M2 phase 1) | `tools\gen\gen_kernel_table.py` (`FATAL_STUBS` now empty), regenerated `kernel\gen\kernel_table.gen.*`, `tests\kernel_tests.cpp` (stub test expects 0 fatal stubs, 107/108 implemented), `src\INTERFACES.md` 4.6 (one sentence) | 107 MmCreateKernelStack / 108 MmDeleteKernelStack are implemented (`kernel\mm_kernel_stack.cpp`, `KRNL_FUNC`), so they left the always-abort list; `stubFatal` is false for every slot, the mechanism stays. `gen_all.py --check` passes. |
| 2026-10-05 | fiber (M2 phase 1) | `core\config.cpp` (`kKnownKeys`), `core\config.h` (doc) | `[kernel] fiber_stack` (U32, default 0x80000): minimum committed size of MmCreateKernelStack stacks (clamped to 0x10000..0x4000000 with a warning). |
| 2026-10-05 | fiber (M2 phase 1) | `core\guest.h` | `kCrtSehProlog` 0x167DC8, `kCrtSehEpilog` 0x167E03 (MSVC 7 SEH3 helpers), `kCrtEhVectorCtorIterator` 0x166E00 (`??_L`, stdcall 5, __try/__finally), `kCrtStrchr` 0x166FC0 (cdecl, no SEH) - used by the self-test `GuestSehThroughHostFrame`; evidence in the header. |
| 2026-10-05 | fiber (M2 phase 1) | (fiber-owned files and APIs, listed for the other agents) | New `asm\fiber_switch.asm` (`fut_switch_to_fiber`, `fut_seh3_frame_call`), `kernel\mm_kernel_stack.h/.cpp`, `xapi\xapi_fiber.h/.cpp`, `core\selftest_m2.cpp`, `tests\fiber_tests.cpp`; section 4.14. Hook `XAPILIB::SwitchToFiber` 0x17094F (+1 `HOOK INFO` line). New log lines on the first fiber use: KRNL/info `fiber stacks: ...`, XAPI/info `fiber: first SwitchToFiber ...` (M2 exit 5: fiber switches > 0). Self-tests (AdoptedThread) `GuestSehOnFiberStack` (M2 exit 6) and `GuestSehThroughHostFrame` (m1_report 8: host frame between guest frames - works, but host destructors are skipped; rule in 4.14). **Experiment (R2, Windows 11 WoW64, 32-bit ntdll disassembly)**: without switching TEB StackBase/StackLimit every exception on a fiber stack kills the process; Windows 11 runs SEHOP in audit mode by default, with SEHOP enforced (`PROCESS_CREATION_MITIGATION_POLICY_SEHOP_ENABLE`) a chain ending in -1, or a terminal record at StackBase-8, kills the process - the stacks therefore carry a terminal record from `RtlInitializeExceptionChain` at StackBase-0x10 (deviation from design 1.7 "8-Byte-Record oben", which fails the strict bound). An unhandled exception on a fiber reaches the host top-level filter only through the fiber-top record. Verified: unit tests (also in SEHOP-enforced child processes), loader self-tests and a 600-frame run under enforced SEHOP. **Integration**: please call `xapi::LogFiberSummary()` (and `krnl::io::LogIoApcStats`) before `core::ExitHostProcess` at the orderly `--frames` end (`gfx\nipc_renderer.cpp`), there is no generic exit reporter. |
| 2026-10-05 | gfx (M2 phase 1) | `core\guest.h` | Renderer/D3D constants (evidence in the header): `kNiXBoxRendererVtable` 0x2AA7C0, `kNiXBoxRendererRtti` 0x30EE00, `kNiXBoxRendererSize` 0x6D0, `kNiRendererBaseCtor` 0x140550 / `kNiRendererBaseDtor` 0x1406E0 (thiscall, no stack args), `kNiRendererList` 0x2D7B18, `kGameOperatorNew` 0xA4900 / `kGameOperatorDelete` 0xA3880 (cdecl), `kGameScreenOffsetUser` 0xA1930 / `kGameScreenOffsetZero` 0xA1920, `kNiImageConverterPtr` 0x30EF64, the 8 D3D shim addresses `kD3DDeviceAddRef` .. `kD3DDeviceSetScreenSpaceOffset`. |
| 2026-10-05 | gfx (M2 phase 1) | `core\config.cpp` (`kKnownKeys`, command line), `core\config.h` (doc) | `[debug] frames` (U32, 0 = off) + command line `--frames=N` (design 5.4); new section `[render]` (not `[gfx]`: `tests\test_core.cpp` uses `[gfx]` as its example of an unknown section): `frame_rate` (U32 Hz, default 50, 0 = unthrottled), `d3d_traps` (String abort\|log), `load_textures` (Bool, default 1). |
| 2026-10-05 | gfx (M2 phase 1) | new `tools\gen\gen_d3d_traps.py`, `tools\gen\gen_renderer_slots.py`, generated `src\host\gen\d3d_traps.gen.inc`, `src\host\gen\renderer_slots.gen.inc`, `tools\gen\gen_all.py` (two `GENERATORS.append` lines) | D3D trap table (79 rows) and NiXBoxRenderer slot table (75 rows, `ret N`), both cross-checked with capstone against the XBE if present (section 4.12). |
| 2026-10-05 | gfx (M2 phase 1) | (gfx-owned files and APIs, listed for the other agents) | New module `src\host\gfx\` (`nipc_renderer.*`, `d3d_shim.*`, `d3d_traps.*`, `purge_probe.*`, `guest_memory.*`, `gfx_config.*`, `gfx_abi.h`, `gfx_selftest.cpp`) and `tests\gfx_tests.cpp`: hooks `NiXBoxRenderer::Create`, 8 `D3D8::*` shims, 79 `trap:D3D8::*`; self-tests `GfxRendererSlotTable`, `GfxRendererThunkPurge`, `GfxD3DShimTrapPurge`; crash reporter "gfx". The orderly `--frames` end (`gfx\nipc_renderer.cpp`, `OnFrameDisplayed`) logs `FRAMES: limit of N frames reached`, the renderer/D3D statistics, `xapi::LogFiberSummary()`, `krnl::io::LogIoApcStats("frame limit")`, `xdk::LogCallSummary(Info)` and exits with code 0 (section 4.15). |
| 2026-10-05 | integration (M2 phase 2) | `core\host_main.h/.cpp` | Exit reporters: `core::AddExitReporter`, `RunExitReporters`, `ExitReporterCount`, `kMaxExitReporters` (16); `ExitHostProcess` runs them (sorted by name, not after a watchdog timeout) before the `process exit` line. Registered: `fibers` (`xapi\xapi_fiber.cpp`), `io apc` (`kernel\io_apc.cpp`), `xdk` (`audio\xdk_entries.cpp`), `gfx` (`gfx\nipc_renderer.cpp`, first Create), `game` (`xapi\game_probes.cpp`). The `--frames` end in `gfx\nipc_renderer.cpp` no longer calls `xapi::LogFiberSummary` / `krnl::io::LogIoApcStats` / `xdk::LogCallSummary` directly (includes removed). M1 logs gain 5 lines (fiber summary, APC stats, 3 XDK lines). Section 4.16. |
| 2026-10-05 | integration (M2 phase 2) | `core\guest.h`, `tools\gen\extra_function_starts.tsv` (+ regenerated `gen\xbe_functions.gen.inc`, 7014 entries) | Block "added in M2 phase 2": `kGameArchiveMount` 0x13FE70, `kGameArchiveLookup` 0x13FE10, `kGameArchiveTableVar` 0x2D7AF0, `kGameArchiveCountVar` 0x2D7AF8, `kGameArchiveSearchVar` 0x2D7AE8, `kGameInit` 0xA26A0, `kCAppFrame` 0xD2630, `kXapiXGetLanguage` 0xF6440, `kGameGamePtr` 0x2E7B84, CApp offsets (`kCAppVideoStandardOffset` 0xE8, `kCAppContextListOffset` 0xB8, `kCAppCurrentContextOffset` 0xBC), context offsets (+4 name, +8 next), `kGameStringManagerPtr` 0x2EC640 (+0x18 language), `kGameFpsVar` 0x2D6744, `kGameFrameTimeVar` 0x2D6748, `kGameMovieTagVar` 0x2E7B70, `kGameBuildText` 0x2E7A68 (0x100), `kGameDiscErrorHandlerVar` 0x30E68C, `kGameDiscErrorObject` 0x2D707C, `kGameDiscErrorScreen` 0xF2FD0 (evidence in the header). New extra function start `0x000F2FD0 0xD9 DiscErrorScreen_vf0` (not a Ghidra function; makes it hookable). |
| 2026-10-05 | integration (M2 phase 2) | new `xapi\game_probes.h/.cpp`, `core\trace.cpp` (VFS trace header), new `tests\m2_integration_tests.cpp` | Five game observer probes + `trap:Game::DiscErrorScreen` (section 4.16, design 8.2 B7/B9/B10); `vfs_trace.log` gains `archive mount` / `archive lookup` lines (header explains them). 6 more `HOOK INFO` lines (160 hooks). |
| 2026-10-05 | integration (M2 phase 2) | `CMakeLists.txt`, new `tools\m2_check.py`, run configs `run\m2`, `run\m2-fr`, `run\m2-long`, `run\m2-ctest` | M2 exit check (section 1): seven criteria per run, `--runs N --fresh`; cache variable `FUT_M2_TEST_CONFIG` registers ctest `fut_m2_check` (3 runs, `--fresh`, timeout 900 s, RUN_SERIAL). |
| 2026-10-05 | integration (M2 phase 2) | `docs\loader_design.md` (section 8 split into 8.1 M1 / 8.2 M2, 5.4 `FATAL_STUBS` sentence), `docs\m1_report.md` (anchor of the section 8 link) | M2 deviations B1-B13 with evidence (module agents' deviations included), new hook targets, measured M2 values. |
| 2026-10-05 | M2 review fixes (phase 4) | `core\log.h/.cpp` | `core::LogSeconds()`, `core::LogThreadName()` (time/thread column of the log; finding 9). |
| 2026-10-05 | M2 review fixes (phase 4) | `core\config.h/.cpp` | `KeyType::Choice` + `KnownKey::choices`, `core::ConfigKeyChoices`; `[settings] language`, `[saves] key_mode`, `[render] d3d_traps` are choice keys (invalid value = exit code 1, finding 10); new key `[debug] exit_hang_test` (Bool, finding 7). |
| 2026-10-05 | M2 review fixes (phase 4) | `core\guest.h` | `kGameFrame` 0xA2C20, `kGamePoolMaintenance` 0xA48C0, `kGameFrameContextSwitchReturn` 0xA2E45, `kCAppNextContextOffset` 0xC0 (probe:Game::ContextSwitch, finding 4; evidence in the header). |
| 2026-10-05 | M2 review fixes (phase 4) | `CMakeLists.txt` | cache variable `FUT_M2_EXTRA_TEST_CONFIGS` (list) -> ctest `fut_m2_check_<dir>` with 1 run each (finding 3: english variant). |
| 2026-10-05 | M2 review fixes (phase 4) | (owned files, listed for the other modules) | `core\host_main.*`: `ExitInProgress()`, watchdog during a hung exit terminates (finding 7); `core\selftest.*`: `InSelfTest()`; `core\trace.cpp`: VFS trace lines `[<log seconds>] <tid>:<thread name> ...`, `[self-test]` suffix, header text (findings 8b, 9); `xapi\xapi_fiber.*`: `FiberStats::selfTestSwitches`, summary `(G game, S self-test; ...)`, first-switch lines per kind (finding 5); `xapi\game_probes.*`: `probe:Game::ContextSwitch`, `switchesSeenAfterFrame` (finding 4; 161 hooks); `kernel\io_apc.*`: `IoApcStats::failedWriteRequests`, stats line `failed APC reads/writes` (finding 2); `vfs\vfs_known_missing.cpp`: partition1 misses only for save directories (12 hex digits) and the five save files (finding 8a); `gfx\nipc_renderer.cpp`: slot 65 in the order of FUN_0015E010 with the 8x8 default texture (finding 1); `tools\m2_check.py` (findings 2, 3, 5, 6, 8b, 9). Tests: new cases in `apc_tests`, `kernel_tests`, `m2_integration_tests`, `test_core`, `vfs_trace_tests`. |
| 2026-10-05 | M3 phase 1 (contracts) | new `src\host\gfx\GFX_CONTRACT.md`, `gfx\gfx_math.h`, `gfx\rhi\rhi.h`, `gfx\rhi\rhi.cpp`, `gfx\rhi\d3d11\d3d11_device.*` (stub), `gfx\ni_guest_layouts.h`, `gfx\ni_extract.*` (stub + arena), `gfx\texconv.*` (stub), `gfx\ff_pipeline.*` (stub), `gfx\shaders\ff_shared.h`, `gfx\shaders\ff_ubershader.hlsl` (stub), `platform\window.*`, `platform\frame_pacer.*`, `platform\screenshot.*` (stubs), `tests\gfx_contract_tests.cpp` | M3 contracts for the backend, texconv, extract and shader agents (section 4.17, GFX_CONTRACT.md); everything compiles, the renderer stays the M2 null renderer. |
| 2026-10-05 | M3 phase 1 (contracts) | `src\host\CMakeLists.txt` | gfx section: `find_program(FUT_FXC fxc)`, `fut_add_shader(name file entry profile [defines])` -> `<build>\src\host\gen_shaders\<name>.h` (fxc `/WX`, Debug `/Od /Zi`, else `/O3 /Qstrip_debug /Qstrip_reflect`), include directory for `fut_host_obj`, stubs `ff_stub_vs`/`ff_stub_ps` (vs_4_0/ps_4_0). |
| 2026-10-05 | M3 phase 1 (contracts) | `core\config.cpp` (`kKnownKeys`), `core\config.h` (doc), `gfx\gfx_config.h/.cpp` | `[render] backend` (Choice null\|d3d11, default null), `vsync` (Bool), `debug_layer` (Choice auto\|on\|off), `adapter` (Choice hardware\|warp), `window_scale` (U32), `fullscreen` (Bool), `screenshot_dir` (Path), `screenshot_frames` (List), `trace_frames` (List), `validate` (Bool, see the next row); `gfx::Settings` fields `backend`, `vsync`, `debugLayer`, `softwareAdapter`, `windowScale`, `fullscreen`, `screenshotDir`, `screenshotFrames`, `traceFrames`, enums `RenderBackend`, `DebugLayerMode`, `gfx::WantDebugLayer`. |
| 2026-10-05 | M3 phase 1 (contracts) | `gfx\nipc_renderer.cpp`, new `gfx\nipc_validate.h/.cpp`, `core\config.cpp`/`config.h` (`[render] validate`, Bool), `gfx\gfx_config.*` (`Settings::validate`) | Slots 47, 48, 49, 50, 51, 54, 55 get dedicated handlers (same `ret N`, static_assert + purge self-test as before; still no-ops returning 0) that call the phase-2 validation harness when `[render] validate = 1`; slot 65 runs texconv on the selected NiPixelData; slot 16 resets the harness arena; `LogRendererStatistics` adds the `validate:` summary. Default off: M2 logs and m1/m2 checks unchanged (verified). |
| 2026-10-05 | M3 phase 1 (contracts) | ownership (section 2 note, GFX_CONTRACT.md 10), run configs | `platform\**` moves from the core agent to the M3 backend agent; new M3 owners for `gfx\rhi\d3d11\**`, `gfx\texconv.*`, `gfx\ni_extract.*`, `gfx\ni_guest_layouts.h`, `gfx\ff_pipeline.*`, `gfx\shaders\**`; regression configs `run\m3-iface\de` (m2_check, `[render] backend = null`), `run\m3-iface\m1` (m1_check) and `run\m3-iface\validate` (harness template). |
| 2026-10-05 | M3 phase 2 (texconv agent) | `src\host\gfx\GFX_CONTRACT.md` (section 7 addendum), own files `gfx\texconv.*`, new `tests\texconv_tests.cpp` | texconv implemented (all NiPixelData formats, BCn pass-through, palettes, mips as stored, default texture, Xbox format choice FUN_001572D0). Additive API in `texconv.h`: `ConvertOptions::keep16Bit` (`kKeepR5G6B5/kKeepA1R5G5B5/kKeepA4R4G4B4`), `HostImage::droppedMipLevels/decompressed`, `MakeTextureDesc`, `MakeSubresources`, `XboxFormat`, `XboxFormatName`, `XboxTableFormat`, `MakeRgbFormat/MakePaletteFormat/MakeCompressedFormat/MakeBumpFormat`, `XboxFormatPrefs` + `kNiSourceTextureFormatPrefs` + `ReadFormatPrefs`, `ChooseXboxFormat`, `IsPowerOfTwo`, `TextureSourceInput::imageFormat/prefs` and `TextureSource::DefaultNoXboxFormat` (appended). Contract addendum: no FormatPrefs quality reduction / mip generation on the Xbox either, palette rule verified (FUN_0015F1B0), BCn rule = level 0 multiple of 4, bit-replication expansion (matches xemu), FormatPrefs +0x18 = mipMapped / +0x1C = alphaFormat (`ni::NiTextureFormatPrefs` names them swapped), disc census. |
| 2026-10-05 | M3 phase 2 (shader agent) | `gfx\shaders\ff_shared.h` (add-only) | New `FF_XBOX_TOP_*` (Xbox D3DTEXTUREOP numbering: 13 BLENDCURRENTALPHA, 14 BLENDTEXTUREALPHA, 15 BLENDFACTORALPHA, 16 BLENDTEXTUREALPHAPM, 22 DOTPRODUCT3, 23 MULTIPLYADD, 24 LERP, 25/26 BUMPENVMAP(LUMINANCE); evidence FUN_0023FDB0 jump table 0x240328 / handler 0x23FFA4, REN decal row 0xE in FUN_0015A120, bump ops 0x19/0x1A by caps bit) - **contract correction**: the phase-1 comment "PC and Xbox use the same D3DTOP numbers" was wrong from 13 on; `FF_TOP_*` stay PC codes, REN's values go through `ni::TextureOpFromXbox`. New `FF_TCI_*` (TEXCOORDINDEX generation), `FF_FEATURE_VS_COMPILE_MASK` (PRETRANSFORMED\|LIGHTING) / `FF_FEATURE_PS_COMPILE_MASK` (FLAT_SHADE); `FF_FEATURE_COMPILE_MASK` 0x5 -> 0x205 (allowed by GFX_CONTRACT.md 6.3). Layout of b0-b3 unchanged. |
| 2026-10-05 | M3 phase 2 (shader agent) | `src\host\CMakeLists.txt` (gfx section, `fut_add_shader` lines) | Stub shaders moved unchanged to `gfx\shaders\ff_stub.hlsl` (`g_ff_stub_vs`/`g_ff_stub_ps` as before); ubershader permutations `ff_vs_pretransformed`, `ff_vs_transformed`, `ff_vs_lit` (`gfx\shaders\ff_ubershader.hlsl` VSMain with `FF_PERM_PRETRANSFORMED`/`FF_PERM_LIGHTING`) and `ff_ps_smooth`, `ff_ps_flat` (PSMain, `FF_PERM_FLAT`). Every `.hlsl`/`.h` under `gfx\shaders` is a dependency of every shader (rebuild on change). |
| 2026-10-05 | M3 phase 2 (shader agent) | own files `gfx\ff_pipeline.h/.cpp`, `gfx\shaders\ff_ubershader.hlsl`, new `tests\ff_pipeline_tests.cpp`, `tests\ff_render_tests.cpp` | D3D8 fixed-function emulation implemented (header comment of `ff_pipeline.h` = semantics). Additive API: `FillFogConstants` (fog color/params of a draw into b0; the pipeline re-uploads b0 when they change), `FillLightConstantsWithView`, `ActiveStageCount`, `VertexPermutation`/`PixelPermutation` (+ `kVertexPermutations` 3 / `kPixelPermutations` 2), `VertexShaderBytecode`/`PixelShaderBytecode`/`StubShaderBytecode` (embedded DXBC, so tests need no `gen_shaders` include path), `DrawResultName`, `FixedFunctionPipeline::LastDrawNote()` (reason of the last Skipped/Failed) and `InvalidateState()`, `PipelineStats::litDraws/pretransformedDraws/frameUploads/lightUploads/stageUploads/missingTextures`. `MakeFeatureKey` reads `RenderStateDesc::localViewer` (extract agent). Pre-transformed pixels are mapped relative to `CameraDesc::viewport` (the integration must set that viewport on the context). Tests: pure translations, a recording mock device and rendered pixel checks on a test-only WARP `rhi::Device` (debug layer checked when installed). |
| 2026-10-05 | M3 phase 2 (backend agent) | `src\host\CMakeLists.txt` (gfx section, block "backend agent" after `target_include_directories`), `src\host\gfx\GFX_CONTRACT.md` (section 8 addendum "Phase-2 results (backend agent)") | Five `fut_add_shader` lines compile the backend's built-in shaders `gfx\rhi\d3d11\d3d11_builtin.hlsl` (`file` relative to `gfx\shaders`: `../rhi/d3d11/...`) into `gen_shaders\d3d11_clear_vs.h`, `d3d11_diag_vs.h`, `d3d11_diag_color_ps.h`, `d3d11_diag_texture_ps.h`, `d3d11_diag_tint_ps.h`. Contract addendum: backend logs the debug-layer messages itself (`DrainDebugMessages` callers must not log them again), exit reporters "d3d11" (debug-layer line) and "present" (two `present:` lines, registered by the first `NotePresentedFrame`, which also shows the window), `MainWindow()` = the 4:3 view child, DPI-relative `window_scale`, `SetRenderTargets` resets the viewport, rectangle clears, transient-ring and resource rules, measurements, kept config key names. |
| 2026-10-05 | M3 phase 2 (backend agent) | own files `gfx\rhi\d3d11\**` (D3D11 backend replacing the stub; `#pragma comment(lib, "dxguid.lib")`, `windowscodecs.lib` in `platform\screenshot.cpp`), `platform\**`, new `tests\rhi_d3d11_tests.cpp`, `tests\platform_tests.cpp` | Public additions (owned headers): `gfx\rhi\d3d11\d3d11_device.h`: `BuiltinShader` + `BuiltinShaderCode`, `kDiagVertexStride` + `DiagVertexLayout`, `DebugLayerTotals` + `GetDebugLayerTotals`, `LogDebugLayerSummary`, `DebugLayerStateName`, `LastTeardownLiveObjects`, `DxgiFormatOf`, `PresentResultFromHresult`, `HresultText`. `platform\window.h`: `WindowOptions`, `kViewWidth/kViewHeight`, `CreateMainWindow`, `DestroyMainWindow`, `MainFrameWindow`, `MainWindowShown`, `PostHotkeys`, `ViewRect` + `FitAspectRect`. `platform\frame_pacer.h`: `PresentStatistics::pacingHz/scheduleResets/slowFrames`, `ResetFramePacer`, `PresentStatsAccumulator`. `platform\screenshot.h`: `SaveScreenshot`, `ReadImageFile`. Stage 12 (`platform::StageCreateWindow`) creates the hidden main window when `[render] backend = d3d11` (section 3 table: no longer a no-op from M3 on; a creation failure is a CORE/error and the stage still succeeds). |
| 2026-10-05 | M3 extract (phase 2) | `gfx\ni_extract.h/.cpp`, `gfx\ni_guest_layouts.h` (extract-owned), new `tests\extract_tests.cpp` | **ni_extract implemented** (GFX_CONTRACT.md 4): `ExtractCamera`/`DefaultCamera`/`ClearRect` (x87 order of 0x1469E0, `_ftol` truncation), `ExtractState` (FUN_00159CD0 properties, texture pipeline single-stage + screen paths incl. the Ghidra-gap vf2 0x164360 and pass apply 0x15FFE0, FUN_0015BF80/FUN_0015BA40/FUN_0015B6A0 lights), `ExtractShape/Strips/Triangles/TriStrip` (zero-copy streams, vf12/vf15 by implementation address incl. `NiTriShapeDynamicData` active counts, strips joined with 0xFFFF, CPU skinning through the original FUN_0014F710), `ExtractScreenPolygon`, `ExtractScreenTexture`. **API additions for the integration**: `ni::BeginScene(StateShadow*)` (call from slot 45: FUN_00158B90 + FUN_0015B990); `StateShadow` now models REN's render-state/texture-stage caches and the device values separately (defaults DAT_002D8338/DAT_002D82B8 vs DAT_00242748/DAT_00242728 + REN's init) plus the light manager, a skin cache and `ExtractCounters counters` (log them at the end); `ExtractContext::defaultProperties` (slot 55 defaults, nullptr = guest globals `kDefaultPropertyGlobals`) and `allowGuestCalls`; `StateInput::screenTexture`; `RenderStateDesc::localViewer` (device default TRUE) and `texturePath` (`TexturePath::SingleStage/MultiMap/Screen`); `ni::TextureOpFromXbox` (Xbox D3DTEXTUREOP -> FF_TOP_*: the numbers differ from 13 on); `ExtractArena::Generation()`; arena blocks now come from `gfx::AllocGuestPages`. `TextureResolver`: an empty handle = the original's texture manager failing (stage gets no texture, its filter/address/texcoord states persist). Texture size/mips are read from the renderer data (+0x48/+0x4C/+0x50 = data vf2/vf3/vf5): the integration should store the uploaded mip count at `NiPCTextureData +0x50` (0 falls back to the NiPixelData count). Layout additions (evidence in the header): `NiTexturingProperty::hasNonBaseMaps` (+0x1C, was `multiTexture`), `decalMapCount`, apply-mode constants, texture renderer-data offsets, NiSourceTexture/NiRenderedTexture vtables, GetEffectType implementations, `NiTriShapeDynamicData`, geometry vtables + vf12/vf15 implementations, `kNiTextureEffectClipped`, default property globals, slot 55 property constants. Verified with real data (validation harness, 1000 frames): Shape 71481 / ScreenPolygon 46 / ScreenTexture 4838 calls all ok (481 skinned draws through FUN_0014F710); skinned vertices are shape-local (slot transform = world). |
| 2026-10-05 | M3 phase 3 (integration) | `gfx\nipc_renderer.h/.cpp`, new `gfx\nipc_gpu.h/.cpp` (section 4.18), new `tests\nipc_gpu_tests.cpp` | NiPCRenderer draws through `ni_extract` -> `ff_pipeline` -> RHI (D3D11) when `[render] backend = d3d11`: new `gfx::GpuRenderer`; new slot-17 handler (ClearBuffer); slots 15/16/19/27/45/47-51/54/55/65 extended; `NiPCTextureData` `field64[3]` -> `hostTexture`/`hostInfo`/`hostRevision` + `kHostOwnsTexture`; slot 65 complete per FUN_0015E010 incl. FUN_001572D0 (both backends); texture resolver per FUN_0015F930/FUN_0015E260; M2 `PaceFrame` removed (both backends use `platform::PaceFrame`); +0.5-pixel projection shift (GFX_CONTRACT 9/11); new exit statistics and `draw trace/summary` lines. |
| 2026-10-05 | M3 phase 3 (integration) | `core\config.cpp`/`config.h`, `gfx\gfx_config.h/.cpp` | `[render] backend` default `d3d11` (was `null`); `gfx::Settings::backend` default `RenderBackend::D3D11`. |
| 2026-10-05 | M3 phase 3 (integration) | `gfx\ni_guest_layouts.h` | `NiTextureFormatPrefs` field names fixed: +0x18 `mipMapped`, +0x1C `alphaFormat` (texconv finding; no reader of the old names). |
| 2026-10-05 | M3 phase 3 (integration) | new `tools\m3_check.py`, `GFX_CONTRACT.md` section 9 addendum + section 11 note, `docs\loader_design.md` 8.3 (B19-B29) | M3 exit check (imports `m2_check`), run configs `run\m3\futurama.ini`, `run\m3\rel`, `run\m3\m1`, `run\m3\m2`, `run\m3\m2-null` (git-ignored). |
| 2026-10-05 | M3 phase 5 (review fixes) | `gfx\nipc_renderer.cpp`, `gfx\nipc_gpu.h/.cpp`, `gfx\ni_extract.h/.cpp`, `gfx\ni_guest_layouts.h`, `core\guest.h`, `xapi\game_probes.*` | Slot 65 frees the NiPixelData pixel buffer after the upload like FUN_0015E010 0x15E1DD (both backends; finding 1); new slot handler 60 PurgeEffect (`ni::PurgeLight`, finding 4); exit line `game memory pool: ...`; `GpuRenderer::OnRenderThread`, frame counters without draw/color clear, picture origin in `draw summary`, screen bounds in `draw trace`; ClearBuffer evidence addresses 0x14654D..0x1465B9 (finding 6). Constants see section 4.18. |
| 2026-10-05 | M3 phase 5 (review fixes) | `gfx\rhi\rhi.h/.cpp`, `gfx\rhi\d3d11\*`, `src\host\CMakeLists.txt` (two `fut_add_shader` lines `d3d11_present_vs/ps`), `gfx\shaders\ff_ubershader.hlsl` | Persistent back buffers in rotation + present pass into a window-size swap chain with `rhi::PresentScaling` (findings 2, 8, 11); `DeviceDesc::scaling`; the D3D11 context re-binds its state after Present; client size read in the view's DPI context; the "d3d11" exit reporter pumps the info queue on the render thread (finding 10); transformed vertices snapped to 1/16 pixel (`SnapToSubpixelGrid`, finding 9). |
| 2026-10-05 | M3 phase 5 (review fixes) | `platform\window.h/.cpp`, `platform\frame_pacer.h/.cpp`, `core\config.cpp`, `gfx\gfx_config.*` | `platform::CloseRequested()` (WM_CLOSE -> end after the next frame on the guest main thread, 3 s fallback timer; findings 5/10); `platform::NoteFrameWorkDone()` + frame-work statistics line (finding 3); config key `[render] scaling` (Choice linear\|point\|integer). |
| 2026-10-05 | M3 phase 5 (review fixes) | `tools\m3_check.py`, `tools\xemu_capture.py`, tests (`rhi_d3d11_tests`, `platform_tests`, `extract_tests`, `ff_render_tests`), `GFX_CONTRACT.md` section 9 "Phase-5 results" + section 11, `docs\loader_design.md` 8.3 (B30-B37), new `docs\m3_report.md` | M3-3 = frame work 99th percentile <= 20 ms + paced average >= 49.5 Hz (finding 3); frontend static mask from `run\xemu_ref` + `run\xemu_ref_steam` and the steam region masked (finding 7); frames without draws checked through the picture they show; `xemu_capture.py --start`. |
| 2026-10-05 | timing (M3b) | `core\config.cpp` (`kKnownKeys`), `core\config.h` (doc) | New section `[game]`: `tick_rate` (Choice `60\|50`, default 60; anything else = exit code 1). Read by `xapi::ConfiguredTickRate` (section 4.19) and the frame pacer. `frame_clock` (Choice `lockstep\|realtime`, default lockstep): time source of Game::Frame's accumulator (section 4.19). |
| 2026-10-05 | timing (M3b) | `core\guest.h` | Block "added in M3b" (evidence in the header, `analysis\loader\timing60.md` 2): Game::Init time base instructions `kGameInitVideoStandardBranch` 0xA26BD, `kGameInitNtscFpsLoad` 0xA26CC, `kGameInitNtscDtLoad` 0xA26E3, `kGameInitNtscFpsCopyStore` 0xA26E9, `kGameInitNtscDtMaxStore` 0xA26F3, `kGameInitNtscFrameTimeStore` 0xA26FD, `kGameInitPalFpsLoad` 0xA2714, `kGameInitPalFpsCopyStore` 0xA271A, `kGameInitPalFpsCopy1Store` 0xA2724, `kGameInitPalDtMaxStore` 0xA272A, `kGameInitPalDtLoad` 0xA2734, `kGameInitPalFrameTimeStore` 0xA273A, `kGameInitPalMovieTagStore` 0xA2744, `kGameInitTimeBaseTail` 0xA274E; .rdata `kRdataFloat50` 0x2955AC, `kRdataFloat0p02` 0x2942C0, `kRdataFloat60` 0x2942CC, `kRdataFloatOneSixtieth` 0x29E390; globals `kGameFixedStepVar` 0x2D674C, `kGameDtMinVar` 0x2E7A58, `kGameDtMaxVar` 0x2E7A5C, `kGameFpsCopy1Var` 0x2E7A60, `kGameVideoFrameTimeVar` 0x2E7B68, `kGameFpsCopy2Var` 0x2E7B6C; per-step helper `kGameFixedStepHelper` 0x2C890 and its call `kGameFrameFixedStepCall` 0xA2DD3; game timer `kGameTimer` 0xD2B90, `kGameTimerFrequencyVar` 0x2EB448, `kGameTimerInverseFrequencyVar` 0x2EB450 and the Game::Frame sample returns `kGameFrameClockSampleReturn` 0xA2C6F, `kGameFrameRenderInitSampleReturn` 0xA2FA6, `kGameFrameRenderSampleReturn` 0xA2FBB. |
| 2026-10-05 | timing (M3b) | `core\host_main.cpp`, `core\selftest_m1.cpp` (core-owned, additive lines only), `xapi\game_probes.h/.cpp` (one call + comment) | HostMain stage 8b `ApplyGameTiming` = `xapi::StageApplyGameTiming(cfg)` between ApplyFsPatches and InstallHooks (one stage line, include, stage comment row; the log now counts `stage N/14`, InstallHooks is 9/14 ... MessageLoop 14/14). Self-test `MappingMatchesFile` also excludes `xapi::AppliedTimingPatches()` (7 ranges at 60 Hz, 0 at 50 Hz). `probe:Game::Init` calls `xapi::NoteGameInitTiming(self)` after its "video standard" line (new log line `game timing: effective after Game::Init: ...`). |
| 2026-10-05 | timing (M3b) | new `xapi\game_timing.h/.cpp`, new `tests\timing_tests.cpp`, `src\INTERFACES.md` (section 3 row 8b, section 4.19) | 60 Hz logic tick with PAL content (section 4.19): patch table + engine with original-byte checks against image and XBE file, read-back after Game::Init, `probe:Game::FixedStep` (FUN_0002C890) and `probe:Game::Timer` (FUN_000D2B90, lockstep frame clock; +2 `HOOK INFO` lines), self-test `GameTimingPatched` (Mapped), exit reporter "game timing" (`game ticks: ...`, `game clock summary: ...`). New log lines `game timing: ...`, `game clock: ...` (GAME/info). 18 test cases. |
| 2026-10-05 | pacing (M3b) | `core\config.cpp` (`kKnownKeys`, `[render]` block), `core\config.h` (doc) | `[render] pacing` (Choice `auto\|vsync\|timer\|off`, default auto), `[render] pacing_trace` (Path, default off); documented: `[render] frame_rate` defaults to `[game] tick_rate` (was 50). Section 4.20. |
| 2026-10-05 | pacing (M3b) | `gfx\rhi\rhi.h` (add-only) | `DeviceDesc::frameLatencyWaitable`, `DeviceInfo::tearingSupported` / `frameLatency`, `PresentDesc::allowTearing` / `skipSwapChain`, `struct PresentTiming`, `Device::FrameLatencyWaitable()` / `LastPresentTiming()` (virtual with default implementations; the test devices stay unchanged). Behaviour change of the D3D11 backend: `DXGI_PRESENT_ALLOW_TEARING` only on request (M3 set it for every sync interval 0). |
| 2026-10-05 | pacing (M3b) | own: `platform\frame_pacer.h/.cpp` (rewritten; M3 API kept), new `platform\pacer_policy.h/.cpp`, new `platform\pacer_display.h/.cpp`, `gfx\rhi\d3d11\d3d11_device.h/.cpp`, `d3d11_internal.h` (frame-latency waitable swap chain, frame statistics, skipped presents), new `tests\pacer_tests.cpp`; additive: `platform\window.h/.cpp` (`DisplayEnvironment`, `GetDisplayEnvironment`, `NoteDisplayChange`, power/WTS notifications, `#pragma comment(lib, "wtsapi32.lib")`), `gfx\gfx_config.h/.cpp` (`Settings::tickRate/pacing/pacingTrace`, `SettingsFromConfig`, `PacingConfigOf`, frame_rate default), `gfx\nipc_gpu.h/.cpp` (DisplayFrame: `PlanPresent` / `NoteSwapChainPresent`, `frameLatencyWaitable = 1`), `gfx\nipc_renderer.cpp` (OnFrameDisplayed: `EndFrame`), `tests\gfx_tests.cpp` (default frame rate = tick rate), `GFX_CONTRACT.md` section 8 addendum | Display-coupled pacing (section 4.20): vblank k / tearing-timer + VRR probe / display-rate / free / timer / unthrottled / headless; the 50 Hz default is gone. |
| 2026-10-05 | verify (M3b) | `tools\m2_check.py`, `tools\m3_check.py`, new `tools\timing_compare.py`, `docs\loader_design.md` 8.4 (B38-B46), this file (section 1, 4.18) | Checks at the tick rate: frame numbers are seconds x rate (intro threshold 20 s, scenes 3.0/8.0/13.6/20.0 s, timeout frames/rate + 90 s), M3-3 minimum rate - 0.5 Hz, work p99 <= one tick period, >= 95 % frame starts on period; new timing criteria T1/T2 (`m2_check.check_timing`, in `m3_check` M3b-5/6); M3b pacing statistics reported. Run configs `run\m3b\*`; `run\m3final\*` switched from `[render] frame_rate = 50` to `[game] tick_rate = 50` + `[render] pacing = timer`. No source change. |
| 2026-10-06 | M3b phase 5 (review fixes) | `platform\pacer_policy.h/.cpp` (`PlanHysteresis` resume exception, new `CadenceGuard` (VRR-aware, alternating shifts, stops after 4), `VrrDetector` warm-up, `OnScreenTracker` jumps + refresh fit, `LastPair`), `platform\frame_pacer.h/.cpp` (slow frames from frame starts, cadence shifts, VRR warm-up/retry, `DescribePlan` "no Present", new `PresentStatistics` fields), `core\config.cpp` (`--set` values unquoted), `gfx\gfx_config.cpp` (GFX/warn for ineffective pacing combinations), `xapi\game_timing.h` (comment), `tests\pacer_tests.cpp`, `tools\m3_check.py` (M3b-7), new `analysis\loader\scripts\timing_review.py` | Review findings 5-11, 13a fixed; findings 1-4 (tick-bound gameplay, NTSC parity) documented in `timing60.md` 12; second `present:` exit line now counts slow frame starts (the M3 prefix is unchanged). Sections 4.19, 4.20. Report `docs\m3b_60hz_report.md`. |
| 2026-10-06 | M3c phase 1 (host-CLI/schema agent) | new `core\config_schema.h/.cpp` | Schema table of every configuration key (section 4.21): type, default, choices with de/en labels, ranges, de/en label + help for players, UI tab/order/widget, advanced/developer/dynamic-default flags; validation `core::ValidateConfigValue` (+ float, resolution, enforced ranges), shared parsers `core::ParseConfigBool/U32/Float/Resolution`, `MatchesConfigChoice`, `ConfigKeyDefault`, `ConfigChoiceList`. Contains the M3c `[render]` keys `resolution`, `render_scale`, `msaa`, `alpha_to_coverage`, `anisotropy` (contract item 5; the renderer agent added `screenshot_size` through the same table). |
| 2026-10-06 | M3c phase 1 (host-CLI/schema agent) | `core\config.cpp`, `core\config.h` (shared) | `kKnownKeys`, `KeyType`, `MatchesChoice`, `FindKnownKey`, `IsKnownSection` removed (replaced by the schema table; `IsKnownConfigKey`/`ConfigKeyChoices` keep their signatures and results, choice lists list the visible values first); local `ParseBool`/`ParseU32` delegate to the schema parsers. Added: `core::ConfigLocationMode`, `ConfigLocation`, `ResolveConfigLocation`, `ConfigLocationModeName`, `kConfigFileName`, `kPortableMarkerName`, `kUserdataFolderName`, `Config::configMode/portable`. **Behaviour change (contract item 4):** without `--config` the INI is `%LOCALAPPDATA%\FuturamaPC\futurama.ini` (portable: `<exe dir>\futurama.ini` with `portable.txt`, userdata `<exe dir>`); an `<exe dir>\futurama.ini` alone is ignored with a `config:` warning. `[log] level` is validated as a choice and `[kernel] min_guest_stack` outside 0x10000..0x4000000 is a configuration error (exit 1, was exit 2 from the kernel stage). |
| 2026-10-06 | M3c phase 1 (host-CLI/schema agent) | new `core\cli_tools.h/.cpp`; `core\host_main.cpp` (core-owned, additive: include, tool dispatch at the start of `RunHost`, stage comment, config log line with the location mode) | Tool modes `--print-config-schema`, `--check-iso` + `--result`, `--query-gpu`, `--print-paths` (contract items 1-3 + extension) before configuration/log/XBE; `core::cli::JsonWriter`, `ConfigSchemaJson`, `CheckIso`/`IsoCheckJson`, `QueryGpu`/`GpuInfoJson`, `PathsJson`, `BuildDescription`, `kProtoNtscXbeSha1`, exit codes `kToolExitOk/Usage/NotSupported`. Log line `config    <file> (<mode>[, portable.txt])` (the `userdata  ` line read by the checks is unchanged). `src\launcher\launcher.c` unchanged (the stub's TLS/placeholder checks run before the tools). |
| 2026-10-06 | M3c phase 1 (host-CLI/schema agent) | new `tests\schema_tests.cpp`, `tests\cli_tests.cpp`, `tests\cli_json.h`; this file (sections 1, 4.3, new 4.21); `docs\loader_design.md` (4.1 note, new 8.5 rows K1-K6) | Tests and documentation of section 4.21. |
| 2026-10-06 | M3c phase 1 (renderer agent) | own: `gfx\gfx_config.*`, `gfx\nipc_gpu.*`, `gfx\nipc_renderer.cpp` (one line), `gfx\ff_pipeline.*`, `gfx\shaders\ff_shared.h` + `ff_ubershader.hlsl`, `gfx\rhi\rhi.h` + `rhi.cpp` (add-only), `gfx\rhi\d3d11\*`, `platform\screenshot.*`, `gfx\GFX_CONTRACT.md` (new section 12); new `tests\aa_tests.cpp`, new `tools\m3c_check.py` | Render resolution and anti-aliasing (section 4.22): internal resolution with the guest kept at 640x480, MSAA back buffers + resolve, alpha-to-coverage with alpha sharpening, supersampling downsample (box / Mitchell-Netravali), anisotropic filtering, anti-aliased nearest for point-filtered textures, screenshots at the internal or output size (asynchronous PNG), GPU frame timing. Classic configuration bit-identical to M3. |
| 2026-10-06 | M3c phase 1 (renderer agent) | `src\host\CMakeLists.txt` (shared, additive) | One `fut_add_shader(d3d11_resample_ps ... ResamplePS ps_4_0)` line in the backend block. |
| 2026-10-06 | M3c phase 1 (renderer agent) | `core\config_schema.cpp` (host-CLI/schema agent's table, one row as its header asks of a module with a new key) | Row `[render] screenshot_size` (choice `output\|internal`, default output, Advanced tab, order 100) + `kScreenshotSizeChoices`. The renderer's defaults `gfx::kDefault*` equal the schema rows of resolution/render_scale/msaa/alpha_to_coverage/anisotropy (unit test). |
| 2026-10-06 | M3c phase 1 (renderer agent) | this file (new section 4.22) | Section 4.22. |
| 2026-10-06 | M3c phase 1 (launcher agent) | new `launcher\**` (own CMake project), new `build_launcher.cmd`, new `tools\package.py`, `.gitignore` (`dist/`, additive), this file (new section 4.23) | Qt 6 launcher `FuturamaLauncher.exe` (section 4.23, `launcher\README.md`): schema-driven tabs, first-run wizard, INI round trip with host rules, game start + result panel, automation modes; player package `dist\FuturamaPC\`. No change to the x86 build or to host files. |
| 2026-10-06 | M3c phase 2 (integration) | `platform\window.h/.cpp`, `platform\platform.cpp`, `gfx\gfx_config.h/.cpp`, `core\config_schema.cpp` (row), `tests\platform_tests.cpp`, `tests\schema_tests.cpp` | `[render] monitor` (section 4.21): `platform::SelectMonitor`, `WindowOptions::monitor`, window size, DPI and work area of the chosen monitor (`GetDpiForMonitor`, `shcore.lib` via `#pragma comment`), log suffix `monitor <device> (primary)`; schema row (string shown with widget monitor - the one widget exception in `schema_tests`); unit test of the selection. |
| 2026-10-06 | M3c phase 2 (integration) | `core\config_schema.cpp`, `core\cli_tools.cpp` | Player texts only: `[settings] language` label "Spielsprache"/"Game language", `[render] window_scale` help mentions the Windows scaling, check-iso messages say "europäische Verkaufsversion" (the launcher's wording). No behaviour change. |
| 2026-10-06 | M3c phase 2 (integration) | `launcher\src\ui\iso_status.*` (new), `key_editors.cpp`, `settings_tab.*`, `main_window.*`, `wizard.cpp`, `screen_info.*`, `automation.cpp`, `launcher\src\core\gpu_info.*`, `l10n.cpp`, `l10n_table.cpp`, `launcher\CMakeLists.txt`, `launcher\tests\tst_ui.cpp`, `launcher\testdata\sample_schema.json` | Launcher against the real host (section 4.23, last item): ISO verdict box on the game tab, correct resolution-auto texts and a 4:3 list, window size in pixels, alpha-to-coverage dependency, one label column per tab, frame-rate display from the tick rate, number format per UI language, screenshots at the default window size; UI tests `displayHelpers`, `dependentKeysAndIsoStatus`. |
| 2026-10-06 | M3c phase 2 (integration) | `docs\loader_design.md` (section 8 title, new 8.7 rows I1-I8), this file (4.21, 4.23), `launcher\README.md`, `README.md`, `docs\ROADMAP.md` | Integration documentation. |
| 2026-10-06 | M3c phase 4 (review fixes) | `gfx\rhi\d3d11\d3d11_builtin.hlsl` (`ResamplePS`), `gfx\rhi\d3d11\d3d11_device.cpp`, `d3d11_internal.h`, `gfx\rhi\rhi.h` (add-only `DeviceDesc/DeviceInfo::linearResolve`), `gfx\nipc_gpu.h/.cpp`, `gfx\nipc_renderer.cpp`, `gfx\gfx_config.h/.cpp`, `gfx\ff_pipeline.h/.cpp`, `gfx\GFX_CONTRACT.md` 12, `tests\aa_tests.cpp`, `tools\m3c_check.py` | Section 4.22: Catmull-Rom downsample with anti-ringing (finding 1), quality ladder + `core::Fatal` instead of the invisible null renderer (finding 2), `[render] linear_aa` (finding 3), anisotropy only with MAG = LINEAR (finding 4), destination-alpha rule of alpha-to-coverage (finding 5); log `render:` line + `resolve ...[, reduced from ...]`, `render totals:` + destination-alpha draws; m3c_check variants `max`, `frac`. |
| 2026-10-06 | M3c phase 4 (review fixes) | `core\host_main.h/.cpp`, `core\config_schema.cpp`, `core\config.cpp`, `core\cli_tools.cpp` | Exit code 6 `core::kExitAlreadyRunning`: named mutex `Local\FuturamaPC-userdata-<hash>` per user data folder before the log opens (finding 15). Schema: row `[render] linear_aa`; `[render] frame_rate` is a developer key (finding 18); `[game]` section "Spiellogik"; player texts (finding 16: Sie, "Zeitquelle der Spiellogik", "Alt+Eingabetaste", "Spielabbild (ISO)", "Verkaufsversion", "Fehlende D3D-Funktionen", "Zeitlimit", "Rückgabecode"); the unknown-key error names no source file; VRR hint reworded. |
| 2026-10-06 | M3c phase 4 (review fixes) | `launcher\**`, `build_launcher.cmd`, `tools\package.py`, new `launcher\licenses\` | Section 4.23 "M3c review fixes" (findings 7-15, 17-23). |
| 2026-10-06 | M3c phase 4 (review fixes) | new `docs\m3c_report.md`, `docs\loader_design.md` (new 8.8), `docs\ROADMAP.md`, `README.md`, `launcher\README.md`, this file | M3c report and documentation. |
