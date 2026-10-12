export const meta = {
  name: 'futurama-m5-audio',
  description: 'Milestone M5: audio (DSOUND HLE on XAudio2: buffers, streams, Xbox ADPCM, 3D, I3DL2 reverb, volume categories), output capture for verification, launcher audio tab; analysis, implementation, integration, reviews, report',
  phases: [
    { title: 'Analyse', detail: 'game sound system and DSOUND contract' },
    { title: 'Implement', detail: 'DSOUND core, codecs/3D/reverb, tooling + launcher tab' },
    { title: 'Integrate', detail: 'sound in frontend and level 1-1, capture analysis' },
    { title: 'Review', detail: 'audio correctness and robustness/threading' },
    { title: 'Fix', detail: 'findings, docs, report' },
  ],
}

const CONTEXT = `
PROJECT: D:\\FuturamaDecomp (git repo, branch main, remote origin = private GitHub repo; do NOT commit or push - the user does that on request). Native PC port of the Xbox game "Futurama" (retail EU XBE) via a loader: futurama.exe (x86 stub at 0x10000) + futurama_host.dll (HLE kernel, XDK replacements, NiPCRenderer on D3D11, 60 Hz logic, pacing/interpolation up to 120 fps, XPP input HLE) + Qt launcher (launcher\\, 64-bit, schema-driven via futurama.exe --print-config-schema).
STATE: M1-M4 done (reports docs\\m1_report.md .. m4_report.md). The game reaches and plays Level 1-1 (scripted via [debug] input_script, tools\\m4_check.py); the user tested with a real Xbox controller: controller, rumble and picture fine, but NO AUDIO (DSOUND entry points are silent stubs: 40 entries + 5 XMediaObject stream vtable methods, generated from analysis\\loader\\avi_api_surface.tsv, see docs\\m2_report.md section 5) and no movies (M6, not in scope; but keep the 2 DSOUND entries used only by XMV working enough that M6 can build on them).
SCOPE (docs\\ROADMAP.md "M5 - Audio"): DSOUND on XAudio2, Xbox ADPCM, streaming music, 3D sound, I3DL2 reverb; volume categories; audio tab in the launcher. Read analysis\\loader\\avi.md (DSOUND sections: API surface, SoundManager 0x7c bytes at app+0xf0, SoundData, SoundVoice, RIFF WAVs from files/NiUDSFileObject, static buffers mostly Xbox ADPCM 22 kHz mono, music streamed PCM16 stereo in 10 packets x 0.3 s, DS3D deferred with own 6-point rolloff, HRTF/crosstalk mixbins, 5 I3DL2 presets from dsstdfx.bin, volume categories 0/1/2, and the traps: after Stop GetStatus must report 0 immediately, stream packet status must be set to 0 (XMEDIAPACKET pdwStatus/completed size/hCompletionEvent), etc.), docs\\loader_design.md (E7, E10: host worker threads never run guest code and may only write status DWORDs / signal events; 5.x conventions; section 8 deviations), src\\INTERFACES.md, docs\\m4_report.md.
RULES FOR GAME DATA: game audio, captured output and decoded samples stay under D:\\FuturamaDecomp\\run\\<your-name>\\ (git-ignored); never copy game sounds into the repo or tests - unit tests use synthetic signals only. ISO (read-only) via INI [paths] iso = Y:\\xbox games\\Futurama (Jun 17, 2003 prototype)\\Futurama (Europe) (En,Fr,De,Es,It).iso; tools\\xdvdfs.py and tools\\futimg.py extract files into run\\ for analysis.
VERIFICATION WITHOUT EARS: implement [audio] capture_wav = <file> (the final XAudio2 mix also written to a WAV via a capture/mastering tap or a parallel software mix of the same submix graph) and verify by analysis: compare captured segments with the original sound files (decoded independently, e.g. PyAV/ffmpeg 'adpcm_ima_xbox' / own reference decoder) by cross-correlation (pitch/speed/time alignment), RMS levels per category, clipping, dropouts/underruns in the music stream, stereo placement. The user will judge by ear afterwards.
EVIDENCE: Ghidra snapshot (never open Ghidra): D:\\FuturamaDecomp\\analysis\\retail_eu_export\\decomp\\*.c, functions.tsv, callgraph.tsv, strings.tsv; Python + capstone + PyAV; tools\\xbe.py.
TOOLCHAIN: game: cmd /c "D:\\FuturamaDecomp\\build.cmd" <builddir> <Debug|RelWithDebInfo> (FULL path; clang via ... clang); launcher: cmd /c "D:\\FuturamaDecomp\\build_launcher.cmd" <builddir> <Debug|Release> [test]; tools\\package.py. XAudio2 2.9 + X3DAudio + XAPOFX/reverb from the Windows SDK 10.0.26100. Checks: tools\\m1_check.py, m2_check.py, m3_check.py, m3c_check.py, m3d_check.py, m4_check.py (commands in the reports; the 120 Hz TV may be off: use --display-optional / documented flags and say so). futurama.exe is a GUI exe. No Futurama artwork in the repo or launcher.
CONVENTIONS: INTERFACES.md / design 5.3 (English code+comments, German docs/UI texts with English alternatives, noexcept guest entries, no inline asm, zero warnings incl. FUT_WERROR/LAUNCHER_WERROR, generated files --check, every config key in the single config schema with de/en labels).
`

