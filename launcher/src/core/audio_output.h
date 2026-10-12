// Audio output of the launcher's "Audio" tab (M5): the Windows render endpoints (WASAPI IMMDeviceEnumerator, the same
// IDs the game's [audio] device key and XAudio2's CreateMasteringVoice take), a generated test tone (no game data)
// and a player that sends it through XAudio2 2.9 to the selected device - the same API and device selection the
// game uses, so a working test tone means the game can open the device.
//
// Test tone: a short chime per speaker in channel order (stereo: left, right; 5.1: front left, front right, center,
// LFE (low tone), back left, back right), then all main speakers together; -12 dBFS peak times the master volume,
// 15 ms fades, no clipping.
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace fl {

struct AudioDevice {
    QString id;        // IMMDevice::GetId (e.g. "{0.0.0.00000000}.{guid}")
    QString name;      // friendly name ("LG TV (NVIDIA High Definition Audio)")
    bool isDefault = false;  // the Windows default render device (console role)
    int channels = 0;  // mix format of the endpoint (0 = unknown)
    int sampleRate = 0;
};

struct AudioDeviceList {
    QList<AudioDevice> devices;  // active render endpoints, default first
    bool enumerated = false;     // false: WASAPI not available (error says why)
    QString error;
    bool simulated = false;      // from --audio-file (development, screenshots)

    const AudioDevice* defaultDevice() const;
    const AudioDevice* find(const QString& id) const;  // case-insensitive id
    // The device a key value means: empty = default device; nullptr if the id is not connected.
    const AudioDevice* resolve(const QString& value) const;
};

// Active render endpoints of this machine.
AudioDeviceList EnumerateAudioDevices();
// Development/test data: {"devices":[{"id","name","default","channels","rate"}]}; false + error if unusable.
bool ParseAudioDeviceList(const QByteArray& json, AudioDeviceList* out, QString* error);
// "LG TV (2 channels, 48 kHz)" in the UI language; without the format when unknown.
QString AudioDeviceText(const AudioDevice& device);

// ---- test tone ------------------------------------------------------------------------------------------------
struct ToneSegment {
    int channel = -1;  // -1 = all main speakers
    double startSeconds = 0;
    double seconds = 0;
    double frequency = 0;
};
struct TestTone {
    int channels = 2;
    int sampleRate = 48000;
    std::vector<float> samples;  // interleaved
    QList<ToneSegment> segments;
    double seconds() const { return sampleRate > 0 ? double(samples.size()) / channels / sampleRate : 0.0; }
};
inline constexpr double kTestTonePeakDb = -12.0;
// Speaker order of a channel count (WAVEFORMATEXTENSIBLE default masks): 1 C, 2 L R, 4 L R BL BR, 6 L R C LFE BL BR,
// 8 L R C LFE BL BR SL SR. LFE gets a low tone; the "all" chime leaves the LFE out.
TestTone MakeTestTone(int channels, int sampleRate, double volume);
bool IsLfeChannel(int channels, int channel);
// Localized speaker name of `channel` ("links", "rechts", "Mitte", "Subwoofer", ...).
QString SpeakerName(int channels, int channel);

// ---- player ----------------------------------------------------------------------------------------------------
struct ToneRequest {
    QString deviceId;  // empty = Windows default device
    int speakers = 0;  // 0 = device layout, 2 = stereo, 6 = 5.1
    double volume = 1.0;  // linear 0..1 (master volume)
};
struct ToneResult {
    bool ok = false;
    QString error;  // localized
    int channels = 0;
    int sampleRate = 0;
    double seconds = 0;
    QStringList speakers;  // speaker names in the order they sounded
    bool fellBack = false;  // the requested device was not available: played on the Windows default device (as the
                            // game does)
};

// Maps the INI values of [audio] speakers / master_volume (whatever their schema says: "auto"/"stereo"/"5.1" or
// channel counts; percent 0..100 or a factor 0..1) to the request fields.
int SpeakersFromValue(const QString& value);
double VolumeFromValue(const QString& value);

// Plays the tone on a worker thread (COM + XAudio2 there) and emits finished() on the owner's thread. One tone at a
// time: play() while busy returns false. Destroying the player (the tab is rebuilt, the window closes) stops a
// running tone at once instead of waiting for its end.
class TonePlayer : public QObject {
    Q_OBJECT
public:
    explicit TonePlayer(QObject* parent = nullptr);
    ~TonePlayer() override;
    bool play(const ToneRequest& request);
    bool busy() const { return busy_; }

    // Tests: replaces the XAudio2 backend (called on the worker thread). nullptr = real backend.
    using Backend = std::function<ToneResult(const ToneRequest&)>;
    static void SetBackendForTesting(Backend backend);
    // The real backend, synchronous (blocks for the tone's length unless *cancel becomes true). A device that is not
    // available falls back to the Windows default device like the game (ToneResult::fellBack).
    static ToneResult PlayWithXAudio2(const ToneRequest& request, const std::atomic<bool>* cancel = nullptr);

signals:
    void started();
    void finished(const fl::ToneResult& result);

private:
    bool busy_ = false;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};

}  // namespace fl

Q_DECLARE_METATYPE(fl::ToneResult)
