export const meta = {
  name: 'futurama-m3d-120hz',
  description: 'Milestone M3d: up to 120 fps via renderer-level interpolation between 60 Hz logic ticks (camera, rigid objects, skinning, particles), half-tick frame clock and 120 Hz pacing; analysis, implementation, verification, reviews, report',
  phases: [
    { title: 'Analyse', detail: 'per-frame code audit and interpolation design' },
    { title: 'Implement', detail: 'frame clock/pacing/options and renderer interpolation' },
    { title: 'Verify', detail: 'm3d_check, regressions, motion sequences' },
    { title: 'Review', detail: 'interpolation correctness and timing/pacing/latency' },
    { title: 'Fix', detail: 'findings, docs, report' },
  ],
}

const CONTEXT = `
PROJECT: D:\\FuturamaDecomp (git repo, branch main, remote origin = private GitHub repo; do NOT commit or push - the user does that on request). Native PC port of the Xbox game "Futurama" (retail EU XBE) via a loader: futurama.exe (x86 stub at 0x10000) + futurama_host.dll (HLE kernel, XDK replacements, NiPCRenderer on D3D11 with native resolution/MSAA/SSAA, display-coupled pacer) + Qt launcher (launcher\\, 64-bit, schema-driven via futurama.exe --print-config-schema).
STATE: M1-M3c done and verified. Logic runs at 60 Hz (NTSC timing values with PAL content, [game] tick_rate), presentation at 60.01 Hz (every 2nd vblank on the user's 120 Hz LG TV with VRR), frame clock "lockstep" (one tick per CApp::Frame). Read: docs\\ROADMAP.md (section "M3d - Bildraten bis 120 Hz (Interpolation)" = your scope), docs\\m3b_60hz_report.md, docs\\m3c_report.md (incl. deferred finding 6: present-to-visible latency), docs\\loader_design.md (1.9, 2.2, 8.x), src\\INTERFACES.md, src\\host\\gfx\\GFX_CONTRACT.md, analysis\\loader\\startup.md 3.1-3.4 (CApp::Frame 0xD2630, Game::Frame 0xA2C20 pseudocode: fixed step loop, leftover elapsed, last at app+0xC8, render once after the ticks, g_frameDelta [0x2E7B88], per-frame helpers FUN_000d2370/FUN_000f2050/FUN_000f2680/FUN_000a46d0/FUN_0002c9c0/FUN_000d7630), analysis\\loader\\timing60.md (tick-bound behaviours), analysis\\loader\\pacing60.md.
USER REQUIREMENTS (binding): frame rate at least 60 fps, higher refresh rates welcome up to MAX 120 Hz. Logic must stay at 60 Hz (tick-bound behaviours would run too fast). Rendering up to 120 fps with interpolation between the last two logic states. 60 Hz displays keep 60 fps (no interpolation needed). Option in the launcher: frame rate "60", "120 (interpoliert)", "Monitor (max. 120)".
EVIDENCE: Ghidra snapshot (never open Ghidra): D:\\FuturamaDecomp\\analysis\\retail_eu_export\\decomp\\*.c, functions.tsv, callgraph.tsv, strings.tsv; Python + capstone; tools\\xbe.py; renderer.md + nixboxrenderer_vtable.tsv (slot semantics: RenderShape(NiTriShapeData*, NiSkinInstance*, NiTransform*, NiBound*) etc.). Motion-rich test material: the frontend (animated background) and the attract mode (gameplay demos, starts after ~65 s idle in the frontend; xemu reference timeline in run\\xemu_ref\\scenes.tsv).
TOOLCHAIN: game: cmd /c "D:\\FuturamaDecomp\\build.cmd" <builddir> <Debug|RelWithDebInfo> (FULL path; clang via ... clang); launcher: cmd /c "D:\\FuturamaDecomp\\build_launcher.cmd" <builddir> <Debug|Release> [test]; packaging tools\\package.py. Checks: tools\\m1_check.py, m2_check.py, m3_check.py, m3c_check.py (commands in m3b/m3c reports). futurama.exe is a GUI exe. Runs/screenshots ONLY under D:\\FuturamaDecomp\\run\\<your-name>\\ (git-ignored); game screenshots never go into the repo. ISO (read-only) via INI [paths] iso = Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso.
CONVENTIONS: INTERFACES.md / design 5.3 (English code+comments, German docs, noexcept guest entries, no inline asm, zero warnings incl. FUT_WERROR/LAUNCHER_WERROR, generated files --check, every new config key in the single config schema with de/en labels so the launcher picks it up).
`