const ANA_SCHEMA = { type: 'object', properties: { report_path: { type: 'string' }, summary: { type: 'string' }, recommendations: { type: 'array', items: { type: 'string' } }, risks: { type: 'array', items: { type: 'string' } } }, required: ['report_path', 'summary', 'recommendations', 'risks'] }

phase('Analyse')
const ana = await agent(`${CONTEXT}
TASK: AUDIO ANALYSIS (read-only for src\\/launcher\\; scripts analysis\\loader\\scripts\\m5_*.py; extracted data only under run\\analyse_m5\\).
Determine exactly, from the decomp and the data: (1) the DSOUND contract of every entry point and stream method the game (and XMV) calls - arguments, structures (DSBUFFERDESC, WAVEFORMATEX incl. Xbox ADPCM 0x0069 with wSamplesPerBlock 64 / nBlockAlign 36 per channel, DSSTREAMDESC with callbacks?, XMEDIAPACKET, DS3DBUFFER/DS3DLISTENER, DSMIXBINS, DSI3DL2LISTENER/BUFFER, DSENVELOPEDESC/DSFILTERDESC if used), units (volume in hundredths of dB, pitch/frequency, headroom, distance factors), status semantics, and which calls happen per tick vs per event; (2) the game's sound system design (SoundManager/SoundData/SoundVoice, voice limits, priorities, looping, streaming music packet flow, volume categories 0/1/2 and the options menu sliders, 3D listener update, rolloff curve, mixbin usage, the 5 I3DL2 presets and when they change), (3) all audio data formats on the disc (scan the archives: counts per format/rate/channels, ADPCM vs PCM, loop points in RIFF smpl chunks?), and where voice/dialogue lines live (sounds\\localized\\<lang>\\...), (4) what XMV needs from DSOUND (for M6). Write analysis\\loader\\m5_audio.md (German, evidence per claim) with a concrete mapping to XAudio2/X3DAudio/reverb (submix graph, category submixes, 3D DSP settings, I3DL2 -> XAUDIO2FX_REVERB_PARAMETERS via ReverbConvertI3DL2ToNative), threading model and a verification plan.`,
  { label: 'analyse:audio', phase: 'Analyse', schema: ANA_SCHEMA })
const anaDigest = ana ? `${ana.report_path}: ${ana.summary}\nEmpfehlungen: ${ana.recommendations.join(' | ')}\nRisiken: ${ana.risks.join(' | ')}` : 'analysis failed - do it yourself as needed'

const MOD_SCHEMA = { type: 'object', properties: { module: { type: 'string' }, compiles: { type: 'boolean' }, implemented: { type: 'array', items: { type: 'string' } }, shared_changes: { type: 'array', items: { type: 'string' } }, open_points: { type: 'array', items: { type: 'string' } }, summary: { type: 'string' } }, required: ['module', 'compiles', 'implemented', 'shared_changes', 'open_points', 'summary'] }
const RULES = `
RULES: three agents IN PARALLEL with disjoint files; shared files (CMakeLists.txt, build*.cmd, src\\shared\\**, core\\config*.*, core\\config_schema.*, core\\guest.h, src\\INTERFACES.md, docs\\*) only ADDITIVE changes with a changelog entry in src\\INTERFACES.md. Own build dirs build\\m5-<module>*. Shared interface: src\\host\\audio\\audio_api.h (first writer defines, others extend additively): an AudioEngine (XAudio2 device, mastering voice, category submixes music/sfx/voice/movie, reverb submix, capture tap), a SourceVoice wrapper, a Decoder interface (PCM8/16, Xbox ADPCM), and a Spatializer interface. Analysis:
${anaDigest}`

