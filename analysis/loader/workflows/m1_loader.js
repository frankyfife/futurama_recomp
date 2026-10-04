export const meta = {
  name: 'futurama-m1-loader',
  description: 'Implement loader milestone M1 (XBE mapped, kernel resolved, run to main) with skeleton, parallel modules, integration, review and fixes',
  phases: [
    { title: 'Skeleton', detail: 'CMake, launcher, host DLL skeleton, interfaces, generators' },
    { title: 'Modules', detail: 'core/loader/hooks, kernel core, file I/O + VFS in parallel' },
    { title: 'Integrate', detail: 'build, run to main, debug' },
    { title: 'Review', detail: 'ABI review and independent reproduction of exit criteria' },
    { title: 'Fix', detail: 'apply confirmed findings, final report' },
  ],
}

const CONTEXT = `
PROJECT: D:\\FuturamaDecomp - native PC port of the Xbox game "Futurama" (2003, retail EU XBE, XDK 5233, NetImmerse 4.2.1) via a loader: a launcher EXE fixed at 0x10000 + a host DLL that provides the xboxkrnl kernel (HLE), maps the original XBE and runs it natively (the Xbox CPU is x86).
BINDING DESIGN: D:\\FuturamaDecomp\\docs\\loader_design.md - read it fully before working; follow its decisions (E1-E12) and section details. Supporting verified reports in D:\\FuturamaDecomp\\analysis\\loader\\: kernel.md + kernel_imports.tsv (111 kernel slots: ordinal, name, kind, convention, args, purge_bytes, slot, thunk_va, host_plan ...), cpu.md + fs_sites.tsv (fs: sites; the 38 rows with offset != 0x0 get patched), startup.md, files.md, launcher_probe.md (+ working probe code in analysis\\loader\\toolchain_probe\\: placeholder.asm, launcher.c, build_launcher.cmd), d3d.md, avi.md, renderer.md.
GHIDRA SNAPSHOT (grep it, do not open Ghidra): D:\\FuturamaDecomp\\analysis\\retail_eu_export\\decomp\\*.c (decompiled C of all functions, each starts "// ==== FUNC <addr> <name> size=<n> ===="), functions.tsv, callgraph.tsv, strings.tsv. Python 3.14 with capstone; D:\\FuturamaDecomp\\tools\\xbe.py (class Xbe: read/u32/cstr/section/va2off).
MILESTONE M1 (design section 6, Anhang A/B): XBE mapped at 0x10000 with PE/XBE polyglot header, all 111 kernel thunk slots resolved (real implementations for the M1 set of Anhang A, generated logging stubs with correct stack purge for the rest), 38 fs patches, XAPI hooks (QPC/QPF, OutputDebugStringA, XMountUtilityDrive, XLaunchNewImageA), boot: entry 0xF84DD -> PsCreateSystemThreadEx -> XapiThreadStartup -> 0xF8469 -> XapiInitProcess -> _rtinit -> _cinit -> main 0xD29B0; with --stop-at=main the main hook logs and the process exits with code 0. Self-tests of design 1.4 step 8.
TOOLCHAIN: MSVC 14.44 x86 via "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars32.bat"; cmake + ninja on PATH; Windows SDK 10.0.26100; MASM ml.exe; clang-cl 23 at C:\\Program Files\\LLVM\\bin (optional). QUIRK: cmd's AutoRun changes the working directory, so ALWAYS call batch files by FULL path (e.g. from PowerShell: cmd /c "D:\\FuturamaDecomp\\build.cmd" <args>) and every .cmd must start with: cd /d "%~dp0". vcvars32 prints a harmless vswhere message.
GAME DATA (read-only, never modify, never copy into src\\): ISO "Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso" (path has spaces, parentheses and commas: pass it through a config file, not the command line); extracted XBE D:\\FuturamaDecomp\\orig\\retail_eu\\default.xbe (SHA-1 58f76de5bc739bf016d74c1d7f23e348f65ea4b4).
TEST RUNS: use per-agent user data dirs under D:\\FuturamaDecomp\\run\\<name>\\ (partition1, logs) via config, never %LOCALAPPDATA% during tests.
CONVENTIONS (design 5.3): code and comments in English, docs in German; namespaces krnl::, xapi::, vfs::, guest::, core::; guest-visible structs are POD with static_assert(sizeof/offsetof); no C++ exception may leave an HLE/hook entry (noexcept + catch-all -> KeBugCheck-style abort with log); NO inline assembly - use MASM .asm files; guest addresses only via named constants; C++20, /W4 /permissive- /utf-8 /EHsc. Every HLE function gets a header comment: ordinal/slot or address, convention, args, evidence (report + section or address). Mark guesses "// ASSUMPTION:".
`

