// Screenshots as PNG through WIC (GFX_CONTRACT.md 8). Owned by the backend agent.
//
// Taken in DisplayFrame on the guest main thread before Present (rhi::Device::CaptureBackBuffer) when the
// screenshot hotkey was pressed (platform::ConsumeHotkeys) or the displayed-frame number is listed in
// [render] screenshot_frames (automatic screenshots for the M3 check). Files go to [render]
// screenshot_dir (default <userdata>\screenshots, i.e. under run\<name>\ for test runs - never into the
// repository): frame_<N, 6 digits>.png, plus a GFX/info log line "screenshot: <path> (WxH)".
// The PNG is 24-bit RGB of the 640x480 back buffer (alpha dropped, as the screen shows it). M3c: the renderer
// captures the internal render size or the presented output size ([render] screenshot_size) and writes the
// PNG asynchronously (SaveScreenshot with a ScreenshotSource).
#pragma once

#include "gfx/rhi/rhi.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

// Writes `image` (BGRA8_UNorm, BGRX8_UNorm or RGBA8_UNorm; alpha written as opaque = 24-bit PNG) as PNG.
// Creates the directory. Returns false and the reason on failure (a partial file is removed). Callable
// from any thread (initializes COM for itself).
bool WritePngFile(const std::filesystem::path& path, const gfx::rhi::Image& image, std::string* error) noexcept;

// <screenshot_dir>\frame_NNNNNN.png for a displayed-frame number.
std::filesystem::path ScreenshotPath(const std::filesystem::path& directory, std::uint32_t frame);

// CaptureBackBuffer + WritePngFile(ScreenshotPath(directory, frame)) + the log line
// "screenshot: <path> (WxH)" (GFX/info; failures GFX/error "screenshot of frame N failed: <reason>").
// Call before Present (flip-model back buffers are undefined afterwards).
bool SaveScreenshot(gfx::rhi::Device& device, const std::filesystem::path& directory, std::uint32_t frame) noexcept;

// M3c (render resolution): which picture a screenshot shows ([render] screenshot_size).
enum class ScreenshotSource : std::uint8_t {
    Internal,  // the back buffer at the internal render size, MSAA resolved (rhi::Device::CaptureBackBuffer)
    Output,    // the image Present shows, after the supersampling downsample (rhi::Device::CaptureOutput)
};
// M3c: captures `source` on the calling (render) thread before Present, then encodes and writes the PNG on a
// worker thread - a 2880x2160 or larger PNG takes 100+ ms, which would cost the paced frame rate. The worker logs
// the same "screenshot: <path> (WxH)" line (or the GFX/error line) when the file is complete. The first call
// registers the exit reporter "screenshots", which waits for the pending files (FlushScreenshots). False only if
// the capture failed (logged).
bool SaveScreenshot(gfx::rhi::Device& device, const std::filesystem::path& directory, std::uint32_t frame,
                    ScreenshotSource source) noexcept;
// Waits up to `timeoutMs` until every screenshot queued by the asynchronous SaveScreenshot is written; true if
// none is pending any more. Any thread.
bool FlushScreenshots(std::uint32_t timeoutMs) noexcept;

// Reads a PNG (or any WIC format) as BGRA8 (tests, comparisons). False + reason on failure.
bool ReadImageFile(const std::filesystem::path& path, gfx::rhi::Image& out, std::string* error) noexcept;

}  // namespace platform
