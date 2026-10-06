export const meta = {
  name: 'futurama-m3c-launcher-aa',
  description: 'Milestone M3c: Qt launcher, native render resolution, MSAA + alpha-to-coverage, supersampling, anisotropic filtering, host config schema/ISO/GPU CLI; integration, reviews, fixes, report',
  phases: [
    { title: 'Build', detail: 'renderer AA/resolution, host CLI + schema, Qt launcher in parallel' },
    { title: 'Integrate', detail: 'packaging, launcher-driven runs, checks' },
    { title: 'Review', detail: 'renderer quality review and launcher UX/robustness review' },
    { title: 'Fix', detail: 'apply findings, report' },
  ],
}

const CONTEXT = `
PROJECT: D:\\FuturamaDecomp (git repo, branch main, remote origin = private GitHub repo; do NOT commit or push - the user does that on request). Native PC port of the Xbox game "Futurama" (retail EU XBE) via a loader: futurama.exe (x86 launcher stub fixed at 0x10000) + futurama_host.dll (HLE kernel, XDK replacements, NiPCRenderer with D3D11 backend, display-coupled pacer).
STATE: M1-M3b done and verified: the game boots, runs its main loop at 60 Hz logic (NTSC timing values with PAL content, [game] tick_rate) with display-coupled pacing (60.01 Hz; on the user's 120 Hz LG TV exactly every 2nd vblank) and renders legal/tech notice/loading/frontend pixel-accurately vs xemu at 640x480. Read: docs\\ROADMAP.md (section "M3c - Launcher (Qt) und Anzeige-Optionen" incl. the anti-aliasing bullet list = your scope), docs\\m3_report.md, docs\\m3b_60hz_report.md, docs\\loader_design.md (2.2, 5.x conventions, section 8 deviations), src\\INTERFACES.md, src\\host\\gfx\\GFX_CONTRACT.md.
USER REQUIREMENTS (binding): a small launcher with a Qt UI ("Goldstandard": resolution, ISO path, etc.); anti-aliasing must be implemented: MSAA or another sensible AA solution, explicitly NO FXAA (TAA also not wanted); native render resolution scaling is part of it; frame rate min 60 Hz, later up to max 120 Hz (M3d, not in this milestone - but the launcher may already show the frame-rate option as "60" only or prepared).
ARTWORK RULE (binding): do NOT add any Futurama artwork, logos, character images, game fonts or game screenshots to the repository or as launcher resources. The launcher uses an original, neutral design and an original icon (abstract/typographic, not resembling any Futurama character, ship or logo). Game screenshots for verification stay under run\\ (git-ignored) and are never copied into the repo.
MACHINE: Windows 11, RTX 5090, primary display LG TV 3840x2160 @ 120 Hz with VRR, 225 % DPI scaling. Qt 6.8.3 MSVC 2022 64-bit at D:\\Qt\\6.8.3\\msvc2022_64 (Widgets, Network, Test, offscreen platform plugin, modern windows style; NO LinguistTools/lrelease -> UI translations de/en via in-code tables). MSVC 14.44 (vcvars32 for the game, vcvars64 for the launcher), CMake, Ninja, Windows SDK 10.0.26100.
TOOLCHAIN: game: cmd /c "D:\\FuturamaDecomp\\build.cmd" <builddir> <Debug|RelWithDebInfo> (FULL path; x86-only project). futurama.exe is a GUI exe (PowerShell: Start-Process -Wait -PassThru). Checks: tools\\m1_check.py, m2_check.py, m3_check.py (commands in m3b_60hz_report.md; configs run\\m3b\\*). Test runs/screenshots ONLY under D:\\FuturamaDecomp\\run\\<your-name>\\. ISO (read-only): "Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso" (note the spaces, parentheses and commas - every path handling must cope with it).
CONVENTIONS: INTERFACES.md / design 5.3 (English code+comments, German docs, noexcept guest entries, no inline asm, zero warnings incl. FUT_WERROR, generated files with --check). Launcher code: C++20, Qt Widgets, English code/comments, UI texts de/en.

SHARED CONTRACT (all agents must follow exactly):
1. futurama.exe --print-config-schema=<file.json> : writes the schema and exits 0 without starting the game. JSON:
   {"schema_version":1,"build":"<string>","sections":[{"name":"render","label":{"de":"..","en":".."},"keys":[{"name":"msaa","type":"choice|bool|int|float|string|path_file|path_dir|resolution","default":<value>,"choices":[{"value":"off","label":{"de":"Aus","en":"Off"}}],"min":<num>,"max":<num>,"step":<num>,"label":{"de":"..","en":".."},"help":{"de":"..","en":".."},"advanced":false,"restart_required":true,"ui":{"tab":"game|graphics|paths|advanced","order":10,"widget":"combo|check|spin|dspin|text|file|dir|resolution|monitor"}}]}]}
   Every key the host accepts must be in the schema (single source of truth; the host's existing config validation must use the same definitions).
2. futurama.exe --check-iso=<path> --result=<file.json> : {"ok":bool,"reason":"..","xdvdfs":bool,"layout":"xiso|redump|unknown","xbe_sha1":"..","version":"retail_eu|proto_ntsc|unknown","supported":bool,"title":"..","size":<bytes>} exit 0 if supported.
3. futurama.exe --query-gpu=<file.json> : {"adapters":[{"name":"..","vendor_id":..,"dedicated_mb":..}],"outputs":[{"name":"..","device":"\\\\\\\\.\\\\DISPLAY1","primary":bool,"rect":[x,y,w,h],"refresh":{"num":..,"den":..},"modes":[[w,h]...]}],"msaa":[1,2,4,8],"max_anisotropy":16,"tearing":bool,"vrr_hint":".."}
4. Player configuration location: default %LOCALAPPDATA%\\FuturamaPC\\futurama.ini with userdata %LOCALAPPDATA%\\FuturamaPC\\ ; portable mode when a file "portable.txt" lies next to futurama.exe (then config and userdata next to the exe); --config=<file> always overrides. The launcher passes --config=<ini> explicitly.
5. New [render] keys (host side, owned by the renderer agent, registered with the schema agent's mechanism): resolution = auto | <W>x<H> (auto = fit the window/display; the 4:3 game image is rendered at the largest 4:3 size inside the output with pillar/letterbox), render_scale = 1.0..2.0 (supersampling factor on top, step 0.25), msaa = off|2|4|8 (clamped to device support with a log line), alpha_to_coverage = on|off (default on, only effective with MSAA), anisotropy = off|2|4|8|16 (default 16). The existing scaling (linear|point|integer) applies to the final upscale when the internal size is below the output. "Classic" = resolution 640x480, render_scale 1, msaa off, anisotropy off must reproduce the M3 images (xemu regression).
`

