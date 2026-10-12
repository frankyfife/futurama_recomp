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
Added in M4 (test automation): `7` the input script failed (`[debug] input_script`: timeout, `expect`, `fail`; section 4.27).
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
silent stubs in `audio\dsound_stubs.cpp`, `video\xmv_stubs.cpp`, `input\xpp_stubs.cpp` (the XPP part is replaced
by the HLE `input\xpp_hle.cpp` since M4, section 4.28) (namespaces `snd`,
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
- **M5:** the 40 DSOUND entries are HLE (`audio\dsound_hle.cpp`, section 4.29 "DSOUND core"; `dsound_stubs.*`
  deleted); the DSOUND stub behaviour above and `[audio] dummy_streams` are history (the key is still accepted).
- **M6:** the 7 XMV entries are HLE (`video\xmv_hle.cpp`, section 4.32; `xmv_stubs.*` renamed to `xmv_hle.*`); the
  XMV stub behaviour above survives only as `[debug] movies = skip`. The summary line reads `XDK XMV HLE: ...`.
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

### 4.24 Frame rates up to 120 Hz: frame clock in quanta, `FrameInterp`, pacing - `xapi\game_timing.h`, `xapi\frame_fixes.h`, `platform\pacer_policy.h`, `platform\frame_pacer.h` (M3d, frame-clock/pacing agent)

Design and measurements: `analysis\loader\m3d_frameclock.md` (German). Header comments of the four headers are the
reference. Logic stays at `[game] tick_rate` (60 Hz); frames between two ticks show the renderer's interpolation
(`gfx\interp_*`, interpolation agent) with the clock's alpha. At 60 fps (one frame per tick) everything is the M3b
behaviour bit for bit (unit test `frameclock: one frame per tick is the M3b lockstep clock bit for bit`).

- Config: `[render] frame_rate = 60 | 120 | display` is a choice since M3d (de/en "60" / "120 (interpoliert)" /
  "Monitor (max. 120)" - the labels of the user requirement since the review, finding 12; hidden: `50` and the developer numbers `0..1000` with their M3b meaning; unset = `[game]
  tick_rate`, dynamic default). `gfx::ParseFrameRate(text, &hz, &mode)` -> `platform::pacing::FrameRateMode {Fixed,
  Max120, Display}` (`kMaxFrameRate` 120); `gfx::Settings::frameRateMode/lowLatency`, `platform::PacingConfig::
  rateMode/justInTime`. `[render] low_latency = on | off` (bool, advanced, default on): just-in-time frame start.
- Plans (`pacer_policy.h`, pure): `ChooseFrameRate(mode, refresh, cap)` -> `RateChoice {frameHz, k}`; `SelectPlan`
  with `PlanInput::rateMode` != Fixed: refresh <= 120 Hz (+0.2 %) -> `vblank k = 1` at the refresh (60 Hz: 60 fps,
  one frame per tick, no interpolation); k x 120 Hz -> `vblank k`; otherwise with tearing support the VRR timer at
  120 (`tearing-timer`), without VRR (or VRR rejected) "120" -> new `Path::Cadence` (frame-latency waitable +
  Present(s_n), s_n = 1 or 2 refreshes from `CadencePattern`, 120 frames per second on average) and "display" ->
  `vblank k = ceil(R / 120)` (72 fps on 144 Hz, 82.5 on 165 Hz). Background / minimized / timer overrides run at
  the even rate. `Plan::nominalHz` (all plans; Fixed: the configured rate), `Plan::cadenceRatio`; `SamePlan`
  compares the ratio. Fixed mode = the M3b selection unchanged.
- Frame clock (`game_timing.h`): `FrameClock::Advance(real, step, ClockQuantum, lastPrev, capTicks)` -> `Sample
  {value, reset, aligned, quanta}` generalises `LockstepClock`: N frames per tick -> quanta `TickQuantum(step, N, i)`
  (floor/ceil, N quanta = ticks(dt)); N = 0 -> the frame's display time in QPC ticks with a fraction carry. M3b
  corrections at 3/4 quantum (catch-up in whole quanta, hold). Phase B: the guest remainder is aligned once to
  `ticks(dt) - mu (mod quantum)`, `mu = PhaseMargin(step) = max(16, step / 1024)`, in the second frame, after the
  guest's 6 dt cap and after a regime change (frames per tick, nominal period or ticks(dt) changed = `reset`).
  The pacer supplies the regime: `platform::CurrentFrameAdvance()` -> `pacing::FrameAdvance {known, framesPerTick,
  seconds, nominalSeconds, variable, startDelayTicks}` (`FrameAdvanceOf(plan, tickHz, refreshHz, lastRefreshes)`,
  `FramesPerTick(nominalHz, tickHz)` within 100 ppm, and 1 for every rate up to the tick rate x 1.002 - review
  finding 8; the Cadence plan always uses display time). The clock compares `real - startDelayTicks` with its virtual
  time (just-in-time start); in the sub-tick regimes `real` is the pacer's frame start (review finding 11, below).
- Interpolation values: `xapi::FrameAlpha(rest, step, N, phaseB)` = `clamp(1 - (L - rest) / step, 0, 1)`, L = step
  - mu in phase B, else step; on the 1/N grid within 0.01, >= 0.999 -> 1. Published at the render sample (return
  0xA2FBB): `xapi::CurrentFrameInterp()` -> `FrameInterp {tick, stepsThisFrame, pairTicks, alpha, flags, enabled,
  frame}`; flags `kFrameInterpSnapAll` (context switch, clock reset), `kFrameInterpInsideStep` (draws inside the step
  loop or outside Game::Frame: alpha 1), `kFrameInterpClockReset`, `kFrameInterpCatchUp` (catch-up above the regime's
  steps or the 6 dt cap: interpolated over the pair of `pairTicks` ticks, review finding 10).
  The stage registers it as the renderer's source (`gfx::interp::SetFrameInterpSource`, `gfx\interp_frame.h`, same
  flag bits). Contract: display = interp(S[G-1], S[G], alpha); at 120 fps alpha is 1/2 in the frame that runs the
  tick and exactly 1 in the frame between (displayed time + 1/2 tick per frame). `xapi::SubTickFramesActive()`,
  `xapi::RenderingFrameWithoutStep()`, `xapi::GetFrameClockStats()` (M3d fields).
- Per-frame fixes, active only while the clock produces frames between ticks (`xapi\frame_fixes.h`): F1 the pacer
  keeps the previous picture for a frame without a fixed step that drew nothing and cleared no color
  (`SwapChainInfo::frameUnchanged`, set by `GpuRenderer::DisplayFrame`; `PresentRequest::repeatPrevious`;
  `platform::NoteFrameTicks(steps)` from the render sample; the intro's legal screen and tech notice are drawn in
  Update). Verify phase: the frame is PRESENTED - `rhi::Device::RepeatPreviousPicture()` (add-only) makes the present
  pass and the captures read the buffer of the previous Present and keeps the rotation - so the vblank plans keep one
  Present (and one waitable signal) per frame; the draw summary says "shows the picture of frame M (...), repeated
  (previous present)" and a screenshot shows what is on screen. (Not presenting it let the next frame start as soon
  as its GPU work was done: 146 fps in the intro on the 120 Hz plan, 77 % of the frames on plan.) F2 `probe:Death::Render` 0x2BF10 (6 stolen bytes) sets the update time +0x20 to 0 after the original,
  F3 `probe:Session::OccluderFade` 0xCD340 (6 bytes) is skipped in frames without a step, F4
  `Session::RandomFlicker` 0x45EF0 (replacement) calls the guest's rand (0x1668CC, `CallGuest`) only in frames with a
  step (`xapi::FlickerValue`). Exit line `frame fixes (frames between ticks): ...` (reporter "frame fixes").
- Latency (M3c review finding 6): `pacing::LatencyBudget` (max(4 ms, p99 work + 3.5 ms), x2 per late frame up to x4,
  halved after a relax interval of 2 s; review finding 9: a late frame within one interval after a relax step doubles
  the interval up to `kMaxRelaxSeconds` 32 s, after `kMaxRelaxFailures` 2 such failures the factor holds until
  `NotePlanChange()` - called by the pacer on every plan change; `RelaxFailures()`, `Held()`),
  `pacing::JustInTimeStart`, `OnScreenTracker::PredictNextVisible(refreshSeconds, &qpc)`: on `vblank`/`display-rate`
  plans the next frame starts `budget` before the vblank at which it becomes visible.
- Display-limited detector (M3b, `pacing::DisplayLimitDetector`): `StartWarmup()` - the first `kWarmupWaits` (12) vblank waits after
  entering a vblank plan are not judged (verify phase: the transient of a composed window fired it at every start at
  120 fps).
- Window (`platform\window.h`): `platform::ViewLayout(clientW, clientH, fullscreen)` - in borderless fullscreen the
  view (swap chain) covers the whole monitor (independent flip, VRR, tearing); the present pass draws the 4:3
  picture with bars (`ComputeRenderSizes` fits 4:3 inside the view). Windowed unchanged.
- Logs: `game clock: 2 frames per tick (quanta 83333/83333 ticks ...)` / `1 frame per tick (... M3b)` / `display time
  per frame (...)`, exit `game clock M3d: <regime>; R regime changes, A phase B alignments, J samples after a
  just-in-time start; F rendered frames, S between ticks enabled: alpha 1/2 x a, 1 x b, other x c; snap-all: context
  switch w, clock reset z; interpolated over the step pair: catch-up x, 6 dt cap y; clock input: frame start x f,
  plan-change rephase x p`, `present: frame rate <fixed|120|display> (plan P Hz, N frames per tick); cadence frames
  1:a 2:b 3+:c; repeated pictures R (of T frames without a fixed step); just-in-time start on|off: J of V vblank
  frames (average delay D ms, budget B ms, L late; K failed relax steps[, budget held])`; pacing trace columns
  `frame_ticks, repeat, refreshes, jit_ms` appended.
