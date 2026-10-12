#include "core/audio_output.h"

#include "core/l10n.h"

// clang-format off
#include <windows.h>
#include <initguid.h>  // defines the PKEY_* / CLSID values of the headers below in this file
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <xaudio2.h>
// clang-format on

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numbers>

namespace fl {
namespace {

template <class T>
void SafeRelease(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

QString HrText(HRESULT hr) {
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')).toUpper().replace(
        QLatin1String("0X"), QLatin1String("0x"));
}

// COM on the calling thread for the lifetime of the object (keeps the apartment Qt chose).
class ComScope {
public:
    explicit ComScope(DWORD model) : hr_(CoInitializeEx(nullptr, model)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    bool usable() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (ksmedia.h needs ks.h first; the value is fixed).
constexpr GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

std::mutex g_backendMutex;
TonePlayer::Backend g_backend;

}  // namespace

// ---- device list -----------------------------------------------------------------------------------------------

const AudioDevice* AudioDeviceList::defaultDevice() const {
    for (const AudioDevice& d : devices) {
        if (d.isDefault) {
            return &d;
        }
    }
    return nullptr;
}

const AudioDevice* AudioDeviceList::find(const QString& id) const {
    for (const AudioDevice& d : devices) {
        if (d.id.compare(id.trimmed(), Qt::CaseInsensitive) == 0) {
            return &d;
        }
    }
    return nullptr;
}

const AudioDevice* AudioDeviceList::resolve(const QString& value) const {
    return value.trimmed().isEmpty() ? defaultDevice() : find(value);
}

AudioDeviceList EnumerateAudioDevices() {
    AudioDeviceList list;
    ComScope com(COINIT_APARTMENTTHREADED);
    if (!com.usable()) {
        list.error = QStringLiteral("CoInitializeEx failed");
        return list;
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        list.error = QStringLiteral("MMDeviceEnumerator ") + HrText(hr);
        return list;
    }
    QString defaultId;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            defaultId = QString::fromWCharArray(id);
            CoTaskMemFree(id);
        }
        SafeRelease(def);
    }
    IMMDeviceCollection* coll = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr)) {
        list.error = QStringLiteral("EnumAudioEndpoints ") + HrText(hr);
        SafeRelease(enumerator);
        return list;
    }
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev))) {
            continue;
        }
        AudioDevice d;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            d.id = QString::fromWCharArray(id);
            CoTaskMemFree(id);
        }
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                d.name = QString::fromWCharArray(v.pwszVal);
            }
            PropVariantClear(&v);
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_AudioEngine_DeviceFormat, &v)) && v.vt == VT_BLOB &&
                v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                const auto* wf = reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData);
                d.channels = wf->nChannels;
                d.sampleRate = static_cast<int>(wf->nSamplesPerSec);
            }
            PropVariantClear(&v);
            SafeRelease(props);
        }
        if (d.name.isEmpty()) {
            d.name = d.id;
        }
        d.isDefault = !defaultId.isEmpty() && d.id == defaultId;
        list.devices.append(d);
        SafeRelease(dev);
    }
    SafeRelease(coll);
    SafeRelease(enumerator);
    std::stable_sort(list.devices.begin(), list.devices.end(),
                     [](const AudioDevice& a, const AudioDevice& b) { return a.isDefault && !b.isDefault; });
    list.enumerated = true;
    return list;
}

bool ParseAudioDeviceList(const QByteArray& json, AudioDeviceList* out, QString* error) {
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isObject() || !doc.object().value(QLatin1String("devices")).isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("not an audio device list: ") + pe.errorString();
        }
        return false;
    }
    AudioDeviceList list;
    for (const QJsonValue& v : doc.object().value(QLatin1String("devices")).toArray()) {
        const QJsonObject o = v.toObject();
        AudioDevice d;
        d.id = o.value(QLatin1String("id")).toString();
        d.name = o.value(QLatin1String("name")).toString(d.id);
        d.isDefault = o.value(QLatin1String("default")).toBool(false);
        d.channels = o.value(QLatin1String("channels")).toInt(0);
        d.sampleRate = o.value(QLatin1String("rate")).toInt(0);
        if (!d.id.isEmpty()) {
            list.devices.append(d);
        }
    }
    std::stable_sort(list.devices.begin(), list.devices.end(),
                     [](const AudioDevice& a, const AudioDevice& b) { return a.isDefault && !b.isDefault; });
    list.enumerated = true;
    list.simulated = true;
    *out = list;
    return true;
}

QString AudioDeviceText(const AudioDevice& device) {
    if (device.channels <= 0 || device.sampleRate <= 0) {
        return device.name;
    }
    const QString khz = QLocale().toString(device.sampleRate / 1000.0, 'g', 4);
    return T("audio.device_format").arg(device.name).arg(device.channels).arg(khz);
}

