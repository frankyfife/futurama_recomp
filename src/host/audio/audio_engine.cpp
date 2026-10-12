// Audio engine: XAudio2 2.9 backend, software mixer, device-loss switching (see audio_engine.h for the structure;
// analysis/loader/m5_audio.md 6.1 graph, 6.6 reverb and levels, 6.8 capture and operation without a device, 7
// threading). M5 DSOUND core agent.
//
// Thread rules (design E10): the facade, voice creation/destruction and Pump run on the guest thread; XAudio2's
// engine thread and the software mixer thread only call VoiceEvents::OnBufferEnd and the taps; the device worker
// only creates and destroys whole backends that carry no voice at that moment. A proxy voice's
// mutex only guards its list of queued buffers and is never held while calling into a backend, and the software
// mixer calls VoiceEvents with its own mutex held - so the lock order is always backend -> proxy -> DSOUND event
// queue, never the reverse.
#include "audio/audio_engine.h"

#include "audio/reverb_i3dl2.h"
#include "audio/reverb_software.h"
#include "core/config.h"
#include "core/log.h"

#include <mmdeviceapi.h>
#include <xapo.h>
#include <xaudio2.h>
#include <xaudio2fx.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace snd {

// ---- shared helpers -------------------------------------------------------------------------------------------------

const char* CategoryName(Category category) noexcept {
    switch (category) {
        case Category::Effects:
            return "effects";
        case Category::Speech:
            return "speech";
        case Category::Music:
            return "music";
        case Category::Movie:
            return "movie";
    }
    return "?";
}

// FUN_0024C3CA: att = -(total mB) clamped to >= 0, register (att << 6) / 100 in 1/64 dB, at most 0xFFF.
float MixbinGain(std::int32_t totalMillibel) noexcept {
    std::int64_t att = -static_cast<std::int64_t>(totalMillibel);
    if (att < 0) {
        att = 0;
    }
    const std::int64_t reg = std::min<std::int64_t>((att << 6) / 100, 0xFFF);
    return static_cast<float>(std::pow(10.0, -static_cast<double>(reg) / 64.0 / 20.0));
}

float MillibelToGain(std::int32_t millibel) noexcept {
    if (millibel <= kMillibelMin) {
        return 0.0f;
    }
    if (millibel >= 0) {
        return 1.0f;
    }
    return static_cast<float>(std::pow(10.0, static_cast<double>(millibel) / 2000.0));
}

std::uint32_t DefaultChannelMask(std::uint32_t channels) noexcept {
    switch (channels) {
        case 1:
            return 0x4;  // SPEAKER_MONO (front centre)
        case 2:
            return 0x3;  // SPEAKER_STEREO
        case 4:
            return 0x33;  // SPEAKER_QUAD
        case 6:
            return 0x3F;  // SPEAKER_5POINT1
        case 8:
            return 0x63F;  // SPEAKER_7POINT1_SURROUND
        default:
            return channels >= 32 ? 0xFFFFFFFFu : ((1u << channels) - 1u);
    }
}

bool ParseSpeakers(std::string_view text, std::uint32_t& channels) noexcept {
    std::string t;
    for (const char c : text) {
        if (c != ' ' && c != '\t') {
            t.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
        }
    }
    if (t.empty() || t == "auto") {
        channels = 0;
        return true;
    }
    if (t == "stereo" || t == "2.0") {
        channels = 2;
        return true;
    }
    if (t == "5.1") {
        channels = 6;
        return true;
    }
    return false;
}

namespace {

std::int64_t Qpc() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
std::int64_t Qpf() noexcept {
    static const std::int64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q.QuadPart;
    }();
    return f;
}

// ---- Windows render endpoints (MMDevice API) -------------------------------------------------------------------------
// PKEY_Device_FriendlyName of functiondiscoverykeys_devpkey.h (spelled out: no initguid.h in this file).
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

struct EndpointInfo {
    bool present = false;  // an active render endpoint
    std::wstring id;       // IMMDevice::GetId ("{0.0.0.00000000}.{guid}", the format [audio] device stores)
    std::string name;      // friendly name (UTF-8, "" if unknown)
};

// The Windows default render endpoint (console role) for an empty `id`, else the endpoint `id` if it is active.
// COM (MTA) is initialised for the call; any thread but a mixer thread.
EndpointInfo QueryEndpoint(const std::wstring& id) noexcept {
    EndpointInfo info;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* e = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&e)))) {
        IMMDevice* d = nullptr;
        const HRESULT hr =
            id.empty() ? e->GetDefaultAudioEndpoint(eRender, eConsole, &d) : e->GetDevice(id.c_str(), &d);
        DWORD state = 0;
        if (SUCCEEDED(hr) && d != nullptr && SUCCEEDED(d->GetState(&state)) && state == DEVICE_STATE_ACTIVE) {
            info.present = true;
            LPWSTR text = nullptr;
            if (SUCCEEDED(d->GetId(&text)) && text != nullptr) {
                try {
                    info.id = text;
                } catch (...) {
                }
                CoTaskMemFree(text);
            }
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) && props != nullptr) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, &v)) && v.vt == VT_LPWSTR &&
                    v.pwszVal != nullptr) {
                    try {
                        info.name = core::WideToUtf8(v.pwszVal);
                    } catch (...) {
                    }
                }
                PropVariantClear(&v);
                props->Release();
            }
        }
        if (d != nullptr) {
            d->Release();
        }
        e->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return info;
}

bool SameEndpointId(const std::wstring& a, const std::wstring& b) noexcept {
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                                        static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

TapPoint TapOfCategory(Category c) noexcept {
    return static_cast<TapPoint>(static_cast<std::uint8_t>(TapPoint::Effects) + static_cast<std::uint8_t>(c));
}

// Guarded MixTap pointer: SetTap(nullptr) waits until no OnMix call is running.
class TapSlot {
public:
    void Set(MixTap* tap) noexcept {
        AcquireSRWLockExclusive(&lock_);
        tap_ = tap;
        ReleaseSRWLockExclusive(&lock_);
    }
    void Call(TapPoint point, const float* samples, std::uint32_t frames, std::uint32_t channels,
              std::uint32_t rate) noexcept {
        AcquireSRWLockShared(&lock_);
        if (tap_ != nullptr) {
            tap_->OnMix(point, samples, frames, channels, rate);
        }
        ReleaseSRWLockShared(&lock_);
    }
    bool Has() noexcept {
        AcquireSRWLockShared(&lock_);
        const bool has = tap_ != nullptr;
        ReleaseSRWLockShared(&lock_);
        return has;
    }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
    MixTap* tap_ = nullptr;
};

std::atomic<EngineObserver*>& Observer() noexcept {
    static std::atomic<EngineObserver*> observer{nullptr};
    return observer;
}
void NotifyBackendChanged(AudioEngine& engine) noexcept {
    if (EngineObserver* o = Observer().load()) {
        o->OnBackendChanged(engine);
    }
}

// Master stage (last step of the mix, after the Master tap): peak limiter ([audio] limiter) and master volume. The
// limiter only acts on peaks above -0.1 dBFS (instant attack, 80 ms release; transparent below), so levels stay
// exact for the verification. FXMasteringLimiter is not used: it also applies a loudness makeup gain (measured +25 dB
// on a quiet signal). XAudio2 applies a mastering voice's own volume BEFORE its effect chain (measured), so the master
// volume is applied here as well. One caller at a time (mixer thread).
class MasterStage {
public:
    void Configure(bool limiter, std::uint32_t rate, std::uint32_t channelMask = 0) noexcept {
        limiter_ = limiter;
        release_ = 1.0f - static_cast<float>(std::exp(-1.0 / (0.080 * static_cast<double>(std::max(1u, rate)))));
        // LFE low-pass (4th-order Butterworth at 120 Hz = two biquads, Q 0.5412 and 1.3066): the game feeds every 3D
        // voice's full-band signal into the LFE mixbin (bin 3, lVolume + distance); on the Xbox only the Dolby Digital
        // encoder carries that channel, and AC-3 codes LFE up to 120 Hz. Without it a TV that folds LFE into its
        // speakers plays every footstep twice (measured: LFE was the loudest channel of the effects stem).
        lfeIndex_ = -1;
        constexpr std::uint32_t kSpeakerLfe = 0x8;
        if ((channelMask & kSpeakerLfe) != 0) {
            std::uint32_t below = channelMask & (kSpeakerLfe - 1u);
            int index = 0;
            for (; below != 0; below &= below - 1u) {
                ++index;
            }
            lfeIndex_ = index;
        }
        constexpr double kPi = 3.14159265358979323846;
        const double w = 2.0 * kPi * 120.0 / static_cast<double>(std::max(1u, rate));
        const double q[2] = {0.54119610, 1.30656296};
        for (int i = 0; i < 2; ++i) {
            const double alpha = std::sin(w) / (2.0 * q[i]);
            const double cw = std::cos(w);
            const double a0 = 1.0 + alpha;
            lfe_[i].b0 = static_cast<float>((1.0 - cw) / 2.0 / a0);
            lfe_[i].b1 = static_cast<float>((1.0 - cw) / a0);
            lfe_[i].b2 = lfe_[i].b0;
            lfe_[i].a1 = static_cast<float>(-2.0 * cw / a0);
            lfe_[i].a2 = static_cast<float>((1.0 - alpha) / a0);
            lfe_[i].z1 = lfe_[i].z2 = 0.0f;
        }
    }
    // Low-passes the LFE channel in place (before the Master tap, so captures hold what the device gets).
    void FilterLfe(float* x, std::uint32_t frames, std::uint32_t channels) noexcept {
        if (lfeIndex_ < 0 || static_cast<std::uint32_t>(lfeIndex_) >= channels) {
            return;
        }
        for (std::uint32_t f = 0; f < frames; ++f) {
            float& v = x[static_cast<std::size_t>(f) * channels + static_cast<std::uint32_t>(lfeIndex_)];
            float y = v;
            for (Biquad& b : lfe_) {
                const float out = b.b0 * y + b.z1;  // transposed direct form II
                b.z1 = b.b1 * y - b.a1 * out + b.z2;
                b.z2 = b.b2 * y - b.a2 * out;
                y = out;
            }
            v = y;
        }
    }
    int LfeIndex() const noexcept { return lfeIndex_; }
    void SetGain(float gain) noexcept { gain_.store(gain, std::memory_order_relaxed); }
    bool LimiterOn() const noexcept { return limiter_; }
    void Process(float* x, std::uint32_t frames, std::uint32_t channels) noexcept {
        const float gain = gain_.load(std::memory_order_relaxed);
        if (!limiter_) {
            if (gain != 1.0f) {
                for (std::size_t i = 0, n = static_cast<std::size_t>(frames) * channels; i < n; ++i) {
                    x[i] *= gain;
                }
            }
            return;
        }
        constexpr float kThreshold = 0.98855309f;  // -0.1 dBFS
        float g = envelope_;
        float minGain = 1.0f;
        std::uint64_t limited = 0;
        for (std::uint32_t f = 0; f < frames; ++f) {
            float* frame = x + static_cast<std::size_t>(f) * channels;
            float peak = 0.0f;
            for (std::uint32_t c = 0; c < channels; ++c) {
                peak = std::max(peak, std::fabs(frame[c]));
            }
            peak *= gain;
            const float target = peak > kThreshold ? kThreshold / peak : 1.0f;
            g = target < g ? target : std::min(1.0f, g + (1.0f - g) * release_);
            if (g < 0.99885f) {  // at least 0.01 dB of reduction
                ++limited;
                minGain = std::min(minGain, g);
            }
            const float k = g * gain;
            for (std::uint32_t c = 0; c < channels; ++c) {
                frame[c] *= k;
            }
        }
        envelope_ = g;
        if (limited != 0) {
            limited_.fetch_add(limited, std::memory_order_relaxed);
            float old = minGain_.load(std::memory_order_relaxed);
            while (minGain < old && !minGain_.compare_exchange_weak(old, minGain, std::memory_order_relaxed)) {
            }
        }
    }
    float MaxReductionDb() const noexcept {
        return std::max(0.0f, -20.0f * std::log10(std::max(1e-6f, minGain_.load(std::memory_order_relaxed))));
    }
    std::uint64_t LimitedFrames() const noexcept { return limited_.load(std::memory_order_relaxed); }

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    };
    Biquad lfe_[2];
    int lfeIndex_ = -1;
    bool limiter_ = false;
    float release_ = 0.0003f;
    float envelope_ = 1.0f;
    std::atomic<float> gain_{1.0f};
    std::atomic<float> minGain_{1.0f};
    std::atomic<std::uint64_t> limited_{0};
};

