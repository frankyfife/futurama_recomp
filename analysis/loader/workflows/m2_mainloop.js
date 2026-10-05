export const meta = {
  name: 'futurama-m2-mainloop',
  description: 'Implement loader milestone M2 (file system/archives, fibers, null renderer, XDK stubs, main loop without picture and sound) with parallel modules, integration, review and fixes',
  phases: [
    { title: 'Modules', detail: 'gfx null renderer, XDK stubs, fibers/kernel stacks, async I/O in parallel' },
    { title: 'Integrate', detail: 'run past main, --frames=3000, m2_check.py' },
    { title: 'Review', detail: 'semantics review and independent reproduction' },
    { title: 'Fix', detail: 'apply confirmed findings, report' },
  ],
}

const CONTEXT = `
PROJECT: D:\\FuturamaDecomp (git repo, branch main; do NOT commit - the user commits on request). Native PC port of the Xbox game "Futurama" (retail EU XBE) via a loader: launcher EXE fixed at 0x10000 + host DLL futurama_host.dll that maps the original XBE and provides the xboxkrnl kernel (HLE).
STATE: Milestone M1 is done and verified: the guest boots entry -> XapiInitProcess -> _rtinit -> _cinit -> main 0xD29B0 (exit 0 with --stop-at=main). Read D:\\FuturamaDecomp\\docs\\m1_report.md (build/run guide, sections 8 "Bekannte Probleme" and 9 "Naechste Schritte fuer M2" are your task list), D:\\FuturamaDecomp\\src\\INTERFACES.md (APIs, registration macros KRNL_FUNC/KRNL_DATA, FUT_HOOK, FUT_SELFTEST, file ownership conventions, changelog), and the binding design D:\\FuturamaDecomp\\docs\\loader_design.md (sections 1.7 threads/fibers/APCs, 2.2 graphics boundary + NiPCRenderer layout + D3D shim table, 4 VFS, 5.4 logging/tracing, 6 "M2" scope and exit criteria, Anhang A/B M2 rows).
GOAL: Milestone M2 = "Dateisystem, Archive, Hauptschleife ohne Bild und Ton". Exit criteria (design section 6, M2): 1. log shows SysPAL.img and FutPAL00-04.img mounted, FutPAL05.img not found, PAL mode (CApp+0xE8 = 0); 2. language from config takes effect (e.g. german -> XGetLanguage = 3); 3. BUILD.TXT = "1.25" read; 4. the intro context runs and --frames=3000 ends orderly without hanging; 5. fiber switches > 0 and APC deliveries > 0 counted, the disc error handler (target of DAT_0030E68C) is never called; 6. self-test "SEH on fiber stack" (IsBadReadPtr inside a guest fiber) passes; 7. vfs_trace.log lists all opened paths, each a hit or a documented "missing".
EVIDENCE SOURCES: verified reports in D:\\FuturamaDecomp\\analysis\\loader\\ (renderer.md + nixboxrenderer_vtable.tsv + scripts\\renderer_retn.py for the ret N of all 75 NiXBoxRenderer slots, d3d.md + d3d_functions.tsv, avi.md + avi_api_surface.tsv for DSOUND/XMV/XPP entry points incl. conventions, files.md (archives, NiUDSStream 0x13F250, APC completion 0x13F1D0, alertable waits 0x13F4C0/0x13F590/0xE0030, disc error handler), startup.md (fibers, CApp, main loop 0xA2C20, contexts), cpu.md, kernel.md + kernel_imports.tsv). Ghidra snapshot to grep (never open Ghidra): D:\\FuturamaDecomp\\analysis\\retail_eu_export\\decomp\\*.c ("// ==== FUNC <addr> <name> size=<n> ===="), functions.tsv, callgraph.tsv, strings.tsv. Python 3.14 + capstone; D:\\FuturamaDecomp\\tools\\xbe.py.
TOOLCHAIN: build with cmd /c "D:\\FuturamaDecomp\\build.cmd" <builddir> <Debug|RelWithDebInfo> (always the FULL path; cmd AutoRun changes the directory). MSVC 14.44 x86, MASM, CMake/Ninja; clang-cl preset exists (build.cmd ... clang). futurama.exe is a GUI-subsystem exe: from PowerShell use Start-Process -Wait -PassThru; from Bash call it directly. Unit tests: <builddir>\\bin\\fut_tests.exe; M1 check: tools\\m1_check.py (keep it passing).
GAME DATA (read-only): ISO "Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso" - pass via INI ([paths] iso=...), see run\\integ\\futurama.ini as template. Test runs only under D:\\FuturamaDecomp\\run\\<your-name>\\ (own userdata). Never copy game data into src\\.
CONVENTIONS: as in INTERFACES.md / design 5.3: English code+comments, German docs; noexcept HLE entries with catch-all; no inline asm (MASM files in src\\host\\asm\\); guest addresses only as named constants; every HLE/hook function has a header comment with address/slot, convention, args, evidence; "// ASSUMPTION:" for guesses; generated files checked in with --check mode in tools\\gen\\gen_all.py; zero warnings (also with FUT_WERROR).
`

