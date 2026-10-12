// Movie core (M6): packet source -> XMV demuxer -> queues of WMV2 frames and ADPCM audio, plus the WMV2 decoder.
// Shared by the XMV HLE (packets from the game's callbacks, video/xmv_hle.cpp) and the host test mode (packets from
// a file on the disc image, video/xmv_test_mode.cpp). Single-threaded: the HLE uses it on the guest thread only.
#pragma once

#include "video/wmv_decoder.h"
#include "video/xmv_demux.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace vid {

// Sequential packet chain of one movie.
class PacketSource {
public:
    virtual ~PacketSource() = default;
    // Copies the next packet (exactly `size` bytes; the first call: the first packet including the file header) into
    // `out`. False on a read error.
    virtual bool Read(std::uint32_t size, std::vector<std::uint8_t>& out) = 0;
};

// Packets of a movie held in memory (test mode, unit tests).
class MemoryPacketSource final : public PacketSource {
public:
    explicit MemoryPacketSource(std::vector<std::uint8_t> file) : file_(std::move(file)) {}
    bool Read(std::uint32_t size, std::vector<std::uint8_t>& out) override;

private:
    std::vector<std::uint8_t> file_;
    std::size_t offset_ = 0;
};

struct MovieCoreStats {
    std::uint32_t packets = 0;
    std::uint32_t framesDemuxed = 0;
    std::uint32_t framesDecoded = 0;    // pictures the decoder produced
    std::uint32_t decodeErrors = 0;
    std::uint32_t extradataChanges = 0; // packets whose WMV2 extradata differs (the decoder keeps the first)
    std::uint32_t maxFrameGapMs = 0;
    std::uint32_t lastFrameGapMs = 0;
    std::uint32_t lastPts = 0;
};

class MovieCore {
public:
    // `firstPacketSize` = file header +0x04. `keepAudioTrackMask`: bit t set = queue the audio of track t (the
    // others are dropped while demuxing). Reads nothing yet.
    void Open(PacketSource* source, std::uint32_t firstPacketSize, std::uint32_t keepAudioTrackMask) noexcept;
    void SetAudioTrackMask(std::uint32_t mask) noexcept { keepMask_ = mask; }

    // Reads and demuxes one more packet. False at the end of the chain or on an error (Failed()).
    bool ReadPacket();
    bool Eof() const noexcept { return eof_; }
    bool Failed() const noexcept { return failed_; }
    const std::string& Error() const noexcept { return error_; }
    bool HasHeader() const noexcept { return demux_.HasHeader(); }
    const xmv::FileHeader& Header() const noexcept { return demux_.Header(); }

    std::deque<xmv::VideoFrame>& Video() noexcept { return video_; }
    std::deque<xmv::AudioChunk>& Audio(std::uint32_t track) noexcept { return audio_[track % xmv::kMaxAudioTracks]; }
    // Audio frames demuxed so far for a track (whole blocks).
    std::uint64_t AudioFramesDemuxed(std::uint32_t track) const noexcept {
        return audioFrames_[track % xmv::kMaxAudioTracks];
    }

    // Decodes one frame (opens the WMV2 decoder with the current extradata at the first call). False = the decoder
    // could not be opened (Failed()) or rejected the frame (counted, `produced` false).
    bool Decode(const xmv::VideoFrame& frame, I420Frame& out, bool& produced);
    const MovieCoreStats& Stats() const noexcept { return stats_; }

private:
    bool Fail(std::string text);

    PacketSource* source_ = nullptr;
    std::uint32_t firstPacketSize_ = 0;
    std::uint32_t keepMask_ = 0;
    xmv::Demuxer demux_;
    WmvDecoder decoder_;
    bool eof_ = false;
    bool failed_ = false;
    std::string error_;
    std::vector<std::uint8_t> packet_;
    xmv::PacketOutput out_;
    std::deque<xmv::VideoFrame> video_;
    std::deque<xmv::AudioChunk> audio_[xmv::kMaxAudioTracks];
    std::uint64_t audioFrames_[xmv::kMaxAudioTracks] = {};
    MovieCoreStats stats_;
    bool havePts_ = false;
};

}  // namespace vid