phase('Implement')
const impls = await parallel([
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE DSOUND CORE AGENT. You own src\\host\\audio\\dsound_*.* (replace the silent stubs; keep the generated hook table, switch entries to real implementations), audio_engine.*, tests\\audio_dsound_*.cpp. Implement per analysis\\loader\\m5_audio.md: DirectSoundCreate + device object, static buffers (create, SetBufferData/lock semantics, Play/Stop/looping, GetStatus with the exact required timing, SetVolume/SetFrequency/SetPitch/SetHeadroom/SetMixBins/SetPosition-related 2D parts), streams (XMediaObject Process/Flush/Discontinuity/GetStatus/GetInfo, packet queue with completion: write *pdwStatus/*pdwCompletedSize and signal hCompletionEvent through the kernel event objects - only from the guest thread or via the allowed status-write/event path of design E10), DirectSoundDoWork, category routing to submixes, lifetime/refcounts, device loss/no audio device (silent but functional), [audio] enabled/device/master volume keys in the schema. Unit tests with fake XAudio2 or the real engine on synthetic data.`,
    { label: 'impl:dsound', phase: 'Implement', schema: MOD_SCHEMA }),
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE CODEC/3D/REVERB AGENT. You own src\\host\\audio\\codec_*.*, spatial_*.*, reverb_*.*, tests\\audio_codec_*.cpp, tests\\audio_spatial_*.cpp. Implement: Xbox ADPCM decoder (IMA ADPCM, 36-byte blocks per channel, 64 samples, mono/stereo interleaving per Xbox layout; bit-exact against PyAV/ffmpeg 'adpcm_ima_xbox' on synthetic and - in run\\ only - real game files), PCM8/16 conversion, resampling handled by XAudio2 (frequency ratios); 3D: X3DAudio with the game's listener/emitter data (DS3D deferred settings committed at CommitDeferredSettings), the game's custom 6-point rolloff curve (X3DAUDIO_DISTANCE_CURVE), doppler as the game sets it, cone if used, HRTF/crosstalk mixbins mapped sensibly to stereo/5.1 output; I3DL2 reverb: the 5 presets from dsstdfx.bin (parse the file from the ISO at runtime, no copy in the repo) converted with ReverbConvertI3DL2ToNative, per-buffer send levels; [audio] reverb on/off, output layout (stereo/5.1/auto), 3D on/off in the schema. Unit tests with synthetic data.`,
    { label: 'impl:codec3d', phase: 'Implement', schema: MOD_SCHEMA }),
  () => agent(`${CONTEXT}${RULES}
YOU ARE THE TOOLING/LAUNCHER AGENT. You own src\\host\\audio\\capture_*.*, tools\\m5_check.py, tools\\audio_compare.py, launcher\\** changes (Audio tab), tests\\audio_capture_*.cpp. Implement [audio] capture_wav = <file> (exact copy of the final mix, 48 kHz float or 16-bit, with per-category RMS statistics logged at exit) and tools\\audio_compare.py (align captured audio against original sound files decoded independently; report pitch ratio, time offset, correlation, level). tools\\m5_check.py: frontend run (music stream plays continuously without underruns, menu sounds on scripted navigation) and a Level 1-1 run via the M4 input script (sfx, voice lines, ambience; 3D panning when the camera turns), checks: XAudio2 initialised, voices created/started/stopped counts plausible, no stuck voices (GetStatus traps), capture non-silent with category RMS in plausible ranges, at least N matched original sounds with pitch ratio 1.00 +-0.01 and correlation >= threshold, no clipping, no guest hangs, all M4 criteria. Launcher: tab "Audio" (output device from the system, master volume, output layout, reverb, 3D, test sound button that plays a generated tone - no game data), built from the schema. Qt tests; screenshots and VIEW them.`,
    { label: 'impl:tooling', phase: 'Implement', schema: MOD_SCHEMA }),
])
const implDigest = impls.filter(Boolean).map((m) => `[${m.module}] compiles=${m.compiles}; ${m.implemented.join(', ')}; shared: ${m.shared_changes.join(' | ')}; open: ${m.open_points.join(' | ')}`).join('\n')

const VER_SCHEMA = { type: 'object', properties: { checks: { type: 'array', items: { type: 'object', properties: { check: { type: 'string' }, status: { type: 'string', enum: ['bestanden', 'teilweise', 'gescheitert'] }, evidence: { type: 'string' } }, required: ['check', 'status', 'evidence'] } }, audio_result: { type: 'string', description: 'what is audible where, measured evidence' }, open_problems: { type: 'array', items: { type: 'string' } } }, required: ['checks', 'audio_result', 'open_problems'] }

phase('Integrate')
const ver = await agent(`${CONTEXT}
PHASE 3: INTEGRATION. You own all files. Implementation results:
${implDigest}
Integrate the three parts; build game (Debug, RelWithDebInfo, clang) and launcher (Debug, Release) without warnings; all unit tests green; regressions m1..m4 (display-optional where needed). Then make sound work in the frontend (music + menu sounds) and in Level 1-1 (sfx, voices, ambience, 3D, reverb): run m5_check, analyse captures with audio_compare.py, fix the root causes of silence, wrong pitch/speed, stuck voices, dropouts, hangs (GetStatus/packet status traps), clicks at loop points, wrong volumes per category. Also test without an audio device (disabled endpoint) and with audio disabled in the config. Record deviations in docs\\loader_design.md section 8 (German). Return structured status with measured evidence.`,
  { label: 'integrate', phase: 'Integrate', schema: VER_SCHEMA })
const verDigest = ver ? `checks: ${ver.checks.map((c) => `${c.check}: ${c.status} (${c.evidence})`).join(' | ')}; audio: ${ver.audio_result}; open: ${ver.open_problems.join(' | ')}` : 'integration failed'

const FIND_SCHEMA = { type: 'object', properties: { findings: { type: 'array', items: { type: 'object', properties: { file: { type: 'string' }, severity: { type: 'string', enum: ['kritisch', 'hoch', 'mittel', 'niedrig'] }, problem: { type: 'string' }, evidence: { type: 'string' }, suggested_fix: { type: 'string' } }, required: ['file', 'severity', 'problem', 'evidence', 'suggested_fix'] } }, summary: { type: 'string' } }, required: ['findings', 'summary'] }

phase('Review')
const reviews = await parallel([
  () => agent(`${CONTEXT}
REVIEW A - AUDIO CORRECTNESS (do NOT edit src\\/launcher\\; own build dir build\\review-m5-audio; runs under run\\review_m5_audio\\). Status: ${verDigest}
Verify against the decomp and the data: units/conversions (dB, pitch, frequency, headroom, mixbin volumes), ADPCM bit-exactness, loop handling, stream packet semantics and timing, 3D math (coordinate system/handedness of NetImmerse vs X3DAudio, listener orientation, rolloff curve, doppler), reverb presets and send levels, category volumes vs the game's options menu. Re-measure with your own captures. Report findings with evidence.`,
    { label: 'review:audio', phase: 'Review', schema: FIND_SCHEMA }),
  () => agent(`${CONTEXT}
REVIEW B - ROBUSTNESS, THREADING, PERFORMANCE, LAUNCHER (do NOT edit; own build dirs build\\review-m5-robust*; runs under run\\review_m5_robust\\). Status: ${verDigest}
Check the threading rules (no guest code on XAudio2 threads, races on status DWORDs/events, shutdown order), long runs (memory, voice leaks, 10-minute level run), device changes (default device switch, unplug headset, no device), CPU cost, latency, the launcher Audio tab (render and VIEW screenshots; de/en wording; test sound; device list), INI round trip. Report findings with evidence.`,
    { label: 'review:robust', phase: 'Review', schema: FIND_SCHEMA }),
])
const allFindings = reviews.filter(Boolean).flatMap((r) => r.findings)
log(`Review: ${allFindings.length} Befunde`)

phase('Fix')
const final = await agent(`${CONTEXT}
PHASE 5: FIX AND REPORT. You own all files. Status: ${verDigest}
Review findings (verify each first; reject with reason if it does not hold):
${allFindings.map((f, i) => `${i + 1}. [${f.severity}] ${f.file}: ${f.problem} | evidence: ${f.evidence} | fix: ${f.suggested_fix}`).join('\n')}
Fix confirmed findings; rebuild all (no warnings); rerun unit tests, launcher tests, m1..m4 checks (display-optional if the TV is off - say so) and m5_check; package dist\\FuturamaPC. Write docs\\m5_report.md (German): audio architecture, what plays where (measured), how to configure (launcher Audio tab, config keys), verification method and results, what the user should listen for (a short listening checklist for the frontend and Level 1-1), accepted/rejected findings, known issues, next steps (M3e render-to-texture etc., M6 video incl. movie audio via the DSOUND parts XMV uses). Update docs\\ROADMAP.md (M5 erledigt, M3e als Naechstes), README.md, INTERFACES.md. Do NOT commit. Return a short German summary (max 12 sentences).`,
  { label: 'fix+report', phase: 'Fix' })

return { analysis: ana && ana.report_path, impl: impls.map((m) => m && { module: m.module, compiles: m.compiles }), integration: ver && { checks: ver.checks.map((c) => `${c.check}: ${c.status}`), audio: ver.audio_result }, findings: allFindings.length, final }
