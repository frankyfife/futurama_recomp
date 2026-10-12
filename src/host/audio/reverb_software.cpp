// The XAudio2 reverb XAPO driven directly (see reverb_software.h).
#include "audio/reverb_software.h"

#include "audio/reverb_i3dl2.h"
#include "core/log.h"

#include <xapo.h>
#include <xaudio2fx.h>

#include <algorithm>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <new>

namespace snd::reverb {
namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (ksmedia.h needs ks.h first; the GUID is the WAVE_FORMAT_IEEE_FLOAT
// subtype). SPEAKER_FRONT_CENTER (0x4) and SPEAKER_5POINT1 (FL FR FC LFE BL BR = 0x3F) of ksmedia.h.
constexpr DWORD kMaskMono = 0x4;
constexpr DWORD kMask51 = 0x3F;
constexpr GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

WAVEFORMATEXTENSIBLE FloatFormat(std::uint32_t rate, std::uint32_t channels) noexcept {
    WAVEFORMATEXTENSIBLE f = {};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = static_cast<WORD>(4 * channels);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = channels == 1 ? kMaskMono : kMask51;
    f.SubFormat = kSubtypeIeeeFloat;
    return f;
}

struct AlignedFree {
    void operator()(float* p) const noexcept { _aligned_free(p); }
};
using AlignedBuffer = std::unique_ptr<float, AlignedFree>;

AlignedBuffer AllocAligned(std::size_t floats) noexcept {
    return AlignedBuffer(static_cast<float*>(_aligned_malloc(floats * sizeof(float), 16)));
}

}  // namespace

struct SoftwareReverb::Impl {
    IXAPO* xapo = nullptr;
    IXAPOParameters* params = nullptr;
    bool locked = false;
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t block = 0;  // frames per Process call (10 ms)
    AlignedBuffer inBuffer;
    AlignedBuffer outBuffer;
    std::mutex pendingMutex;
    bool pendingValid = false;
    XAUDIO2FX_REVERB_PARAMETERS pending = {};

    ~Impl() {
        if (xapo != nullptr && locked) {
            xapo->UnlockForProcess();
        }
        if (params != nullptr) {
            params->Release();
        }
        if (xapo != nullptr) {
            xapo->Release();
        }
    }

    void ApplyPending() noexcept {
        XAUDIO2FX_REVERB_PARAMETERS p;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            if (!pendingValid) {
                return;
            }
            p = pending;
            pendingValid = false;
        }
        params->SetParameters(&p, sizeof(p));
    }
};

SoftwareReverb::SoftwareReverb() noexcept = default;
SoftwareReverb::~SoftwareReverb() = default;

