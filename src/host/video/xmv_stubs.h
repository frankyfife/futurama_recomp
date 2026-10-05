// XMV stubs for M2-M5 (design E7, 2.1 "XMV ... M2-M5: Decoder anlegen, GetNextFrame -> Ergebnis 2
// sofort", 6 "M2"; avi.md 3.1, 3.2, 6 step 2). Every movie is skipped by reporting "end of file" on the
// first frame: the game's movie player 0xEDE40..0xEE220 stays unchanged and finishes the movie
// orderly (done flag, player destroyed by App slot 7 0xA24C0).
//
// Returned values (evidence in video/xmv_stubs.cpp):
//   CreateDecoderForPackets  S_OK, *ppDecoder = dummy decoder (width/height/audio tracks parsed from the
//                            first-packet header the game passes); E_FAIL for a header without 'xobX'
//   GetVideoDescriptor       desc[0] = width, desc[1] = height, desc[3] = audio stream count
//   EnableAudioStream        S_OK, *ppStream = NULL (if requested)
//   GetAudioStream           *ppStream = NULL
//   GetNextFrame             *pResult = 2 (XMV_ENDOFFILE), S_OK
//   GetPlaybackTimeMs        0
//   CloseDecoder             frees the dummy decoder
#pragma once

#include <cstdint>

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

// Host-side helpers (tests, diagnostics).
bool IsStubDecoder(const void* decoder) noexcept;
std::uint32_t LiveStubDecoders() noexcept;

}  // namespace vid
