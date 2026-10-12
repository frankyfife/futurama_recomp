// M5 audio: shared interface of the audio modules (src/INTERFACES.md 4.29; analysis/loader/m5_audio.md 6, 7).
//
// Written first by the DSOUND core agent; the other M5 agents extend it ADDITIVELY (new declarations only, a
// Changelog row in src/INTERFACES.md 8). Contents:
//   - categories (the game's volume categories 0/1/2 + movie) and formats,
//   - Decoder: PCM8/PCM16/Xbox ADPCM -> interleaved PCM16 (factory CreateDecoder),
//   - the DSOUND level model shared by the 2D (DSOUND core) and 3D (Spatializer) paths,
//   - Spatializer: listener + one 3D emitter -> output matrix, reverb send, LFE (factory CreateSpatializer),
//   - AudioEngine + SourceVoice: XAudio2 2.9 graph (mastering voice, category submixes, reverb submix) or the
//     device-less software mixer with the same graph, plus mix taps (capture_wav, stems).
//
// Threading (design E10, m5_audio.md 7): every DSOUND entry runs on the guest thread (main thread and its
// fibers). The engine's mixer thread (XAudio2 or the software mixer) calls VoiceEvents and MixTap callbacks;
// those must never touch guest memory, never call guest code and never create/destroy voices - they only
// queue events that DirectSoundDoWork applies on the guest thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace snd {

// ---- categories ------------------------------------------------------------------------------------------------
// The game's volume categories (SoundVoice+0x70, m5_audio.md 6.7): 0 effects (also movies, Movie::Open 0xEE43D),
// 1 speech, 2 music. Movie = DSOUND objects without a CreateDsObject tag (XMV streams, M6).
enum class Category : std::uint8_t { Effects = 0, Speech = 1, Music = 2, Movie = 3 };
inline constexpr std::uint32_t kCategoryCount = 4;
const char* CategoryName(Category category) noexcept;  // "effects", "speech", "music", "movie"

// ---- formats ---------------------------------------------------------------------------------------------------
inline constexpr std::uint16_t kFormatTagPcm = 1;
inline constexpr std::uint16_t kFormatTagXboxAdpcm = 0x69;  // 64 samples per 36-byte block and channel
inline constexpr std::uint32_t kXboxAdpcmBlockBytesPerChannel = 36;
inline constexpr std::uint32_t kXboxAdpcmSamplesPerBlock = 64;

// The fields of a guest WAVEFORMATEX that matter. nAvgBytesPerSec and cbSize of the game's formats are wrong
// (m5_audio.md 2.3, trap 6) and deliberately absent.
struct SourceFormat {
    std::uint16_t formatTag = kFormatTagPcm;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t blockAlign = 0;     // PCM: channels * bits / 8; ADPCM: 36 * channels (recomputed, see Valid)
    std::uint16_t bitsPerSample = 0;  // 8 or 16 (PCM), 4 (ADPCM)
};

// Decoded format: always interleaved signed PCM16 (PCM8 is converted), channels and rate of the source.
struct PcmFormat {
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t FrameBytes() const noexcept { return 2u * channels; }
};

// ---- decoder ---------------------------------------------------------------------------------------------------
// Converts guest sample data to PCM16. Xbox ADPCM: header sample + first 63 nibbles per block and channel, step
// diff = ((2*(n & 7) + 1) * step) >> 3: bit-exact with ffmpeg adpcm_ima_xbox (m5_audio.md 4.5). Whether the Xbox
// hardware rounds the same way is not established: the IMA reference uses the shift-add form (step>>3 + step>>2 ...),
// which differs by at most 28 LSB on the game's files (below -61 dBFS, m5_adpcm_check.out). A trailing partial
// block is decoded as if zero-padded (m5_audio.md 6.3); a trailing partial PCM frame is dropped.
class Decoder {
public:
    virtual ~Decoder() = default;
    virtual const SourceFormat& Source() const noexcept = 0;
    virtual PcmFormat Output() const noexcept = 0;
    // Output frames produced by Decode for `inputBytes` of input.
    virtual std::size_t FramesFor(std::size_t inputBytes) const noexcept = 0;
    // Decodes `input` into `out` (FramesFor(input.size()) * channels samples). Returns the frames written.
    virtual std::size_t Decode(std::span<const std::uint8_t> input, std::span<std::int16_t> out) noexcept = 0;
};
// nullptr for unsupported formats (anything but PCM8/PCM16/Xbox ADPCM with 1..8 channels and 1..192000 Hz).
std::unique_ptr<Decoder> CreateDecoder(const SourceFormat& format) noexcept;

