// Screenshots (screenshot.h): PNG through the Windows Imaging Component.
#include "platform/screenshot.h"

#include "core/host_main.h"
#include "core/log.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <system_error>
#include <thread>

#pragma comment(lib, "windowscodecs.lib")

namespace platform {
namespace {

using Microsoft::WRL::ComPtr;

// COM for the calling thread for the lifetime of the object; works whether or not (and how) the thread
// initialized COM before (RPC_E_CHANGED_MODE: COM is usable, but this object must not uninitialize).
class ComScope {
public:
    ComScope() noexcept : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    bool Usable() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

std::string HrText(const char* what, HRESULT hr) {
    char text[128];
    std::snprintf(text, sizeof(text), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return text;
}

bool SetError(std::string* error, const std::string& text) noexcept {
    if (error != nullptr) {
        try {
            *error = text;
        } catch (...) {
        }
    }
    return false;
}

// Encodes `rows` (24-bit BGR, or 32-bit BGRA with `alpha` (G2 texture dump), tightly packed) as PNG into `path`.
HRESULT EncodePng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                  const std::vector<std::uint8_t>& rows, const char** step, bool alpha = false) {
    ComPtr<IWICImagingFactory> factory;
    *step = "CoCreateInstance(WICImagingFactory)";
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(hr)) {
        *step = "IWICImagingFactory::CreateStream";
        hr = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(hr)) {
        *step = "IWICStream::InitializeFromFilename";
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) {
        *step = "CreateEncoder(PNG)";
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(hr)) {
        *step = "IWICBitmapEncoder::Initialize";
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (SUCCEEDED(hr)) {
        *step = "CreateNewFrame";
        hr = encoder->CreateNewFrame(&frame, &properties);
    }
    if (SUCCEEDED(hr)) {
        *step = "IWICBitmapFrameEncode::Initialize";
        hr = frame->Initialize(properties.Get());
    }
    if (SUCCEEDED(hr)) {
        *step = "SetSize";
        hr = frame->SetSize(width, height);
    }
    const WICPixelFormatGUID wanted = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    WICPixelFormatGUID format = wanted;
    if (SUCCEEDED(hr)) {
        *step = "SetPixelFormat";
        hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && format != wanted) {
            hr = E_FAIL;  // the PNG encoder takes 24bpp BGR and 32bpp BGRA as they are
        }
    }
    if (SUCCEEDED(hr)) {
        *step = "WritePixels";
        const UINT stride = width * (alpha ? 4u : 3u);
        hr = frame->WritePixels(height, stride, static_cast<UINT>(rows.size()), const_cast<BYTE*>(rows.data()));
    }
    if (SUCCEEDED(hr)) {
        *step = "IWICBitmapFrameEncode::Commit";
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        *step = "IWICBitmapEncoder::Commit";
        hr = encoder->Commit();
    }
    return hr;
}

}  // namespace

bool WritePngFile(const std::filesystem::path& path, const gfx::rhi::Image& image, std::string* error) noexcept {
    try {
        using gfx::rhi::Format;
        const bool rgba = image.format == Format::RGBA8_UNorm;
        if (!rgba && image.format != Format::BGRA8_UNorm && image.format != Format::BGRX8_UNorm) {
            return SetError(error, std::string("unsupported image format ") + gfx::rhi::GetFormatInfo(image.format).name);
        }
        if (image.width == 0 || image.height == 0 || image.rowPitch < image.width * 4 ||
            image.pixels.size() < static_cast<std::size_t>(image.rowPitch) * image.height) {
            return SetError(error, "invalid image size");
        }
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) {
                return SetError(error, "cannot create " + path.parent_path().string() + ": " + ec.message());
            }
        }
        std::vector<std::uint8_t> rows(static_cast<std::size_t>(image.width) * 3 * image.height);
        for (std::uint32_t y = 0; y < image.height; ++y) {
            const std::uint8_t* src = image.pixels.data() + static_cast<std::size_t>(y) * image.rowPitch;
            std::uint8_t* dst = rows.data() + static_cast<std::size_t>(y) * image.width * 3;
            for (std::uint32_t x = 0; x < image.width; ++x, src += 4, dst += 3) {
                dst[0] = rgba ? src[2] : src[0];  // B
                dst[1] = src[1];                  // G
                dst[2] = rgba ? src[0] : src[2];  // R
            }
        }
        ComScope com;
        if (!com.Usable()) {
            return SetError(error, "CoInitializeEx failed");
        }
        const char* step = "";
        const HRESULT hr = EncodePng(path, image.width, image.height, rows, &step);
        if (FAILED(hr)) {
            std::filesystem::remove(path, ec);
            return SetError(error, HrText(step, hr));
        }
        return true;
    } catch (...) {
        return SetError(error, "out of memory");
    }
}