const BUILD_SCHEMA = {
  type: 'object',
  properties: {
    part: { type: 'string' },
    builds: { type: 'boolean' },
    implemented: { type: 'array', items: { type: 'string' } },
    shared_changes: { type: 'array', items: { type: 'string' } },
    open_points: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' },
  },
  required: ['part', 'builds', 'implemented', 'shared_changes', 'open_points', 'summary'],
}

const RULES = `
PHASE 1 RULES: three agents work IN PARALLEL on disjoint files. Shared files (CMakeLists.txt, build.cmd, src\\shared\\**, core\\config.*, core\\guest.h, src\\INTERFACES.md, docs\\*) only ADDITIVE changes, each noted in the changelog of src\\INTERFACES.md. Build only in your own build directories build\\m3c-<part>*. If a build breaks because of another agent's file mid-edit, wait and retry.`

phase('Build')
const parts = await parallel([
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE RENDERER AGENT. You own src\\host\\gfx\\** (except files the schema agent creates for the CLI), src\\host\\platform\\window.* and screenshot.*, tests\\aa_*.cpp, tools\\m3c_check.py (new).
Implement contract item 5: internal render resolution (color + depth targets at the internal size = 4:3 fit of the output x render_scale; viewport/scissor, pre-transformed screen-space vertices of slots 54/55 and the 1/16-pixel snapping scaled from 640x480 to the internal size; line/point sizes; render-to-texture (slot 15) keeps original texture sizes; MSAA color+depth targets with resolve; alpha-to-coverage for alpha-tested materials when MSAA is on (with alpha sharpening in the shader so edges stay crisp); supersampling downsample with a proper filter (exact box for integer factors, a high-quality filter for fractional factors) before the final scale/pillarbox to the back buffer; anisotropic sampling for mip-mapped trilinear/bilinear textures (not for UI/point-filtered ones); screenshots at internal and output resolution ([render] screenshot_size = internal|output). Query device MSAA support and clamp. Keep "classic" mode bit-exact to M3 (m3_check xemu comparisons must still pass with the classic config).
Write tools\\m3c_check.py: runs a config matrix (classic regression vs xemu; high quality: resolution auto on the 3840x2160 output -> 2880x2160 internal, msaa 4, a2c on, anisotropy 16, render_scale 1; supersampling render_scale 2 at a smaller output) and checks: exit 0, frame limit reached, presented rate >= 59.5 Hz with work p99 well below the period, D3D debug layer 0 errors, screenshots at the expected size, an edge-aliasing metric showing MSAA reduces stair-stepping vs no-AA at the same resolution (e.g. count of high-contrast 1-pixel stair steps along edges, or difference to a 4x supersampled reference you render), and that the downscaled high-res frontend still matches xemu within a tolerance.`,
    { label: 'build:renderer', phase: 'Build', schema: BUILD_SCHEMA }),
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE HOST-CLI / SCHEMA AGENT. You own src\\host\\core\\config_schema.* (new), src\\host\\core\\cli_tools.* (new), additive changes to core\\config.* and core\\host_main.* and the launcher stub src\\launcher\\launcher.c if needed for early CLI handling, tests\\schema_*.cpp, tests\\cli_*.cpp.
Implement contract items 1-4: a single schema definition table for ALL config keys (existing ones from INTERFACES.md/config.cpp plus the new [render] keys of item 5 - coordinate names exactly as in the contract; the renderer agent consumes them) with de/en labels and help texts written for players (German must be natural, e.g. "Kantenglaettung (MSAA)"), defaults, choices, ranges, ui hints and advanced flags; the existing validation must use this table (no duplicate definitions). --print-config-schema, --check-iso (reuse the VFS XDVDFS reader; detect retail EU by default.xbe SHA-1 58f76de5bc739bf016d74c1d7f23e348f65ea4b4 and the NTSC prototype by f443d255eab39584dcd5dcea6f7c057182c4b854; both plain xiso and redump layouts), --query-gpu (DXGI adapters/outputs with exact refresh fractions via QueryDisplayConfig, supported display modes, D3D11 MSAA levels for the formats the renderer uses, tearing support). These modes must not map the XBE or start the game and must work even when the 0x10000 placeholder logic runs (they run in the host DLL after LoadLibrary but before XBE mapping). Implement the player configuration location of item 4 (default/portable/--config) and document it. Unit tests for schema completeness (every accepted key has a schema entry and vice versa), JSON output, ISO detection on the real ISO (read-only) and on a fake file.`,
    { label: 'build:host-cli', phase: 'Build', schema: BUILD_SCHEMA }),
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE LAUNCHER AGENT. You own launcher\\** (new top-level directory, its own CMake project, 64-bit), build_launcher.cmd (new, root; args [builddir] [Debug|Release]; uses vcvars64 and CMAKE_PREFIX_PATH=D:\\Qt\\6.8.3\\msvc2022_64; runs windeployqt into the output dir), tools\\package.py (new), the .gitignore entry for dist\\ (additive).
Build "FuturamaLauncher.exe" with Qt 6 Widgets, following the contract (items 1-4; develop against a hand-written sample schema JSON in launcher\\testdata\\ until the host CLI exists, then the integration agent switches to the real output):
- On start: locate futurama.exe (same dir, or a configured game dir), call --print-config-schema and --query-gpu (cache results per build string), load/create the player INI (contract item 4).
- First-run wizard (QWizard): choose ISO (file dialog) -> --check-iso -> clear result text (supported / unsupported version / not an Xbox image); language of the game; display (monitor from query-gpu + QScreen), window or borderless fullscreen; quality preset (Klassisch 640x480 / Standard: native + MSAA 4x / Maximum: native + MSAA 8x + Supersampling 1.5x - clamp to what the GPU supports).
- Main window: header with an original neutral title (text, no game artwork) and an original icon (generate it procedurally or as a small original .ico you create - abstract/typographic only); tabs Spiel / Grafik / Pfade / Erweitert built from the schema (widgets per ui hints, tooltips from help texts, advanced keys only on Erweitert), resolution picker with the monitor's modes + "Automatisch (nativ)", "Auf Standard zuruecksetzen" per tab, Speichern, big "Spielen" button. "Spielen" saves, starts futurama.exe --config=<ini>, hides the launcher and shows it again when the game exits with a status panel: exit code meaning (0 ok, 1 configuration error, 2 startup stage failed, 3 fatal error, 4 game ended, 5 watchdog), last 30 log lines, buttons Log oeffnen / Ordner oeffnen / Erneut versuchen.
- UI language de/en (follows system, switchable), in-code translation tables. Style: Qt modern windows style following system dark/light. Correct high-DPI behaviour (the user runs 225 %).
- INI handling: read/write preserving unknown keys, comments and key order; atomic write; validate against the schema (choices, ranges) with inline error hints.
- Paths with spaces, parentheses and commas everywhere (the user's ISO path has all three).
- Automation for tests: FuturamaLauncher.exe --screenshots=<dir> (renders every tab and wizard page to PNG using the offscreen platform if requested, e.g. with -platform offscreen), --set section.key=value (repeatable) --save, --launch --wait --result=<file.json>.
- tools\\package.py: assembles dist\\FuturamaPC\\ from a game build dir (futurama.exe, futurama_host.dll) and the launcher build (FuturamaLauncher.exe + windeployqt output), plus a short German LIESMICH.txt for players (no game data!).
- Qt Test unit tests for INI round trip, schema parsing, validation, path handling. Build Debug and Release without warnings (/W4).`,
    { label: 'build:launcher', phase: 'Build', schema: BUILD_SCHEMA }),
])

const partDigest = parts.filter(Boolean).map((p) => `[${p.part}] builds=${p.builds}; ${p.implemented.join(', ')}; shared: ${p.shared_changes.join(' | ')}; open: ${p.open_points.join(' | ')}`).join('\n')

const STATUS_SCHEMA = {
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
    screenshots: { type: 'array', items: { type: 'string' } },
    open_problems: { type: 'array', items: { type: 'string' } },
    how_to_use: { type: 'string' },
  },
  required: ['checks', 'screenshots', 'open_problems', 'how_to_use'],
}

phase('Integrate')
const integ = await agent(`${CONTEXT}
PHASE 2: INTEGRATION. Part results:
${partDigest}
You own all files now. Make the three parts work together: launcher reads the REAL schema/ISO/GPU output; schema covers every key (incl. the renderer's); build game (Debug, RelWithDebInfo, clang) and launcher (Debug, Release) without warnings; run tools\\package.py into dist\\FuturamaPC\\ (git-ignored) and start the game from there through the launcher automation (--set ... --save --launch --wait) with: classic, Standard (native + MSAA 4x + A2C + aniso 16), Maximum (MSAA 8x + render_scale 1.5). Run m1/m2/m3 checks (classic config) and m3c_check; render the launcher screenshots (--screenshots) and VIEW them, iterate on layout and German texts until they look clean and professional. View the game screenshots of the high-quality configs (frontend, legal, loading) and compare with classic: edges smoother, no blur of UI text, no misplaced 2D elements, correct pillarbox. Fix root causes; record deviations in docs\\loader_design.md section 8 (German). Return the structured status with screenshot paths.`,
  { label: 'integrate', phase: 'Integrate', schema: STATUS_SCHEMA })

const integDigest = integ ? `checks: ${integ.checks.map((c) => `${c.check}: ${c.status} (${c.evidence})`).join(' | ')}; screenshots: ${integ.screenshots.join(', ')}; open: ${integ.open_problems.join(' | ')}; usage: ${integ.how_to_use}` : 'integration agent failed'

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
REVIEW A - RENDERER QUALITY (do NOT edit src\\; own build dir build\\review-m3c-gfx, runs under run\\review_m3c_gfx\\). Integration status: ${integDigest}
Clean build; run m3_check (classic) and m3c_check; take and VIEW your own screenshots at classic, native+MSAA 4x, native+MSAA 8x+render_scale 1.5/2.0, with and without alpha-to-coverage and anisotropy. Judge like a demanding PC player: remaining jaggies, shimmering of alpha-tested textures, blurry or misaligned 2D/UI/text, pillarbox correctness, color/gamma shifts after resolve/downsample, depth artefacts, performance at 4K-class internal resolution (headroom for 120 fps in M3d), debug-layer messages, render-to-texture paths. Check the implementation against D3D11 rules (MSAA resolve formats, A2C with blending off, sample counts, depth format, SV_Coverage/A2C sharpening). Report findings with screenshot paths/metrics.`,
    { label: 'review:renderer', phase: 'Review', schema: FINDINGS_SCHEMA }),
  () => agent(`${CONTEXT}
REVIEW B - LAUNCHER UX AND ROBUSTNESS (do NOT edit launcher\\ or src\\; own build dirs build\\review-m3c-launcher*, runs under run\\review_m3c_launcher\\). Integration status: ${integDigest}
Build the launcher and package dist; render the launcher screenshots and VIEW them: layout, German wording (natural, consistent, no Denglisch where avoidable, correct umlauts), English texts, tooltips, defaults, first-run wizard flow, dark/light mode, 225 % DPI. Test robustness: the user's ISO path with spaces/parentheses/commas, a non-Xbox file, the prototype .bin (Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Xbox - Futurama.bin -> unsupported version message), missing futurama.exe, read-only config location, corrupt INI, unknown keys preserved, portable mode, game exit codes 0/1/3 shown correctly, log tail, re-launch. Check that no Futurama artwork/logo/fonts are in launcher resources or the repo. Report findings with evidence (screenshot paths, commands).`,
    { label: 'review:launcher', phase: 'Review', schema: FINDINGS_SCHEMA }),
])
const allFindings = reviews.filter(Boolean).flatMap((r) => r.findings)
log(`Review: ${allFindings.length} Befunde`)