// ---- backend interface (internal) ------------------------------------------------------------------------------
class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* Name() const noexcept = 0;
    virtual bool HasDevice() const noexcept = 0;
    virtual std::uint32_t Channels() const noexcept = 0;
    virtual std::uint32_t ChannelMask() const noexcept = 0;
    virtual std::uint32_t SampleRate() const noexcept = 0;
    virtual bool TapFormat(TapPoint point, std::uint32_t& channels, std::uint32_t& sampleRate) const noexcept = 0;
    virtual std::unique_ptr<SourceVoice> CreateVoice(const VoiceDesc& desc) noexcept = 0;
    virtual void SetReverb(const I3dl2Listener& listener) noexcept = 0;
    virtual void SetMasterVolume(float gain) noexcept = 0;
    virtual void BeginBatch() noexcept {}
    virtual void CommitBatch() noexcept {}
    virtual void SetTap(MixTap* tap) noexcept = 0;
    virtual std::uint32_t RenderSoftware(std::uint32_t frames, float* out) noexcept = 0;
    virtual bool Lost() const noexcept { return false; }
    virtual void SimulateLoss() noexcept {}
    virtual std::uint32_t Glitches() const noexcept { return 0; }
    virtual float LimiterMaxReductionDb() const noexcept = 0;
    virtual std::uint64_t LimitedFrames() const noexcept = 0;
    virtual std::uint64_t FramesRendered() const noexcept = 0;
    virtual std::string Description() const = 0;
};

// =================================================================================================================
// Software mixer
// =================================================================================================================
// Resampler: 16-tap windowed sinc (Kaiser, beta 8) at 512 fractional phases, rows normalized to unit DC gain; full
// source bandwidth (pitching up above the output rate aliases, like a plain interpolator would). Every voice keeps a
// ring of the last source frames it pulled from its buffer queue, so buffer and packet boundaries are seamless.
// A buffer counts as played (OnBufferEnd) when the read position passes its last frame - the lookahead of the filter
// does not move the event (unit tests rely on exact frame counts).
constexpr int kSincTaps = 16;
constexpr int kSincHalf = kSincTaps / 2;   // taps at offsets -7..8 around floor(pos)
constexpr int kSincPhases = 512;
constexpr std::uint32_t kRingFrames = 32;  // >= kSincTaps + 2 (power of two)

const float* SincTable() noexcept {
    static const std::vector<float> table = [] {
        std::vector<float> t(static_cast<std::size_t>(kSincPhases) * kSincTaps);
        const auto besselI0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        constexpr double kBeta = 8.0;
        constexpr double kPi = 3.14159265358979323846;
        const double i0Beta = besselI0(kBeta);
        for (int p = 0; p < kSincPhases; ++p) {
            const double frac = static_cast<double>(p) / kSincPhases;
            double sum = 0.0;
            double row[kSincTaps];
            for (int k = 0; k < kSincTaps; ++k) {
                const double x = static_cast<double>(k - (kSincHalf - 1)) - frac;  // tap offset -7..8 minus frac
                const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double r = x / static_cast<double>(kSincHalf);
                const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(kBeta * std::sqrt(1.0 - r * r)) / i0Beta;
                row[k] = sinc * w;
                sum += row[k];
            }
            for (int k = 0; k < kSincTaps; ++k) {
                t[static_cast<std::size_t>(p) * kSincTaps + k] = static_cast<float>(row[k] / sum);
            }
        }
        return t;
    }();
    return table.data();
}

class SoftBackend;

class SoftVoice final : public SourceVoice {
public:
    SoftVoice(SoftBackend& backend, const VoiceDesc& desc, std::uint32_t outChannels) noexcept;
    ~SoftVoice() override;
    bool Submit(const SubmitDesc& buffer) noexcept override;
    void Start() noexcept override;
    void Stop() noexcept override;
    void Flush() noexcept override;
    void Discontinuity() noexcept override {}
    std::uint64_t FramesPlayed() noexcept override;
    std::uint32_t BuffersQueued() noexcept override;
    void SetFrequencyRatio(float ratio) noexcept override;
    void SetOutputMatrix(std::span<const float> levels) noexcept override;
    void SetReverbSend(float gain) noexcept override;

    // State (guarded by the backend mutex). Frames are numbered by the order in which they were pulled from the
    // queue ("absolute" frames); `pos` is the fractional read position in that numbering.
    struct Buf {
        SubmitDesc d;
        std::uint32_t next = 0;  // next frame to pull
    };
    struct PendingEnd {
        std::uint64_t at = 0;  // absolute frame after the buffer's last frame
        std::uint32_t context = 0;
    };
    // Pulls one frame into the ring; false when the queue is empty.
    bool Pull() noexcept {
        if (queue.empty()) {
            return false;
        }
        Buf& b = queue.front();
        const std::int16_t* src = b.d.data + static_cast<std::size_t>(b.next) * channels;
        float* dst = &ring[static_cast<std::size_t>(pulled % kRingFrames) * channels];
        for (std::uint32_t c = 0; c < channels; ++c) {
            dst[c] = static_cast<float>(src[c]) * (1.0f / 32768.0f);
        }
        ++pulled;
        if (++b.next >= b.d.frames) {
            if (b.d.loop) {
                b.next = 0;  // loop over the whole buffer (XAUDIO2_LOOP_INFINITE, LoopLength 0)
            } else {
                try {
                    ends.push_back({pulled, b.d.context});
                } catch (...) {
                }
                queue.pop_front();
            }
        }
        return true;
    }
    float At(std::int64_t frame, std::uint32_t c) const noexcept {
        if (frame < 0 || static_cast<std::uint64_t>(frame) >= pulled ||
            static_cast<std::uint64_t>(frame) + kRingFrames <= pulled) {
            return 0.0f;
        }
        return ring[static_cast<std::size_t>(static_cast<std::uint64_t>(frame) % kRingFrames) * channels + c];
    }

    SoftBackend& backend;
    VoiceEvents* events;
    Category category;
    std::uint32_t channels;
    std::uint32_t rate;
    float maxRatio;
    bool reverbSend;
    std::deque<Buf> queue;
    std::deque<PendingEnd> ends;         // fully pulled, not yet played
    std::vector<std::uint32_t> flushed;  // contexts reported at the next render (asynchronous like XAudio2)
    std::vector<float> ring;             // kRingFrames x channels
    std::uint64_t pulled = 0;
    double pos = 0.0;
    bool started = false;
    float ratio = 1.0f;
    std::vector<float> matrix;  // channels x outChannels
    float send = 0.0f;
};

class SoftBackend final : public Backend {
public:
    SoftBackend(const EngineConfig& config, std::uint32_t channels) noexcept
        : channels_(std::max<std::uint32_t>(1, std::min(channels, kMaxOutputChannels))),
          rate_(config.softwareSampleRate != 0 ? config.softwareSampleRate : 48000),
          realtime_(config.softwareRealtime) {
        SincTable();
        stage_.Configure(config.limiter, rate_, DefaultChannelMask(channels_));
        if (config.reverb && reverb::ReverbSampleRate(rate_) == rate_) {
            reverbChannels_ = reverb::ReverbEffectChannels(channels_);
            reverbOn_ = reverbFx_.Initialize(rate_, reverbChannels_) &&
                        reverb::ReverbOutputMatrix(reverbChannels_, channels_, reverbMatrix_, std::size(reverbMatrix_));
        }
        if (realtime_) {
            try {
                thread_ = std::thread([this] { ThreadMain(); });
            } catch (...) {
                realtime_ = false;
            }
        }
    }
    ~SoftBackend() override {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    const char* Name() const noexcept override { return "software"; }
    bool HasDevice() const noexcept override { return false; }
    std::uint32_t Channels() const noexcept override { return channels_; }
    std::uint32_t ChannelMask() const noexcept override { return DefaultChannelMask(channels_); }
    std::uint32_t SampleRate() const noexcept override { return rate_; }
    bool TapFormat(TapPoint point, std::uint32_t& channels, std::uint32_t& sampleRate) const noexcept override {
        if (point >= TapPoint::Count) {
            return false;
        }
        // Reverb: the effect output, or the dry mono send without reverb (Render).
        channels = point == TapPoint::Reverb ? (reverbOn_ ? reverbChannels_ : 1u) : channels_;
        sampleRate = rate_;
        return true;
    }
    std::unique_ptr<SourceVoice> CreateVoice(const VoiceDesc& desc) noexcept override {
        if (desc.format.channels == 0 || desc.format.channels > kMaxOutputChannels || desc.format.sampleRate == 0) {
            return nullptr;
        }
        try {
            auto v = std::make_unique<SoftVoice>(*this, desc, channels_);
            std::lock_guard<std::mutex> lock(mutex_);
            voices_.push_back(v.get());
            return v;
        } catch (...) {
            return nullptr;
        }
    }
    void SetReverb(const I3dl2Listener& listener) noexcept override {
        if (reverbOn_) {
            reverbFx_.SetListener(listener);  // handed over under the reverb's own lock
        }
    }
    void SetMasterVolume(float gain) noexcept override { stage_.SetGain(gain); }
    void SetTap(MixTap* tap) noexcept override { tap_.Set(tap); }
    std::uint32_t RenderSoftware(std::uint32_t frames, float* out) noexcept override {
        std::uint32_t done = 0;
        while (done < frames) {
            const std::uint32_t n = std::min<std::uint32_t>(frames - done, kBlock);
            Render(n, out != nullptr ? out + static_cast<std::size_t>(done) * channels_ : nullptr);
            done += n;
        }
        return done;
    }
    std::uint64_t FramesRendered() const noexcept override { return rendered_.load(); }
    float LimiterMaxReductionDb() const noexcept override { return stage_.MaxReductionDb(); }
    std::uint64_t LimitedFrames() const noexcept override { return stage_.LimitedFrames(); }
    std::string Description() const override {
        return std::string("software mixer (no output), ") + std::to_string(channels_) + " ch " +
               std::to_string(rate_) + " Hz" + (realtime_ ? ", real time" : ", rendered on demand") +
               (reverbOn_ ? ", reverb on" : ", reverb off") + (stage_.LimiterOn() ? ", limiter on" : ", limiter off") +
               ", " + std::to_string(lightBlocks_.load()) + " blocks positions only (no listener)";
    }

    void Remove(SoftVoice* v) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.erase(std::remove(voices_.begin(), voices_.end(), v), voices_.end());
    }
    std::mutex& Mutex() noexcept { return mutex_; }

private:
    static constexpr std::uint32_t kBlock = 480;  // 10 ms at 48 kHz

    void ThreadMain() noexcept {
        SetThreadDescription(GetCurrentThread(), L"audio software mixer");
        const std::int64_t start = Qpc();
        std::uint64_t rendered = 0;
        while (!stop_.load()) {
            const std::int64_t elapsed = Qpc() - start;
            const std::uint64_t due =
                static_cast<std::uint64_t>(static_cast<double>(elapsed) * rate_ / static_cast<double>(Qpf()));
            if (due >= rendered + kBlock) {
                // Catch up at most 100 ms at once (e.g. after a debugger break), then continue from now.
                std::uint64_t missing = due - rendered;
                if (missing > 10ull * kBlock) {
                    rendered = due - 10ull * kBlock;
                    missing = 10ull * kBlock;
                }
                while (missing >= kBlock) {
                    Render(kBlock, nullptr);
                    rendered += kBlock;
                    missing -= kBlock;
                }
            } else {
                Sleep(2);
            }
        }
    }

