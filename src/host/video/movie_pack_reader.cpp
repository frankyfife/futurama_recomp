// Movie pack reader on Media Foundation (M6b, see movie_pack_reader.h).
#include "video/movie_pack_reader.h"

#include "video/movie_pack.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace vid {
namespace {

constexpr std::size_t kFramesAhead = 6;  // decoded frames kept ahead of the XMV HLE (about 15 MB at 1280x960)

std::string Hr(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

template <typename T>
void SafeRelease(T*& p) noexcept {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace

struct PackVideoReader::Impl {
    std::filesystem::path file;
    std::uint32_t width = 0, height = 0;

    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<PackFrame> queue;
    bool stop = false;
    bool ended = false;
    bool failed = false;
    std::string error;
    std::string description;

    void Fail(std::string text) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failed) {
            failed = true;
            error = std::move(text);
        }
        cv.notify_all();
    }

    // Creates the source reader with YUY2 output. `processing`: let the reader insert a converter if the decoder
    // cannot output YUY2 itself.
    IMFSourceReader* CreateReader(bool processing, std::string& why) {
        IMFAttributes* attrs = nullptr;
        IMFSourceReader* reader = nullptr;
        IMFMediaType* type = nullptr;
        HRESULT hr = MFCreateAttributes(&attrs, 2);
        if (SUCCEEDED(hr) && processing) {
            hr = attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        }
        if (SUCCEEDED(hr)) {
            hr = MFCreateSourceReaderFromURL(file.wstring().c_str(), attrs, &reader);
            if (FAILED(hr)) {
                why = Hr("MFCreateSourceReaderFromURL", hr);
            }
        }
        if (SUCCEEDED(hr)) {
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
            if (FAILED(hr)) {
                why = Hr("no video stream: SetStreamSelection", hr);
            }
        }
        if (SUCCEEDED(hr)) {
            hr = MFCreateMediaType(&type);
        }
        if (SUCCEEDED(hr)) {
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2);
            hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type);
            if (FAILED(hr)) {
                why = Hr(processing ? "YUY2 output with video processing" : "YUY2 output of the decoder", hr);
            }
        }
        SafeRelease(type);
        SafeRelease(attrs);
        if (FAILED(hr)) {
            SafeRelease(reader);
        }
        return reader;
    }

    // Picture size and row pitch of the current output type.
    bool OutputFormat(IMFSourceReader* reader, UINT32& w, UINT32& h, LONG& stride, std::string& subtype) {
        IMFMediaType* type = nullptr;
        if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type))) {
            return false;
        }
        const bool ok = SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h));
        UINT32 s = 0;
        stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? static_cast<LONG>(s)
                                                                      : static_cast<LONG>(w * 2u);
        GUID g{};
        type->GetGUID(MF_MT_SUBTYPE, &g);
        subtype = g == MFVideoFormat_YUY2 ? "YUY2" : "?";
        SafeRelease(type);
        return ok;
    }

    void Run() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        IMFSourceReader* reader = nullptr;
        if (FAILED(mf)) {
            Fail(Hr("MFStartup (Media Foundation missing?)", mf));
        } else {
            std::string why1, why2;
            reader = CreateReader(false, why1);
            bool processing = false;
            if (reader == nullptr) {
                reader = CreateReader(true, why2);
                processing = true;
            }
            if (reader == nullptr) {
                Fail(why1 + "; " + why2);
            } else {
                Decode(reader, processing);
            }
        }
        SafeRelease(reader);
        if (SUCCEEDED(mf)) {
            MFShutdown();
        }
        if (SUCCEEDED(co)) {
            CoUninitialize();
        }
    }

    void Decode(IMFSourceReader* reader, bool processing) {
        UINT32 w = 0, h = 0;
        LONG stride = 0;
        std::string subtype;
        if (!OutputFormat(reader, w, h, stride, subtype) || w < width || h < height) {
            Fail("picture " + std::to_string(w) + "x" + std::to_string(h) + ", index says " + std::to_string(width) +
                 "x" + std::to_string(height));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            description = "Media Foundation source reader, output " + subtype +
                          (processing ? " (video processing)" : " (decoder)");
        }
        std::uint32_t index = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return stop || queue.size() < kFramesAhead; });
                if (stop) {
                    return;
                }
            }
            DWORD stream = 0, flags = 0;
            LONGLONG time = 0;
            IMFSample* sample = nullptr;
            const HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream,
                                                  &flags, &time, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
                SafeRelease(sample);
                Fail(Hr("ReadSample", FAILED(hr) ? hr : E_FAIL));
                return;
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0 &&
                (!OutputFormat(reader, w, h, stride, subtype) || w < width || h < height)) {
                SafeRelease(sample);
                Fail("output type changed to " + std::to_string(w) + "x" + std::to_string(h));
                return;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                SafeRelease(sample);
                std::lock_guard<std::mutex> lock(mutex);
                ended = true;
                cv.notify_all();
                return;
            }
            if (sample == nullptr) {
                continue;  // stream tick / gap
            }
            std::string copyError;
            auto image = std::make_shared<gfx::d3d::HostImage>();
            image->width = width;
            image->height = height;
            image->pitch = width * 2u;
            const bool copied = Copy(sample, stride, *image, copyError);
            SafeRelease(sample);
            if (!copied) {
                Fail(copyError);
                return;
            }
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back(PackFrame{index++, HnsToMs(time), std::move(image)});
            cv.notify_all();
        }
    }

    // Copies the top-left width x height YUY2 pixels of the sample.
    bool Copy(IMFSample* sample, LONG defaultStride, gfx::d3d::HostImage& out, std::string& why) {
        IMFMediaBuffer* buffer = nullptr;
        HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) {
            why = Hr("ConvertToContiguousBuffer", hr);
            return false;
        }
        try {
            out.yuy2.resize(static_cast<std::size_t>(out.pitch) * out.height);
        } catch (...) {
            SafeRelease(buffer);
            why = "out of memory";
            return false;
        }
        const std::size_t rowBytes = out.pitch;
        bool ok = false;
        IMF2DBuffer* buffer2d = nullptr;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d)))) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
                if (pitch > 0 && static_cast<std::size_t>(pitch) >= rowBytes) {
                    for (std::uint32_t y = 0; y < out.height; ++y) {
                        std::memcpy(out.yuy2.data() + y * rowBytes, scan0 + static_cast<std::size_t>(pitch) * y,
                                    rowBytes);
                    }
                    ok = true;
                }
                buffer2d->Unlock2D();
            }
            SafeRelease(buffer2d);
        }
        if (!ok) {
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                const std::size_t pitch = defaultStride > 0 ? static_cast<std::size_t>(defaultStride) : rowBytes;
                if (pitch >= rowBytes && static_cast<std::size_t>(length) >= pitch * (out.height - 1) + rowBytes) {
                    for (std::uint32_t y = 0; y < out.height; ++y) {
                        std::memcpy(out.yuy2.data() + y * rowBytes, data + pitch * y, rowBytes);
                    }
                    ok = true;
                }
                buffer->Unlock();
            }
            if (!ok) {
                why = "decoded buffer smaller than the picture";
            }
        }
        SafeRelease(buffer);
        return ok;
    }
};