bool WritePngFileRgba(const std::filesystem::path& path, const gfx::rhi::Image& image, std::string* error) noexcept {
    try {
        using gfx::rhi::Format;
        const bool rgba = image.format == Format::RGBA8_UNorm;
        if (!rgba && image.format != Format::BGRA8_UNorm) {
            return SetError(error, std::string("unsupported image format ") + gfx::rhi::GetFormatInfo(image.format).name);
        }
        if (image.width == 0 || image.height == 0 || image.rowPitch < image.width * 4 ||
            image.pixels.size() < static_cast<std::size_t>(image.rowPitch) * image.height) {
            return SetError(error, "invalid image size");
        }
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) {
                return SetError(error, "cannot create " + path.parent_path().string() + ": " + ec.message());
            }
        }
        std::vector<std::uint8_t> rows(static_cast<std::size_t>(image.width) * 4 * image.height);
        for (std::uint32_t y = 0; y < image.height; ++y) {
            const std::uint8_t* src = image.pixels.data() + static_cast<std::size_t>(y) * image.rowPitch;
            std::uint8_t* dst = rows.data() + static_cast<std::size_t>(y) * image.width * 4;
            for (std::uint32_t x = 0; x < image.width; ++x, src += 4, dst += 4) {
                dst[0] = rgba ? src[2] : src[0];  // B
                dst[1] = src[1];                  // G
                dst[2] = rgba ? src[0] : src[2];  // R
                dst[3] = src[3];                  // A
            }
        }
        ComScope com;
        if (!com.Usable()) {
            return SetError(error, "CoInitializeEx failed");
        }
        const char* step = "";
        const HRESULT hr = EncodePng(path, image.width, image.height, rows, &step, true);
        if (FAILED(hr)) {
            std::filesystem::remove(path, ec);
            return SetError(error, HrText(step, hr));
        }
        return true;
    } catch (...) {
        return SetError(error, "out of memory");
    }
}

std::filesystem::path ScreenshotPath(const std::filesystem::path& directory, std::uint32_t frame) {
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06u.png", frame);
    return directory / name;
}

bool SaveScreenshot(gfx::rhi::Device& device, const std::filesystem::path& directory, std::uint32_t frame) noexcept {
    try {
        gfx::rhi::Image image;
        if (!device.CaptureBackBuffer(image)) {
            FUT_LOGE(Gfx, "screenshot of frame %u failed: %s", frame, device.LastError());
            return false;
        }
        const std::filesystem::path path = ScreenshotPath(directory, frame);
        std::string error;
        if (!WritePngFile(path, image, &error)) {
            FUT_LOGE(Gfx, "screenshot of frame %u failed: %s (%s)", frame, error.c_str(), path.string().c_str());
            return false;
        }
        FUT_LOGI(Gfx, "screenshot: %s (%ux%u)", path.string().c_str(), image.width, image.height);
        return true;
    } catch (...) {
        FUT_LOGE(Gfx, "screenshot of frame %u failed: out of memory", frame);
        return false;
    }
}

// ---- M3c: asynchronous screenshots --------------------------------------------------------------------------
namespace {

struct AsyncShots {
    std::mutex mutex;
    std::condition_variable done;
    std::uint32_t pending = 0;
    std::uint64_t written = 0;
    std::uint64_t failed = 0;
};

AsyncShots& Shots() noexcept {
    static AsyncShots shots;  // function-local: usable from static initialisation and at exit
    return shots;
}

void LogScreenshotTotals() noexcept {
    const bool flushed = FlushScreenshots(10000);
    AsyncShots& s = Shots();
    std::uint64_t written = 0, failed = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        written = s.written;
        failed = s.failed;
    }
    FUT_LOGI(Gfx, "screenshots: %llu written, %llu failed%s", static_cast<unsigned long long>(written),
             static_cast<unsigned long long>(failed), flushed ? "" : " (some were still being written at exit)");
}