const SKELETON = `${CONTEXT}
YOUR TASK (phase 1 of 5, you are alone right now): build the project skeleton that three agents will then fill in parallel. Everything must compile and link at the end of your task.
1. Root: CMakeLists.txt, CMakePresets.json (Ninja, MSVC x86, presets x86-debug and x86-relwithdebinfo), build.cmd (args: [builddir] [Debug|RelWithDebInfo] [target]; calls vcvars32, configures if needed, builds; usable from any agent with its own build dir, default build\\x86-debug), .clang-format, .gitignore (build\\, run\\).
2. src\\launcher\\: placeholder.asm + launcher.c per design 1.3 (start from analysis\\loader\\toolchain_probe\\, which works): target fut_launcher -> futurama.exe with the exact link options of design 5.2 on the command line; LauncherEntry allocates the 5 TLS slots (S_TLS, S_KTHREAD, S_PRCB, S_IRQL, S_DPC) before LoadLibrary, checks index < 64, checks &xbe_space == 0x11000 and size, loads futurama_host.dll, calls HostMain(LauncherInfo*). Define LauncherInfo in src\\shared\\launcher_info.h.
3. src\\host\\ (target fut_host, futurama_host.dll exporting HostMain) with the directory layout of design 5.1. Use file(GLOB CONFIGURE_DEPENDS ...) per directory so other agents never need to edit CMake. Provide:
   - core\\log.h/.cpp (channels/levels of design 5.4, file + console output), core\\config.h/.cpp (INI file futurama.ini next to the exe or --config=<file>; keys at least: iso, xbe (optional path to an extracted default.xbe), userdata, stop_at, selftest, strict, trace; command line --stop-at=, --selftest, --config=, --userdata=), core\\guest.h (guest address constants and helpers: read/write guest memory, named constants for every address used in M1), core\\hooks.h (API: register a detour address -> replacement with expected original bytes; apply/validate; disable by name), core\\selftest.h (registry of named self-tests).
   - kernel\\kernel_api.h: the registration mechanism for kernel implementations. Required design: tools\\gen\\gen_kernel_table.py reads analysis\\loader\\kernel_imports.tsv and generates kernel\\gen\\kernel_table.gen.h (per export: constexpr ordinal, slot, kind, convention, arg bytes/purge) and kernel\\gen\\kernel_table.gen.cpp (for every FUNC slot a logging stub with the exact convention and purge using DWORD parameters, e.g. NTSTATUS __stdcall Stub_NtCreateFile(DWORD a1..a9) - no asm; stubs log "UNIMPLEMENTED Ord <n> <Name>(args) ret=<caller>" once per return address and return STATUS_NOT_IMPLEMENTED; DATA/ADDR slots get placeholders). Implementations register via a macro, e.g. KRNL_FUNC(NtCreateFile, &krnl::NtCreateFile) / KRNL_DATA(XboxKrnlVersion, &g_XboxKrnlVersion), which static_asserts that the implementation's calling convention and argument byte count match the generated constants (template trait over the function pointer type) and records the pointer in a registry. At thunk resolution each slot uses the registered implementation or the stub; HostMain logs which slots are still stubs. Fastcall slots (KfLowerIrql, KfRaiseIrql, IofCompleteRequest, ObfDereferenceObject) and cdecl DbgPrint must be handled.
   - kernel\\ob_handles.h/.cpp: guest handle table (guest HANDLE values -> refcounted host objects with a virtual destructor/close), plus NtClose (slot from kernel_imports.tsv) implemented here because every object type needs it.
   - vfs\\vfs.h: interfaces for the object namespace, devices, symbolic links, file objects and an XDVDFS ISO reader class (implemented later by the I/O agent; you provide declarations and minimal stubs that compile).
   - xapi\\, platform\\, asm\\ folders with placeholder files; HostMain in core\\host_main.cpp calling clearly named, empty stage functions in the order of design 1.10 (load config/log -> load+map XBE -> polyglot -> thunks -> fs patches -> hooks -> self-tests -> VFS -> adopt boot thread and call entry -> wait/message loop), each stage a function declared in a header owned by the agent that implements it.
   - src\\shared\\: guest structs needed by more than one module (ANSI_STRING, UNICODE_STRING (Xbox: ANSI object names), OBJECT_ATTRIBUTES (Xbox layout: RootDirectory, ObjectName, Attributes - verify against kernel.md), IO_STATUS_BLOCK, LARGE_INTEGER, NTSTATUS constants, XBE header structs) with static_asserts.
4. tools\\gen\\gen_kernel_table.py and tools\\gen\\gen_fs_patches.py (from fs_sites.tsv rows with offset != 0x0: address, original bytes, disp position, target slot per design 1.6) and tools\\gen\\gen_functions.py (functions.tsv -> src\\host\\gen\\xbe_functions.gen.inc address/name table for backtraces; exclude the init stubs range 0x17e010-0x233880 to keep it small). Generated files are checked in (no Python needed at build time); a --check mode verifies they are current.
5. Write D:\\FuturamaDecomp\\src\\INTERFACES.md (English): every API the other agents must use, the registration macros with an example, the stage functions of HostMain and who implements them, and this FILE OWNERSHIP table (binding for phase 2):
   - core agent: src\\launcher\\**, src\\host\\core\\** (except log/config which it may extend), src\\host\\xapi\\**, src\\host\\asm\\**, src\\host\\platform\\**, src\\host\\gen\\fs_patch_table.gen.inc consumer code
   - kernel agent: src\\host\\kernel\\** except kernel\\io_*.* and ob_handles.*
   - io agent: src\\host\\vfs\\**, src\\host\\kernel\\io_*.*
   - shared (skeleton-owned; other agents may only ADD declarations, and must note each change in a "Changelog" section at the end of INTERFACES.md): CMakeLists.txt files, build.cmd, src\\shared\\**, kernel\\kernel_api.h, kernel\\ob_handles.*, kernel\\gen\\**, core\\log.*, core\\config.*, core\\guest.h, vfs\\vfs.h
6. Build with build.cmd (Debug) and make sure futurama.exe and futurama_host.dll link; run futurama.exe once (it may stop early; it must not crash before logging). Verify with dumpbin that the placeholder is at 0x11000 in futurama.exe.
Return the structured result.`

