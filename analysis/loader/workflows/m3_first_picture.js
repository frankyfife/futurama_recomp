export const meta = {
  name: 'futurama-m3-first-picture',
  description: 'Implement loader milestone M3 (first picture: D3D11 window, NiPCRenderer P0+P1, textures, properties, fixed-function shader) with interface design, parallel modules, integration, review and fixes',
  phases: [
    { title: 'Interfaces', detail: 'RHI, draw items, texture and shader contracts' },
    { title: 'Modules', detail: 'D3D11 backend, texture conversion, NI extraction, shaders in parallel' },
    { title: 'Integrate', detail: 'wire NiPCRenderer slots, get splash/legal/menu on screen' },
    { title: 'Review', detail: 'technical review and visual reproduction' },
    { title: 'Fix', detail: 'apply confirmed findings, report' },
  ],
}

const CONTEXT = `
PROJECT: D:\\FuturamaDecomp (git repo, branch main; do NOT commit - the user commits on request). Native PC port of the Xbox game "Futurama" (retail EU XBE, NetImmerse 4.2.1) via a loader: launcher EXE fixed at 0x10000 + host DLL futurama_host.dll mapping the original XBE, HLE kernel and XDK replacements.
STATE: M1 and M2 are done and verified. The game runs past main through archives and the main loop (contexts Intro -> Frontend) with a NULL renderer: NiPCRenderer (gfx\\nipc_renderer.cpp) replaces NiXBoxRenderer::Create 0x14C170; all 75 slots are no-ops with exact ret N. Read first: D:\\FuturamaDecomp\\docs\\m2_report.md (esp. section 5 measured slot usage per 3000 frames, section 9 known issues, section 10 = the M3 plan you implement), D:\\FuturamaDecomp\\src\\INTERFACES.md (APIs, macros, conventions, changelog), D:\\FuturamaDecomp\\docs\\loader_design.md (2.2 graphics boundary + NiPCRenderer layout + RHI note, 5.x conventions, 6 "M3" scope and exit criteria, section 8 deviations).
M3 GOAL (design section 6 M3): window + RHI with a D3D11 backend; NiPCRenderer P0+P1 per renderer.md 7.2: ClearBuffer (17), DisplayFrame (16, Present), SetBackgroundColor (27) and depth/stencil clear values (26/28), Width/Height (21/22), SetRenderTarget(NULL) (15), BeginScene/EndScene (45/46), SetCameraData (47), RenderScreenPolygon (54), RenderScreenTexture (55, measured as needed for the frontend text), RenderShape/Triangles/Strip/Strips (48-51), CreateSourceTextureRendererData (65) with the formats of renderer.md 5.2 (P8 via palette, slot 69); property->state for Alpha, ZBuffer, Texturing (base map), VertexColor, Material; fixed-function lighting in the shader; Present with VSync, optional 50 Hz limiter; screenshot key. Exit criteria: 1. Splash (NI_Splash_PAL.nif), legal screen and frontend menu appear completely (no xemu reference exists here: judge by viewing your screenshots and by checking that every draw call of those scenes produced visible, plausible output; list remaining visual differences honestly); 2. no D3D debug-layer error (if the D3D11 debug layer / Graphics Tools is not installed, detect it, report it and capture what is possible); 3. --frames=3000 without crash, presented frame rate >= 50.
EVIDENCE: D:\\FuturamaDecomp\\analysis\\loader\\renderer.md (sections 1-8: vtable semantics, data flow, texture formats 5.2, properties 5.4, lights 5.5, priorities 7.2) + nixboxrenderer_vtable.tsv; the original NiXBoxRenderer code in the XBE is the executable specification - read its decompiled functions before implementing a slot (e.g. FUN_00159cd0 property->state, FUN_0015ba40/FUN_0015b6a0 lights, FUN_0015ab40/FUN_00162020 texture pipeline, FUN_0015E010 original slot 65, FUN_001572D0 format choice, FUN_0014f710 CPU skinning, viewport logic around 0x1469E0, and each slot's original implementation listed in nixboxrenderer_vtable.tsv). Verify NetImmerse conventions from the binary rather than memory (camera axes/handedness in the original SetCameraData, matrix layout of NiTransform/NiMatrix3, triangle winding and culling, vertex data layout in NiTriShapeData/NiTriStripsData, NiTexturingProperty map array layout, NiAlphaProperty flag bits). Ghidra snapshot to grep (never open Ghidra): D:\\FuturamaDecomp\\analysis\\retail_eu_export\\decomp\\*.c, functions.tsv, callgraph.tsv, strings.tsv; class metadata analysis\\retail_eu_ni_classes.json; Python 3.14 + capstone; tools\\xbe.py.
TOOLCHAIN: cmd /c "D:\\FuturamaDecomp\\build.cmd" <builddir> <Debug|RelWithDebInfo> [target] (FULL path always). MSVC 14.44 x86, MASM, CMake/Ninja, Windows SDK 10.0.26100 (d3d11, dxgi, d3dcompiler, fxc.exe under C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64 or x86, WIC for PNG). Shaders: compile HLSL at build time (fxc via a CMake custom command into generated headers) so the runtime needs no compiler DLL. futurama.exe is a GUI-subsystem exe (PowerShell: Start-Process -Wait -PassThru; Bash: call directly). Regression tools: tools\\m1_check.py and tools\\m2_check.py must keep passing (see m2_report 3.3 for commands; run\\m2fix\\* configs as templates).
GAME DATA (read-only): ISO "Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso" via INI [paths] iso. Test runs and screenshots ONLY under D:\\FuturamaDecomp\\run\\<your-name>\\ (git-ignored). Never copy game data or screenshots into src\\, docs\\ or tests\\.
CONVENTIONS: INTERFACES.md / design 5.3: English code+comments, German docs; noexcept HLE/slot entries with catch-all; no inline asm; guest addresses as named constants; header comments with address/slot, convention, args, evidence; "// ASSUMPTION:" for guesses; zero warnings (also FUT_WERROR); generated files with --check in tools\\gen\\gen_all.py. The renderer runs on the guest main thread; the window belongs to the boot/UI thread (message loop already exists as stage 13) - a DXGI swapchain may use a window of another thread.
`

