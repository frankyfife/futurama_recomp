// Screenshots as PNG through WIC (GFX_CONTRACT.md 8). Owned by the backend agent.
//
// Taken in DisplayFrame on the guest main thread before Present (rhi::Device::CaptureBackBuffer) when the
// screenshot hotkey was pressed (platform::ConsumeHotkeys) or the displayed-frame number is listed in
// [render] screenshot_frames (automatic screenshots for the M3 check). Files go to [render]
// screenshot_dir (default <userdata>\screenshots, i.e. under run\<name>\ for test runs - never into the
// repository): frame_<N, 6 digits>.png, plus a GFX/info log line "screenshot: <path> (WxH)".
// The PNG is 24-bit RGB of the 640x480 back buffer (alpha dropped, as the screen shows it).
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

// Reads a PNG (or any WIC format) as BGRA8 (tests, comparisons). False + reason on failure.
bool ReadImageFile(const std::filesystem::path& path, gfx::rhi::Image& out, std::string* error) noexcept;

}  // namespace platform