const SKELETON_SCHEMA = {
  type: 'object',
  properties: {
    build_ok: { type: 'boolean' },
    interfaces_doc: { type: 'string' },
    summary: { type: 'string', description: 'what exists now, key APIs, how to build/run' },
    known_issues: { type: 'array', items: { type: 'string' } },
  },
  required: ['build_ok', 'interfaces_doc', 'summary', 'known_issues'],
}

const MODULE_SCHEMA = {
  type: 'object',
  properties: {
    module: { type: 'string' },
    compiles: { type: 'boolean' },
    implemented: { type: 'array', items: { type: 'string' }, description: 'functions/features implemented' },
    shared_changes: { type: 'array', items: { type: 'string' } },
    open_points: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' },
  },
  required: ['module', 'compiles', 'implemented', 'shared_changes', 'open_points', 'summary'],
}

const MODULE_RULES = `
PHASE 2 RULES: three agents work IN PARALLEL on disjoint files. Read D:\\FuturamaDecomp\\src\\INTERFACES.md first and obey its FILE OWNERSHIP table strictly: edit only files you own; in shared files only ADD declarations and log each change in the Changelog of INTERFACES.md. Build only in YOUR OWN build directory (pass it to build.cmd, e.g. build\\<module>), never in build\\x86-debug. If the build fails because of another agent's file that is mid-edit, wait a minute and retry; your own files must compile without warnings. Do not run the full game yet unless your module can be tested in isolation (unit tests in tests\\ are welcome, doctest single header may be added under third_party\\ if you need it). Prefer verifying semantics against the decompiled guest code (callers of each kernel function) over memory.`

