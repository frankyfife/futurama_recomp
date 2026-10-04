export const meta = {
  name: 'futurama-loader-recon',
  description: 'Analyze the Futurama XBE for everything a PC host loader must provide, verify each report, then write the loader design',
  phases: [
    { title: 'Recon', detail: '7 parallel analyses of loader requirements' },
    { title: 'Verify', detail: 'independent fact-check of each report against the binary' },
    { title: 'Design', detail: 'synthesize the loader architecture document' },
  ],
}

const CONTEXT = `
PROJECT CONTEXT (read carefully)
- Project root: D:\\FuturamaDecomp. Goal: native x86 PC port of the Xbox game "Futurama" (2003, Unique Development Studios, NetImmerse 4.2.1 engine, XDK 5233).
- Chosen strategy ("loader way", like re3): a 32-bit Windows host process maps the ORIGINAL XBE sections at their original virtual addresses (base 0x10000), provides the xboxkrnl kernel imports itself (HLE), replaces statically linked XDK library functions (D3D8, DSOUND, XMV, XInput/XPP, parts of XAPI) with PC implementations by patching/hooking them, then game functions get replaced one by one with decompiled C++. Later the renderer becomes Vulkan/D3D12.
- Target binary: D:\\FuturamaDecomp\\orig\\retail_eu\\default.xbe (Retail EU/PAL, 5 languages). The prototype XBE (orig\\proto) is only a reference, ignore it unless explicitly useful.
- XBE sections (retail_eu): .text va 0x11000 vsz 0x2232c0 | D3D 0x2342c0 vsz 0x12a38 | DSOUND 0x246d00 vsz 0x1d654 | XMV 0x264360 vsz 0x27b6c | XPP 0x28bee0 vsz 0x767c | .rdata 0x293560 vsz 0x2a97c | .data 0x2bdee0 vsz 0x52858 (raw only 0x1b558, rest is bss) | DOLBY 0x310740 vsz 0x7180 | $$XTIMAGE 0x3178c0 | .XTLID 0x31a0c0.
- Known addresses (retail_eu): entry 0xf84dd (creates main thread at 0xf8469), XAPILIB::XapiInitProcess 0xf92c0, XAPILIB::_cinit 0xf9783 (C init table 0x2d5b34-0x2d5b48, C++ init table 0x2bdef0-0x2d5b30 with ~24300 NetImmerse NiStream::RegisterLoader stubs in 0x17e010-0x233880), kernel thunk table at 0x293560 (111 entries, ordinal|0x80000000, zero-terminated), TLS directory at 0x2a5d2c. NiStream::RegisterLoader 0x1087d0, NiRTTI ctor 0x141e60, allocator used by CreateObject 0xa4900.
- Exported Ghidra snapshot (USE THIS, it is fast to grep):
  * D:\\FuturamaDecomp\\analysis\\retail_eu_export\\decomp\\*.c  - decompiled C of all 31337 functions, one file per 64 KB, each function starts with "// ==== FUNC <addr> <name> size=<n> ====". Initializer stubs are in decomp\\initstubs_*.c.
  * ...\\functions.tsv (address, size, block, name, signature), ...\\callgraph.tsv (caller, caller_name, callee, callee_name), ...\\strings.tsv (address, referencing functions, string)
  * Names: XbSymbolDatabase identified XDK functions as "D3D8::D3DDevice_*", "DSOUND::*", "XAPILIB::*"; Ghidra FID named CRT functions; kernel imports appear by name (NtCreateFile, KeSetTimer, ...). NetImmerse classes were recovered (e.g. "NiNode::vf12" = virtual slot 12, "NiXBoxRenderer::vf40"); class metadata in D:\\FuturamaDecomp\\analysis\\retail_eu_ni_classes.json. Everything else is FUN_xxxxxxxx.
- Raw binary helpers: python with capstone 5 is installed. D:\\FuturamaDecomp\\tools\\xbe.py has class Xbe(path) with .sections, .va2off(va), .off2va(off), .read(va,n), .u32(va), .cstr(va), .section(va). Example: import sys; sys.path.insert(0, r"D:\\FuturamaDecomp\\tools"); from xbe import Xbe.
- Rules: do NOT open or run Ghidra on D:\\FuturamaDecomp\\ghidra (project lock would break other agents). Do not modify anything outside D:\\FuturamaDecomp\\analysis\\loader\\. Helper scripts you write go to D:\\FuturamaDecomp\\analysis\\loader\\scripts\\ (name them <topic>_*.py) so results are reproducible. Disc images must not be modified.
- Be factual: every claim needs evidence (address, function, instruction bytes or a script output). Mark guesses explicitly as "Vermutung". Do not invent kernel ordinals, signatures or API behaviour: if you state an xboxkrnl ordinal->name mapping or a calling convention, back it with the binary (e.g. how call sites pass arguments, stack cleanup "ret N") rather than memory alone.
- Report language: German prose, technical identifiers in English. Use Markdown tables where useful.
`

