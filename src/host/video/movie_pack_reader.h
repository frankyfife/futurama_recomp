// Movie pack reader (M6b, see movie_pack.h): decodes one pack .mp4 (H.264 / HEVC) with Windows Media Foundation
// (IMFSourceReader; the H.264 decoder is part of every Windows 10/11 with Media Foundation, nothing is downloaded)
// to YUY2 pictures - the format of the game's movie texture, so the renderer converts them like the XMV frames
// (BT.601, YuvEnable). Decoding runs on a worker thread of its own (COM/MF are initialised there, never on the guest
// thread) a few frames ahead of the XMV HLE, which takes the frames in order by index.
#pragma once

#include "gfx/d3d_shim.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace vid {

struct PackFrame {
    std::uint32_t index = 0;   // decode order = display order (the packs have no B-frames)
    std::int64_t ptsMs = 0;    // sample time of the mp4
    std::shared_ptr<const gfx::d3d::HostImage> image;  // YUY2
};

class PackVideoReader {
public:
    PackVideoReader();
    ~PackVideoReader();
    PackVideoReader(const PackVideoReader&) = delete;
    PackVideoReader& operator=(const PackVideoReader&) = delete;

    // Starts the worker on `file`; the picture must be width x height. Returns at once (errors show up in Get).
    bool Open(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height, std::string& error);

    enum class Result : std::uint8_t { Ok, Timeout, End, Error };
    // Frame `index`: older decoded frames are dropped; waits up to `timeoutMs` for the worker. `error` on Error.
    Result Get(std::uint32_t index, std::uint32_t timeoutMs, PackFrame& out, std::string& error);
    // Waits up to `timeoutMs` until the first frame is decoded (or the reader ended/failed). The XMV HLE calls it
    // before it starts the audio, so Media Foundation's start-up does not make the first frames late.
    void WaitReady(std::uint32_t timeoutMs);
    // Stops and joins the worker (also in the destructor).
    void Close() noexcept;

    // Decoder description for the log (e.g. "H.264 via Media Foundation, output YUY2"); valid after the first frame.
    std::string Description() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vid