- Review fixes (phase 5, `docs\m3d_report.md`): (8) `FramesPerTick` returns 1 up to the tick rate x (1 +
  `kIntegerTolerance`): "120"/"display" on 50/59.94/59.95/60.02 Hz run the M3b clock without interpolation like "60"
  (launcher `IsLogicRate` likewise). (10) catch-up/cap frames are no snap-all; `gfx::interp::FrameInterp::pairTicks`
  carries k and the renderer shows `interp(S[G-k], S[G], (k - 1 + alpha) / k)` for k <= `kMaxPairTicks` (6). (11)
  `FrameAdvance::frameStartQpc` (the pacer's release of the frame) and `planSerial` (every accepted plan change):
  in the sub-tick regimes the clock compares the frame start (fresh and <= the guest's sample, else the sample) and
  absorbs the start phase jump of a plan change once (`ClockQuantum::rephase`, `FrameClock` offset, at most one tick,
  never with N = 1; `Stats::rephases`, `Sample::rephased`).
- Launcher (`launcher\src\core\frame_rate.h`): `fl::EstimateFrameRate(value, refreshHz, tearing, tickRate)`,
  `fl::FrameRateNote` - the graphics tab names what "120" / "display" give on the selected monitor; the window size
  is disabled with a note while borderless fullscreen is on (M3c review nit).
- Tests: `tests\frameclock_tests.cpp` (exact guest accumulator model at QPF 10 MHz / 3.58 MHz / 24 MHz / 733 MHz,
  N = 1 vs `LockstepClock`, 1/0 pattern and alpha 1/2 / 1, events, regime changes, display-time quanta, fix
  targets vs the XBE), `tests\pacer_m3d_tests.cpp` (rate choice, cadence, plans, advance, budget, prediction,
  config/schema, `ViewLayout`), launcher `MiscTest::frameRateEstimate`, `UiTest::dependentKeysAndIsoStatus`.

### 4.25 Renderer interpolation between logic ticks - `gfx\interp_frame.h`, `gfx\interp_history.h`, `gfx\interp_math.h` (M3d, interpolation agent)

Design and evidence: `analysis\loader\m3d_interpolation.md` (German). The header comments are the reference. The
renderer shows `interp(S[G-1], S[G], alpha)` of the frame clock's values (section 4.24); `alpha >= 1 - 1e-3` and every
frame while the clock produces no frames between ticks (60 fps) pass the guest pointers through untouched (zero
visual change; unit test `interp history: 60 fps and interpolation off pass every camera and draw through
unchanged`).

- Source (`interp_frame.h`): `gfx::interp::FrameInterp {tick, stepsThisFrame, pairTicks, alpha, flags, enabled}`,
  flags `kFrameInterpSnapAll` / `kFrameInterpInsideStep` / `kFrameInterpClockReset` / `kFrameInterpCatchUp` (same bits
  as `xapi::`), `kMaxPairTicks` 6;
  `SetFrameInterpSource(fn)` (registered by `xapi::StageApplyGameTiming`), `CurrentFrameInterp()`,
  `StubFrameInterp()` (no source: `enabled = false`), `FrameInterpSourceName()`. Developer source
  `FUT_INTERP_STUB=steps` (environment, only without a registered source): 1 step -> alpha 1/2, 0 steps -> 1, >= 2 ->
  snap-all (`StepsFrameInterp`, pure).
- History (`interp_history.h`): one `History` per `GpuRenderer` (D3D11 path only; null renderer and `validate`
  unchanged). Calls: `SetCameraData` -> `Camera(location - 0xA0, in)` (latches the frame values, swaps the tables
  when the tick advanced; returns host copies of the interpolated camera), `DrawGeometry` (slots 48-51, not for
  texture targets) -> `Geometry(kind, in, arena)` -> `GeometryResult {input, reason, tint}` (transform and skin
  instance replaced by guest-visible arena copies when interpolated; `ni_extract` unchanged), `PrepareFrame` ->
  `BeginFrame(frame, traceFrame)`, `DisplayFrame` -> `EndFrame(frame, traceFrame)` before the arena reset,
  `GpuRenderer::LogStatistics` -> `LogStatistics()`.
- Keys and snaps: key (object = transform - 0x70, camera, occurrence), signature (vtable, data, skin, property state),
  two generation-stamped open-addressing tables (Fibonacci hash on the high bits); interpolation only from exactly
  G-1. `SnapReason` (None, Disabled, AlphaOne, Static, FirstSight, GenerationGap, Identity, Destroyed, Translation,
  Rotation, Scale, Mirror, NotOrthonormal, FrameDriven, SnapAll, CameraCut, BoneCount); `SnapLimits` (rules R2/R4 of
  analysis 3.4, camera cut rule 3.7, frame-driven hold 120 frames).
- Stage 1b, hierarchy (`[render] interpolation_hierarchy`, default on): every recorded draw records its parent chain
  (`NiAVObject +0x14`, `kMaxHierarchyDepth` 16) once per generation in two node tables keyed (node, camera); an
  interpolated draw gets `W_parent(alpha) * interp(L_prev, L_cur, alpha)`, `L = W_parent^-1 * W`, the topmost node
  with history in world space; node motion is evaluated once per generation (growth rule of the next tick), the
  interpolated world once per frame. Reason (measured, two-tick midpoint test against the real middle state, frontend
  fly-in at 640x480): world interpolation p99 13.6 px / max 108.6 px, hierarchy p99 0.09 px / max 17.5 px; steady
  frontend 0.41 / 1.13 px -> 0.04 / 0.11 px.
- Skinning: relative bone poses `root^-1 * bone` interpolated per bone, the original `FUN_0014F710` deforms a
  substitute `NiSkinInstance` in the arena (+0x08 real data, +0x0C identity root, +0x10 substitute bones with the pose
  at +0x70); self-test of the first 4 skinned draws against the real instance (log `interpolation: skin self-test`).
- Frame-driven objects (changed within one tick generation, `g_frameDelta` animation in Render) and cameras are drawn
  as given for 120 frames after the last change.
- `probe:NiAVObject::Destroy` on `FUN_00102CE0` (thiscall, 7 stolen bytes `6A FF 68 93 A4 17 00`): records destroyed
  addresses while some History records (a key captured before the destruction snaps, `Destroyed`). Installed always
  (hooks are installed only by the HostMain stage, before the configuration of the frame rate is known); at 60 fps it
  only calls the original. `[hooks] disable = probe:NiAVObject::Destroy` removes it.
- Config (schema rows, de/en): `[render] interpolation` (bool, default on, graphics tab, advanced),
  `interpolation_hierarchy` (bool, default on, developer), `interpolation_debug = off | tint | trace` (developer):
  `tint` = snapped draws get an extra texture stage multiplying with TFACTOR (red jump/identity/gap, yellow first
  sight, cyan frame-driven, magenta snap-all; `TintColor`, `ApplyTint`), `trace` = `<logs>\interp_trace.csv` (one
  line per frame), for the `[render] trace_frames` frames also `<logs>\interp_draws.csv` (one line per draw: reason,
  hierarchy, motion, prev/cur/shown/world-lerp translation), `<logs>\interp_nodes.csv` (node worlds of each traced
  tick) and log lines `interp camera frame N: ...`.
- Logs: start `interpolation: on ([render] interpolation_hierarchy = on, interpolation_debug = off; frame clock
  source clock; ...)` | `off (...)`, `interpolation: active from frame N ...` / `inactive from frame N (...)`,
  `interp frame N: tick G alpha A, D draws (I interpolated [H along the hierarchy, N nodes], ...)` for the
  `trace_frames` frames, exit (reporter "gfx"): `interpolation: N frames (R recorded, M with alpha < 1); draws ...`,
  `interpolation: cameras ...; skinned draws ...`, `interpolation: G generations ...; cost average A us, p99 P us, max
  X us per recorded frame`, `interpolation: hierarchy: ...`.
- Review fixes (phase 5, `docs\m3d_report.md`): (1) jump rule: `MotionReference` / `MotionSnapRule` - the reference
  is the larger of the last two known per-tick motions (`Entry::refStep/refAngleCenti`, `NodeEntry::refStep/refAngle`,
  `BoneRecord::refStep`); with an unknown previous motion only steps above `SnapLimits::unknownStepRadii` (16) x r or
  angles above `unknownAngleDeg` (90) snap; with the hierarchy the parent chain decides the jump of a drawn object that
  has a parent with history (only a node whose own local transform jumped snaps; `kChainSnap` marks it and its
  descendants; `FrameCounters::chainDecisions`). (3) a recapture of an address destroyed since the earlier capture
  restarts the entry (no frame-driven mark; `recaptureDestroyed`); an entry captured after a destruction in the same
  frame (`kAfterDestroy`) ignores that destruction. (4) `Camera()` checks the destroyed registry (`CameraEntry::
  destroyedCheckFrom`, camera reason `Destroyed`). (6) the readable-page cache of the hierarchy walk is dropped when
  `guest::MemoryReleaseEpoch()` moved (`kernel\mm.cpp` calls `guest::NoteMemoryReleased()` on every release or
  decommit; `Stats::readableCacheResets`). (10) a tick advance by exactly `pairTicks` = k (2..6) is a pair: motions
  are compared per tick (step / k), alpha = (k - 1 + clock alpha) / k (`Stats::pairSpans`). New exit-line fields:
  `recaptures after a destruction`, camera `destroyed`, `catch-up pairs interpolated`, `jump verdicts of the chain
  against the leaf rule`, `readable-page cache reset`.
- Not covered (documented open points): slots 52/53 are not drawn by the port (P2; `InterpolatePoints` in
  `interp_math.h` for particle arrays with stable indices), lights / `NiTextureEffect` (stage 3), `NiOutline`
  silhouettes (stage 4), culling guard band (after M4b); UV/alpha/material/flipbook/visibility animations and screen
  polygons/textures (HUD, menus, intro) stay at the tick rate by design.
- Tests: `tests\interp_math_tests.cpp` (quaternion round trip and Shepperd branches, slerp, fast paths, matrix checks,
  rules R2/R4 with the measured cases, Inverse/Compose vs `FUN_0014D880`/`FUN_0010DFC0`, camera, particles),
  `tests\interp_history_tests.cpp` (60 fps pass-through, phase B, snaps, frame-driven keys, occurrences, cameras,
  skin substitute, hash spread, growth, tint, developer source, config, hierarchy: arc vs chord, root equivalence,
  re-parenting, node motion after a snapped tick; review: oscillation reversal S10, first sight S1, rigid assembly
  S2, pointer reuse S6, camera reuse S11, step pairs, readable-page cache).

### 4.26 Controls configuration: `[input]` keys and binding strings (M4, written first by the launcher agent)

Shared contract between the input layer (`input\**`, which reads the keys) and the launcher's "Steuerung" tab
(which edits them). Basis: `analysis\loader\m4_input.md` 8.6 and 9 (bindings target **Xbox controls**, not game
actions: the game's action table is fixed). If the input agent's parser needs a different spelling, it changes this
section and the launcher follows (changelog row); until then this is the format the launcher writes.

- **Keys** (section `[input]`, schema `ui.tab` = `controls`): one key per device and Xbox control,
  `pad_<control>` (Xbox controller / XInput) and `kbm_<control>` (keyboard and mouse), 24 controls:
  `a b x y white black lt rt start back dpad_up dpad_down dpad_left dpad_right ls_click rs_click ls_up ls_down
  ls_left ls_right rs_up rs_down rs_left rs_right` (stick directions are half axes; up = +Y as in XInput/Xbox).
  Schema: type `string`, `ui.widget` = `binding` (new widget name), label `"<control> – <meaning>"` in de/en with an
  en dash (`"A – Bestätigen / Angriff 1"`, `"A – Confirm / Attack 1"`; the launcher splits at the first " – "),
  the same `ui.order` for `pad_x` and `kbm_x` (the launcher shows two tables in that order). **Input agent (host
  schema):** `ui.order` is unique per tab (schema test), so the rows carry `pad_x` = 100 + 2i and `kbm_x` = 101 + 2i
  (i = position in the control list above): the order within each device table is the same, `kbm_x` = `pad_x` + 1.
- **Value** = binding list: tokens separated by `,`, blanks around tokens and around the `:` ignored
  (`key : a` = `key:a`), case-insensitive, empty = not bound (the key is present with an empty value; a missing key
  means the default), at most 4 different tokens per key (duplicates are ignored and not counted; M4 review); the
  launcher writes lower case, `", "`-separated, without duplicates. `f<n>`, `num<n>` and `vk_<xx>` take digits only
  (no sign, no blank); `tests\input_bindings_tests.cpp` and `launcher\tests\tst_bindings.cpp` share the edge cases. Two lists with the same tokens in another
  order or case are the same binding.
  `pad_*` keys take only `pad:` tokens, `kbm_*` keys only `key:` and `mouse:` tokens; anything else is a
  configuration error (launcher: inline error, saving blocked; host: `core::ValidateConfigValue` calls the row's
  `ConfigKeyDef::validate` = `inp::ValidatePadBindingValue` / `ValidateKbmBindingValue`, exit code 1 with
  `invalid value for [input] kbm_a: 'pad:a' ('pad:a' is not a keyboard/mouse (key:, mouse:) token)`).
  The host's parser (`input\bindings.h`) is the reference: `key:vk_<xx>` of a named key is the same token as the
  name (`key:vk_41` = `key:a`, formatted as `key:a`).
  - `pad:<name>`: `a b x y lb rb lt rt start back dpad_up dpad_down dpad_left dpad_right ls_click rs_click` and the
    half axes `lx+ lx- ly+ ly- rx+ rx- ry+ ry-` (`ly+` = left stick up). LB/RB are the Xbox WHITE/BLACK buttons.
  - `key:<name>` (Windows virtual-key codes, position names of the US layout): `a`..`z`, `0`..`9`, `f1`..`f24`,
    `space enter escape backspace tab capslock lshift rshift lctrl rctrl lalt ralt lwin rwin menu up down left right
    insert delete home end pageup pagedown pause scrolllock numlock printscreen`, `num0`..`num9`, `num_add num_sub
    num_mul num_div num_dec`, the OEM keys `minus plus comma period semicolon slash grave lbracket backslash rbracket
    apostrophe oem102` (VK_OEM_MINUS 0xBD, _PLUS 0xBB, _COMMA 0xBC, _PERIOD 0xBE, _1 0xBA, _2 0xBF, _3 0xC0, _4 0xDB,
    _5 0xDC, _6 0xDD, _7 0xDE, _102 0xE2), and `vk_<2 hex digits>` for any other code 0x01..0xFE. `enter` includes
    the numpad Enter; `lshift`..`ralt` are VK_LSHIFT 0xA0..VK_RMENU 0xA5.
  - `mouse:<name>`: buttons `left right middle x1 x2`, wheel `wheel_up wheel_down` (one notch = pressed for 2
    ticks), movement `move_up move_down move_left move_right` (accumulated until the tick's sample).
- **Meaning** (input layer): bindings of one control are OR'ed (buttons) or the largest deflection wins (axes);
  a digital source on an analog control = full value (trigger 255, half axis 32767); an analog source on a digital
  control = pressed at half deflection or more; `mouse:move_*` on a stick direction goes through
  `mouse_sensitivity` and the inverse game curve (m4_input.md 8.4), on a button it presses while the mouse moves.
- **Other `[input]` keys**: `pad_deadzone` int 0..50 (percent of the physical stick range cut away before the game's
  own 0.4 dead zone, default 0; input agent: radial - a stick inside the circle reads 0, outside it passes unchanged,
  not rescaled), `mouse_sensitivity` float 0.1..10.0 step 0.1 (default 1.0), `rumble` bool
  (default 1), `rumble_strength` int 0..100 percent (default 100), `pad_mode` choice `single` (all controllers +
  keyboard/mouse on port 0, default) | `per_port` (one port per controller). Sliders use `ui.widget` = `slider`
  (new widget name; int/float with min/max) and may carry the optional field `"unit"` (shown after the value, e.g.
  `"%"`, `"×"`). `[debug] input_script` belongs to the input layer (path_file).
  **Added by the input agent** (bool, tab `controls`, widget `check`): `mouse_capture` (default 1: a click into the
  window captures the mouse - cursor hidden and confined, released when the window loses the focus; in borderless
  fullscreen captured whenever the window gets the focus; mouse input counts only while captured), `menu_escape`
  (default 1: in the game context `Frontend` the keyboard/mouse START acts as BACK, m4_input.md 9.2 "Esc
  kontextabhängig"; Ingame it stays START = pause), `background_input` (default 0, advanced: controllers also work
  without focus). Mouse scale: `mouse_sensitivity` 1.0 = 24 counts of movement within one logic tick give a full
  stick deflection (`inp::kMouseCountsPerFullDeflection`).
- **Defaults** (m4_input.md 9): pad = identity (`pad_a = pad:a`, `pad_white = pad:lb`, `pad_black = pad:rb`,
  `pad_ls_up = pad:ly+`, ...). Keyboard/mouse: `ls_up/down/left/right = key:w / key:s / key:a / key:d`,
  `rs_up/down/left/right = mouse:move_up / move_down / move_left / move_right`, `dpad_* = key:up/down/left/right`,
  `a = key:enter, mouse:left`, `b = key:space`, `x = key:f, mouse:right`, `y = key:e`, `rt = key:lshift`,
  `lt = key:q`, `white = key:1, mouse:wheel_down`, `black = key:2, mouse:wheel_up`, `rs_click = key:v,
  mouse:middle`, `ls_click = key:c`, `start = key:escape`, `back = key:backspace`.
- Launcher side (`launcher\src\core\bindings.*` parser/formatter with the same rules and the same key-name table as
  `input\bindings.cpp`, `core\pad_reader.*` XInput loaded at run time (`xinput1_4.dll`, fallbacks `xinput1_3` /
  `xinput9_1_0`) + `SimulatedPadReader`, `ui\controls_panel.*`, `ui\binding_capture.*`, `ui\pad_poller.*`): tab
  "Steuerung" after "Grafik" with one binding table per device (rows grouped face buttons / shoulders / START+BACK /
  d-pad / left stick / right stick, within a group by `ui.order`), "press a key/button" capture (Qt key events with
  the native virtual-key code, left/right Shift/Ctrl/Alt from the scan code; mouse buttons, wheel and four movement
  buttons; controller buttons, triggers and half axes from XInput polling, inputs held when the dialog opens count
  only after a release), add (max. 4) / clear / per-key and per-device reset, conflict warnings (one physical input on
  two controls of the same device; allowed, the host ORs them), invalid lists as errors (saving blocked), sliders for
  `ui.widget` = `slider`, and a live controller test (4 slots, buttons, sticks, triggers, which Xbox controls the pad
  bindings produce, 0.6 s rumble test at `rumble_strength`; connected slots polled every 16 ms, empty ones once per
  second, only while the tab is visible).
  **M4 review fixes:** left/right Ctrl/Alt in the capture come from the `WM_KEYDOWN`/`WM_SYSKEYDOWN` message itself
  (lParam bit 24 via a native event filter; Qt's `nativeScanCode` lacked the extended bit), Print Screen is captured on
  the key release; the controller capture says when no controller is connected (the countdown waits) and offers every
  `pad:` token in a list; F12/Print Screen (screenshot) and Alt with Enter bound (fullscreen) warn on the row; A, START
  and the left stick without a binding warn; resets of several rows ask first (`ui::ConfirmChange`); a replacing
  capture names the list it replaces; sliders show and keep a configured value off the step grid until moved; the
  "Für das Spiel" line of the controller test follows the host (`GameControlsForPad`: analog controls from 30/255,
  digital from 0.5, sticks after `pad_deadzone` and outside 0.4); with `menu_escape` on, the keyboard START row reads
  "Pause (in Menüs: Zurück)". Valid lists are written in canonical form (also via `--set`). A schema
  without binding keys (game builds before M4) shows the tab with a note and the controller test only.

### 4.27 Input interface and scripted input - `input\input_api.h`, `input\input_script.h` (M4, test-automation agent)

**Shared interface `input\input_api.h`** (first writer: test-automation agent; header-only, so registering needs no
link dependency; extensions additive with a changelog row - the input agent added `InputSource::HasMotors`,
`InputRouter::PortEpoch`, `InputRouter::SimulateUnplug`):

- `inp::PadState { buttons (kButton* = XINPUT wButtons bits), analog[8] (AnalogButton order A B X Y BLACK WHITE LT RT,
  0..255), thumb[4] (LX LY RX RY, -32768..32767, +Y up) }`, `MergePad` (buttons OR, analog max, larger deflection per
  axis), `RumbleState { left, right }` (0..65535), `kPortCount` 4, `kAnalogPressThreshold` 30.
- `inp::InputSource` (`Name`, `Kind` Physical/Script/Synthetic, `DefaultPort` (0 = player 1), `Connected`,
  `Sample(port, tick, out)` once per logic tick inside the game's `XInputGetState`, `SetRumble`); registry
  `RegisterSource` / `SourceCount` / `SourceAt` (static registration, at most 16).
- `inp::InputRouter` (`ConnectedPorts`, `SampleGamePort`, `SetGameRumble`, `Divert(port, InputConsumer*)` for the M4b
  freecam: a diverted port stays connected and neutral for the game, held controls pass again only after a release),
  `SetRouter` / `GetRouter`; `inp::InputConsumer::Consume(port, tick, state)`.

**Scripted input `[debug] input_script = <file>`** (`input\input_script.*`; format reference
`tools\input_scripts\README.md`, analysis `analysis\loader\m4_input.md` 10):

- The script is the `InputSource` "input script" (`SourceKind::Script`, port 0 = "port 1" of the game), registered at
  static initialisation and inactive without the key. Loaded and parsed by `inp::StartInput` (`script::PreloadScript`,
  before any XPP or router lock is taken); a missing file or a syntax error ends the process with exit code 1
  (`input script: <file>: line N: ...`). (Before the M4 review the script loaded lazily inside the first
  `XGetDevices`, and an error ended the process under `XppDevices::mutex_`: 0xC0000409 instead of 1.)
- Time base = the logic tick: one player step per tick. The tick heartbeat is `probe:Game::HotplugCheck`
  (FUN_0009C140, called once per tick by CApp vf10, return 0xD25D8); `Sample` steps at most once per heartbeat and
  the heartbeat itself steps the player while the script holds its port unplugged (the game then does not call
  `XInputGetState` for the port). Without heartbeats every sample is a tick. Seconds are game seconds (ticks /
  `[game] tick_rate`), `f` counts `CApp::Frame` calls. With the lockstep frame clock a scripted phase plays the same
  tick for tick, but absolute ticks drift by a few (measured up to 18) across intro and loading (I/O and QPC time):
  scripts synchronise on conditions, not on absolute times (`docs\loader_design.md` 8.11 E14).
- Commands `press/hold/set/release/wait/at/expect/loop ... until/repeat/end/log/screenshot/unplug/replug/quit/fail`;
  conditions `context <name>`, `log "<text>"` (log observer, `core::AddLogObserver`), `main_menu`, `dialog`/`no_dialog`,
  `level_loaded`, `cutscene`, `paused`, `controllable`, `active_pad`, `rumble`, `true`/`false`, each negatable with
  `not`. `quit` and the end after a failure take effect at the next tick heartbeat (outside the XPP entry points: the
  input layer's exit reporters take the locks the `XInputGetState` call stack holds).
- New exit code **7** `inp::script::kExitInputScriptFailed`: a timeout, a failed `expect` or `fail` logs
  `input script: FAILED at line N: ...`, requests a screenshot "fail" and ends the process two frames later.
- Log lines (read by `tools\m4_check.py`): `input script: loaded <file>: N instructions, ...`, `input script: started`,
  `input script: line N: <command or event> (tick T, frame F)`, `input script: screenshot "<name>" requested
  (frame F)` (then the renderer's `screenshot: <path> (WxH)`), `input script: rumble port 1 left L right R`,
  exit reporter `input script summary: <state> at line N, T ticks sampled, P presses, ...`.
- Observer probes (logged on INP/info even without a script): `probe:Game::MainMenuActivate` FUN_00020150
  (`game: main menu active (menu 0x..., entry N)`), `probe:Game::LevelReadyTest` FUN_000C3E20 at its call 0x2AB2F
  (`game: level loaded (level object 0x..., frame F)`), `probe:Game::PauseOpen` FUN_000A8220 (`game: pause opened
  (pause object 0x..., called from 0x...)`; 0x0002ACDB = the active pad was removed), `probe:Game::HotplugCheck`
  FUN_0009C140 (silent). Addresses and offsets: `core\guest.h` section "added in M4".
- `inp::script::Parse`, `Player`, `Environment` are plain C++ for unit tests (`tests\input_script_tests.cpp`).

**M4 check** `python tools\m4_check.py --exe build\<dir>\bin\futurama.exe --config run\m4-scripts\debug\futurama.ini
[--runs N] [--fresh] [--no-rumble]` runs `tools\input_scripts\m4_newgame.txt` (Intro -> main menu -> "Neues Spiel" ->
Level1-1, play, simulated unplug, START resumes) and `tools\input_scripts\m4_rumble.txt` (options menu vibration
test) and checks M4-1..M4-8 (header of the script: contexts, level files, frames in the level, screenshots, tick
sampling, active pad, unplug pause from 0x2ACD6, rumble with fade, keyboard/mouse + XInput backend state); rendering
gaps in the level (non-drawn outcomes, generic no-op renderer slots, texture gaps) and undocumented VFS misses are
reported as information. Verified 2026-10-12 with the Debug build: ALL PASS (about 40 s + 21 s).

### 4.28 Input layer: router, XPP HLE, XInput and keyboard/mouse backends - `input\*.h` (M4, input agent)

Basis: `analysis\loader\m4_input.md` 2 (HLE contract), 3, 8 (layers, port model, sampling, mouse curve), 9 (defaults).
Files (input agent): `input\xpp_hle.*` (replaces `xpp_stubs.*`), `host_router.*`, `input_system.*`, `input_config.*`,
`bindings.*`, `stick_curve.h`, `xinput_source.*`, `kbm_source.*`, `kbm_window.*`; tests `tests\input_bindings_tests.cpp`,
`input_router_tests.cpp`, `input_backend_tests.cpp`.

- **XPP HLE** (`input\xpp_hle.h`, hook names `XPP::<entry>` unchanged): `XInitDevices` starts the input layer
  (`inp::StartInput`); `XGetDevices(Gamepad)` = `InputRouter::ConnectedPorts`, MU = 0; `XGetDeviceChanges` = differences
  since the last call, both bits when `InputRouter::PortEpoch` changed while the port stayed connected (re-connection);
  `XInputOpen` = a guest dummy object per port (`xdk::AllocGuestObject`, never NULL for ports 0..3);
  `XInputGetState` = `SampleGamePort` (once per tick and open port) with a packet number that changes with the state,
  0x48F + zeroed state without device, 0x57 for an invalid/closed handle; `XInputSetState` -> `SetGameRumble`,
  `Header.dwStatus` set synchronously to the result (0 / 0x48F / 0x57, never 0x3E5), `hEvent` ignored (the game never
  writes it); `XInputClose` stops the port's motors. Logic class `inp::XppDevices` (router getter injectable),
  `inp::HotplugTracker`, `inp::GlobalXpp()`.
- **Router** `inp::HostRouter` (installed with `SetRouter` at DLL load; `inp::GlobalRouter()`): sources from the registry
  (or `UseSources` in tests), each on `DefaultPort()`. A port is connected while one of its sources is; keyboard/mouse
  is always connected, so port 1 never reports a removal (no "connect the controller" pause without a controller).
  Physical sources count only while the game window is in the foreground (`background_input`); after the focus comes
  back and after a diversion ends, controls still held pass only after a release (buttons, analog >= 30, axes beyond
  0.2). Rumble: `rumble`/`rumble_strength` scale, muted without focus, on diverted and unplugged ports, sent to the
  most recently used source with motors (`InputSource::HasMotors`; before any use: all of the port), sources without
  motors get the game's values unscaled; changes only. "Used" means a deliberate input - a button, an analog button
  from 30/255 or a stick beyond 0.2 - not merely a non-zero state (resting stick noise of a second pad; M4 review). `SimulateUnplug(port, on)` (test runs: removal -> in-game
  pause + ERRCTRLINGAME), `RefreshFocus`, `StopAllMotors`, `Stats(port)`.
- **Port model** (`pad_mode`): `single` - all XInput controllers + keyboard/mouse + script on port 1 (merged);
  `per_port` - XInput user n on port n+1, keyboard/mouse and the script on port 1.
- **XInput** (`input\xinput_source.h`): `xinput1_4.dll` (fallback `xinput9_1_0.dll`) loaded at run time
  (`inp::SystemXInput()`, `XInputApi` interface for test doubles); one `XInputPadSource` per user index; empty slots
  probed at most once per second, connected but unsampled slots every 250 ms; pad_* bindings + radial dead zone;
  analog triggers -> analog LT/RT 0..255; rumble re-sent after a reconnection, motors off at exit/crash.
- **Keyboard/mouse** (`input\kbm_source.h`, `input\kbm_window.h`): Raw Input (keyboard 1/6, mouse 1/2, foreground
  only, legacy messages kept) on the frame window's UI thread via a window subclass installed from the guest thread
  (`SetWindowLongPtrW`) + a registered message; taps shorter than a tick last one tick, wheel notch = 2 ticks pressed
  + 1 released (3 queued at most; wheel deltas are summed per direction, a notch is queued per 120 units, so
  high-resolution wheels give one notch per detent - `KeyboardMouseSource::WheelDeltaEvent`), movement accumulated
  until the tick; digital stick directions (W+D) are limited to the unit circle like a pad on its round gate
  (0.707 per axis on a diagonal); focus loss (WM_ACTIVATE inactive,
  WM_ACTIVATEAPP, WM_KILLFOCUS) releases everything and the capture; Enter while Alt is held is not passed on (Alt+Enter
  = fullscreen). `KeyboardMouseSource::ReadRaw` = raw keys + mouse delta for later consumers (M4b freecam).
  `inp::NormalizeRawKey` (left/right modifiers).
- **Mouse look**: wished deflection -> raw stick value through the inverse of the game's curve (`input\stick_curve.h`
  `GameStickOutput` / `GameStickInverse` / `ThumbFromUnit`; dead zone 0.4, 0.2 when the other axis is beyond 0.4,
  gain 1.3; a consistent pair of dead zones is chosen, the curve is asymmetric for small diagonals).
- **Log** (INP): `input: XPP HLE active - pad mode ..., XInput xinput1_4.dll (N of 4 controllers connected),
  keyboard/mouse ..., rumble on 100 %, ...`, `input: keyboard/mouse attached to the game window (Raw Input, ...)`,
  `input: port N connected|disconnected (connection #E)`, `input: XInput controller N connected|disconnected`,
  `input: XGetDeviceChanges reports insertions 0x.., removals 0x..`, `input: first rumble request of the game on port N:
  left L right R ...` (later on/off changes at debug), `input: port N diverted to <consumer>` / `back to the game`,
  bindings per control at debug; exit reporter "input" (also stops the motors): `input summary: XInputGetState N (F
  failed), XInputSetState N, opens, closes, insertions, removals; port N: T ticks (A with input, D diverted, H held
  back), C connection(s), rumble R requests (Z non-zero, max L/R); controller N: ...; keyboard K / mouse M events
  (...)`; crash reporter "input" stops the motors.
- **Not tested with real hardware** (no controller at hand): XInput input, hotplug of real controllers and the feel of
  rumble. The real `xinput1_4.dll` is checked down to "connected or not connected"; everything else runs against
  `XInputApi` doubles and simulated sources. Keyboard: injected with `SendInput` into the running game (menus, new game,
  walking, jumping, pause); mouse: unit tests only.

### 4.29 Audio (M5): shared audio interface - `audio\audio_api.h` (written first by the DSOUND core agent)

Basis: `analysis\loader\m5_audio.md` (6 mapping, 7 threading, 8 traps). `audio\audio_api.h` (namespace `snd`) is the
shared interface of the M5 audio agents; extend it additively (Changelog row).

| Part | Declared in `audio_api.h` | Implemented by |
|---|---|---|
| `Category` (0 effects, 1 speech, 2 music = game categories; 3 movie = untagged DSOUND objects), `CategoryName` | yes | DSOUND core (`audio_engine.cpp`) |
| `SourceFormat`, `PcmFormat`, `Decoder` + `CreateDecoder` (PCM8/PCM16/Xbox ADPCM -> PCM16) | yes | decoder owner |
| `MixbinGain` (1/64-dB register, clamp 63.98 dB), `MillibelToGain`, `MixBin` numbers | yes | DSOUND core (`audio_engine.cpp`) |
| `Listener3D`, `Emitter3D`, `I3dl2Source`, `SpatialResult`, `Spatializer` + `CreateSpatializer` (3D level model 3.6, X3DAudio direction) | yes | 3D owner |
| `I3dl2Listener` (DSI3DL2LISTENER) | yes | - |
| `AudioEngine`, `SourceVoice`, `VoiceEvents`, `MixTap`/`TapPoint`, `EngineConfig`, `CreateAudioEngine`, `EngineConfigFromSettings`, `Engine()`, `SetEngineForTesting` | yes | DSOUND core (`audio_engine.*`) |

Threading: DSOUND entries run on the guest thread; `VoiceEvents::OnBufferEnd` and `MixTap::OnMix` run on the mixer
thread and only queue (design E10). DSOUND core files: `audio\dsound_*.*`, `audio\audio_engine.*`, tests
`tests\audio_dsound_*.cpp`.

**DSOUND core (M5 DSOUND core agent; files `audio\dsound_hle.*`, `audio\dsound_mix.*`, `audio\dsound_probe.cpp`,
`audio\audio_engine.*`; tests `tests\audio_dsound_tests.cpp`).** `audio\dsound_stubs.*` are deleted; the 40 DSOUND
entries keep their hook names (`DSOUND::<entry>`, generated table unchanged) and are HLE now (`audio\dsound_hle.cpp`):

- **Objects**: guest handles (16 bytes, own pool of 4096 below 0x80000000; +0 = trap vtable for the device and buffers,
  the XMediaObject vtable for streams). `IDirectSound` singleton with a reference count, never torn down. One engine
  `SourceVoice` per buffer / stream (PCM16 after `CreateDecoder`; `MaxFrequencyRatio = max(4, codec::MaxFrequencyRatio)`).
- **Buffers**: `SetBufferData` copies/decodes at once (the game may free its memory), stops a playing buffer and resets
  the cursor; `(0,0)` = empty. `Play` resumes at the remembered cursor (loop = whole buffer, `smpl` ignored), `Stop`
  remembers it; `GetStatus` = 1 / 5 (looping) while playing, **0 at once** after `Stop`, `SetBufferData` and the end
  (end detected through `OnBufferEnd` of the current submission, no tick delay), always written.
- **Streams**: `Process` copies + decodes the packet and writes `*pdwStatus = E_PENDING` (and `*pdwCompletedSize = 0`)
  synchronously; a full queue returns `0x88780032` without touching the status. Finished packets are written **only in
  `DirectSoundDoWork`** (guest thread): completed size, status 0, then the completion event (`krnl::LookupEvent`) or
  the callback (`core::CallGuest(cb, {context, packetContext, status})`, after the DSOUND lock is released). `Pause`
  0/1/2 (2 = wait for `SynchPlayback`, which starts all of them in one engine batch); `FlushEx`/`Flush` abort every
  queued packet with `E_ABORT` at once and clear PLAYING; the last `Release` destroys the voice synchronously, writes
  `E_ABORT` into the still queued packets inside the call and nothing afterwards. `Discontinuity` ->
  `SourceVoice::Discontinuity`. Streams are created running (the game sends `Pause(0)` anyway).
- **Start at the commit** (M5 phase 5, review finding 1): `Play`, `Pause(0)` and stream creation change the host state
  at once (`GetStatus` 1/5, PLAYING) but only mark the object `startPending`; the engine voice starts in
  `ApplyAllDirty` (`CommitDeferredSettings` 0xCBE95 or `DoWork`) in the same engine batch as the levels, pitch and
  mixbins the game sets after the call (SoundVoice::vf11 calls Play 0xCC72E / Pause(0) 0xCC711 before SetVolume,
  SetFrequency, SetMixBins, SetI3DL2Source). `Stop`/`Pause(1|2)`/`SetBufferData` before the commit cancel it;
  `SynchPlayback` starts at once (XMV). `ObjectView::startPending` for tests.
- **Levels**: 2D voices by `audio\dsound_mix.h` (`Compute2D`: lVolume - 600 mB headroom + mixbin volume per bin,
  1/64-dB register, channel c serves bins `[c*n/ch, (c+1)*n/ch)`, Xbox bins -> host speakers), 3D voices by
  `Spatializer::Compute` with the committed listener/emitter. Values are applied per tick in one engine batch at
  `CommitDeferredSettings` / `DoWork` (changes only); a starting voice starts in that batch with the tick's levels
  (3D: its pending values). Frequency: `codec::FrequencyRatio(Hz, rate, spatial::DopplerPitch)` (Xbox pitch quantization).
- **Categories**: `probe:Game::SoundVoiceCreateDsObject` (0xCC150, thiscall, 6 stolen bytes `64 A1 00 00 00 00`) tags
  the object created inside it (category +0x70, 3D, stream, id +0x6C, name `[SoundData+0x20]`); untagged = movie.
  A second Play on a running SoundVoice is reported (`SoundEventKind::OrphanedObject`, counter); the old object plays on.
- **Host API** (`audio\dsound_hle.h`): guest structs (`XDsBufferDesc`, `XDsStreamDesc`, `XMediaPacket`, `XDsMixBins`,
  `XDsI3dl2Listener/Buffer`), status constants, the entries and `DsStream*` vtable methods (for tests),
  `SetPendingVoiceTag`/`ClearPendingVoiceTag`, `NoteOrphanedObject`, `IsDsoundDevice/Buffer/Stream`,
  `DsoundStreamQueuedPackets`, `InspectObject` (cursor, frames, applied ratio/matrix/send), `CommittedListener`,
  `CommittedReverb`, `GetDsoundStatistics`, `LogDsoundSummary` (also in `xdk::LogCallSummary`),
  `ResetDsoundForTesting`; `snd::SetSoundEventSink` (events of `audio_api.h`, guest thread).
- **Engine** (`audio\audio_engine.*`): `EngineFacade` over a backend, proxies remember parameters and queued buffers so
  that `Pump` (in `DoWork`) moves every voice to the software mixer when XAudio2 reports a critical error (device gone,
  e.g. TV off), back when an endpoint appears, and XAudio2 -> XAudio2 when the `[audio] device` that was missing comes
  back or (default device in use) Windows has a new default device (the playing buffer of every voice continues at
  its play position - frames played minus the frames of the buffers that ended since the last attach/flush, measured
  within +-0.08 ms on the streamed music - and the queued ones follow; DSOUND rebuilds the matrices if the layout
  changed). **Device worker** (M5 phase 5, findings 6/7): the host thread "audio device worker" (E10: no guest code)
  looks for endpoints, creates the new XAudio2 backend and destroys the replaced one; it registers an
  `IMMNotificationClient` that only counts device changes. `Pump` hands requests over and takes results without
  waiting: at once on a notification, every 3 s while no device is open, after failed opens of a present device 3, 6,
  12 .. 30 s (one WARN per run). With `capture_wav` active a reopened device keeps the capture's channel count.
  `SourceVoice::FramesPlayed` of the XAudio2 backend stays monotonic over `END_OF_STREAM` (Discontinuity) restarts
  (measured: inside a pass's callbacks `GetState` reports the pass start, the restart is visible from
  `OnVoiceProcessingPassEnd`; the base grows there by the value at `OnStreamEnd`, so the count is at most one pass,
  480 output frames, short after a stream end, and a read never steps back); a proxy takes its position base after a
  `Flush` at the next `Submit` (XAudio2 flushes asynchronously). XAudio2: mastering voice 48 kHz with `[audio] speakers`/device
  channels, one submix per category, reverb submix (1 input, `reverb::ReverbEffectChannels`, `reverb::ToNative`,
  `ReverbOutputMatrix`), pass-through tap XAPOs on every submix and the mastering voice (MixTap). **Master stage** after
  the Master tap: peak limiter at -0.1 dBFS (`[audio] limiter`; FXMasteringLimiter is not used - it adds a loudness
  makeup gain), a 120-Hz low-pass (4th-order Butterworth) on the LFE output channel before the Master tap (AC-3 carries
  LFE only up to 120 Hz; 3D voices feed their full-band signal into bin 3) and `[audio] master_volume` (XAudio2 applies a mastering voice's own volume before its effects, so the
  volume is applied in the tap XAPO). Software mixer (no device / `[audio] enabled = 0`): same graph, 16-tap
  windowed-sinc resampler (70 dB SNR at 1 kHz), `reverb::SoftwareReverb`, 10 ms blocks in real time or
  `RenderSoftware` (tests); while nobody listens (no tap and no output buffer: no device and no `capture_wav`) it only
  advances play positions and buffer ends (same frames, same `OnBufferEnd` moments; finding 15). An endpoint with
  more than 8 channels gets an 8-channel mastering voice (finding 12). `AudioEngine::TapFormat(point, channels, rate)`
  (additive, default false) tells the capture the block format of every tap point. `SourceVoice::SetOutputMatrix` takes [src][dst] (`src * outputChannels + dst`); the XAudio2
  backend transposes to XAudio2's `[dst * srcChannels + src]` (verified with a tap on a real device). Observer
  `snd::SetEngineObserver` (capture session); test hooks `SimulateDeviceLossForTesting`, `RetryDeviceNowForTesting`
  (asks the worker and waits for its answer), `SwitchToNewSoftwareBackendForTesting`, `EngineDescriptionForTesting`,
  `CreateSoftwareEngine`, `ParseSpeakers`, `DefaultChannelMask`. `audio summary: ...` is logged by
  `snd::LogDsoundSummary` inside the existing reporter "xdk" (no own exit reporter; `kMaxExitReporters` is nearly full).
- **Keys**: `[audio] enabled` (bool 1), `device` (string, widget `audio_device`), `master_volume` (0..100 %, 100),
  `limiter` (bool 1, advanced); `dummy_streams` stays accepted without effect (log line). `[audio] device` holds the
  `IMMDevice::GetId` string the launcher stores ("{0.0.0.00000000}.{guid}"), passed to `CreateMasteringVoice` as is; a
  device that is not available falls back to the default (one WARN per run). Developer key
  `device_test = off | absent | loss` (`EngineConfig::deviceTest`; M5 integration): `absent` = no XAudio2 and no
  device worker (as if Windows had no output device), `loss` = a simulated `XAUDIO2_E_DEVICE_INVALIDATED` 20 s after
  every XAudio2 start, back at the next 3-s look (checks without switching the TV off).
- **Logs** (SND/info): `audio: <backend description>, master volume N %` (backend description: `XAudio2 2.9, default
  device '<name>' | device from [audio] device '<name>', N ch (mask 0x..) 48000 Hz, reverb on|off, limiter on|off` or
  `software mixer (no output), ... N blocks positions only (no listener)`; INFO also without a device), `audio:
  output device lost ... | available again - ... | changed - ...`, `DSOUND: HLE device ...`,
  `DSOUND: effects image ...`, `DSOUND: I3DL2 listener ...`, `DSOUND: first buffer playing: "<name>" ...`, `DSOUND:
  first stream packet ...`; exit: `audio summary: ...; voices N created, N live, peak N; T s mixed; glitches N; device
  losses N; limiter active on N frames (max X dB)`, `DSOUND HLE: buffers ... plays ... orphaned ... streams ... packets
  N submitted, N completed, N flushed, N rejected; N format errors, N objects without voice`.

**Codec, 3D and reverb (M5 codec/3D/reverb agent; files `audio\codec_*.*`, `audio\spatial_*.*`, `audio\reverb_*.*`,
tests `tests\audio_codec_*.cpp`, `tests\audio_spatial_*.cpp`).** Implements `CreateDecoder` and `CreateSpatializer`
of `audio_api.h` and adds own headers for the DSOUND core and the engine:

| Header | API (namespace) | User |
|---|---|---|
| `audio\codec_adpcm.h` | `snd::codec::DecodeXboxAdpcmBlock`, `DecodeXboxAdpcm(data, bytes, ch, out, outFrames, padPartialBlock)`, `XboxAdpcmFrames`, `ConvertPcm8`, `AdpcmExpandNibble`, IMA tables. Variant "header sample + 63 nibbles, `((2n+1)*step)>>3`" = ffmpeg `adpcm_ima_xbox` bit-exact (synthetic golden hashes; 13 game files via `FUT_AUDIO_CODEC_REFS`); whether the Xbox hardware rounds the same way is not established (the IMA shift-add form differs by at most 28 LSB on the game's files, below -61 dBFS) | decoder, tests |
| `audio\codec_format.h` | `snd::codec::ParseWaveFormat(guestWfx, SourceFormat&, &error)` (ignores nAvgBytesPerSec / nBlockAlign / cbSize), `IsSupportedFormat`, `BlockBytes`, `FramesPerBlock`, `BytesToFrames`/`FramesToBytes` (buffer cursor, whole blocks); pitch model `XboxPitchFromFrequency` (= XAudioCalculatePitch 0x246ED1), `ClampXboxPitch` (-0x7FFF..0x1FFF, FUN_0024C5C7), `FrequencyFromXboxPitch`, **`FrequencyRatio(requestedHz, sourceRate, extraPitch)`** (SetFrequency -> `SourceVoice::SetFrequencyRatio`, 0 = source rate, doppler pitch added before the clamp), `MaxFrequencyRatio(rate)`, `PitchFromRatio` | DSOUND core |
| `audio\spatial_model.h` | `snd::spatial::DistanceMillibel` (FUN_00249893 exact, mB truncated like cvttss2si), `GainToMillibel`, `DefaultRolloffMillibel`, `HwMixbinGain` (1/64-dB register, <= 63.98 dB), `ComputeLevels3D` (direct / reverb / LFE / distance mB), `BuildX3dDistanceCurve` (8-point `X3DAUDIO_DISTANCE_CURVE`), `MakeListenerBasis` (right = top x front), `ToListenerSpace(pos, listener, basis, PositionalMode)`, `DopplerPitch` (DS3D, Xbox pitch units; always 0 in this game), `PositionalMode` + `ParsePositionalMode` | Spatializer, DSOUND core (doppler) |
| `audio\spatial_x3d.h` | `snd::spatial::SpatialSettings{positional, trim3dDb}`, `SpatialSettingsFromConfig()` (`[audio] positional`, `trim_3d`), `CreateSpatializer(settings)`, `DefaultSpeakerMask(channels)`. `snd::CreateSpatializer()` = settings from the configuration. `SpatialResult::matrix` index = **src * outputChannels + dst** (the `SourceVoice::SetOutputMatrix` order); LFE (game bin 3) only on outputs with `SPEAKER_LOW_FREQUENCY`; X3DAudio pans over **quad (FL FR + the rear pair: BL/BR, else SL/SR) or stereo** and the host maps the coefficients to the output channels - never over the front centre (the game's 3D mixbins have no centre bin; phase 5, finding 2) and only over standard masks (X3DAudio 2.9 returns channel 0 = 1 for every direction with non-standard masks such as 0x60B/0x603, which the first phase-5 fix produced on the TV's 5.1 side layout 0x60F); direction scaled to constant power but no channel above the front reference's 1 (0 dB, the Xbox register ceiling; stereo side = 1 on the near channel, finding 3); `reverbSend` = mono send gain. One instance per thread | DSOUND core |
| `audio\reverb_i3dl2.h` | `snd::reverb::GuestI3dl2Listener` / `GuestI3dl2Buffer` (48/36 bytes), `ReadI3dl2Listener`, `ReadI3dl2Source` (unaligned guest copies), `GameEnvironmentPreset(0..4)` (Padded Cell .. Concert Hall = table 0x2D6818), `ClampI3dl2`, **`ToNative(listener, sevenDotOne)`** (WetDryMix 100, always `ValidNative`), `IsSilent`; XAudio2 layout: reverb submix 1 input channel, `ReverbEffectChannels(out)` (1 or 6: the XAudio2 reverb only does mono->mono / mono->5.1), `ReverbSampleRate(out)` (20000..48000), `ReverbOutputMatrix(effectCh, outCh, m, n)` (effect -> output, src*out+dst), `CreateReverbEffect` (XAudio2CreateReverb); DSP image `ParseEffectsImage(image, size, info, &error)` + `ImageHasReverb(info, loc)` for `DownloadEffectsImage` (dsstdfx.bin: 10 effects, descriptor at 0x6110, I3DL2 = effect 0) | engine (`SetReverb`), DSOUND core |
| `audio\reverb_software.h` | `snd::reverb::SoftwareReverb`: the same reverb XAPO driven through IXAPO without an XAudio2 graph (`Initialize(rate, 1|6)`, `SetListener`, `Process(monoIn, out, frames)` in 10-ms blocks `BlockFrames()`, `Reset`) | engine's device-less software mixer |

Config rows (schema, `Tab::Audio`): `speakers` (auto\|stereo\|5.1, hidden 2.0; read by the engine), `reverb` (bool 1; engine),
`positional` (corrected\|xbox\|off; Spatializer), developer `trim_3d` (float dB -20..20, Advanced; Spatializer).

### 4.30 Audio capture, analysis tools and the launcher's Audio tab (M5, tooling/launcher agent)

Verification without ears (`analysis\loader\m5_audio.md` 6.8, 9): the final mix and the category submixes are copied
unchanged into WAV files and analysed against the original sound files, which the tools decode independently.

- **Keys** (`core\config_schema.cpp`, developer, Advanced tab): `[audio] capture_wav = <file>` (path, relative = INI
  folder; empty = off), `capture_format = float | pcm16` (float = exact copy), `capture_stems = 0|1` (one file per
  submix next to it: `<name>.music.wav`, `.speech.wav`, `.effects.wav`, `.movie.wav`, `.reverb.wav`), `event_log = 0|1`
  (`<name>.events.tsv`: `sample` (mix frames captured at the event), `seconds`, `wall_s`, `tick`, `kind`, `object`,
  `category`, `name`, `detail`). Schema additions for the other agents: `core::ConfigUiTab::Audio` (`"audio"`) and
  `core::ConfigUiWidget::AudioDevice` (`"audio_device"`: a string holding a Windows endpoint ID, empty = default
  device; meant for `[audio] device`; the launcher also shows a plain string `[audio] device` as a device list).
- **API** (`snd::capture`, `audio\capture_session.h`; building blocks `capture_wav.h`, `capture_tap.h`):
  `Session::Instance()`, `Start(settings, source)` / `StartFromConfig(source)` (writer thread; exit reporter
  `"audio capture"` finalizes the files and logs the statistics), `DeclareStem(stem, channels, rate, mask)`,
  `Push(stem, interleaved, frames, channels)` (real-time safe: SPSC ring of about 1 s per stem, a block that does not
  fit is dropped and counted, `nullptr` = silence), `CreateTap(stem)` (in-place XAPO for an XAudio2 effect chain; it
  declares its stem in `LockForProcess`, `XAPO_BUFFER_SILENT` is captured as zeros; XAudio2 applies a voice's volume
  before its effect chain, so a tap includes the volume of its voice), `NoteSource(Source::XAudio2 |
  SoftwareMix)`, `RecordEvent(Event)` (any host thread; `tick` ~0 = filled from `xapi::GetGameTickStats().steps`),
  `Stop()`, `Statistics(stem)`, `MixFrames()`. `Stem` = Mix, Music, Speech, Effects, Movie, Reverb;
  `StemForCategory(0/1/2/3)`. Plain helpers for tests: `WavFileWriter` (float32/pcm16/extensible, header rewritten
  about once per second so a file stays readable after a watchdog or fatal exit), `LevelMeter` (rms, active rms over
  100 ms blocks above -70 dBFS, peak, clipped |x| >= 1, gaps = digital silence >= 5 ms between sound), `FloatRing`.
  Engine glue `audio\capture_engine.*`: a `snd::EngineObserver` registered at static initialisation; when the process
  engine is created and `capture_wav` is set it starts `Session::Instance()` (source from `HasDevice()`), registers a
  `SessionMixTap` (`AudioEngine::SetTap`; TapPoint Master -> mix, category points -> stems, Reverb -> reverb; every
  stem is declared before from `AudioEngine::TapFormat`, so the mixer thread never allocates) and with
  `event_log` a `SessionEventSink` (`snd::SetSoundEventSink`; detail column `buffer|stream [3d] [loop] voice= mB= hz=
  pos= frame= level=` plus ` dist= min= max= curve=` for 3D objects: the last applied level in dB - 3D: lVolume +
  DSOUND distance term, <= -100 = muted beyond MaxDistance; 2D: lVolume - 600 mB - and the listener distance, Min/Max
  distance and rolloff points of the emitter; kind `level` = a playing 3D voice's applied level moved by >= 3 dB or
  into/out of the muted range, `SoundEventKind::Level3D`); `OnBackendChanged` notes the source switch. `AttachCapture(session, settings, engine, tap, sink)`
  for tests. The capture never touches guest memory (E10).
- **Exit lines** (SND/info, parsed by `tools\m5_check.py`): `audio capture: <file> - <rate> Hz, <ch> ch, <format>,
  source <xaudio2|software-mix>, <s> s (<frames> frames), <n> dropped, <n> mismatched, <n> truncated, files: <stems>,
  <n> events`; per stem `audio levels <stem>: <ch> ch, <s> s, rms .. dBFS (ch ../..), active rms .. dBFS over .. % of
  100 ms blocks, peak .. dBFS at .. s, clipped <n> (.. %), gaps >= 5 ms: <n> (longest .. ms at .. s; <list>), first
  sound at .. s, non-finite <n>, dropped <n>` (or `no data`); `audio capture taps: ...` (only when a `CreateTap` XAPO
  was used - the engine feeds the session through its own MixTap); `audio dsound calls: <entry>
  <count>, ...` (every DSOUND entry with `xdk::CallCount` != 0).
- **`tools\audio_compare.py`**: `--capture <wav> --config <ini> --ref <archive path> [--ref-file <wav>] [--timeline]
  [--events <tsv> [--stems]] [--window a:b] [--json out]`; references are read from the ISO's `.img` archives in
  memory (never written to the repository; `--cache-dir` only below `run\`) and decoded with PyAV/ffmpeg or the own
  Xbox ADPCM decoder (`--decoder own`, bit-exact with ffmpeg on synthetic blocks and on game files). Reports per
  occurrence: offset, NCC, pitch ratio, gain per channel, balance, residual SNR; `--timeline` follows a looping
  stream chunk by chunk (loop gaps, dropouts; its level per chunk with the reference resampled at the measured speed
  and aligned to a fraction of a sample, `aligned_gains` / `stretch_sinc`); capture overview (rms, peak, clipping,
  gaps). `--selftest` uses synthetic signals only.
- **`tools\m5_check.py`**: `--exe <futurama.exe> --config <ini with [paths] iso + userdata> [--runs frontend,level]
  [--device-optional] [--fresh] [--keep-wav] [--analyse-only]`; runs `tools\input_scripts\m5_frontend.txt` (main
  menu about 130 s, menu sounds, options) and the M4 level script with capture, stems and event list; criteria A1..A12
  (header of the script; A9 = 3D panning with sound louder on each side, A10 = 2D stems only on front left/right
  with a 5.1 output, A11 = 2D level -6 dB on the first start, music below 2 kHz +-0.1 dB (each 2-s chunk resampled
  at the measured speed, sub-sample aligned), menu sounds +-0.3 dB, m5_audio.md 3.6; A12 = limiter statistics consistent with the
  pre-limiter peak and `master_volume`) plus all M4 criteria on the level run (M4-7 rumble and M4-9 as extra runs). `--device-optional` accepts the
  software mixer when Windows has no active output device (this machine: the TV is the only endpoint).
- **Launcher** (`launcher\**`): tab "Audio" (`fl::UiTab::Audio`, between Grafik and Steuerung; section `[audio]`
  falls back to it) with the schema's keys, an `audio_device` editor (WASAPI render endpoints, "Windows-Standardgerät
  (<name>)" = empty value, unknown IDs marked "nicht angeschlossen"), and below them "Ausgabe prüfen": the device in
  use (a configured device that is not connected shows the default device the game falls back to), warnings (no
  active device - the game runs on silently and switches the sound on when one appears -, device not connected,
  more speakers selected than the device has channels: Windows downmixes) and "Testton abspielen" (`fl::TonePlayer`: a generated
  chime per speaker in channel order, then all, -12 dBFS times `[audio] master_volume`, through XAudio2 2.9 on the
  selected device with `[audio] speakers`, falling back to the default device like the game (`ToneResult::fellBack`);
  destroying the player stops a running tone at once; "Geräte neu suchen" is disabled while a tone plays; no game
  data). `--audio-file=<json>` (development, screenshots) replaces the
  device list. Core: `core\audio_output.*` (`EnumerateAudioDevices`, `ParseAudioDeviceList`, `MakeTestTone`,
  `SpeakersFromValue`, `VolumeFromValue`, `TonePlayer::SetBackendForTesting`), UI: `ui\audio_panel.*`.
- **Tests**: `tests\audio_capture_tests.cpp` (WAV headers and files, pcm16 conversion, level meter, ring, tap XAPO
  registration/negotiation/process, session files/stems/events/overflow/settings, the capture attached to the software
  engine, an XAudio2 submix-tap test that is skipped without an audio endpoint), `tests\schema_tests.cpp` (tab `audio`, widget `audio_device`); launcher
  `tst_misc` (`audioTestTone`, `audioDeviceList`, `audioValueMapping`), `tst_schema::audioTab`, `tst_ui::audioTab`.

### 4.31 Freecam (M4b) - `gfx\freecam.h`, `input\chord_filter.h`

- **Toggle**: `inp::HostRouter::SetSampleFilter(fn)` - called in `SampleGamePort` with the merged state before the
  divert check; the freecam's filter (`gfx::freecam::SampleFilter`) runs `inp::ChordFilter` (L3+R3, chord buttons
  held back up to 4 ticks, a short tap becomes a one-tick press) and the toggle key (`[freecam] toggle_key`, F6).
  Registered in `inp::StartInput` together with `gfx::freecam::Configure()`.
- **Input while active**: `InputRouter::Divert(port, freecam)`; `inp::KeyboardMouseSource::SetPadEmulation(false)`
  (no pad state from the keyboard, raw keys/mouse through `ReadRaw`).
- **Camera**: probe hook `probe:NiCamera::Click` 0x100C50 (`[hooks] disable` name). Swaps the scene camera's world
  pose and frustum for the call and recomputes the derived NiCamera data (vf39 tail), then restores them.
- **Renderer**: `gfx::RenderTargetOf(renderer)`, `gfx::ClearBackBuffer(renderer)` (`gfx
ipc_renderer.h`);
  `gfx::freecam::NoteCameraData(location)` from slot 47 and `gfx::freecam::SuppressOverlayDraw(target, geometry)` in
  slots 48-51/54/55 (HUD hidden).
- **Config**: `[freecam] enabled, toggle_pad (l3r3|off), toggle_key (off|f1..f12), move_speed, look_speed,
  mouse_sensitivity, invert_y` (launcher tab Steuerung).
- **Tests**: `tests\freecam_tests.cpp`; script `tools\input_scripts\m4b_freecam.txt`. Report: `docs\m4b_report.md`.

### 4.32 Video (M6): XMV HLE, movie textures, skip option - `video\*.h`, `gfx\d3d_shim.h`, `gfx\nipc_gpu.h`

- **Modules** (`src\host\video\`, all new except `xmv_hle.*`): `xmv_demux.h` (XMV demuxer: `vid::xmv::ParseFileHeader`,
  `ConvertExtradata`, `Demuxer::PushPacket` -> `VideoFrame` {index, ptsMs, keyframe, WMV2 data byte-swapped} and
  `AudioChunk` per track), `wmv_decoder.h` (`vid::WmvDecoder`: the Windows "WMVideo Decoder DMO" from wmvdecod.dll
  through `DllGetClassObject`, WMV2 in, planar I420 out; `WmvDecoderAvailable`), `movie_core.h` (`PacketSource`,
  `MemoryPacketSource`, `MovieCore` = source -> demuxer -> queues + decoder), `movie_clock.h` (`MovieClock`: audio
  clock from the stream's played frames, wall clock without audio and after the audio end; `FramesDue`,
  `MovieEndMs`), `yuv_pack.h` (`PackI420ToYuy2`, interstitial chroma), `xmv_hle.h` (the 7 entries,
  `IsXmvDecoder`, `LiveXmvDecoders`, `GetMovieStatistics`, `LogMovieSummary`), `movie_skip.h` (`SkipVideos`,
  `ParseSkipVideos`, `CutsceneSkipAllowed`, probe hook), `xmv_test_mode.h` (`DecodeMoviesJson`, `Crc32`).
- **Packets**: through the movie player's callbacks GetNextPacket 0xEDBA0 / ReleasePreviousPacket 0xEDE60
  (`guest::kMovieGetNextPacket`, `kMovieReleasePreviousPacket`), called from GetNextFrame on the guest thread like
  the original; each packet is copied at once and the next read started.
- **Audio**: `EnableAudioStream` creates a DSOUND HLE stream (format 0x69 of the track, 32 packets, no callback),
  `Pause(2)`; the first `GetNextFrame` queues ~0.6 s and starts it with `DirectSoundCreate` + `SynchPlayback` +
  `Release`; `GetAudioStream` returns it AddRef'ed (the game calls `SetVolume` + `Release`). `CloseDecoder`:
  `FlushEx` + `Release`. New additive DSOUND helper `snd::StreamFramesPlayed(stream, frames)` (audio clock);
  `DsStreamDiscontinuity` marks the end of data (no dropout warning afterwards).
- **Video**: `GetNextFrame` writes the newest due frame as YUY2 into the shim surface (`gfx::d3d::LookupResource`)
  and calls `gfx::d3d::NoteResourceWritten(surface)`; `ResourceInfo` gained `serial` and `generation`, the header
  exports `kFormatYuy2` (additive). Renderer: `GpuRenderer::MovieTexture(info, yuvEnable)` (BGRA8 host texture per
  shim resource, uploaded when the generation or the YuvEnable mode changes, `tex::ConvertYuy2ToBgra` BT.601 or
  raw), texel-addressed stages (UV 0..W x 0..H of the movie quad scaled by 1/W, 1/H, clamped). GFX_CONTRACT.md 15.
- **Results**: 1 new frame, 0 none due, 2 end (all frames shown and the clock past max(last pts + gap, audio end)),
  3 error (decoder unavailable or broken file: the movie ends, the game continues).
- **Config**: `[game] skip_videos = always | original` (default always, launcher tab Game): probe hook
  `probe:Game::CutsceneSkipAllowed` on FUN_000D3120 (`guest::kGameCutsceneSkipAllowed`, only caller 0x2ABD4).
  `[debug] movies = play | skip` (developer): `skip` = the M2-M5 stub behaviour (no picture, no sound), used by the
  frame-timeline checks m2/m3/m3c/m3d through `--set=debug.movies=skip`.
- **Tool mode**: `futurama.exe --decode-movies=<image|folder> --result=<file.json> [--movie-dump=<dir>]
  [--movie-filter=<text>]` (core/cli_tools.h): decodes every movie; per file the pts, a luma CRC-32 per frame and a
  PCM CRC-32 per track; `tools\m6_check.py` compares it with FFmpeg (PyAV) and analyses game runs (`--run`).
- **Logs** (VID): `XMV: movie #n: WxH, D ms, T audio tracks ..., packets up to 0x.. bytes (first 0x.., next 0x..)`,
  `XMV: movie #n playing: ... (track t), clock audio|wall (QPC)`, `XMV: movie #n ended|closed early (skipped) at N
  ms: F frames shown of D demuxed, X dropped (late), max L ms late, E decode errors, clock ...`, `movies: cut-scene
  skip asked (...): original yes|no ... -> skip|keep playing`, exit summary `XMV HLE: ...`; GFX `d3d11 movie
  textures (YUY2): ...`.
- **Tests**: `tests\video_tests.cpp` (demuxer on synthetic packets, movie core, YUY2 packing/conversion, clock,
  skip option, shim generation, CRC-32, optional decode of `analysis\loader\samples`), `tests\xdk_tests.cpp` (XMV
  entries without callbacks). Scripts `tools\input_scripts\m6_boot.txt`, `m6_level.txt`, `m6_attract.txt`.
  Report: `docs\m6_report.md`.

### 4.33 Save games (M7): save probes, `game level_end`, FATX listing order, free-space test key - `xapi\save_probes.h`, `vfs\vfs.h`

- **Unchanged path**: the game's save layer (0xE0040..0xE1500) and XAPILIB (`XCreateSaveGame`, `XDeleteSaveGame`,
  `XFindFirst/NextSaveGame`, `XCalculateSignature*`, `CopyFileA`, `WriteFileEx`, ...) run natively on the kernel HLE
  of M1/M2. Layout, container format, signature and flows: `analysis\loader\m7_saves.md`.
- **Observer probes** (`xapi\save_probes.h`, all default on, `[hooks] disable = probe:Save::*` etc.):
  `probe:XAPILIB::XCreateSaveGame` 0xF6987 (9 bytes), `probe:XAPILIB::XDeleteSaveGame` 0xF6BEA (9),
  `probe:Save::ReadContainer` 0xE0210 (7), `probe:Save::VerifySignature` 0xE0340 (5), `probe:Save::WriteContainer`
  0xE0B00 (6), `probe:Save::OpenSaveMenu` 0xE4F70 (7), `probe:Save::Dialog` 0xDF750 (7). Log lines (XAPI/info),
  meant for input scripts (`wait log "..."`) and `tools\m7_check.py`: `save: XCreateSaveGame(U:\, "<name>",
  OPEN_ALWAYS|OPEN_EXISTING, flags 0) -> <err> U:\<dir>\`, `save: XDeleteSaveGame(U:\, "<name>") -> 0 (directory
  removed)`, `save: read <path> -> ok|missing|wrong size (not 4096 bytes)|read error`, `save: signature of <path>
  ok|MISMATCH (the game reports it as damaged)`, `save: loaded game data: <Futurama.dat text>`, `save: write
  <file> for "<name>" (icon <xbx>) -> ok|FAILED`, `save: saved game data: <text>`, `save: save menu opened|not
  opened (...)`, `save: dialog MEMCARD_0xx (MEMHINT_00x|no hint)`, exit reporter `save summary: ...`
  (`xapi::GetSaveObserverStats`). Guest constants: `core\guest.h` section "added in M7".
- **Debug action** `xapi::RequestLevelEnd(nextLevel)` / `RunPendingGameActions()`: the input script command
  `game level_end <NextLevel>` (`inp::script::Op::Game`, `Environment::GameAction`, default false) asks for it; the
  tick heartbeat probe `probe:Game::HotplugCheck` runs it at the next tick, outside the XPP/router locks: cdecl
  `FUN_000B0520(nextLevel)` = the level-script native FID 0xA1 (FsChangeLevel) -> the game's Breakdown screen with
  "Speichern". Refused (script fails) while no level object exists. FsGameSave (0xB0180) alone is not offered: after
  a successful save the dialog writes `[0x2E8E4C]+0x90` (Breakdown) and crashes without it.
- **VFS** (`vfs\host_dir_device.cpp`): a directory scan (`File::NextDirEntry`, NtQueryDirectoryFile) is a snapshot
  taken at the (re)start; writable host directories list their entries in creation order (ties: name), like FATX
  (the save lists are in enumeration order), read-only ones in the host's name order (XDVDFS). `vfs::
  CreateHostDirDevice(root, readOnly, freeSpaceLimit = 0)`: `freeSpaceLimit` caps the reported free bytes
  (partition1 only, from `[saves] free_space_limit`, developer key, 0 = off; `vfs_init.cpp`).
- **XLaunchNewImageA** (`xapi\xapi_hooks.cpp`): the memory dashboard request `{2, 0, 'U', blocks}` (MEMCARD_047
  "Steuerung", `FUN_000E4B00`) logs a warning, shows a message box with `[debug] msgbox` and returns
  `ERROR_NOT_SUPPORTED` to the game (before M7: process exit with code 4). Every other request still ends the
  process.
- **Kernel** (`kernel\io_apc.cpp`): the delivery of the save layer's WriteFileEx completion 0xE01A0 is logged with
  its latency (`I/O APC: save write completion 0x000E01A0 delivered N ms after the write`, design R16).
- **Tools**: `tools\verify_save.py` (HMAC check with the configured key, `--resign`, `--json`; module functions
  `fixed_key`, `xbox_key`, `sign`, `make_container`, `check_container`, `save_dir_name`, `save_meta`),
  `tools\m7_check.py` (5 scripted runs on `run\m7\check`, criteria M7-1..M7-8), scripts
  `tools\input_scripts\m7_save.txt`, `m7_load.txt`, `m7_damaged.txt`, `m7_slots.txt`, `m7_nospace.txt`.
- **Tests**: `tests\save_tests.cpp` (creation-order listing, snapshot while deleting, free-space limit, XAPI's HMAC
  over the XcSHA exports against a Python value, `game level_end` parsing and player). Report: `docs\m7_report.md`.

### 4.35 G5 camera response and G7 level skip cheat - `xapi\camera_response.h`, `input\level_skip.h`, `input\host_router.h`

- **`[input] camera_response = original|precise`** (Controls tab, default `original`; `xapi::camera::ResponseFromConfig`).
  `precise` installs two conditional probes (`FUT_PROBE_IF`, nothing with `original`): `probe:Camera::FollowUpdate`
  0x15990 (follow camera, camera mode 0; prologue `55 8B EC 83 E4 F0`) computes the turn of the tick from the
  player's `+0xB38` (`PreciseTurn`: linear, 200 deg/s, 20 ms smoothing) and clears the game's turn filter state
  (`+0x154`, `[+0xE4]+0xC`) after the original; `probe:NiMatrix3::MakeYRotation` 0x103020 (`D9 44 24 04 33 C0`)
  replaces the angle only for the follow camera's call (return address 0x17B30) while a precise turn is active. The
  game's collision runs afterwards unchanged. Exit reporter "camera". Evidence: `analysis\loader\g5_camera.md`.
  The G6 first-person probes (`xapi\first_person.h`) use other addresses (camera mode 3 is 0x18250).
- **Pad shaper** (`input\xinput_source.h`, additive): `inp::SetPadShaper(fn)` - a process-wide
  `void(int port, PadState&)` applied to every controller's mapped state in `XInputPadSource::Sample` (after the
  bindings and `pad_deadzone`); `nullptr` = none (default). `precise` sets `PreciseRightStick` (`input_system.cpp`):
  radial dead zone 0.18 on the right stick, linear to 1 at 0.95, encoded through `GameStickInverse` so that the
  game's curve yields it (`stick_curve.h` `PreciseStickOutput`, `PreciseStickToGameThumbs`); skipped while the port is
  diverted. Keyboard/mouse and scripts are not shaped (the mouse already arrives as the exact game deflection).
- **Camera trace** (tests): input-script command `game camera_trace on|off` -> `xapi::camera::SetTrace`; the tick
  heartbeat probe calls `xapi::camera::TraceTick()` (one INFO line `camera trace: <n> mode M yaw Y pos x y z v ..
  w .. rate .. hrate .. stick S ctrl C` per tick, guest reads only, also with `original`).
  `analysis\loader\scripts\g5_camera_trace.py` evaluates it.
- **`[game] level_skip_cheat`** (bool, Game tab, default on; `inp::levelskip`): A A B B A A B B A A within 4 s (at
  most 1 s between presses; a press is a rising edge of the analog A/B from 30/256, released in between; any other
  button, A+B together or a longer pause starts over; sticks are ignored) while the character is controllable
  (`inp::script::GameControllable()`: Ingame, level loaded, `+0x630`, no cut scene, pause or save dialog; no
  Breakdown screen `[0x2E8E4C]+1`; port not diverted) calls `xapi::RequestLevelEnd(next)` - at the next tick the
  level-script native FsChangeLevel `FUN_000B0520(next)`, `next` = the level's own successor
  (`inp::levelskip::NextLevel`, the opcode-0xA1 argument of `scripts\levelN-M.ful`; Level5-3 -> Level1-1 without the
  credits). Log `cheat: level skip (A A B B A A B B A A, port N) in <Level> - level end via the script native
  FsChangeLevel("<Next>") ...`; presses that do not count are logged at debug level.
- **Router filter chain** (`input\host_router.h`, additive): `HostRouter::AddSampleFilter(fn)` - up to
  `kMaxChainedFilters` (4) further sample filters, called after the `SetSampleFilter` filter (freecam) in the order
  added, before the divert check; `HostRouter::Diverted(port)`. The input system adds `inp::levelskip::SampleFilter`.
- **Input script**: `inp::script::GameControllable()` (the `controllable` condition without a script, additive);
  `game camera_trace on|off`. Scripts `tools\input_scripts\g5_camera.txt`, `g7_level_skip.txt`.
- **Tests**: `tests\g5_g7_tests.cpp`. Report: `docs\g5_g7_report.md`.

### 4.36 G6 first-person control - `xapi\first_person.h`, `input\kbm_source.h`

- **`[input] first_person = off|on`** (Controls tab, "Egoperspektive (experimentell)", default `off`;
  `xapi::fp::ModeFromConfig`) and **`[input] first_person_look_speed`** (float 30..720, default 180 deg/s at full
  right-stick deflection). `on` installs six conditional probes (`FUT_PROBE_IF`, condition `xapi::fp::HooksWanted`;
  nothing with `off`, hook total unchanged): `probe:Player::SetFirstPerson` 0x54DA0, `probe:Player::vfViewToggle`
  0x588B0, `probe:Player::Move` 0x54AE0, `probe:Player::MoveDirection` 0x536D0, `probe:Player::vfJump` 0x50D10,
  `probe:Camera::FirstPersonUpdate` 0x18250 (prologues `55 8B EC 83 E4 F0` / `83 EC 24 56 8B F1` /
  `83 EC 10 56 8B F1`). They turn the game's own R3 view (camera mode 3) into a playable view: automatic entry while
  wanted (R3 toggles the wish), walking/jumping with the lock byte `+0xB88` cleared for the call, the move direction
  from the left stick relative to the view rows, the automatic exit of `FUN_00057940` (return 0x587F8) skipped unless
  dead or `+0x7A0`, own yaw/pitch (right stick of the active controller object, own curve; mouse counts), near plane
  0.1, HUD on and Bender's visor off after each entry. Levels of `xapi::select::IsPlayAsBlockedLevel` and cut scenes
  keep the game's camera. Exit reporter "first person"; with `[debug] trace = game` one TRACE line
  `first person trace: ...` per view tick (`analysis\loader\scripts\g6_firstperson_trace.py`). Evidence:
  `analysis\loader\g6_firstperson.md`.
- **Extra function starts** (`tools\gen\extra_function_starts.tsv`, regenerated `gen\xbe_functions.gen.inc`):
  0x50D10 `Player_vfJump` (size 0x163), 0x588B0 `Player_vfViewToggle` (size 0x12E) - vtable-only player methods,
  hook targets of G6.
- **Keyboard/mouse** (`input\kbm_source.h`, additive): `KeyboardMouseSource::SetMouseMotionQuery(fn)` with
  `using MouseMotionQuery = bool (*)() noexcept` (`nullptr` = none, default). While `fn()` returns true, `Sample`
  drops the mouse motion instead of mapping it to the bound controls (keys, buttons, wheel unchanged); `ReadRaw` still
  reports it. The query runs outside the source's lock. G6 sets `xapi::fp::MouseLookActive` (true while the camera
  hook ran within the last 150 ms).
- **G5 interplay:** with `camera_response = precise` the controller's right stick reaches the game encoded for the
  game curve (`inp::SetPadShaper`); the first-person view decodes it with `inp::GameStickOutput` before its own curve
  (`xapi::fp::LookDeflection(..., gameEncoded = true)`). G5 owns camera mode 0, G6 mode 3; no shared hook address.
- **Tests**: `tests\first_person_tests.cpp`. Script `tools\input_scripts\g6_first_person.txt`. Report:
  `docs\g6_report.md`.

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
| 2026-10-11 | M3d (frame-clock/pacing agent) | own: `xapi\game_timing.h/.cpp` (`FrameClock`, `ClockQuantum`, `FrameAlpha`, `FrameInterp`, `CurrentFrameInterp`, `SubTickFramesActive`, `RenderingFrameWithoutStep`, M3d fields of `FrameClockStats`, exit line `game clock M3d: ...`), new `xapi\frame_fixes.h/.cpp` (F2 `probe:Death::Render`, F3 `probe:Session::OccluderFade`, F4 hook `Session::RandomFlicker`), `platform\pacer_policy.h/.cpp` (`FrameRateMode`, `ChooseFrameRate`, `CadencePattern`, `Path::Cadence`, `Plan::nominalHz/cadenceRatio`, `FramesPerTick`, `FrameAdvance`, `FrameAdvanceOf`, `LatencyBudget`, `JustInTimeStart`, `OnScreenTracker::PredictNextVisible/ExtendLastPresent`), `platform\frame_pacer.h/.cpp` (`PacingConfig::rateMode/justInTime`, `SwapChainInfo::frameUnchanged`, `PresentRequest::repeatPrevious`, `CurrentFrameAdvance`, `NoteFrameTicks`, M3d `PresentStatistics` fields, line `present: frame rate ...`, trace columns), `platform\window.h/.cpp` (`ViewLayout`: the view covers the monitor in borderless fullscreen; the fullscreen flag is set before the frame is resized), new `tests\frameclock_tests.cpp`, new `tests\pacer_m3d_tests.cpp`, `tests\platform_tests.cpp` (fullscreen view), `tests\pacer_tests.cpp` (monitor test tolerates a disconnected display) | Section 4.24: frame rates up to 120 Hz with the half-tick frame clock (alpha 1/2 / 1 at 120 fps, M3b bit for bit at 60 fps), plans per refresh, repeated pictures and per-frame fixes between ticks, just-in-time frame start. |
| 2026-10-11 | M3d (frame-clock/pacing agent) | `core\config_schema.cpp` (shared, rows) | `[render] frame_rate` is a choice `60 \| 120 \| display` (de/en labels, hidden `50` and `0..1000`, dynamic default = `[game] tick_rate`, graphics tab, combo) instead of the M3c developer int key; new row `[render] low_latency` (bool, default on, advanced). |
| 2026-10-11 | M3d (frame-clock/pacing agent) | `gfx\gfx_config.h/.cpp` (renderer agent's module, additive) | `gfx::ParseFrameRate`, `Settings::frameRateMode/lowLatency`; `PacingConfigOf` passes `rateMode`/`justInTime`. |
| 2026-10-11 | M3d (frame-clock/pacing agent) | `gfx\nipc_gpu.cpp` (interpolation agent's file, one statement) | `GpuRenderer::DisplayFrame` sets `SwapChainInfo::frameUnchanged = no draw and no color clear in the frame` for the pacer's repeated picture (F1). |
| 2026-10-11 | M3d (frame-clock/pacing agent) | `launcher\**` (new `src\core\frame_rate.h/.cpp`, `src\ui\settings_tab.cpp`, `src\ui\main_window.cpp` (comments), `src\core\l10n_table.cpp`, `CMakeLists.txt`, `testdata\sample_schema.json`, `tests\tst_misc.*`, `tests\tst_ui.cpp`) | Frame rate choice with a note what "120" / "display" give on the selected monitor (`fl::EstimateFrameRate`, `fl::FrameRateNote`); the window size is disabled with a note while borderless fullscreen is on (M3c review nit). |
| 2026-10-11 | M3d (interpolation agent) | own: new `gfx\interp_frame.h/.cpp`, `gfx\interp_history.h/.cpp`, `gfx\interp_math.h`; `gfx\nipc_gpu.cpp` (History calls in `Create`, `PrepareFrame`, `SetCameraData`, `DrawGeometry`, `DisplayFrame`, `LogStatistics`); new `tests\interp_math_tests.cpp`, `tests\interp_history_tests.cpp` | Section 4.25: renderer-level interpolation (cameras, rigid draws of slots 48-51, hierarchical stage 1b, skinning through a substitute `NiSkinInstance`, snap rules, frame-driven detection, statistics, debug tint/trace); new probe `probe:NiAVObject::Destroy` (FUN_00102CE0, 7 bytes, registered in `gfx\interp_history.cpp`). `ni_extract` and `nipc_renderer.cpp` unchanged. |
| 2026-10-11 | M3d (interpolation agent) | `core\config_schema.cpp` (shared, rows) | `[render] interpolation` (bool, default on, graphics tab, advanced), `[render] interpolation_debug` (choice `off \| tint \| trace`, `kInterpolationDebugChoices`, developer, advanced tab), `[render] interpolation_hierarchy` (bool, default on, developer, advanced tab); header comment of the `[render]` owners. |
| 2026-10-11 | M3d phase 3 (integration + verification) | `gfx\rhi\rhi.h` (add-only `Device::RepeatPreviousPicture`), `gfx\rhi\d3d11\d3d11_device.cpp`, `d3d11_internal.h`, `gfx\nipc_gpu.cpp`, `platform\frame_pacer.h/.cpp`, `platform\pacer_policy.h/.cpp` (`OnScreenTracker::ExtendLastPresent` removed), `tests\rhi_d3d11_tests.cpp`, `tests\pacer_m3d_tests.cpp` | F1 repeated picture is presented: the device shows the buffer of the previous Present again and keeps the rotation (captures read it too), the pacer treats the frame like any other (sync interval, waitable, just-in-time start). Root cause found on the 120 Hz display: a frame without a Present left the waitable without a queued flip, the next frame started right after its GPU work - 146 fps in the intro, 77 % of the vblank frames on plan; after the fix 120 fps and 99.9 % on plan in the foreground frames. `d3d11 frames:` exit line counts "showing the previous picture again"; the draw summary of such a frame ends with ", repeated (previous present)". |
| 2026-10-11 | M3d phase 3 (integration + verification) | `platform\pacer_policy.h/.cpp` (`DisplayLimitDetector::StartWarmup`, `kWarmupWaits` 12), `platform\frame_pacer.cpp`, `tests\pacer_tests.cpp` | The display-limited detector ignores the first 12 vblank waits after the pacer enters a vblank plan: in a composed window the first waits after the switch from the timer took 16-29 ms; at 120 fps (1.5 periods = 12.5 ms) that fired the detector at every start (1 s free running plus 30 frames hysteresis = 179 frames on the timer); at 60 fps a single 33 ms wait stayed below 25 ms. |
| 2026-10-11 | M3d phase 3 (integration + verification) | `gfx\interp_history.cpp` | `interp_trace.csv` header: the per-reason columns are `reason_<name>` (the "interpolated" column appeared twice). |
| 2026-10-11 | M3d phase 3 (integration + verification) | new `tools\m3d_check.py`, `tools\foreground_hold.py` (keeps the game windows of a check run in front: `AllowSetForegroundWindow(ASFW_ANY)` while its own window is in front); `launcher\testdata\sample_schema.json` (`[render] interpolation`), `launcher\tests\tst_ui.cpp` | M3d check (variants i120, d120, t120, t120off, t120x, c60, ll60off, c60off; criteria in the header). Launcher UI test: the frame-rate note without in-between frames when `[render] interpolation = off`. |
| 2026-10-11 | M3d phase 3 (integration + verification) | `docs\loader_design.md` (new 8.9), this file (4.24 F1, changelog), `gfx\GFX_CONTRACT.md` (13), `docs\ROADMAP.md` | Verification documentation. |
| 2026-10-11 | M3d phase 5 (review fixes) | `gfx\interp_math.h` (`SnapLimits::unknownStepRadii/unknownAngleDeg`, `MotionReference`, `MotionSnapRule`), `gfx\interp_history.h/.cpp`, `gfx\interp_frame.h` (`FrameInterp::pairTicks`, `kFrameInterpCatchUp`, `kMaxPairTicks`), `tests\interp_history_tests.cpp` | Findings 1, 3, 4, 6, 10 (section 4.25 "Review fixes"): jump rule against the larger of the last two motions and decided by the parent chain, pointer reuse within a generation, camera destroyed check, readable-page cache epoch, interpolation over catch-up/cap step pairs. |
| 2026-10-11 | M3d phase 5 (review fixes) | `core\guest.h/.cpp` (shared, add-only `NoteMemoryReleased`, `MemoryReleaseEpoch`), `kernel\mm.cpp` (two calls) | Finding 6: release epoch of guest memory for caches of `IsReadable` results. |
| 2026-10-11 | M3d phase 5 (review fixes) | `xapi\game_timing.h/.cpp` (`ClockQuantum::rephase`, `FrameClock` offset, `Stats::rephases`, `Sample::rephased`, `kFrameInterpCatchUp`, `FrameClockStats::frameStartSamples/rephases`, exit line), `platform\pacer_policy.h/.cpp` (`FramesPerTick` <= tick rate -> 1, `FrameAdvance::frameStartQpc/planSerial`, `LatencyBudget` backoff), `platform\frame_pacer.h/.cpp`, `tests\frameclock_tests.cpp`, `tests\pacer_m3d_tests.cpp` | Findings 8, 9, 10, 11 (section 4.24 "Review fixes"). |
| 2026-10-11 | M3d phase 5 (review fixes) | `core\config_schema.cpp`, `launcher\src\core\frame_rate.cpp`, `launcher\testdata\sample_schema.json`, `launcher\tests\tst_misc.cpp`, `tst_ui.cpp` | Finding 12: `[render] frame_rate` labels "60" / "120 (interpoliert)" / "Monitor (max. 120)" (en "120 (interpolated)"); finding 8: the launcher note shows no in-between frames up to the logic rate (59.94 Hz). |
| 2026-10-11 | M3d phase 5 (review fixes) | `tools\timing_compare.py` (steam cloud period by autocorrelation, finding 13), `tools\m3d_check.py` (new log formats), `analysis\loader\m3d_frameclock.md` (2.5, 2.7, 3.2 corrections), new `docs\m3d_report.md`, `docs\loader_design.md` (8.9), `docs\ROADMAP.md`, `README.md`, `gfx\GFX_CONTRACT.md` (13), this file | M3d report and documentation. |
| 2026-10-11 | M3d phase 5 (final check) | `platform\frame_pacer.cpp`, `tools\foreground_hold.py`, `tools\m3d_check.py` | F1 keeps the previous picture also without presents (display off, minimized: the screenshot of an intro frame between ticks was black); `foreground_hold.py` holds a display/system power request (`SetThreadExecutionState`) for the measurement; `m3d_check` R2 skips a window's first frame whose partner lies before the reference run's window. |
| 2026-10-11 | M4 launcher agent | `src\INTERFACES.md` (new 4.26) | Proposed contract of the `[input]` keys: `pad_<control>`/`kbm_<control>` binding strings (`pad:`/`key:`/`mouse:` tokens), `pad_deadzone`, `mouse_sensitivity`, `rumble`, `rumble_strength`, `pad_mode`, schema `ui.tab` = `controls`, `ui.widget` = `binding`/`slider`, defaults of m4_input.md 9. The input agent's parser is the reference once it exists. |
| 2026-10-11 | M4 test-automation agent | new `input\input_api.h` (shared, first writer), this file (new 4.27) | Shared input interface: `PadState`, `MergePad`, `RumbleState`, `InputSource`, `InputConsumer`, `InputRouter`, header-only source registry and router slot (section 4.27). |
| 2026-10-11 | M4 test-automation agent | own: new `input\input_script.h/.cpp`, new `tests\input_script_tests.cpp`, new `tools\m4_check.py`, new `tools\input_scripts\README.md` + `m4_newgame.txt` + `m4_rumble.txt` | `[debug] input_script`: script parser/player, `InputSource` "input script" on port 0, observer probes `probe:Game::MainMenuActivate` (0x20150, 7 bytes), `probe:Game::LevelReadyTest` (0xC3E20, 6 bytes), `probe:Game::PauseOpen` (0xA8220, 7 bytes), `probe:Game::HotplugCheck` (0x9C140, 8 bytes; tick heartbeat); new exit code 7 (section 1); M4 check (section 4.27). |
| 2026-10-11 | M4 test-automation agent | `core\log.h/.cpp` (shared, add-only) | `core::AddLogObserver` / `RemoveLogObserver` / `kMaxLogObservers` (4): observers with the `LogTap` contract next to the single tap (the input script's `log` condition). |
| 2026-10-11 | M4 test-automation agent | `core\guest.h` (shared, add-only) | Section "added in M4": `kGameMainMenuActivate`, `kMainMenu*Offset`, `kMenuCameraMovingOffset`, `kGameLevelReadyTest(+Return)`, `kGameLevelPtr`, `kLevelPlayerOffset`, `kPlayerInputEnabledOffset`, `kGameCutscenePtr`, `kCutsceneActiveOffset`, `kCAppSaveUiOffset`, `kSaveUiDialogOpenOffset`, `kGamePauseOpen`, `kPauseOpenOffset`, `kGameActiveControllerVar`, `kControllerPortOffset`, `kGameHotplugCheck(+TickReturn)` with evidence (m4_input.md, capstone prologues, raw branch scan). |
| 2026-10-11 | M4 test-automation agent | `core\config_schema.cpp` (shared, row) | Row `[debug] input_script` (path_file, developer, advanced tab, `*.txt`, de/en). |
| 2026-10-11 | M4 launcher agent | own `launcher\**`; `launcher\CMakeLists.txt` (shared, add-only: new sources `core\bindings`, `core\pad_reader`, `ui\binding_capture`, `ui\controls_panel`, `ui\pad_poller`, test `tests\tst_bindings`) | Launcher tab "Steuerung" (section 4.26, "Launcher side"): the launcher's schema parser knows `ui.tab` = `controls`, `ui.widget` = `binding` / `slider`, the key field `unit`, section `input` falls back to the controls tab; `IssueCode::BadBinding`; `SettingsTab::setExtraErrors` / `setEmptyNoteVisible`; screenshots `*_main_controls`, `*_tab_controls_full`, `*_tab_controls_kbm_full`, `*_controls_capture_kbm`, `*_controls_pad_test_simulated`; testdata `sample_schema.json` with the `[input]` section of the contract. |
| 2026-10-12 | M4 input agent | own: new `input\xpp_hle.h/.cpp` (replaces the deleted `input\xpp_stubs.h/.cpp`), new `input\host_router.*`, `input_system.*`, `input_config.*`, `bindings.*`, `stick_curve.h`, `xinput_source.*`, `kbm_source.*`, `kbm_window.*`; new `tests\input_bindings_tests.cpp`, `input_router_tests.cpp`, `input_backend_tests.cpp`; this file (new 4.28) | Input layer (section 4.28): XPP HLE of the 7 entries, router (focus, diversion, release-first, rumble routing, simulated unplug), XInput 1.4 backend, keyboard/mouse via Raw Input, bindings of 4.26. |
| 2026-10-12 | M4 input agent | `input\input_api.h` (shared, additive) | `InputSource::HasMotors()` (default false), `InputRouter::PortEpoch(port)` (default 0), `InputRouter::SimulateUnplug(port, unplugged)` (default no-op). |
| 2026-10-12 | M4 input agent | `core\config_schema.h/.cpp` (shared, additive) | `ConfigUiTab::Controls` ("controls"), `ConfigUiWidget::Binding` / `Slider` ("binding" / "slider"), `ConfigKeyDef::unit` (JSON "unit") and `ConfigKeyDef::validate` (called for string/path keys after the type check); section `[input]` "Steuerung"/"Controls" with 56 rows: 48 bindings `pad_*`/`kbm_*` (validators from `input\bindings.h`, defaults m4_input.md 9), `pad_mode`, `rumble`, `rumble_strength` (slider, %), `pad_deadzone` (slider, %), `mouse_sensitivity` (slider, ×), `mouse_capture`, `menu_escape`, `background_input` (advanced). |
| 2026-10-12 | M4 input agent | `core\cli_tools.cpp` (additive) | `--print-config-schema` writes the key field `"unit"` when set. |
| 2026-10-12 | M4 input agent | `tests\schema_tests.cpp`, `tests\xdk_tests.cpp` (other agents' tests, minimal) | Schema test accepts widgets `binding` (string + validator, section input) / `slider` (ranged int/float), tab `controls`, checks `unit`; the XPP case of the XDK tests checks the HLE (port 1 connected, handle, synchronous status) instead of the stubs. |
| 2026-10-12 | M4 input agent | this file (4.26) | Input agent's notes on the contract: `ui.order` pad 100 + 2i / kbm 101 + 2i (unique per tab), host-side validation (exit code 1), radial `pad_deadzone`, mouse scale, new keys `mouse_capture`, `menu_escape`, `background_input`. |
| 2026-10-12 | M4 integration | `vfs\vfs_known_missing.cpp` (io, entries), `tests\m2_integration_tests.cpp` (one case) | Documented misses of level loading: `\Levels\*\backdrop.nif`, `backdrop_static.nif`, `camerapathtest.nif` (optional files, existence test in `FUN_000C5B40` states 5-7) and `\Sounds\Level1\Ambience-PlanetExp\Hum-loop2.wav` (named by `Level1-1.Ful`, not on the disc). `loader_design.md` 8.11 E12. |
| 2026-10-12 | M4 integration | `audio\xdk_entries.h/.cpp`, `tests\xdk_tests.cpp` (comments, one log text) | The exit summary of the XPP entries reads `XDK XPP HLE: N calls ...` (DSOUND/XMV keep `... stubs: ...`); comments no longer point at the deleted `input\xpp_stubs.*`. Section 4.13: the XPP entries are HLE since M4 (4.28). |
| 2026-10-12 | M4 review fixes | `input\input_script.*`, `input\input_system.cpp`, `input\xpp_hle.*`, `input\host_router.cpp`, `input\kbm_source.*`, `input\kbm_window.cpp`, `input\bindings.*` | Script loaded in `StartInput` (`inp::script::PreloadScript`), `XppDevices` queries the router before taking its mutex (a broken script exits with 1 instead of 0xC0000409); rumble target only on deliberate input; high-resolution wheel deltas summed (`WheelDeltaEvent`, `kWheelDelta`); keyboard stick diagonals on the unit circle; duplicates do not count towards the 4-token limit; header text on determinism (E14). Tests: router (noisy pads), backend (high-res wheel), bindings (edge cases, diagonals). Sections 4.26-4.28. |
| 2026-10-12 | M4 review fixes | `core\config_schema.cpp` (labels only), launcher `core\bindings.*`, `ui\binding_capture.*`, `ui\controls_panel.*`, `ui\key_editors.cpp`, `ui\settings_tab.*`, `ui\app_style.*`, `ui\automation.cpp`, `core\l10n_table.cpp` | Section `[input]` titled "Eingabegeräte und Vibration"; `menu_escape` label "START-Taste der Tastatur geht in Menüs zurück". Launcher: token parser equal to the host (digits only, blanks around `:`), native extended bit for right Ctrl/Alt, Print on release, controller list in the capture, reserved-key and essential-control warnings, reset confirmation (`ConfirmChange`, test hook), exact slider values, `GameControlsForPad`, compact controller test, wording; screenshot of the pad capture and correctly sized capture dialogs. Section 4.26. |
| 2026-10-12 | M5 DSOUND core agent | `audio\audio_api.h` (new shared file), this file (4.29) | Shared M5 audio interface (namespace `snd`): categories, formats, `Decoder`/`CreateDecoder`, DSOUND level helpers `MixbinGain`/`MillibelToGain`, 3D state + `Spatializer`/`CreateSpatializer`, `I3dl2Listener`, `AudioEngine`/`SourceVoice`/`VoiceEvents`/`MixTap`, `CreateAudioEngine`, `Engine()`. Other M5 agents extend it additively. |
| 2026-10-12 | M5 codec/3D/reverb agent | own: new `audio\codec_adpcm.*`, `codec_format.*`, `codec_decoder.cpp`, `spatial_model.*`, `spatial_x3d.*`, `reverb_i3dl2.*`, `reverb_software.*`, `tests\audio_codec_tests.cpp`, `tests\audio_spatial_tests.cpp`, `tests\audio_spatial_reverb_tests.cpp`, `analysis\loader\scripts\m5_codec_refs.py`, `m5_codec_golden.py`; this file (4.29 paragraph "Codec, 3D and reverb") | `snd::CreateDecoder` (PCM8/PCM16/Xbox ADPCM, bit-exact with ffmpeg), `snd::CreateSpatializer` (DSOUND 3D level model + X3DAudio direction, positional modes), I3DL2 conversion/layout helpers, effects image parser, stand-alone reverb XAPO for the software mixer, DSOUND pitch model (section 4.29). |
| 2026-10-12 | M5 codec/3D/reverb agent | `core\config_schema.cpp` (shared, rows + 2 choice lists + header comment) | `[audio] speakers` (auto\|stereo\|5.1, hidden 2.0), `reverb` (bool, 1), `positional` (corrected\|xbox\|off), all `Tab::Audio`; developer `trim_3d` (float dB, -20..20, Advanced 855). `speakers`/`reverb` are read by `audio\audio_engine.cpp` (DSOUND core), `positional`/`trim_3d` by `audio\spatial_x3d.cpp`. |
| 2026-10-12 | M5 tooling/launcher agent | `core\config_schema.h/.cpp` (shared, additive), `tests\schema_tests.cpp` (accepts the new tab/widget) | `ConfigUiTab::Audio` ("audio"), `ConfigUiWidget::AudioDevice` ("audio_device"); rows `[audio] capture_wav` (path_file `*.wav`), `capture_format` (`float \| pcm16`, `kCaptureFormatChoices`), `capture_stems`, `event_log` (all developer, Advanced 810-840); header comment of the `[audio]` readers. |
| 2026-10-12 | M5 tooling/launcher agent | own: new `audio\capture_wav.*`, `audio\capture_tap.*`, `audio\capture_session.*`, `audio\capture_engine.*` (EngineObserver), `tests\audio_capture_tests.cpp`, `tools\audio_compare.py`, `tools\m5_check.py`, `tools\input_scripts\m5_frontend.txt`; this file (new 4.30) | Audio capture session with WAV writer, level statistics, tap XAPO, event list and exit reporter `"audio capture"`; reference comparison and the M5 check (section 4.30). |
| 2026-10-12 | M5 tooling/launcher agent | own `launcher\**`: new `core\audio_output.*`, `ui\audio_panel.*`; `core\config_schema.*` (tab `audio`, widget `audio_device`), `ui\key_editors.*` (`EditorContext::audio`, `AudioDeviceEditor`), `ui\app_context.*` (`AppContext::audio`, `StartupOptions::audioFile`, `LoadAudioDevices`), `ui\main_window.*`, `ui\automation.cpp`, `main.cpp` (`--audio-file`), `core\l10n_table.cpp`, `CMakeLists.txt` (sources, `ole32` + `xaudio2`), `testdata\sample_schema.json` (`[audio]` keys), tests | Launcher tab "Audio" with device list, device check and test tone (section 4.30); screenshots `*_main_audio`, `*_tab_audio_full`. |
| 2026-10-12 | M5 codec/3D/reverb agent | `docs\loader_design.md` (shared, new section 8.12) | Deviations K1-K11 of the codec/3D/reverb part (ADPCM variant, pitch quantization, X3DAudio instead of HRTF/crosstalk, `positional` modes, LFE, reverb wiring/`trim_3d`, exact level truncation, presets come from the XBE table not from dsstdfx.bin, corrected `m5_i3dl2.out`/`m5_gain_model.out` values, doppler, no cone). Numbering may need a renumber if another M5 agent also adds an 8.12. |
| 2026-10-12 | M5 DSOUND core agent | own: new `audio\dsound_hle.*`, `audio\dsound_mix.*`, `audio\dsound_probe.cpp`, `audio\audio_engine.*`, `tests\audio_dsound_tests.cpp`; deleted `audio\dsound_stubs.*` | DSOUND HLE on XAudio2 / the software mixer (section 4.29 "DSOUND core"). |
| 2026-10-12 | M5 DSOUND core agent | `audio\audio_api.h` (shared, additive) | `SoundEvent`/`SoundEventKind`/`SoundEventSink` + `SetSoundEventSink`, `EngineObserver` + `SetEngineObserver` (used by `capture_engine.cpp`), `AudioEngine::OutputFramesRendered`, `Stats::limiterMaxReductionDb`/`limitedFrames`; `SubmitDesc::context`/`OnBufferEnd` are 32-bit. |
| 2026-10-12 | M5 DSOUND core agent | `core\guest.h` (shared, additive) | `kSoundVoiceCreateDsObject` 0xCC150 and the SoundVoice/SoundData offsets read by `probe:Game::SoundVoiceCreateDsObject`. |
| 2026-10-12 | M5 DSOUND core agent | `core\config_schema.cpp` (shared, rows + header comment) | `[audio] enabled`, `device` (widget `audio_device`), `master_volume` (slider %), `limiter` (Audio tab); `dummy_streams` relabelled "obsolete" (still accepted, read only for a log line). |
| 2026-10-12 | M5 DSOUND core agent | `audio\xdk_entries.h/.cpp` (comments, summary), `tests\xdk_tests.cpp` (other agent's test, minimal) | Exit summary `XDK DSOUND HLE: ...` + `snd::LogDsoundSummary` instead of the stub stream statistics; the DSOUND stub cases of `xdk_tests.cpp` replaced by one HLE smoke case (device singleton, NULL handles, GetStatus written) on a software engine. |
| 2026-10-12 | M5 DSOUND core agent | `docs\loader_design.md` (shared, new section 8.13) | Deviations D1-D12 of the DSOUND core and the engine (guest writes only on the guest thread, own peak limiter after the Master tap instead of FXMasteringLimiter, software mixer with the same reverb and device switching both ways, a starting 3D voice takes its pending values, ...). |
| 2026-10-12 | M5 integration | `audio\audio_api.h` (additive), `audio\dsound_hle.cpp`, `audio\capture_engine.cpp` | `SoundEventKind::Level3D`, `SoundEvent::levelDb`/`distance`/`minDistance`/`maxDistance`/`rolloffPoints` (diagnostics); event list detail ` level= dist= min= max= curve=` and kind `level` (section 4.30). |
| 2026-10-12 | M5 integration | `audio\audio_api.h` (additive), `audio\audio_engine.*`, `core\config_schema.cpp` (row + choice list), `tests\audio_dsound_tests.cpp` | `EngineConfig::DeviceTest` + `[audio] device_test = off\|absent\|loss` (developer, Advanced 845); a backend switch continues the playing buffer at its position (`ProxyVoice` bookkeeping) instead of restarting it; the software mixer starts a buffer after `Flush` at the read position (was one frame late); test hook `SwitchToNewSoftwareBackendForTesting`, new test case. Section 4.29. |
| 2026-10-12 | M5 integration | `docs\loader_design.md` (new section 8.14) | Integration results and deviations I1-I9 (German). |
| 2026-10-12 | M5 integration | `audio\audio_engine.cpp`, `tests\audio_dsound_tests.cpp` | LFE low-pass 120 Hz in the master stage (both backends, before the Master tap); limiter statistics summed over every backend of a run; "no output device" / "device lost" logged at INFO like "display off" (the m2/m3 checks require logs without WARN). New test case for the LFE filter. Section 4.29. |
| 2026-10-12 | M5 phase 5 (review fixes) | `audio\dsound_hle.*`, `tests\audio_dsound_tests.cpp` | Finding 1: `Play` / `Pause(0)` / stream creation start the engine voice at the tick's commit (`ApplyAllDirty`) with the levels set after the call; `ObjectView::startPending`; the DSOUND tests commit after `Play` like the game; new test case. Section 4.29. |
| 2026-10-12 | M5 phase 5 (review fixes) | `audio\audio_api.h` (additive), `audio\audio_engine.*`, `audio\capture_*`, `tests\audio_dsound_tests.cpp` | Findings 6/7/8/9/12/13/14/15/17: device worker thread + `IMMNotificationClient` (no device work on the guest thread, backoff, default-device and configured-device return), device name in the log, 8-channel mastering voice for wider endpoints, monotonic `FramesPlayed` over Discontinuity, flush rebase at the next Submit, `AudioEngine::TapFormat` (capture rings allocated up front), "audio capture taps" only when used, software mixer positions-only mode without a listener, summary only under the facade lock; `EngineDescriptionForTesting`; 5 new test cases (2 skip without an endpoint). Section 4.29/4.30. |
| 2026-10-12 | M5 phase 5 (review fixes) | `audio\spatial_x3d.*`, `tests\audio_spatial_tests.cpp` | Findings 2/3: X3DAudio pans 3D voices without the front centre; the direction scale keeps every channel at or below the front reference (no +3 dB to the side). Section 4.29. |
| 2026-10-12 | M5 phase 5 (review fixes) | `tools\audio_compare.py`, `tools\m5_check.py` | Finding 5: timeline levels with the chunk resampled at the measured speed and sub-sample alignment (`stretch_sinc`, `aligned_gains`, selftest case); A11 tolerance music +-0.1 dB, menu +-0.25 dB; finding 16: new criterion A12 (limiter statistics vs. pre-limiter peak and `master_volume`). Section 4.30. |
| 2026-10-12 | M5 phase 5 (review fixes) | `launcher\src\core\audio_output.*`, `launcher\src\ui\audio_panel.cpp`, `launcher\src\core\l10n_table.cpp`, `launcher\tests\tst_ui.cpp`, `core\config_schema.cpp` (help text) | Findings 10/11: test tone stops when the player is destroyed, "Geräte neu suchen" disabled while it plays, test tone falls back to the default device like the game; texts: no device = silent until a device appears, not connected = default device shown and named, downmix note; `[audio] device` help mentions following the default and switching back. Section 4.30. |
| 2026-10-12 | M5 phase 5 (final check) | `audio\spatial_x3d.cpp`, `audio\audio_engine.cpp`, `tests\audio_spatial_tests.cpp`, `tests\audio_dsound_tests.cpp`, `tools\m5_check.py` | The finding-2 fix panned every 3D voice to front left on the 5.1 side layout 0x60F (X3DAudio with the non-standard mask 0x60B): X3DAudio now pans over quad/stereo and the host maps to the output (test covers 0x3F and 0x60F); XAudio2 `FramesPlayed` after `Discontinuity` takes the stream-end value at the pass end (was 0 in the test, now at most one pass short); A9 requires sound louder on each side (a one-sided panning passed before). Sections 4.29, 4.30. |
| 2026-10-12 | M5 phase 5 | `docs\loader_design.md` (new section 8.15), `docs\m5_report.md` (new) | Review findings R1-R17 (accepted/rejected) and the M5 report (German). |
| 2026-10-12 | M4b (main session) | `input\host_router.h`, `input\kbm_source.h`, `gfx\nipc_renderer.h`, `core\config_schema.cpp` | `HostRouter::SetSampleFilter`, `KeyboardMouseSource::SetPadEmulation`, `gfx::RenderTargetOf`, `gfx::ClearBackBuffer`, section `[freecam]`; new `gfx\freecam.*`, `gfx\freecam_math.h`, `input\chord_filter.h` (section 4.31) |
| 2026-10-12 | G1 outlines | new `gfx\outline.h/.cpp`, `tests\outline_tests.cpp`; `gfx\gfx_config.h/.cpp` (`OutlineSettings`, `ParseOutlineColor/Opacity/Thickness`), `gfx\ni_extract.h` (additive `GeometryInput::positionsOverride`), `gfx\ni_extract.cpp` (`Skin` source positions, cache bypass while `DAT_0030EB40 != 0`), `gfx\ni_guest_layouts.h` (`kNiOutlinePositionBuffer`), `gfx\nipc_gpu.cpp`, `gfx\nipc_renderer.cpp` (one call in `CreateRenderer`), `core\config_schema.cpp` (3 rows + 2 validators), `tests\extract_tests.cpp` (one case) | `[render] outline_color` (original\|RRGGBB), `outline_opacity` (original\|0..1), `outline_thickness` (0..4, 1.0) on the Graphics tab; defaults run no outline code. GFX_CONTRACT.md section 14, `analysis\loader\g1_outlines.md`. |
| 2026-10-12 | M6 (video) | `video\xmv_stubs.*` -> `video\xmv_hle.*`; new `video\xmv_demux.*`, `wmv_decoder.*`, `movie_core.*`, `movie_clock.h`, `yuv_pack.*`, `movie_skip.*`, `xmv_test_mode.*`; `tests\video_tests.cpp`, `tests\xdk_tests.cpp` (XMV case) | XMV HLE: own demuxer, WMV2 through the Windows WMVideo Decoder DMO, audio through a DSOUND HLE stream, A/V by pts on the audio clock (section 4.32). |
| 2026-10-12 | M6 (video) | `audio\dsound_hle.h/.cpp` (additive), `audio\xdk_entries.h/.cpp` | `snd::StreamFramesPlayed`; stream `endOfData` after `Discontinuity` (no dropout warning at a movie end); XDK summary says "XMV HLE" and calls `vid::LogMovieSummary`. |
| 2026-10-12 | M6 (video) | `gfx\d3d_shim.h/.cpp` (additive), `gfx\texconv.h/.cpp` (additive), `gfx\nipc_gpu.h/.cpp`, `gfx\nipc_renderer.cpp` | `ResourceInfo::serial/generation`, `NoteResourceWritten`, `kFormatYuy2`; `tex::Yuy2Mode`, `YuvToRgbBt601`, `ConvertYuy2ToBgra`; `GpuRenderer::MovieTexture` + texel addressing of movie stages; the resolver no longer returns the black texture for movie textures (GFX_CONTRACT.md 15). |
| 2026-10-12 | M6 (video) | `core\guest.h` | added `kGameCutsceneSkipAllowed` 0xD3120, `kGameCutsceneSkipAllowedReturn` 0x2ABD9, `kGameCheatSkipCutscenesVar` 0x2EB7D4, `kMovieGetNextPacket` 0xEDBA0, `kMovieReleasePreviousPacket` 0xEDE60 (evidence in the header). |
| 2026-10-12 | M6 (video) | `core\config_schema.cpp`, `core\cli_tools.h/.cpp`, `src\host\CMakeLists.txt` | `[game] skip_videos` (always\|original, tab Game), `[debug] movies` (play\|skip, developer); tool mode `--decode-movies` (+ `--movie-dump`, `--movie-filter`, `--result`); `ole32.lib` (CoTaskMemFree for the DMO media types). |
| 2026-10-12 | M6 (video) | `tools\m6_check.py` (new), `tools\m2_check.py`, `m3_check.py`, `m3c_check.py`, `m3d_check.py`, `tools\input_scripts\m4_newgame.txt`, `m4_rumble.txt`, `m4b_freecam.txt`, `m5_frontend.txt`, new `m6_boot.txt`, `m6_level.txt`, `m6_attract.txt` | Frame-timeline checks run with `--set=debug.movies=skip`; the scripts press START until the front end (the boot movies play in real time since M6). |
| 2026-10-12 | M6 (video) | `docs\loader_design.md` (new section 8.16), `docs\m6_report.md` (new), `gfx\GFX_CONTRACT.md` (section 15), `docs\ROADMAP.md`, `README.md` | M6 deviations, report (German), movie texture contract, roadmap, player notes. |
| 2026-10-12 | G4 select | new `xapi\game_select.h/.cpp`, `tests\game_select_tests.cpp`; `core\hooks.h/.cpp` (additive: `HookDesc::condition`, `FUT_PROBE_IF`; a probe whose condition is false is skipped without log line and not counted), `core\config_schema.cpp` (2 rows + choice lists) | `[game] start_level` (original\|Level1-1..Level5-3 in game order, tab Game): probe `FUN_000A2340` only at the "Neues Spiel" call (return 0x200B8) sets name + start state of table 0x2D60BC (`FUN_000C1D30`). `[game] play_as` (original\|fry\|bender\|leela, experimental): probes 0x5E580/0xC3E80/0xC3F20 rename the player def; blocked Level1-6/2-5/4-1/5-3. Both `original` = no hook installed. `docs\g4_report.md`, `loader_design.md` 8.17. |
| 2026-10-12 | M7 (save games) | new `xapi\save_probes.h/.cpp`; `core\guest.h` (section "added in M7": `kXapiXCreateSaveGame`, `kXapiXDeleteSaveGame`, `kGameSaveReadContainer`, `kGameSaveVerifySignature`, `kGameSaveWriteContainer`, `kGameOpenSaveMenu`, `kGameSaveDialogCtor`, `kGameNativeFsGameSave`, `kGameNativeFsChangeLevel`, `kGameBreakdownPtr`, `kSaveObjectContainerOffset`, `kGameSaveWriteCompletion`, `kGameSavingEnabledVar`) | Save observer probes (7 probes, log lines `save: ...`, exit reporter "save"), debug action `RequestLevelEnd`/`RunPendingGameActions` (section 4.33). |
| 2026-10-12 | M7 (save games) | `input\input_script.h/.cpp` (additive: `Op::Game`, `Environment::GameAction` default false, command `game level_end <NextLevel>`; the tick heartbeat probe calls `xapi::RunPendingGameActions`), `tools\input_scripts\README.md` | Input scripts reach the game's end-of-level screen (save point) without playing the level. |
| 2026-10-12 | M7 (save games) | `vfs\vfs.h` (`CreateHostDirDevice(..., freeSpaceLimit = 0)`, additive default), `vfs\host_dir_device.cpp`, `vfs\vfs_init.cpp`, `core\config_schema.cpp` (row `[saves] free_space_limit`, developer), `tests\schema_tests.cpp` (only the two key rows of `[saves]` get a hex value) | Directory scans are snapshots in creation order on writable host directories (FATX order of the save lists); developer key to simulate a full disk. |
| 2026-10-12 | M7 (save games) | `xapi\xapi_hooks.cpp`, `kernel\io_apc.cpp` | XLaunchNewImageA `{2, 0, 'U', n}` returns to the game (message instead of the dashboard, design 4.5); latency log line of the save write completion (R16). |
| 2026-10-12 | M7 (save games) | new `tools\verify_save.py`, `tools\m7_check.py`, `tools\input_scripts\m7_*.txt` (5), `tests\save_tests.cpp`, `analysis\loader\m7_saves.md`, `docs\m7_report.md`; `docs\loader_design.md` (new section 8.18, R11/R16, M7), `docs\ROADMAP.md`, `README.md`, this file (4.33) | M7 tools, tests, analysis, report and documentation. |
| 2026-10-12 | G5/G7 | new `xapi\camera_response.h/.cpp`, `input\level_skip.h/.cpp`, `tests\g5_g7_tests.cpp`, `analysis\loader\g5_camera.md`, `analysis\loader\scripts\g5_camera_trace.py`, `tools\input_scripts\g5_camera.txt`, `g7_level_skip.txt`, `docs\g5_g7_report.md`; `input\host_router.h/.cpp` (additive: `AddSampleFilter`, `Diverted`), `input\xinput_source.h/.cpp` (additive: `SetPadShaper`), `input\stick_curve.h` (additive: `PreciseStickOutput`, `PreciseStickToGameThumbs`), `input\input_system.cpp` (wiring), `input\input_script.h/.cpp` (additive: `GameControllable`, `game camera_trace on\|off`, the heartbeat calls `xapi::camera::TraceTick`), `core\config_schema.cpp` (2 rows + choice list), `tools\input_scripts\README.md` | `[input] camera_response = original\|precise` (2 conditional probes 0x15990/0x103020, pad right-stick shaper), `[game] level_skip_cheat` (A A B B A A B B A A -> FsChangeLevel with the level's successor). Section 4.35, `loader_design.md` 8.19. |
| 2026-10-12 | G6 first person | new `xapi\first_person.h/.cpp`, `tests\first_person_tests.cpp`, `analysis\loader\g6_firstperson.md`, `analysis\loader\scripts\g6_firstperson_trace.py`, `tools\input_scripts\g6_first_person.txt`, `docs\g6_report.md`; `input\kbm_source.h/.cpp` (additive: `SetMouseMotionQuery`), `tools\gen\extra_function_starts.tsv` + regenerated `gen\xbe_functions.gen.inc` (0x50D10, 0x588B0), `core\config_schema.cpp` (2 rows + choice list), `tools\input_scripts\README.md` | `[input] first_person = off\|on` (6 conditional probes 0x54DA0/0x588B0/0x54AE0/0x536D0/0x50D10/0x18250, nothing with `off`), `[input] first_person_look_speed`. Section 4.36, `loader_design.md` 8.20. |