const MODULE_RULES = `
PHASE 1 RULES: four agents work IN PARALLEL on disjoint files. Edit only the files you own (listed in your task); shared files (CMakeLists.txt, build.cmd, src\\shared\\**, kernel\\kernel_api.h, kernel\\ob_handles.*, core\\log.*, core\\config.*, core\\guest.h, vfs\\vfs.h, tools\\gen\\gen_all.py, src\\INTERFACES.md) may only receive ADDITIVE changes, each noted in the Changelog section of src\\INTERFACES.md. Build only in YOUR OWN build directory build\\m2-<module>. If a build breaks because of another agent's file mid-edit, wait and retry; your own files must compile without warnings. Test your part in isolation where possible (unit tests in tests\\, self-tests, or a run with --stop-at at a suitable address); the full M2 run is the integration agent's job. Verify every guest-facing contract (calling convention, ret N, out-params, return values) against the decompiled guest callers, not from memory.`

const MODULES = [
  {
    key: 'gfx',
    prompt: `YOU ARE THE GFX AGENT. You own src\\host\\gfx\\**, tools\\gen\\gen_d3d_traps.py, src\\host\\gen\\d3d_traps.gen.inc, tests\\gfx_*.cpp.
Implement (design 2.2, m1_report 9 items 1, 2, 6):
- Detour of NiXBoxRenderer::Create 0x14C170 (cdecl, 8 args; verify) returning a NiPCRenderer null renderer: guest-visible object 0x6D0 bytes with the layout table of design 2.2 (+0x00 own 75-slot vtable, +0x04 refcount, +0x08 accumulator, +0x0C/+0x10 property/effect state written by callers, +0x60 dummy device pointer != 0, +0x6C0 host pointer). Construct via the original NiRenderer base ctor FUN_00140550 (first verify its convention at the call in FUN_0014af00) so it is in the global renderer list 0x2D7B1C, then set the vtable. Every slot is a no-op with the exact ret N from analysis\\loader\\scripts\\renderer_retn.py / nixboxrenderer_vtable.tsv (generate the 75 thunks; MASM or C++ with matching thiscall signatures - prove each ret N by disassembling your build), except: slots 2-10 point to the original entries of vtable 0x2AA7C0, slot 0 destroys via FUN_001406e0, slot 1 returns &NiXBoxRenderer::ms_RTTI 0x30EE00, slots 65/66 return TRUE, slot 39 returns FALSE, slot 16 (DisplayFrame) increments a frame counter used by --frames=N for an orderly shutdown (config key + command line option; design 5.4). Texture renderer data created by slots 65/66 etc. must be layout-compatible (0x70 bytes, reserved offsets +0x3C, +0x48, +0x4C, +0x54, +0x58, +0x60) so the movie player can write into it. Check the renderer.md list of what the game reads from the renderer (e.g. slot 15/17/21/22/27/39/71) and return sane values (e.g. screen size 640x480).
- D3D shim for the 8 functions of the design 2.2 table (stdcall, ret per d3d_functions.tsv) and D3D traps: generator gen_d3d_traps.py (from d3d_functions.tsv, all D3D entry points with ext_call_sites > 0 that are not shimmed) producing hooks that log "TRAP <name> @<addr> from <ret>" and abort; register them through the hook engine (FUT_HOOK machinery) so overlaps are validated.
- Self-test: the NiPCRenderer vtable thunks' ret N verified at runtime against the table (decode first instruction bytes or compare against expected stack adjustment).`,
  },
  {
    key: 'xdk',
    prompt: `YOU ARE THE XDK AGENT. You own src\\host\\audio\\**, src\\host\\video\\**, src\\host\\input\\**, tools\\gen\\gen_xdk_entries.py, src\\host\\gen\\xdk_entries.gen.inc, tests\\xdk_*.cpp.
Implement safe stubs for the XDK libraries that the game calls (design E7, Anhang B M2 rows, m1_report 9 item 5, avi.md sections on DSOUND/XMV/XPP and the "Fallen fuer den Host"): DSOUND 40 entry points + the 5 XMediaObject stream vtable methods (DirectSoundCreate etc.: out pointers set to NULL or to a harmless dummy object where the game would crash on NULL - check every caller in the decomp; GetStatus -> 0; DirectSoundDoWork no-op), XMV 7 entry points (create returns failure/"2" as the design says so the movie player skips; verify what the callers expect), XPP 7 entry points (XInitDevices no-op, XGetDevices -> 0 devices, XGetDeviceChanges -> FALSE, XInputOpen -> NULL, XInputGetState -> ERROR_DEVICE_NOT_CONNECTED, XInputSetState sets dwStatus synchronously). Generate the hook table from avi_api_surface.tsv with exact conventions and ret N (gen_xdk_entries.py with --check). Each stub logs once per call site at debug level. Make sure the game's sound manager and movie player tolerate the stubs (read their code: SoundManager, SoundData, SoundVoice, movie player 0xEDE40 per avi.md/renderer.md) and document every returned value with evidence.`,
  },
  {
    key: 'fiber',
    prompt: `YOU ARE THE FIBER AGENT. You own src\\host\\asm\\fiber_switch.asm (new), src\\host\\kernel\\mm_kernel_stack.* (new), src\\host\\xapi\\xapi_fiber.cpp (new), src\\host\\core\\selftest_m2.cpp (new), tests\\fiber_*.cpp; you may make additive changes to the kernel's FATAL_STUBS list / generator plan for slots 107/108.
Implement (design 1.7 "Fibers", E9, m1_report 9 items 3, 4, 10, risks R2):
- MmCreateKernelStack / MmDeleteKernelStack (slots 107/108; check kernel_imports.tsv for args/purge): reserve max(Size, config fiberStack default 512 KiB) fully committed, place an 8-byte SEH terminal record at the top, return the top below it, keep a bookkeeping map top -> {base, limit, allocation}; remove 107/108 from the always-abort list.
- SwitchToFiber replacement for 0x17094F (63 bytes, read the original in the decomp/disassembly and startup.md 4.2): MASM function with identical semantics (save ebp/esi/edi/ebx and fs:[0], cur+0xC = esp, esp = new+0xC, [TLS+0xC] = new, restore) that additionally switches TEB.NtTib.StackBase/StackLimit and TEB.DeallocationStack (0xE0C) from the bookkeeping map; for the thread fiber from ConvertThreadToFiber ([f+8] = 0) use the real TEB values saved at the first switch; SEHOP: if the saved ExceptionList of a new fiber is -1, set it to the terminal record. Install it via the hook engine. Count fiber switches (exposed for the M2 check, log at shutdown).
- Self-tests: (a) "SEH on fiber stack": create a guest fiber via the original XAPI CreateFiber 0x1708B0, switch into it, call guest IsBadReadPtr on a NOACCESS page inside the fiber (must return TRUE via guest SEH), switch back; (b) "guest SEH through host wrapper": a guest exception that unwinds through a host noexcept wrapper frame between guest frames (design m1_report section 8) - prove it works or document precisely why not and what rule hooks must follow.`,
  },
  {
    key: 'io',
    prompt: `YOU ARE THE IO AGENT. You own src\\host\\vfs\\**, src\\host\\kernel\\io_*.*, src\\host\\kernel\\ke_apc.* and ke_wait.* (create if missing; move wait functions there only if they are not already implemented elsewhere - coordinate via INTERFACES.md changelog), src\\host\\core\\trace.* (new), tests\\vfs_*.cpp, tests\\apc_*.cpp.
Implement (design 1.7 "APCs", 4, 5.4, Anhang A M2 row, m1_report 9 items 7, 8, files.md 2.x):
- asynchronous NtReadFile/NtWriteFile when ApcRoutine == NtUserIoApcDispatcher: perform the I/O synchronously, write IoStatusBlock (= OVERLAPPED) only on delivery, return STATUS_PENDING, queue {completion = ApcContext, iosb} on a per-guest-thread APC queue (no QueueUserAPC); deliver only in alertable waits: KeDelayExecutionThread (Alertable=TRUE), NtWaitForSingleObjectEx (Alertable=TRUE), returning STATUS_USER_APC (0xC0); delivery calls Completion(dwErr, Information, lpOverlapped) stdcall ret 0xC with dwErr = RtlNtStatusToDosError; plus NtWaitForSingleObject, NtFlushBuffersFile, NtQueryFullAttributesFile, NtQueryDirectoryFile (directory enumeration for the ISO and host dir devices), signal the Event parameter where given; count APC deliveries (expose for the M2 check).
- Re-check deviation A8 (read at/after EOF -> STATUS_END_OF_FILE) against the game's stream code and the disc error handler (DAT_0030E68C target, files.md): reads may legitimately run up to 2047 bytes past EOF; make sure no path triggers the disc error handler.
- --trace=vfs -> <userdata>\\logs\\vfs_trace.log: every open with guest path -> device -> result (hit / not found), and the kernel trace ring buffer (--trace=krnl) wired into the crash report via core::AddCrashReporter.
- Unit tests for the APC queue semantics and directory enumeration.`,
  },
]