    void FireEnds(SoftVoice& v) noexcept {
        while (!v.ends.empty() && v.pos >= static_cast<double>(v.ends.front().at)) {
            const std::uint32_t context = v.ends.front().context;
            v.ends.pop_front();
            if (v.events != nullptr) {
                v.events->OnBufferEnd(context);
            }
        }
    }

    // `render` false: only the read position, the pulls and the buffer ends advance (exactly as when rendering).
    void MixVoice(SoftVoice& v, std::uint32_t frames, bool render) noexcept {
        const double step = static_cast<double>(v.rate) * static_cast<double>(v.ratio) / static_cast<double>(rate_);
        float* bus = render ? buses_[static_cast<std::size_t>(v.category)].data() : nullptr;
        const bool toReverb = v.reverbSend && v.send != 0.0f;
        const float* table = SincTable();
        float x[kMaxOutputChannels];
        for (std::uint32_t f = 0; f < frames; ++f) {
            const auto ip = static_cast<std::int64_t>(v.pos);
            while (v.pulled <= static_cast<std::uint64_t>(ip + kSincHalf) && v.Pull()) {
            }
            if (v.pos >= static_cast<double>(v.pulled)) {
                break;  // starved: no source frame left at the read position (silence, position kept)
            }
            if (!render) {
                v.pos += step;
                FireEnds(v);
                continue;
            }
            const double frac = v.pos - static_cast<double>(ip);
            const int phase = std::min(kSincPhases - 1, static_cast<int>(frac * kSincPhases));
            const float* coef = table + static_cast<std::size_t>(phase) * kSincTaps;
            for (std::uint32_t c = 0; c < v.channels; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < kSincTaps; ++k) {
                    acc += coef[k] * v.At(ip + k - (kSincHalf - 1), c);
                }
                x[c] = acc;
            }
            float* o = bus + static_cast<std::size_t>(f) * channels_;
            for (std::uint32_t c = 0; c < v.channels; ++c) {
                const float* row = &v.matrix[static_cast<std::size_t>(c) * channels_];
                for (std::uint32_t d = 0; d < channels_; ++d) {
                    o[d] += x[c] * row[d];
                }
                if (toReverb) {
                    reverb_[f] += x[c] * v.send;
                }
            }
            v.pos += step;
            FireEnds(v);
        }
    }

    void ReportFlushed(SoftVoice& v) noexcept {
        if (v.flushed.empty()) {
            return;
        }
        const std::vector<std::uint32_t> flushed = std::move(v.flushed);
        v.flushed.clear();
        for (const std::uint32_t context : flushed) {
            if (v.events != nullptr) {
                v.events->OnBufferEnd(context);
            }
        }
    }

    void Render(std::uint32_t frames, float* out) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        // Nobody listens (no output buffer and no tap: no device and no capture_wav, e.g. [audio] enabled = 0 or the
        // TV is off): only the play positions and buffer ends advance - the same frames and OnBufferEnd moments as a
        // full mix, without resampling, reverb, LFE filter, limiter (about 20 % of the process's CPU time otherwise).
        if (out == nullptr && !tap_.Has()) {
            for (SoftVoice* v : voices_) {
                ReportFlushed(*v);
                if (v->started) {
                    MixVoice(*v, frames, false);
                }
            }
            ++lightBlocks_;
            rendered_.fetch_add(frames);
            return;
        }
        const std::size_t n = static_cast<std::size_t>(frames) * channels_;
        try {
            for (auto& b : buses_) {
                b.assign(n, 0.0f);
            }
            reverb_.assign(frames, 0.0f);
            mix_.assign(n, 0.0f);
        } catch (...) {
            return;
        }
        for (SoftVoice* v : voices_) {
            ReportFlushed(*v);
            if (v->started) {
                MixVoice(*v, frames, true);
            }
        }
        for (std::uint32_t c = 0; c < kCategoryCount; ++c) {
            const float* bus = buses_[c].data();
            for (std::size_t i = 0; i < n; ++i) {
                mix_[i] += bus[i];
            }
        }
        // Reverb: the mono send through the XAudio2 reverb XAPO (SoftwareReverb), effect channels to the output.
        const float* reverbTap = reverb_.data();
        std::uint32_t reverbTapChannels = 1;
        if (reverbOn_) {
            try {
                reverbOut_.assign(static_cast<std::size_t>(frames) * reverbChannels_, 0.0f);
            } catch (...) {
                return;
            }
            reverbFx_.Process(reverb_.data(), reverbOut_.data(), frames);
            for (std::uint32_t f = 0; f < frames; ++f) {
                const float* e = &reverbOut_[static_cast<std::size_t>(f) * reverbChannels_];
                float* o = &mix_[static_cast<std::size_t>(f) * channels_];
                for (std::uint32_t k = 0; k < reverbChannels_; ++k) {
                    const float* row = &reverbMatrix_[static_cast<std::size_t>(k) * channels_];
                    for (std::uint32_t d = 0; d < channels_; ++d) {
                        o[d] += e[k] * row[d];
                    }
                }
            }
            reverbTap = reverbOut_.data();
            reverbTapChannels = reverbChannels_;
        }
        // Taps: categories, reverb (effect output like XAudio2's reverb submix tap; the dry mono send without reverb),
        // mix.
        for (std::uint32_t c = 0; c < kCategoryCount; ++c) {
            tap_.Call(TapOfCategory(static_cast<Category>(c)), buses_[c].data(), frames, channels_, rate_);
        }
        tap_.Call(TapPoint::Reverb, reverbTap, frames, reverbTapChannels, rate_);
        stage_.FilterLfe(mix_.data(), frames, channels_);
        tap_.Call(TapPoint::Master, mix_.data(), frames, channels_, rate_);
        // Limiter and master volume after the taps (captures are the plain mix), also without output (statistics).
        stage_.Process(mix_.data(), frames, channels_);
        if (out != nullptr) {
            std::memcpy(out, mix_.data(), n * sizeof(float));
        }
        rendered_.fetch_add(frames);
    }

    friend class SoftVoice;
    const std::uint32_t channels_;
    const std::uint32_t rate_;
    bool realtime_;
    std::mutex mutex_;
    std::vector<SoftVoice*> voices_;
    std::vector<float> buses_[kCategoryCount];
    std::vector<float> reverb_;
    std::vector<float> mix_;
    reverb::SoftwareReverb reverbFx_;
    bool reverbOn_ = false;
    std::uint32_t reverbChannels_ = 1;
    float reverbMatrix_[kMaxOutputChannels * kMaxOutputChannels] = {};
    std::vector<float> reverbOut_;
    MasterStage stage_;
    std::atomic<std::uint64_t> rendered_{0};
    std::atomic<std::uint64_t> lightBlocks_{0};  // blocks that only advanced the positions (nobody listening)
    std::atomic<bool> stop_{false};
    TapSlot tap_;
    std::thread thread_;
};

SoftVoice::SoftVoice(SoftBackend& b, const VoiceDesc& desc, std::uint32_t outChannels) noexcept
    : backend(b),
      events(desc.events),
      category(desc.category),
      channels(desc.format.channels),
      rate(desc.format.sampleRate),
      maxRatio(desc.maxFrequencyRatio > 0 ? desc.maxFrequencyRatio : 4.0f),
      reverbSend(desc.reverbSend) {
    ring.assign(static_cast<std::size_t>(kRingFrames) * channels, 0.0f);
    matrix.assign(static_cast<std::size_t>(channels) * outChannels, 0.0f);
    // Default like XAudio2's for mono/stereo sources: mono to every front channel, channel i to output i.
    for (std::uint32_t c = 0; c < channels; ++c) {
        if (channels == 1) {
            for (std::uint32_t d = 0; d < std::min<std::uint32_t>(outChannels, 2); ++d) {
                matrix[d] = 1.0f;
            }
        } else if (c < outChannels) {
            matrix[static_cast<std::size_t>(c) * outChannels + c] = 1.0f;
        }
    }
}
SoftVoice::~SoftVoice() {
    backend.Remove(this);
}
bool SoftVoice::Submit(const SubmitDesc& buffer) noexcept {
    if (buffer.data == nullptr || buffer.frames == 0 || buffer.playBegin >= buffer.frames) {
        return false;
    }
    std::lock_guard<std::mutex> lock(backend.Mutex());
    try {
        queue.push_back({buffer, buffer.playBegin});
    } catch (...) {
        return false;
    }
    return true;
}
void SoftVoice::Start() noexcept {
    std::lock_guard<std::mutex> lock(backend.Mutex());
    started = true;
}
void SoftVoice::Stop() noexcept {
    std::lock_guard<std::mutex> lock(backend.Mutex());
    started = false;
}
void SoftVoice::Flush() noexcept {
    std::lock_guard<std::mutex> lock(backend.Mutex());
    try {
        for (const PendingEnd& e : ends) {
            flushed.push_back(e.context);
        }
        for (const Buf& b : queue) {
            flushed.push_back(b.d.context);
        }
    } catch (...) {
    }
    ends.clear();
    queue.clear();
    // Drop the filter lookahead taken from the flushed buffers: the next buffer starts at the read position (the frame
    // at an integer position has not been output yet; a fractional position starts at the next frame).
    const auto next = static_cast<std::uint64_t>(std::ceil(pos));
    if (pulled > next) {
        pulled = next;
    }
}
std::uint64_t SoftVoice::FramesPlayed() noexcept {
    std::lock_guard<std::mutex> lock(backend.Mutex());
    return static_cast<std::uint64_t>(pos);
}
std::uint32_t SoftVoice::BuffersQueued() noexcept {
    std::lock_guard<std::mutex> lock(backend.Mutex());
    return static_cast<std::uint32_t>(queue.size() + ends.size() + flushed.size());
}
void SoftVoice::SetFrequencyRatio(float r) noexcept {
    std::lock_guard<std::mutex> lock(backend.Mutex());
    ratio = std::clamp(r, 1.0f / 1024.0f, maxRatio);
}
void SoftVoice::SetOutputMatrix(std::span<const float> levels) noexcept {
    std::lock_guard<std::mutex> lock(backend.Mutex());
    const std::size_t n = std::min(levels.size(), matrix.size());
    std::copy_n(levels.begin(), n, matrix.begin());
}
void SoftVoice::SetReverbSend(float gain) noexcept {
    std::lock_guard<std::mutex> lock(backend.Mutex());
    send = gain;
}

// =================================================================================================================
// XAudio2 backend
// =================================================================================================================