const IFACE_SCHEMA = {
  type: 'object',
  properties: {
    build_ok: { type: 'boolean' },
    contract_doc: { type: 'string' },
    summary: { type: 'string' },
    decisions: { type: 'array', items: { type: 'string' } },
  },
  required: ['build_ok', 'contract_doc', 'summary', 'decisions'],
}

phase('Interfaces')
const iface = await agent(`${CONTEXT}
PHASE 1 (you are alone): define the contracts that four agents will implement in parallel, and make the skeleton compile.
First study the evidence enough to get the contracts right: the original slots 16/17/45-51/54/55/65 and property code, the guest structures they read (NiTriShapeData/NiTriStripsData vertex arrays, normals, colors, UV sets, triangle lists/strips, NiSkinInstance, NiTransform, NiBound, NiCamera frustum/port, NiPropertyState at renderer+0xC with its property array, NiDynamicEffectState at +0x10 with lights/texture effects, NiPixelData/NiPalette/NiSourceTexture, NiScreenPolygon/NiScreenTexture). Write src\\host\\gfx\\GFX_CONTRACT.md (English) and the headers:
1. gfx\\rhi\\rhi.h: thin RHI (device/context abstraction: create/update/destroy buffers and textures (formats: RGBA8, BGRA8, RGB565/BGR565, RGBA4/ARGB1555 if needed, BC1/BC2/BC3, A8, L8 - pick from renderer.md 5.2), samplers, blend/depth/raster state objects from small POD descriptors (hash-cached by the backend), constant buffers, draw indexed/non-indexed, viewport/scissor, clear, present, screenshot/readback, debug-layer message retrieval). Backend-neutral so that a Vulkan/D3D12 backend can follow (M10).
2. gfx\\ni_extract.h: plain host-side structs produced from guest NetImmerse objects: DrawItem {world matrix, vertex stream views (positions, normals, colors, uv0..uvN, already converted to host-friendly float arrays or pointing into guest memory with stride info), index data (triangle list or strips), RenderStateDesc (from properties: alpha blend/test, z test/write, cull, texturing stages with texture handles and address/filter modes, vertex color mode, material colors, specular, fog), LightSetDesc (ambient + up to N lights with type/color/position/direction/attenuation/spot), TextureEffect slots reserved}, CameraDesc (view, projection from frustum incl. orthographic, viewport from port), ScreenPolygonDesc and ScreenTextureDesc. Functions: ExtractCamera(guest camera...), ExtractShape(...), ExtractStrips(...), ExtractScreenPolygon(...), ExtractScreenTexture(...), ExtractState(propertyState, effectState).
3. gfx\\texconv.h: ConvertPixelData(guest NiPixelData*, optional palette) -> HostImage {format, width, height, mip chain, bytes} with the format table; plus the mapping of slot 65's source order to it.
4. gfx\\shaders\\: ubershader interface: the constant buffer layout (per-frame, per-draw, material, lights, texture stage ops) as a shared header usable from C++ and HLSL (or mirrored with static_asserts), a "feature key" bitfield selecting shader permutations (or uniform branching), and the vertex input layout(s).
5. Ownership table for phase 2 in GFX_CONTRACT.md: backend agent: gfx\\rhi\\d3d11\\**, platform\\** (window, present loop integration, screenshot key, 50 Hz limiter, debug-layer capture), tests\\rhi_*; texconv agent: gfx\\texconv.*, tests\\texconv_*; extract agent: gfx\\ni_extract.*, gfx\\ni_guest_layouts.h (guest NetImmerse struct layouts with static_asserts and evidence), tests\\extract_*; shader agent: gfx\\shaders\\**, gfx\\ff_pipeline.* (RenderStateDesc/LightSetDesc -> RHI state objects + constant buffers + shader selection), CMake custom commands for fxc (additive change in the gfx CMake section); integration later owns gfx\\nipc_renderer.cpp wiring. Shared headers: only additive changes, logged in the changelog of INTERFACES.md.
6. Make everything compile (stub implementations returning empty/unsupported) in build\\m3-iface; keep tools\\m2_check.py passing (renderer still null). Return the structured result.`,
  { label: 'interfaces', phase: 'Interfaces', schema: IFACE_SCHEMA })

