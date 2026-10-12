// Capture tap XAPO and lock-free ring (see capture_tap.h).
#include "audio/capture_tap.h"

#include <xapo.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace snd::capture {

// ---- FloatRing -------------------------------------------------------------------------------------------------

bool FloatRing::Allocate(std::size_t minSamples) noexcept {
    std::size_t cap = 1024;
    while (cap < minSamples) {
        cap <<= 1;
    }
    data_.reset(new (std::nothrow) float[cap]);
    if (data_ == nullptr) {
        mask_ = 0;
        return false;
    }
    mask_ = cap - 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    return true;
}

std::size_t FloatRing::Available() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}

bool FloatRing::Write(const float* samples, std::size_t n) noexcept {
    if (data_ == nullptr) {
        return false;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (n > Capacity() - (head - tail)) {
        return false;
    }
    const std::size_t start = head & mask_;
    const std::size_t first = std::min(n, Capacity() - start);
    if (samples != nullptr) {
        std::memcpy(data_.get() + start, samples, first * sizeof(float));
        std::memcpy(data_.get(), samples + first, (n - first) * sizeof(float));
    } else {
        std::memset(data_.get() + start, 0, first * sizeof(float));
        std::memset(data_.get(), 0, (n - first) * sizeof(float));
    }
    head_.store(head + n, std::memory_order_release);
    return true;
}

std::size_t FloatRing::Read(float* out, std::size_t max) noexcept {
    if (data_ == nullptr) {
        return 0;
    }
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t n = std::min(max, head - tail);
    const std::size_t start = tail & mask_;
    const std::size_t first = std::min(n, Capacity() - start);
    std::memcpy(out, data_.get() + start, first * sizeof(float));
    std::memcpy(out + first, data_.get(), (n - first) * sizeof(float));
    tail_.store(tail + n, std::memory_order_release);
    return n;
}

// ---- tap XAPO --------------------------------------------------------------------------------------------------
namespace {

// {6E1F4C2B-5A3D-4F0E-9B71-2C8D5E4A9F10} - CLSID of the capture tap (registration properties only, not registered).
constexpr CLSID kTapClsid = {0x6E1F4C2B, 0x5A3D, 0x4F0E, {0x9B, 0x71, 0x2C, 0x8D, 0x5E, 0x4A, 0x9F, 0x10}};

std::atomic<std::uint64_t> g_processCalls{0};
std::atomic<std::uint64_t> g_frames{0};
std::atomic<std::uint64_t> g_silentBlocks{0};
std::atomic<std::uint32_t> g_lockedTaps{0};

bool IsFloat32(const WAVEFORMATEX* f) noexcept {
    if (f == nullptr || f->wBitsPerSample != 32) {
        return false;
    }
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00aa00389b71}
        return ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT && ext->SubFormat.Data2 == 0 &&
               ext->SubFormat.Data3 == 0x10;
    }
    return false;
}

bool ValidFormat(const WAVEFORMATEX* f) noexcept {
    return IsFloat32(f) && f->nChannels >= XAPO_MIN_CHANNELS && f->nChannels <= XAPO_MAX_CHANNELS &&
           f->nSamplesPerSec >= XAPO_MIN_FRAMERATE && f->nSamplesPerSec <= XAPO_MAX_FRAMERATE;
}

std::uint32_t MaskOf(const WAVEFORMATEX* f) noexcept {
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->dwChannelMask;
    }
    return 0;
}

// Copy of `reference` as a float32 format (XAPOAlloc), for the "nearest supported format" answers.
WAVEFORMATEX* NearestFloatFormat(const WAVEFORMATEX* reference) noexcept {
    auto* f = static_cast<WAVEFORMATEX*>(XAPOAlloc(sizeof(WAVEFORMATEX)));
    if (f == nullptr) {
        return nullptr;
    }
    const WORD ch = reference != nullptr && reference->nChannels >= 1 && reference->nChannels <= XAPO_MAX_CHANNELS
                        ? reference->nChannels
                        : 2;
    const DWORD rate = reference != nullptr && reference->nSamplesPerSec >= XAPO_MIN_FRAMERATE &&
                               reference->nSamplesPerSec <= XAPO_MAX_FRAMERATE
                           ? reference->nSamplesPerSec
                           : 48000;
    f->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f->nChannels = ch;
    f->nSamplesPerSec = rate;
    f->wBitsPerSample = 32;
    f->nBlockAlign = static_cast<WORD>(ch * 4);
    f->nAvgBytesPerSec = rate * f->nBlockAlign;
    f->cbSize = 0;
    return f;
}