// ---- DSOUND level model (m5_audio.md 3.6) -------------------------------------------------------------------------
inline constexpr std::int32_t kMillibelMin = -10000;     // DSBVOLUME_MIN
inline constexpr std::int32_t kHeadroom2DMillibel = 600;  // default headroom of non-3D voices (0x248A63); 3D: 0
// Linear gain of one mixbin for a total level in mB (lVolume - headroom + mixbinVolume + 3D terms): the hardware
// register (att * 64) / 100 in 1/64 dB, clamped to 0..0xFFF (FUN_0024C3CA) - so -10000 mB gives -63.98 dB, not 0.
float MixbinGain(std::int32_t totalMillibel) noexcept;
// Plain 10^(mB/2000) (0 for <= -10000), for host-side levels.
float MillibelToGain(std::int32_t millibel) noexcept;

// Xbox mixbin numbers (DSMIXBIN_*) used by the game and DSOUND's defaults.
enum MixBin : std::uint32_t {
    kMixBinFrontLeft = 0, kMixBinFrontRight = 1, kMixBinFrontCenter = 2, kMixBinLowFrequency = 3,
    kMixBinBackLeft = 4, kMixBinBackRight = 5, kMixBinXtlkFrontLeft = 6, kMixBinXtlkFrontRight = 7,
    kMixBinXtlkBackLeft = 8, kMixBinXtlkBackRight = 9, kMixBinI3dl2 = 10, kMixBinCount = 36
};

// ---- 3D (DS3D state as the guest set it; applied at CommitDeferredSettings) --------------------------------------
struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Listener3D {
    Vec3 position;
    Vec3 velocity;
    Vec3 front{0, 0, 1};  // as passed to IDirectSound_SetOrientation (the game passes -view direction, 3.7)
    Vec3 top{0, 1, 0};
    float distanceFactor = 1.0f;
    float dopplerFactor = 1.0f;
    float rolloffFactor = 1.0f;
};
// DSI3DL2BUFFER (36 bytes, m5_audio.md 2.3).
struct I3dl2Source {
    std::int32_t direct = 0;
    std::int32_t directHF = 0;
    std::int32_t room = 0;
    std::int32_t roomHF = 0;
    float roomRolloffFactor = 0;
    std::int32_t obstructionHF = 0;
    float obstructionLFRatio = 0;
    std::int32_t occlusionHF = 0;
    float occlusionLFRatio = 0;
};
inline constexpr std::uint32_t kMaxRolloffPoints = 16;
struct Emitter3D {
    Vec3 position;
    Vec3 velocity;
    float minDistance = 1.0f;  // DS3D_DEFAULTMINDISTANCE
    float maxDistance = 1.0e9f;
    std::uint32_t bufferFlags = 0;  // DSBUFFERDESC/DSSTREAMDESC dwFlags (0x10 3D, 0x20000 mute at max distance)
    std::uint32_t rolloffCount = 0;  // 0 = no curve (rolloffFactor rule)
    float rolloff[kMaxRolloffPoints] = {};
    I3dl2Source i3dl2;
    std::int32_t volumeMillibel = 0;  // SetVolume value (3D voices have no headroom)
    std::uint32_t channels = 1;       // source channels (the game only plays mono samples in 3D)
};
// Result for one emitter: per source channel x output channel gains (row-major, [src][dst]) towards the voice's
// category submix, the mono send to the reverb submix and the LFE gain (already inside `matrix` when the output
// has an LFE channel; reported separately for logs).
inline constexpr std::uint32_t kMaxOutputChannels = 8;
struct SpatialResult {
    float matrix[kMaxOutputChannels * kMaxOutputChannels] = {};
    float reverbSend = 0;
    float lfe = 0;
    float distanceGain = 1;  // the DSOUND rolloff curve result (for event logs)
};
class Spatializer {
public:
    virtual ~Spatializer() = default;
    // Output layout of the engine (channel count, speaker mask of the mastering voice).
    virtual void Compute(const Listener3D& listener, const Emitter3D& emitter, std::uint32_t outputChannels,
                         std::uint32_t outputChannelMask, SpatialResult& out) noexcept = 0;
};
std::unique_ptr<Spatializer> CreateSpatializer() noexcept;