const MODULES = [
  {
    key: 'core',
    prompt: `${MODULE_RULES}
YOU ARE THE CORE AGENT. Implement (design 1.3, 1.4, 1.6, 1.9-1.11, 3, 5.4, Anhang B M1 rows):
- launcher finalisation if needed; HostMain stages: config/log init, crash handling (VEH ring buffer for first-chance exceptions with EIP in 0x10000-0x31B000 without interfering, SetUnhandledExceptionFilter, minidump via DbgHelp into userdata\\logs), XBE loading (from config "xbe" path or, if the I/O agent's XDVDFS reader is available via vfs.h, from the ISO; verify SHA-1 58f76de5...), header checks, VirtualProtect + zero fill 0x11000-0x31B000, section copy for all 10 sections, PE/XBE polyglot exactly as design 1.4 step 5 (save launcher NT headers first), kernel thunk resolution using the kernel registry API from kernel_api.h (abort on unknown ordinal), DATA/ADDR slots, fs patches from src\\host\\gen\\fs_patch_table.gen.inc (verify original bytes, retarget disp32 to 0xE10+4*slot using LauncherInfo TLS indices), the hook engine (5-byte jmp detours with original-byte verification, registry, per-hook disable via config), XAPI hooks of Anhang B for M1 (QueryPerformanceCounter 0xF641B / QueryPerformanceFrequency 0xF642C -> Win32 QPC/QPF, OutputDebugStringA 0xF3DE0 -> log, XMountUtilityDrive 0xF7F7C -> TRUE (and whatever the design says about Z:), XLaunchNewImageA 0xF82E3 -> log + exit), the --stop-at=main hook on 0xD29B0 (log, flush, ExitProcess(0)), boot-thread adoption (TLS slot values per design 1.6 for the boot thread: KTHREAD.TlsData = 0 with a valid dummy TLS anchor) and the call of entry 0xF84DD, then waiting until the guest main thread ends the process (M1 needs no window yet; keep a simple wait/message loop).
- The guest main thread itself is created by the kernel agent's PsCreateSystemThreadEx; agree on the TLS-slot setup helper: you own a function in core (declare it in INTERFACES.md if missing) that fills the 5 TLS slots for the current thread given a fake KTHREAD/TlsData; the kernel agent calls it.
- self-tests of design 1.4 step 8 (TLS slot readback via __readfsdword, RtlImageNtHeader(GetModuleHandle(NULL)) after polyglot, CreateThread with 0 stack, guest IsBadReadPtr(0,4) at 0x1709C9 returns TRUE on an adopted thread, section/BSS comparison) controlled by --selftest; XBE function table backtraces in crash logs (xbe_functions.gen.inc).
- Use MASM files in src\\host\\asm\\ for anything that needs exact register control (e.g. calling guest stdcall/fastcall functions if C++ function pointer casts are insufficient).`,
  },
  {
    key: 'kernel',
    prompt: `${MODULE_RULES}
YOU ARE THE KERNEL AGENT. Implement every non-file kernel export of the M1 set (design Anhang A, rows "M1 - Boot-Pfad" and the "Defensiv" row) plus all DATA/ADDR exports (design 1.5 table), following design 1.5, 1.7, 1.8 exactly and kernel.md (section numbers there give per-function semantics and evidence):
KfLowerIrql, KeRaiseIrqlToDpcLevel, (KfRaiseIrql), KeInitializeTimerEx, KeInitializeDpc, KeSetTimer (remember only), KeCancelTimer if trivial, RtlInitializeCriticalSection/RtlEnterCriticalSection/RtlLeaveCriticalSection (map guest address -> host CRITICAL_SECTION, lazily created), NtAllocateVirtualMemory/NtFreeVirtualMemory/NtQueryVirtualMemory (Xbox argument lists, assertion < 0x80000000), MmFreeContiguousMemory (+ MmAllocateContiguousMemory(Ex) and MmQueryAllocationSize are cheap to do now), MmQueryAddressProtect/MmSetAddressProtect, RtlInitAnsiString, RtlCompareMemoryUlong, RtlEqualString, RtlNtStatusToDosError (full mapping table for the statuses the guest can see), RtlRaiseException and RtlUnwind (forward to ntdll), KeQuerySystemTime, ExQueryNonVolatileSetting (values per kernel.md/files.md: language from config default English, video flags, AV region etc.; document every index you answer), XcSHAInit/XcSHAUpdate/XcSHAFinal (state inside the guest context buffer, 0x74 bytes per design), XcVerifyPKCS1Signature (return what the design says), HalReturnToFirmware (log + exit process), HalRegisterShutdownNotification (no-op), HalInitiateShutdown (log), PsCreateSystemThreadEx (10 args, host CreateThread with max(KernelStackSize, 2 MiB) stack; new thread: allocate TlsData 0x24 and a zeroed fake KTHREAD 0x140 with the fields of design 1.6, call the core agent's TLS-slot helper, then call SystemRoutine(StartContext1, StartContext2) exactly like the Xbox kernel: verify the calling convention of the system routine 0xF99DA from the decomp), PsTerminateSystemThread (ExitThread), KeBugCheck (log code + backtrace via core crash API, terminate), XeLoadSection/XeUnloadSection (section header pointers at 0x1036C.. per cpu.md/kernel.md; loading just marks loaded and returns success since all sections are pre-mapped; check what XAPI expects in the section header ref counts), KeSetEvent/KeWaitForSingleObject minimal (design 1.7), DbgPrint (cdecl, log). Data exports: IoInvalidDeviceRequest (ADDR), ExEventObjectType, XboxHDKey (16 bytes != 0), XboxSignatureKey (16 bytes from config, default as design 4.5), XboxAlternateSignatureKeys, NtUserIoApcDispatcher (ADDR: implement as a callable routine with the right convention per kernel.md; the I/O agent will use its address as marker), HalDiskCachePartitionCount, LaunchDataPage, XboxHardwareInfo (Flags bit 3 = 0), XePublicKeyData, XboxKrnlVersion {1,0,5838,1}, IdexChannelObject, HalBootSMCVideoMode.
Register everything via the macros in kernel_api.h; the static_asserts on purge must pass. For each function check the guest callers in the decomp to confirm argument order and how results/out-params are used.`,
  },
  {
    key: 'io',
    prompt: `${MODULE_RULES}
YOU ARE THE I/O AGENT. Implement the virtual file system and the file-related kernel exports of the M1 set (design section 4 and 1.7 APC notes, files.md, kernel.md):
- vfs: object namespace (\\Device\\CdRom0, \\Device\\Harddisk0\\Partition1, \\??\\ drive symlinks created by IoCreateSymbolicLink from XapiInitProcess: D:, T:, U:, Z: ...), symlink table, an XDVDFS ISO reader in C++ (port tools\\xdvdfs.py; the image may be a plain XDVDFS image at offset 0 or a redump with the game partition at 0x18300000), the CdRom0 device serving the ISO's files (read-only, 2048-byte sector semantics; reads may run past EOF per files.md), and a host-directory device for partition1 rooted at <userdata>\\hdd\\partition1 (create on demand). Case-insensitive path handling as on the Xbox.
- kernel exports: NtCreateFile, NtOpenFile, NtReadFile and NtWriteFile (synchronous in M1, but keep the structure for the M2 APC path of design 1.7: if ApcRoutine is NtUserIoApcDispatcher, queue the completion instead of finishing synchronously - at least leave a clear TODO and log), NtQueryInformationFile, NtSetInformationFile, NtQueryVolumeInformationFile (partition1 must report a cluster size so that XGetDiskClusterSizeA("U:\\\\") == 0x4000 and free space < 4 GiB, see files.md), NtDeviceIoControlFile (error status as design says, but log the IOCTL), NtFsControlFile (defensive), IoCreateSymbolicLink, IoDeleteSymbolicLink if trivial, NtQueryFullAttributesFile and NtQueryDirectoryFile if cheap (M2 needs them). File objects live in the guest handle table from ob_handles.h; NtClose is already implemented there - make your objects close correctly.
- Get the exact Xbox argument lists from kernel.md / kernel_imports.tsv (Xbox NtCreateFile has 9 args, NtReadFile 8 without Key, etc.) and verify against the XAPI callers in the decomp (CreateFileA 0xF76E0 path, XapiInitProcess drive mapping, XGetDiskClusterSizeA, the TitleMeta/TitleImage writes of design M1 exit criterion 3).
- unit tests for the XDVDFS reader and path resolution (tests\\, may read the real ISO read-only).`,
  },
]

