// XMV demuxer (see xmv_demux.h).
#include "video/xmv_demux.h"

#include <cstdio>
#include <cstring>

namespace vid::xmv {
namespace {

std::uint32_t Rd32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
std::uint16_t Rd16(const std::uint8_t* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

std::string Format(const char* fmt, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), fmt, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                  static_cast<unsigned long long>(c));
    return buf;
}

constexpr std::uint32_t kPacketFixedBytes = 12;      // next size, video header, unknown
constexpr std::uint32_t kMaxPacketBytes = 0x1000000;  // 16 MiB (the game's files: <= 0x28000)
constexpr std::uint32_t kMaxDimension = 4096;

}  // namespace

bool ParseFileHeader(std::span<const std::uint8_t> data, FileHeader& out, std::string& error) {
    if (data.size() < kFileHeaderFixedBytes) {
        error = "file header: shorter than 0x24 bytes";
        return false;
    }
    const std::uint8_t* p = data.data();
    if (Rd32(p + 0x0C) != kMagic) {
        error = Format("file header: magic 0x%08llX instead of 'xobX'", Rd32(p + 0x0C));
        return false;
    }
    FileHeader h;
    h.nextPacketSize = Rd32(p + 0x00);
    h.firstPacketSize = Rd32(p + 0x04);
    h.maxPacketSize = Rd32(p + 0x08);
    h.version = Rd32(p + 0x10);
    h.width = Rd32(p + 0x14);
    h.height = Rd32(p + 0x18);
    h.durationMs = Rd32(p + 0x1C);
    const std::uint32_t tracks = Rd16(p + 0x20);
    if (tracks > kMaxAudioTracks) {
        error = Format("file header: %llu audio tracks (at most 16 supported)", tracks);
        return false;
    }
    if (h.width == 0 || h.height == 0 || h.width > kMaxDimension || h.height > kMaxDimension || (h.width & 1) != 0) {
        error = Format("file header: unsupported picture size %llux%llu", h.width, h.height);
        return false;
    }
    if (data.size() < kFileHeaderFixedBytes + kAudioTrackBytes * tracks) {
        error = "file header: audio track table cut off";
        return false;
    }
    for (std::uint32_t i = 0; i < tracks; ++i) {
        const std::uint8_t* t = p + kFileHeaderFixedBytes + kAudioTrackBytes * i;
        AudioTrackInfo a;
        a.formatTag = Rd16(t + 0);
        a.channels = Rd16(t + 2);
        a.sampleRate = Rd32(t + 4);
        a.bitsPerSample = Rd16(t + 8);
        a.flags = Rd16(t + 10);
        h.tracks.push_back(a);
    }
    if (h.firstPacketSize < h.HeaderBytes() + kPacketFixedBytes || h.firstPacketSize > kMaxPacketBytes) {
        error = Format("file header: first packet size 0x%llX does not fit the header (0x%llX bytes)",
                       h.firstPacketSize, h.HeaderBytes());
        return false;
    }
    out = std::move(h);
    return true;
}

std::array<std::uint8_t, 4> ConvertExtradata(std::uint32_t x) noexcept {
    // FFmpeg xmv_read_extradata: XMV bit i -> WMV2 sequence header bit (15 - i) for the six flags, slice count
    // (bits 6..8) -> bits 7..9; the result is written big-endian (the WMV2 decoder reads it MSB first).
    std::uint32_t v = 0;
    v |= (x & 0x01u) << 15;  // mspel
    v |= (x & 0x02u) << 13;  // loop filter
    v |= (x & 0x04u) << 11;  // abt
    v |= (x & 0x08u) << 9;   // j-type
    v |= (x & 0x10u) << 7;   // top-left mv
    v |= (x & 0x20u) << 5;   // per-mb rl
    v |= ((x >> 6) & 7u) << 7;  // slice count
    return {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8),
            static_cast<std::uint8_t>(v)};
}

bool Demuxer::Fail(std::string text) {
    failed_ = true;
    error_ = std::move(text);
    return false;
}

bool Demuxer::PushPacket(std::span<const std::uint8_t> packet, PacketOutput& out) {
    out.frames.clear();
    out.audio.clear();
    out.extradataChanged = false;
    if (failed_) {
        return false;
    }
    try {
        if (!haveHeader_) {
            FileHeader h;
            std::string error;
            if (!ParseFileHeader(packet, h, error)) {
                return Fail(error);
            }
            if (packet.size() < h.firstPacketSize) {
                return Fail(Format("first packet: %llu bytes delivered, header says 0x%llX", packet.size(),
                                   h.firstPacketSize));
            }
            header_ = std::move(h);
            haveHeader_ = true;
            audioFrames_.assign(header_.tracks.size(), 0);
            const std::uint32_t skip = header_.HeaderBytes();
            ++packets_;
            return ParseBody(packet.subspan(skip, header_.firstPacketSize - skip), out);
        }
        if (nextPacketSize_ == 0) {
            return Fail("packet after the last packet of the chain");
        }
        if (packet.size() < nextPacketSize_) {
            return Fail(Format("packet %llu: %llu bytes delivered, 0x%llX expected", packets_, packet.size(),
                               nextPacketSize_));
        }
        ++packets_;
        return ParseBody(packet.subspan(0, nextPacketSize_), out);
    } catch (...) {
        return Fail("out of memory");
    }
}