// I3DL2 listener (DSI3DL2LISTENER, 48 bytes, table 0x2D6818).
struct I3dl2Listener {
    std::int32_t room = -1000;
    std::int32_t roomHF = -100;
    float roomRolloffFactor = 0;
    float decayTime = 1.49f;
    float decayHFRatio = 0.83f;
    std::int32_t reflections = -2602;
    float reflectionsDelay = 0.007f;
    std::int32_t reverb = 200;
    float reverbDelay = 0.011f;
    float diffusion = 100.0f;
    float density = 100.0f;
    float hfReference = 5000.0f;
};

// ---- engine ------------------------------------------------------------------------------------------------------
// Called on the mixer thread. Must only record/queue (no guest memory, no guest calls, no voice create/destroy).
class VoiceEvents {
public:
    virtual ~VoiceEvents() = default;
    // A submitted buffer finished playing or was flushed (`context` = SubmitDesc::context).
    virtual void OnBufferEnd(std::uint32_t context) noexcept = 0;
};

struct VoiceDesc {
    PcmFormat format;  // PCM16
    Category category = Category::Effects;
    bool reverbSend = false;           // 3D voices: mono send to the reverb submix
    float maxFrequencyRatio = 4.0f;    // pitch up to 4 (0xCA110)
    VoiceEvents* events = nullptr;     // must outlive the voice
    const char* debugName = nullptr;   // copied
};

struct SubmitDesc {
    const std::int16_t* data = nullptr;  // PCM16 frames; must stay valid until OnBufferEnd or voice destruction
    std::uint32_t frames = 0;
    std::uint32_t playBegin = 0;   // first frame played
    bool loop = false;             // loop over the whole buffer [0, frames) after reaching the end, forever
    std::uint32_t context = 0;     // passed to VoiceEvents::OnBufferEnd
};

class SourceVoice {
public:
    // Destruction is synchronous: no VoiceEvents call after the destructor returned.
    virtual ~SourceVoice() = default;
    virtual bool Submit(const SubmitDesc& buffer) noexcept = 0;
    virtual void Start() noexcept = 0;
    virtual void Stop() noexcept = 0;   // pause; queued buffers stay
    virtual void Flush() noexcept = 0;  // drop queued buffers (OnBufferEnd follows for each, asynchronously)
    // No more buffers follow the queued ones (IXAudio2SourceVoice::Discontinuity).
    virtual void Discontinuity() noexcept = 0;
    // Source frames played since creation (monotonic; loops count every pass).
    virtual std::uint64_t FramesPlayed() noexcept = 0;
    virtual std::uint32_t BuffersQueued() noexcept = 0;
    virtual void SetFrequencyRatio(float ratio) noexcept = 0;
    // [src][dst] gains towards the category submix (dst = AudioEngine::OutputChannels()).
    virtual void SetOutputMatrix(std::span<const float> levels) noexcept = 0;
    virtual void SetReverbSend(float gain) noexcept = 0;  // ignored unless VoiceDesc::reverbSend
    // Set via AudioEngine::BeginBatch/CommitBatch: changes between the two are applied together (operation set).
};