const ANA_SCHEMA = {
  type: 'object',
  properties: { report_path: { type: 'string' }, summary: { type: 'string' }, recommendations: { type: 'array', items: { type: 'string' } }, risks: { type: 'array', items: { type: 'string' } } },
  required: ['report_path', 'summary', 'recommendations', 'risks'],
}

phase('Analyse')
const anas = await parallel([
  () => agent(`${CONTEXT}
TASK A - PER-FRAME CODE AUDIT AND FRAME CLOCK (read-only for src\\/launcher\\; scripts analysis\\loader\\scripts\\m3d_*.py; experiments under run\\analyse_m3d_clock\\).
1. Everything that runs once per CApp::Frame / Game::Frame outside the tick loop (and per DisplayFrame): list it, find what it does and whether running it 2x per tick (120 fps) changes behaviour (call-count-based maintenance like FUN_000a46d0, g_frameDelta [0x2E7B88] readers, UI/menu code driven per frame, the 8x ClearBuffer+DisplayFrame in context switches FUN_0002a660, movie path FUN_000ee080, loading screens, fps counter app+0xD0..0xE0, input polling placement (app->vf10 is inside the tick loop?), sound DoWork). Verify by measurement where possible (run with frame clock at 2 frames per tick and log).
2. How the current lockstep frame clock works (M3b) and the exact design for a half-tick clock: QPC values seen by FUN_000d2b90 so that every second CApp::Frame runs 0 ticks and the leftover (elapsed) is exactly dt/2; how to compute alpha robustly at render time (from app+0xC8 'last', the QPC returned for this frame, dt [0x2D6748]) and its range/edge cases (catch-up frames with several ticks, stalls, context switches, loading hitches, the 6*dt debt cap).
3. Pacing for up to 120 Hz: refresh 120 -> every vblank; 144/165/240 -> 120 via timer/VRR or vblank pattern (evaluate judder), 60 -> stay 60 without interpolation; frame-rate option semantics "60 | 120 | display (max 120)"; latency improvements (deferred M3c finding 6).
Write analysis\\loader\\m3d_frameclock.md (German, evidence per claim, concrete recommendations).`,
    { label: 'analyse:frameclock', phase: 'Analyse', schema: ANA_SCHEMA }),
  () => agent(`${CONTEXT}
TASK B - INTERPOLATION DESIGN (read-only for src\\/launcher\\; scripts analysis\\loader\\scripts\\m3d_interp_*.py; experiments under run\\analyse_m3d_interp\\).
Design renderer-level interpolation between the previous and the current logic state:
1. What each draw slot receives (NiTransform* pointing into which object? camera in SetCameraData 47, RenderShape/Triangles/Strip/Strips 48-51, particles, lines, skinned shapes with NiSkinInstance bones, billboards/NiBillboardNode, NiSwitchNode/LOD changes, screen polygons/textures 54/55) and how to key a stable per-object history (guest object address + generation; handling objects created/destroyed/reused between ticks, pooled particles).
2. Interpolation math: translation lerp, rotation slerp from NetImmerse rotation matrices (orthonormalisation, scale), uniform scale lerp; camera frustum/viewport; when to snap (first sight, large translation relative to bound radius, rotation > threshold, context switch, teleports, cuts in scripted cameras); skinning: interpolate bone world transforms before skinning (where does the current CPU skinning happen - FUN_0014f710 via the original or a host replica? can it consume interpolated bones?); particles: per-particle position interpolation when counts/indices are stable, otherwise snap; texture/UV and vertex-colour animations (leave at tick rate).
3. Culling: the game culls at tick state; consequences at alpha 0.5 (pop-in at frustum edges) and mitigation (cheap: none / small guard band).
4. Memory/CPU cost per frame (number of draws per frame in the frontend ~345 and attract mode) and data structures (open addressing hash map keyed by guest pointer, double-buffered snapshots).
Write analysis\\loader\\m3d_interpolation.md (German, evidence per claim, concrete design with interfaces).`,
    { label: 'analyse:interp', phase: 'Analyse', schema: ANA_SCHEMA }),
])
const anaDigest = anas.filter(Boolean).map((a) => `${a.report_path}: ${a.summary}\nEmpfehlungen: ${a.recommendations.join(' | ')}\nRisiken: ${a.risks.join(' | ')}`).join('\n\n')