const MODULE_SCHEMA = {
  type: 'object',
  properties: {
    module: { type: 'string' },
    compiles: { type: 'boolean' },
    implemented: { type: 'array', items: { type: 'string' } },
    shared_changes: { type: 'array', items: { type: 'string' } },
    open_points: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' },
  },
  required: ['module', 'compiles', 'implemented', 'shared_changes', 'open_points', 'summary'],
}

phase('Modules')
const mods = await parallel(MODULES.map((m) => () =>
  agent(`${CONTEXT}\n${MODULE_RULES}\n${m.prompt}\nReturn the structured result.`,
    { label: `module:${m.key}`, phase: 'Modules', schema: MODULE_SCHEMA })))

const modDigest = mods.filter(Boolean).map((m) => `[${m.module}] compiles=${m.compiles}; implemented: ${m.implemented.join(', ')}; shared changes: ${m.shared_changes.join(' | ')}; open: ${m.open_points.join(' | ')}`).join('\n')
const missing = MODULES.filter((_, i) => !mods[i]).map((m) => m.key)
if (missing.length) log(`Module ohne Ergebnis: ${missing.join(', ')}`)

const STATUS_SCHEMA = {
  type: 'object',
  properties: {
    criteria: {
      type: 'array',
      items: {
        type: 'object',
        properties: { criterion: { type: 'string' }, status: { type: 'string', enum: ['erfuellt', 'teilweise', 'offen'] }, evidence: { type: 'string' } },
        required: ['criterion', 'status', 'evidence'],
      },
    },
    frames_reached: { type: 'integer' },
    stubs_and_traps_hit: { type: 'array', items: { type: 'string' } },
    problems_solved: { type: 'array', items: { type: 'string' } },
    open_problems: { type: 'array', items: { type: 'string' } },
    how_to_run: { type: 'string' },
  },
  required: ['criteria', 'frames_reached', 'stubs_and_traps_hit', 'problems_solved', 'open_problems', 'how_to_run'],
}

