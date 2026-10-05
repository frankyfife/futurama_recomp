// GENERATED FILE - DO NOT EDIT.
// Generator: tools/gen/gen_xdk_entries.py (run with --check to verify).
// Sources:   analysis/loader/avi_api_surface.tsv, analysis/retail_eu_export/functions.tsv (Ghidra names)
//
// XDK library entry points the game calls (design E7, Anhang B; avi.md 2.2, 3.1, 4.1). All are
// __stdcall; purgeBytes = `ret N` of the original = size of the argument block (avi_callconv.out,
// re-verified with capstone against the XBE by the generator). Consumed by audio/xdk_entries.h:
//   XDK_ENTRY(id, address, lib, hookName, purgeBytes, flags, sitesGame, sitesXmv, ghidraName, callers)
//   XDK_STREAM_METHOD(id, slot, purgeBytes, inTsv)  - XMediaObject vtable 0x2A5F5C of CDirectSoundStream
// flags: kFlagJmpThunk = the entry is a 5-byte jmp to the implementation (the hook replaces the jmp),
//        kFlagNameAssumed = name derived from behaviour (XMV has no symbols, avi.md 3.1).
// sitesGame/sitesXmv: call sites in game code / inside the XMV library (avi_rawcalls.out).
#ifndef XDK_ENTRY
#define XDK_ENTRY(id, address, lib, hookName, purgeBytes, flags, sitesGame, sitesXmv, ghidraName, callers)
#endif
#ifndef XDK_STREAM_METHOD
#define XDK_STREAM_METHOD(id, slot, purgeBytes, inTsv)
#endif
// 54 entries: DSOUND 40, XMV 7, XPP 7
XDK_ENTRY(IDirectSound_Release, 0x00246E7Du, Dsound, "DSOUND::IDirectSound_Release", 4, 0, 1, 1, "DSOUND::IDirectSound_Release", "000cbc30,00264962")
XDK_ENTRY(IDirectSoundBuffer_Release, 0x00246E93u, Dsound, "DSOUND::IDirectSoundBuffer_Release", 4, 0, 1, 0, "DSOUND::IDirectSoundBuffer_Release", "000cc030")
XDK_ENTRY(DirectSoundUseFullHRTF, 0x00246EA9u, Dsound, "DSOUND::DirectSoundUseFullHRTF", 0, 0, 1, 0, "DSOUND::DirectSoundUseFullHRTF", "000ccd00")
XDK_ENTRY(IDirectSound_DownloadEffectsImage, 0x00247911u, Dsound, "DSOUND::IDirectSound_DownloadEffectsImage", 20, 0, 1, 0, "DSOUND::IDirectSound_DownloadEffectsImage", "000cbeb0")
XDK_ENTRY(IDirectSound_SynchPlayback, 0x00247938u, Dsound, "DSOUND::IDirectSound_SynchPlayback", 4, 0, 0, 1, "DSOUND::IDirectSound_SynchPlayback", "00264962")
XDK_ENTRY(IDirectSoundBuffer_SetVolume, 0x00247950u, Dsound, "DSOUND::IDirectSoundBuffer_SetVolume", 8, 0, 1, 0, "DSOUND::IDirectSoundBuffer_SetVolume", "nofunc@000cc91d")
XDK_ENTRY(IDirectSoundBuffer_SetMixBins, 0x0024796Cu, Dsound, "DSOUND::IDirectSoundBuffer_SetMixBins", 8, 0, 2, 0, "DSOUND::IDirectSoundBuffer_SetMixBins", "000cc150,nofunc@000cc9d3")
XDK_ENTRY(IDirectSoundBuffer_Play, 0x00247988u, Dsound, "DSOUND::IDirectSoundBuffer_Play", 16, 0, 4, 0, "DSOUND::IDirectSoundBuffer_Play", "000cc390,000cc410,000cc4b0,nofunc@000cc72e")
XDK_ENTRY(IDirectSoundBuffer_Stop, 0x002479ACu, Dsound, "DSOUND::IDirectSoundBuffer_Stop", 4, 0, 1, 0, "DSOUND::IDirectSoundBuffer_Stop", "000cb540")
XDK_ENTRY(IDirectSoundBuffer_GetStatus, 0x002479C4u, Dsound, "DSOUND::IDirectSoundBuffer_GetStatus", 8, 0, 2, 0, "DSOUND::IDirectSoundBuffer_GetStatus", "000cc030,nofunc@000cca27")
// DSOUND::IDirectSoundStream_SetVolume: jmp -> DSOUND::CDirectSoundStream_SetVolume
XDK_ENTRY(IDirectSoundStream_SetVolume, 0x002479E0u, Dsound, "DSOUND::IDirectSoundStream_SetVolume", 8, kFlagJmpThunk, 2, 0, "DSOUND::IDirectSoundStream_SetVolume", "000ee220,nofunc@000cc8c4")
// DSOUND::IDirectSoundStream_SetMixBins: jmp -> DSOUND::CDirectSoundStream_SetMixBins
XDK_ENTRY(IDirectSoundStream_SetMixBins, 0x002479E5u, Dsound, "DSOUND::IDirectSoundStream_SetMixBins", 8, kFlagJmpThunk, 2, 0, "DSOUND::IDirectSoundStream_SetMixBins", "000cc150,nofunc@000cc9c0")
// DSOUND::IDirectSoundStream_Pause: jmp -> DSOUND::CDirectSoundStream_Pause
XDK_ENTRY(IDirectSoundStream_Pause, 0x002479EAu, Dsound, "DSOUND::IDirectSoundStream_Pause", 8, kFlagJmpThunk, 5, 1, "DSOUND::IDirectSoundStream_Pause", "000cb540,000cc390,000cc410,000cc4b0,00264962,nofunc@000cc711")
XDK_ENTRY(IDirectSoundStream_FlushEx, 0x002479EFu, Dsound, "DSOUND::IDirectSoundStream_FlushEx", 16, 0, 0, 1, "DSOUND::IDirectSoundStream_FlushEx", "00264677")
XDK_ENTRY(DirectSoundDoWork, 0x00247A07u, Dsound, "DSOUND::DirectSoundDoWork", 0, 0, 1, 2, "DSOUND::DirectSoundDoWork", "000cbcb0,00264677,00264962")
XDK_ENTRY(IDirectSound_CommitDeferredSettings, 0x00248380u, Dsound, "DSOUND::IDirectSound_CommitDeferredSettings", 4, 0, 1, 0, "DSOUND::IDirectSound_CommitDeferredSettings", "000cbcb0")
XDK_ENTRY(IDirectSoundBuffer_SetFrequency, 0x00248398u, Dsound, "DSOUND::IDirectSoundBuffer_SetFrequency", 8, 0, 1, 0, "DSOUND::IDirectSoundBuffer_SetFrequency", "nofunc@000cc910")
XDK_ENTRY(IDirectSoundBuffer_SetMaxDistance, 0x002483B4u, Dsound, "DSOUND::IDirectSoundBuffer_SetMaxDistance", 12, 0, 2, 0, "DSOUND::IDirectSoundBuffer_SetMaxDistance", "nofunc@000cc6ce,nofunc@000cc83e")
XDK_ENTRY(IDirectSoundBuffer_SetMinDistance, 0x002483D8u, Dsound, "DSOUND::IDirectSoundBuffer_SetMinDistance", 12, 0, 2, 0, "DSOUND::IDirectSoundBuffer_SetMinDistance", "nofunc@000cc6b9,nofunc@000cc829")
XDK_ENTRY(IDirectSoundBuffer_SetPosition, 0x002483FCu, Dsound, "DSOUND::IDirectSoundBuffer_SetPosition", 20, 0, 2, 0, "DSOUND::IDirectSoundBuffer_SetPosition", "nofunc@000cc6df,nofunc@000cc7f7")
XDK_ENTRY(IDirectSoundBuffer_SetVelocity, 0x00248431u, Dsound, "DSOUND::IDirectSoundBuffer_SetVelocity", 20, 0, 2, 0, "DSOUND::IDirectSoundBuffer_SetVelocity", "nofunc@000cc6fc,nofunc@000cc814")
XDK_ENTRY(IDirectSoundBuffer_SetRolloffCurve, 0x00248466u, Dsound, "DSOUND::IDirectSoundBuffer_SetRolloffCurve", 16, 0, 1, 0, "DSOUND::IDirectSoundBuffer_SetRolloffCurve", "000cc150")
XDK_ENTRY(IDirectSoundBuffer_SetI3DL2Source, 0x0024848Au, Dsound, "DSOUND::IDirectSoundBuffer_SetI3DL2Source", 12, 0, 1, 0, "DSOUND::IDirectSoundBuffer_SetI3DL2Source", "nofunc@000cca0e")
// DSOUND::IDirectSoundStream_SetFrequency: jmp -> DSOUND::CDirectSoundStream_SetFrequency
XDK_ENTRY(IDirectSoundStream_SetFrequency, 0x002484AAu, Dsound, "DSOUND::IDirectSoundStream_SetFrequency", 8, kFlagJmpThunk, 1, 0, "DSOUND::IDirectSoundStream_SetFrequency", "nofunc@000cc8ba")
XDK_ENTRY(IDirectSoundStream_SetMaxDistance, 0x002484AFu, Dsound, "DSOUND::IDirectSoundStream_SetMaxDistance", 12, 0, 1, 0, "DSOUND::IDirectSoundStream_SetMaxDistance", "nofunc@000cc7e9")
XDK_ENTRY(IDirectSoundStream_SetMinDistance, 0x002484C7u, Dsound, "DSOUND::IDirectSoundStream_SetMinDistance", 12, 0, 1, 0, "DSOUND::IDirectSoundStream_SetMinDistance", "nofunc@000cc7d7")
XDK_ENTRY(IDirectSoundStream_SetPosition, 0x002484DFu, Dsound, "DSOUND::IDirectSoundStream_SetPosition", 20, 0, 1, 0, "DSOUND::IDirectSoundStream_SetPosition", "nofunc@000cc7ab")
XDK_ENTRY(IDirectSoundStream_SetVelocity, 0x00248508u, Dsound, "DSOUND::IDirectSoundStream_SetVelocity", 20, 0, 1, 0, "DSOUND::IDirectSoundStream_SetVelocity", "nofunc@000cc7c5")
// DSOUND::IDirectSoundStream_SetRolloffCurve: jmp -> DSOUND::CDirectSoundStream_SetRolloffCurve
XDK_ENTRY(IDirectSoundStream_SetRolloffCurve, 0x00248531u, Dsound, "DSOUND::IDirectSoundStream_SetRolloffCurve", 16, kFlagJmpThunk, 1, 0, "DSOUND::IDirectSoundStream_SetRolloffCurve", "000cc150")
XDK_ENTRY(IDirectSound_SetDistanceFactor, 0x00248D60u, Dsound, "DSOUND::IDirectSound_SetDistanceFactor", 12, 0, 1, 0, "DSOUND::IDirectSound_SetDistanceFactor", "000ccd00")
XDK_ENTRY(IDirectSound_SetDopplerFactor, 0x00248D84u, Dsound, "DSOUND::IDirectSound_SetDopplerFactor", 12, 0, 1, 0, "DSOUND::IDirectSound_SetDopplerFactor", "000ccd00")
XDK_ENTRY(IDirectSound_SetOrientation, 0x00248DA8u, Dsound, "DSOUND::IDirectSound_SetOrientation", 32, 0, 1, 0, "DSOUND::IDirectSound_SetOrientation", "000cbcb0")
XDK_ENTRY(IDirectSound_SetPosition, 0x00248DF2u, Dsound, "DSOUND::IDirectSound_SetPosition", 20, 0, 2, 0, "DSOUND::IDirectSound_SetPosition", "000cbcb0,000ccd00")
XDK_ENTRY(IDirectSound_SetRolloffFactor, 0x00248E27u, Dsound, "DSOUND::IDirectSound_SetRolloffFactor", 12, 0, 1, 0, "DSOUND::IDirectSound_SetRolloffFactor", "000ccd00")
XDK_ENTRY(IDirectSound_SetVelocity, 0x00248E4Bu, Dsound, "DSOUND::IDirectSound_SetVelocity", 20, 0, 1, 0, "DSOUND::IDirectSound_SetVelocity", "000cbcb0")
XDK_ENTRY(IDirectSound_SetI3DL2Listener, 0x00248E80u, Dsound, "DSOUND::IDirectSound_SetI3DL2Listener", 12, 0, 6, 0, "DSOUND::IDirectSound_SetI3DL2Listener", "000ccd00,nofunc@000cbfbb,nofunc@000cbfce,nofunc@000cbfe1,nofunc@000cbff4,nofunc@000cc007")
XDK_ENTRY(IDirectSoundBuffer_SetBufferData, 0x00248EA0u, Dsound, "DSOUND::IDirectSoundBuffer_SetBufferData", 12, 0, 3, 0, "DSOUND::IDirectSoundBuffer_SetBufferData", "000cb580,000cc030,000cc150")
XDK_ENTRY(DirectSoundCreate, 0x0024935Cu, Dsound, "DSOUND::DirectSoundCreate", 12, 0, 1, 1, "DSOUND::DirectSoundCreate", "000ccd00,00264962")
XDK_ENTRY(DirectSoundCreateBuffer, 0x002493A3u, Dsound, "DSOUND::DirectSoundCreateBuffer", 8, 0, 1, 0, "DSOUND::DirectSoundCreateBuffer", "000cc150")
XDK_ENTRY(DirectSoundCreateStream, 0x002493FAu, Dsound, "DSOUND::DirectSoundCreateStream", 8, 0, 1, 1, "DSOUND::DirectSoundCreateStream", "000cc150,002647c6")
// XMVDecoder_CreateDecoderForPackets?
XDK_ENTRY(XMVDecoder_CreateDecoderForPackets, 0x00264360u, Xmv, "XMV::XMVDecoder_CreateDecoderForPackets", 24, kFlagNameAssumed, 1, 0, "FUN_00264360", "000ee220")
// XMVDecoder_CloseDecoder?
XDK_ENTRY(XMVDecoder_CloseDecoder, 0x00264677u, Xmv, "XMV::XMVDecoder_CloseDecoder", 4, kFlagNameAssumed, 1, 0, "FUN_00264677", "000ee080")
// XMVDecoder_GetVideoDescriptor?
XDK_ENTRY(XMVDecoder_GetVideoDescriptor, 0x00264759u, Xmv, "XMV::XMVDecoder_GetVideoDescriptor", 8, kFlagNameAssumed, 1, 0, "FUN_00264759", "000ee220")
// XMVDecoder_EnableAudioStream?
XDK_ENTRY(XMVDecoder_EnableAudioStream, 0x002647C6u, Xmv, "XMV::XMVDecoder_EnableAudioStream", 20, kFlagNameAssumed, 1, 0, "FUN_002647c6", "000ee220")
// XMVDecoder_GetAudioStream? (AddRef'd IDirectSoundStream*)
XDK_ENTRY(XMVDecoder_GetAudioStream, 0x00264943u, Xmv, "XMV::XMVDecoder_GetAudioStream", 12, kFlagNameAssumed, 1, 0, "FUN_00264943", "000ee220")
// XMVDecoder_GetNextFrame?
XDK_ENTRY(XMVDecoder_GetNextFrame, 0x00264962u, Xmv, "XMV::XMVDecoder_GetNextFrame", 16, kFlagNameAssumed, 1, 0, "FUN_00264962", "000ede90")
// XMVDecoder_GetPlaybackTimeMs?
XDK_ENTRY(XMVDecoder_GetPlaybackTimeMs, 0x00264FDFu, Xmv, "XMV::XMVDecoder_GetPlaybackTimeMs", 4, kFlagNameAssumed, 1, 0, "FUN_00264fdf", "000ede90")
XDK_ENTRY(XInputOpen, 0x0028CE5Cu, Xpp, "XPP::XInputOpen", 16, 0, 2, 0, "XAPILIB::XInputOpen", "0009c140,0009c2a0")
XDK_ENTRY(XInputClose, 0x0028CEB2u, Xpp, "XPP::XInputClose", 4, 0, 3, 0, "XAPILIB::XInputClose", "0009c140,0009c2a0")
XDK_ENTRY(XInputGetState, 0x0028CEBEu, Xpp, "XPP::XInputGetState", 8, 0, 1, 0, "XAPILIB::XInputGetState", "nofunc@0009c3e6")
XDK_ENTRY(XInputSetState, 0x0028CF2Au, Xpp, "XPP::XInputSetState", 8, 0, 1, 0, "XAPILIB::XInputSetState", "0009c220")
// XAPILIB::XInitDevices: jmp -> XAPILIB::XInitDevices
XDK_ENTRY(XInitDevices, 0x0028CFE1u, Xpp, "XPP::XInitDevices", 8, kFlagJmpThunk, 1, 0, "XAPILIB::XInitDevices", "0009c2a0")
XDK_ENTRY(XGetDevices, 0x0028CFE6u, Xpp, "XPP::XGetDevices", 4, 0, 2, 0, "XAPILIB::XGetDevices", "0009c2a0")
XDK_ENTRY(XGetDeviceChanges, 0x0028D008u, Xpp, "XPP::XGetDeviceChanges", 12, 0, 1, 0, "XAPILIB::XGetDeviceChanges", "0009c140")
// stream vtable 0x002A5F5C: 7 XMediaObject slots (5 called by the game/XMV, rows vtbl+0xN of the TSV)
XDK_STREAM_METHOD(AddRef, 0, 4, 1)
XDK_STREAM_METHOD(Release, 1, 4, 1)
XDK_STREAM_METHOD(GetInfo, 2, 8, 0)
XDK_STREAM_METHOD(GetStatus, 3, 8, 1)
XDK_STREAM_METHOD(Process, 4, 12, 1)
XDK_STREAM_METHOD(Discontinuity, 5, 4, 1)
XDK_STREAM_METHOD(Flush, 6, 4, 0)
#undef XDK_ENTRY
#undef XDK_STREAM_METHOD
