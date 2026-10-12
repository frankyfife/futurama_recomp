export const meta = {
  name: 'futurama-m4-input',
  description: 'Milestone M4: input (XPP/XInput HLE, XInput + keyboard/mouse backends, bindings, launcher controls tab, scripted input for automated in-game tests); analysis, implementation, integration, reviews, report',
  phases: [
    { title: 'Analyse', detail: 'game input path and new-game/level start path' },
    { title: 'Implement', detail: 'input HLE + backends, launcher controls tab, input scripts + checks' },
    { title: 'Integrate', detail: 'menu navigation, first automated in-game run' },
    { title: 'Review', detail: 'input correctness and UX/robustness' },
    { title: 'Fix', detail: 'findings, docs, report' },
  ],
}

const CONTEXT = `
PROJECT: D:\\FuturamaDecomp (git repo, branch main, remote origin = private GitHub repo; do NOT commit or push - the user does that on request). Native PC port of the Xbox game "Futurama" (retail EU XBE) via a loader: futurama.exe (x86 stub at 0x10000) + futurama_host.dll (HLE kernel, XDK replacements, NiPCRenderer on D3D11 with native resolution/MSAA/SSAA, 60 Hz logic, display-coupled pacing, interpolation up to 120 fps) + Qt launcher (launcher\\, 64-bit, schema-driven via futurama.exe --print-config-schema).
STATE: M1-M3d done (reports docs\\m1_report.md .. m3d_report.md). The game boots to the frontend menu and renders it; XPP (XInput) entry points are still stubs reporting no devices (docs\\m2_report.md section 5: 7 XPP entries 0x28CFE1, 0x28CFE6, 0x28D008, 0x28CE5C, 0x28CEB2, 0x28CEBE, 0x28CF2A; generated hooks from analysis\\loader\\avi_api_surface.tsv). Read: docs\\ROADMAP.md (M4 = your scope: XInput, keyboard and mouse with freely assignable keys, rumble, controls tab in the launcher, plus scripted input for automated test runs that enter a level), analysis\\loader\\avi.md section on input (4 ports with hotplug, 16 logical channels with threshold 30/256, sticks with 0.4, rumble via XInputSetState which must set dwStatus synchronously, no MU/headset/capabilities), docs\\loader_design.md (E7, 5.x conventions, section 8 deviations), src\\INTERFACES.md, docs\\m3c_report.md (launcher + config schema contract), docs\\m3d_report.md.
USER REQUIREMENTS: freely assignable controls (controller and keyboard/mouse), rumble, a controls tab in the Qt launcher; later a freecam (M4b) will use this input layer (keep it extensible: an input router that can divert devices away from the game). Scripted input ([debug] input_script) so automated runs can navigate the menu and start a new game into the first level.
EVIDENCE: Ghidra snapshot (never open Ghidra): D:\\FuturamaDecomp\\analysis\\retail_eu_export\\decomp\\*.c, functions.tsv, callgraph.tsv, strings.tsv; Python + capstone; tools\\xbe.py. Level scripts and data names in orig\\retail_eu\\*_files.txt.
TOOLCHAIN: game: cmd /c "D:\\FuturamaDecomp\\build.cmd" <builddir> <Debug|RelWithDebInfo> (FULL path; clang via ... clang); launcher: cmd /c "D:\\FuturamaDecomp\\build_launcher.cmd" <builddir> <Debug|Release> [test]; tools\\package.py. Checks: tools\\m1_check.py, m2_check.py, m3_check.py, m3c_check.py, m3d_check.py (commands in the reports; the user's 120 Hz TV may be switched off: use --display-optional / documented flags where display-dependent criteria cannot be measured, and say so). Windows SDK has XInput 1.4. No physical controller can be assumed to be connected: test real backends where possible, otherwise via injected/simulated devices and unit tests, and state clearly what was not tested with real hardware. futurama.exe is a GUI exe. Runs/screenshots ONLY under D:\\FuturamaDecomp\\run\\<your-name>\\ (git-ignored); game screenshots never go into the repo. ISO (read-only) via INI [paths] iso = Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso. No Futurama artwork in the repo or launcher.
CONVENTIONS: INTERFACES.md / design 5.3 (English code+comments, German docs/UI texts with English alternatives, noexcept guest entries, no inline asm, zero warnings incl. FUT_WERROR/LAUNCHER_WERROR, generated files --check, every config key in the single config schema with de/en labels).
`