if (!iface || !iface.build_ok) {
  log('Schnittstellenphase gescheitert - Abbruch')
  return { stage: 'interfaces', iface }
}

const MODULE_RULES = `
PHASE 2 RULES: four agents work IN PARALLEL. Read src\\host\\gfx\\GFX_CONTRACT.md and obey its ownership table; shared headers only additively with a changelog entry in src\\INTERFACES.md. Build only in YOUR OWN build directory build\\m3-<module>. If a build breaks because of another agent's file mid-edit, wait and retry. Unit-test your module in isolation (doctest in tests\\). Verify every guest structure offset against the decompiled original renderer code and record the evidence (function + instruction address) next to the definition.`

const MODULES = [
  { key: 'backend', prompt: `YOU ARE THE BACKEND AGENT: implement the D3D11 backend of rhi.h (device with debug layer in Debug builds when available - detect absence of the SDK layers cleanly; ID3D11InfoQueue messages routed to the GFX log channel, errors counted for exit criterion 2), the window (platform: 640x480 client area scalable with aspect preserved, created on the UI thread, visible from the first Present), swapchain with VSync (flip model), Present from DisplayFrame, optional 50 Hz limiter ([render] limit_50hz), screenshot key F12 plus config [debug] screenshot_frames=<list> writing PNG via WIC to <userdata>\\screenshots\\frame_<n>.png, presented-frame-rate statistics logged at exit, clean shutdown/teardown and device-removed handling. Unit-test what can run headless (e.g. WARP device for state-object caching, texture upload/readback round trips for every RHI format).` },
  { key: 'texconv', prompt: `YOU ARE THE TEXCONV AGENT: implement texconv per contract: every NiPixelData format the game actually uses (enumerate them: read renderer.md 5.2, FUN_001572D0, FUN_0015E010, NiPixelData/NiPixelFormat layouts, and measure real data: write a scratch tool under run\\m3-texconv\\ that loads all .nif files from the ISO archives via tools\\futimg.py/xdvdfs.py and lists the pixel formats found (do not store game data outside run\\)), palette expansion (NiPalette, alpha handling), DXT1/3/5 pass-through as BCn (check block layout/endianness), 16-bit formats, mipmaps (generate if absent? follow what the original renderer does), power-of-two handling and the 8x8 default texture. Unit tests with synthetic data for each format; a scratch run over the real textures counting conversions per format and failures.` },
  { key: 'extract', prompt: `YOU ARE THE EXTRACT AGENT: implement ni_extract per contract and the guest layout header gfx\\ni_guest_layouts.h: NiAVObject world transform (NiTransform: rotation matrix layout, translation, uniform scale), NiTriShapeData/NiTriStripsData/NiGeometryData vertex arrays (positions, normals, colors, texture sets count and arrays, active vertex count), triangle lists and strips (strip lengths), NiSkinInstance (use the original CPU skinning helper FUN_0014f710 if it is side-effect free - verify - otherwise replicate), NiCamera (frustum l/r/t/b/n/f, orthographic flag, port, world transform; derive view/projection exactly like the original SetCameraData 47 incl. NetImmerse camera axes), NiPropertyState (property array indices per property type), the properties of M3 (NiAlphaProperty flags/blend modes/test ref, NiZBufferProperty, NiTexturingProperty (apply mode, base map texture/clamp/filter/uv set), NiVertexColorProperty, NiMaterialProperty, plus Specular/Wireframe/Shade/Dither/Fog/Stencil decoded but possibly unused), NiDynamicEffectState lights (Ambient/Directional/Point/Spot per FUN_0015b6a0), NiScreenPolygon/NiScreenTexture (screen-space vertices, texture, clipping). Unit tests on synthetic guest objects built in memory with the verified layouts.` },
  { key: 'shader', prompt: `YOU ARE THE SHADER AGENT: implement the fixed-function emulation: HLSL ubershader(s) (vertex: transform, per-vertex lighting like D3D fixed function with material/vertex-color sources, fog factor; pixel: texture stages per NiTexturingProperty apply mode (replace/decal/modulate, as the original maps them to D3D texture stage states in FUN_00159cd0/FUN_0015ab40), vertex color, alpha test, fog), compiled at build time with fxc into headers (CMake custom command, rebuild on change), ff_pipeline.* translating RenderStateDesc/LightSetDesc into RHI state objects, constant buffer contents and shader permutation keys, plus screen-space paths for screen polygons/textures (pre-transformed vertices). Mirror the original D3D render-state choices: read how NiXBoxRenderer sets D3DRS_* and texture stage states for each property (renderer.md 5.4, FUN_00159cd0) and replicate the semantics. Unit tests: constant buffer packing, state mapping tables, shader compile in the build.` },
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
  agent(`${CONTEXT}\nInterface phase summary: ${iface.summary}\nDecisions: ${iface.decisions.join(' | ')}\n${MODULE_RULES}\n${m.prompt}\nReturn the structured result.`,
    { label: `module:${m.key}`, phase: 'Modules', schema: MODULE_SCHEMA })))