phase('Skeleton')
const skel = await agent(SKELETON, { label: 'skeleton', phase: 'Skeleton', schema: SKELETON_SCHEMA })
if (!skel || !skel.build_ok) {
  log('Gerüst baut nicht - Abbruch nach Phase 1')
  return { stage: 'skeleton', skel }
}
log('Gerüst steht, starte drei Modul-Agenten parallel')

phase('Modules')
const mods = await parallel(MODULES.map((m) => () =>
  agent(`${CONTEXT}\nSkeleton summary from phase 1: ${skel.summary}\nKnown skeleton issues: ${skel.known_issues.join(' | ')}\n${m.prompt}`,
    { label: `module:${m.key}`, phase: 'Modules', schema: MODULE_SCHEMA })))

const modDigest = mods.filter(Boolean).map((m) => `[${m.module}] compiles=${m.compiles}; implemented: ${m.implemented.join(', ')}; shared changes: ${m.shared_changes.join(' | ')}; open: ${m.open_points.join(' | ')}`).join('\n')

const STATUS_SCHEMA = {
  type: 'object',
  properties: {
    reached_main: { type: 'boolean' },
    exit_criteria: {
      type: 'array',
      items: {
        type: 'object',
        properties: { criterion: { type: 'string' }, status: { type: 'string', enum: ['erfuellt', 'teilweise', 'offen'] }, evidence: { type: 'string' } },
        required: ['criterion', 'status', 'evidence'],
      },
    },
    stubs_hit: { type: 'array', items: { type: 'string' }, description: 'UNIMPLEMENTED kernel calls seen on the boot path' },
    problems_solved: { type: 'array', items: { type: 'string' } },
    open_problems: { type: 'array', items: { type: 'string' } },
    how_to_run: { type: 'string' },
  },
  required: ['reached_main', 'exit_criteria', 'stubs_hit', 'problems_solved', 'open_problems', 'how_to_run'],
}