const ANA_SCHEMA = { type: 'object', properties: { report_path: { type: 'string' }, summary: { type: 'string' }, recommendations: { type: 'array', items: { type: 'string' } }, risks: { type: 'array', items: { type: 'string' } } }, required: ['report_path', 'summary', 'recommendations', 'risks'] }

phase('Analyse')
const ana = await agent(`${CONTEXT}
TASK: INPUT PATH ANALYSIS (read-only for src\\/launcher\\; scripts analysis\\loader\\scripts\\m4_*.py). Determine exactly:
1. The XPP API contract per entry point (arguments, structures XINPUT_STATE/XINPUT_GAMEPAD with 8 analog buttons + digital buttons + sticks, XINPUT_FEEDBACK/rumble with dwStatus/hEvent, device types/XDEVICE_PREALLOC_TYPE, XGetDevices bitmasks, XGetDeviceChanges insertions/removals, handles) as used by THIS game (callers in the decomp), so the HLE can emulate up to 4 gamepads with hotplug.
2. How the game turns gamepad state into its 16 logical channels and menu actions (threshold 30/256, stick 0.4), which channels map to which actions in menus and in gameplay (jump, attack, use, camera, start/pause ...), which port is the active player, and what the frontend needs to start a new game (button sequence from the main menu "Neues Spiel" through any profile/save prompts, difficulty, cutscene skipping) until the first level (level1-1) is loading and playable; how to detect each step in the log/state (context names, files opened, script names).
3. Rumble usage (when, intensities).
Write analysis\\loader\\m4_input.md (German, evidence per claim) including a concrete recommended default binding table (Xbox controller -> game, keyboard/mouse -> game) and a proposed input script that starts a new game.`,
  { label: 'analyse:input', phase: 'Analyse', schema: ANA_SCHEMA })
const anaDigest = ana ? `${ana.report_path}: ${ana.summary}\nEmpfehlungen: ${ana.recommendations.join(' | ')}\nRisiken: ${ana.risks.join(' | ')}` : 'analysis failed - do it yourself as needed'

const MOD_SCHEMA = { type: 'object', properties: { module: { type: 'string' }, compiles: { type: 'boolean' }, implemented: { type: 'array', items: { type: 'string' } }, shared_changes: { type: 'array', items: { type: 'string' } }, open_points: { type: 'array', items: { type: 'string' } }, summary: { type: 'string' } }, required: ['module', 'compiles', 'implemented', 'shared_changes', 'open_points', 'summary'] }
const RULES = `
RULES: three agents IN PARALLEL with disjoint files; shared files (CMakeLists.txt, build*.cmd, src\\shared\\**, core\\config*.*, core\\config_schema.*, core\\guest.h, src\\INTERFACES.md, docs\\*) only ADDITIVE changes with a changelog entry in src\\INTERFACES.md. Own build dirs build\\m4-<module>*. Shared interface (define it in src\\host\\input\\input_api.h if the input agent has not yet; first writer wins, others extend additively): a virtual pad state per port (buttons, 8 analog values 0-255, sticks -32768..32767), an InputSource interface (physical backends and the script player both produce pad states), an InputRouter that decides which sources feed the game (and can later divert to a freecam), rumble sink. Analysis:
${anaDigest}`