bool Demuxer::ParseBody(std::span<const std::uint8_t> body, PacketOutput& out) {
    const std::uint32_t tracks = static_cast<std::uint32_t>(header_.tracks.size());
    const std::size_t fixed = kPacketFixedBytes + 4u * tracks;
    if (body.size() < fixed) {
        return Fail(Format("packet %llu: %llu bytes, shorter than its header", packets_, body.size()));
    }
    const std::uint8_t* p = body.data();
    const std::uint32_t next = Rd32(p);
    if (next > kMaxPacketBytes) {
        return Fail(Format("packet %llu: next packet size 0x%llX too large", packets_, next));
    }
    const std::uint32_t videoHeader = Rd32(p + 4);
    std::uint32_t videoSize = videoHeader & 0x007FFFFFu;
    std::uint32_t frameCount = (videoHeader >> 23) & 0xFFu;
    const bool hasExtradata = (videoHeader & 0x80000000u) != 0;
    // FFmpeg: "Subtracting 4 bytes for every audio track from the video data works at least for the audio."
    videoSize = videoSize >= 4u * tracks ? videoSize - 4u * tracks : 0u;

    std::vector<std::uint32_t> audioSize(tracks, 0);
    for (std::uint32_t t = 0; t < tracks; ++t) {
        audioSize[t] = Rd32(p + kPacketFixedBytes + 4u * t) & 0x007FFFFFu;
        if (audioSize[t] == 0 && t != 0) {
            audioSize[t] = audioSize[t - 1];  // FFmpeg: a file with one audio track and no video
        }
    }
    std::size_t offset = fixed;
    std::size_t audioOffset = fixed + videoSize;
    std::size_t total = audioOffset;
    for (const std::uint32_t s : audioSize) {
        total += s;
    }
    if (total > body.size()) {
        return Fail(Format("packet %llu: video + audio data (%llu bytes) exceed the packet (%llu bytes)", packets_,
                           total, body.size()));
    }

    // Video frames.
    std::size_t videoLeft = videoSize;
    if (videoLeft > 0 && hasExtradata) {
        if (videoLeft < 4) {
            return Fail(Format("packet %llu: extradata flag without data", packets_));
        }
        const std::array<std::uint8_t, 4> e = ConvertExtradata(Rd32(p + offset));
        if (haveExtradata_ && e != extradata_) {
            out.extradataChanged = true;
        }
        extradata_ = e;
        haveExtradata_ = true;
        offset += 4;
        videoLeft -= 4;
    }
    for (std::uint32_t i = 0; i < frameCount && videoLeft > 0; ++i) {
        if (videoLeft < 4) {
            return Fail(Format("packet %llu: frame %llu header cut off", packets_, i));
        }
        const std::uint32_t fh = Rd32(p + offset);
        const std::uint32_t size = (fh & 0x1FFFFu) * 4u + 4u;
        const std::uint32_t delta = fh >> 17;
        if (static_cast<std::size_t>(size) + 4 > videoLeft) {
            return Fail(Format("packet %llu: frame of %llu bytes exceeds the video data (%llu left)", packets_, size,
                               videoLeft));
        }
        VideoFrame f;
        f.index = frameIndex_++;
        videoPts_ += delta;
        f.ptsMs = static_cast<std::uint32_t>(videoPts_);
        f.data.resize(size);
        const std::uint8_t* src = p + offset + 4;
        for (std::uint32_t k = 0; k < size; k += 4) {  // little-endian words -> WMV2 big-endian bit order
            f.data[k + 0] = src[k + 3];
            f.data[k + 1] = src[k + 2];
            f.data[k + 2] = src[k + 1];
            f.data[k + 3] = src[k + 0];
        }
        f.keyframe = (f.data[0] & 0x80u) == 0;  // msmpeg4/WMV2 picture header: first bit 0 = I-frame
        out.frames.push_back(std::move(f));
        offset += size + 4u;
        videoLeft -= size + 4u;
    }

    // Audio: the whole run of each track.
    for (std::uint32_t t = 0; t < tracks; ++t) {
        if (audioSize[t] != 0) {
            AudioChunk c;
            c.track = t;
            c.firstFrame = audioFrames_[t];
            c.data.assign(p + audioOffset, p + audioOffset + audioSize[t]);
            const std::uint32_t align = header_.tracks[t].BlockAlign();
            if (align != 0) {
                audioFrames_[t] += static_cast<std::uint64_t>(audioSize[t] / align) * 64u;
            }
            out.audio.push_back(std::move(c));
        }
        audioOffset += audioSize[t];
    }
    nextPacketSize_ = next;
    return true;
}

}  // namespace vid::xmv