phase('Integrate')
const integ = await agent(`${CONTEXT}
PHASE 3: INTEGRATION. Three module agents finished (results below). You now own ALL files. Goal: reach milestone M1 and its six exit criteria (design section 6, M1).
Module results:
${modDigest}
Steps: build everything in build\\x86-debug (Debug) without warnings; create a test config run\\integ\\futurama.ini (iso path, userdata D:\\FuturamaDecomp\\run\\integ, stop_at=main, selftest=1); run futurama.exe; debug every failure on the boot path (use the logs, the VEH ring buffer, the decompiled guest code in analysis\\retail_eu_export\\decomp, and capstone on the XBE). Typical traps to expect: wrong purge/convention, out-parameters not written, NTSTATUS expected by XAPI, path/device naming, TLS/fs slots, SEH validation (R1), XGetDiskClusterSize 0x4000, TitleMeta/TitleImage writes. Fix root causes, not symptoms; keep design decisions unless they are proven wrong (then document the deviation in docs\\loader_design.md section "Abweichungen in M1" in German with evidence). Iterate until main is reached with exit code 0, or until you are certain what blocks it.
Then check exit criteria 1-6 one by one (run 10 times for criterion 6 and compare logs after stripping timestamps/addresses). Return the structured status.`,
  { label: 'integrate', phase: 'Integrate', schema: STATUS_SCHEMA })

const integDigest = integ ? `reached_main=${integ.reached_main}; criteria: ${integ.exit_criteria.map((c) => `${c.criterion}: ${c.status} (${c.evidence})`).join(' | ')}; stubs hit: ${integ.stubs_hit.join(', ')}; open: ${integ.open_problems.join(' | ')}; run: ${integ.how_to_run}` : 'integration agent failed'

const FINDINGS_SCHEMA = {
  type: 'object',
  properties: {
    findings: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          file: { type: 'string' },
          severity: { type: 'string', enum: ['kritisch', 'hoch', 'mittel', 'niedrig'] },
          problem: { type: 'string' },
          evidence: { type: 'string' },
          suggested_fix: { type: 'string' },
        },
        required: ['file', 'severity', 'problem', 'evidence', 'suggested_fix'],
      },
    },
    summary: { type: 'string' },
  },
  required: ['findings', 'summary'],
}