const MOD_SCHEMA = {
  type: 'object',
  properties: { module: { type: 'string' }, compiles: { type: 'boolean' }, implemented: { type: 'array', items: { type: 'string' } }, shared_changes: { type: 'array', items: { type: 'string' } }, open_points: { type: 'array', items: { type: 'string' } }, summary: { type: 'string' } },
  required: ['module', 'compiles', 'implemented', 'shared_changes', 'open_points', 'summary'],
}
const RULES = `
RULES: two agents IN PARALLEL with disjoint files; shared files (CMakeLists.txt, build*.cmd, src\\shared\\**, core\\config*.*, core\\config_schema.*, core\\guest.h, src\\INTERFACES.md, docs\\*) only ADDITIVE changes with a changelog entry in src\\INTERFACES.md. Own build dirs build\\m3d-<module>*. Analysis results:
${anaDigest}`

phase('Implement')
const impls = await parallel([
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE FRAME-CLOCK/PACING AGENT. You own src\\host\\platform\\frame_pacer.*, pacer_*.*, the frame clock (wherever the M3b lockstep clock lives - xapi\\game_timing.* / QPC hooks), launcher\\** changes for the frame-rate option and the M3c launcher UX nit (disable/grey out "Fenstergroesse" while borderless fullscreen is active), tests\\pacer_*.cpp, tests\\frameclock_*.cpp. Implement per analysis\\loader\\m3d_frameclock.md: [render] frame_rate = 60 | 120 | display (display = refresh capped at 120; schema de/en labels "60 fps", "120 fps (interpoliert)", "Bildschirm (max. 120)"), half-tick frame clock giving deterministic alpha 0.5 on intermediate frames at 120 fps, 60 fps behaviour unchanged, alpha published to the renderer through a small interface (e.g. gfx::FrameInterp { tickGeneration, alpha, snap reason }) read from guest state at render time, pacing for 120/144/165/240/60 Hz displays, latency improvements; fixes for any per-frame code that misbehaves at 2 frames per tick (from the audit). Unit tests.`,
    { label: 'impl:frameclock', phase: 'Implement', schema: MOD_SCHEMA }),
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE INTERPOLATION AGENT. You own src\\host\\gfx\\interp_*.* (new), and the minimal hooks in gfx\\nipc_renderer.cpp / gfx\\nipc_gpu.cpp / gfx\\ni_extract.* needed to apply interpolated transforms, tests\\interp_*.cpp. Implement per analysis\\loader\\m3d_interpolation.md, consuming the FrameInterp interface (tick generation + alpha; until the frame-clock agent lands, use a stub returning alpha 1 / no interpolation): per-object transform history keyed by guest pointer with generation tracking, camera interpolation (SetCameraData), rigid objects (all geometry draw slots), skinning (interpolated bone transforms), particles (stable index interpolation, else snap), snapping rules, statistics (interpolated/snapped/first-sight counts per frame, logged at exit), [render] interpolation = on|off (schema de/en) and a debug overlay/log mode to visualise snaps. Exactly zero visual change at 60 fps (alpha always 1). Unit tests for math (slerp from rotation matrices, snapping) and history bookkeeping.`,
    { label: 'impl:interp', phase: 'Implement', schema: MOD_SCHEMA }),
])
const implDigest = impls.filter(Boolean).map((m) => `[${m.module}] compiles=${m.compiles}; ${m.implemented.join(', ')}; shared: ${m.shared_changes.join(' | ')}; open: ${m.open_points.join(' | ')}`).join('\n')

const VER_SCHEMA = {
  type: 'object',
  properties: {
    checks: { type: 'array', items: { type: 'object', properties: { check: { type: 'string' }, status: { type: 'string', enum: ['bestanden', 'teilweise', 'gescheitert'] }, evidence: { type: 'string' } }, required: ['check', 'status', 'evidence'] } },
    screenshots: { type: 'array', items: { type: 'string' } },
    open_problems: { type: 'array', items: { type: 'string' } },
  },
  required: ['checks', 'screenshots', 'open_problems'],
}

phase('Verify')
const ver = await agent(`${CONTEXT}
PHASE 3: INTEGRATION + VERIFICATION. You own all files. Implementation results:
${implDigest}
1. Integrate (real FrameInterp instead of the stub), build game (Debug, RelWithDebInfo, clang) and launcher (Debug, Release) without warnings; all unit tests green.
2. Write tools\\m3d_check.py: at frame_rate 120 on this 120 Hz display: presented ~120 Hz with >= 99 % of frames exactly one refresh, logic still 60.0 ticks/s (M3b-6 equivalent), alpha sequence 0/0.5 (log), interpolation statistics (most draws interpolated during motion, snaps explained), no debug-layer errors, all M2/M3 criteria; motion validation: capture consecutive frames in the attract mode (gameplay camera motion) and verify intermediate frames lie between their neighbours (e.g. via logged camera/object transforms: interpolated = midpoint within tolerance; and an image metric on screenshot triplets); at frame_rate 60: bit-identical behaviour to before (m3_check classic xemu comparison, m3c_check).
3. Run m1/m2/m3/m3c/m3d checks; render launcher screenshots (frame-rate option, greyed window size in fullscreen) and VIEW them; view game frame sequences at 120 fps and judge smoothness/artefacts (ghosting, popping, wrong interpolation of UI/cuts).
Fix root causes; deviations into docs\\loader_design.md section 8 (German). Return the structured status.`,
  { label: 'verify', phase: 'Verify', schema: VER_SCHEMA })