const TOPICS = [
  {
    key: 'kernel',
    title: 'Kernel-Imports (xboxkrnl)',
    prompt: `Analyze the xboxkrnl kernel imports the host loader must implement.
1. Decode all 111 entries of the kernel thunk table at 0x293560 (ordinal = value & 0x7fffffff). Map each ordinal to its export name; cross-check every name against the names Ghidra shows in the decompiled code (call sites in decomp\\*.c use the names) and against the thunk-table position (the call sites reference "[0x2935xx]" style addresses).
2. Separate functions from DATA exports (e.g. KeTickCount, XboxHardwareInfo, XboxKrnlVersion, LaunchDataPage, XeImageFileName, HalDiskCachePartitionCount, IdexChannelObject, ...): data imports are dereferenced, not called. Determine for each data import how the game uses it.
3. For every function import determine from the binary: calling convention (stdcall with ret N / fastcall with ecx/edx args like KfRaiseIrql, KfLowerIrql, ObfDereferenceObject, ObfReferenceObject, KiUnlockDispatcherDatabase / cdecl like DbgPrint), argument count, number of call sites, and which code calls it (XAPI library, D3D, DSOUND, XMV, XPP, LIBCMT, NetImmerse, game code). Write the scripts that compute this.
4. For each import give a host implementation note: Win32 equivalent or approach, and difficulty (trivial / mittel / schwer), and whether it is only needed by an XDK library we plan to replace anyway (D3D/DSOUND/XMV/XPP would be HLE-replaced at API level, so kernel calls made only from those libraries may be stubbed). Pay special attention to: memory (MmAllocateContiguousMemory(Ex), MmFreeContiguousMemory, MmGetPhysicalAddress, MmMapIoSpace, MmPersistContiguousMemory, MmQueryAllocationSize, NtAllocateVirtualMemory, MmClaimGpuInstanceMemory), IRQL/DPC/interrupt functions (KeConnectInterrupt, KeInitializeDpc, KeInsertQueueDpc, KfRaiseIrql), timers/events/semaphores/waits, file I/O (NtCreateFile/NtReadFile/IoCreateSymbolicLink/IoDismountVolume...), threads (PsCreateSystemThreadEx), HAL (HalReadSMBusValue, HalWriteSMBusValue, HalReturnToFirmware, AvSetDisplayMode, AvGetSavedDataAddress...), XboxEEPROM/ExQueryNonVolatileSetting, crypto (XcSHA*, XcHMAC...).
Write the report to D:\\FuturamaDecomp\\analysis\\loader\\kernel.md with a full table (ordinal, name, kind, convention, args, call sites, callers, host plan, difficulty) and also a machine-readable D:\\FuturamaDecomp\\analysis\\loader\\kernel_imports.tsv with the same columns.`,
  },
  {
    key: 'cpu',
    title: 'FS-Segment, privilegierte Befehle, Hardwarezugriffe',
    prompt: `Find every piece of code that would not work as-is in a 32-bit Windows user-mode process.
1. FS segment: disassemble all executable sections (.text, D3D, DSOUND, XMV, XPP, also DOLBY if executable) with capstone (linear sweep is fine, but cross-check suspicious hits against Ghidra function boundaries from functions.tsv) and list every instruction with an fs: segment override. Group by fs offset (fs:[0] SEH chain is compatible with the Windows TEB/NT_TIB; others like fs:[0x18], fs:[0x20], fs:[0x28], fs:[0x2c], fs:[0x34]... are Xbox KPCR/KPRCB/KTHREAD fields and differ from the Windows TEB). For each non-zero offset: which functions, what they read (e.g. KPCR->PrcbData.CurrentThread, thread->TlsData), and how __declspec(thread)/TLS is accessed (TLS directory at 0x2a5d2c, index variable). Propose a concrete strategy: patch each site (give the instruction byte patterns and lengths, whether a 5-byte jmp/call fits), or reuse TEB slots, or an alternative. Evaluate the risks of each.
2. Privileged / I/O instructions: in, out, ins, outs, cli, sti, hlt, lidt/lgdt, mov to/from cr0-cr4/dr*, rdmsr/wrmsr, wbinvd, invd, invlpg, sysenter, int (other than 3), iret, also rdtsc and cpuid (allowed but note usage). List location + owning function + library.
3. Hardware/MMIO addresses as immediates or in data: NV2A GPU 0xFD000000-0xFDFFFFFF, APU 0xFE800000-0xFE8FFFFF, AC97 0xFEC00000/0xFEC00100, USB 0xFED00000/0xFED08000, NIC 0xFEF00000, flash 0xFF000000+, PCI config ports 0xCF8/0xCFC, SMBus 0xC000, physical-memory windows (0x80000000-0x83FFFFFF contiguous memory mapping, 0xF0000000 write-combined/tiled frame buffer aliases), kernel addresses (0x80010000 kernel image). Report which functions/libraries reference them and whether game/NetImmerse code (not only D3D/DSOUND/XPP libs) does.
4. Self-modifying code or code that depends on its own address in ways that matter for a loader (e.g. jump tables are fine; but XLoadSection/XFreeSection of the DOLBY/$$XTIMAGE sections, section handles, XeLoadSection) - list them.
Write the report to D:\\FuturamaDecomp\\analysis\\loader\\cpu.md and a machine-readable list of all fs:-sites to D:\\FuturamaDecomp\\analysis\\loader\\fs_sites.tsv (address, bytes, instruction, offset, function, section).`,
  },
  {
    key: 'd3d',
    title: 'Direct3D-8-Schnittstelle (Xbox D3D8)',
    prompt: `Inventory the Xbox Direct3D 8 surface used by the game, to decide where the graphics HLE boundary goes.
1. List all functions named D3D8::* (and other XGraphics/XGRAPHC names like XGSwizzleRect, XGSetVertexBufferHeader...) with address, section, size, and number of callers; then list all callers grouped by caller kind: NiXBoxRenderer::* methods, other Ni* classes, game code (FUN_ outside the libraries), XMV library, other library code.
2. The Xbox D3D8 has many inline functions that write directly into the GPU push buffer (pattern: load D3D device / g_pDevice pointer, compare put pointer against a limit, call a MakeSpace function, write NV2A method headers like (count<<18)|(subchannel<<13)|method). Find such inlined push-buffer writes OUTSIDE the D3D section (in .text): identify the device global (D3D__pDevice), the MakeSpace function(s), and every function that writes the push buffer directly. Group them by owner (NiXBoxRenderer methods vs others). These are the hard part of a D3D-level HLE.
3. Find the D3D device creation (Direct3D_CreateDevice / D3DDevice_* init), present parameters (resolution, back buffer format, depth format, refresh rate / PAL 50-60 Hz, flags), frame presentation (D3DDevice_Swap/Present, BlockUntilVerticalBlank, SetVerticalBlankCallback), resource creation (textures, vertex buffers, index buffers: D3DDevice_CreateTexture, Lock/Unlock, D3DResource_Register, XGSwizzle), vertex shaders (D3DDevice_CreateVertexShader with NV2A microcode, SetVertexShaderConstant), pixel shaders (CreatePixelShader / register combiners), render states incl. Xbox-specific ones.
4. Identify non-renderer D3D users: movie playback (XMV), fonts / text rendering, loading screen, HUD/2D, screenshot, save icons, anything else.
5. Recommend the HLE boundary with arguments and numbers: (a) Xbox-D3D8-API level (like Cxbx-Reloaded: every D3D8 function + all inline push-buffer sites must be handled), (b) NiRenderer level (replace NiXBoxRenderer methods) plus a small D3D shim for the remaining non-renderer users, (c) hybrid. Estimate the number of functions/sites each option requires.
Write the report to D:\\FuturamaDecomp\\analysis\\loader\\d3d.md and a TSV of all D3D functions with caller counts to D:\\FuturamaDecomp\\analysis\\loader\\d3d_functions.tsv.`,
  },
  {
    key: 'avi',
    title: 'Audio (DSOUND), Video (XMV), Input (XInput/XPP)',
    prompt: `Inventory the audio, movie and input APIs the host must provide.
1. DSOUND: all DSOUND::* functions (named by XbSymbolDatabase) and the unnamed functions in the DSOUND section; which are called from outside the DSOUND section (that is the API surface the game/NetImmerse/XMV actually uses): DirectSoundCreate, IDirectSound_CreateSoundBuffer / CreateSoundStream, buffer methods (SetBufferData, Play, Stop, SetVolume, SetFrequency, SetPosition (3D), SetHeadroom, SetMixBins...), DirectSoundDoWork, XAudio functions (XAudioCreateAdpcmFormat...), I3DL2 listener, effects (DSP image from the DOLBY section / dsstdfx.bin file). Give each with call-site count and callers. Find the game's sound system (wrapper classes around these calls, the SoundLookup/.wav loading) and describe it: formats used (PCM, Xbox ADPCM), streaming vs static buffers, number of voices, 3D sound.
2. XMV: the XMV section is unnamed by XbSymbolDatabase. Identify the XMV decoder API entry points used by the game (XMVDecoder_CreateDecoderForFile, XMVDecoder_GetNextFrame, XMVDecoder_GetVideoDescriptor, XMVDecoder_EnableAudioStream, XMVDecoder_CloseDecoder, ...) from call structure, strings and behaviour; find the game's movie player code (Video\\%s.xmv paths, the .txt subtitle files) and how frames are presented (D3D textures/overlay, YUY2?). Check the actual .xmv files' format (they are on the disc image Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso; you may read but never modify it; tools in D:\\FuturamaDecomp\\tools\\xdvdfs.py can list/extract; extract at most two small .xmv samples to D:\\FuturamaDecomp\\analysis\\loader\\samples\\). Check whether the installed Python package "av" (PyAV/ffmpeg) can demux/decode them (try it and report codec names), since ffmpeg would be the host decoder.
3. Input: XInitDevices, XGetDevices, XGetDeviceChanges, XInputOpen, XInputClose, XInputGetState, XInputSetState (rumble), XInputGetCapabilities, XInputPoll, memory unit / headset APIs, XPP section functions. Which are used, how many controllers, how the game maps buttons (find button handling code, e.g. analog button thresholds), rumble usage.
For each API give the recommended host implementation (XAudio2 or SDL audio, ffmpeg via libav* for XMV, XInput/SDL GameController) and difficulty.
Write the report to D:\\FuturamaDecomp\\analysis\\loader\\avi.md.`,
  },
  {
    key: 'files',
    title: 'Dateisystem, Archive, Spielstände',
    prompt: `Describe everything the game does with files and persistent data, so the host can provide a virtual file system.
1. All file paths and path templates in strings.tsv and the decompiled code: drive letters (D:\\ disc, T:\\ title data, U:\\ user data, Z:\\ utility/cache, \\Device\\CdRom0, \\Device\\Harddisk0\\Partition1..., the .img archive names FutPAL0x.img / SysPAL.img / sysNTSC.img, video paths, scripts\\%s.Ful, Language\\FileNames.ldb, BUILD.TXT, EffectList.txt, menu\\extras\\unlock.txt, ...). For each: who uses it and how.
2. The file API calls: XAPI CreateFile/ReadFile/ReadFileEx (overlapped/async?), SetFilePointer, GetFileSize, FindFirstFile, XMountUtilityDrive, XFormatUtilityDrive, XGetDiskSectorSize, direct NtCreateFile/NtReadFile from game code, and the asynchronous loading/streaming design (threads, completion routines, sector alignment / no-buffering requirements).
3. Find the game's .img archive reader: the code that parses the archive header (format: u32 header size; entries = NUL-terminated name padded to 4 bytes, directory if first char has bit 7 set followed by u32 end offset, otherwise u32 size + u32 offset; see D:\\FuturamaDecomp\\tools\\futimg.py). Find the lookup function(s) the rest of the game uses to open a file inside an archive, the archive selection logic (which of the 5-6 archives, PAL vs NTSC, language), and caching.
4. Save games and settings: XCreateSaveGame, XFindFirstSaveGame, XDeleteSaveGame, XCalculateSignature*, saveimage.xbx/icon handling, file names and sizes of save data, and any settings persisted (XGetLanguage, XGetAVPack, XGetVideoStandard/flags, XGetGameRegion, ExQueryNonVolatileSetting).
5. Recommend the host virtual file system: read directly from the original ISO via an XDVDFS reader vs. extracted files; mapping of D:/T:/U:/Z:; save location on PC; what must be byte-identical (signatures).
Write the report to D:\\FuturamaDecomp\\analysis\\loader\\files.md.`,
  },
  {
    key: 'startup',
    title: 'Startsequenz, Hauptschleife, Threads, Speicher',
    prompt: `Reconstruct how the game starts and runs, from the XBE entry point to the main loop.
1. Follow entry (0xf84dd) -> main thread (0xf8469) -> XapiInitProcess (0xf92c0) -> _cinit -> the game's main()/WinMain equivalent. Name the functions on this path, explain each step (what XapiInitProcess sets up: heaps, TLS, cache partition, XAPI init; which kernel calls), and the exact order of game initialisation (memory manager, file system/archives, renderer/D3D device creation, sound, input, script system, front end).
2. Find the main game loop: per-frame update, render, present, frame timing (QueryPerformanceCounter / KeQueryPerformanceCounter / KeTickCount / vblank-based), fixed or variable time step, PAL 50 Hz vs NTSC 60 Hz handling, and how the loop exits (XLaunchNewImage/reboot to dashboard).
3. Threads: every CreateThread/PsCreateSystemThreadEx call site, thread entry functions, priorities (KeSetBasePriorityThread/SetThreadPriority), what each thread does (loader/streaming, sound DoWork, XMV decoding, ...), and the synchronisation primitives used (critical sections, events, semaphores, timers, DPCs).
4. Memory: which allocators the game uses (NetImmerse NiMalloc/NiNew -> ?, game allocator at 0xa4900 used by CreateObject, CRT malloc, XAPI HeapAlloc/LocalAlloc, XPhysicalAlloc/MmAllocateContiguousMemory for GPU/audio data, MmAllocateContiguousMemoryEx alignment/protection flags), peak memory expectations (Xbox has 64 MB), any memory pools with fixed addresses.
5. TLS: the IMAGE_TLS_DIRECTORY at 0x2a5d2c (raw data range, zero fill, index address, callbacks) and how TLS slots are used.
6. Exceptions: SEH/C++ EH usage (__CxxFrameHandler present) - anything loader relevant (e.g. unhandled exception filter, RaiseException).
Produce a call-tree style description with addresses. Write the report to D:\\FuturamaDecomp\\analysis\\loader\\startup.md.`,
  },
  {
    key: 'renderer',
    title: 'NetImmerse-Renderer-Schnittstelle',
    prompt: `Analyze the NetImmerse renderer interface, which is where a future Vulkan/D3D12 renderer will plug in, and which may also be the graphics HLE boundary for the loader.
1. Use D:\\FuturamaDecomp\\analysis\\retail_eu_ni_classes.json: NiRenderer (vtable 0x2a9ef0, 72 slots) and NiXBoxRenderer (vtable 0x2aa7c0, 75 slots, object size 1744). For every vtable slot of NiXBoxRenderer: the implementing function, whether it overrides NiRenderer, and from the decompiled code what it does (e.g. BeginScene/EndScene/DisplayScene, Clear, SetCameraData, Draw of NiTriShape/NiTriStrips/NiParticles/NiLines/NiScreenPolygon, texture/renderer data creation (PrecacheTexture?), PurgeGeometryData/PurgeTexture, Get/Set render target, screen shots, etc.). Use the names of the NetImmerse API you can justify from behaviour; mark as Vermutung where unsure. Also list who calls each slot (game code vs NetImmerse core) by scanning the decomp for virtual calls through the renderer object (e.g. "(**(code **)(*piVar + 0xNN))" with the renderer pointer) and direct calls.
2. Non-virtual NiXBoxRenderer helpers and Xbox-specific renderer data classes (texture data, geometry data / vertex buffers, shaders, render state cache). Describe the data flow: NiSourceTexture/NiPixelData -> GPU texture (formats, swizzling, DXT), NiGeometryData/NiTriShapeData -> vertex/index buffers (dynamic vs static), properties (NiAlphaProperty, NiTexturingProperty, NiZBufferProperty, NiMaterialProperty, NiVertexColorProperty, NiStencilProperty, NiFogProperty, NiSpecularProperty, NiWireframeProperty, NiDitherProperty, NiShadeProperty) -> render states, lights (NiPointLight/NiDirectionalLight/NiSpotLight/NiAmbientLight), NiTextureEffect (projected lights/shadows, environment maps), skinning (NiSkinInstance/NiSkinPartition: hardware skinning with vertex shaders?).
3. Find where the game creates the renderer (NiXBoxRenderer::Create or ctor call) and the game-side render loop calls into it (camera setup, culling, NiNode drawing, 2D/HUD drawing).
4. Count how many distinct D3D8 functions/push-buffer sites are reachable only through NiXBoxRenderer vs. from elsewhere.
5. Recommend what an own renderer class (NiVulkanRenderer / NiD3D12Renderer) must implement, ordered by priority to get first pixels.
Write the report to D:\\FuturamaDecomp\\analysis\\loader\\renderer.md and a TSV of the NiXBoxRenderer vtable to D:\\FuturamaDecomp\\analysis\\loader\\nixboxrenderer_vtable.tsv (slot, address, overrides_parent, guessed_name, confidence, callers).`,
  },
]