const modDigest = mods.filter(Boolean).map((m) => `[${m.module}] compiles=${m.compiles}; implemented: ${m.implemented.join(', ')}; shared: ${m.shared_changes.join(' | ')}; open: ${m.open_points.join(' | ')}`).join('\n')
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
    screenshots: { type: 'array', items: { type: 'string' }, description: 'absolute paths of representative screenshots under run\\' },
    visual_differences: { type: 'array', items: { type: 'string' } },
    open_problems: { type: 'array', items: { type: 'string' } },
    how_to_run: { type: 'string' },
  },
  required: ['criteria', 'screenshots', 'visual_differences', 'open_problems', 'how_to_run'],
}

phase('Integrate')
const integ = await agent(`${CONTEXT}
PHASE 3: INTEGRATION. Module results${missing.length ? ` (missing: ${missing.join(', ')} - implement those parts yourself)` : ''}:
${modDigest}
You own ALL files now. Wire NiPCRenderer (gfx\\nipc_renderer.cpp) slots 15/16/17/21/22/26/27/28/45/46/47/48-51/54/55/65/69 (and whatever else the scenes need, measured) to extraction -> ff_pipeline -> RHI. Build Debug + RelWithDebInfo without warnings. Create run\\m3\\futurama.ini (ISO, userdata run\\m3, frames=3000, screenshot_frames covering the legal screen, the splash and the frontend menu - find the right frame numbers from the context log). Run, view the screenshots yourself (Read tool on the PNG), and iterate until legal screen, splash and frontend menu look complete and correct (orientation, colors, alpha, text, depth, no missing elements). Count per scene the draw calls issued vs. skipped. Fix root causes; record deviations in docs\\loader_design.md section 8 (German, B19+). Write tools\\m3_check.py (pattern of m2_check.py): exit 0, frame limit reached, presented fps >= 50, 0 debug-layer errors, screenshots written and non-trivial (e.g. not uniform, plausible histogram), plus everything m2_check requires. Keep m1/m2 checks passing (the null-renderer path may become a config option [render] backend=null|d3d11). Return the structured status including screenshot paths and an honest list of visual differences.`,
  { label: 'integrate', phase: 'Integrate', schema: STATUS_SCHEMA })

