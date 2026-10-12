// DSOUND HLE (M5): the 40 DSOUND entry points the game and XMV call, on the audio engine of audio/audio_api.h
// (design E7; analysis/loader/m5_audio.md 2 contract, 6 mapping, 7 threading, 8 traps). Replaces the silent
// M2-M4 stubs; the generated hook table (gen/xdk_entries.gen.inc, XDK_HOOK) is unchanged.
//
// Objects (all guest-visible parts are small handles below 0x80000000; the game never reads them):
//   IDirectSound        singleton with a reference count, never torn down (XMV creates/releases it per sync).
//   IDirectSoundBuffer  one SourceVoice per buffer; SetBufferData copies/decodes at once (DSOUND does not copy,
//                       0x248B1E - the game may free its data afterwards); Play resumes at the remembered cursor,
//                       Stop remembers it (0x24DF9E, 0x24E04E); GetStatus is computed from host state and is 0 at
//                       once after Stop, SetBufferData(0,0) and the end of playback (0x24DB55).
//   IDirectSoundStream  guest object with the XMediaObject vtable at +0 (slots AddRef, Release, GetInfo, GetStatus,
//                       Process, Discontinuity, Flush); Process copies + decodes the packet and sets *pdwStatus =
//                       E_PENDING synchronously, a full queue returns 0x88780032 without touching the status
//                       (0x247676); finished packets are written (*pdwCompletedSize, *pdwStatus = 0, completion
//                       event or callback) only in DirectSoundDoWork on the guest thread.
// Levels: 2D voices by the DSOUND model of audio/dsound_mix.h, 3D voices by snd::Spatializer at
// CommitDeferredSettings. Categories from the probe on SoundVoice::CreateDsObject 0xCC150 (dsound_probe.cpp).
//
// Threads (design E10): every entry runs on the guest thread; the engine's mixer thread only records "buffer k
// ended" (host memory). No guest memory is written and no guest code is called from any other thread, and nothing
// is written after a stream's last Release (the game frees the packet memory right after it, 0xCC107 -> 0xCC121).
#pragma once

#include "audio/audio_api.h"
#include "core/log.h"

#include <cstdint>