const RECON_SCHEMA = {
  type: 'object',
  properties: {
    report_path: { type: 'string' },
    summary: { type: 'string', description: 'German, 5-10 sentences, the most loader-relevant results' },
    key_facts: {
      type: 'array',
      items: {
        type: 'object',
        properties: { claim: { type: 'string' }, evidence: { type: 'string' } },
        required: ['claim', 'evidence'],
      },
    },
    open_questions: { type: 'array', items: { type: 'string' } },
  },
  required: ['report_path', 'summary', 'key_facts', 'open_questions'],
}

const VERIFY_SCHEMA = {
  type: 'object',
  properties: {
    report_path: { type: 'string' },
    checked: { type: 'integer' },
    confirmed: { type: 'integer' },
    corrections: {
      type: 'array',
      items: {
        type: 'object',
        properties: { claim: { type: 'string' }, problem: { type: 'string' }, fix: { type: 'string' } },
        required: ['claim', 'problem', 'fix'],
      },
    },
    missing: { type: 'array', items: { type: 'string' }, description: 'important things the report missed' },
    confidence: { type: 'string', enum: ['hoch', 'mittel', 'niedrig'] },
  },
  required: ['report_path', 'checked', 'confirmed', 'corrections', 'missing', 'confidence'],
}

phase('Recon')
log('Starte 7 Analysen mit je einer unabhängigen Verifikation (Pipeline, keine Barriere)')

