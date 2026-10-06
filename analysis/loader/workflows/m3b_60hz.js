export const meta = {
  name: 'futurama-m3b-60hz',
  description: 'Switch the game to 60 Hz logic (NTSC timing with PAL content) and display-coupled frame pacing (>= 60 fps), with analysis, implementation, verification, review and report',
  phases: [
    { title: 'Analyse', detail: 'timing semantics and presentation/pacing analysis in parallel' },
    { title: 'Implement', detail: 'timing patch and display-coupled pacer in parallel' },
    { title: 'Verify', detail: 'checks at 60 Hz, 50 vs 60 Hz duration comparison' },
    { title: 'Review', detail: 'timing correctness and pacing review' },
    { title: 'Fix', detail: 'apply findings, update design and report' },
  ],
}

const CONTEXT = `
PROJECT: D:\\FuturamaDecomp (git repo, branch main, remote origin = private GitHub repo; do NOT commit or push - the user does that on request). Native PC port of the Xbox game "Futurama" (retail EU XBE) via a loader: launcher EXE fixed at 0x10000 + host DLL futurama_host.dll (HLE kernel, XDK replacements, NiPCRenderer with a D3D11 backend).
STATE: M1-M3 done: the game boots, runs its main loop and renders legal screen, tech notice, loading screen and frontend menu pixel-accurately compared with xemu reference captures (run\\xemu_ref, run\\xemu_ref_steam). Read: D:\\FuturamaDecomp\\docs\\m3_report.md (build/run/check commands in 3.x, performance in 7, known issues in 10), docs\\m2_report.md, docs\\loader_design.md (1.9 Zeitbasis, 6 M3, 7 R9, 8.x deviations), src\\INTERFACES.md, src\\host\\gfx\\GFX_CONTRACT.md, analysis\\loader\\startup.md (sections 3.1-3.4: CApp::Frame 0xD2630, Game::Frame 0xA2C20 fixed time step, Basis-Init 0xA26A0, PAL/NTSC).
USER REQUIREMENT (binding): the port must run at AT LEAST 60 fps; the 50 Hz PAL timing is explicitly unwanted ("nicht zeitgemaess und unerwuenscht").
KEY FACTS: 0xA26A0 sets the timing by CApp+0xE8 (0 = PAL, the EU disc always yields PAL): PAL branch: [0x2E7B6C]=[0x2E7A60]=[0x2D6744]=50.0f (0x42480000), [0x2E7A5C]=[0x2D6748]=0.02f (0x3CA3D70A, dt), [0x2E7B68]=0x3D214285, [0x2E7B70]="PAL"; NTSC branch: 60.0f (0x42700000), dt 1/60 (0x3C888889), [0x2E7B68]=0x3D064B8A, [0x2E7B70]="NTSC", [0x2E7B74]=0; afterwards [0x2D674C]=1 (fixed step) and [0x2E7A58]=dt. dt [0x2D6748] has ~280 code references; fps [0x2D6744] only 3. The render path has no interpolation (state changes in tick steps). CApp+0xE8 must stay 0 so PAL archives, languages, PAL movies and PAL splash/legal images are kept - only the timing values change.
MACHINE: primary display 3840x2160 at 120 Hz (RTX 5090, 225 % DPI scaling). With the current 50 Hz pacer and VSync, m3_check fails M3-3 here (49.2-49.3 Hz average, 35-41 schedule resets) although frame work is ~3.2 ms (sustainable > 300 Hz): 50 Hz does not divide 120 Hz. 60 Hz = exactly 2 vblanks.
EVIDENCE SOURCES: Ghidra snapshot to grep (never open Ghidra): D:\\FuturamaDecomp\\analysis\\retail_eu_export\\decomp\\*.c ("// ==== FUNC <addr> <name> size=<n> ===="), functions.tsv, callgraph.tsv, strings.tsv; Python 3.14 + capstone; tools\\xbe.py; orig\\proto\\default.xbe is the NTSC prototype (17 June 2003, same engine) for PAL/NTSC comparisons; level scripts orig\\proto\\img\\FutNTSC01\\scripts\\*.ful/*.dbg (compiled script bytecode + debug symbols, magic "L2iM").
TOOLCHAIN: cmd /c "D:\\FuturamaDecomp\\build.cmd" <builddir> <Debug|RelWithDebInfo> (FULL path always; clang via build.cmd ... clang). futurama.exe is a GUI exe (PowerShell: Start-Process -Wait -PassThru). Checks: tools\\m1_check.py, m2_check.py, m3_check.py (commands in m3_report 3.3; configs under run\\m3final\\*). Test runs/screenshots ONLY under D:\\FuturamaDecomp\\run\\<your-name>\\ (git-ignored). ISO (read-only) via INI [paths] iso = Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso.
CONVENTIONS: INTERFACES.md / design 5.3 (English code+comments, German docs, noexcept entries, no inline asm, named guest addresses, header comments with evidence, "// ASSUMPTION:", zero warnings incl. FUT_WERROR, generated files with --check). Code patches go through the existing patch/hook machinery with original-byte verification.
`

