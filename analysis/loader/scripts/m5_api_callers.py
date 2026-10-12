"""Call sites of the game's sound API wrappers (0xc97d0..0xc9d30) and of SoundMgr helpers.

Output: scripts/out/m5_api_callers.out (counts per wrapper, sites grouped by calling function).
"""
from collections import defaultdict
from m5_common import xrefs, owner, name_of, Tee, out_path

API_NAMES = {
    0xc97b0: "Snd_CreateFromFileObject(fileobj)",
    0xc97d0: "Snd_GetSound(name,bStream)",
    0xc97f0: "Snd_ReleaseSound(data)",
    0xc9830: "Snd_HasVoices(data)",
    0xc9870: "Snd_IsVoiceAlive(id)",
    0xc98a0: "Snd_GetDuration(data)",
    0xc98e0: "Snd_ResumeVoice(id,loop)",
    0xc9910: "Snd_Play2D_Sfx(data,loop)->id",
    0xc9970: "Snd_Play2D_Music(data,loop)->id",
    0xc99d0: "Snd_Play2D_Speech(data,loop)->id",
    0xc9a30: "Snd_Play3DAt(data,pos,loop)->id (max 8/data)",
    0xc9a80: "Snd_StopVoice(id)",
    0xc9ab0: "Snd_StopAll()",
    0xc9ae0: "Snd_StopAllOf(data)",
    0xc9b10: "Snd_PauseVoice(id)",
    0xc9b40: "Snd_SetPitch(id,f)",
    0xc9b70: "Snd_SetPan(id,f)",
    0xc9ba0: "Snd_GetVolume(id)",
    0xc9be0: "Snd_SetVolume(id,f)",
    0xc9c10: "Snd_SetMaxDistance(id,f)",
    0xc9c60: "Snd_SetMinDistance(id,f)",
    0xc9cb0: "Snd_FadeOut(id,t) [no audible effect]",
    0xc96e0: "Snd_SetEnvironmentByName(name)",
}
P = Tee(out_path("m5_api_callers.out"))
tot = 0
for a, nm in sorted(API_NAMES.items()):
    sites = [r[0] for r in xrefs(a, kinds=("call",))]
    jmps = [r[0] for r in xrefs(a, kinds=("call",)) if r[2] == "jmp"]
    by = defaultdict(list)
    for s in sites:
        o = owner(s)
        by[name_of(o) if o else "nofunc"].append(s)
    tot += len(sites)
    P("%08x %-46s %4d sites in %3d functions" % (a, nm, len(sites), len(by)))
    for fn, ss in sorted(by.items()):
        P("        %-28s %s" % (fn, " ".join("%x" % s for s in ss)))
P("total sites:", tot)