namespace snd {

// ---- guest structures (XDK dsound.h layout) ---------------------------------------------------------------------
#pragma pack(push, 1)
// WAVEFORMATEX (18 bytes, packed; the game's copy lies at SoundData+0x34 resp. unaligned +0x59).
struct XWaveFormatEx {
    std::uint16_t wFormatTag;  // 1 PCM, 0x69 Xbox ADPCM
    std::uint16_t nChannels;
    std::uint32_t nSamplesPerSec;
    std::uint32_t nAvgBytesPerSec;  // wrong for the game's ADPCM buffer format, never used
    std::uint16_t nBlockAlign;      // recomputed
    std::uint16_t wBitsPerSample;
    std::uint16_t cbSize;  // forced to 0x12 by the game's WAV parser, never used
};
#pragma pack(pop)
static_assert(sizeof(XWaveFormatEx) == 18);

// DSBUFFERDESC (0xCC23F..0xCC292: dwSize 0x18, dwFlags (3D ? 0x60010 : 0) | 0x100, lpwfxFormat).
struct XDsBufferDesc {
    std::uint32_t dwSize;
    std::uint32_t dwFlags;
    std::uint32_t dwBufferBytes;
    std::uint32_t lpwfxFormat;  // guest XWaveFormatEx*
    std::uint32_t lpMixBins;    // guest XDsMixBins* (NULL from the game)
    std::uint32_t dwInputMixBin;
};
static_assert(sizeof(XDsBufferDesc) == 0x18);

// DSSTREAMDESC (0xCC18A..0xCC1C3: dwFlags, dwMaxAttachedPackets = 10, lpwfxFormat = SoundData+0x34).
struct XDsStreamDesc {
    std::uint32_t dwFlags;
    std::uint32_t dwMaxAttachedPackets;
    std::uint32_t lpwfxFormat;   // guest XWaveFormatEx*
    std::uint32_t lpfnCallback;  // XMV only (0x264775); the game passes NULL
    std::uint32_t lpvContext;
    std::uint32_t lpMixBins;
};
static_assert(sizeof(XDsStreamDesc) == 0x18);

// XMEDIAPACKET (pump 0xCB680..0xCB6B7: pdwStatus = &voice[0x7C + 4*i], hCompletionEvent 0).
struct XMediaPacket {
    std::uint32_t pvBuffer;
    std::uint32_t dwMaxSize;
    std::uint32_t pdwCompletedSize;  // guest DWORD* or 0
    std::uint32_t pdwStatus;         // guest DWORD* or 0
    std::uint32_t hCompletionEvent;  // event handle, or the packet context of a callback stream (XMV)
    std::uint32_t prtTimestamp;
};
static_assert(sizeof(XMediaPacket) == 0x18);

// XMEDIAINFO (XMediaObject::GetInfo).
struct XMediaInfo {
    std::uint32_t dwFlags;
    std::uint32_t dwInputSize;
    std::uint32_t dwOutputSize;
    std::uint32_t dwMaxLookahead;
};
static_assert(sizeof(XMediaInfo) == 0x10);

// DSMIXBINS {count, DSMIXBINVOLUMEPAIR*} and the pair {dwMixBin, lVolume} (0xCC93C..0xCC9D3).
struct XDsMixBinVolumePair {
    std::uint32_t dwMixBin;
    std::int32_t lVolume;
};
struct XDsMixBins {
    std::uint32_t dwMixBinCount;
    std::uint32_t lpMixBinVolumePairs;  // guest XDsMixBinVolumePair*
};
static_assert(sizeof(XDsMixBins) == 8);

// DSI3DL2LISTENER (48 bytes, table 0x2D6818) and DSI3DL2BUFFER (36 bytes, 0xCC9DF..0xCCA0E).
struct XDsI3dl2Listener {
    std::int32_t lRoom;
    std::int32_t lRoomHF;
    float flRoomRolloffFactor;
    float flDecayTime;
    float flDecayHFRatio;
    std::int32_t lReflections;
    float flReflectionsDelay;
    std::int32_t lReverb;
    float flReverbDelay;
    float flDiffusion;
    float flDensity;
    float flHFReference;
};
static_assert(sizeof(XDsI3dl2Listener) == 48);
struct XDsI3dl2Buffer {
    std::int32_t lDirect;
    std::int32_t lDirectHF;
    std::int32_t lRoom;
    std::int32_t lRoomHF;
    float flRoomRolloffFactor;
    std::int32_t lObstructionHF;
    float flObstructionLFRatio;
    std::int32_t lOcclusionHF;
    float flOcclusionLFRatio;
};
static_assert(sizeof(XDsI3dl2Buffer) == 36);

// ---- constants --------------------------------------------------------------------------------------------------
inline constexpr std::int32_t kS_OK = 0;
inline constexpr std::uint16_t kWaveFormatPcm = 1;
inline constexpr std::uint16_t kWaveFormatXboxAdpcm = 0x69;
// XMEDIAPACKET status values (m5_audio.md 2.4): E_PENDING set synchronously by Process (0x24B1D9; the pump refills a
// slot once *pdwStatus != E_PENDING, 0xCB5D4), 0 = played, E_ABORT = flushed/released (CMcpxStream_Flush 0x24BA46).
inline constexpr std::uint32_t kPacketPending = 0x8000000A;
inline constexpr std::uint32_t kPacketSuccess = 0;
inline constexpr std::uint32_t kPacketFlushed = 0x80004004;
// IDirectSoundBuffer_GetStatus (CMcpxBuffer_GetStatus 0x24DB55): 1 playing, +4 looping, 2 paused (never used).
inline constexpr std::uint32_t kBufferStatusPlaying = 0x1;
inline constexpr std::uint32_t kBufferStatusLooping = 0x4;
// Stream status (CMcpxStream_GetStatus 0x24B3CE): READY = the queue has room, PLAYING (XMV CloseDecoder 0x264677
// waits until it drops), PAUSED.
inline constexpr std::uint32_t kStreamStatusReady = 0x00000001;
inline constexpr std::uint32_t kStreamStatusPlaying = 0x00010000;
inline constexpr std::uint32_t kStreamStatusPaused = 0x00020000;
// IDirectSoundStream_Pause: 0 resume (0xCC711), 1 pause (0xCB563), 2 wait for SynchPlayback (XMV, 0x264A81).
inline constexpr std::uint32_t kStreamResume = 0;
inline constexpr std::uint32_t kStreamPause = 1;
inline constexpr std::uint32_t kStreamPauseSynch = 2;
// IDirectSoundBuffer_Play flags (0xCC72E: loop ? 1 : 0).
inline constexpr std::uint32_t kPlayLooping = 0x1;
// IDirectSoundStream_FlushEx flags (XMV CloseDecoder: 1 = asynchronous).
inline constexpr std::uint32_t kFlushExAsync = 0x1;
// DSBUFFERDESC / DSSTREAMDESC flags (m5_audio.md 2.3).
inline constexpr std::uint32_t kDsFlag3D = 0x10;
inline constexpr std::uint32_t kDsFlagMuteAtMaxDistance = 0x20000;
// HRESULTs.
inline constexpr std::int32_t kDsErrInvalidCall = static_cast<std::int32_t>(0x88780032);  // full stream queue
inline constexpr std::int32_t kE_InvalidArg = static_cast<std::int32_t>(0x80070057);
inline constexpr std::int32_t kE_OutOfMemory = static_cast<std::int32_t>(0x8007000E);
inline constexpr std::uint32_t kXmoFlagFixedSampleSize = 0x1;  // ASSUMPTION: XMO_STREAMF_FIXED_SAMPLE_SIZE

// ---- the 40 entry points (stdcall; evidence per function in dsound_hle.cpp) ----------------------------------------
std::uint32_t __stdcall IDirectSound_ReleaseHle(void* ds) noexcept;
std::uint32_t __stdcall IDirectSoundBuffer_ReleaseHle(void* buffer) noexcept;
void __stdcall DirectSoundUseFullHRTFHle() noexcept;
std::int32_t __stdcall IDirectSound_DownloadEffectsImageHle(void* ds, const void* image, std::uint32_t size,
                                                            const void* imageLoc,
                                                            std::uint32_t* ppImageDesc) noexcept;
std::int32_t __stdcall IDirectSound_SynchPlaybackHle(void* ds) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetVolumeHle(void* buffer, std::int32_t volume) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetMixBinsHle(void* buffer, const void* mixBins) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_PlayHle(void* buffer, std::uint32_t reserved1,
                                                  std::uint32_t reserved2, std::uint32_t flags) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_StopHle(void* buffer) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_GetStatusHle(void* buffer, std::uint32_t* status) noexcept;
std::int32_t __stdcall IDirectSoundStream_SetVolumeHle(void* stream, std::int32_t volume) noexcept;
std::int32_t __stdcall IDirectSoundStream_SetMixBinsHle(void* stream, const void* mixBins) noexcept;
std::int32_t __stdcall IDirectSoundStream_PauseHle(void* stream, std::uint32_t pause) noexcept;
std::int32_t __stdcall IDirectSoundStream_FlushExHle(void* stream, std::int64_t timeStamp,
                                                     std::uint32_t flags) noexcept;
void __stdcall DirectSoundDoWorkHle() noexcept;
std::int32_t __stdcall IDirectSound_CommitDeferredSettingsHle(void* ds) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetFrequencyHle(void* buffer, std::uint32_t frequency) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetMaxDistanceHle(void* buffer, float distance,
                                                            std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetMinDistanceHle(void* buffer, float distance,
                                                            std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetPositionHle(void* buffer, float x, float y, float z,
                                                         std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetVelocityHle(void* buffer, float x, float y, float z,
                                                         std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetRolloffCurveHle(void* buffer, const float* points,
                                                             std::uint32_t count,
                                                             std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetI3DL2SourceHle(void* buffer, const void* source,
                                                            std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundStream_SetFrequencyHle(void* stream, std::uint32_t frequency) noexcept;
std::int32_t __stdcall IDirectSoundStream_SetMaxDistanceHle(void* stream, float distance,
                                                            std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundStream_SetMinDistanceHle(void* stream, float distance,
                                                            std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundStream_SetPositionHle(void* stream, float x, float y, float z,
                                                         std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundStream_SetVelocityHle(void* stream, float x, float y, float z,
                                                         std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundStream_SetRolloffCurveHle(void* stream, const float* points,
                                                             std::uint32_t count,
                                                             std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSound_SetDistanceFactorHle(void* ds, float factor,
                                                         std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSound_SetDopplerFactorHle(void* ds, float factor, std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSound_SetOrientationHle(void* ds, float xFront, float yFront, float zFront,
                                                      float xTop, float yTop, float zTop,
                                                      std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSound_SetPositionHle(void* ds, float x, float y, float z,
                                                   std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSound_SetRolloffFactorHle(void* ds, float factor, std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSound_SetVelocityHle(void* ds, float x, float y, float z,
                                                   std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSound_SetI3DL2ListenerHle(void* ds, const void* listener,
                                                        std::uint32_t apply) noexcept;
std::int32_t __stdcall IDirectSoundBuffer_SetBufferDataHle(void* buffer, void* data,
                                                           std::uint32_t size) noexcept;
std::int32_t __stdcall DirectSoundCreateHle(const void* deviceGuid, void** ppDirectSound,
                                            void* unkOuter) noexcept;
std::int32_t __stdcall DirectSoundCreateBufferHle(const XDsBufferDesc* desc, void** ppBuffer) noexcept;
std::int32_t __stdcall DirectSoundCreateStreamHle(const XDsStreamDesc* desc, void** ppStream) noexcept;

// XMediaObject methods of a DSOUND stream (vtable slots of guest::kDsoundStreamVtable; reachable through the
// stream's guest vtable, declared for tests).
std::uint32_t __stdcall DsStreamAddRef(void* self) noexcept;
std::uint32_t __stdcall DsStreamRelease(void* self) noexcept;
std::int32_t __stdcall DsStreamGetInfo(void* self, XMediaInfo* info) noexcept;
std::int32_t __stdcall DsStreamGetStatus(void* self, std::uint32_t* status) noexcept;
std::int32_t __stdcall DsStreamProcess(void* self, const XMediaPacket* in, const XMediaPacket* out) noexcept;
std::int32_t __stdcall DsStreamDiscontinuity(void* self) noexcept;
std::int32_t __stdcall DsStreamFlush(void* self) noexcept;

// ---- host-side helpers (probe, tests, diagnostics) ------------------------------------------------------------------
// Category/name tag of the next DSOUND object, set by the probe on SoundVoice::CreateDsObject (0xCC150) for the
// duration of the original call; DirectSoundCreateBuffer/Stream inside it take it over.
struct VoiceTag {
    bool valid = false;
    Category category = Category::Movie;
    bool is3D = false;
    bool stream = false;
    std::uint32_t voiceId = 0;    // SoundVoice+0x6C
    std::uint32_t voice = 0;      // guest SoundVoice*
    char name[64] = {};           // SoundData+0x20
};
void SetPendingVoiceTag(const VoiceTag& tag) noexcept;
void ClearPendingVoiceTag() noexcept;
// Records that CreateDsObject is about to replace a still live DSOUND object of the same SoundVoice (double Play,
// m5_audio.md 3.3 item 6): counted and reported as SoundEventKind::OrphanedObject; the object keeps playing.
void NoteOrphanedObject(const void* object, std::uint32_t voiceId) noexcept;

bool IsDsoundDevice(const void* p) noexcept;
bool IsDsoundBuffer(const void* p) noexcept;
bool IsDsoundStream(const void* p) noexcept;
std::uint32_t DsoundStreamQueuedPackets(const void* stream) noexcept;
// Read-only view of an object's current output parameters (tests).
struct ObjectView {
    bool found = false;
    bool hasVoice = false;
    Category category = Category::Movie;
    bool is3D = false;
    std::uint32_t cursor = 0;         // buffers: remembered play position in frames
    std::uint32_t frames = 0;         // buffers: decoded frames
    float frequencyRatio = 1.0f;
    std::uint32_t outputChannels = 0;
    float matrix[kMaxOutputChannels * kMaxOutputChannels] = {};
    float reverbSend = 0;
    bool startPending = false;  // Play / Pause(0) done, the engine voice starts at the next commit
};
ObjectView InspectObject(const void* object) noexcept;
// Listener as committed (CommitDeferredSettings or an immediate set).
Listener3D CommittedListener() noexcept;
I3dl2Listener CommittedReverb() noexcept;

struct DsoundStatistics {
    std::uint32_t buffersCreated = 0;
    std::uint32_t buffersLive = 0;
    std::uint32_t buffersPeak = 0;
    std::uint32_t streamsCreated = 0;
    std::uint32_t streamsLive = 0;
    std::uint32_t plays = 0;
    std::uint32_t packetsSubmitted = 0;
    std::uint32_t packetsCompleted = 0;
    std::uint32_t packetsFlushed = 0;
    std::uint32_t packetsRejected = 0;  // Process with a full queue (0x88780032)
    std::uint32_t starvations = 0;      // a running stream's queue ran empty (dropout; SoundEventKind::PacketsLate)
    std::uint32_t orphans = 0;          // double Play (never released objects)
    std::uint32_t formatErrors = 0;
    std::uint32_t voiceErrors = 0;      // no engine voice for an object (it stays silent)
    std::uint32_t tagged = 0;           // objects created inside the CreateDsObject probe
};
DsoundStatistics GetDsoundStatistics() noexcept;
// One summary line (xdk::LogCallSummary and the exit reporter "audio").
void LogDsoundSummary(core::Level level) noexcept;
// Tests: releases every object (without guest writes), resets the device and the statistics.
void ResetDsoundForTesting() noexcept;

}  // namespace snd
