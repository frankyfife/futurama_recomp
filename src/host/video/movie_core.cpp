// Movie core (see movie_core.h).
#include "video/movie_core.h"

#include <cstring>

namespace vid {

bool MemoryPacketSource::Read(std::uint32_t size, std::vector<std::uint8_t>& out) {
    if (offset_ > file_.size() || file_.size() - offset_ < size) {
        return false;
    }
    out.assign(file_.begin() + static_cast<std::ptrdiff_t>(offset_),
               file_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += size;
    return true;
}

void MovieCore::Open(PacketSource* source, std::uint32_t firstPacketSize, std::uint32_t keepAudioTrackMask) noexcept {
    source_ = source;
    firstPacketSize_ = firstPacketSize;
    keepMask_ = keepAudioTrackMask;
}

bool MovieCore::Fail(std::string text) {
    failed_ = true;
    error_ = std::move(text);
    return false;
}

bool MovieCore::ReadPacket() {
    if (eof_ || failed_) {
        return false;
    }
    if (source_ == nullptr) {
        return Fail("no packet source");
    }
    try {
        const std::uint32_t size = demux_.HasHeader() ? demux_.NextPacketSize() : firstPacketSize_;
        if (size == 0) {
            eof_ = true;
            return false;
        }
        if (!source_->Read(size, packet_)) {
            return Fail("packet " + std::to_string(stats_.packets) + ": read of " + std::to_string(size) +
                        " bytes failed");
        }
        if (!demux_.PushPacket(packet_, out_)) {
            return Fail(demux_.Error());
        }
        ++stats_.packets;
        if (out_.extradataChanged) {
            ++stats_.extradataChanges;
        }
        for (xmv::VideoFrame& f : out_.frames) {
            if (havePts_) {
                const std::uint32_t gap = f.ptsMs - stats_.lastPts;
                stats_.lastFrameGapMs = gap;
                if (gap > stats_.maxFrameGapMs) {
                    stats_.maxFrameGapMs = gap;
                }
            }
            havePts_ = true;
            stats_.lastPts = f.ptsMs;
            ++stats_.framesDemuxed;
            video_.push_back(std::move(f));
        }
        for (xmv::AudioChunk& c : out_.audio) {
            const std::uint32_t t = c.track % xmv::kMaxAudioTracks;
            const std::uint32_t align = Header().tracks[c.track].BlockAlign();
            if (align != 0) {
                audioFrames_[t] += static_cast<std::uint64_t>(c.data.size() / align) * 64u;
            }
            if ((keepMask_ & (1u << t)) != 0) {
                audio_[t].push_back(std::move(c));
            }
        }
        if (demux_.AtEnd()) {
            eof_ = true;
        }
        return true;
    } catch (...) {
        return Fail("out of memory");
    }
}

bool MovieCore::Decode(const xmv::VideoFrame& frame, I420Frame& out, bool& produced) {
    produced = false;
    if (failed_) {
        return false;
    }
    if (!decoder_.IsOpen()) {
        if (!demux_.HasExtradata()) {
            return Fail("first video frame without WMV2 extradata");
        }
        std::string error;
        if (!decoder_.Open(Header().width, Header().height, demux_.Extradata(), error)) {
            return Fail(error);
        }
    }
    if (!decoder_.Decode(frame.data, frame.ptsMs, out, produced)) {
        ++stats_.decodeErrors;
        return false;
    }
    if (produced) {
        ++stats_.framesDecoded;
    }
    return true;
}

}  // namespace vid
