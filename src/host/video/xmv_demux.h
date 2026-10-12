// XMV demuxer (M6): splits the packet chain of an Xbox .xmv movie into WMV2 video frames and per-track Xbox ADPCM
// audio data (analysis/loader/avi.md 3.3; layout as in the public FFmpeg libavformat/xmv.c). Pure code without
// guest or OS dependencies: the XMV HLE (video/xmv_hle.cpp) feeds it the packets the game's callbacks deliver, the
// test mode (video/xmv_test_mode.cpp) the packets of a file.
//
// File = chain of packets; the first DWORD of every packet body is the size of the NEXT packet (0 = last packet).
// The first packet starts with the file header (avi.md 3.3):
//   +0x00 next packet size, +0x04 size of this (first) packet, +0x08 max packet size, +0x0C 'xobX', +0x10 version
//   (2), +0x14 width, +0x18 height, +0x1C duration ms, +0x20 u16 audio track count, +0x22 u16 padding,
//   +0x24 per track 12 bytes {u16 format tag (0x69), u16 channels, u32 rate, u16 bits, u16 flags}
// and its packet body follows right after the header (FFmpeg: next_packet_offset = header end).
// Packet body:
//   +0 u32 next packet size
//   +4 u32 video header: bits 0..22 video data size, 23..30 frame count, 31 new extradata
//   +8 u32 (unknown)
//   +12 per track u32: bits 0..22 audio data size (0 for track > 0 = size of the previous track)
//   then the video data (size - 4 * tracks, FFmpeg's alignment rule) and the audio data of each track in order.
// Video data: [4-byte XMV extradata if bit 31] then per frame u32 {bits 0..16 size/4 - 1, 17..31 pts delta ms} and
// (size) bytes of WMV2 whose 32-bit words are stored little-endian (byte-swapped to the usual big-endian WMV2 bit
// order here). The XMV extradata bits {mspel, loop filter, abt, j-type, top-left mv, per-mb rl, slice count} are
// rewritten into the standard 4-byte WMV2 sequence header (bits 15..10 and slice count << 7, big-endian).
// Audio: per packet and track one contiguous run of 36 * channels-byte ADPCM blocks (64 frames per block).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vid::xmv {

inline constexpr std::uint32_t kMagic = 0x58626F78;  // 'xobX' (bytes 78 6F 62 58)
inline constexpr std::uint32_t kFileHeaderFixedBytes = 0x24;
inline constexpr std::uint32_t kAudioTrackBytes = 12;
inline constexpr std::uint32_t kMaxAudioTracks = 16;  // the game's files have 1 or 5
inline constexpr std::uint16_t kFormatXboxAdpcm = 0x69;

struct AudioTrackInfo {
    std::uint16_t formatTag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t flags = 0;
    std::uint32_t BlockAlign() const noexcept { return 36u * channels; }  // Xbox ADPCM: 36 bytes per channel
};

struct FileHeader {
    std::uint32_t nextPacketSize = 0;   // +0x00
    std::uint32_t firstPacketSize = 0;  // +0x04
    std::uint32_t maxPacketSize = 0;    // +0x08
    std::uint32_t version = 0;          // +0x10
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t durationMs = 0;
    std::vector<AudioTrackInfo> tracks;
    std::uint32_t HeaderBytes() const noexcept {
        return kFileHeaderFixedBytes + kAudioTrackBytes * static_cast<std::uint32_t>(tracks.size());
    }
};

// Parses the file header at the start of the first packet (at least HeaderBytes() bytes). False + error text for
// a short buffer, a wrong magic, an unsupported size or more than kMaxAudioTracks tracks.
bool ParseFileHeader(std::span<const std::uint8_t> data, FileHeader& out, std::string& error);

// XMV extradata DWORD (as stored, little-endian value) -> the standard 4-byte WMV2 extradata.
std::array<std::uint8_t, 4> ConvertExtradata(std::uint32_t xmvExtradata) noexcept;

struct VideoFrame {
    std::uint32_t index = 0;    // 0-based frame number in the file
    std::uint32_t ptsMs = 0;    // sum of the pts deltas up to this frame (the first frame has delta 0)
    bool keyframe = false;      // WMV2 picture type bit 0 = I-frame
    std::vector<std::uint8_t> data;  // WMV2 bitstream (byte-swapped to big-endian words)
};

struct AudioChunk {
    std::uint32_t track = 0;
    std::uint64_t firstFrame = 0;  // first audio frame (sample per channel) of this chunk in its track
    std::vector<std::uint8_t> data;  // whole ADPCM blocks (a trailing partial block is kept as delivered)
};

struct PacketOutput {
    std::vector<VideoFrame> frames;
    std::vector<AudioChunk> audio;  // one chunk per track with data (track order)
    bool extradataChanged = false;  // a packet carried extradata that differs from the current one
};

class Demuxer {
public:
    // Next expected packet: the first packet (whole, including the file header) and then each packet as the chain
    // delivers it. Returns false (error() set) for a malformed packet; the demuxer stays failed afterwards.
    bool PushPacket(std::span<const std::uint8_t> packet, PacketOutput& out);

    bool HasHeader() const noexcept { return haveHeader_; }
    const FileHeader& Header() const noexcept { return header_; }
    // Size of the packet that follows the last pushed one (first packet: the header's +0x04 size); 0 = end.
    std::uint32_t NextPacketSize() const noexcept { return nextPacketSize_; }
    bool AtEnd() const noexcept { return haveHeader_ && nextPacketSize_ == 0; }
    bool Failed() const noexcept { return failed_; }
    const std::string& Error() const noexcept { return error_; }
    // Current WMV2 extradata (valid once a packet with extradata was pushed).
    bool HasExtradata() const noexcept { return haveExtradata_; }
    const std::array<std::uint8_t, 4>& Extradata() const noexcept { return extradata_; }
    std::uint32_t PacketsPushed() const noexcept { return packets_; }
    std::uint32_t FramesOut() const noexcept { return frameIndex_; }

private:
    bool Fail(std::string text);
    bool ParseBody(std::span<const std::uint8_t> body, PacketOutput& out);

    FileHeader header_;
    bool haveHeader_ = false;
    bool failed_ = false;
    std::string error_;
    std::uint32_t nextPacketSize_ = 0;
    std::uint32_t packets_ = 0;
    std::uint32_t frameIndex_ = 0;
    std::uint64_t videoPts_ = 0;
    std::vector<std::uint64_t> audioFrames_;  // per track: frames delivered so far
    std::array<std::uint8_t, 4> extradata_{};
    bool haveExtradata_ = false;
};

}  // namespace vid::xmv