// Pass-through XAPO at the end of a voice's effect chain: feeds the MixTap with every processed block (`point`) and,
// on the mastering voice, counts the frames (OutputFramesRendered). In place, float32, never allocates in Process
// (the TapSlot's shared lock only waits for a concurrent SetTap).
class TapXapo final : public IXAPO {
public:
    TapXapo(TapPoint point, TapSlot& tap, std::atomic<std::uint64_t>* frames, MasterStage* stage) noexcept
        : point_(point), frames_(frames), tap_(tap), stage_(stage) {}
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
    STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(++refs_); }
    STDMETHOD_(ULONG, Release)() override {
        const long r = --refs_;
        if (r == 0) {
            delete this;
        }
        return static_cast<ULONG>(r);
    }
    STDMETHOD(GetRegistrationProperties)(XAPO_REGISTRATION_PROPERTIES** pp) override {
        if (pp == nullptr) {
            return E_POINTER;
        }
        auto* p = static_cast<XAPO_REGISTRATION_PROPERTIES*>(XAPOAlloc(sizeof(XAPO_REGISTRATION_PROPERTIES)));
        if (p == nullptr) {
            return E_OUTOFMEMORY;
        }
        std::memset(p, 0, sizeof(*p));
        // {6E8D5F1A-5C3B-4F0E-9A31-46D50B6E0C11}: private id of this pass-through effect.
        p->clsid = {0x6e8d5f1a, 0x5c3b, 0x4f0e, {0x9a, 0x31, 0x46, 0xd5, 0x0b, 0x6e, 0x0c, 0x11}};
        wcscpy_s(p->FriendlyName, L"FuturamaPC mix tap");
        wcscpy_s(p->CopyrightInfo, L"FuturamaPC");
        p->MajorVersion = 1;
        p->Flags = XAPO_FLAG_CHANNELS_MUST_MATCH | XAPO_FLAG_FRAMERATE_MUST_MATCH |
                   XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH | XAPO_FLAG_BUFFERCOUNT_MUST_MATCH |
                   XAPO_FLAG_INPLACE_SUPPORTED | XAPO_FLAG_INPLACE_REQUIRED;
        p->MinInputBufferCount = p->MaxInputBufferCount = 1;
        p->MinOutputBufferCount = p->MaxOutputBufferCount = 1;
        *pp = p;
        return S_OK;
    }
    STDMETHOD(IsInputFormatSupported)(const WAVEFORMATEX* out, const WAVEFORMATEX* req,
                                      WAVEFORMATEX** supported) override {
        return CheckFormat(out, req, supported);
    }
    STDMETHOD(IsOutputFormatSupported)(const WAVEFORMATEX* in, const WAVEFORMATEX* req,
                                       WAVEFORMATEX** supported) override {
        return CheckFormat(in, req, supported);
    }
    STDMETHOD(Initialize)(const void*, UINT32) override { return S_OK; }
    STDMETHOD_(void, Reset)() override {}
    STDMETHOD(LockForProcess)(UINT32 inCount, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* in, UINT32,
                              const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS*) override {
        if (inCount != 1 || in == nullptr || in[0].pFormat == nullptr) {
            return E_INVALIDARG;
        }
        channels_ = in[0].pFormat->nChannels;
        rate_ = in[0].pFormat->nSamplesPerSec;
        maxFrames_ = in[0].MaxFrameCount;
        try {
            zeros_.assign(static_cast<std::size_t>(maxFrames_) * channels_, 0.0f);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }
    STDMETHOD_(void, UnlockForProcess)() override {}
    STDMETHOD_(void, Process)(UINT32 inCount, const XAPO_PROCESS_BUFFER_PARAMETERS* in, UINT32 outCount,
                              XAPO_PROCESS_BUFFER_PARAMETERS* out, BOOL) override {
        if (inCount != 1 || in == nullptr) {
            return;
        }
        const UINT32 frames = in[0].ValidFrameCount;
        if (outCount == 1 && out != nullptr) {
            out[0].BufferFlags = in[0].BufferFlags;
            out[0].ValidFrameCount = frames;
        }
        if (stage_ != nullptr && in[0].BufferFlags != XAPO_BUFFER_SILENT && in[0].pBuffer != nullptr) {
            stage_->FilterLfe(static_cast<float*>(in[0].pBuffer), frames, channels_);  // in place, before the tap
        }
        const float* samples = in[0].BufferFlags == XAPO_BUFFER_SILENT || in[0].pBuffer == nullptr
                                   ? (frames <= maxFrames_ ? zeros_.data() : nullptr)
                                   : static_cast<const float*>(in[0].pBuffer);
        if (samples != nullptr) {
            tap_.Call(point_, samples, frames, channels_, rate_);
        }
        if (frames_ != nullptr) {
            frames_->fetch_add(frames, std::memory_order_relaxed);
        }
        if (stage_ != nullptr && in[0].BufferFlags != XAPO_BUFFER_SILENT && in[0].pBuffer != nullptr) {
            stage_->Process(static_cast<float*>(in[0].pBuffer), frames, channels_);  // in place (required)
        }
    }
    STDMETHOD_(UINT32, CalcInputFrames)(UINT32 frames) override { return frames; }
    STDMETHOD_(UINT32, CalcOutputFrames)(UINT32 frames) override { return frames; }

private:
    static HRESULT CheckFormat(const WAVEFORMATEX* other, const WAVEFORMATEX* req, WAVEFORMATEX** supported) {
        if (supported != nullptr) {
            *supported = nullptr;
        }
        if (req == nullptr) {
            return E_POINTER;
        }
        const bool isFloat =
            req->wBitsPerSample == 32 &&
            (req->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
             (req->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
              reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(req)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
        const bool same = other == nullptr ||
                          (other->nChannels == req->nChannels && other->nSamplesPerSec == req->nSamplesPerSec);
        if (isFloat && same) {
            return S_OK;
        }
        if (supported != nullptr && other != nullptr) {
            auto* f = static_cast<WAVEFORMATEX*>(XAPOAlloc(sizeof(WAVEFORMATEX)));
            if (f != nullptr) {
                *f = {};
                f->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                f->nChannels = other->nChannels;
                f->nSamplesPerSec = other->nSamplesPerSec;
                f->wBitsPerSample = 32;
                f->nBlockAlign = static_cast<WORD>(4 * other->nChannels);
                f->nAvgBytesPerSec = f->nBlockAlign * f->nSamplesPerSec;
                *supported = f;
            }
        }
        return XAPO_E_FORMAT_UNSUPPORTED;
    }

    std::atomic<long> refs_{1};
    TapPoint point_;
    std::atomic<std::uint64_t>* frames_;
    TapSlot& tap_;
    MasterStage* stage_;
    std::uint32_t channels_ = 2;
    std::uint32_t rate_ = 48000;
    std::uint32_t maxFrames_ = 0;
    std::vector<float> zeros_;
};

class XaBackend;

class XaVoice final : public SourceVoice, public IXAudio2VoiceCallback {
public:
    XaVoice(XaBackend& backend, const VoiceDesc& desc) noexcept
        : backend_(backend),
          events_(desc.events),
          channels_(desc.format.channels),
          reverbSend_(desc.reverbSend),
          maxRatio_(desc.maxFrequencyRatio > 0 ? desc.maxFrequencyRatio : 4.0f) {}
    ~XaVoice() override {
        if (voice_ != nullptr) {
            voice_->DestroyVoice();  // synchronous: no callback after this returns
        }
    }
    bool Init(IXAudio2* xa, IXAudio2Voice* categoryVoice, IXAudio2Voice* reverbVoice, std::uint32_t rate) noexcept {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = static_cast<WORD>(channels_);
        wfx.nSamplesPerSec = rate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = static_cast<WORD>(2 * channels_);
        wfx.nAvgBytesPerSec = wfx.nBlockAlign * rate;
        XAUDIO2_SEND_DESCRIPTOR sends[2] = {{0, categoryVoice}, {0, reverbVoice}};
        const UINT32 sendCount = reverbSend_ && reverbVoice != nullptr ? 2u : 1u;
        reverbVoice_ = sendCount == 2 ? reverbVoice : nullptr;
        categoryVoice_ = categoryVoice;
        XAUDIO2_VOICE_SENDS list{sendCount, sends};
        const float maxRatio = std::clamp(maxRatio_, 1.0f, XAUDIO2_MAX_FREQ_RATIO);
        const HRESULT hr = xa->CreateSourceVoice(&voice_, &wfx, 0, maxRatio, this, &list, nullptr);
        if (FAILED(hr)) {
            voice_ = nullptr;
            FUT_LOGW(Snd, "audio: CreateSourceVoice(%u ch, %u Hz) failed 0x%08lX", channels_, rate,
                     static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }
    bool Submit(const SubmitDesc& b) noexcept override {
        if (b.data == nullptr || b.frames == 0 || b.playBegin >= b.frames) {
            return false;
        }
        XAUDIO2_BUFFER x{};
        x.AudioBytes = b.frames * 2u * channels_;
        x.pAudioData = reinterpret_cast<const BYTE*>(b.data);
        x.PlayBegin = b.playBegin;
        x.PlayLength = 0;
        x.LoopCount = b.loop ? XAUDIO2_LOOP_INFINITE : 0;
        x.pContext = reinterpret_cast<void*>(static_cast<std::uintptr_t>(b.context));
        return SUCCEEDED(voice_->SubmitSourceBuffer(&x));
    }
    void Start() noexcept override;
    void Stop() noexcept override;
    void Flush() noexcept override { voice_->FlushSourceBuffers(); }
    void Discontinuity() noexcept override { voice_->Discontinuity(); }
    // Monotonic (SourceVoice contract): XAudio2 restarts SamplesPlayed at 0 after a buffer marked END_OF_STREAM
    // (Discontinuity, XMV). OnStreamEnd adds the value before the restart to streamBase_ (see there: up to one pass
    // short); the read never steps back (a read in the gap returns the previous value). Guest thread only.
    std::uint64_t FramesPlayed() noexcept override {
        XAUDIO2_VOICE_STATE st{};
        voice_->GetState(&st, 0);
        const std::uint64_t value = streamBase_.load(std::memory_order_acquire) + st.SamplesPlayed;
        lastReturned_ = std::max(lastReturned_, value);
        return lastReturned_;
    }
    std::uint32_t BuffersQueued() noexcept override {
        XAUDIO2_VOICE_STATE st{};
        voice_->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return st.BuffersQueued;
    }
    void SetFrequencyRatio(float ratio) noexcept override;
    void SetOutputMatrix(std::span<const float> levels) noexcept override;
    void SetReverbSend(float gain) noexcept override;

    // IXAudio2VoiceCallback (XAudio2 engine thread).
    STDMETHOD_(void, OnVoiceProcessingPassStart)(UINT32) override {}
    // Measured (XAudio2 2.9, Windows 11): inside the pass's callbacks GetState still reports SamplesPlayed of the pass
    // start; the END_OF_STREAM restart to 0 is visible from OnVoiceProcessingPassEnd on. The base grows there (never
    // before the restart, so a read cannot count the stream twice) by the value at the pass start, up to one pass
    // (480 output frames, 10 ms) short of the true end of the stream.
    STDMETHOD_(void, OnVoiceProcessingPassEnd)() override {
        if (streamEnded_) {
            streamEnded_ = false;
            streamBase_.fetch_add(streamEndSamples_, std::memory_order_release);
        }
    }
    STDMETHOD_(void, OnStreamEnd)() override {
        XAUDIO2_VOICE_STATE st{};
        voice_->GetState(&st, 0);  // allowed in a voice callback
        streamEndSamples_ = st.SamplesPlayed;
        streamEnded_ = true;
    }
    STDMETHOD_(void, OnBufferStart)(void*) override {}
    STDMETHOD_(void, OnBufferEnd)(void* context) override {
        if (events_ != nullptr) {
            events_->OnBufferEnd(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(context)));
        }
    }
    STDMETHOD_(void, OnLoopEnd)(void*) override {}
    STDMETHOD_(void, OnVoiceError)(void*, HRESULT hr) override;

private:
    XaBackend& backend_;
    VoiceEvents* events_;
    std::uint32_t channels_;
    bool reverbSend_;
    float maxRatio_;
    IXAudio2SourceVoice* voice_ = nullptr;
    IXAudio2Voice* categoryVoice_ = nullptr;
    IXAudio2Voice* reverbVoice_ = nullptr;
    bool streamEnded_ = false;              // XAudio2 thread: OnStreamEnd in the current pass
    std::uint64_t streamEndSamples_ = 0;    // XAudio2 thread: SamplesPlayed at that OnStreamEnd
    std::atomic<std::uint64_t> streamBase_{0};  // sum of SamplesPlayed at every END_OF_STREAM restart
    std::uint64_t lastReturned_ = 0;            // FramesPlayed never steps back (guest thread)
};

class XaBackend final : public Backend, public IXAudio2EngineCallback {
public:
    static std::unique_ptr<XaBackend> Create(const EngineConfig& config, std::string& error) noexcept {
        std::unique_ptr<XaBackend> b(new (std::nothrow) XaBackend());
        if (b == nullptr) {
            error = "out of memory";
            return nullptr;
        }
        if (!b->Init(config, error)) {
            return nullptr;
        }
        return b;
    }
    ~XaBackend() override { Shutdown(); }

    const char* Name() const noexcept override { return "xaudio2"; }
    bool HasDevice() const noexcept override { return true; }
    std::uint32_t Channels() const noexcept override { return channels_; }
    std::uint32_t ChannelMask() const noexcept override { return mask_; }
    std::uint32_t SampleRate() const noexcept override { return rate_; }
    bool TapFormat(TapPoint point, std::uint32_t& channels, std::uint32_t& sampleRate) const noexcept override {
        if (point >= TapPoint::Count) {
            return false;
        }
        if (point == TapPoint::Reverb) {
            if (reverb_ == nullptr) {
                return false;  // no reverb submix, no tap
            }
            channels = reverbEffectChannels_;
            sampleRate = reverb::ReverbSampleRate(rate_);
            return true;
        }
        channels = channels_;
        sampleRate = rate_;
        return true;
    }
    std::unique_ptr<SourceVoice> CreateVoice(const VoiceDesc& desc) noexcept override {
        if (desc.format.channels == 0 || desc.format.channels > kMaxOutputChannels ||
            desc.format.sampleRate < XAUDIO2_MIN_SAMPLE_RATE || desc.format.sampleRate > XAUDIO2_MAX_SAMPLE_RATE) {
            return nullptr;
        }
        std::unique_ptr<XaVoice> v(new (std::nothrow) XaVoice(*this, desc));
        if (v == nullptr) {
            return nullptr;
        }
        const std::size_t cat = std::min<std::size_t>(static_cast<std::size_t>(desc.category), kCategoryCount - 1);
        if (!v->Init(xa_, categories_[cat], reverb_, desc.format.sampleRate)) {
            return nullptr;
        }
        return v;
    }
    // I3DL2 -> native parameters with the reverb agent's conversion (reverb_i3dl2.h ToNative: clamped, WetDryMix 100,
    // double-precision log10 - the SDK inline is not stable across translation units).
    void SetReverb(const I3dl2Listener& l) noexcept override {
        if (reverb_ == nullptr) {
            return;
        }
        const XAUDIO2FX_REVERB_PARAMETERS native = reverb::ToNative(l, false);
        const HRESULT hr = reverb_->SetEffectParameters(0, &native, sizeof(native), XAUDIO2_COMMIT_NOW);
        if (FAILED(hr)) {
            FUT_LOGW(Snd, "audio: reverb SetEffectParameters failed 0x%08lX", static_cast<unsigned long>(hr));
        }
    }
    // In the master stage after the Master tap (a quiet test run still captures at full level).
    void SetMasterVolume(float gain) noexcept override { stage_.SetGain(gain); }
    void BeginBatch() noexcept override {
        if (opSet_.load() == XAUDIO2_COMMIT_NOW) {
            UINT32 op = nextOpSet_++;
            if (op == XAUDIO2_COMMIT_NOW) {
                op = nextOpSet_++;
            }
            opSet_.store(op);
        }
    }
    void CommitBatch() noexcept override {
        const UINT32 op = opSet_.exchange(XAUDIO2_COMMIT_NOW);
        if (op != XAUDIO2_COMMIT_NOW) {
            xa_->CommitChanges(op);
        }
    }
    UINT32 OperationSet() const noexcept { return opSet_.load(); }
    const std::wstring& EndpointId() const noexcept { return endpointId_; }
    bool UsedConfiguredDevice() const noexcept { return usedConfiguredDevice_; }
    void SetTap(MixTap* tap) noexcept override { tap_.Set(tap); }
    std::uint32_t RenderSoftware(std::uint32_t, float*) noexcept override { return 0; }
    bool Lost() const noexcept override { return lost_.load(); }
    void SimulateLoss() noexcept override { OnCriticalError(static_cast<HRESULT>(0x88890004)); }  // DEVICE_INVALIDATED
    HRESULT LostReason() const noexcept { return lostReason_.load(); }
    std::uint32_t Glitches() const noexcept override {
        if (xa_ == nullptr) {
            return 0;
        }
        XAUDIO2_PERFORMANCE_DATA perf{};
        xa_->GetPerformanceData(&perf);
        return perf.GlitchesSinceEngineStarted;
    }
    std::uint64_t FramesRendered() const noexcept override { return frames_.load(); }
    float LimiterMaxReductionDb() const noexcept override { return stage_.MaxReductionDb(); }
    std::uint64_t LimitedFrames() const noexcept override { return stage_.LimitedFrames(); }
    std::string Description() const override {
        char text[256];
        std::snprintf(text, sizeof(text), "XAudio2 2.9, %s, %u ch (mask 0x%X) %u Hz, reverb %s, limiter %s",
                      deviceText_.c_str(), channels_, mask_, rate_, reverb_ != nullptr ? "on" : "off",
                      stage_.LimiterOn() ? "on" : "off");
        return text;
    }
    void NoteVoiceError(HRESULT hr) noexcept {
        if (voiceErrors_.fetch_add(1) == 0) {
            lostReason_.store(hr);
        }
    }

    // IXAudio2EngineCallback (engine thread).
    STDMETHOD_(void, OnProcessingPassStart)() override {}
    STDMETHOD_(void, OnProcessingPassEnd)() override {}
    STDMETHOD_(void, OnCriticalError)(HRESULT error) override {
        lostReason_.store(error);
        lost_.store(true);
    }

private:
    XaBackend() = default;

    bool Init(const EngineConfig& config, std::string& error) noexcept {
        HRESULT hr = XAudio2Create(&xa_, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) {
            error = "XAudio2Create failed";
            lostReason_.store(hr);
            return false;
        }
        xa_->RegisterForCallbacks(this);
        // [audio] device: the IMMDevice id the launcher stores (IMMDevice::GetId, "{0.0.0.00000000}.{guid}");
        // XAudio2 2.9 takes that string as szDeviceId. A device that is not connected falls back to the default.
        const wchar_t* device = config.deviceId.empty() ? nullptr : config.deviceId.c_str();
        hr = xa_->CreateMasteringVoice(&master_, config.speakers, 48000, 0, device, nullptr, AudioCategory_GameEffects);
        if (FAILED(hr) && device != nullptr) {
            // Once per run (the engine tries again whenever the devices change; see EngineFacade::Pump).
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                FUT_LOGW(Snd, "audio: output device from [audio] device not available (0x%08lX) - using the default",
                         static_cast<unsigned long>(hr));
            }
            hr = xa_->CreateMasteringVoice(&master_, config.speakers, 48000, 0, nullptr, nullptr,
                                           AudioCategory_GameEffects);
            device = nullptr;
        }
        if (FAILED(hr)) {
            char text[96];
            std::snprintf(text, sizeof(text), "no output device (CreateMasteringVoice 0x%08lX)",
                          static_cast<unsigned long>(hr));
            error = text;
            return false;
        }
        usedConfiguredDevice_ = device != nullptr;
        const EndpointInfo endpoint = QueryEndpoint(usedConfiguredDevice_ ? config.deviceId : std::wstring());
        endpointId_ = endpoint.id;
        deviceText_ = std::string(usedConfiguredDevice_ ? "device from [audio] device" : "default device") + " '" +
                      (endpoint.name.empty() ? std::string("?") : endpoint.name) + "'";
        XAUDIO2_VOICE_DETAILS details{};
        master_->GetVoiceDetails(&details);
        if (details.InputChannels > kMaxOutputChannels) {
            // More than 8 endpoint channels (7.1.4, spatial endpoints): the graph, the taps and the reverb matrix
            // work with at most 8, so the mastering voice gets 8 input channels; XAudio2/WASAPI map them to the
            // endpoint.
            const std::uint32_t endpointChannels = details.InputChannels;
            master_->DestroyVoice();
            master_ = nullptr;
            hr = xa_->CreateMasteringVoice(&master_, kMaxOutputChannels, 48000, 0, device, nullptr,
                                           AudioCategory_GameEffects);
            if (FAILED(hr)) {
                error = "CreateMasteringVoice with 8 channels failed";
                return false;
            }
            master_->GetVoiceDetails(&details);
            FUT_LOGI(Snd, "audio: the endpoint has %u channels - mixing 8 (7.1), the device maps them",
                     endpointChannels);
        }
        channels_ = std::min<std::uint32_t>(details.InputChannels, kMaxOutputChannels);
        rate_ = details.InputSampleRate;
        DWORD mask = 0;
        if (FAILED(master_->GetChannelMask(&mask)) || mask == 0 ||
            static_cast<std::uint32_t>(std::popcount(static_cast<std::uint32_t>(mask))) != channels_) {
            mask = DefaultChannelMask(channels_);
        }
        mask_ = mask;

        // Mastering voice chain: one XAPO = tap Master + frame counter + master stage (limiter, master volume).
        stage_.Configure(config.limiter, rate_, mask_);
        {
            std::vector<XAUDIO2_EFFECT_DESCRIPTOR> chain;
            auto* counter = new (std::nothrow) TapXapo(TapPoint::Master, tap_, &frames_, &stage_);
            if (counter != nullptr) {
                chain.push_back({static_cast<IXAPO*>(counter), TRUE, channels_});
            }
            XAUDIO2_EFFECT_CHAIN c{static_cast<UINT32>(chain.size()), chain.data()};
            if (!chain.empty()) {
                hr = master_->SetEffectChain(&c);
                if (FAILED(hr)) {
                    FUT_LOGW(Snd, "audio: mastering effect chain failed 0x%08lX", static_cast<unsigned long>(hr));
                }
            }
            for (XAUDIO2_EFFECT_DESCRIPTOR& d : chain) {
                d.pEffect->Release();
            }
        }
        // Category submixes (stage 0 -> mastering voice), each with a tap.
        for (std::uint32_t c = 0; c < kCategoryCount; ++c) {
            IUnknown* tap = new (std::nothrow) TapXapo(TapOfCategory(static_cast<Category>(c)), tap_, nullptr, nullptr);
            XAUDIO2_EFFECT_DESCRIPTOR d{tap, TRUE, channels_};
            XAUDIO2_EFFECT_CHAIN chain{1, &d};
            hr = xa_->CreateSubmixVoice(&categories_[c], channels_, rate_, 0, 0, nullptr,
                                        tap != nullptr ? &chain : nullptr);
            if (tap != nullptr) {
                tap->Release();
            }
            if (FAILED(hr)) {
                error = "CreateSubmixVoice failed";
                return false;
            }
        }
        // Reverb submix (reverb_i3dl2.h layout, the same as the software mixer's SoftwareReverb): 1 input channel (the
        // mono I3DL2 send), XAudio2 reverb with 1 or 6 output channels, then the Reverb tap, then the effect channels
        // mixed to the output layout (ReverbOutputMatrix).
        if (config.reverb) {
            reverbEffectChannels_ = reverb::ReverbEffectChannels(channels_);
            IUnknown* fx = nullptr;
            hr = reverb::CreateReverbEffect(&fx);
            if (SUCCEEDED(hr)) {
                XAUDIO2_EFFECT_DESCRIPTOR d[2] = {{fx, TRUE, reverbEffectChannels_},
                                                  {nullptr, TRUE, reverbEffectChannels_}};
                IUnknown* tap = new (std::nothrow) TapXapo(TapPoint::Reverb, tap_, nullptr, nullptr);
                d[1].pEffect = tap;
                XAUDIO2_EFFECT_CHAIN chain{tap != nullptr ? 2u : 1u, d};
                hr = xa_->CreateSubmixVoice(&reverb_, reverb::kReverbInputChannels, reverb::ReverbSampleRate(rate_), 0,
                                            0, nullptr, &chain);
                fx->Release();
                if (tap != nullptr) {
                    tap->Release();
                }
            }
            if (SUCCEEDED(hr)) {
                float api[kMaxOutputChannels * kMaxOutputChannels] = {};
                float xm[kMaxOutputChannels * kMaxOutputChannels] = {};
                if (reverb::ReverbOutputMatrix(reverbEffectChannels_, channels_, api, std::size(api))) {
                    for (std::uint32_t e = 0; e < reverbEffectChannels_; ++e) {
                        for (std::uint32_t o = 0; o < channels_; ++o) {
                            xm[o * reverbEffectChannels_ + e] = api[e * channels_ + o];  // XAudio2: [dst][src]
                        }
                    }
                    hr = reverb_->SetOutputMatrix(master_, reverbEffectChannels_, channels_, xm, XAUDIO2_COMMIT_NOW);
                }
            }
            if (FAILED(hr)) {
                FUT_LOGW(Snd, "audio: reverb submix not available (0x%08lX) - 3D voices without reverb",
                         static_cast<unsigned long>(hr));
                if (reverb_ != nullptr) {
                    reverb_->DestroyVoice();
                }
                reverb_ = nullptr;
            }
        }
        hr = xa_->StartEngine();
        if (FAILED(hr)) {
            error = "StartEngine failed";
            return false;
        }
        return true;
    }

    void Shutdown() noexcept {
        if (xa_ == nullptr) {
            return;
        }
        xa_->StopEngine();
        if (reverb_ != nullptr) {
            reverb_->DestroyVoice();
            reverb_ = nullptr;
        }
        for (IXAudio2SubmixVoice*& v : categories_) {
            if (v != nullptr) {
                v->DestroyVoice();
                v = nullptr;
            }
        }
        if (master_ != nullptr) {
            master_->DestroyVoice();
            master_ = nullptr;
        }
        xa_->UnregisterForCallbacks(this);
        xa_->Release();
        xa_ = nullptr;
    }

    friend class XaVoice;
    IXAudio2* xa_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SubmixVoice* categories_[kCategoryCount] = {};
    IXAudio2SubmixVoice* reverb_ = nullptr;
    std::uint32_t channels_ = 2;
    std::uint32_t mask_ = 0x3;  // SPEAKER_STEREO
    std::uint32_t rate_ = 48000;
    std::uint32_t reverbEffectChannels_ = 1;
    std::string deviceText_;
    std::wstring endpointId_;            // the endpoint the mastering voice was opened on ("" if unknown)
    bool usedConfiguredDevice_ = false;  // [audio] device opened (false: the default device)
    std::atomic<bool> lost_{false};
    std::atomic<HRESULT> lostReason_{S_OK};
    std::atomic<std::uint32_t> voiceErrors_{0};
    std::atomic<UINT32> opSet_{XAUDIO2_COMMIT_NOW};
    UINT32 nextOpSet_ = 1;
    std::atomic<std::uint64_t> frames_{0};
    TapSlot tap_;
    MasterStage stage_;
};

void XaVoice::Start() noexcept {
    voice_->Start(0, backend_.OperationSet());
}
void XaVoice::Stop() noexcept {
    voice_->Stop(0, backend_.OperationSet());
}
void XaVoice::SetFrequencyRatio(float ratio) noexcept {
    voice_->SetFrequencyRatio(std::clamp(ratio, XAUDIO2_MIN_FREQ_RATIO, std::clamp(maxRatio_, 1.0f, 1024.0f)),
                              backend_.OperationSet());
}
void XaVoice::SetOutputMatrix(std::span<const float> levels) noexcept {
    const std::uint32_t out = backend_.channels_;
    if (levels.size() < static_cast<std::size_t>(channels_) * out || channels_ > kMaxOutputChannels ||
        out > kMaxOutputChannels) {
        return;
    }
    // The API matrix is [src][dst]; XAudio2 expects pLevelMatrix[SourceChannels * D + S] ([dst][src]).
    float m[kMaxOutputChannels * kMaxOutputChannels] = {};
    for (std::uint32_t s = 0; s < channels_; ++s) {
        for (std::uint32_t d = 0; d < out; ++d) {
            m[d * channels_ + s] = levels[static_cast<std::size_t>(s) * out + d];
        }
    }
    voice_->SetOutputMatrix(categoryVoice_, channels_, out, m, backend_.OperationSet());
}
void XaVoice::SetReverbSend(float gain) noexcept {
    if (reverbVoice_ == nullptr) {
        return;
    }
    // The reverb submix has one input (reverb::kReverbInputChannels): every source channel sends `gain` into it.
    float m[kMaxOutputChannels] = {};
    for (std::uint32_t c = 0; c < channels_ && c < kMaxOutputChannels; ++c) {
        m[c] = gain;
    }
    voice_->SetOutputMatrix(reverbVoice_, channels_, reverb::kReverbInputChannels, m, backend_.OperationSet());
}
void XaVoice::OnVoiceError(void*, HRESULT hr) {
    backend_.NoteVoiceError(hr);
}

// =================================================================================================================
// Facade: proxies + backend switching
// =================================================================================================================
class EngineFacade;

class ProxyVoice final : public SourceVoice, public VoiceEvents {
public:
    ProxyVoice(EngineFacade& owner, const VoiceDesc& desc) noexcept;
    ~ProxyVoice() override;

    bool Submit(const SubmitDesc& b) noexcept override {
        if (rebase_ && impl_ != nullptr) {
            // First buffer after a Flush: it starts where the voice stands now. Read here and not right after the
            // flush, which XAudio2 applies asynchronously (up to one processing pass could still be counted).
            rebase_ = false;
            const std::uint64_t played = impl_->FramesPlayed();
            std::lock_guard<std::mutex> lock(mutex_);
            base_ = played;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            try {
                pending_.push_back(b);
            } catch (...) {
                return false;
            }
        }
        if (impl_ == nullptr || !impl_->Submit(b)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_.empty()) {
                pending_.pop_back();
            }
            return false;
        }
        return true;
    }
    void Start() noexcept override {
        started_ = true;
        if (impl_ != nullptr) {
            impl_->Start();
        }
    }
    void Stop() noexcept override {
        started_ = false;
        if (impl_ != nullptr) {
            impl_->Stop();
        }
    }
    void Flush() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.clear();
            ended_ = 0;
        }
        if (impl_ != nullptr) {
            impl_->Flush();
            rebase_ = true;  // base_ is taken at the next Submit (backend calls never under mutex_)
        }
    }
    void Discontinuity() noexcept override {
        if (impl_ != nullptr) {
            impl_->Discontinuity();
        }
    }
    std::uint64_t FramesPlayed() noexcept override {
        return framesBase_ + (impl_ != nullptr ? impl_->FramesPlayed() : 0);
    }
    std::uint32_t BuffersQueued() noexcept override { return impl_ != nullptr ? impl_->BuffersQueued() : 0; }
    void SetFrequencyRatio(float ratio) noexcept override {
        ratio_ = ratio;
        if (impl_ != nullptr) {
            impl_->SetFrequencyRatio(ratio);
        }
    }
    void SetOutputMatrix(std::span<const float> levels) noexcept override {
        try {
            matrix_.assign(levels.begin(), levels.end());
        } catch (...) {
        }
        if (impl_ != nullptr) {
            impl_->SetOutputMatrix(levels);
        }
    }
    void SetReverbSend(float gain) noexcept override {
        send_ = gain;
        if (impl_ != nullptr) {
            impl_->SetReverbSend(gain);
        }
    }

    // VoiceEvents of the backend voice (mixer thread).
    void OnBufferEnd(std::uint32_t context) noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                if (it->context == context) {
                    if (!it->loop) {
                        ended_ += it->frames - it->playBegin;
                    }
                    pending_.erase(it);
                    break;
                }
            }
        }
        if (desc_.events != nullptr) {
            desc_.events->OnBufferEnd(context);
        }
    }

    // Backend switch (guest thread, facade lock held).
    // The front queued buffer continues where it was: frames played by the old backend voice since `base_` minus
    // the frames of the buffers that ended meanwhile are the position inside it (buffers play back to back; a
    // starved voice does not count frames). A one-shot that already reached its end restarts at its last frame, so
    // its OnBufferEnd still arrives from the new backend.
    std::unique_ptr<SourceVoice> Detach() noexcept {
        const std::uint64_t played = impl_ != nullptr ? impl_->FramesPlayed() : 0;
        framesBase_ += played;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.empty()) {
            SubmitDesc& front = pending_.front();
            const std::uint64_t into = played > base_ + ended_ ? played - base_ - ended_ : 0;
            if (front.loop) {
                front.playBegin = static_cast<std::uint32_t>((front.playBegin + into) % front.frames);
            } else {
                front.playBegin =
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(front.playBegin + into, front.frames - 1u));
            }
        }
        base_ = 0;
        ended_ = 0;
        rebase_ = false;
        return std::move(impl_);
    }
    void Attach(Backend& backend) noexcept {
        VoiceDesc d = desc_;
        d.events = this;
        d.debugName = name_.c_str();
        impl_ = backend.CreateVoice(d);
        if (impl_ == nullptr) {
            return;
        }
        impl_->SetFrequencyRatio(ratio_);
        if (!matrix_.empty()) {
            impl_->SetOutputMatrix(matrix_);
        }
        impl_->SetReverbSend(send_);
        std::deque<SubmitDesc> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending = pending_;
            base_ = 0;  // late events of the old backend voice are already in (it was destroyed synchronously)
            ended_ = 0;
        }
        for (const SubmitDesc& b : pending) {
            impl_->Submit(b);  // the front one continues at the position Detach computed
        }
        if (started_) {
            impl_->Start();
        }
    }
    bool HasImpl() const noexcept { return impl_ != nullptr; }

