// DSOUND stubs for M2-M4 (design E7, 2.1 "DSOUND ... M2-M4: Stub (*pp = NULL)", 6 "M2"; avi.md 2.2,
// 2.3, 2.5, 6 step 1). The game runs silently: every one of the 40 DSOUND entry points the game or XMV
// calls is replaced (audio/dsound_stubs.cpp, registered with XDK_HOOK), the DSOUND library code and
// the APU are never touched.
//
// Returned values (evidence for each in the function comments of dsound_stubs.cpp):
//   DirectSoundCreate        S_OK, *ppDS = dummy device handle (non-NULL so that SoundManager::Update
//                            0xCBCB0 keeps running its voice/data update; C API only)
//   DirectSoundCreateBuffer  S_OK, *ppBuffer = NULL (every use of SoundVoice+0xA4 is NULL-checked or
//                            goes to a stub)
//   DirectSoundCreateStream  S_OK, *ppStream = NULL (default) or a stub stream object with the
//                            XMediaObject vtable ([audio] dummy_streams = 1)
//   IDirectSoundBuffer_GetStatus  *pdwStatus = 0 always (the voice destructor 0xCC030 spins otherwise)
//   all other HRESULT entries S_OK, Release -> remaining reference count, void entries no-op.
//
// The functions are declared here so that unit tests (tests/xdk_tests.cpp) can call them directly.
#pragma once

#include <cstdint>

namespace snd {

// ---- guest structures (XDK dsound.h layout; only the fields the stubs read) ---------------------
#pragma pack(push, 1)
// WAVEFORMATEX (18 bytes, packed; the game's copy lies at SoundData+0x34 resp. unaligned +0x59).
struct XWaveFormatEx {
    std::uint16_t wFormatTag;  // 1 PCM, 0x69 Xbox ADPCM (avi.md 2.4.3)
    std::uint16_t nChannels;
    std::uint32_t nSamplesPerSec;
    std::uint32_t nAvgBytesPerSec;  // wrong for the game's ADPCM buffer format (avi.md 2.4.3), not used
    std::uint16_t nBlockAlign;
    std::uint16_t wBitsPerSample;
    std::uint16_t cbSize;  // forced to 0x12 by the game's WAV parser
};
#pragma pack(pop)
static_assert(sizeof(XWaveFormatEx) == 18);

// DSBUFFERDESC (0xCC23F..0xCC292: dwSize 0x18, dwFlags (3D ? 0x60010 : 0) | 0x100, lpwfxFormat).
struct XDsBufferDesc {
    std::uint32_t dwSize;
    std::uint32_t dwFlags;
    std::uint32_t dwBufferBytes;
    std::uint32_t lpwfxFormat;  // guest XWaveFormatEx*
    std::uint32_t lpMixBins;
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

// XMEDIAPACKET (pump 0xCB680..0xCB6B7: 6 DWORDs, pdwStatus = &voice[0x7C + 4*i]).
struct XMediaPacket {
    std::uint32_t pvBuffer;
    std::uint32_t dwMaxSize;
    std::uint32_t pdwCompletedSize;  // guest DWORD* or 0
    std::uint32_t pdwStatus;         // guest DWORD* or 0
    std::uint32_t hCompletionEvent;  // or pContext
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

// ---- constants ----------------------------------------------------------------------------------
inline constexpr std::int32_t kS_OK = 0;
inline constexpr std::uint16_t kWaveFormatPcm = 1;
inline constexpr std::uint16_t kWaveFormatXboxAdpcm = 0x69;  // 64 samples per 36-byte block and channel
// XMEDIAPACKET status values. E_PENDING: the game refills a packet slot once *pdwStatus != it
// (0xCB5D4); the original Process sets it synchronously (0x24B1D9, avi.md 2.3).
inline constexpr std::uint32_t kPacketPending = 0x8000000A;  // E_PENDING
inline constexpr std::uint32_t kPacketSuccess = 0;
// ASSUMPTION: XMEDIAPACKET_STATUS_FLUSHED = E_ABORT (XDK xmo.h); the game only tests != E_PENDING.
inline constexpr std::uint32_t kPacketFlushed = 0x80004004;
// IDirectSoundBuffer_GetStatus: the game tests bit 0 (0xCCA2C, DSBSTATUS_PLAYING) and the whole DWORD
// against 0 (destructor 0xCC0C4).
inline constexpr std::uint32_t kBufferStatusPlaying = 0x1;
// Stream status bits. 0x10000 is tested by XMV CloseDecoder 0x264677 (avi.md 2.3);
// ASSUMPTION: names/other bits after XDK dsound.h (DSSTREAMSTATUS_READY/PLAYING/PAUSED).
inline constexpr std::uint32_t kStreamStatusReady = 0x00000001;
inline constexpr std::uint32_t kStreamStatusPlaying = 0x00010000;
inline constexpr std::uint32_t kStreamStatusPaused = 0x00020000;
// IDirectSoundStream_Pause: 0 resume, 1 pause (game 0xCC711/0xCB560), 2 = paused until
// IDirectSound_SynchPlayback (XMV only; avi.md 2.5, XDK DSSTREAMPAUSE_SYNCHPLAYBACK, assumption).
inline constexpr std::uint32_t kStreamResume = 0;
inline constexpr std::uint32_t kStreamPause = 1;
inline constexpr std::uint32_t kStreamPauseSynch = 2;
// Returned by the original Process when the stream cannot take another packet (avi.md 2.3).
inline constexpr std::int32_t kDsErrInvalidCall = static_cast<std::int32_t>(0x88780032);
inline constexpr std::int32_t kE_InvalidArg = static_cast<std::int32_t>(0x80070057);

// ---- the 40 entry points (stdcall; one comment block per function in dsound_stubs.cpp) ----------
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

// ---- host-side helpers (tests, diagnostics) -----------------------------------------------------
// [audio] dummy_streams: DirectSoundCreateStream returns stub stream objects instead of NULL. Read from
// the configuration on the first DirectSoundCreateStream; tests can override it.
void SetDummyStreams(bool enabled) noexcept;
bool DummyStreamsEnabled() noexcept;
// True if `p` is the dummy IDirectSound handle / a live stub stream.
bool IsDummyDevice(const void* p) noexcept;
bool IsStubStream(const void* p) noexcept;
// Number of live stub streams and of packets queued in them.
std::uint32_t StubStreamCount() noexcept;
std::uint32_t StubStreamQueuedPackets(const void* stream) noexcept;
// Process-wide counters of the stub streams (logged by xdk::LogCallSummary when streams were created).
struct StubStreamStatistics {
    std::uint32_t created = 0;
    std::uint32_t live = 0;
    std::uint32_t packetsSubmitted = 0;
    std::uint32_t packetsCompleted = 0;
    std::uint32_t packetsFlushed = 0;
    std::uint32_t packetsRejected = 0;  // Process with a full queue (DSERR_INVALIDCALL)
};
StubStreamStatistics GetStubStreamStatistics() noexcept;
// Completes every queued packet whose play time has elapsed (called by DirectSoundDoWork; `nowQpc`
// = 0 uses the current QueryPerformanceCounter). Returns the number of completed packets.
std::uint32_t PumpStubStreams(std::int64_t nowQpc = 0) noexcept;

}  // namespace snd