const ANALYSE_SCHEMA = {
  type: 'object',
  properties: {
    report_path: { type: 'string' },
    summary: { type: 'string' },
    recommendations: { type: 'array', items: { type: 'string' } },
    risks: { type: 'array', items: { type: 'string' } },
  },
  required: ['report_path', 'summary', 'recommendations', 'risks'],
}

phase('Analyse')
const analyses = await parallel([
  () => agent(`${CONTEXT}
TASK: TIMING SEMANTICS ANALYSIS (read-only for src\\; scripts under analysis\\loader\\scripts\\timing_*.py). Determine whether switching the EU build to the NTSC timing values (60 Hz) keeps gameplay timing correct, and exactly how to do it.
1. Find the exact instructions of the PAL branch in 0xA26A0 that store the timing values (addresses, encodings, immediates) so they can be patched to the NTSC values with original-byte verification; check whether the compiler shares constants between branches.
2. What is [0x2E7B68] (0x3D214285 PAL / 0x3D064B8A NTSC)? Find all readers, decode the meaning (e.g. a per-tick factor, smoothing constant, interlace field time) and whether NTSC's value is the right one at 60 Hz. Same for [0x2E7A60], [0x2E7B6C], [0x2E7A58]/[0x2E7A5C], [0x2E7B70]/[0x2E7B74] (tag: which code reads it - movie names, file names? must stay "PAL").
3. Hunt for frame-count-based timing that would run faster at 60 Hz: constants like 50/50.0f/0.02f/25/100 used as tick counts in game code; counters incremented per tick and compared against fixed limits; the script VM (find the .ful interpreter, its wait/delay opcodes and whether they count ticks or seconds); NiTimeController/animation (seconds?), particle systems, camera smoothing, physics integration (dt-scaled?), sound streaming (packet timing), movie player (15 fps XMV, own clock?), input repeat rates, UI fades. Compare against the NTSC prototype binary where useful (same code, NTSC timing) - if the prototype and retail use identical tick-based code, the US release ran it at 60 Hz, which is strong evidence it is fine.
4. Fixed-step loop details at 60 Hz: debt cap 6*dt, max 100 steps, QPC-based timer FUN_000d2b90 - any issue?
Write analysis\\loader\\timing60.md (German) with evidence per claim and a clear recommendation (values to patch, things to verify at runtime, a list of concrete runtime observables to compare between 50 Hz and 60 Hz runs, e.g. durations in seconds of the legal screen, tech notice, loading screen, frontend idle time until attract mode, attract cycle length).`,
    { label: 'analyse:timing', phase: 'Analyse', schema: ANALYSE_SCHEMA }),
  () => agent(`${CONTEXT}
TASK: PRESENTATION / PACING ANALYSIS (read-only for src\\; experiments only as separate scratch programs or runs under run\\analyse_pacing\\). Analyse the current frame pacer (platform\\frame_pacer.*, gfx DisplayFrame/Present path, [render] vsync/limit settings) and design display-coupled pacing for a 60 Hz logic tick:
- query the display refresh rate of the output the window is on (DXGI output mode / DwmGetCompositionTimingInfo / QueryDisplayConfig; handle fractional rates like 59.94/119.88), react to the window moving to another monitor and to refresh changes;
- policy: if refresh is an integer multiple of the tick rate (60, 120, 180, 240) -> flip-model Present with SyncInterval = refresh/tick (or SyncInterval 1 with a pacer that skips vblanks); otherwise (144, 165, 75 ...) -> if DXGI_FEATURE_PRESENT_ALLOW_TEARING is supported (VRR/G-Sync capable path) present with SyncInterval 0 + ALLOW_TEARING paced by a high-resolution waitable timer at the tick rate; else SyncInterval 1 at the display rate (presented fps >= 60, duplicates) - evaluate trade-offs; use a frame-latency waitable swapchain (MaxFrameLatency 1) for low latency;
- what to measure: presented Hz, frame-time histogram, vblank alignment (DXGI frame statistics), schedule resets;
- effect of a switched-off monitor or locked session on these measurements (document what you can determine);
- a design for a later >60 Hz mode with render interpolation (what would have to be decompiled: Game::Frame 0xA2C20, transforms) - only as an outlook.
Write analysis\\loader\\pacing60.md (German) with concrete API-level design and test plan.`,
    { label: 'analyse:pacing', phase: 'Analyse', schema: ANALYSE_SCHEMA }),
])