private:
    EngineFacade& owner_;
    VoiceDesc desc_;
    std::string name_;
    std::unique_ptr<SourceVoice> impl_;
    std::mutex mutex_;
    std::deque<SubmitDesc> pending_;
    bool started_ = false;
    float ratio_ = 1.0f;
    std::vector<float> matrix_;
    float send_ = 0.0f;
    std::uint64_t framesBase_ = 0;
    std::uint64_t base_ = 0;   // impl FramesPlayed where pending_.front()'s predecessors began (0, or a Flush)
    bool rebase_ = false;      // a Flush happened: the next Submit takes base_ (guest thread only)
    std::uint64_t ended_ = 0;  // frames of non-looping buffers that ended since base_ (guarded by mutex_)
};

// ---- device worker ------------------------------------------------------------------------------------------------
// Windows reports endpoint changes (TV on/off, headset plugged in, a new default device) through this client. It only
// counts them (any thread, never blocks); the guest thread's Pump compares the count with the last one it acted on.
class DeviceNotifier final : public IMMNotificationClient {
public:
    explicit DeviceNotifier(std::atomic<std::uint32_t>& changes) noexcept : changes_(changes) {}
    // Not reference counted: owned by the worker thread and unregistered before it goes away.
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(OnDeviceStateChanged)(LPCWSTR, DWORD) override {
        Note();
        return S_OK;
    }
    STDMETHOD(OnDeviceAdded)(LPCWSTR) override {
        Note();
        return S_OK;
    }
    STDMETHOD(OnDeviceRemoved)(LPCWSTR) override {
        Note();
        return S_OK;
    }
    STDMETHOD(OnDefaultDeviceChanged)(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            Note();
        }
        return S_OK;
    }
    STDMETHOD(OnPropertyValueChanged)(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void Note() noexcept { changes_.fetch_add(1); }
    std::atomic<std::uint32_t>& changes_;
};

