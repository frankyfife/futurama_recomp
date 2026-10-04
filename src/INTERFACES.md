# Loader interfaces (phase 2 contract)

Binding design: `docs/loader_design.md` (decisions E1-E12). This file lists every API the module
agents use, who owns which file, and how to build and run. Code and comments are English; design
documents are German.

## 1. Build and run

| What | How |
|---|---|
| Build | `cmd /c "D:\FuturamaDecomp\build.cmd" [builddir] [Debug\|RelWithDebInfo] [target]` (always the FULL path; cmd AutoRun changes the working directory). Default `build\x86-debug Debug`, all targets. Every agent uses **its own** build dir, e.g. `build\core`, `build\kernel`, `build\io`. |
| Targets | `fut_launcher` (futurama.exe), `fut_host` (futurama_host.dll), `fut_game` (static), `fut_tests` (only if `third_party\doctest\doctest.h` exists), `fut_gen` (checks generated files, needs Python). |
| Output | `<builddir>\bin\futurama.exe`, `futurama_host.dll`, PDBs. |
| Presets | `CMakePresets.json`: `x86-debug`, `x86-relwithdebinfo` (Ninja + cl, needs a vcvars32 environment). |
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

Exit codes: `0` ok / stop-at reached, `1` startup/config error, `2` a HostMain stage failed,
`3` fatal error (`core::Fatal`, KeBugCheck-style), `10..14` launcher errors (`shared\launcher_info.h`).
Added in phase 2 (core): `4` the guest ended itself (`XLaunchNewImageA` hook, `HalReturnToFirmware`, all
guest threads exited), `5` `[debug] timeout` watchdog (thread dump + minidump in the log directory).
Core debug keys: `[debug] timeout = <s>` (0 = off), `minidump = 1`, `mmio_trap = 1` (reserve 0xFD000000..0xFEFFFFFF),
`crash_test = 1` (provokes an unhandled guest access violation before entry to check the crash path, exit 3).
M1 boot path verified (core run 2026-10-04, `--stop-at=main --selftest`): all self-tests pass, entry ->
PsCreateSystemThreadEx -> XapiThreadStartup -> 0xF8469 -> XapiInitProcess -> _rtinit -> _cinit -> main hook, exit 0.

**M1 exit check (phase 3 integration)**: `python tools\m1_check.py --exe build\x86-debug\bin\futurama.exe
--config run\integ\futurama.ini --runs 10 --xbe orig\retail_eu\default.xbe [--fresh]` runs the loader N times
(the INI must set `userdata`, `stop_at = main`, `selftest = 1`), checks the six M1 exit criteria of design
section 6 per run (self-tests, boot probes, partition1 files, pool + cluster size, main hook + exit 0, log
without error/warn/UNIMPLEMENTED/TRAP) and compares the logs after masking timestamps and 8-digit addresses.
Logs of every run: `<userdata>\m1_check\`. Integration configs: `run\integ` (Debug), `run\integ-rel`
(RelWithDebInfo), `run\integ-main` (no stop-at; ends in the MMIO trap of NiXBoxRenderer::Create = M2 start).

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
`core::SetFatalHook(fn)` (crash module) runs inside `Fatal`. printf format strings are checked by MSVC
(`_Printf_format_string_`): use `%lu`/`%lX` for `ULONG`/`DWORD`/`NTSTATUS` casts, `%u`/`%X` for `uint32_t`.

### 4.3 Configuration - `core\config.h`

`core::GetConfig()` returns the process configuration (`iso`, `xbe`, `userdata`, `stopAt`, `selftest`,
`trace`, `strict`, `msgbox`, `logLevel`, `hooksDisable`, plus `LogDirectory()`, `Partition1Directory()`
= `<userdata>\hdd\partition1`, `CacheDirectory()`). Module-specific keys need no header change:
`cfg.GetString("settings", "language", "english")`, `GetBool`, `GetU32` (dec/0x), `GetList`, `GetPath`
(relative to the INI). Any key can be overridden with `--set=section.key=value`.
`core::WideToUtf8` / `core::Utf8ToWide` convert strings.

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
  returns `STATUS_NOT_IMPLEMENTED`; with `[kernel] strict=1` debug break + `core::Fatal`.
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
expected bytes. `[hooks] disable = name, 0xADDR, prefix*`. Member functions: `core::detail::CodeAddress(&C::f)`.

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

Run with `--selftest`; result lines `SELFTEST PASS|FAIL <name>: <detail>`.

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

`python tools\gen\gen_all.py` regenerates, `--check` (or target `fut_gen`) verifies. Generated files are
checked in; the build needs no Python. New function starts outside Ghidra go into
`extra_function_starts.tsv` (with evidence) - this also makes them hookable.

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