PackVideoReader::PackVideoReader() : impl_(std::make_unique<Impl>()) {}

PackVideoReader::~PackVideoReader() {
    Close();
}

bool PackVideoReader::Open(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height,
                           std::string& error) {
    Close();
    impl_ = std::make_unique<Impl>();
    impl_->file = file;
    impl_->width = width;
    impl_->height = height;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        error = "file missing";
        return false;
    }
    try {
        impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
    } catch (...) {
        error = "no worker thread";
        return false;
    }
    return true;
}

PackVideoReader::Result PackVideoReader::Get(std::uint32_t index, std::uint32_t timeoutMs, PackFrame& out,
                                             std::string& error) {
    Impl& d = *impl_;
    std::unique_lock<std::mutex> lock(d.mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        while (!d.queue.empty()) {
            switch (PackStepFor(d.queue.front().index, index)) {
            case PackQueueStep::Discard:
                d.queue.pop_front();
                d.cv.notify_all();
                break;
            case PackQueueStep::Take:
                out = std::move(d.queue.front());
                d.queue.pop_front();
                d.cv.notify_all();
                return Result::Ok;
            case PackQueueStep::Ahead:
                error = "frame " + std::to_string(index) + " lost (oldest decoded " +
                        std::to_string(d.queue.front().index) + ")";
                return Result::Error;
            }
        }
        if (d.failed) {
            error = d.error;
            return Result::Error;
        }
        if (d.ended) {
            return Result::End;
        }
        if (!d.worker.joinable()) {
            error = "not open";
            return Result::Error;
        }
        if (d.cv.wait_until(lock, deadline) == std::cv_status::timeout && d.queue.empty() && !d.failed &&
            !d.ended) {
            return Result::Timeout;
        }
    }
}

void PackVideoReader::WaitReady(std::uint32_t timeoutMs) {
    Impl& d = *impl_;
    std::unique_lock<std::mutex> lock(d.mutex);
    d.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                  [&] { return !d.queue.empty() || d.failed || d.ended || !d.worker.joinable(); });
}

void PackVideoReader::Close() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
        impl_->cv.notify_all();
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

std::string PackVideoReader::Description() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->description;
}

}  // namespace vid