phase('Integrate')
const integ = await agent(`${CONTEXT}
PHASE 2: INTEGRATION. The four module agents finished${missing.length ? ` (missing: ${missing.join(', ')} - implement those parts yourself)` : ''}. You now own ALL files.
Module results:
${modDigest}
Steps: build build\\x86-debug (Debug) and build\\x86-relwithdebinfo without warnings; keep tools\\m1_check.py passing; create run\\m2\\futurama.ini (ISO, userdata run\\m2, no stop_at, frames=3000, trace=vfs, language from config) and run past main. Debug every failure on the way (logs, VEH ring buffer, crash minidump, decompiled guest code, capstone). Expect: renderer slots called with unexpected arguments, NULL out-pointers from stubs crashing callers, archive mounting details, APC timing, fiber/SEH issues, a context that waits for a movie or input forever (decide with evidence how the stubs must answer so the intro context proceeds), time step behaviour with real QPC. Fix root causes; document deviations from the design in docs\\loader_design.md section 8 (German, with evidence).
Write tools\\m2_check.py (pattern of m1_check.py; checks all seven M2 exit criteria from the log/trace files, supports --runs N and --fresh; registered as a ctest test) and run it until all criteria pass or you are certain what blocks them. Return the structured status.`,
  { label: 'integrate', phase: 'Integrate', schema: STATUS_SCHEMA })