const results = await pipeline(
  TOPICS,
  (t) => agent(
    `${CONTEXT}\nYOUR TOPIC: ${t.title}\n\n${t.prompt}\n\nWhen done, return the structured summary. key_facts: the 8-20 most important, checkable facts with evidence.`,
    { label: `recon:${t.key}`, phase: 'Recon', schema: RECON_SCHEMA },
  ),
  (r, t) => r && agent(
    `${CONTEXT}\nYou are an independent, skeptical reviewer. Another agent analysed the topic "${t.title}" and wrote the report ${r.report_path}.
Its summary: ${r.summary}
Its key facts (claim | evidence):
${r.key_facts.map((f, i) => `${i + 1}. ${f.claim} | ${f.evidence}`).join('\n')}

Your job:
1. Read the report fully. Re-check EVERY key fact above and at least 10 further concrete claims from the report (tables included) directly against the binary / exports with your own scripts (put them in D:\\FuturamaDecomp\\analysis\\loader\\scripts\\verify_${t.key}_*.py). Try to REFUTE each claim; assume nothing.
2. Look for important things the report missed for this topic (original task below).
3. Edit the report in place: fix wrong statements directly in the text/tables, and append a section "## Verifikation" listing what you checked, what was confirmed, what you corrected and what remains uncertain. Keep machine-readable TSV files the report produced consistent with your fixes.
Original task given to the first agent, for reference:
${t.prompt}`,
    { label: `verify:${t.key}`, phase: 'Verify', schema: VERIFY_SCHEMA },
  ).then((v) => ({ topic: t.key, title: t.title, recon: r, verify: v })),
)