// Host thread "audio device worker" (design E10: runs no guest code, touches no guest memory) for the slow device
// work that used to stall the guest thread inside DirectSoundDoWork: looking for an endpoint and creating an XAudio2
// backend (XAudio2Create, CreateMasteringVoice - slow while an HDMI or Bluetooth endpoint wakes up - submixes,
// StartEngine), and destroying replaced backends. It also registers the DeviceNotifier. The guest thread hands a
// request over (Submit) and takes the result at a later Pump (Take); it never waits for the worker.
class DeviceWorker {
public:
    enum class Outcome : std::uint8_t { None, Opened, NoEndpoint, Failed, Unchanged };
    struct Request {
        EngineConfig config;
        bool replace = false;        // a device is open: reopen only for a better endpoint (Attempt)
        std::wstring currentEndpoint;  // replace: the endpoint in use
        bool currentConfigured = false;  // replace: the endpoint in use is [audio] device
    };
    struct Result {
        Outcome outcome = Outcome::None;
        bool replace = false;
        std::unique_ptr<XaBackend> backend;
        std::string error;
    };

    DeviceWorker() noexcept {
        try {
            thread_ = std::thread([this] { Main(); });
        } catch (...) {
        }
    }
    ~DeviceWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    bool Running() const noexcept { return thread_.joinable(); }
    std::atomic<std::uint32_t>& Changes() noexcept { return changes_; }

