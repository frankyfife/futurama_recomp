"""avi_make_tsv.py - build avi_api_surface.tsv (one row per DSOUND/XMV/XPP entry point the game
calls) from avi_rawcalls.out + avi_callconv.out, plus the indirect XMediaObject vtable calls and the
recommended host implementation (hand-written mapping below; see avi.md for reasoning)."""
import re, collections, os
L = r"D:\FuturamaDecomp\analysis\loader"
rows = []; cur = None
for line in open(os.path.join(L, "avi_rawcalls.out")):
    m = re.match(r"([0-9a-f]{8}) (\S+)\s+\[(\w+)\] sites=(\d+)", line)
    if m:
        cur = dict(addr=m.group(1), name=m.group(2), sec=m.group(3), sites=int(m.group(4)), text=0, xmv=0, callers=[]); rows.append(cur); continue
    m = re.match(r"\s+(call|jmp) @([0-9a-f]{8}) in (.*)", line)
    if m and cur:
        a = int(m.group(2), 16)
        cur["xmv" if 0x264360 <= a < 0x28bee0 else "text"] += 1
        c = m.group(3)
        cur["callers"].append(c.split()[0] if not c.startswith("NO-") else "nofunc@" + m.group(2))
conv = {l.split()[0]: l.split(None, 2)[2].strip() for l in open(os.path.join(L, "avi_callconv.out"))}
XMV_NAMES = {  # Vermutung: names from behaviour/arg count (see avi.md section 2)
 "00264360": "XMVDecoder_CreateDecoderForPackets?", "00264677": "XMVDecoder_CloseDecoder?",
 "00264759": "XMVDecoder_GetVideoDescriptor?", "002647c6": "XMVDecoder_EnableAudioStream?",
 "00264943": "XMVDecoder_GetAudioStream? (AddRef'd IDirectSoundStream*)", "00264962": "XMVDecoder_GetNextFrame?",
 "00264fdf": "XMVDecoder_GetPlaybackTimeMs?"}
HOST_OVERRIDE = {  # added during verification (see avi.md "## Verifikation")
 "DSOUND::DirectSoundCreate": "XAudio2 engine + mastering voice; singleton with refcount (XMV calls Create/Release per sync)",
 "DSOUND::IDirectSound_Release": "refcount only; never tear down the device (singleton)",
 "DSOUND::IDirectSound_DownloadEffectsImage": "stub (S_OK); game ignores HRESULT and desc and frees the image right after; XBOX\\dsstdfx.bin must still be openable or the 3D init is skipped",
 "DSOUND::IDirectSound_SynchPlayback": "no-op while XMV is replaced; with original XMV: start all streams paused via Pause(2)",
 "DSOUND::DirectSoundCreateBuffer": "XAudio2 source voice; for ADPCM (lpwfx=SoundData+0x59) ignore nAvgBytesPerSec (game computes 36*ch*ch/64)",
 "DSOUND::IDirectSoundBuffer_GetStatus": "bit0 PLAYING; must report 0 right after Stop/SetBufferData(0,0) (destructor 0xcc030 spins otherwise)",
 "DSOUND::IDirectSoundStream_Pause": "0=run, 1=pause, 2=wait for SynchPlayback (original XMV only)",
 "DSOUND::IDirectSoundStream_FlushEx": "drop queue, complete packets, GetStatus must drop 0x10000 (XMV close spins otherwise)",
 "XAPILIB::XInputSetState": "XInput/SDL rumble; never leave feedback.dwStatus at 0x3E5 (game only sends when != 0x3E5)",
 # added in verification round 2 (see avi.md "## Verifikation", Runde 2)
 "DSOUND::DirectSoundCreateStream": "XAudio2 source voice + packet queue (max 10); lpwfx=SoundData+0x34 (raw fmt, cbSize forced 0x12): PCM16 music AND Xbox ADPCM (localized speech via 0xd88d6 bStream=1, 19.2 s packets) -> decode ADPCM per packet; returned object needs vtable at +0 (see vtbl rows)",
}
def host(n, sec):
    if n in HOST_OVERRIDE: return HOST_OVERRIDE[n]
    if sec == "XMV": return "ffmpeg/libav (xmv demuxer, wmv2 + adpcm_ima_xbox), frames into game D3D surface (YUY2), audio via host DSOUND stream"
    if sec == "XPP": return "XInput (XInputGetState/SetState) or SDL3 Gamepad; port bitmask bookkeeping"
    if "I3DL2" in n: return "XAudio2 reverb submix (XAUDIO2FX_I3DL2_PARAMETERS + ReverbConvertI3DL2ToNative)"
    if any(k in n for k in ("Position", "Velocity", "Orientation", "Distance", "Rolloff", "Doppler", "HRTF", "CommitDeferred")):
        return "X3DAudio (deferred: apply on CommitDeferredSettings)"
    if "DownloadEffectsImage" in n: return "stub (S_OK, image ignored)"
    if "DoWork" in n or "SynchPlayback" in n: return "no-op / pump host audio"
    return "XAudio2 source voice (PCM, Xbox ADPCM decoded on host)"
with open(os.path.join(L, "avi_api_surface.tsv"), "w", encoding="utf-8") as f:
    f.write("address\tsection\tname\tcallconv\tsites_game\tsites_xmv\tcallers\thost_impl\n")
    for r in rows:
        if r["addr"] == "0024aebe": continue  # linker this-adjust thunk at 0xf9cbd, not a game call
        n = XMV_NAMES.get(r["addr"], r["name"])
        f.write("\t".join([r["addr"], r["sec"], n, "stdcall " + conv.get(r["addr"], "?"), str(r["text"]), str(r["xmv"]),
                           ",".join(sorted(set(r["callers"]))), host(n, r["sec"])]) + "\n")
    for off, meth, who in ((0x0, "XMediaObject::AddRef", "XMV 00264943, 002647c6 (if ppStream)"), (0x4, "XMediaObject::Release", "game 000ee220 (0xee47d), game 000cc030 (0xcc107), XMV 00264677/002647c6"),
                           (0xc, "XMediaObject::GetStatus", "XMV 00264677 (tests 0x10000)"), (0x10, "XMediaObject::Process", "game 000cb6b7, XMV 00264962"),
                           (0x14, "XMediaObject::Discontinuity", "XMV 00264962")):
        note = "host stream object must expose same vtable layout"
        if off == 0x10: note += "; Process must set *pdwStatus=0x8000000A (E_PENDING) synchronously and 0 on completion (game only polls, 0xcb5d4; original does the same in FUN_0024b1c4 @0x24b1d9)"
        f.write("vtbl+%#x\tDSOUND\t%s (IDirectSoundStream vtable 0x2a5f5c)\tstdcall(this on stack)\t-\t-\t%s\t%s\n" % (off, meth, who, note))
print("rows:", len(rows) - 1 + 5)