phase('Fix')
const final = await agent(`${CONTEXT}
PHASE 4: FIX AND REPORT. You own all files. Integration status: ${integDigest}
Review findings (verify each before changing anything; reject with reason if it does not hold):
${allFindings.map((f, i) => `${i + 1}. [${f.severity}] ${f.file}: ${f.problem} | evidence: ${f.evidence} | fix: ${f.suggested_fix}`).join('\n')}
Fix confirmed findings; rebuild everything (game Debug/RelWithDebInfo/clang, launcher Debug/Release; no warnings); rerun fut_tests, launcher tests, m1/m2/m3 (classic) and m3c checks; package dist\\FuturamaPC; render final launcher screenshots and final game screenshots (classic vs Standard vs Maximum) under run\\m3c_final\\ and view them. Write docs\\m3c_report.md (German): what was built, how players use the launcher (first run, settings, presets), how developers build/package, the config schema/CLI contract, AA/resolution design and measurements (performance per preset, edge metric, xemu regression), screenshots paths (run\\, not committed), accepted/rejected findings, known issues, next steps (M3d interpolation up to 120 Hz, M4 input incl. launcher controls tab). Update docs\\ROADMAP.md (M3c to "Erledigt", M3d to "In Arbeit" only if you did not start it: keep it "Als Naechstes"), README.md (player quick start with the launcher), INTERFACES.md. Do NOT commit. Return a short German summary (max 12 sentences) with the most important screenshot paths.`,
  { label: 'fix+report', phase: 'Fix' })

return { parts: parts.map((p) => p && { part: p.part, builds: p.builds }), integration: integ && integ.checks.map((c) => `${c.check}: ${c.status}`), findings: allFindings.length, final }