bool SoftwareReverb::Initialize(std::uint32_t sampleRate, std::uint32_t outputChannels) noexcept {
    impl_.reset();
    if ((outputChannels != 1 && outputChannels != 6) || sampleRate < XAUDIO2FX_REVERB_MIN_FRAMERATE ||
        sampleRate > XAUDIO2FX_REVERB_MAX_FRAMERATE) {
        return false;
    }
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
    if (!impl) {
        return false;
    }
    IUnknown* unknown = nullptr;
    HRESULT hr = CreateReverbEffect(&unknown);
    if (FAILED(hr) || unknown == nullptr) {
        FUT_LOGW(Snd, "reverb: XAudio2CreateReverb failed (0x%08lX) - software mix without reverb",
                 static_cast<unsigned long>(hr));
        return false;
    }
    hr = unknown->QueryInterface(__uuidof(IXAPO), reinterpret_cast<void**>(&impl->xapo));
    if (SUCCEEDED(hr)) {
        hr = unknown->QueryInterface(__uuidof(IXAPOParameters), reinterpret_cast<void**>(&impl->params));
    }
    unknown->Release();
    if (FAILED(hr)) {
        FUT_LOGW(Snd, "reverb: reverb XAPO interfaces missing (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    hr = impl->xapo->Initialize(nullptr, 0);
    if (FAILED(hr)) {
        FUT_LOGW(Snd, "reverb: IXAPO::Initialize failed (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    impl->rate = sampleRate;
    impl->channels = outputChannels;
    impl->block = sampleRate / 100;
    const WAVEFORMATEXTENSIBLE inFormat = FloatFormat(sampleRate, kReverbInputChannels);
    const WAVEFORMATEXTENSIBLE outFormat = FloatFormat(sampleRate, outputChannels);
    XAPO_LOCKFORPROCESS_PARAMETERS inLock = {&inFormat.Format, impl->block};
    XAPO_LOCKFORPROCESS_PARAMETERS outLock = {&outFormat.Format, impl->block};
    hr = impl->xapo->LockForProcess(1, &inLock, 1, &outLock);
    if (FAILED(hr)) {
        FUT_LOGW(Snd, "reverb: IXAPO::LockForProcess(1 -> %u channels, %u Hz) failed (0x%08lX)",
                 outputChannels, sampleRate, static_cast<unsigned long>(hr));
        return false;
    }
    impl->locked = true;
    impl->inBuffer = AllocAligned(impl->block);
    impl->outBuffer = AllocAligned(static_cast<std::size_t>(impl->block) * outputChannels);
    if (!impl->inBuffer || !impl->outBuffer) {
        return false;
    }
    const I3dl2Listener generic;
    const XAUDIO2FX_REVERB_PARAMETERS p = ToNative(generic, false);
    impl->params->SetParameters(&p, sizeof(p));
    impl_ = std::move(impl);
    return true;
}

bool SoftwareReverb::Ready() const noexcept {
    return impl_ != nullptr;
}
std::uint32_t SoftwareReverb::OutputChannels() const noexcept {
    return impl_ ? impl_->channels : 0;
}
std::uint32_t SoftwareReverb::SampleRate() const noexcept {
    return impl_ ? impl_->rate : 0;
}
std::uint32_t SoftwareReverb::BlockFrames() const noexcept {
    return impl_ ? impl_->block : 0;
}

void SoftwareReverb::SetListener(const I3dl2Listener& listener) noexcept {
    if (!impl_) {
        return;
    }
    const XAUDIO2FX_REVERB_PARAMETERS p = ToNative(listener, false);
    std::lock_guard<std::mutex> lock(impl_->pendingMutex);
    impl_->pending = p;
    impl_->pendingValid = true;
}

void SoftwareReverb::Process(const float* in, float* out, std::uint32_t frames) noexcept {
    if (out == nullptr) {
        return;
    }
    if (!impl_) {
        std::fill(out, out + frames, 0.0f);  // channel count unknown: callers check Ready() first
        return;
    }
    Impl& m = *impl_;
    m.ApplyPending();
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t n = std::min(m.block, frames - done);
        bool silent = in == nullptr;
        if (!silent) {
            std::memcpy(m.inBuffer.get(), in + done, n * sizeof(float));
        }
        if (n < m.block || silent) {
            std::fill(m.inBuffer.get() + (silent ? 0 : n), m.inBuffer.get() + m.block, 0.0f);
        }
        XAPO_PROCESS_BUFFER_PARAMETERS inParams = {m.inBuffer.get(),
                                                   silent ? XAPO_BUFFER_SILENT : XAPO_BUFFER_VALID, m.block};
        XAPO_PROCESS_BUFFER_PARAMETERS outParams = {m.outBuffer.get(), XAPO_BUFFER_VALID, m.block};
        m.xapo->Process(1, &inParams, 1, &outParams, TRUE);
        if (outParams.BufferFlags == XAPO_BUFFER_SILENT) {
            std::fill(out + static_cast<std::size_t>(done) * m.channels,
                      out + static_cast<std::size_t>(done + n) * m.channels, 0.0f);
        } else {
            std::memcpy(out + static_cast<std::size_t>(done) * m.channels, m.outBuffer.get(),
                        static_cast<std::size_t>(n) * m.channels * sizeof(float));
        }
        done += n;
    }
}

void SoftwareReverb::Reset() noexcept {
    if (impl_) {
        impl_->xapo->Reset();
    }
}

}  // namespace snd::reverb