// One detached worker per screenshot (a few per run): encode + write + log, then count it as done. Detached, so
// neither a normal exit (unit tests) nor TerminateProcess (ExitHostProcess) waits for or kills a joinable thread.
void WriteAsync(std::filesystem::path path, gfx::rhi::Image image, std::uint32_t frame) {
    AsyncShots& s = Shots();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        ++s.pending;
    }
    try {
        std::thread([path = std::move(path), image = std::move(image), frame]() noexcept {
            bool ok = false;
            try {
                core::LogSetThreadName("screenshot");
                std::string error;
                ok = WritePngFile(path, image, &error);
                if (ok) {
                    FUT_LOGI(Gfx, "screenshot: %s (%ux%u)", path.string().c_str(), image.width, image.height);
                } else {
                    FUT_LOGE(Gfx, "screenshot of frame %u failed: %s (%s)", frame, error.c_str(),
                             path.string().c_str());
                }
            } catch (...) {
                FUT_LOGE(Gfx, "screenshot of frame %u failed: out of memory", frame);
            }
            AsyncShots& shots = Shots();
            {
                std::lock_guard<std::mutex> lock(shots.mutex);
                --shots.pending;
                ++(ok ? shots.written : shots.failed);
            }
            shots.done.notify_all();
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(s.mutex);
        --s.pending;
        throw;
    }
}

}  // namespace

bool SaveScreenshot(gfx::rhi::Device& device, const std::filesystem::path& directory, std::uint32_t frame,
                    ScreenshotSource source) noexcept {
    try {
        static std::once_flag reporter;
        std::call_once(reporter, [] { core::AddExitReporter("screenshots", &LogScreenshotTotals); });
        gfx::rhi::Image image;
        const bool captured =
            source == ScreenshotSource::Output ? device.CaptureOutput(image) : device.CaptureBackBuffer(image);
        if (!captured) {
            FUT_LOGE(Gfx, "screenshot of frame %u failed: %s", frame, device.LastError());
            return false;
        }
        WriteAsync(ScreenshotPath(directory, frame), std::move(image), frame);
        return true;
    } catch (...) {
        FUT_LOGE(Gfx, "screenshot of frame %u failed: out of memory or no thread", frame);
        return false;
    }
}

bool FlushScreenshots(std::uint32_t timeoutMs) noexcept {
    try {
        AsyncShots& s = Shots();
        std::unique_lock<std::mutex> lock(s.mutex);
        return s.done.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&s] { return s.pending == 0; });
    } catch (...) {
        return false;
    }
}

bool ReadImageFile(const std::filesystem::path& path, gfx::rhi::Image& out, std::string* error) noexcept {
    try {
        ComScope com;
        if (!com.Usable()) {
            return SetError(error, "CoInitializeEx failed");
        }
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(hr)) {
            hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                    &decoder);
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(hr)) {
            hr = decoder->GetFrame(0, &frame);
        }
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(hr)) {
            hr = factory->CreateFormatConverter(&converter);
        }
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        }
        UINT w = 0, h = 0;
        if (SUCCEEDED(hr)) {
            hr = converter->GetSize(&w, &h);
        }
        if (SUCCEEDED(hr)) {
            out.format = gfx::rhi::Format::BGRA8_UNorm;
            out.width = w;
            out.height = h;
            out.rowPitch = w * 4;
            out.pixels.resize(static_cast<std::size_t>(out.rowPitch) * h);
            hr = converter->CopyPixels(nullptr, out.rowPitch, static_cast<UINT>(out.pixels.size()), out.pixels.data());
        }
        if (FAILED(hr)) {
            return SetError(error, HrText(("reading " + path.string()).c_str(), hr));
        }
        return true;
    } catch (...) {
        return SetError(error, "out of memory");
    }
}

}  // namespace platform
