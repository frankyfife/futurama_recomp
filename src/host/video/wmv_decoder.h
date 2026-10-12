// WMV2 video decoder (M6) on the "WMVideo Decoder DMO" that ships with Windows (wmvdecod.dll, CLSID
// CLSID_CWMVDecMediaObject {82D353DF-90BD-4382-8BC2-3F6192B76E34}), used as a DirectX Media Object (IMediaObject).
// No FFmpeg and no download (design R10 fallback, docs/m6_report.md). The DMO is created from the DLL's class
// factory (LoadLibrary + DllGetClassObject: no COM registration lookup and no CoInitialize on the guest thread).
//
// Input: one XMV video frame (vid::xmv::VideoFrame::data, big-endian WMV2 words) per Decode call; the 4-byte WMV2
// sequence header goes into the input media type (VIDEOINFOHEADER + 4 bytes, biCompression 'WMV2'). Output: planar
// I420 (IYUV) frames, width x height luma + two (width/2) x (height/2) chroma planes, tightly packed. WMV2 has no
// B-frames: every input frame produces one output frame at once (checked by the test mode: frames in == out).
// Verified against FFmpeg (PyAV) on the game's files: luma bit-exact (tools/m6_check.py, docs/m6_report.md).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vid {

struct I420Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> data;  // Y (w*h), U (w/2*h/2), V (w/2*h/2)
    const std::uint8_t* Y() const noexcept { return data.data(); }
    const std::uint8_t* U() const noexcept { return data.data() + static_cast<std::size_t>(width) * height; }
    const std::uint8_t* V() const noexcept {
        return data.data() + static_cast<std::size_t>(width) * height + static_cast<std::size_t>(width / 2) * (height / 2);
    }
};

class WmvDecoder {
public:
    WmvDecoder();
    ~WmvDecoder();
    WmvDecoder(const WmvDecoder&) = delete;
    WmvDecoder& operator=(const WmvDecoder&) = delete;

    // Creates the DMO and sets the media types. False (error text) if wmvdecod.dll or WMV2 support is missing.
    bool Open(std::uint32_t width, std::uint32_t height, const std::array<std::uint8_t, 4>& extradata,
              std::string& error);
    bool IsOpen() const noexcept;
    // Decodes one frame. True with `produced` = whether `out` received a picture. False = decoder error (text in
    // LastError()); the caller may continue with the next keyframe.
    bool Decode(std::span<const std::uint8_t> frame, std::uint32_t ptsMs, I420Frame& out, bool& produced);
    // End of stream: returns a remaining picture if the decoder held one back (never for WMV2).
    bool Drain(I420Frame& out, bool& produced);
    const std::string& LastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// True if the Windows WMV decoder can be created at all (wmvdecod.dll present and its class factory works).
bool WmvDecoderAvailable(std::string* error = nullptr);

}  // namespace vid