// ---- test tone -------------------------------------------------------------------------------------------------

bool IsLfeChannel(int channels, int channel) {
    return (channels == 6 || channels == 8) && channel == 3;
}

QString SpeakerName(int channels, int channel) {
    if (channel < 0) {
        return T("audio.speaker.all");
    }
    if (channels == 1) {
        return T("audio.speaker.center");
    }
    static const char* const kSix[] = {"audio.speaker.left",       "audio.speaker.right",     "audio.speaker.center",
                                       "audio.speaker.lfe",        "audio.speaker.back_left", "audio.speaker.back_right",
                                       "audio.speaker.side_left",  "audio.speaker.side_right"};
    static const char* const kFour[] = {"audio.speaker.left", "audio.speaker.right", "audio.speaker.back_left",
                                        "audio.speaker.back_right"};
    if (channels == 4 && channel < 4) {
        return T(kFour[channel]);
    }
    if (channel < 8 && (channels == 2 || channels >= 6)) {
        return T(kSix[channel]);
    }
    return T("audio.speaker.channel").arg(channel + 1);
}

TestTone MakeTestTone(int channels, int sampleRate, double volume) {
    TestTone t;
    t.channels = std::clamp(channels, 1, 8);
    t.sampleRate = std::clamp(sampleRate, 8000, 192000);
    const double peak = std::pow(10.0, kTestTonePeakDb / 20.0) * std::clamp(volume, 0.0, 1.0);
    constexpr double kChime = 0.45;
    constexpr double kGap = 0.12;
    constexpr double kAll = 0.6;
    constexpr double kFade = 0.015;
    // C5 E5 G5 C6 ... per speaker, a low A for the LFE, C-major chord for "all"
    static const double kNotes[] = {523.25, 659.25, 783.99, 80.0, 587.33, 698.46, 880.0, 987.77};
    double at = 0.05;
    for (int c = 0; c < t.channels; ++c) {
        const double f = IsLfeChannel(t.channels, c) ? 80.0 : kNotes[c % 8];
        t.segments.append({c, at, kChime, f});
        at += kChime + kGap;
    }
    if (t.channels > 1) {
        t.segments.append({-1, at, kAll, 523.25});
        at += kAll;
    }
    at += 0.05;
    const auto frames = static_cast<std::size_t>(std::ceil(at * t.sampleRate));
    t.samples.assign(frames * t.channels, 0.0f);
    for (const ToneSegment& s : t.segments) {
        const auto first = static_cast<std::size_t>(s.startSeconds * t.sampleRate);
        const auto n = static_cast<std::size_t>(s.seconds * t.sampleRate);
        for (std::size_t i = 0; i < n && first + i < frames; ++i) {
            const double time = static_cast<double>(i) / t.sampleRate;
            const double env = std::min({1.0, time / kFade, (s.seconds - time) / kFade});
            double x = std::sin(2.0 * std::numbers::pi * s.frequency * time);
            if (s.channel < 0) {  // chord: root + third + fifth, scaled to the same peak
                x = (x + std::sin(2.0 * std::numbers::pi * s.frequency * 1.25992 * time) +
                     std::sin(2.0 * std::numbers::pi * s.frequency * 1.49831 * time)) /
                    3.0;
            }
            const auto v = static_cast<float>(peak * std::max(0.0, env) * x);
            float* frame = t.samples.data() + (first + i) * t.channels;
            if (s.channel >= 0) {
                frame[s.channel] = v;
            } else {
                for (int c = 0; c < t.channels; ++c) {
                    if (!IsLfeChannel(t.channels, c)) {
                        frame[c] = v;
                    }
                }
            }
        }
    }
    return t;
}

int SpeakersFromValue(const QString& value) {
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("5.1") || v == QLatin1String("6") || v == QLatin1String("surround")) {
        return 6;
    }
    if (v == QLatin1String("7.1") || v == QLatin1String("8")) {
        return 8;
    }
    if (v == QLatin1String("stereo") || v == QLatin1String("2") || v == QLatin1String("2.0")) {
        return 2;
    }
    if (v == QLatin1String("mono") || v == QLatin1String("1")) {
        return 1;
    }
    return 0;
}

double VolumeFromValue(const QString& value) {
    bool ok = false;
    double v = QLocale::c().toDouble(value.trimmed(), &ok);
    if (!ok) {
        return 1.0;
    }
    if (v > 1.0) {
        v /= 100.0;  // percent
    }
    return std::clamp(v, 0.0, 1.0);
}

// ---- player ----------------------------------------------------------------------------------------------------

TonePlayer::TonePlayer(QObject* parent) : QObject(parent) {
    qRegisterMetaType<fl::ToneResult>();
}