const integDigest = integ ? `criteria: ${integ.criteria.map((c) => `${c.criterion}: ${c.status} (${c.evidence})`).join(' | ')}; frames: ${integ.frames_reached}; stubs/traps hit: ${integ.stubs_and_traps_hit.join(', ')}; open: ${integ.open_problems.join(' | ')}; run: ${integ.how_to_run}` : 'integration agent failed'

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
PHASE 3: INDEPENDENT SEMANTICS REVIEW (read-only for src\\; scripts and runs only under D:\\FuturamaDecomp\\run\\review_m2_sem\\). Integration status: ${integDigest}
Review all code added or changed for M2 (git diff HEAD shows it) against the binary and the reports: NiPCRenderer slot table (every ret N vs renderer_retn.py, required return values, object layout vs the guest's direct field accesses), D3D shim/traps (conventions, coverage of every D3D entry with external callers), XDK stubs (each out-parameter and return value vs every guest caller - would any caller dereference NULL or loop forever?), MmCreateKernelStack/SwitchToFiber (exact register/stack semantics vs the original 0x17094F, TEB field offsets, SEHOP terminal record), APC queue and alertable waits (delivery points, STATUS_USER_APC, completion signature, IoStatusBlock timing, event signalling), EOF semantics vs the disc error handler. Report only issues with concrete evidence.`,
  { label: 'review:semantics', phase: 'Review', schema: FINDINGS_SCHEMA }),
  () => agent(`${CONTEXT}
PHASE 3: INDEPENDENT REPRODUCTION (do NOT edit src\\; own build dir build\\review-m2, user data D:\\FuturamaDecomp\\run\\review_m2_repro\\). Integration status claimed: ${integDigest}
Clean configure+build from scratch (Debug, RelWithDebInfo, clang), run fut_tests and ctest, run tools\\m1_check.py and tools\\m2_check.py (10 runs each, fresh userdata), and verify each of the seven M2 exit criteria yourself directly from the logs/trace (not only via m2_check.py: also inspect vfs_trace.log by hand against the archive lists in orig\\retail_eu\\*_files.txt and files.md). Test languages english/german/french, a run with --frames=100, and a forced hang check (does the watchdog end a hanging run?). Report every deviation as a finding with commands and outputs.`,
  { label: 'review:repro', phase: 'Review', schema: FINDINGS_SCHEMA }),
])

const allFindings = reviews.filter(Boolean).flatMap((r) => r.findings)
log(`Review: ${allFindings.length} Befunde`)

phase('Fix')
const final = await agent(`${CONTEXT}
PHASE 4: FIX AND REPORT. You own all files. Integration status: ${integDigest}
Review findings (verify each yourself before changing anything; reject findings that do not hold, with a reason):
${allFindings.map((f, i) => `${i + 1}. [${f.severity}] ${f.file}: ${f.problem} | evidence: ${f.evidence} | fix: ${f.suggested_fix}`).join('\n')}
Fix all confirmed findings, rebuild (Debug, RelWithDebInfo, clang; no warnings), rerun fut_tests, ctest, m1_check.py and m2_check.py (10 runs each). Then write the German report D:\\FuturamaDecomp\\docs\\m2_report.md in the style of docs\\m1_report.md: what was built, how to run (config example), status of each M2 exit criterion with evidence, implemented kernel slots/hooks/stubs with counts, what the game does after main now (contexts reached, files opened), deviations, accepted/rejected findings, known issues, and concrete next steps for M3 (first picture with D3D11: which NiRenderer slots in which order, per renderer.md priorities). Update README.md "Loader" section briefly. Do NOT commit. Return a short German summary (max 12 sentences).`,
  { label: 'fix+report', phase: 'Fix' })

return { modules: mods.map((m, i) => ({ module: MODULES[i].key, ok: !!m && m.compiles })), integration: integ && { criteria: integ.criteria.map((c) => `${c.criterion}: ${c.status}`), frames: integ.frames_reached }, findings: allFindings.length, final }