phase('Implement')
const impls = await parallel([
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE INPUT AGENT. You own src\\host\\input\\** (except input_script.*), the XPP stub replacements (switch the 7 XPP hooks from stubs to real HLE), tests\\input_*.cpp. Implement per analysis\\loader\\m4_input.md: XPP HLE (XInitDevices, XGetDevices, XGetDeviceChanges, XInputOpen/Close, XInputGetState with packet numbers, XInputSetState rumble with synchronous dwStatus), up to 4 ports with hotplug; backends: XInput 1.4 (all 4 controllers, deadzones, analog triggers -> analog buttons), keyboard + mouse via Raw Input on the window thread (mouse as right stick/camera with sensitivity, key-to-analog-button mapping), focus handling (no input when unfocused, release all on focus loss); bindings from config ([input] section, schema entries with de/en labels; a compact binding string format documented in INTERFACES.md), defaults from the analysis; rumble to XInput (config toggle + strength). Unit tests with simulated devices.`,
    { label: 'impl:input', phase: 'Implement', schema: MOD_SCHEMA }),
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE LAUNCHER AGENT. You own launcher\\** changes. Add a "Steuerung" tab: per device (Controller / Tastatur und Maus) a binding table of the game actions from the schema (labels de/en), "press a key/button to assign" capture (keyboard and mouse buttons in Qt; controller buttons via XInput polling in the launcher, 64-bit is fine), conflict detection, reset to defaults, deadzone/sensitivity sliders, rumble on/off + strength, a live controller test view (shows connected pads and pressed buttons). Persist exactly in the binding string format of INTERFACES.md / schema. Qt tests for the binding editor round trip. Screenshots via --screenshots and VIEW them.`,
    { label: 'impl:launcher', phase: 'Implement', schema: MOD_SCHEMA }),
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE TEST-AUTOMATION AGENT. You own src\\host\\input\\input_script.*, tests\\input_script_*.cpp, tools\\m4_check.py, run\\m4-scripts\\ (sample scripts, git-ignored) and a checked-in example under tools\\input_scripts\\ (no game data). Implement [debug] input_script = <file>: a small text format (frame- or time-based events: press/release/hold buttons, set analog/stick values, wait until a log condition or context name, repeat), played as an InputSource on port 1; deterministic with the lockstep clock. Write the script that starts a new game from the main menu into level1-1 (per analysis) and tools\\m4_check.py: runs it, verifies the contexts reached (Frontend -> new game -> ... -> Ingame), the level files opened (level1-1 archive/scripts), N frames played in the level without crash, screenshots in the level (view them!), plus controller/keyboard criteria that can be checked headlessly; it must also report rendering gaps seen in the level (draw outcomes not drawn, unimplemented renderer slots) as information, not as failure.`,
    { label: 'impl:automation', phase: 'Implement', schema: MOD_SCHEMA }),
])
const implDigest = impls.filter(Boolean).map((m) => `[${m.module}] compiles=${m.compiles}; ${m.implemented.join(', ')}; shared: ${m.shared_changes.join(' | ')}; open: ${m.open_points.join(' | ')}`).join('\n')

const VER_SCHEMA = { type: 'object', properties: { checks: { type: 'array', items: { type: 'object', properties: { check: { type: 'string' }, status: { type: 'string', enum: ['bestanden', 'teilweise', 'gescheitert'] }, evidence: { type: 'string' } }, required: ['check', 'status', 'evidence'] } }, ingame: { type: 'string', description: 'how far the automated run got into the game, what is visible, what breaks' }, screenshots: { type: 'array', items: { type: 'string' } }, open_problems: { type: 'array', items: { type: 'string' } } }, required: ['checks', 'ingame', 'screenshots', 'open_problems'] }

phase('Integrate')
const ver = await agent(`${CONTEXT}
PHASE 3: INTEGRATION. You own all files. Implementation results:
${implDigest}
Integrate the three parts; build game (Debug, RelWithDebInfo, clang) and launcher (Debug, Release) without warnings; all unit tests green; regressions m1/m2/m3/m3c/m3d (display-optional where needed). Then drive the game with the input script into level1-1: fix whatever blocks the way (input HLE semantics, menu logic, save/profile prompts (M7 is not done - find a way that works without real saves, e.g. the game's own "continue without saving" path), loading, crashes in kernel/VFS/renderer). This is the first time the game leaves the frontend: expect missing renderer features (lights, texture effects, particles, fog...), missing XDK pieces, unimplemented kernel calls - fix crash blockers properly, log and list visual gaps for later milestones. View in-game screenshots. Record deviations in docs\\loader_design.md section 8 (German). Return structured status including how far in-game the run gets.`,
  { label: 'integrate', phase: 'Integrate', schema: VER_SCHEMA })
const verDigest = ver ? `checks: ${ver.checks.map((c) => `${c.check}: ${c.status} (${c.evidence})`).join(' | ')}; ingame: ${ver.ingame}; screenshots: ${ver.screenshots.join(', ')}; open: ${ver.open_problems.join(' | ')}` : 'integration failed'

const FIND_SCHEMA = { type: 'object', properties: { findings: { type: 'array', items: { type: 'object', properties: { file: { type: 'string' }, severity: { type: 'string', enum: ['kritisch', 'hoch', 'mittel', 'niedrig'] }, problem: { type: 'string' }, evidence: { type: 'string' }, suggested_fix: { type: 'string' } }, required: ['file', 'severity', 'problem', 'evidence', 'suggested_fix'] } }, summary: { type: 'string' } }, required: ['findings', 'summary'] }

phase('Review')
const reviews = await parallel([
  () => agent(`${CONTEXT}
REVIEW A - INPUT CORRECTNESS (do NOT edit src\\/launcher\\; own build dir build\\review-m4-input; runs under run\\review_m4_input\\). Status: ${verDigest}
Verify the XPP HLE against the game's callers (packet numbers, analog thresholds, hotplug sequence, rumble dwStatus), backend mapping (deadzones, trigger->analog, keyboard analog emulation, mouse camera feel), focus/alt-tab behaviour, multiple controllers, binding string parsing edge cases, input script determinism, the in-game run. Report findings with evidence.`,
    { label: 'review:input', phase: 'Review', schema: FIND_SCHEMA }),
  () => agent(`${CONTEXT}
REVIEW B - LAUNCHER CONTROLS UX AND ROBUSTNESS (do NOT edit; own build dirs build\\review-m4-launcher*; runs under run\\review_m4_launcher\\). Status: ${verDigest}
Render and VIEW the controls tab (de/en, dark/light, 225 % DPI); check rebinding flow, conflicts, reset, sliders, live test view without a controller, persisted INI vs what the game reads (round trip with the real futurama.exe), wording. Report findings with evidence.`,
    { label: 'review:launcher', phase: 'Review', schema: FIND_SCHEMA }),
])
const allFindings = reviews.filter(Boolean).flatMap((r) => r.findings)
log(`Review: ${allFindings.length} Befunde`)

phase('Fix')
const final = await agent(`${CONTEXT}
PHASE 5: FIX AND REPORT. You own all files. Status: ${verDigest}
Review findings (verify each first; reject with reason if it does not hold):
${allFindings.map((f, i) => `${i + 1}. [${f.severity}] ${f.file}: ${f.problem} | evidence: ${f.evidence} | fix: ${f.suggested_fix}`).join('\n')}
Fix confirmed findings; rebuild all (no warnings); rerun unit tests, launcher tests, m1..m3d checks (display-optional if the TV is off - say so) and m4_check; package dist\\FuturamaPC. Write docs\\m4_report.md (German): input architecture, default bindings, launcher controls tab, input scripts, how far the automated run gets in-game (with screenshot paths under run\\), list of in-game rendering/feature gaps for the next milestones, what was not tested with real hardware, accepted/rejected findings, next steps (M4b freecam on the InputRouter, M5 audio). Update docs\\ROADMAP.md (M4 erledigt, M4b als Naechstes), README.md, INTERFACES.md. Do NOT commit. Return a short German summary (max 12 sentences).`,
  { label: 'fix+report', phase: 'Fix' })

return { analysis: ana && ana.report_path, impl: impls.map((m) => m && { module: m.module, compiles: m.compiles }), integration: ver && { checks: ver.checks.map((c) => `${c.check}: ${c.status}`), ingame: ver.ingame }, findings: allFindings.length, final }