TonePlayer::~TonePlayer() {
    cancel_.store(true);  // the worker stops the tone and returns at once (no frozen window while a tone plays)
    if (worker_.joinable()) {
        worker_.join();
    }
}

void TonePlayer::SetBackendForTesting(Backend backend) {
    std::lock_guard<std::mutex> lock(g_backendMutex);
    g_backend = std::move(backend);
}

bool TonePlayer::play(const ToneRequest& request) {
    if (busy_) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    busy_ = true;
    cancel_.store(false);
    emit started();
    Backend backend;
    {
        std::lock_guard<std::mutex> lock(g_backendMutex);
        backend = g_backend;
    }
    worker_ = std::thread([this, request, backend] {
        const ToneResult r = backend ? backend(request) : PlayWithXAudio2(request, &cancel_);
        QMetaObject::invokeMethod(
            this,
            [this, r] {
                busy_ = false;
                emit finished(r);
            },
            Qt::QueuedConnection);
    });
    return true;
}

ToneResult TonePlayer::PlayWithXAudio2(const ToneRequest& request, const std::atomic<bool>* cancel) {
    const auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };
    ToneResult r;
    ComScope com(COINIT_MULTITHREADED);
    IXAudio2* xa = nullptr;
    HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        r.error = T("audio.err.xaudio2").arg(HrText(hr));
        return r;
    }
    IXAudio2MasteringVoice* master = nullptr;
    const std::wstring id = request.deviceId.trimmed().toStdWString();
    const UINT32 channels = request.speakers > 0 ? static_cast<UINT32>(request.speakers) : XAUDIO2_DEFAULT_CHANNELS;
    hr = xa->CreateMasteringVoice(&master, channels, XAUDIO2_DEFAULT_SAMPLERATE, 0, id.empty() ? nullptr : id.c_str(),
                                  nullptr, AudioCategory_GameEffects);
    if (FAILED(hr) && !id.empty()) {
        // Like the game (XaBackend::Init): a configured device that is not available -> the Windows default device.
        hr = xa->CreateMasteringVoice(&master, channels, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr,
                                      AudioCategory_GameEffects);
        r.fellBack = SUCCEEDED(hr);
    }
    if (FAILED(hr)) {
        r.error = (hr == static_cast<HRESULT>(0x80070490) || hr == static_cast<HRESULT>(0x88890004))
                      ? T("audio.err.no_device")
                      : T("audio.err.device").arg(HrText(hr));
        xa->Release();
        return r;
    }
    XAUDIO2_VOICE_DETAILS details{};
    master->GetVoiceDetails(&details);
    DWORD mask = 0;
    master->GetChannelMask(&mask);
    const TestTone tone = MakeTestTone(static_cast<int>(details.InputChannels),
                                       static_cast<int>(details.InputSampleRate), request.volume);
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = static_cast<WORD>(tone.channels);
    fmt.Format.nSamplesPerSec = static_cast<DWORD>(tone.sampleRate);
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = static_cast<WORD>(4 * tone.channels);
    fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = 22;
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = mask;
    fmt.SubFormat = kSubtypeIeeeFloat;
    IXAudio2SourceVoice* src = nullptr;
    hr = xa->CreateSourceVoice(&src, &fmt.Format);
    if (SUCCEEDED(hr)) {
        XAUDIO2_BUFFER buf{};
        buf.Flags = XAUDIO2_END_OF_STREAM;
        buf.AudioBytes = static_cast<UINT32>(tone.samples.size() * sizeof(float));
        buf.pAudioData = reinterpret_cast<const BYTE*>(tone.samples.data());
        hr = src->SubmitSourceBuffer(&buf);
        if (SUCCEEDED(hr)) {
            hr = src->Start();
        }
        if (SUCCEEDED(hr)) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(static_cast<int>(tone.seconds() * 1000.0) + 3000);
            for (;;) {
                XAUDIO2_VOICE_STATE vs{};
                src->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (vs.BuffersQueued == 0 || std::chrono::steady_clock::now() > deadline || cancelled()) {
                    break;
                }
                Sleep(20);
            }
            if (cancelled()) {
                src->Stop();
            } else {
                Sleep(80);
            }
        }
        src->DestroyVoice();
    }
    master->DestroyVoice();
    xa->Release();
    if (FAILED(hr)) {
        r.error = T("audio.err.device").arg(HrText(hr));
        return r;
    }
    r.ok = true;
    r.channels = tone.channels;
    r.sampleRate = tone.sampleRate;
    r.seconds = tone.seconds();
    for (const ToneSegment& s : tone.segments) {
        r.speakers.append(SpeakerName(tone.channels, s.channel));
    }
    return r;
}

}  // namespace fl
