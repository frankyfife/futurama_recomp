// Movie clock of the XMV HLE (M6): the playback position that decides which video frame is due. Strictly by pts:
// the game's files have irregular frame gaps (66/67 ms, 40 ms in the 25-fps files, single gaps of 133 ms .. 2 s,
// avi.md 3.3), so frames are shown when the clock reaches their pts, never at a fixed rate.
//
// Source of time (design M6 "Takt strikt nach pts"; avi.md 3.5):
//   - Audio clock: source frames the movie's DSOUND stream has played (engine voice, snd::StreamFramesPlayed) /
//     sample rate. Audio and picture cannot drift apart, and a stall of the audio output stops the picture too.
//   - Wall clock (QPC) when the movie has no audio stream or no engine voice, and after the audio has played out
//     (a picture longer than its audio continues from the audio end on the wall clock).
// Pure code (no OS calls): the caller passes the counters it read; unit tests drive it directly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vid {

class MovieClock {
public:
    // Starts the clock at movie time 0. `sampleRate` = 0: wall clock only.
    void Start(std::uint32_t sampleRate, std::int64_t wallTicks, std::int64_t ticksPerSecond) noexcept {
        rate_ = sampleRate;
        freq_ = ticksPerSecond > 0 ? ticksPerSecond : 1;
        startTicks_ = wallTicks;
        started_ = true;
        audioDone_ = false;
        lastMs_ = 0;
    }
    bool Started() const noexcept { return started_; }
    bool UsesAudio() const noexcept { return rate_ != 0 && !audioDone_; }

    // Current movie time in ms. `audioFrames` = frames played by the stream (ignored without audio),
    // `audioFinished` = every audio frame of the movie was submitted and played (EOF reached): from then on the
    // clock continues on the wall clock from the audio end. Never runs backwards.
    std::uint64_t NowMs(std::uint64_t audioFrames, bool audioFinished, std::int64_t wallTicks) noexcept {
        if (!started_) {
            return 0;
        }
        std::uint64_t ms;
        if (rate_ != 0 && !audioDone_) {
            ms = audioFrames * 1000u / rate_;
            if (audioFinished) {
                audioDone_ = true;
                audioEndMs_ = ms;
                audioEndTicks_ = wallTicks;
            }
        } else if (rate_ != 0) {
            ms = audioEndMs_ + TicksToMs(wallTicks - audioEndTicks_);
        } else {
            ms = TicksToMs(wallTicks - startTicks_);
        }
        if (ms < lastMs_) {
            ms = lastMs_;
        }
        lastMs_ = ms;
        return ms;
    }
    std::uint64_t LastMs() const noexcept { return lastMs_; }

private:
    std::uint64_t TicksToMs(std::int64_t ticks) const noexcept {
        return ticks <= 0 ? 0u : static_cast<std::uint64_t>(ticks) * 1000u / static_cast<std::uint64_t>(freq_);
    }

    std::uint32_t rate_ = 0;
    std::int64_t freq_ = 1;
    std::int64_t startTicks_ = 0;
    bool started_ = false;
    bool audioDone_ = false;
    std::uint64_t audioEndMs_ = 0;
    std::int64_t audioEndTicks_ = 0;
    std::uint64_t lastMs_ = 0;
};

// Number of leading frames (pts in display order) that are due at `nowMs` (pts <= now). The newest of them is the
// one to show; the older ones are decoded (P-frames depend on them) but not shown (late frames are dropped).
inline std::size_t FramesDue(std::span<const std::uint32_t> pts, std::uint64_t nowMs) noexcept {
    std::size_t n = 0;
    while (n < pts.size() && pts[n] <= nowMs) {
        ++n;
    }
    return n;
}

// End of the movie in ms: the later of the last frame's display end (pts + its gap to the previous frame, at least
// 1 ms) and the end of the audio.
inline std::uint64_t MovieEndMs(std::uint32_t lastPts, std::uint32_t lastGap, std::uint64_t audioEndMs) noexcept {
    const std::uint64_t video = static_cast<std::uint64_t>(lastPts) + (lastGap != 0 ? lastGap : 1u);
    return video > audioEndMs ? video : audioEndMs;
}

}  // namespace vid