// Mix tap (capture_wav, stems): called on the mixer thread with each processed block (interleaved float).
enum class TapPoint : std::uint8_t { Master = 0, Effects, Speech, Music, Movie, Reverb, Count };
class MixTap {
public:
    virtual ~MixTap() = default;
    virtual void OnMix(TapPoint point, const float* samples, std::uint32_t frames, std::uint32_t channels,
                       std::uint32_t sampleRate) noexcept = 0;
};

struct EngineConfig {
    bool enabled = true;                // false: software mixer without output (the game runs the same)
    std::wstring deviceId;              // empty = Windows default endpoint
    std::uint32_t speakers = 0;         // 0 = device layout, 2 = stereo, 6 = 5.1
    float masterVolume = 1.0f;          // linear, on top of the game's levels (after the taps and the limiter input)
    bool reverb = true;
    bool limiter = true;                // peak limiter at -0.1 dBFS after the Master tap
    std::uint32_t softwareSampleRate = 48000;  // software mixer (no device)
    bool softwareRealtime = true;       // false: the software mixer only renders on RenderSoftware() (tests)
    // [audio] device_test: Absent = behave as if Windows had no output endpoint (software mixer for the whole run);
    // Loss = a simulated device loss 20 s after every XAudio2 start (back at the next 3 s endpoint look).
    enum class DeviceTest : std::uint8_t { Off, Absent, Loss };
    DeviceTest deviceTest = DeviceTest::Off;
};

class AudioEngine {
public:
    virtual ~AudioEngine() = default;
    virtual bool HasDevice() const noexcept = 0;  // false: software mixer (no device / audio disabled)
    virtual std::uint32_t OutputChannels() const noexcept = 0;
    virtual std::uint32_t OutputChannelMask() const noexcept = 0;
    virtual std::uint32_t OutputSampleRate() const noexcept = 0;
    // Format of the blocks MixTap::OnMix receives at `point` (Reverb: the reverb effect's channels and rate); false
    // if the point delivers nothing (e.g. no reverb submix). Lets the capture session allocate its rings up front.
    virtual bool TapFormat(TapPoint point, std::uint32_t& channels, std::uint32_t& sampleRate) const noexcept {
        static_cast<void>(point);
        static_cast<void>(channels);
        static_cast<void>(sampleRate);
        return false;
    }
    virtual std::unique_ptr<SourceVoice> CreateVoice(const VoiceDesc& desc) noexcept = 0;
    virtual void SetReverb(const I3dl2Listener& listener) noexcept = 0;
    virtual void SetMasterVolume(float gain) noexcept = 0;
    // Batching of voice parameter changes (one XAudio2 operation set per tick).
    virtual void BeginBatch() noexcept = 0;
    virtual void CommitBatch() noexcept = 0;
    // Guest thread, once per DirectSoundDoWork: device handling - after a loss the software mixer takes over at
    // once; a device that appears (Windows device notification, or a look every 3 s) or a new default device is
    // opened by a host worker thread and handed over here (no waiting on the guest thread).
    virtual void Pump() noexcept = 0;
    // At most one tap; nullptr removes it. The tap must outlive its registration.
    virtual void SetTap(MixTap* tap) noexcept = 0;
    // Software mixer only: renders `frames` output frames synchronously (tests; softwareRealtime = false).
    virtual std::uint32_t RenderSoftware(std::uint32_t frames, float* out) noexcept = 0;
    // Counters for logs and checks.
    struct Stats {
        std::uint32_t voicesCreated = 0;
        std::uint32_t voicesLive = 0;
        std::uint32_t voicesPeak = 0;
        std::uint32_t glitches = 0;         // XAUDIO2_PERFORMANCE_DATA::GlitchesSinceEngineStarted
        std::uint32_t deviceLosses = 0;
        float limiterMaxReductionDb = 0;    // [audio] limiter: deepest gain reduction so far (0 = never active)
        std::uint64_t limitedFrames = 0;    // output frames the limiter reduced
    };
    virtual Stats GetStats() const noexcept = 0;
    virtual const char* BackendName() const noexcept = 0;  // "xaudio2", "software"
    // Output frames mixed since the engine started (sample clock of capture_wav; any thread).
    virtual std::uint64_t OutputFramesRendered() const noexcept = 0;
};

