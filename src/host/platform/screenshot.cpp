// Screenshots (screenshot.h): PNG through the Windows Imaging Component.
#include "platform/screenshot.h"

#include "core/log.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <system_error>

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

// Encodes `rows` (24-bit BGR, tightly packed) as PNG into `path`.
HRESULT EncodePng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                  const std::vector<std::uint8_t>& rows, const char** step) {
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
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) {
        *step = "SetPixelFormat";
        hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && format != GUID_WICPixelFormat24bppBGR) {
            hr = E_FAIL;  // the PNG encoder always takes 24bpp BGR
        }
    }
    if (SUCCEEDED(hr)) {
        *step = "WritePixels";
        hr = frame->WritePixels(height, width * 3, static_cast<UINT>(rows.size()), const_cast<BYTE*>(rows.data()));
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