class CaptureTap final : public IXAPO {
public:
    CaptureTap(Session& session, Stem stem) noexcept : session_(session), stem_(stem) {}

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IXAPO) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IXAPO*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs_; }
    STDMETHOD_(ULONG, Release)() override {
        const ULONG n = --refs_;
        if (n == 0) {
            delete this;
        }
        return n;
    }

    // IXAPO
    STDMETHOD(GetRegistrationProperties)(XAPO_REGISTRATION_PROPERTIES** pp) override {
        if (pp == nullptr) {
            return E_POINTER;
        }
        auto* p = static_cast<XAPO_REGISTRATION_PROPERTIES*>(XAPOAlloc(sizeof(XAPO_REGISTRATION_PROPERTIES)));
        if (p == nullptr) {
            return E_OUTOFMEMORY;
        }
        *p = Properties();
        *pp = p;
        return S_OK;
    }
    STDMETHOD(IsInputFormatSupported)(const WAVEFORMATEX* out, const WAVEFORMATEX* requested,
                                      WAVEFORMATEX** supported) override {
        return CheckPair(out, requested, supported);
    }
    STDMETHOD(IsOutputFormatSupported)(const WAVEFORMATEX* in, const WAVEFORMATEX* requested,
                                       WAVEFORMATEX** supported) override {
        return CheckPair(in, requested, supported);
    }
    STDMETHOD(Initialize)(const void*, UINT32) override { return S_OK; }
    STDMETHOD_(void, Reset)() override {}
    STDMETHOD(LockForProcess)(UINT32 inCount, const XAPO_LOCKFORPROCESS_PARAMETERS* in, UINT32 outCount,
                              const XAPO_LOCKFORPROCESS_PARAMETERS* out) override {
        if (inCount != 1 || outCount != 1 || in == nullptr || out == nullptr || !ValidFormat(in[0].pFormat) ||
            !ValidFormat(out[0].pFormat) || in[0].pFormat->nChannels != out[0].pFormat->nChannels ||
            in[0].pFormat->nSamplesPerSec != out[0].pFormat->nSamplesPerSec) {
            return XAPO_E_FORMAT_UNSUPPORTED;
        }
        channels_ = in[0].pFormat->nChannels;
        // The stem keeps its first format; a refused re-declaration only means this tap's frames are counted as
        // mismatched (Push checks the channel count). Capture never fails the voice.
        session_.DeclareStem(stem_, channels_, in[0].pFormat->nSamplesPerSec, MaskOf(in[0].pFormat));
        locked_ = true;
        ++g_lockedTaps;
        return S_OK;
    }
    STDMETHOD_(void, UnlockForProcess)() override {
        if (locked_) {
            locked_ = false;
            --g_lockedTaps;
        }
    }
    STDMETHOD_(void, Process)(UINT32 inCount, const XAPO_PROCESS_BUFFER_PARAMETERS* in, UINT32 outCount,
                              XAPO_PROCESS_BUFFER_PARAMETERS* out, BOOL) override {
        if (inCount != 1 || outCount != 1 || in == nullptr || out == nullptr) {
            return;
        }
        const UINT32 frames = in[0].ValidFrameCount;
        // In place: the output buffer is the input buffer; only the flags/frame count are passed on.
        out[0].BufferFlags = in[0].BufferFlags;
        out[0].ValidFrameCount = frames;
        if (in[0].pBuffer != out[0].pBuffer && in[0].BufferFlags == XAPO_BUFFER_VALID && out[0].pBuffer != nullptr) {
            std::memcpy(out[0].pBuffer, in[0].pBuffer, static_cast<std::size_t>(frames) * channels_ * sizeof(float));
        }
        ++g_processCalls;
        g_frames += frames;
        if (in[0].BufferFlags == XAPO_BUFFER_SILENT) {
            ++g_silentBlocks;
            session_.Push(stem_, nullptr, frames, channels_);
        } else {
            session_.Push(stem_, static_cast<const float*>(in[0].pBuffer), frames, channels_);
        }
    }
    STDMETHOD_(UINT32, CalcInputFrames)(UINT32 outputFrames) override { return outputFrames; }
    STDMETHOD_(UINT32, CalcOutputFrames)(UINT32 inputFrames) override { return inputFrames; }

private:
    ~CaptureTap() {
        UnlockForProcess();
    }

    static XAPO_REGISTRATION_PROPERTIES Properties() noexcept {
        XAPO_REGISTRATION_PROPERTIES p{};
        p.clsid = kTapClsid;
        wcscpy_s(p.FriendlyName, L"FuturamaPC capture tap");
        wcscpy_s(p.CopyrightInfo, L"FuturamaPC");
        p.MajorVersion = 1;
        p.MinorVersion = 0;
        p.Flags = XAPO_FLAG_CHANNELS_MUST_MATCH | XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
                  XAPO_FLAG_BUFFERCOUNT_MUST_MATCH | XAPO_FLAG_INPLACE_SUPPORTED | XAPO_FLAG_INPLACE_REQUIRED;
        p.MinInputBufferCount = 1;
        p.MaxInputBufferCount = 1;
        p.MinOutputBufferCount = 1;
        p.MaxOutputBufferCount = 1;
        return p;
    }

    static HRESULT CheckPair(const WAVEFORMATEX* other, const WAVEFORMATEX* requested, WAVEFORMATEX** supported) {
        if (other == nullptr || requested == nullptr) {
            return E_INVALIDARG;
        }
        const bool ok = ValidFormat(requested) && ValidFormat(other) && requested->nChannels == other->nChannels &&
                        requested->nSamplesPerSec == other->nSamplesPerSec;
        if (ok) {
            return S_OK;
        }
        if (supported != nullptr) {
            *supported = NearestFloatFormat(other);
        }
        return XAPO_E_FORMAT_UNSUPPORTED;
    }

    std::atomic<ULONG> refs_{1};
    Session& session_;
    Stem stem_;
    std::uint32_t channels_ = 0;
    bool locked_ = false;
};

}  // namespace

IUnknown* CreateCaptureTap(Session& session, Stem stem) noexcept {
    return static_cast<IXAPO*>(new (std::nothrow) CaptureTap(session, stem));
}

TapCounters GetTapCounters() noexcept {
    return TapCounters{g_processCalls.load(), g_frames.load(), g_silentBlocks.load(), g_lockedTaps.load()};
}

}  // namespace snd::capture