    // Guest thread: queues one attempt. False while one is queued, running or its result has not been taken.
    bool Submit(Request request) noexcept {
        if (!Running()) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (hasRequest_ || busy_ || result_.outcome != Outcome::None) {
                return false;
            }
            request_ = std::move(request);
            hasRequest_ = true;
        }
        wake_.notify_all();
        return true;
    }
    // Guest thread: the finished attempt (outcome None if there is none).
    Result Take() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        Result r = std::move(result_);
        result_ = Result{};
        return r;
    }
    // Destroys a replaced backend on the worker thread (its voices are already gone; XAudio2 StopEngine and the
    // voice teardown, or the software mixer's join).
    void Dispose(std::unique_ptr<Backend> old) noexcept {
        if (old == nullptr) {
            return;
        }
        if (Running()) {
            std::lock_guard<std::mutex> lock(mutex_);
            try {
                trash_.push_back(std::move(old));
            } catch (...) {
            }
        }
        if (old != nullptr) {
            old.reset();  // no worker (or no memory): here
            return;
        }
        wake_.notify_all();
    }
    // Tests: waits until no attempt is queued or running (at most `milliseconds`).
    void WaitIdle(std::uint32_t milliseconds) noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return !hasRequest_ && !busy_; });
    }

private:
    void Main() noexcept {
        SetThreadDescription(GetCurrentThread(), L"audio device worker");
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DeviceNotifier notifier(changes_);
        IMMDeviceEnumerator* enumerator = nullptr;
        bool registered = false;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
            registered = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(&notifier));
        }
        if (!registered) {
            FUT_LOGI(Snd, "audio: no device change notifications - looking for devices every 3 s only");
        }
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stop_ || hasRequest_ || !trash_.empty(); });
            while (!trash_.empty()) {
                std::unique_ptr<Backend> old = std::move(trash_.front());
                trash_.pop_front();
                lock.unlock();
                old.reset();
                lock.lock();
            }
            if (stop_) {
                break;
            }
            if (hasRequest_) {
                const Request request = std::move(request_);
                hasRequest_ = false;
                busy_ = true;
                lock.unlock();
                Result r = Attempt(request);
                lock.lock();
                result_ = std::move(r);
                busy_ = false;
                idle_.notify_all();
            }
        }
        lock.unlock();
        if (enumerator != nullptr) {
            if (registered) {
                enumerator->UnregisterEndpointNotificationCallback(&notifier);
            }
            enumerator->Release();
        }
        if (SUCCEEDED(init)) {
            CoUninitialize();
        }
    }

    static Result Attempt(const Request& q) noexcept {
        Result r;
        r.replace = q.replace;
        const bool configured = !q.config.deviceId.empty();
        if (q.replace) {
            // A device is open: reopen for the [audio] device that was missing and is back, or - while the default
            // device is in use - for a new Windows default device.
            bool better = configured && !q.currentConfigured && QueryEndpoint(q.config.deviceId).present;
            if (!better && !q.currentConfigured) {
                const EndpointInfo def = QueryEndpoint(std::wstring());
                better = def.present && !def.id.empty() && !SameEndpointId(def.id, q.currentEndpoint);
            }
            if (!better) {
                r.outcome = Outcome::Unchanged;
                return r;
            }
        } else if (!QueryEndpoint(std::wstring()).present && (!configured || !QueryEndpoint(q.config.deviceId).present)) {
            r.outcome = Outcome::NoEndpoint;
            return r;
        }
        try {
            r.backend = XaBackend::Create(q.config, r.error);
        } catch (...) {
            r.backend = nullptr;
        }
        r.outcome = r.backend != nullptr ? Outcome::Opened : Outcome::Failed;
        return r;
    }

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    bool stop_ = false;
    bool hasRequest_ = false;
    bool busy_ = false;
    Request request_;
    Result result_;
    std::deque<std::unique_ptr<Backend>> trash_;
    std::atomic<std::uint32_t> changes_{0};
};

class EngineFacade final : public AudioEngine {
public:
    EngineFacade(const EngineConfig& config, std::unique_ptr<Backend> backend, std::string reason) noexcept
        : config_(config), backend_(std::move(backend)), noDeviceReason_(std::move(reason)) {
        backend_->SetMasterVolume(config_.masterVolume);
        backend_->SetReverb(reverb_);
        lastChannels_ = backend_->Channels();
        nextRetry_ = Qpc() + kRetrySeconds * Qpf();
        nextLoss_ = Qpc() + kLossPeriodSeconds * Qpf();
        // The worker only exists where a device can be opened at run time (the game's engine; device tests).
        if (config_.enabled && config_.softwareRealtime && config_.deviceTest != EngineConfig::DeviceTest::Absent) {
            worker_ = std::make_unique<DeviceWorker>();
        }
    }
    ~EngineFacade() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (ProxyVoice* p : voices_) {
            p->Detach().reset();
        }
        voices_.clear();
    }

    bool HasDevice() const noexcept override { return backend_->HasDevice(); }
    std::uint32_t OutputChannels() const noexcept override { return backend_->Channels(); }
    std::uint32_t OutputChannelMask() const noexcept override { return backend_->ChannelMask(); }
    std::uint32_t OutputSampleRate() const noexcept override { return backend_->SampleRate(); }
    bool TapFormat(TapPoint point, std::uint32_t& channels, std::uint32_t& sampleRate) const noexcept override {
        return backend_->TapFormat(point, channels, sampleRate);
    }
    std::unique_ptr<SourceVoice> CreateVoice(const VoiceDesc& desc) noexcept override {
        std::unique_ptr<ProxyVoice> p(new (std::nothrow) ProxyVoice(*this, desc));
        if (p == nullptr) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        p->Attach(*backend_);
        if (!p->HasImpl()) {
            return nullptr;
        }
        try {
            voices_.insert(p.get());
        } catch (...) {
            return nullptr;
        }
        ++created_;
        peak_ = std::max<std::uint32_t>(peak_, static_cast<std::uint32_t>(voices_.size()));
        return p;
    }
    void Unregister(ProxyVoice* p) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        voices_.erase(p);
    }
    void SetReverb(const I3dl2Listener& listener) noexcept override {
        reverb_ = listener;
        backend_->SetReverb(listener);
    }
    void SetMasterVolume(float gain) noexcept override {
        config_.masterVolume = gain;
        backend_->SetMasterVolume(gain);
    }
    void BeginBatch() noexcept override { backend_->BeginBatch(); }
    void CommitBatch() noexcept override { backend_->CommitBatch(); }
    // Guest thread, every DirectSoundDoWork. Only hand-overs happen here; the device work runs on the worker.
    void Pump() noexcept override {
        const std::int64_t now = Qpc();
        if (config_.deviceTest == EngineConfig::DeviceTest::Loss && backend_->HasDevice() && !backend_->Lost() &&
            now >= nextLoss_) {
            FUT_LOGI(Snd, "audio: [audio] device_test = loss - simulating the loss of the output device");
            backend_->SimulateLoss();
        }
        if (backend_->Lost()) {
            const auto* xa = static_cast<const XaBackend*>(backend_.get());
            // INFO like "window: display off": a missing device is a state of the machine, not an emulation error.
            FUT_LOGI(Snd, "audio: output device lost (XAudio2 critical error 0x%08lX) - continuing with the software "
                          "mixer until a device is available",
                     static_cast<unsigned long>(xa->LostReason()));
            ++deviceLosses_;
            glitchesBefore_ += backend_->Glitches();
            EngineConfig c = config_;
            c.softwareRealtime = true;
            std::unique_ptr<Backend> soft;
            try {
                soft = std::make_unique<SoftBackend>(c, lastChannels_);
            } catch (...) {
                return;  // out of memory: try again at the next DoWork
            }
            SwitchTo(std::move(soft));
            NotifyBackendChanged(*this);
            retrySeconds_ = kRetrySeconds;
            nextRetry_ = now + kRetrySeconds * Qpf();
            return;
        }
        if (worker_ == nullptr) {
            return;
        }
        // A finished attempt of the worker.
        DeviceWorker::Result r = worker_->Take();
        switch (r.outcome) {
            case DeviceWorker::Outcome::Opened:
                if (r.replace && !backend_->HasDevice()) {
                    r.replace = false;  // the device went away meanwhile: the new one is welcome all the same
                }
                if (r.replace || !backend_->HasDevice()) {
                    FUT_LOGI(Snd, "audio: %s - %s",
                             r.replace ? "output device changed" : "output device available again",
                             r.backend->Description().c_str());
                    SwitchTo(std::move(r.backend));
                    NotifyBackendChanged(*this);
                    nextLoss_ = now + kLossPeriodSeconds * Qpf();
                    retrySeconds_ = kRetrySeconds;
                    failureLogged_ = false;
                } else {
                    worker_->Dispose(std::move(r.backend));
                }
                break;
            case DeviceWorker::Outcome::Failed:
                // An endpoint is there but XAudio2 cannot open it (e.g. held in exclusive mode): try less often.
                if (!failureLogged_) {
                    failureLogged_ = true;
                    FUT_LOGW(Snd, "audio: an output device is present but could not be opened (%s) - %s",
                             r.error.c_str(),
                             r.replace ? "staying on the current device" : "retrying less and less often");
                } else {
                    FUT_LOGD(Snd, "audio: opening the output device failed again (%s)", r.error.c_str());
                }
                if (!r.replace) {
                    retrySeconds_ = std::min<std::int64_t>(retrySeconds_ * 2, kMaxRetrySeconds);
                    nextRetry_ = now + retrySeconds_ * Qpf();
                }
                break;
            case DeviceWorker::Outcome::NoEndpoint:
                retrySeconds_ = kRetrySeconds;
                break;
            case DeviceWorker::Outcome::None:
            case DeviceWorker::Outcome::Unchanged:
                break;
        }
        // Device changes reported by Windows, or the periodic look while no device is open.
        const std::uint32_t changes = worker_->Changes().load();
        const bool changed = changes != seenChanges_;
        if (!backend_->HasDevice()) {
            if (config_.enabled && config_.softwareRealtime && (changed || now >= nextRetry_)) {
                if (worker_->Submit(MakeRequest(false))) {
                    seenChanges_ = changes;
                    nextRetry_ = now + retrySeconds_ * Qpf();
                }
            }
        } else if (changed) {
            if (worker_->Submit(MakeRequest(true))) {
                seenChanges_ = changes;
            }
        }
    }
    void SetTap(MixTap* tap) noexcept override {
        tap_ = tap;
        backend_->SetTap(tap);
    }
    std::uint32_t RenderSoftware(std::uint32_t frames, float* out) noexcept override {
        return backend_->RenderSoftware(frames, out);
    }
    Stats GetStats() const noexcept override {
        Stats s;
        // try_lock: also called from the crash reporter (xdk::LogCallSummary), possibly while this thread holds it.
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return s;
        }
        s.voicesCreated = created_;
        s.voicesLive = static_cast<std::uint32_t>(voices_.size());
        s.voicesPeak = peak_;
        s.glitches = glitchesBefore_ + backend_->Glitches();
        s.limiterMaxReductionDb = std::max(limiterMaxBefore_, backend_->LimiterMaxReductionDb());
        s.limitedFrames = limitedBefore_ + backend_->LimitedFrames();
        s.deviceLosses = deviceLosses_;
        return s;
    }
    const char* BackendName() const noexcept override { return backend_->Name(); }
    std::uint64_t OutputFramesRendered() const noexcept override {
        return framesBefore_ + backend_->FramesRendered();
    }
    void SimulateDeviceLoss() noexcept { backend_->SimulateLoss(); }
    void SwitchToNewSoftware() noexcept {
        try {
            SwitchTo(std::make_unique<SoftBackend>(config_, lastChannels_));
        } catch (...) {
        }
    }
    // Tests: asks the worker at once and waits for its answer; the next Pump hands the device over.
    void RetryDeviceNow() noexcept {
        nextRetry_ = 0;
        Pump();
        if (worker_ != nullptr) {
            worker_->WaitIdle(15000);
        }
    }
    std::string Description() const {
        std::string d = backend_->Description();
        if (!backend_->HasDevice() && !noDeviceReason_.empty()) {
            d += " - " + noDeviceReason_;
        }
        return d;
    }
    // LogEngineSummary (exit; crash reporter on any thread): reads backend_ only under the facade lock, which
    // SwitchTo holds while it replaces the backend; false (summary skipped) while another thread holds it.
    bool SummaryText(std::string& description, double& seconds) const noexcept {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        try {
            description = Description();
        } catch (...) {
        }
        seconds = static_cast<double>(framesBefore_ + backend_->FramesRendered()) /
                  static_cast<double>(std::max(1u, backend_->SampleRate()));
        return true;
    }