phase('Review')
const reviews = await parallel([
  () => agent(`${CONTEXT}
PHASE 4: INDEPENDENT ABI/SEMANTICS REVIEW (read-only: do NOT edit src\\; you may write scripts under D:\\FuturamaDecomp\\run\\review_abi\\). Integration status: ${integDigest}
Review every kernel implementation, the generated stub table, the thunk resolution, fs patches and hooks against the binary and the reports: calling convention and purge per slot (compare with kernel_imports.tsv and with the actual guest call sites: stack cleanup after the call, register args for fastcall), callee-saved registers, out-parameters and IO_STATUS_BLOCK writes, NTSTATUS values the guest code checks, struct layouts (OBJECT_ATTRIBUTES, ANSI_STRING, FILE_* info classes, XBE section headers), APC/alertable semantics left for M2, fs patch disp positions vs instruction encodings (decode each of the 38 sites with capstone before/after), hook original-byte checks, TLS slot values per design 1.6, polyglot header correctness (compare against design_header_refs.out: no guest-read byte overwritten). Try hard to find real bugs; report only issues with concrete evidence. Return findings.`,
  { label: 'review:abi', phase: 'Review', schema: FINDINGS_SCHEMA }),
  () => agent(`${CONTEXT}
PHASE 4: INDEPENDENT REPRODUCTION (do NOT edit src\\; use your own build dir build\\review and user data D:\\FuturamaDecomp\\run\\review_repro\\). Integration status claimed: ${integDigest}
Do a clean configure+build from scratch with build.cmd (Debug and RelWithDebInfo), then verify each M1 exit criterion yourself (design section 6 M1, 1-6): self-tests, XapiInitProcess without BootToDash/HalReturnToFirmware, partition1 contents (TDATA\\46490002\\, UDATA\\46490002\\TitleMeta.xbx in UTF-16, TitleImage.xbx 0x2800 bytes identical to the $$XTIMAGE section of the XBE - compare bytes), _cinit runs through incl. the 38 MiB pool allocation and XGetDiskClusterSizeA("U:\\\\") == 0x4000, main hook with exit code 0, 10 runs with identical logs modulo timestamps/addresses. Also test robustness: missing ISO path, wrong XBE, run from another working directory, path with spaces. Report every deviation as a finding with evidence (commands and outputs).`,
  { label: 'review:repro', phase: 'Review', schema: FINDINGS_SCHEMA }),
])

const allFindings = reviews.filter(Boolean).flatMap((r) => r.findings)
log(`Review: ${allFindings.length} Befunde`)

phase('Fix')
const final = await agent(`${CONTEXT}
PHASE 5: FIX AND REPORT. You own all files. Integration status: ${integDigest}
Review findings (verify each one yourself before changing anything; reject findings that do not hold, with a reason):
${allFindings.map((f, i) => `${i + 1}. [${f.severity}] ${f.file}: ${f.problem} | evidence: ${f.evidence} | fix: ${f.suggested_fix}`).join('\n')}
Fix all confirmed findings, rebuild (Debug and RelWithDebInfo, no warnings), rerun the M1 checks (stop-at=main, self-tests, 10 runs), then write the German report D:\\FuturamaDecomp\\docs\\m1_report.md: what was built (structure, components), how to build and run (exact commands, config file example with the ISO path), status of each M1 exit criterion with evidence, list of kernel slots implemented vs stub (with counts), deviations from the design, accepted/rejected review findings, known issues, and concrete next steps for M2. Also update D:\\FuturamaDecomp\\README.md briefly (German) with a "Loader" section pointing to the build/run instructions. Return a short German summary (max 12 sentences) including whether main was reached.`,
  { label: 'fix+report', phase: 'Fix' })

return { skeleton: skel.summary, modules: mods.filter(Boolean).map((m) => ({ module: m.module, compiles: m.compiles })), integration: integ && { reached_main: integ.reached_main, criteria: integ.exit_criteria }, findings: allFindings.length, final }