const timingA = analyses[0]
const pacingA = analyses[1]
const anaDigest = analyses.filter(Boolean).map((a) => `${a.report_path}: ${a.summary}\nEmpfehlungen: ${a.recommendations.join(' | ')}\nRisiken: ${a.risks.join(' | ')}`).join('\n\n')

const MOD_SCHEMA = {
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

const RULES = `
RULES: two agents work IN PARALLEL with disjoint files; shared files (CMakeLists.txt, build.cmd, core\\config.*, core\\guest.h, src\\INTERFACES.md, docs\\*) only ADDITIVE changes, logged in the INTERFACES.md changelog. Build only in your own build dir build\\m3b-<module>. Analysis results:
${anaDigest}`

phase('Implement')
const impls = await parallel([
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE TIMING AGENT. You own src\\host\\xapi\\game_timing.* (new) and tests\\timing_*.cpp, plus additive changes to game_probes.* if needed. Implement config [game] tick_rate = 60 | 50 (default 60; validated choice, invalid -> exit 1) that patches the PAL branch of 0xA26A0 to the NTSC timing values (fps, dt, [0x2E7B68] and the derived globals - exactly per analysis\\loader\\timing60.md), keeping CApp+0xE8 = 0 and the "PAL" tag; verify original bytes; log the active timing once ("game timing: 60 Hz (NTSC timing values, PAL content)"); expose the effective dt/fps read back from guest memory after 0xA26A0 for the checks (log line). Handle anything else the timing analysis found necessary (e.g. frame-count constants). Unit tests for the patch table and config.`,
    { label: 'impl:timing', phase: 'Implement', schema: MOD_SCHEMA }),
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE PACING AGENT. You own src\\host\\platform\\frame_pacer.*, the present path in src\\host\\gfx\\rhi\\d3d11\\** and the DisplayFrame pacing call in gfx (coordinate minimal edits in gfx\\nipc_gpu.cpp), tests\\pacer_*.cpp. Implement display-coupled pacing per analysis\\loader\\pacing60.md: refresh detection per output (incl. fractional rates and monitor changes), tick rate from [game] tick_rate (read the config key the timing agent adds; default 60), policy integer-multiple -> SyncInterval n, otherwise tearing/VRR path with high-resolution waitable timer if supported, else vsync at display rate; frame-latency waitable swapchain; [render] pacing = auto | vsync | timer | off override; statistics at exit (presented Hz, histogram, schedule resets, vblank alignment from DXGI frame statistics). Remove the 50 Hz default. Unit tests for the policy selection and timer math (headless).`,
    { label: 'impl:pacing', phase: 'Implement', schema: MOD_SCHEMA }),
])
const implDigest = impls.filter(Boolean).map((m) => `[${m.module}] compiles=${m.compiles}; ${m.implemented.join(', ')}; shared: ${m.shared_changes.join(' | ')}; open: ${m.open_points.join(' | ')}`).join('\n')

const VERIFY_SCHEMA = {
  type: 'object',
  properties: {
    checks: {
      type: 'array',
      items: {
        type: 'object',
        properties: { check: { type: 'string' }, status: { type: 'string', enum: ['bestanden', 'teilweise', 'gescheitert'] }, evidence: { type: 'string' } },
        required: ['check', 'status', 'evidence'],
      },
    },
    duration_comparison: { type: 'string', description: 'table 50 Hz vs 60 Hz of the observables in seconds' },
    open_problems: { type: 'array', items: { type: 'string' } },
  },
  required: ['checks', 'duration_comparison', 'open_problems'],
}