const integDigest = integ ? `criteria: ${integ.criteria.map((c) => `${c.criterion}: ${c.status} (${c.evidence})`).join(' | ')}; screenshots: ${integ.screenshots.join(', ')}; visual diffs: ${integ.visual_differences.join(' | ')}; open: ${integ.open_problems.join(' | ')}; run: ${integ.how_to_run}` : 'integration agent failed'

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
PHASE 4: TECHNICAL REVIEW (read-only for src\\; scratch under run\\review_m3_tech\\). Integration status: ${integDigest}
Review the M3 changes (git diff HEAD) against the original renderer code and renderer.md: camera/projection math and axes, matrix order, winding/culling, vertex layout and strides, strips, index limits, texture formats and palette/alpha, sampler/address modes, every property->state mapping vs FUN_00159cd0 (alpha blend factors, alpha test ref/func, z func/write, vertex color/material source, lighting equations vs D3D fixed function), screen polygon/texture coordinate conventions (pixel centers), resource lifetime (purge slots, device lost), threading (render thread vs window thread), performance hot spots (per-draw allocations, state hashing), debug-layer usage. Report only issues with concrete evidence.`,
  { label: 'review:tech', phase: 'Review', schema: FINDINGS_SCHEMA }),
  () => agent(`${CONTEXT}
PHASE 4: VISUAL + REPRODUCTION REVIEW (do NOT edit src\\; own build dir build\\review-m3, user data run\\review_m3_vis\\). Integration status claimed: ${integDigest}
Clean build (Debug, RelWithDebInfo, clang), run fut_tests, m1_check, m2_check and m3_check (several runs). Take your own screenshots of the legal screen, splash and frontend menu (and additional frames in between), view them, and judge critically like a player who knows the game: wrong colors/channel order, upside-down or mirrored images, wrong aspect, missing text or elements, alpha/transparency errors, z-fighting, texture seams, flicker between frames (compare consecutive screenshots), letterboxing. Also check the frame rate and that the window behaves (resize, minimize, close -> orderly exit). Report every deviation as a finding with screenshot paths.`,
  { label: 'review:visual', phase: 'Review', schema: FINDINGS_SCHEMA }),
])

const allFindings = reviews.filter(Boolean).flatMap((r) => r.findings)
log(`Review: ${allFindings.length} Befunde`)

phase('Fix')
const final = await agent(`${CONTEXT}
PHASE 5: FIX AND REPORT. You own all files. Integration status: ${integDigest}
Review findings (verify each before changing anything; reject with reason if it does not hold):
${allFindings.map((f, i) => `${i + 1}. [${f.severity}] ${f.file}: ${f.problem} | evidence: ${f.evidence} | fix: ${f.suggested_fix}`).join('\n')}
Fix all confirmed findings; rebuild Debug, RelWithDebInfo, clang (no warnings); rerun fut_tests, m1_check, m2_check, m3_check; take final screenshots of legal screen, splash and frontend menu under run\\m3final\\ and view them. Write the German report D:\\FuturamaDecomp\\docs\\m3_report.md in the style of m2_report.md: what was built, how to run (config example, screenshot key/options), status of each M3 exit criterion with evidence, paths of the final screenshots (under run\\, not committed), remaining visual differences, slot coverage, performance numbers, deviations, accepted/rejected findings, known issues, next steps for M4 (input) and beyond. Update README.md "Loader" section briefly. Do NOT commit. Return a short German summary (max 12 sentences) including the final screenshot paths.`,
  { label: 'fix+report', phase: 'Fix' })

return { iface: iface.summary, modules: mods.map((m, i) => ({ module: MODULES[i].key, ok: !!m && m.compiles })), integration: integ && { criteria: integ.criteria.map((c) => `${c.criterion}: ${c.status}`), screenshots: integ.screenshots }, findings: allFindings.length, final }