const verDigest = ver ? `checks: ${ver.checks.map((c) => `${c.check}: ${c.status} (${c.evidence})`).join(' | ')}; screenshots: ${ver.screenshots.join(', ')}; open: ${ver.open_problems.join(' | ')}` : 'verify agent failed'

const FIND_SCHEMA = {
  type: 'object',
  properties: { findings: { type: 'array', items: { type: 'object', properties: { file: { type: 'string' }, severity: { type: 'string', enum: ['kritisch', 'hoch', 'mittel', 'niedrig'] }, problem: { type: 'string' }, evidence: { type: 'string' }, suggested_fix: { type: 'string' } }, required: ['file', 'severity', 'problem', 'evidence', 'suggested_fix'] } }, summary: { type: 'string' } },
  required: ['findings', 'summary'],
}

phase('Review')
const reviews = await parallel([
  () => agent(`${CONTEXT}
REVIEW A - INTERPOLATION CORRECTNESS AND LOOK (do NOT edit src\\/launcher\\; own build dir build\\review-m3d-interp; runs under run\\review_m3d_interp\\). Verification status: ${verDigest}
Try to break the interpolation: frame sequences at 120 fps in frontend and attract mode (camera cuts, scripted cameras, characters with skinning, particles, objects spawning/despawning, LOD switches, billboards, UI and 2D elements, loading screens, context switches), slerp correctness for mirrored/scaled matrices, history keying bugs (pointer reuse), memory growth over long runs, CPU cost per frame. View screenshot triplets and judge like a player who knows 120 Hz games. Report findings with evidence.`,
    { label: 'review:interp', phase: 'Review', schema: FIND_SCHEMA }),
  () => agent(`${CONTEXT}
REVIEW B - TIMING, PACING, LATENCY (do NOT edit src\\/launcher\\; own build dirs build\\review-m3d-timing*; runs under run\\review_m3d_timing\\). Verification status: ${verDigest}
Verify that game logic stays exactly 60 Hz at 120 fps (ticks/s, scripted durations, per-frame code audit claims re-measured), alpha correctness at hitches/catch-up/stalls, pacing on this 120 Hz VRR display (every vblank, frame-time histogram, foreground/background behaviour), the 'display' option logic for other refresh rates (simulate via unit tests/overrides), latency measurements before/after, 60 fps mode unchanged, launcher option and greyed-out window size. Report findings with evidence.`,
    { label: 'review:timing', phase: 'Review', schema: FIND_SCHEMA }),
])
const allFindings = reviews.filter(Boolean).flatMap((r) => r.findings)
log(`Review: ${allFindings.length} Befunde`)

phase('Fix')
const final = await agent(`${CONTEXT}
PHASE 5: FIX AND REPORT. You own all files. Verification status: ${verDigest}
Review findings (verify each first; reject with reason if it does not hold):
${allFindings.map((f, i) => `${i + 1}. [${f.severity}] ${f.file}: ${f.problem} | evidence: ${f.evidence} | fix: ${f.suggested_fix}`).join('\n')}
Fix confirmed findings; rebuild all (game Debug/RelWithDebInfo/clang, launcher Debug/Release; no warnings); rerun unit tests, launcher tests, m1/m2/m3/m3c/m3d checks; package dist\\FuturamaPC; final frame sequences under run\\m3d_final\\ (view them). Write docs\\m3d_report.md (German): design (frame clock, alpha, interpolation stages, snapping), how to use (launcher option, config keys), measurements (presented Hz, logic ticks/s, interpolation statistics, CPU/GPU cost, latency), accepted/rejected findings, known issues, next steps (M4 input with launcher controls tab, M4b freecam). Update docs\\ROADMAP.md (M3d erledigt, M4 als Naechstes), README.md, INTERFACES.md, GFX_CONTRACT.md. Do NOT commit. Return a short German summary (max 12 sentences).`,
  { label: 'fix+report', phase: 'Fix' })

return { analyses: anas.map((a) => a && a.report_path), impl: impls.map((m) => m && { module: m.module, compiles: m.compiles }), verify: ver && ver.checks.map((c) => `${c.check}: ${c.status}`), findings: allFindings.length, final }