phase('Verify')
const verify = await agent(`${CONTEXT}
PHASE 3: INTEGRATION + VERIFICATION. You own all files now. Implementation results:
${implDigest}
1. Build Debug, RelWithDebInfo, clang without warnings; fut_tests green.
2. Update tools\\m3_check.py: default --min-hz 59.5 with tick rate 60, report the pacing mode/policy, refresh rate and presented-frame statistics; add a timing check (log shows 60 Hz timing, read-back dt = 1/60, fps 60, CApp+0xE8 = 0, PAL archives/language unchanged). Keep m1/m2 checks passing (m2_check must accept 60 Hz timing; its 3000-frame criterion now covers 50 s of game time - adjust thresholds that assumed 50 Hz, with justification).
3. Run m3_check (3 runs, fresh) at tick_rate 60 on this machine; all criteria must pass incl. >= 60 Hz presented and the xemu image comparisons.
4. Duration comparison: run tick_rate 50 and tick_rate 60 and measure in seconds the observables listed in analysis\\loader\\timing60.md (legal screen, tech notice, loading, frontend idle until attract, attract cycle, any timed fades); differences must be explained (expected equal in seconds). Also compare against the xemu timeline (run\\xemu_ref\\scenes.tsv, xemu runs PAL 50 Hz).
5. Test the pacing policies via [render] pacing overrides (auto/vsync/timer/off) and report their statistics.
Fix problems at the root; record deviations in docs\\loader_design.md section 8 (German). Return the structured status.`,
  { label: 'verify', phase: 'Verify', schema: VERIFY_SCHEMA })

const verifyDigest = verify ? `checks: ${verify.checks.map((c) => `${c.check}: ${c.status} (${c.evidence})`).join(' | ')}; durations: ${verify.duration_comparison}; open: ${verify.open_problems.join(' | ')}` : 'verify agent failed'

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
REVIEW A - GAME TIMING CORRECTNESS (read-only for src\\; scratch under run\\review_m3b_timing\\). Verification status: ${verifyDigest}
Try to refute that 60 Hz keeps gameplay timing correct: independently search the binary for tick-count-based logic the timing analysis may have missed (script VM wait opcodes, per-tick counters, hard-coded 50/0.02 constants, animation/particle/camera code, sound/movie timing), check the patch (exact bytes, all globals, tag kept) and the 50-vs-60 duration measurements (re-measure at least two observables yourself). Report only issues with concrete evidence.`,
    { label: 'review:timing', phase: 'Review', schema: FINDINGS_SCHEMA }),
  () => agent(`${CONTEXT}
REVIEW B - PACING AND PRESENTATION (do NOT edit src\\; own build dir build\\review-m3b, user data run\\review_m3b_pacing\\). Verification status: ${verifyDigest}
Clean build, run m3_check (several runs) and examine the pacer: correct refresh detection (incl. DPI and fractional rates), policy choice on this 120 Hz display, SyncInterval/tearing/timer paths, latency (frame-latency waitable), behaviour on window move/minimize/restore, frame-time stability (histograms), CPU usage of the timer path (no busy-wait burning a core), and what happens with --set=render.pacing=... overrides. Report deviations with commands and outputs.`,
    { label: 'review:pacing', phase: 'Review', schema: FINDINGS_SCHEMA }),
])
const allFindings = reviews.filter(Boolean).flatMap((r) => r.findings)
log(`Review: ${allFindings.length} Befunde`)

phase('Fix')
const final = await agent(`${CONTEXT}
PHASE 5: FIX AND REPORT. You own all files. Verification status: ${verifyDigest}
Review findings (verify each before changing anything; reject with reason if it does not hold):
${allFindings.map((f, i) => `${i + 1}. [${f.severity}] ${f.file}: ${f.problem} | evidence: ${f.evidence} | fix: ${f.suggested_fix}`).join('\n')}
Fix confirmed findings; rebuild Debug, RelWithDebInfo, clang (no warnings); rerun fut_tests, m1/m2/m3 checks (m3 3 runs at 60 Hz). Update docs\\loader_design.md (German): section 1.9 Zeitbasis (60 Hz default via NTSC timing values with PAL content, [game] tick_rate), risk R9 (resolved for 60 Hz; >60 Hz needs interpolation), M3 exit criterion 3 (>= 60 fps), add a later milestone item "Hohe Bildraten (>60 Hz) mit Interpolation" with prerequisites, and deviations. Write docs\\m3b_60hz_report.md (German): what changed, how to configure (tick_rate, pacing), evidence (timing read-back, 50 vs 60 durations, pacing statistics on the 120 Hz display), accepted/rejected findings, known issues, outlook. Update m3_report.md known issues and README "Loader" section briefly. Do NOT commit. Return a short German summary (max 10 sentences).`,
  { label: 'fix+report', phase: 'Fix' })

return { analyses: analyses.map((a) => a && a.report_path), impl: impls.map((m) => m && { module: m.module, compiles: m.compiles }), verify: verify && verify.checks.map((c) => `${c.check}: ${c.status}`), findings: allFindings.length, final }