const done = results.filter(Boolean)
const missingTopics = TOPICS.filter((t) => !done.find((d) => d.topic === t.key)).map((t) => t.key)
if (missingTopics.length) log(`Ohne Ergebnis: ${missingTopics.join(', ')}`)

phase('Design')
const digest = done.map((d) => `### ${d.title} (${d.recon.report_path})
Zusammenfassung: ${d.recon.summary}
Offene Fragen: ${d.recon.open_questions.join(' | ')}
Verifikation: ${d.verify ? `${d.verify.confirmed}/${d.verify.checked} bestätigt, Vertrauen ${d.verify.confidence}; Korrekturen: ${d.verify.corrections.map((c) => c.claim + ' -> ' + c.fix).join(' | ')}; Fehlend: ${d.verify.missing.join(' | ')}` : 'keine Verifikation'}`).join('\n\n')

const design = await agent(
  `${CONTEXT}
You are the lead architect. Seven verified analysis reports are in D:\\FuturamaDecomp\\analysis\\loader\\ (kernel.md, cpu.md, d3d.md, avi.md, files.md, startup.md, renderer.md, plus TSV files). Digest:

${digest}

IMPORTANT: also read D:\\FuturamaDecomp\\analysis\\loader\\launcher_probe.md - it contains EMPIRICALLY TESTED results that override any assumption: VirtualAlloc at 0x10000 fails in a normal process (error 487); the working solution is a launcher EXE linked /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE (flags on the command line, not via #pragma) with a MASM placeholder segment ALIAS(".text$a") READ EXECUTE 'CODE' that the linker orders exactly at 0x11000-0x31B000; the PE header page at 0x10000 can be made RWX and overwritten with the XBE header (0x968 bytes); 0x80000000+ can be reserved with LAA. The host code lives in a DLL loaded by the launcher. Open point: SEH validation when the PE header is overwritten. Base the design on this.
Some reports may lack a "## Verifikation" section if their verification did not run; treat their claims with appropriate care and say so in the risk section.
Read all reports in full (they contain the verified details). Then write the loader design document D:\\FuturamaDecomp\\docs\\loader_design.md (German, technical identifiers in English). It must be concrete enough that implementation can start immediately:
1. Architektur: Host-Prozess (32 Bit, MSVC 14.44 x86 from VS 2022 Build Tools at C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools, Windows SDK 10.0.26100, CMake + Ninja available; clang-cl 23 also available), how the XBE gets mapped at 0x10000 (link the host with a high /BASE, /FIXED or not, /LARGEADDRESSAWARE, reserve address ranges early, possibly a tiny launcher stub), section mapping incl. bss, kernel thunk resolution, data imports, XBE header/certificate in memory (XAPI reads it at 0x10000), TLS setup, FS-segment strategy (with the concrete site list from cpu.md), threads.
2. HLE-Schnitt: decide per subsystem (Kernel, XAPI, D3D8/NiXBoxRenderer, DSOUND, XMV, XInput/XPP, File system, Saves) what gets implemented natively vs. kept from the original code vs. stubbed, with justification from the reports. Make a clear decision for the graphics boundary.
3. Hooking-Mechanik: how original functions get replaced (patch table: address -> replacement, 5-byte jmp detours, calling convention adapters for thiscall/fastcall), how decompiled game functions will later replace originals one by one (re3 style), and how to keep a mapping file in the repo.
4. Virtuelles Dateisystem and save game location.
5. Projektstruktur des Loader-Codes (directories, CMake targets), coding conventions, logging/tracing of unimplemented kernel calls.
6. Meilensteine with exit criteria, in order. Milestone 1 must be small: "XBE mapped, kernel thunks resolved with logging stubs, runs XapiInitProcess and _cinit, reaches the game's main()". Following milestones: file system + archives, first frame (renderer), input, audio, video, saves, playable level, then incremental decompilation and the Vulkan/D3D12 renderer.
7. Risiken und offene Fragen, with how to resolve each.
Keep it well structured; reference report sections instead of copying large tables. Return a short German summary (max 15 sentences) of the key design decisions and the milestone list.`,
  { label: 'design:loader', phase: 'Design' },
)

return { topics: done.map((d) => ({ topic: d.topic, report: d.recon.report_path, verify: d.verify && { checked: d.verify.checked, confirmed: d.verify.confirmed, corrections: d.verify.corrections.length, confidence: d.verify.confidence } })), missingTopics, design }
