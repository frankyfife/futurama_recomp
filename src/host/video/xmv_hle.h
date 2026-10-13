// XMV HLE (M6; replaces the M2-M5 stubs that ended every movie at once). The 7 XMV entry points of the game's movie
// player (avi.md 3.1/3.2; design M6) are replaced by a host decoder:
//   packets   through the game's own callbacks GetNextPacket 0xEDBA0 / ReleasePreviousPacket 0xEDE60 (the player
//             reads the file sequentially with its file object), demuxed by video/xmv_demux.h;
//   video     WMV2 decoded by the Windows WMVideo Decoder DMO (video/wmv_decoder.h), packed to YUY2 into the surface
//             of the game's texture (D3DDevice_CreateTexture2(W, H, 1, 1, 0, 0x24, 3), D3D shim) like the original
//             FUN_00265B0B; the renderer converts YUY2 on upload (respecting YuvEnable, gfx/nipc_gpu MovieTexture);
//   audio     the track the game enables (5 tracks: language via FUN_000EDBE0, else track 0) as a host DSOUND
//             stream (audio/dsound_hle.h) fed with the Xbox ADPCM blocks; returned to the game, which calls
//             IDirectSoundStream_SetVolume and Release on it (0xEE471/0xEE47D);
//   timing    strictly by pts on the audio clock (video/movie_clock.h), start of all streams with Pause(2) +
//             IDirectSound_SynchPlayback like the original.
//   pack      M6b: with [render] movie_pack the shown frame's upscaled picture from the pack .mp4 (video/movie_pack.h,
//             Media Foundation reader on its own thread) is attached to the texture as a host image; timing, audio
//             and the XMV picture (fallback) stay as above.
//
// Entry points (evidence per function in xmv_hle.cpp):
//   CreateDecoderForPackets  S_OK, *ppDecoder = guest handle; E_FAIL for a header without 'xobX' or a bad size
//   GetVideoDescriptor       desc[0] = width, desc[1] = height, desc[3] = audio stream count (desc[2] untouched)
//   EnableAudioStream        creates the DSOUND stream of the track (paused until the first frame), *pp if asked
//   GetAudioStream           *ppStream = the track's stream with AddRef (NULL if not enabled)
//   GetNextFrame             *pResult 1 = new frame written to the surface, 0 = no new frame yet, 2 = end of the
//                            movie, 3 = error (decoder unavailable, broken file)
//   GetPlaybackTimeMs        movie time in ms (the clock above; 0 before the first frame)
//   CloseDecoder             FlushEx + Release of the streams, frees the decoder
#pragma once

#include <cstdint>
#include <string>

namespace vid {

// First-packet header of an .xmv file as read by the movie player (0x1000 bytes at 0xEE336; avi.md 3.3).
struct XmvFileHeader {
    std::uint32_t nextPacketSize;    // +0x00
    std::uint32_t firstPacketSize;   // +0x04 (game: first read size)
    std::uint32_t maxPacketSize;     // +0x08 (game: size of its two packet buffers)
    std::uint32_t magic;             // +0x0C 'xobX'
    std::uint32_t version;           // +0x10 2
    std::uint32_t width;             // +0x14 640
    std::uint32_t height;            // +0x18 480
    std::uint32_t durationMs;        // +0x1C
    std::uint16_t audioStreamCount;  // +0x20 (5 for cut scenes, 1 for logos/trailers)
    std::uint16_t reserved;
};
static_assert(sizeof(XmvFileHeader) == 0x24);
inline constexpr std::uint32_t kXmvMagic = 0x58626F78;  // "xobX" (bytes 78 6F 62 58)

// Video descriptor filled by GetVideoDescriptor (0x264759: desc[0] = W, desc[1] = H, desc[3] =
// AudioStreamCount; desc[2] is not written; the game passes &player+0x1C, reads +0x1C/+0x20/+0x28).
struct XmvVideoDescriptor {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t notWritten;
    std::uint32_t audioStreamCount;
};
static_assert(sizeof(XmvVideoDescriptor) == 0x10);

// XMVRESULT values of GetNextFrame (avi.md 3.1, player 0xEDE90).
inline constexpr std::int32_t kXmvNoFrame = 0;
inline constexpr std::int32_t kXmvNewFrame = 1;
inline constexpr std::int32_t kXmvEndOfFile = 2;
inline constexpr std::int32_t kXmvFailure = 3;

std::int32_t __stdcall XMVDecoder_CreateDecoderForPacketsHle(std::uint32_t flags, const XmvFileHeader* header,
                                                             void* context, const void* getNextPacket,
                                                             const void* releasePreviousPacket,
                                                             void** ppDecoder) noexcept;
void __stdcall XMVDecoder_CloseDecoderHle(void* decoder) noexcept;
void __stdcall XMVDecoder_GetVideoDescriptorHle(void* decoder, XmvVideoDescriptor* desc) noexcept;
std::int32_t __stdcall XMVDecoder_EnableAudioStreamHle(void* decoder, std::uint32_t stream,
                                                       std::uint32_t flags, const void* mixBins,
                                                       void** ppStream) noexcept;
void __stdcall XMVDecoder_GetAudioStreamHle(void* decoder, std::uint32_t stream, void** ppStream) noexcept;
std::int32_t __stdcall XMVDecoder_GetNextFrameHle(void* decoder, void* surface, std::int32_t* result,
                                                  std::uint32_t* timestamp) noexcept;
std::uint32_t __stdcall XMVDecoder_GetPlaybackTimeMsHle(void* decoder) noexcept;

// ---- host-side helpers (tests, diagnostics) -------------------------------------------------------------------------
bool IsXmvDecoder(const void* decoder) noexcept;
std::uint32_t LiveXmvDecoders() noexcept;

struct MovieStatistics {
    std::uint32_t opened = 0;
    std::uint32_t finished = 0;      // reached the end (result 2)
    std::uint32_t closedEarly = 0;   // closed before the end (skipped)
    std::uint32_t failed = 0;        // result 3 (decoder unavailable, broken file)
    std::uint32_t framesShown = 0;
    std::uint32_t framesDropped = 0; // decoded but already late when the next one was due
    std::uint32_t maxLateMs = 0;     // largest clock - pts of a shown frame
    std::uint32_t audioPackets = 0;  // ADPCM chunks submitted to the DSOUND streams
    std::uint32_t audioRejected = 0;
    std::uint32_t packMovies = 0;     // M6b: movies whose picture came from [render] movie_pack
    std::uint32_t packFrames = 0;     // frames shown from the pack
    std::uint32_t packMisses = 0;     // pack frames not decoded in time (original picture for that frame)
    std::uint32_t packFallbacks = 0;  // pack unusable or abandoned during a movie (original picture)
};
MovieStatistics GetMovieStatistics() noexcept;
// One summary line on VID (exit reporter / tests).
void LogMovieSummary() noexcept;

}  // namespace vid