std::unique_ptr<AudioEngine> CreateAudioEngine(const EngineConfig& config) noexcept;
// Reads [audio] enabled / device / speakers / master_volume / reverb / limiter (core::GetConfig()).
EngineConfig EngineConfigFromSettings() noexcept;

// ---- DSOUND object events (event log next to capture_wav, statistics) -------------------------------------------
// Reported by the DSOUND HLE on the guest thread, synchronously inside the entry. `object` is the guest address of
// the IDirectSoundBuffer / stream; name/category come from the CreateDsObject probe (SoundVoice 0xCC150,
// m5_audio.md 6.1) and are empty / Movie for untagged objects (XMV).
enum class SoundEventKind : std::uint8_t {
    Create, Release, Play, Stop, StreamPause, StreamResume, Flush, Volume, Frequency, Position3D, Ended, PacketsLate,
    OrphanedObject,  // second Play on a running SoundVoice: the previous DSOUND object is never released (3.3 item 6)
    Level3D          // diagnostics: the applied direct level of a playing 3D voice moved by >= 3 dB (or to/from mute)
};
struct SoundEvent {
    SoundEventKind kind = SoundEventKind::Create;
    std::uint32_t object = 0;     // guest address of the DSOUND object
    std::uint32_t voiceId = 0;    // SoundVoice+0x6C (0 = untagged)
    Category category = Category::Movie;
    bool is3D = false;
    bool isStream = false;
    bool looping = false;
    const char* name = "";        // sound file name from the probe ("" if untagged); valid during the call only
    std::int32_t millibel = 0;    // Volume: SetVolume value
    std::uint32_t frequency = 0;  // Frequency: Hz (0 = original rate)
    Vec3 position;                // Position3D
    std::uint64_t outputFrame = 0;  // AudioEngine::OutputFramesRendered() at the event
    // Diagnostics (M5 integration): the level last applied to the voice. 3D: lVolume + DSOUND distance term in dB
    // (before direction and the hardware register; <= -100 = muted) and the listener distance in game units;
    // 2D: lVolume - 600 mB headroom, distance -1.
    float levelDb = 0;
    float distance = -1;
    float minDistance = 0;  // 3D: committed Min/MaxDistance and rolloff curve size of the emitter
    float maxDistance = 0;
    std::uint32_t rolloffPoints = 0;
};
class SoundEventSink {
public:
    virtual ~SoundEventSink() = default;
    virtual void OnSoundEvent(const SoundEvent& event) noexcept = 0;  // guest thread
};
// At most one sink; nullptr removes it. Volume/Frequency/Position3D are reported only when the value changes.
void SetSoundEventSink(SoundEventSink* sink) noexcept;

// Process-wide engine used by the DSOUND HLE: created on first use (the game's DirectSoundCreate) from
// EngineConfigFromSettings().
AudioEngine& Engine() noexcept;
// Tests: replaces the process engine (nullptr = back to the default one). Not thread-safe against DSOUND calls.
void SetEngineForTesting(AudioEngine* engine) noexcept;

// Observer of the process engine (e.g. the capture session of [audio] capture_wav: SetTap + SetSoundEventSink).
// OnEngineCreated runs once on the thread that created the engine (the guest thread, inside DirectSoundCreate), with
// no engine lock held; OnBackendChanged runs on the guest thread inside DirectSoundDoWork after the engine switched
// between XAudio2 and the software mixer (device lost / available again; the output format stays the same). At most
// one observer; register it before the game starts (static initialisation is fine).
class EngineObserver {
public:
    virtual ~EngineObserver() = default;
    virtual void OnEngineCreated(AudioEngine& engine) noexcept = 0;
    virtual void OnBackendChanged(AudioEngine& engine) noexcept { static_cast<void>(engine); }
};
void SetEngineObserver(EngineObserver* observer) noexcept;

}  // namespace snd