private:
    DeviceWorker::Request MakeRequest(bool replace) const {
        DeviceWorker::Request q;
        try {
            q.config = config_;
        } catch (...) {
        }
        if (tap_ != nullptr && backend_->Channels() != 0) {
            // capture_wav runs: keep the channel count of the capture (the device maps it to its own layout).
            q.config.speakers = backend_->Channels();
        }
        q.replace = replace;
        if (replace && backend_->HasDevice()) {
            const auto* xa = static_cast<const XaBackend*>(backend_.get());
            try {
                q.currentEndpoint = xa->EndpointId();
            } catch (...) {
            }
            q.currentConfigured = xa->UsedConfiguredDevice();
        }
        return q;
    }

    void SwitchTo(std::unique_ptr<Backend> next) noexcept {
        std::unique_ptr<Backend> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (ProxyVoice* p : voices_) {
                std::unique_ptr<SourceVoice> v = p->Detach();
                v.reset();  // synchronous destruction before the old backend goes away
            }
            backend_->SetTap(nullptr);  // waits for a running tap call; the old backend feeds nothing any more
            framesBefore_ += backend_->FramesRendered();
            limitedBefore_ += backend_->LimitedFrames();  // statistics of every backend of the run
            limiterMaxBefore_ = std::max(limiterMaxBefore_, backend_->LimiterMaxReductionDb());
            old = std::move(backend_);
            backend_ = std::move(next);
            lastChannels_ = backend_->Channels();
            backend_->SetMasterVolume(config_.masterVolume);
            backend_->SetReverb(reverb_);
            backend_->SetTap(tap_);
            for (ProxyVoice* p : voices_) {
                p->Attach(*backend_);
            }
        }
        if (worker_ != nullptr) {
            worker_->Dispose(std::move(old));  // XAudio2 teardown off the guest thread
        }
    }

    static constexpr std::int64_t kRetrySeconds = 3;      // look for a device while none is open
    static constexpr std::int64_t kMaxRetrySeconds = 30;  // backoff after failed opens of a present device
    static constexpr std::int64_t kLossPeriodSeconds = 20;  // [audio] device_test = loss
    EngineConfig config_;
    std::unique_ptr<Backend> backend_;
    std::string noDeviceReason_;
    I3dl2Listener reverb_;
    MixTap* tap_ = nullptr;
    mutable std::mutex mutex_;
    std::set<ProxyVoice*> voices_;
    std::uint32_t created_ = 0;
    std::uint32_t peak_ = 0;
    std::uint32_t deviceLosses_ = 0;
    std::uint32_t glitchesBefore_ = 0;
    std::uint64_t framesBefore_ = 0;
    std::uint64_t limitedBefore_ = 0;
    float limiterMaxBefore_ = 0.0f;
    std::uint32_t lastChannels_ = 2;
    std::int64_t nextRetry_ = 0;
    std::int64_t retrySeconds_ = kRetrySeconds;
    bool failureLogged_ = false;
    std::uint32_t seenChanges_ = 0;
    std::int64_t nextLoss_ = 0;
    // Declared last: destroyed first (joins the worker and frees the backends it still holds) while backend_ and
    // the voices are still intact.
    std::unique_ptr<DeviceWorker> worker_;
};

ProxyVoice::ProxyVoice(EngineFacade& owner, const VoiceDesc& desc) noexcept : owner_(owner), desc_(desc) {
    try {
        name_ = desc.debugName != nullptr ? desc.debugName : "";
    } catch (...) {
    }
    desc_.debugName = nullptr;
}
ProxyVoice::~ProxyVoice() {
    owner_.Unregister(this);
    impl_.reset();
}

// ---- process engine -----------------------------------------------------------------------------------------------
std::mutex g_engineMutex;
std::unique_ptr<AudioEngine> g_engine;
std::atomic<AudioEngine*> g_testEngine{nullptr};

}  // namespace

std::unique_ptr<AudioEngine> CreateSoftwareEngine(const EngineConfig& config) noexcept {
    try {
        const std::uint32_t channels = config.speakers != 0 ? config.speakers : 2;
        return std::make_unique<EngineFacade>(config, std::make_unique<SoftBackend>(config, channels),
                                              config.enabled ? std::string() : std::string("[audio] enabled = 0"));
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<AudioEngine> CreateAudioEngine(const EngineConfig& config) noexcept {
    if (config.enabled) {
        // COM for XAudio2 / MMDevice on the creating thread (2.9 does not strictly need it; harmless if set).
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string error = "no output device (simulated by [audio] device_test = absent)";
        std::unique_ptr<XaBackend> xa;
        if (config.deviceTest != EngineConfig::DeviceTest::Absent) {
            xa = XaBackend::Create(config, error);
        }
        if (xa != nullptr) {
            try {
                return std::make_unique<EngineFacade>(config, std::move(xa), std::string());
            } catch (...) {
                return nullptr;
            }
        }
        try {
            const std::uint32_t channels = config.speakers != 0 ? config.speakers : 2;
            return std::make_unique<EngineFacade>(config, std::make_unique<SoftBackend>(config, channels), error);
        } catch (...) {
            return nullptr;
        }
    }
    return CreateSoftwareEngine(config);
}

EngineConfig EngineConfigFromSettings() noexcept {
    EngineConfig c;
    try {
        const core::Config& cfg = core::GetConfig();
        c.enabled = cfg.GetBool("audio", "enabled", true);
        c.deviceId = core::Utf8ToWide(cfg.GetString("audio", "device", ""));
        std::uint32_t speakers = 0;
        if (ParseSpeakers(cfg.GetString("audio", "speakers", "auto"), speakers)) {
            c.speakers = speakers;
        }
        const std::uint32_t volume = std::min<std::uint32_t>(cfg.GetU32("audio", "master_volume", 100), 100);
        c.masterVolume = static_cast<float>(volume) / 100.0f;
        c.reverb = cfg.GetBool("audio", "reverb", true);
        c.limiter = cfg.GetBool("audio", "limiter", true);
        const std::string test = cfg.GetString("audio", "device_test", "off");
        if (test == "absent") {
            c.deviceTest = EngineConfig::DeviceTest::Absent;
        } else if (test == "loss") {
            c.deviceTest = EngineConfig::DeviceTest::Loss;
        }
        if (cfg.GetBool("audio", "dummy_streams", false)) {
            FUT_LOGI(Snd, "audio: [audio] dummy_streams has no effect since M5 (streams always play)");
        }
    } catch (...) {
    }
    return c;
}

AudioEngine& Engine() noexcept {
    if (AudioEngine* t = g_testEngine.load()) {
        return *t;
    }
    AudioEngine* created = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        if (g_engine == nullptr) {
            const EngineConfig config = EngineConfigFromSettings();
            g_engine = CreateAudioEngine(config);
            if (g_engine == nullptr) {
                EngineConfig fallback = config;
                fallback.enabled = false;
                g_engine = CreateSoftwareEngine(fallback);
            }
            if (g_engine == nullptr) {
                core::Fatal("audio: cannot create even the software mixer (out of memory)");
            }
            const auto* f = static_cast<const EngineFacade*>(g_engine.get());
            if (g_engine->HasDevice()) {
                FUT_LOGI(Snd, "audio: %s, master volume %.0f %%", f->Description().c_str(),
                         static_cast<double>(config.masterVolume) * 100.0);
            } else if (config.enabled) {
                // INFO like "window: display off" (TV switched off); the sound starts when a device appears.
                FUT_LOGI(Snd, "audio: %s - the game runs without sound output until a device is available",
                         f->Description().c_str());
            } else {
                FUT_LOGI(Snd, "audio: %s", f->Description().c_str());
            }
            created = g_engine.get();
        }
    }
    if (created != nullptr) {
        if (EngineObserver* o = Observer().load()) {
            o->OnEngineCreated(*created);  // e.g. the capture session ([audio] capture_wav, event_log)
        }
    }
    return *g_engine;
}

void SimulateDeviceLossForTesting(AudioEngine& engine) noexcept {
    static_cast<EngineFacade&>(engine).SimulateDeviceLoss();
}

void SwitchToNewSoftwareBackendForTesting(AudioEngine& engine) noexcept {
    static_cast<EngineFacade&>(engine).SwitchToNewSoftware();
}

void RetryDeviceNowForTesting(AudioEngine& engine) noexcept {
    static_cast<EngineFacade&>(engine).RetryDeviceNow();
}

std::string EngineDescriptionForTesting(const AudioEngine& engine) {
    return static_cast<const EngineFacade&>(engine).Description();
}

void SetEngineObserver(EngineObserver* observer) noexcept {
    Observer().store(observer);
}

void SetEngineForTesting(AudioEngine* engine) noexcept {
    g_testEngine.store(engine);
}

void LogEngineSummary() noexcept {
    std::unique_lock<std::mutex> lock(g_engineMutex, std::try_to_lock);  // see EngineFacade::GetStats
    if (!lock.owns_lock() || g_engine == nullptr) {
        return;
    }
    const AudioEngine::Stats s = g_engine->GetStats();
    const auto* f = static_cast<const EngineFacade*>(g_engine.get());
    std::string description;
    double seconds = 0.0;
    if (!f->SummaryText(description, seconds)) {
        return;  // the guest thread is switching the backend (crash reporter): no summary rather than a race
    }
    FUT_LOGI(Snd,
             "audio summary: %s; voices %u created, %u live, peak %u; %.1f s mixed; glitches %u; device losses %u; "
             "limiter active on %llu frames (max %.1f dB)",
             description.c_str(), s.voicesCreated, s.voicesLive, s.voicesPeak, seconds, s.glitches, s.deviceLosses,
             static_cast<unsigned long long>(s.limitedFrames), static_cast<double>(s.limiterMaxReductionDb));
}

}  // namespace snd
