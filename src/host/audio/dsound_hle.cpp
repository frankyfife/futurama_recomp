// DSOUND HLE on the audio engine (M5 DSOUND core agent). Summary and rules: dsound_hle.h. Evidence references are
// VAs of the retail EU XBE and sections of analysis/loader/m5_audio.md ("3.6" = section 3.6). "voice" is the game's
// SoundVoice (0xBC bytes: +0x74 IDirectSoundStream*, +0x7C..+0xA0 packet status DWORDs, +0xA4
// IDirectSoundBuffer*), "mgr" the SoundManager (+0x48 IDirectSound*).
//
// State model: one mutex (g_state.mutex) guards all DSOUND objects; it is held inside every entry and released
// before guest callbacks (XMV stream callbacks) are called. The engine's mixer thread only calls
// Object::OnBufferEnd, which takes the separate event mutex (lock order engine -> event queue; the event mutex is
// never held while calling the engine).
#include "audio/dsound_hle.h"

#include "audio/audio_engine.h"
#include "audio/codec_format.h"
#include "audio/dsound_mix.h"
#include "audio/reverb_i3dl2.h"
#include "audio/spatial_model.h"
#include "audio/xdk_entries.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/log.h"
#include "kernel/ke_wait.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snd {
namespace {

std::uint32_t A(const volatile void* p) noexcept {
    return guest::Addr(p);
}

// ---- guest handles -------------------------------------------------------------------------------------------------
// +0 vtable (trap vtable for the device and buffers - the game only passes them to the C API, m5_audio.md 2.2; the
// XMediaObject vtable for streams), the rest is host bookkeeping the guest never reads.
struct GuestHandle {
    std::uint32_t vtable;
    std::uint32_t magic;
    std::uint32_t id;
    std::uint32_t kind;
};
static_assert(sizeof(GuestHandle) == 16);
constexpr std::uint32_t kHandleMagic = 0x4C485344;  // 'DSHL'
enum class Kind : std::uint32_t { Device = 1, Buffer = 2, Stream = 3 };

// 4096 handles in one 64 KiB block below 0x80000000 (design 1.2). Separate from xdk::AllocGuestObject (1024 slots
// shared with XMV/XPP): the game creates one buffer per sound start and leaks one per double Play (3.3 item 6).
class HandlePool {
public:
    GuestHandle* Alloc(Kind kind, std::uint32_t id, std::uint32_t vtable) noexcept {
        if (!Ensure()) {
            return nullptr;
        }
        if (free_.empty()) {
            if (!exhaustedLogged_) {
                exhaustedLogged_ = true;
                FUT_LOGE(Snd, "DSOUND: handle pool exhausted (%u objects live)", kSlots);
            }
            return nullptr;
        }
        const std::uint32_t slot = free_.back();
        free_.pop_back();
        GuestHandle* h = base_ + slot;
        h->vtable = vtable;
        h->magic = kHandleMagic;
        h->id = id;
        h->kind = static_cast<std::uint32_t>(kind);
        return h;
    }
    void Free(GuestHandle* h) noexcept {
        if (h == nullptr || !Contains(h)) {
            return;
        }
        std::memset(h, 0, sizeof(*h));
        try {
            free_.push_back(static_cast<std::uint32_t>(h - base_));
        } catch (...) {
        }
    }
    bool Contains(const void* p) const noexcept {
        if (base_ == nullptr || p == nullptr) {
            return false;
        }
        const auto a = reinterpret_cast<std::uintptr_t>(p);
        const auto b = reinterpret_cast<std::uintptr_t>(base_);
        return a >= b && a < b + kSlots * sizeof(GuestHandle) && (a - b) % sizeof(GuestHandle) == 0;
    }

private:
    static constexpr std::uint32_t kSlots = 4096;
    bool Ensure() noexcept {
        if (base_ != nullptr || failed_) {
            return base_ != nullptr;
        }
        void* p = VirtualAlloc(nullptr, kSlots * sizeof(GuestHandle), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p == nullptr || static_cast<std::uint64_t>(A(p)) + kSlots * sizeof(GuestHandle) > guest::kGuestMemoryLimit) {
            FUT_LOGE(Snd, "DSOUND: no memory below 0x80000000 for object handles (%p)", p);
            if (p != nullptr) {
                VirtualFree(p, 0, MEM_RELEASE);
            }
            failed_ = true;
            return false;
        }
        base_ = static_cast<GuestHandle*>(p);
        try {
            free_.reserve(kSlots);
            for (std::uint32_t i = kSlots; i-- > 0;) {
                free_.push_back(i);
            }
        } catch (...) {
            failed_ = true;
            return false;
        }
        return true;
    }
    GuestHandle* base_ = nullptr;
    bool failed_ = false;
    bool exhaustedLogged_ = false;
    std::vector<std::uint32_t> free_;
};

// ---- events from the mixer thread ------------------------------------------------------------------------------------
struct EndEvent {
    std::uint32_t objectId;
    std::uint32_t context;
};
std::mutex g_eventMutex;
std::vector<EndEvent> g_events;

// ---- objects -----------------------------------------------------------------------------------------------------------
using PcmBlock = std::shared_ptr<std::vector<std::int16_t>>;

struct Packet {
    std::uint32_t seq = 0;
    std::uint32_t pdwStatus = 0;
    std::uint32_t pdwCompletedSize = 0;
    std::uint32_t eventOrContext = 0;
    std::uint32_t size = 0;  // dwMaxSize (bytes of guest data consumed)
    PcmBlock pcm;
    bool submitted = false;
    bool played = false;
};

struct Object final : public VoiceEvents {
    Kind kind = Kind::Buffer;
    std::uint32_t id = 0;
    GuestHandle* handle = nullptr;
    SourceFormat src;
    std::unique_ptr<Decoder> decoder;
    std::uint32_t flags = 0;
    bool is3D = false;
    VoiceTag tag;
    std::unique_ptr<SourceVoice> voice;

    // Parameters as set by the guest.
    std::int32_t volume = 0;
    std::uint32_t frequency = 0;  // 0 = original rate
    ds::MixBinList mixbins;
    Emitter3D pending3D;    // latest values (deferred ones included)
    Emitter3D committed3D;  // applied at CommitDeferredSettings / immediate sets
    bool dirty = true;
    // Play / Pause(0) / creation happened; the engine voice starts at the tick's commit together with the levels the
    // game sets after the call (ApplyAllDirty). The host state (playing, pauseState) changes at once.
    bool startPending = false;

    // Applied to the voice (change detection).
    float appliedRatio = -1.0f;
    std::uint32_t appliedChannels = 0;
    float appliedMatrix[kMaxOutputChannels * kMaxOutputChannels] = {};
    float appliedSend = -1.0f;

    // Data no longer submitted but possibly still read by the mixer until its queue drains.
    std::vector<PcmBlock> retired;

    // Buffer state.
    PcmBlock data;
    std::uint32_t frames = 0;
    bool playing = false;
    bool looping = false;
    std::uint32_t cursor = 0;  // remembered play position (frames)
    std::uint64_t startFrames = 0;
    std::uint32_t startCursor = 0;
    std::uint32_t gen = 0;
    std::atomic<std::uint32_t> endedGen{0};

    // Stream state.
    std::uint32_t refs = 1;
    std::uint32_t maxPackets = 1;
    std::uint32_t callback = 0;
    std::uint32_t callbackContext = 0;
    std::uint32_t pauseState = kStreamResume;
    std::deque<Packet> packets;
    std::uint32_t nextSeq = 0;
    bool starving = false;  // queue ran empty while running (underrun episode)

    // Diagnostics of the last Apply (SoundEvent::levelDb / distance; Level3D events).
    float levelDb = 0.0f;
    float distance = -1.0f;
    float loggedLevelDb = 1.0e9f;

    // Event log change tracking.
    std::int32_t loggedVolume = 1;
    std::uint32_t loggedFrequency = 0xFFFFFFFFu;
    Vec3 loggedPosition{1e30f, 1e30f, 1e30f};

    Object() = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    ~Object() override { voice.reset(); }  // synchronous: no OnBufferEnd afterwards

    std::uint32_t Addr() const noexcept { return A(handle); }
    PcmFormat Pcm() const noexcept { return {src.channels, src.sampleRate}; }

    void OnBufferEnd(std::uint32_t context) noexcept override {
        if (kind == Kind::Buffer) {
            endedGen.store(context, std::memory_order_release);
        }
        std::lock_guard<std::mutex> lock(g_eventMutex);
        try {
            g_events.push_back({id, context});
        } catch (...) {
        }
    }
};

struct PendingCallback {
    std::uint32_t fn;
    std::uint32_t streamContext;
    std::uint32_t packetContext;
    std::uint32_t status;
};

struct State {
    std::mutex mutex;
    HandlePool pool;
    GuestHandle* device = nullptr;
    std::uint32_t deviceRefs = 0;
    std::uint32_t streamVtable = 0;
    std::uint32_t nextId = 1;
    std::unordered_map<std::uint32_t, std::unique_ptr<Object>> objects;  // by guest address
    std::unordered_map<std::uint32_t, Object*> byId;
    Listener3D pendingListener;
    Listener3D committedListener;
    I3dl2Listener pendingReverb;
    I3dl2Listener committedReverb;
    bool reverbPending = false;
    bool reverbApplied = false;
    VoiceTag pendingTag;
    std::unique_ptr<Spatializer> spatializer;
    bool spatializerTried = false;
    DsoundStatistics stats;
    bool firstPlayLogged = false;
    bool firstPacketLogged = false;
    std::uint32_t outputChannels = 0;  // engine layout last seen by DoWork
    std::uint32_t outputMask = 0;
};
State g_state;

std::atomic<SoundEventSink*> g_sink{nullptr};

// ---- helpers -----------------------------------------------------------------------------------------------------------
Object* Find(const void* p, Kind kind) noexcept {
    if (p == nullptr) {
        return nullptr;
    }
    const auto it = g_state.objects.find(A(p));
    if (it == g_state.objects.end() || it->second->kind != kind) {
        return nullptr;
    }
    return it->second.get();
}

void WriteGuestDword(std::uint32_t address, std::uint32_t value) noexcept {
    if (address != 0) {
        guest::Write<std::uint32_t>(address, value);
    }
}

// Reports an event to the sink (event list of the capture session, tests). Guest thread, g_state.mutex held.
void Emit(SoundEventKind kind, const Object& o, const char* detail = "") noexcept {
    static_cast<void>(detail);
    SoundEventSink* sink = g_sink.load();
    if (sink == nullptr) {
        return;
    }
    SoundEvent e;
    e.kind = kind;
    e.object = o.Addr();
    e.voiceId = o.tag.voiceId;
    e.category = o.tag.valid ? o.tag.category : Category::Movie;
    e.is3D = o.is3D;
    e.isStream = o.kind == Kind::Stream;
    e.looping = o.looping;
    e.name = o.tag.name;
    e.millibel = o.volume;
    e.frequency = o.frequency;
    e.position = o.pending3D.position;
    e.outputFrame = Engine().OutputFramesRendered();
    e.levelDb = o.levelDb;
    e.distance = o.distance;
    e.minDistance = o.committed3D.minDistance;
    e.maxDistance = o.committed3D.maxDistance;
    e.rolloffPoints = o.committed3D.rolloffCount;
    sink->OnSoundEvent(e);
}

Spatializer* GetSpatializer() noexcept {
    if (!g_state.spatializerTried) {
        g_state.spatializerTried = true;
        g_state.spatializer = CreateSpatializer();
        if (g_state.spatializer == nullptr) {
            FUT_LOGW(Snd, "DSOUND: no spatializer - 3D voices play centred with their volume only");
        }
    }
    return g_state.spatializer.get();
}

// Applies volume, mixbins / 3D state and frequency of `o` to its voice (only what changed).
void Apply(Object& o) noexcept {
    o.dirty = false;
    if (o.voice == nullptr) {
        return;
    }
    AudioEngine& engine = Engine();
    const std::uint32_t outCh = std::min<std::uint32_t>(engine.OutputChannels(), kMaxOutputChannels);
    const std::uint32_t mask = engine.OutputChannelMask();
    // 3D voices: DS3D doppler as extra pitch before the hardware clamp (always 0 here: every velocity is 0, 3.7).
    std::int32_t doppler = 0;
    if (o.is3D) {
        Emitter3D e = o.committed3D;
        doppler = spatial::DopplerPitch(g_state.committedListener, e);
    }
    const float ratio = codec::FrequencyRatio(o.frequency, o.src.sampleRate, doppler);
    if (ratio != o.appliedRatio) {
        o.voice->SetFrequencyRatio(ratio);
        o.appliedRatio = ratio;
    }
    float matrix[kMaxOutputChannels * kMaxOutputChannels] = {};
    float send = 0.0f;
    const std::uint32_t ch = std::min<std::uint32_t>(o.src.channels, kMaxOutputChannels);
    if (o.is3D) {
        Emitter3D e = o.committed3D;
        e.volumeMillibel = o.volume;
        e.channels = ch;
        e.bufferFlags = o.flags;
        const spatial::Levels3D levels = spatial::ComputeLevels3D(g_state.committedListener, e);
        o.levelDb = static_cast<float>(levels.direct) / 100.0f;
        o.distance = spatial::Length(spatial::Sub(e.position, g_state.committedListener.position));
        if (Spatializer* s = GetSpatializer()) {
            SpatialResult r;
            s->Compute(g_state.committedListener, e, outCh, mask, r);
            std::memcpy(matrix, r.matrix, sizeof(float) * ch * outCh);
            send = r.reverbSend;
        } else {
            const float g = MixbinGain(o.volume);
            for (std::uint32_t c = 0; c < ch; ++c) {
                for (std::uint32_t d = 0; d < std::min<std::uint32_t>(outCh, 2); ++d) {
                    matrix[c * outCh + d] = g;
                }
            }
        }
    } else {
        o.levelDb = static_cast<float>(o.volume - kHeadroom2DMillibel) / 100.0f;
        o.distance = -1.0f;
        ds::Matrix2D m;
        ds::Compute2D(o.volume, kHeadroom2DMillibel, o.mixbins, ch, outCh, mask, m);
        std::memcpy(matrix, m.levels, sizeof(float) * ch * outCh);
        send = m.reverbSend;
    }
    const std::size_t n = static_cast<std::size_t>(ch) * outCh;
    if (o.appliedChannels != outCh || std::memcmp(matrix, o.appliedMatrix, n * sizeof(float)) != 0) {
        o.voice->SetOutputMatrix(std::span<const float>(matrix, n));
        std::memcpy(o.appliedMatrix, matrix, n * sizeof(float));
        o.appliedChannels = outCh;
    }
    if (send != o.appliedSend) {
        o.voice->SetReverbSend(send);
        o.appliedSend = send;
    }
    // Event log: level changes of playing 3D voices (>= 3 dB, or into / out of the muted range).
    if (o.is3D && (o.playing || o.kind == Kind::Stream)) {
        const bool muted = o.levelDb <= -100.0f;
        const bool wasMuted = o.loggedLevelDb <= -100.0f;
        if (muted != wasMuted || std::fabs(o.levelDb - o.loggedLevelDb) >= 3.0f) {
            o.loggedLevelDb = o.levelDb;
            Emit(SoundEventKind::Level3D, o);
        }
    }
}

// Applies every changed parameter set and starts the voices whose Play / Pause(0) / creation happened since the last
// commit - in one engine batch, so a voice starts with the levels of its tick. The game sets volume, frequency, mixbins
// and the I3DL2 send AFTER Play / Pause(0) in the same update (SoundVoice::vf11: Pause(0) 0xCC711, Play 0xCC72E,
// then SetFrequency 0xCC8BA/0xCC910, SetVolume 0xCC8C4/0xCC91D, SetMixBins 0xCC9C0/0xCC9D3, SetI3DL2Source 0xCCA0E)
// and commits at 0xCBE95 (CommitDeferredSettings) right before DoWork 0xCBE9A. The APU plays the first samples with
// the old values for well under a millisecond; starting at once here would play up to one 10 ms mixer quantum (or
// more) with the defaults (0 mB, original pitch, centre: measured +15 dB on a menu sound with the effects slider at
// 0.25), so the start waits for the commit, a few microseconds later in the same SoundManager::Update.
void ApplyAllDirty() noexcept {
    bool any = false;
    for (auto& [address, o] : g_state.objects) {
        if (o->voice == nullptr) {
            o->startPending = false;
            continue;
        }
        const bool start = o->startPending;
        if (!o->dirty && !start) {
            continue;
        }
        if (!any) {
            Engine().BeginBatch();
            any = true;
        }
        if (start && o->is3D) {
            o->committed3D = o->pending3D;  // a 3D voice starts with its newest position and distances (also DoWork)
            o->dirty = true;
        }
        if (o->dirty) {
            Apply(*o);
        }
        if (start) {
            o->startPending = false;
            if (o->kind == Kind::Buffer) {
                o->startFrames = o->voice->FramesPlayed();  // not started yet: stable
            }
            o->voice->Start();
        }
    }
    if (any) {
        Engine().CommitBatch();
    }
}

void MarkAll3DDirty() noexcept {
    for (auto& [address, o] : g_state.objects) {
        if (o->is3D) {
            o->dirty = true;
        }
    }
}

// Reads a guest WAVEFORMATEX; false (counted, logged once per reason) for unsupported formats.
bool ReadFormat(std::uint32_t wfx, SourceFormat& out) noexcept {
    const char* error = nullptr;
    if (wfx == 0 || !guest::IsReadable(wfx, sizeof(XWaveFormatEx)) ||
        !codec::ParseWaveFormat(guest::Ptr<const void>(wfx), out, &error)) {
        if (g_state.stats.formatErrors++ < 8) {
            FUT_LOGW(Snd, "DSOUND: unsupported wave format at 0x%08X: %s", wfx, error != nullptr ? error : "unreadable");
        }
        return false;
    }
    return true;
}

Object* NewObject(Kind kind, const SourceFormat& format, std::uint32_t flags, std::uint32_t vtable) noexcept {
    std::unique_ptr<Object> o;
    try {
        o = std::make_unique<Object>();
    } catch (...) {
        return nullptr;
    }
    o->kind = kind;
    o->id = g_state.nextId++;
    o->src = format;
    o->flags = flags;
    o->is3D = (flags & kDsFlag3D) != 0;
    o->decoder = CreateDecoder(format);
    o->mixbins = ds::DefaultMixBins(format.channels);
    if (g_state.pendingTag.valid) {
        o->tag = g_state.pendingTag;
        g_state.pendingTag.valid = false;  // exactly one object per CreateDsObject call
        ++g_state.stats.tagged;
    }
    o->handle = g_state.pool.Alloc(kind, o->id, vtable);
    if (o->handle == nullptr || o->decoder == nullptr) {
        g_state.pool.Free(o->handle);
        return nullptr;
    }
    // Engine voice (PCM16 after decoding). A voice that cannot be created leaves a silent object (counted).
    VoiceDesc vd;
    vd.format = o->Pcm();
    vd.category = o->tag.valid ? o->tag.category : Category::Movie;
    vd.reverbSend = o->is3D;
    vd.maxFrequencyRatio = std::max(4.0f, codec::MaxFrequencyRatio(format.sampleRate));
    vd.events = o.get();
    vd.debugName = o->tag.name;
    o->voice = Engine().CreateVoice(vd);
    if (o->voice == nullptr) {
        if (g_state.stats.voiceErrors++ < 8) {
            FUT_LOGW(Snd, "DSOUND: no engine voice for %u ch %u Hz (object stays silent)", format.channels,
                     format.sampleRate);
        }
    }
    Object* raw = o.get();
    try {
        g_state.byId[raw->id] = raw;
        g_state.objects[raw->Addr()] = std::move(o);
    } catch (...) {
        g_state.byId.erase(raw->id);
        g_state.pool.Free(raw->handle);
        return nullptr;
    }
    return raw;
}

void DeleteObject(Object* o) noexcept {
    if (o == nullptr) {
        return;
    }
    const std::uint32_t address = o->Addr();
    GuestHandle* handle = o->handle;
    o->voice.reset();  // synchronous; no more events for this id
    g_state.byId.erase(o->id);
    g_state.objects.erase(address);  // destroys o
    g_state.pool.Free(handle);
}

// ---- buffers ------------------------------------------------------------------------------------------------------------
// Current play position (frames) of a playing buffer from the voice's frame counter.
std::uint32_t BufferPosition(Object& b) noexcept {
    if (!b.playing || b.startPending || b.voice == nullptr || b.frames == 0) {
        return b.cursor;  // a pending start has not played anything yet
    }
    const std::uint64_t played = b.voice->FramesPlayed() - b.startFrames;
    std::uint64_t pos = b.startCursor + played;
    if (b.looping) {
        pos %= b.frames;
    } else if (pos >= b.frames) {
        pos = 0;
    }
    return static_cast<std::uint32_t>(pos);
}

// True while the buffer plays; detects the end of a one-shot (the voice's OnBufferEnd for the current submission).
bool BufferPlaying(Object& b) noexcept {
    if (b.playing && b.endedGen.load(std::memory_order_acquire) == b.gen) {
        b.playing = false;
        b.cursor = 0;
        Emit(SoundEventKind::Ended, b);
    }
    return b.playing;
}

void BufferHalt(Object& b, bool keepCursor) noexcept {
    if (!b.playing) {
        if (!keepCursor) {
            b.cursor = 0;
        }
        return;
    }
    const bool ended = b.endedGen.load(std::memory_order_acquire) == b.gen;
    b.cursor = keepCursor && !ended ? BufferPosition(b) : 0;
    b.playing = false;
    b.startPending = false;
    if (b.voice != nullptr) {
        b.voice->Stop();
        b.voice->Flush();
    }
}

void BufferStart(Object& b) noexcept {
    if (b.voice == nullptr || b.frames == 0 || b.data == nullptr) {
        b.playing = false;
        return;
    }
    if (b.cursor >= b.frames) {
        b.cursor = 0;
    }
    ++b.gen;
    if (b.gen == 0) {
        ++b.gen;
    }
    SubmitDesc s;
    s.data = b.data->data();
    s.frames = b.frames;
    s.playBegin = b.cursor;
    s.loop = b.looping;
    s.context = b.gen;
    if (!b.voice->Submit(s)) {
        b.playing = false;
        return;
    }
    b.startCursor = b.cursor;
    b.startPending = true;  // the voice starts at the tick's commit (ApplyAllDirty; startFrames there)
    b.playing = true;
}

// ---- streams -------------------------------------------------------------------------------------------------------------
std::uint32_t StreamStatus(const Object& s) noexcept {
    std::uint32_t status = 0;
    if (s.packets.size() < s.maxPackets) {
        status |= kStreamStatusReady;
    }
    if (s.pauseState != kStreamResume) {
        status |= kStreamStatusPaused;
    } else if (!s.packets.empty()) {
        status |= kStreamStatusPlaying;
    }
    return status;
}

// Ends packet `p` towards the guest (guest thread): completed size, status, then event or callback.
void FinishPacket(const Object& s, const Packet& p, std::uint32_t status, std::vector<PendingCallback>& callbacks) {
    WriteGuestDword(p.pdwCompletedSize, status == kPacketSuccess ? p.size : 0);
    WriteGuestDword(p.pdwStatus, status);
    if (s.callback != 0) {
        callbacks.push_back({s.callback, s.callbackContext, p.eventOrContext, status});
    } else if (p.eventOrContext != 0) {
        krnl::Ref<krnl::EventObject> ev =
            krnl::LookupEvent(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(p.eventOrContext)));
        if (ev) {
            ev->Set();
        }
    }
}

// Aborts every queued packet (Flush/FlushEx; E_ABORT, m5_audio.md 2.4). The PCM stays alive until the voice drained.
void FlushPackets(Object& s, std::vector<PendingCallback>& callbacks, bool writeGuest) noexcept {
    if (s.voice != nullptr) {
        s.voice->Stop();
        s.voice->Flush();
    }
    while (!s.packets.empty()) {
        Packet p = std::move(s.packets.front());
        s.packets.pop_front();
        try {
            if (p.pcm != nullptr) {
                s.retired.push_back(std::move(p.pcm));
            }
            if (writeGuest) {
                FinishPacket(s, p, kPacketFlushed, callbacks);
            }
        } catch (...) {
        }
        ++g_state.stats.packetsFlushed;
    }
    if (s.voice != nullptr && s.pauseState == kStreamResume && !s.startPending) {
        s.voice->Start();
    }
    s.starving = true;  // emptied on purpose: no underrun report until new packets arrive
}

void RunCallbacks(const std::vector<PendingCallback>& callbacks) noexcept {
    // Guest thread, g_state.mutex released (the callback may call DSOUND again). XMV's callback 0x264775:
    // stdcall(pStreamContext, pPacketContext, dwStatus), ret 0xC (m5_audio.md 5.2).
    for (const PendingCallback& c : callbacks) {
        core::CallGuest(c.fn, {c.streamContext, c.packetContext, c.status});
    }
}

// ---- stream vtable -------------------------------------------------------------------------------------------------------
#define SND_CHECK_STREAM_METHOD(Method, fn)                                                                  \
    static_assert(::xdk::detail::StdcallFnOf<decltype(fn)>::valid &&                                         \
                      ::xdk::detail::StdcallFnOf<decltype(fn)>::purgeBytes ==                                \
                          ::xdk::stream::Method::purgeBytes,                                                 \
                  "stream method " #Method ": convention/purge differs from vtable 0x2A5F5C")
SND_CHECK_STREAM_METHOD(AddRef, &DsStreamAddRef);
SND_CHECK_STREAM_METHOD(Release, &DsStreamRelease);
SND_CHECK_STREAM_METHOD(GetInfo, &DsStreamGetInfo);
SND_CHECK_STREAM_METHOD(GetStatus, &DsStreamGetStatus);
SND_CHECK_STREAM_METHOD(Process, &DsStreamProcess);
SND_CHECK_STREAM_METHOD(Discontinuity, &DsStreamDiscontinuity);
SND_CHECK_STREAM_METHOD(Flush, &DsStreamFlush);
#undef SND_CHECK_STREAM_METHOD

// Guest vtable of our streams: the 7 XMediaObject methods (slot order of guest::kDsoundStreamVtable), the remaining
// slots of the 64-byte block trap.
std::uint32_t StreamVtable() noexcept {
    if (g_state.streamVtable != 0) {
        return g_state.streamVtable;
    }
    auto* table = static_cast<std::uint32_t*>(xdk::AllocGuestObject(xdk::kGuestObjectSize));
    if (table == nullptr) {
        return 0;
    }
    const void* const* trap = xdk::TrapVtable();
    for (std::uint32_t i = 0; i < xdk::kGuestObjectSize / 4; ++i) {
        table[i] = A(trap[i % xdk::kTrapVtableSlots]);
    }
    table[xdk::stream::AddRef::slot] = A(core::detail::CodeAddress(&DsStreamAddRef));
    table[xdk::stream::Release::slot] = A(core::detail::CodeAddress(&DsStreamRelease));
    table[xdk::stream::GetInfo::slot] = A(core::detail::CodeAddress(&DsStreamGetInfo));
    table[xdk::stream::GetStatus::slot] = A(core::detail::CodeAddress(&DsStreamGetStatus));
    table[xdk::stream::Process::slot] = A(core::detail::CodeAddress(&DsStreamProcess));
    table[xdk::stream::Discontinuity::slot] = A(core::detail::CodeAddress(&DsStreamDiscontinuity));
    table[xdk::stream::Flush::slot] = A(core::detail::CodeAddress(&DsStreamFlush));
    g_state.streamVtable = A(table);
    return g_state.streamVtable;
}

// ---- common parameter setters (buffer or stream object, g_state.mutex held) ---------------------------------------------
void SetVolumeOn(Object* o, std::int32_t volume) noexcept {
    if (o == nullptr) {
        return;
    }
    volume = std::clamp(volume, kMillibelMin, 0);
    if (o->volume != volume) {
        o->volume = volume;
        o->dirty = true;
    }
    if (o->loggedVolume != volume) {
        o->loggedVolume = volume;
        Emit(SoundEventKind::Volume, *o);
    }
}

void SetFrequencyOn(Object* o, std::uint32_t frequency) noexcept {
    if (o == nullptr) {
        return;
    }
    if (o->frequency != frequency) {
        o->frequency = frequency;
        o->dirty = true;
    }
    if (o->loggedFrequency != frequency) {
        o->loggedFrequency = frequency;
        Emit(SoundEventKind::Frequency, *o);
    }
}

void SetMixBinsOn(Object* o, const void* mixBins) noexcept {
    if (o == nullptr || mixBins == nullptr || !guest::IsReadable(A(mixBins), sizeof(XDsMixBins))) {
        return;
    }
    const auto* mb = static_cast<const XDsMixBins*>(mixBins);
    const std::uint32_t count = std::min<std::uint32_t>(mb->dwMixBinCount, ds::kMaxMixBins);
    if (count != 0 && !guest::IsReadable(mb->lpMixBinVolumePairs, count * sizeof(XDsMixBinVolumePair))) {
        return;
    }
    ds::MixBinList l;
    l.count = count;
    const auto* pairs = guest::Ptr<const XDsMixBinVolumePair>(mb->lpMixBinVolumePairs);
    for (std::uint32_t i = 0; i < count; ++i) {
        l.entries[i] = {pairs[i].dwMixBin, std::clamp(pairs[i].lVolume, kMillibelMin, 0)};
    }
    if (l.count != o->mixbins.count ||
        std::memcmp(l.entries, o->mixbins.entries, sizeof(ds::MixBinVolume) * l.count) != 0) {
        o->mixbins = l;
        if (!o->is3D) {
            o->dirty = true;  // 3D voices: the spatializer replaces the bins (6..10, 3)
        }
    }
}

// 3D setters: `apply` bit 0 = DS3D_DEFERRED (CDirectSoundVoice_SetI3DL2Source 0x247DB5 only tests bit 0).
template <class Fn>
void Set3D(Object* o, std::uint32_t apply, Fn&& fn) noexcept {
    if (o == nullptr) {
        return;
    }
    fn(o->pending3D);
    if ((apply & 1) == 0) {
        fn(o->committed3D);
        if (o->is3D) {
            o->dirty = true;
        }
    }
}

void NotePosition(Object* o) noexcept {
    if (o == nullptr) {
        return;
    }
    const Vec3 d{o->pending3D.position.x - o->loggedPosition.x, o->pending3D.position.y - o->loggedPosition.y,
                 o->pending3D.position.z - o->loggedPosition.z};
    if (d.x * d.x + d.y * d.y + d.z * d.z >= 1.0f) {
        o->loggedPosition = o->pending3D.position;
        Emit(SoundEventKind::Position3D, *o);
    }
}

// ---- listener --------------------------------------------------------------------------------------------------------------
template <class Fn>
void SetListener(std::uint32_t apply, Fn&& fn) noexcept {
    fn(g_state.pendingListener);
    if ((apply & 1) == 0) {
        fn(g_state.committedListener);
        MarkAll3DDirty();
    }
}

I3dl2Listener ToListener(const XDsI3dl2Listener& x) noexcept {
    I3dl2Listener l;
    l.room = x.lRoom;
    l.roomHF = x.lRoomHF;
    l.roomRolloffFactor = x.flRoomRolloffFactor;
    l.decayTime = x.flDecayTime;
    l.decayHFRatio = x.flDecayHFRatio;
    l.reflections = x.lReflections;
    l.reflectionsDelay = x.flReflectionsDelay;
    l.reverb = x.lReverb;
    l.reverbDelay = x.flReverbDelay;
    l.diffusion = x.flDiffusion;
    l.density = x.flDensity;
    l.hfReference = x.flHFReference;
    return l;
}

void ApplyReverbIfPending() noexcept {
    if (!g_state.reverbPending) {
        return;
    }
    g_state.reverbPending = false;
    g_state.committedReverb = g_state.pendingReverb;
    Engine().SetReverb(g_state.committedReverb);
    if (!g_state.reverbApplied) {
        g_state.reverbApplied = true;
        FUT_LOGI(Snd, "DSOUND: I3DL2 listener room %d, decay %.2f s, reflections %d, reverb %d (reverb submix)",
                 g_state.committedReverb.room, static_cast<double>(g_state.committedReverb.decayTime),
                 g_state.committedReverb.reflections, g_state.committedReverb.reverb);
    }
}

// ---- DoWork --------------------------------------------------------------------------------------------------------------
std::uint32_t DoWorkLocked(std::vector<PendingCallback>& callbacks) noexcept {
    if (g_state.device == nullptr && g_state.objects.empty()) {
        return 0;  // DSOUND not used yet: do not create the engine (e.g. the purge self-test with zero arguments)
    }
    AudioEngine& engine = Engine();
    engine.Pump();
    // After a backend switch (device lost / back) the output layout may differ: rebuild every matrix (the engine
    // itself carries the reverb preset and master volume over).
    if (engine.OutputChannels() != g_state.outputChannels || engine.OutputChannelMask() != g_state.outputMask) {
        g_state.outputChannels = engine.OutputChannels();
        g_state.outputMask = engine.OutputChannelMask();
        for (auto& [address, o] : g_state.objects) {
            o->appliedChannels = 0;
            o->appliedSend = -1.0f;
            o->dirty = true;
        }
    }
    ApplyReverbIfPending();
    ApplyAllDirty();
    std::vector<EndEvent> events;
    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        events.swap(g_events);
    }
    std::uint32_t completed = 0;
    std::vector<Object*> touched;
    for (const EndEvent& e : events) {
        const auto it = g_state.byId.find(e.objectId);
        if (it == g_state.byId.end()) {
            continue;  // released meanwhile
        }
        Object& o = *it->second;
        if (o.kind == Kind::Buffer) {
            BufferPlaying(o);  // reports the end at the tick it happened
            continue;
        }
        for (Packet& p : o.packets) {
            if (p.seq == e.context) {
                p.played = true;
                try {
                    touched.push_back(&o);
                } catch (...) {
                }
                break;
            }
        }
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (Object* s : touched) {
        while (!s->packets.empty() && s->packets.front().played) {
            const Packet p = std::move(s->packets.front());
            s->packets.pop_front();
            try {
                FinishPacket(*s, p, kPacketSuccess, callbacks);
            } catch (...) {
            }
            ++completed;
            ++g_state.stats.packetsCompleted;
        }
    }
    // Underruns: a running stream whose queue ran empty after it had packets (the game's pump refills every tick, so
    // this means the mixer played everything that was submitted - a dropout).
    for (auto& [address, o] : g_state.objects) {
        if (o->kind != Kind::Stream) {
            continue;
        }
        const bool empty = o->packets.empty();
        if (empty && o->nextSeq != 0 && o->pauseState == kStreamResume && !o->starving) {
            o->starving = true;
            if (g_state.stats.starvations++ < 4) {
                FUT_LOGW(Snd, "DSOUND: stream 0x%08X (\"%s\") ran out of packets (dropout)", o->Addr(), o->tag.name);
            }
            Emit(SoundEventKind::PacketsLate, *o);
        } else if (!empty) {
            o->starving = false;
        }
    }
    // Release retired data once the voice no longer reads it.
    for (auto& [address, o] : g_state.objects) {
        if (!o->retired.empty() && (o->voice == nullptr || o->voice->BuffersQueued() == 0)) {
            o->retired.clear();
        }
    }
    return completed;
}

}  // namespace

// ==================================================================================================================
// XMediaObject methods of a stream (vtable slots, `this` on the stack). Slot order and purges: guest vtable 0x2A5F5C
// (generated xdk::stream::*, verified against the XBE). Callers: Release - voice destructor 0xCC107 and the movie
// player 0xEE47D; Process - stream pump 0xCB6B7; AddRef/GetStatus/Discontinuity/GetInfo/Flush - XMV only.
// ==================================================================================================================

// XMediaObject::AddRef - slot 0, stdcall(this), ULONG, ret 4 (XMV GetAudioStream returns an AddRef'd pointer).
std::uint32_t __stdcall DsStreamAddRef(void* self) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    Object* s = Find(self, Kind::Stream);
    return s != nullptr ? ++s->refs : 0;
}

// XMediaObject::Release - slot 1, stdcall(this), ULONG, ret 4. The last reference destroys the voice synchronously
// (no mixer event afterwards), writes E_ABORT into the status of every packet still queued while the owner is alive
// (the voice destructor 0xCC030 frees the packet memory and status DWORDs right after this call, 0xCC107 -> 0xCC121)
// and frees the object - nothing is written later (trap 2, m5_audio.md 8).
std::uint32_t __stdcall DsStreamRelease(void* self) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    Object* s = Find(self, Kind::Stream);
    if (s == nullptr) {
        return 0;
    }
    if (--s->refs != 0) {
        return s->refs;
    }
    s->voice.reset();
    std::vector<PendingCallback> unused;
    for (const Packet& p : s->packets) {
        WriteGuestDword(p.pdwCompletedSize, 0);
        WriteGuestDword(p.pdwStatus, kPacketFlushed);
        ++g_state.stats.packetsFlushed;
    }
    s->packets.clear();
    Emit(SoundEventKind::Release, *s);
    --g_state.stats.streamsLive;
    DeleteObject(s);
    return 0;
}

// XMediaObject::GetInfo - slot 2, stdcall(this, XMEDIAINFO*), HRESULT, ret 8. Not called by the game.
// ASSUMPTION: dwInputSize = one block of the format (XDK: minimum input packet size), fixed sample size.
std::int32_t __stdcall DsStreamGetInfo(void* self, XMediaInfo* info) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    const Object* s = Find(self, Kind::Stream);
    if (info != nullptr) {
        std::memset(info, 0, sizeof(*info));
        info->dwFlags = kXmoFlagFixedSampleSize;
        info->dwInputSize = s != nullptr ? codec::BlockBytes(s->src) : 0;
    }
    return kS_OK;
}

// XMediaObject::GetStatus - slot 3, stdcall(this, DWORD*), HRESULT, ret 8. READY while the queue has room, PLAYING
// while running with queued packets (XMV CloseDecoder 0x264677 spins until it drops), PAUSED after Pause(1/2).
std::int32_t __stdcall DsStreamGetStatus(void* self, std::uint32_t* status) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    const Object* s = Find(self, Kind::Stream);
    if (status != nullptr) {
        *status = s != nullptr ? StreamStatus(*s) : 0;
    }
    return kS_OK;
}

// XMediaObject::Process - slot 4, stdcall(this, const XMEDIAPACKET* in, const XMEDIAPACKET* out), HRESULT, ret 12.
// Game: pump 0xCB5B0 submits Process(&pkt, NULL) (0xCB6AB..0xCB6B7) for every slot whose status is not E_PENDING,
// with dwMaxSize = the full packet size P (zero-padded at the end of the file, 3.5). A full queue returns
// DSERR_INVALIDCALL without touching the status (0x247676). Otherwise the packet is copied and decoded at once (the
// game reuses its memory after completion; ADPCM per packet), submitted, and *pdwStatus = E_PENDING is set
// synchronously (0x24B1D9) - the pump would otherwise refill the slot at once.
std::int32_t __stdcall DsStreamProcess(void* self, const XMediaPacket* in, const XMediaPacket* out) noexcept {
    static_cast<void>(out);  // render stream: no output packet (the game passes NULL)
    std::lock_guard<std::mutex> lock(g_state.mutex);
    Object* s = Find(self, Kind::Stream);
    if (s == nullptr || in == nullptr) {
        return kE_InvalidArg;
    }
    if (s->packets.size() >= s->maxPackets) {
        ++g_state.stats.packetsRejected;
        return kDsErrInvalidCall;
    }
    Packet p;
    p.seq = ++s->nextSeq;
    if (p.seq == 0) {
        p.seq = ++s->nextSeq;
    }
    p.pdwStatus = in->pdwStatus;
    p.pdwCompletedSize = in->pdwCompletedSize;
    p.eventOrContext = in->hCompletionEvent;
    p.size = in->dwMaxSize;
    try {
        if (in->dwMaxSize != 0 && in->pvBuffer != 0 && guest::IsReadable(in->pvBuffer, in->dwMaxSize) &&
            s->decoder != nullptr) {
            const std::size_t frames = s->decoder->FramesFor(in->dwMaxSize);
            if (frames != 0) {
                p.pcm = std::make_shared<std::vector<std::int16_t>>(frames * s->src.channels);
                const std::size_t got = s->decoder->Decode(
                    std::span<const std::uint8_t>(guest::Ptr<const std::uint8_t>(in->pvBuffer), in->dwMaxSize),
                    std::span<std::int16_t>(*p.pcm));
                p.pcm->resize(got * s->src.channels);
            }
        }
        if (p.pcm != nullptr && !p.pcm->empty() && s->voice != nullptr) {
            SubmitDesc d;
            d.data = p.pcm->data();
            d.frames = static_cast<std::uint32_t>(p.pcm->size() / s->src.channels);
            d.context = p.seq;
            p.submitted = s->voice->Submit(d);
        }
        if (!p.submitted) {
            p.played = true;  // nothing to play (empty packet / no voice): completes at the next DoWork
            std::lock_guard<std::mutex> evLock(g_eventMutex);
            g_events.push_back({s->id, p.seq});
        }
        s->packets.push_back(std::move(p));
    } catch (...) {
        return kE_OutOfMemory;  // status untouched
    }
    const Packet& q = s->packets.back();
    WriteGuestDword(q.pdwCompletedSize, 0);
    WriteGuestDword(q.pdwStatus, kPacketPending);
    ++g_state.stats.packetsSubmitted;
    if (!g_state.firstPacketLogged) {
        g_state.firstPacketLogged = true;
        FUT_LOGI(Snd, "DSOUND: first stream packet: %u bytes -> %u frames (%s, %u ch %u Hz), status at 0x%08X",
                 q.size, q.pcm != nullptr ? static_cast<std::uint32_t>(q.pcm->size() / s->src.channels) : 0u,
                 s->src.formatTag == kWaveFormatXboxAdpcm ? "Xbox ADPCM" : "PCM", s->src.channels, s->src.sampleRate,
                 q.pdwStatus);
    }
    return kS_OK;
}

// XMediaObject::Discontinuity - slot 5, stdcall(this), HRESULT, ret 4. XMV at the end of the data: no packet follows
// the queued ones (IXAudio2SourceVoice::Discontinuity, m5_audio.md 6.3).
std::int32_t __stdcall DsStreamDiscontinuity(void* self) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    Object* s = Find(self, Kind::Stream);
    if (s != nullptr && s->voice != nullptr) {
        s->voice->Discontinuity();
    }
    if (s != nullptr) {
        s->starving = true;  // end of the data: running empty afterwards is expected
    }
    return kS_OK;
}

// XMediaObject::Flush - slot 6, stdcall(this), HRESULT, ret 4. Not called by the game. Aborts every queued packet
// (E_ABORT, callbacks with that status) synchronously.
std::int32_t __stdcall DsStreamFlush(void* self) noexcept {
    std::vector<PendingCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (Object* s = Find(self, Kind::Stream)) {
            FlushPackets(*s, callbacks, true);
            Emit(SoundEventKind::Flush, *s);
        }
    }
    RunCallbacks(callbacks);
    return kS_OK;
}

// =================================================================================================================
// The 40 entry points (all __stdcall, `ret N` = generated purge, static_assert in XDK_HOOK). Callers and sites:
// avi.md 2.2, m5_audio.md 2.2; "result ignored" = the instruction after the call does not read eax.
// =================================================================================================================

// ---- creation ---------------------------------------------------------------------------------------------------------

// DSOUND::DirectSoundCreate 0x24935C - stdcall (LPCGUID, LPDIRECTSOUND* ppDS, LPUNKNOWN), HRESULT, ret 12.
// Game: SoundManager ctor 0xCCD41 DirectSoundCreate(0, &mgr+0x48, 0); `test eax,eax; jl 0xCCE0A` skips the
// dsstdfx.bin load and the 3D setup on failure, so S_OK keeps the original init path. The handle must be non-NULL
// (SoundManager::Update 0xCBCB0 returns at once for +0x48 == 0) and only reaches C functions, so its vtable traps.
// Singleton with a reference count like the original (FUN_002492EF / DAT_002641A0); XMV 0x264962 is the other
// caller (creates and releases it around SynchPlayback). The first call creates the audio engine (Engine()).
std::int32_t __stdcall DirectSoundCreateHle(const void* deviceGuid, void** ppDirectSound,
                                            void* unkOuter) noexcept {
    if (xdk::detail::InPurgeProbe()) {
        return kS_OK;  // self-test XdkEntryPurgeMeasured: no device, no reference, no engine
    }
    std::int32_t hr = kS_OK;
    void* device = nullptr;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.device == nullptr) {
            g_state.device = g_state.pool.Alloc(Kind::Device, 0, A(xdk::TrapVtable()));
            first = g_state.device != nullptr;
        }
        if (g_state.device != nullptr) {
            ++g_state.deviceRefs;
            device = g_state.device;
        } else {
            hr = kE_OutOfMemory;  // game: jl -> no 3D init
        }
    }
    if (ppDirectSound != nullptr) {
        *ppDirectSound = device;
    }
    if (first) {
        AudioEngine& engine = Engine();
        FUT_LOGI(Snd, "DSOUND: HLE device 0x%08X on the %s engine (%u ch, %u Hz)", A(device), engine.BackendName(),
                 engine.OutputChannels(), engine.OutputSampleRate());
    }
    XDK_TRACE_CALL(DirectSoundCreate, "(guid=0x%08X, ppDS=0x%08X, outer=0x%08X) -> 0x%08X, *ppDS=0x%08X",
                   A(deviceGuid), A(ppDirectSound), A(unkOuter), static_cast<std::uint32_t>(hr), A(device));
    return hr;
}
XDK_HOOK(DirectSoundCreate, &DirectSoundCreateHle);

// DSOUND::DirectSoundCreateBuffer 0x2493A3 - stdcall (LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*), HRESULT, ret 8.
// Only caller: SoundVoice::CreateDsObject 0xCC150 (0xCC296), result ignored, *pp stored at voice+0xA4.
// DSBUFFERDESC{0x18, flags (3D ? 0x60010 : 0) | 0x100, 0, wfx, NULL, 0}; wfx = SoundData+0x59 (ADPCM) or +0x34.
// Creates the object and its engine voice (PCM16 after decoding; MaxFrequencyRatio >= 4, pitch up to 4 at 0xCA110).
// An unsupported format leaves *pp = NULL, which every use of voice+0xA4 tolerates (M2 stub evidence).
std::int32_t __stdcall DirectSoundCreateBufferHle(const XDsBufferDesc* desc, void** ppBuffer) noexcept {
    std::int32_t hr = kS_OK;
    void* buffer = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SourceFormat f;
        if (desc == nullptr || !ReadFormat(desc->lpwfxFormat, f)) {
            hr = kE_InvalidArg;
        } else if (Object* o = NewObject(Kind::Buffer, f, desc->dwFlags, A(xdk::TrapVtable()))) {
            buffer = o->handle;
            if (desc->lpMixBins != 0) {
                SetMixBinsOn(o, guest::Ptr<const void>(desc->lpMixBins));
            }
            DsoundStatistics& st = g_state.stats;
            ++st.buffersCreated;
            ++st.buffersLive;
            st.buffersPeak = std::max(st.buffersPeak, st.buffersLive);
            Emit(SoundEventKind::Create, *o);
        } else {
            hr = kE_OutOfMemory;
        }
    }
    if (ppBuffer != nullptr) {
        *ppBuffer = buffer;
    }
    XDK_TRACE_CALL(DirectSoundCreateBuffer, "(desc=0x%08X flags=0x%08X fmt=0x%08X) -> 0x%08X, *pp=0x%08X", A(desc),
                   desc != nullptr ? desc->dwFlags : 0u, desc != nullptr ? desc->lpwfxFormat : 0u,
                   static_cast<std::uint32_t>(hr), A(buffer));
    return hr;
}
XDK_HOOK(DirectSoundCreateBuffer, &DirectSoundCreateBufferHle);

// DSOUND::DirectSoundCreateStream 0x2493FA - stdcall (LPCDSSTREAMDESC, LPDIRECTSOUNDSTREAM*), HRESULT, ret 8.
// Game: CreateDsObject 0xCC1C3 (result ignored), *pp stored at voice+0x74; DSSTREAMDESC{flags 0 / 0x60010, 10
// packets, wfx = SoundData+0x34, NULL callback}. XMV EnableAudioStream 0x2647C6: 2 packets + callback 0x264775 and
// its decoder as context (m5_audio.md 5.1). The stream is created running (the game still calls Pause(0) at its
// first update; XMV calls Pause(2) before the first Process); its engine voice starts at the next commit (DoWork or
// CommitDeferredSettings, ApplyAllDirty) with the levels set up to then.
std::int32_t __stdcall DirectSoundCreateStreamHle(const XDsStreamDesc* desc, void** ppStream) noexcept {
    std::int32_t hr = kS_OK;
    void* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SourceFormat f;
        const std::uint32_t vtable = StreamVtable();
        if (desc == nullptr || !ReadFormat(desc->lpwfxFormat, f)) {
            hr = kE_InvalidArg;
        } else if (Object* o = vtable != 0 ? NewObject(Kind::Stream, f, desc->dwFlags, vtable) : nullptr) {
            o->maxPackets = std::max<std::uint32_t>(1, desc->dwMaxAttachedPackets);
            o->callback = desc->lpfnCallback;
            o->callbackContext = desc->lpvContext;
            o->looping = true;  // the game's streams never end by themselves (the pump loops, 3.5)
            if (desc->lpMixBins != 0) {
                SetMixBinsOn(o, guest::Ptr<const void>(desc->lpMixBins));
            }
            o->startPending = o->voice != nullptr;  // running from the next commit on (ApplyAllDirty)
            stream = o->handle;
            ++g_state.stats.streamsCreated;
            ++g_state.stats.streamsLive;
            Emit(SoundEventKind::Create, *o);
        } else {
            hr = kE_OutOfMemory;
        }
    }
    if (ppStream != nullptr) {
        *ppStream = stream;
    }
    XDK_TRACE_CALL(DirectSoundCreateStream,
                   "(desc=0x%08X flags=0x%08X packets=%u fmt=0x%08X callback=0x%08X) -> 0x%08X, *pp=0x%08X", A(desc),
                   desc != nullptr ? desc->dwFlags : 0u, desc != nullptr ? desc->dwMaxAttachedPackets : 0u,
                   desc != nullptr ? desc->lpwfxFormat : 0u, desc != nullptr ? desc->lpfnCallback : 0u,
                   static_cast<std::uint32_t>(hr), A(stream));
    return hr;
}
XDK_HOOK(DirectSoundCreateStream, &DirectSoundCreateStreamHle);

// ---- IDirectSound (device handle from DirectSoundCreate) --------------------------------------------------------------

// DSOUND::IDirectSound_Release 0x246E7D - stdcall (LPDIRECTSOUND), ULONG, ret 4. Game: SoundManager dtor 0xCBC7F
// (result ignored); XMV per sync. Reference count only; the device is never torn down (m5_audio.md 2.2).
std::uint32_t __stdcall IDirectSound_ReleaseHle(void* ds) noexcept {
    std::uint32_t refs = 0;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (ds != nullptr && ds == g_state.device && g_state.deviceRefs > 0) {
            refs = --g_state.deviceRefs;
        }
    }
    XDK_TRACE_CALL(IDirectSound_Release, "(ds=0x%08X) -> %u", A(ds), refs);
    return refs;
}
XDK_HOOK(IDirectSound_Release, &IDirectSound_ReleaseHle);

// DSOUND::IDirectSound_DownloadEffectsImage 0x247911 - stdcall (ds, LPCVOID pvImage, DWORD dwSize,
// LPCDSEFFECTIMAGELOC, LPDSEFFECTIMAGEDESC* ppDesc), HRESULT, ret 20. Only caller FUN_000CBEB0 0xCBF67 with the
// 25256 bytes of XBOX\dsstdfx.bin, loc {0 (I3DL2), 1 (crosstalk)}; result and *ppDesc are never read, the image is
// freed right after (avi.md 2.1). The DSP image is not emulated: the reverb is the XAudio2 reverb submix, the
// crosstalk/HRTF part is replaced by the Spatializer's panning (m5_audio.md 6.5). S_OK, *ppDesc = NULL.
std::int32_t __stdcall IDirectSound_DownloadEffectsImageHle(void* ds, const void* image, std::uint32_t size,
                                                            const void* imageLoc,
                                                            std::uint32_t* ppImageDesc) noexcept {
    if (ppImageDesc != nullptr) {
        *ppImageDesc = 0;
    }
    if (!xdk::detail::InPurgeProbe() && image != nullptr && size != 0 && guest::IsReadable(A(image), size)) {
        reverb::EffectsImageInfo info;
        const char* error = nullptr;
        reverb::EffectsImageLoc loc;
        if (imageLoc != nullptr && guest::IsReadable(A(imageLoc), sizeof(loc))) {
            std::memcpy(&loc, imageLoc, sizeof(loc));
        }
        if (reverb::ParseEffectsImage(image, size, info, &error)) {
            FUT_LOGI(Snd, "DSOUND: effects image %u bytes, %u DSP effects, I3DL2 reverb at index %u %s (not emulated: "
                          "XAudio2 reverb submix; crosstalk index %u replaced by the spatializer)",
                     size, info.effectCount, loc.i3dl2Index, reverb::ImageHasReverb(info, loc) ? "present" : "missing",
                     loc.crosstalkIndex);
        } else {
            FUT_LOGW(Snd, "DSOUND: effects image of %u bytes not recognised (%s); the game continues the same", size,
                     error != nullptr ? error : "?");
        }
    }
    XDK_TRACE_CALL(IDirectSound_DownloadEffectsImage,
                   "(ds=0x%08X, image=0x%08X, size=%u, loc=0x%08X, ppDesc=0x%08X) -> S_OK", A(ds), A(image), size,
                   A(imageLoc), A(ppImageDesc));
    return kS_OK;
}
XDK_HOOK(IDirectSound_DownloadEffectsImage, &IDirectSound_DownloadEffectsImageHle);

// DSOUND::IDirectSound_SynchPlayback 0x247938 - stdcall (LPDIRECTSOUND), HRESULT, ret 4. Only XMV GetNextFrame
// 0x264E1B: starts every stream paused with Pause(2) at the same time (one engine batch = one XAudio2 operation
// set, m5_audio.md 5.4).
std::int32_t __stdcall IDirectSound_SynchPlaybackHle(void* ds) noexcept {
    std::uint32_t started = 0;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        bool batch = false;
        for (auto& [address, o] : g_state.objects) {
            if (o->kind == Kind::Stream && o->pauseState == kStreamPauseSynch) {
                if (!batch) {
                    Engine().BeginBatch();
                    batch = true;
                }
                o->pauseState = kStreamResume;
                o->startPending = false;
                if (o->voice != nullptr) {
                    if (o->dirty) {
                        Apply(*o);
                    }
                    o->voice->Start();
                }
                ++started;
                Emit(SoundEventKind::StreamResume, *o, "synch");
            }
        }
        if (batch) {
            Engine().CommitBatch();
        }
    }
    XDK_TRACE_CALL(IDirectSound_SynchPlayback, "(ds=0x%08X) -> S_OK (%u streams started)", A(ds), started);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SynchPlayback, &IDirectSound_SynchPlaybackHle);

// DSOUND::IDirectSound_CommitDeferredSettings 0x248380 - stdcall (LPDIRECTSOUND), HRESULT, ret 4.
// SoundManager::Update 0xCBE95 once per logic tick (2345 calls in 39.5 s, m5_trace_level.out), result ignored.
// Applies every deferred 3D value (listener, emitters, I3DL2 listener) together; with the 2D changes of the tick
// in one engine batch.
std::int32_t __stdcall IDirectSound_CommitDeferredSettingsHle(void* ds) noexcept {
    if (!xdk::detail::InPurgeProbe()) {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.committedListener = g_state.pendingListener;
        for (auto& [address, o] : g_state.objects) {
            o->committed3D = o->pending3D;
            if (o->is3D) {
                o->dirty = true;
            }
        }
        ApplyReverbIfPending();
        ApplyAllDirty();
    }
    XDK_TRACE_CALL(IDirectSound_CommitDeferredSettings, "(ds=0x%08X) -> S_OK", A(ds));
    return kS_OK;
}
XDK_HOOK(IDirectSound_CommitDeferredSettings, &IDirectSound_CommitDeferredSettingsHle);

// DSOUND::IDirectSound_SetDistanceFactor 0x248D60 - stdcall (ds, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Init 0xCCD98 (0.2, deferred), result ignored. In DS3D it only scales doppler (m5_audio.md 2.2).
std::int32_t __stdcall IDirectSound_SetDistanceFactorHle(void* ds, float factor, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetListener(apply, [&](Listener3D& l) { l.distanceFactor = factor; });
    }
    XDK_TRACE_CALL(IDirectSound_SetDistanceFactor, "(ds=0x%08X, %g, apply=%u) -> S_OK", A(ds), factor, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetDistanceFactor, &IDirectSound_SetDistanceFactorHle);

// DSOUND::IDirectSound_SetDopplerFactor 0x248D84 - stdcall (ds, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Init 0xCCD73 (2.0, immediate), result ignored. Without effect: every velocity is 0 (m5_audio.md 3.7).
std::int32_t __stdcall IDirectSound_SetDopplerFactorHle(void* ds, float factor, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetListener(apply, [&](Listener3D& l) { l.dopplerFactor = factor; });
    }
    XDK_TRACE_CALL(IDirectSound_SetDopplerFactor, "(ds=0x%08X, %g, apply=%u) -> S_OK", A(ds), factor, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetDopplerFactor, &IDirectSound_SetDopplerFactorHle);

// DSOUND::IDirectSound_SetOrientation 0x248DA8 - stdcall (ds, FLOAT xFront, yFront, zFront, xTop, yTop, zTop, DWORD
// dwApply), HRESULT, ret 32. SoundManager::Update 0xCBE85 per tick (deferred): front = -row 0, top = row 1 of the
// camera's world rotation (m5_listener_orient.out); passed on unchanged - the Spatializer handles the front/back
// question ([audio] positional, m5_audio.md 6.5).
std::int32_t __stdcall IDirectSound_SetOrientationHle(void* ds, float xFront, float yFront, float zFront, float xTop,
                                                      float yTop, float zTop, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetListener(apply, [&](Listener3D& l) {
            l.front = {xFront, yFront, zFront};
            l.top = {xTop, yTop, zTop};
        });
    }
    XDK_TRACE_CALL(IDirectSound_SetOrientation, "(ds=0x%08X, front=(%g, %g, %g), top=(%g, %g, %g), apply=%u) -> S_OK",
                   A(ds), xFront, yFront, zFront, xTop, yTop, zTop, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetOrientation, &IDirectSound_SetOrientationHle);

// DSOUND::IDirectSound_SetPosition 0x248DF2 - stdcall (ds, FLOAT x, y, z, DWORD dwApply), HRESULT, ret 20.
// Init 0xCCDB8 (0,0,0) and Update 0xCBE42 (camera position, deferred), result ignored.
std::int32_t __stdcall IDirectSound_SetPositionHle(void* ds, float x, float y, float z, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetListener(apply, [&](Listener3D& l) { l.position = {x, y, z}; });
    }
    XDK_TRACE_CALL(IDirectSound_SetPosition, "(ds=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(ds), x, y, z, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetPosition, &IDirectSound_SetPositionHle);

// DSOUND::IDirectSound_SetRolloffFactor 0x248E27 - stdcall (ds, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Init 0xCCDA7 (1.0), result ignored. Only used without a rolloff curve (FUN_00249893 0x24997E); the game always
// sets one.
std::int32_t __stdcall IDirectSound_SetRolloffFactorHle(void* ds, float factor, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetListener(apply, [&](Listener3D& l) { l.rolloffFactor = factor; });
    }
    XDK_TRACE_CALL(IDirectSound_SetRolloffFactor, "(ds=0x%08X, %g, apply=%u) -> S_OK", A(ds), factor, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetRolloffFactor, &IDirectSound_SetRolloffFactorHle);

// DSOUND::IDirectSound_SetVelocity 0x248E4B - stdcall (ds, FLOAT x, y, z, DWORD dwApply), HRESULT, ret 20.
// Update 0xCBE5C per tick with the zero vector 0x2F90BC, result ignored.
std::int32_t __stdcall IDirectSound_SetVelocityHle(void* ds, float x, float y, float z, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetListener(apply, [&](Listener3D& l) { l.velocity = {x, y, z}; });
    }
    XDK_TRACE_CALL(IDirectSound_SetVelocity, "(ds=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(ds), x, y, z, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetVelocity, &IDirectSound_SetVelocityHle);

// DSOUND::IDirectSound_SetI3DL2Listener 0x248E80 - stdcall (ds, LPCDSI3DL2LISTENER, DWORD dwApply), HRESULT, ret 12.
// Init 0xCCDC7 with Padded Cell 0x2D6884 (deferred) and SetEnvironment 0xCBFA0 (5 presets 0x2D6884..0x2D69BC;
// never called in this game, m5_audio.md 3.10), results ignored. The 12 fields are copied at the call; the reverb
// submix gets them at the next commit (or at once for an immediate apply).
std::int32_t __stdcall IDirectSound_SetI3DL2ListenerHle(void* ds, const void* listener, std::uint32_t apply) noexcept {
    if (!xdk::detail::InPurgeProbe()) {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (listener != nullptr && guest::IsReadable(A(listener), sizeof(XDsI3dl2Listener))) {
            XDsI3dl2Listener x;
            std::memcpy(&x, listener, sizeof(x));
            g_state.pendingReverb = ToListener(x);
            g_state.reverbPending = true;
            if ((apply & 1) == 0) {
                ApplyReverbIfPending();
            }
        }
    }
    XDK_TRACE_CALL(IDirectSound_SetI3DL2Listener, "(ds=0x%08X, preset=0x%08X, apply=%u) -> S_OK", A(ds), A(listener),
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetI3DL2Listener, &IDirectSound_SetI3DL2ListenerHle);

// ---- IDirectSoundBuffer -------------------------------------------------------------------------------------------------

// DSOUND::IDirectSoundBuffer_Release 0x246E93 - stdcall (buffer), ULONG, ret 4. Voice dtor 0xCC0F2 (after Stop,
// SetBufferData(0,0) and the GetStatus spin), result ignored. Destroys the voice synchronously and frees the object.
std::uint32_t __stdcall IDirectSoundBuffer_ReleaseHle(void* buffer) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (Object* b = Find(buffer, Kind::Buffer)) {
            Emit(SoundEventKind::Release, *b);
            --g_state.stats.buffersLive;
            DeleteObject(b);
        }
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_Release, "(buffer=0x%08X) -> 0", A(buffer));
    return 0;
}
XDK_HOOK(IDirectSoundBuffer_Release, &IDirectSoundBuffer_ReleaseHle);

// DSOUND::IDirectSoundBuffer_SetVolume 0x247950 - stdcall (buffer, LONG mB), HRESULT, ret 8. Voice update 0xCC91D
// every tick (mB from category volume * voice volume, 3.4), result ignored. Applied at the tick's commit.
std::int32_t __stdcall IDirectSoundBuffer_SetVolumeHle(void* buffer, std::int32_t volume) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetVolumeOn(Find(buffer, Kind::Buffer), volume);
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetVolume, "(buffer=0x%08X, %d mB) -> S_OK", A(buffer), volume);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetVolume, &IDirectSoundBuffer_SetVolumeHle);

// DSOUND::IDirectSoundBuffer_SetMixBins 0x24796C - stdcall (buffer, LPCDSMIXBINS), HRESULT, ret 8. 2D: every tick
// {2, {0, v0}, {1, v1}} with the pan volumes (0xCC9D3); 3D: once {6, {6,0},{8,0},{7,0},{9,0},{10,0},{3,0}}
// (0xCC35A). Results ignored. Stored per bin (FUN_00247080); 2D levels in audio/dsound_mix.h.
std::int32_t __stdcall IDirectSoundBuffer_SetMixBinsHle(void* buffer, const void* mixBins) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetMixBinsOn(Find(buffer, Kind::Buffer), mixBins);
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetMixBins, "(buffer=0x%08X, mixbins=0x%08X) -> S_OK", A(buffer), A(mixBins));
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetMixBins, &IDirectSoundBuffer_SetMixBinsHle);

// DSOUND::IDirectSoundBuffer_Play 0x247988 - stdcall (buffer, DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags),
// HRESULT, ret 16. Voice update 0xCC72E (flags = loop ? DSBPLAY_LOOPING : 0) and the resume paths 0xCC3DB/
// 0xCC45D/0xCC548 (`test eax,eax; je`: S_OK takes the normal path). Plays from the remembered cursor (0x24DF9E;
// Stop keeps it), looping over the whole buffer (smpl loop points are ignored by the game, 3.2). GetStatus reports
// playing from now on. Play on a playing buffer only changes the loop mode.
std::int32_t __stdcall IDirectSoundBuffer_PlayHle(void* buffer, std::uint32_t reserved1, std::uint32_t reserved2,
                                                  std::uint32_t flags) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (Object* b = Find(buffer, Kind::Buffer)) {
            const bool loop = (flags & kPlayLooping) != 0;
            if (BufferPlaying(*b)) {
                if (loop != b->looping) {
                    BufferHalt(*b, true);
                    b->looping = loop;
                    BufferStart(*b);
                }
            } else {
                b->looping = loop;
                BufferStart(*b);
                ++g_state.stats.plays;
                Emit(SoundEventKind::Play, *b);
                if (!g_state.firstPlayLogged && b->playing) {
                    g_state.firstPlayLogged = true;
                    FUT_LOGI(Snd, "DSOUND: first buffer playing: \"%s\" (%s, %u ch %u Hz, %u frames%s)", b->tag.name,
                             CategoryName(b->tag.valid ? b->tag.category : Category::Movie), b->src.channels,
                             b->src.sampleRate, b->frames, loop ? ", looping" : "");
                }
            }
        }
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_Play, "(buffer=0x%08X, %u, %u, flags=0x%X) -> S_OK", A(buffer), reserved1,
                   reserved2, flags);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_Play, &IDirectSoundBuffer_PlayHle);

// DSOUND::IDirectSoundBuffer_Stop 0x2479AC - stdcall (buffer), HRESULT, ret 4. SoundVoice::vf8 0xCB575 (pause menu,
// Snd_StopVoice) and the voice dtor, result ignored. Stops at once, remembers the cursor (CMcpxBuffer_Stop 0x24E04E),
// GetStatus is 0 right after (trap 3).
std::int32_t __stdcall IDirectSoundBuffer_StopHle(void* buffer) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (Object* b = Find(buffer, Kind::Buffer)) {
            if (b->playing) {
                BufferHalt(*b, true);
                Emit(SoundEventKind::Stop, *b);
            }
        }
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_Stop, "(buffer=0x%08X) -> S_OK", A(buffer));
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_Stop, &IDirectSoundBuffer_StopHle);

// DSOUND::IDirectSoundBuffer_GetStatus 0x2479C4 - stdcall (buffer, LPDWORD pdwStatus), HRESULT, ret 8. Voice update
// 0xCCA27 tests bit 0 every tick (0 = the voice ends, 0xCC63E; the stack DWORD is uninitialised before the call, so
// it is always written - also for NULL / unknown buffers), voice dtor 0xCC0BF spins while != 0. 1 playing, 5 playing
// looped, 0 after the end, Stop and SetBufferData(0,0) (CMcpxBuffer_GetStatus 0x24DB55).
std::int32_t __stdcall IDirectSoundBuffer_GetStatusHle(void* buffer, std::uint32_t* status) noexcept {
    std::uint32_t st = 0;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (Object* b = Find(buffer, Kind::Buffer)) {
            if (BufferPlaying(*b)) {
                st = kBufferStatusPlaying | (b->looping ? kBufferStatusLooping : 0u);
            }
        }
    }
    if (status != nullptr) {
        *status = st;
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_GetStatus, "(buffer=0x%08X, pdwStatus=0x%08X) -> S_OK, *pdwStatus=%u", A(buffer),
                   A(status), st);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_GetStatus, &IDirectSoundBuffer_GetStatusHle);

// DSOUND::IDirectSoundBuffer_SetFrequency 0x248398 - stdcall (buffer, DWORD Hz), HRESULT, ret 8. Voice update 0xCC910
// every tick with (int)(rate * pitch), pitch 0..4 (0xCA110); 0 = the buffer's own rate (0x247C42). Converted with the
// Xbox pitch quantization (codec::FrequencyRatio). Result ignored.
std::int32_t __stdcall IDirectSoundBuffer_SetFrequencyHle(void* buffer, std::uint32_t frequency) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetFrequencyOn(Find(buffer, Kind::Buffer), frequency);
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetFrequency, "(buffer=0x%08X, %u Hz) -> S_OK", A(buffer), frequency);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetFrequency, &IDirectSoundBuffer_SetFrequencyHle);

// DSOUND::IDirectSoundBuffer_SetMaxDistance 0x2483B4 - stdcall (buffer, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Voice update 0xCC6CE/0xCC83E (voice+0x34, default 80, deferred), results ignored.
std::int32_t __stdcall IDirectSoundBuffer_SetMaxDistanceHle(void* buffer, float distance, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        Set3D(Find(buffer, Kind::Buffer), apply, [&](Emitter3D& e) { e.maxDistance = distance; });
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetMaxDistance, "(buffer=0x%08X, %g, apply=%u) -> S_OK", A(buffer), distance,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetMaxDistance, &IDirectSoundBuffer_SetMaxDistanceHle);

// DSOUND::IDirectSoundBuffer_SetMinDistance 0x2483D8 - stdcall (buffer, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Voice update 0xCC6B9/0xCC829 (voice+0x30, default 5, deferred), results ignored.
std::int32_t __stdcall IDirectSoundBuffer_SetMinDistanceHle(void* buffer, float distance, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        Set3D(Find(buffer, Kind::Buffer), apply, [&](Emitter3D& e) { e.minDistance = distance; });
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetMinDistance, "(buffer=0x%08X, %g, apply=%u) -> S_OK", A(buffer), distance,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetMinDistance, &IDirectSoundBuffer_SetMinDistanceHle);

// DSOUND::IDirectSoundBuffer_SetPosition 0x2483FC - stdcall (buffer, FLOAT x, y, z, DWORD dwApply), HRESULT, ret 20.
// Voice update 0xCC6DF/0xCC7F7 (owner node +0xA0 or the fixed position, deferred), results ignored.
std::int32_t __stdcall IDirectSoundBuffer_SetPositionHle(void* buffer, float x, float y, float z,
                                                         std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        Object* b = Find(buffer, Kind::Buffer);
        Set3D(b, apply, [&](Emitter3D& e) { e.position = {x, y, z}; });
        NotePosition(b);
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetPosition, "(buffer=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(buffer), x, y, z,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetPosition, &IDirectSoundBuffer_SetPositionHle);

// DSOUND::IDirectSoundBuffer_SetVelocity 0x248431 - stdcall (buffer, FLOAT x, y, z, DWORD dwApply), HRESULT, ret 20.
// Voice update 0xCC6FC/0xCC814 (always the zero vector), results ignored.
std::int32_t __stdcall IDirectSoundBuffer_SetVelocityHle(void* buffer, float x, float y, float z,
                                                         std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        Set3D(Find(buffer, Kind::Buffer), apply, [&](Emitter3D& e) { e.velocity = {x, y, z}; });
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetVelocity, "(buffer=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(buffer), x, y, z,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetVelocity, &IDirectSoundBuffer_SetVelocityHle);

namespace {
void CopyCurve(Object* o, const float* points, std::uint32_t count, std::uint32_t apply) noexcept {
    if (o == nullptr) {
        return;
    }
    const std::uint32_t n = std::min(count, kMaxRolloffPoints);
    if (n != 0 && (points == nullptr || !guest::IsReadable(A(points), n * sizeof(float)))) {
        return;
    }
    float copy[kMaxRolloffPoints] = {};
    if (n != 0) {
        std::memcpy(copy, points, n * sizeof(float));  // DSOUND keeps the pointer (0x247D72); the table is constant
    }
    Set3D(o, apply, [&](Emitter3D& e) {
        e.rolloffCount = n;
        std::memcpy(e.rolloff, copy, sizeof(copy));
    });
}
}  // namespace

// DSOUND::IDirectSoundBuffer_SetRolloffCurve 0x248466 - stdcall (buffer, const FLOAT* points, DWORD count, DWORD
// dwApply), HRESULT, ret 16. Voice create 0xCC36D (curve 0x2EAD3C {1, 0.9, 0.8, 0.5, 0.25, 0}, 6 points,
// immediate), result ignored. The values are copied at the call (trap 7).
std::int32_t __stdcall IDirectSoundBuffer_SetRolloffCurveHle(void* buffer, const float* points, std::uint32_t count,
                                                             std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        CopyCurve(Find(buffer, Kind::Buffer), points, count, apply);
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetRolloffCurve, "(buffer=0x%08X, points=0x%08X, count=%u, apply=%u) -> S_OK",
                   A(buffer), A(points), count, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetRolloffCurve, &IDirectSoundBuffer_SetRolloffCurveHle);

// DSOUND::IDirectSoundBuffer_SetI3DL2Source 0x24848A - stdcall (buffer, LPCDSI3DL2BUFFER, DWORD dwApply), HRESULT,
// ret 12. Voice update 0xCC9DF..0xCCA0E every tick for 3D voices ({0, 0, -600, 0, 0, 0/0, 0/0}, deferred); for 3D
// STREAMS the game passes voice+0xA4 = NULL (trap 8) - ignored like the original's NULL check would. DSOUND copies
// 9 DWORDs (0x247DB5).
std::int32_t __stdcall IDirectSoundBuffer_SetI3DL2SourceHle(void* buffer, const void* source,
                                                            std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        Object* b = Find(buffer, Kind::Buffer);
        if (b != nullptr && source != nullptr && guest::IsReadable(A(source), sizeof(XDsI3dl2Buffer))) {
            XDsI3dl2Buffer x;
            std::memcpy(&x, source, sizeof(x));
            Set3D(b, apply, [&](Emitter3D& e) {
                e.i3dl2 = {x.lDirect, x.lDirectHF, x.lRoom, x.lRoomHF, x.flRoomRolloffFactor, x.lObstructionHF,
                           x.flObstructionLFRatio, x.lOcclusionHF, x.flOcclusionLFRatio};
            });
        }
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetI3DL2Source, "(buffer=0x%08X, source=0x%08X, apply=%u) -> S_OK", A(buffer),
                   A(source), apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetI3DL2Source, &IDirectSoundBuffer_SetI3DL2SourceHle);

// DSOUND::IDirectSoundBuffer_SetBufferData 0x248EA0 - stdcall (buffer, LPVOID pvData, DWORD dwBytes), HRESULT,
// ret 12. Voice create 0xCC2AF (game memory SoundData+0x48, size +0x14), Reset 0xCB597 and dtor 0xCC06F with (0,0).
// DSOUND does not copy (flag 0x80000000, 0x248B1E); the host copies/decodes at once, so the game may free the data
// any time later. Stops a playing buffer, resets the cursor (0x24E102); (0,0) leaves an empty buffer (GetStatus 0).
std::int32_t __stdcall IDirectSoundBuffer_SetBufferDataHle(void* buffer, void* data, std::uint32_t size) noexcept {
    std::int32_t hr = kS_OK;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (Object* b = Find(buffer, Kind::Buffer)) {
            BufferHalt(*b, false);
            if (b->data != nullptr) {
                try {
                    b->retired.push_back(std::move(b->data));
                } catch (...) {
                }
                b->data.reset();
            }
            b->frames = 0;
            b->cursor = 0;
            if (data != nullptr && size != 0 && b->decoder != nullptr) {
                if (!guest::IsReadable(A(data), size)) {
                    hr = kE_InvalidArg;
                } else {
                    try {
                        const std::size_t frames = b->decoder->FramesFor(size);
                        auto pcm = std::make_shared<std::vector<std::int16_t>>(frames * b->src.channels);
                        const std::size_t got = b->decoder->Decode(
                            std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), size),
                            std::span<std::int16_t>(*pcm));
                        b->frames = static_cast<std::uint32_t>(got);
                        b->data = std::move(pcm);
                    } catch (...) {
                        hr = kE_OutOfMemory;
                    }
                }
            }
        }
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_SetBufferData, "(buffer=0x%08X, data=0x%08X, %u bytes) -> 0x%08X", A(buffer),
                   A(data), size, static_cast<std::uint32_t>(hr));
    return hr;
}
XDK_HOOK(IDirectSoundBuffer_SetBufferData, &IDirectSoundBuffer_SetBufferDataHle);

// ---- IDirectSoundStream (C entries; the XMediaObject methods are above) -------------------------------------------------

// DSOUND::IDirectSoundStream_SetVolume 0x2479E0 (5-byte jmp -> CDirectSoundStream_SetVolume) - stdcall (stream, LONG
// mB), HRESULT, ret 8. Voice update 0xCC8C4 and the movie player 0xEE471 (category 0 volume, once at Open).
std::int32_t __stdcall IDirectSoundStream_SetVolumeHle(void* stream, std::int32_t volume) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetVolumeOn(Find(stream, Kind::Stream), volume);
    }
    XDK_TRACE_CALL(IDirectSoundStream_SetVolume, "(stream=0x%08X, %d mB) -> S_OK", A(stream), volume);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetVolume, &IDirectSoundStream_SetVolumeHle);

// DSOUND::IDirectSoundStream_SetMixBins 0x2479E5 (jmp -> CDirectSoundStream_SetMixBins) - stdcall (stream,
// LPCDSMIXBINS), HRESULT, ret 8. Voice create 0xCC32C, update 0xCC9C0 (2D pan volumes), results ignored.
std::int32_t __stdcall IDirectSoundStream_SetMixBinsHle(void* stream, const void* mixBins) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetMixBinsOn(Find(stream, Kind::Stream), mixBins);
    }
    XDK_TRACE_CALL(IDirectSoundStream_SetMixBins, "(stream=0x%08X, mixbins=0x%08X) -> S_OK", A(stream), A(mixBins));
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetMixBins, &IDirectSoundStream_SetMixBinsHle);

// DSOUND::IDirectSoundStream_Pause 0x2479EA (jmp -> CDirectSoundStream_Pause 0x24BEBD) - stdcall (stream, DWORD
// dwPause), HRESULT, ret 8. Game: 0 = start/resume (voice update 0xCC711, Play variants 0xCC3B9/0xCC43A/0xCC500),
// 1 = pause (Stop 0xCB563; queue kept); XMV 0x264A81: 2 = wait for SynchPlayback. Results ignored by the game.
std::int32_t __stdcall IDirectSoundStream_PauseHle(void* stream, std::uint32_t pause) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (Object* s = Find(stream, Kind::Stream)) {
            const std::uint32_t next = pause == kStreamResume ? kStreamResume
                                       : pause == kStreamPauseSynch ? kStreamPauseSynch
                                                                    : kStreamPause;
            if (next != s->pauseState) {
                s->pauseState = next;
                if (next == kStreamResume) {
                    s->startPending = s->voice != nullptr;  // starts at the tick's commit (ApplyAllDirty)
                } else {
                    s->startPending = false;
                    if (s->voice != nullptr) {
                        s->voice->Stop();
                    }
                }
                Emit(next == kStreamResume ? SoundEventKind::StreamResume : SoundEventKind::StreamPause, *s);
            }
        }
    }
    XDK_TRACE_CALL(IDirectSoundStream_Pause, "(stream=0x%08X, %u) -> S_OK", A(stream), pause);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_Pause, &IDirectSoundStream_PauseHle);

// DSOUND::IDirectSoundStream_FlushEx 0x2479EF - stdcall (stream, REFERENCE_TIME rtTimeStamp, DWORD dwFlags), HRESULT,
// ret 16. Only XMV CloseDecoder 0x2646A4 with (s, 0, 0, ASYNC); it then spins on GetStatus until PLAYING (0x10000)
// drops, calling DirectSoundDoWork (m5_audio.md 5.6). Every queued packet ends with E_ABORT at once (the original
// does it in its DPC; doing it here on the guest thread is allowed by E10 and makes PLAYING drop immediately);
// callbacks run after the entry's lock is released. The time stamp (flush at a time) is not supported (always 0).
std::int32_t __stdcall IDirectSoundStream_FlushExHle(void* stream, std::int64_t timeStamp,
                                                     std::uint32_t flags) noexcept {
    std::vector<PendingCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (Object* s = Find(stream, Kind::Stream)) {
            FlushPackets(*s, callbacks, true);
            Emit(SoundEventKind::Flush, *s, (flags & kFlushExAsync) != 0 ? "async" : "sync");
        }
    }
    RunCallbacks(callbacks);
    XDK_TRACE_CALL(IDirectSoundStream_FlushEx, "(stream=0x%08X, rt=%lld, flags=0x%X) -> S_OK", A(stream),
                   static_cast<long long>(timeStamp), flags);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_FlushEx, &IDirectSoundStream_FlushExHle);

// DSOUND::IDirectSoundStream_SetFrequency 0x2484AA (jmp -> CDirectSoundStream_SetFrequency) - stdcall (stream, DWORD
// Hz), HRESULT, ret 8. Voice update 0xCC8BA, result ignored. Like the buffer version.
std::int32_t __stdcall IDirectSoundStream_SetFrequencyHle(void* stream, std::uint32_t frequency) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        SetFrequencyOn(Find(stream, Kind::Stream), frequency);
    }
    XDK_TRACE_CALL(IDirectSoundStream_SetFrequency, "(stream=0x%08X, %u Hz) -> S_OK", A(stream), frequency);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetFrequency, &IDirectSoundStream_SetFrequencyHle);

// DSOUND::IDirectSoundStream_SetMaxDistance 0x2484AF - stdcall (stream, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Voice update 0xCC7E9 (3D streams only; none in the trace run), result ignored.
std::int32_t __stdcall IDirectSoundStream_SetMaxDistanceHle(void* stream, float distance, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        Set3D(Find(stream, Kind::Stream), apply, [&](Emitter3D& e) { e.maxDistance = distance; });
    }
    XDK_TRACE_CALL(IDirectSoundStream_SetMaxDistance, "(stream=0x%08X, %g, apply=%u) -> S_OK", A(stream), distance,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetMaxDistance, &IDirectSoundStream_SetMaxDistanceHle);

// DSOUND::IDirectSoundStream_SetMinDistance 0x2484C7 - stdcall (stream, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Voice update 0xCC7D7, result ignored.
std::int32_t __stdcall IDirectSoundStream_SetMinDistanceHle(void* stream, float distance, std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        Set3D(Find(stream, Kind::Stream), apply, [&](Emitter3D& e) { e.minDistance = distance; });
    }
    XDK_TRACE_CALL(IDirectSoundStream_SetMinDistance, "(stream=0x%08X, %g, apply=%u) -> S_OK", A(stream), distance,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetMinDistance, &IDirectSoundStream_SetMinDistanceHle);

// DSOUND::IDirectSoundStream_SetPosition 0x2484DF - stdcall (stream, FLOAT x, y, z, DWORD dwApply), HRESULT, ret 20.
// Voice update 0xCC7AB, result ignored.
std::int32_t __stdcall IDirectSoundStream_SetPositionHle(void* stream, float x, float y, float z,
                                                         std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        Object* s = Find(stream, Kind::Stream);
        Set3D(s, apply, [&](Emitter3D& e) { e.position = {x, y, z}; });
        NotePosition(s);
    }
    XDK_TRACE_CALL(IDirectSoundStream_SetPosition, "(stream=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(stream), x, y, z,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetPosition, &IDirectSoundStream_SetPositionHle);

// DSOUND::IDirectSoundStream_SetVelocity 0x248508 - stdcall (stream, FLOAT x, y, z, DWORD dwApply), HRESULT, ret 20.
// Voice update 0xCC7C5, result ignored.
std::int32_t __stdcall IDirectSoundStream_SetVelocityHle(void* stream, float x, float y, float z,
                                                         std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        Set3D(Find(stream, Kind::Stream), apply, [&](Emitter3D& e) { e.velocity = {x, y, z}; });
    }
    XDK_TRACE_CALL(IDirectSoundStream_SetVelocity, "(stream=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(stream), x, y, z,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetVelocity, &IDirectSoundStream_SetVelocityHle);

// DSOUND::IDirectSoundStream_SetRolloffCurve 0x248531 (jmp -> CDirectSoundStream_SetRolloffCurve) - stdcall (stream,
// const FLOAT* points, DWORD count, DWORD dwApply), HRESULT, ret 16. Voice create 0xCC33C, result ignored.
std::int32_t __stdcall IDirectSoundStream_SetRolloffCurveHle(void* stream, const float* points, std::uint32_t count,
                                                             std::uint32_t apply) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        CopyCurve(Find(stream, Kind::Stream), points, count, apply);
    }
    XDK_TRACE_CALL(IDirectSoundStream_SetRolloffCurve, "(stream=0x%08X, points=0x%08X, count=%u, apply=%u) -> S_OK",
                   A(stream), A(points), count, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetRolloffCurve, &IDirectSoundStream_SetRolloffCurveHle);

// ---- global functions --------------------------------------------------------------------------------------------------

// DSOUND::DirectSoundUseFullHRTF 0x246EA9 - stdcall (void), void, ret 0. Init 0xCCD64 (selects DSOUND's full HRTF
// filters, 0x246DF1). The host has no HRTF DSP; direction comes from the Spatializer (m5_audio.md 6.5). No-op.
void __stdcall DirectSoundUseFullHRTFHle() noexcept {
    XDK_TRACE_CALL(DirectSoundUseFullHRTF, "() -> no-op");
}
XDK_HOOK(DirectSoundUseFullHRTF, &DirectSoundUseFullHRTFHle);

// DSOUND::DirectSoundDoWork 0x247A07 - stdcall (void), void, ret 0. SoundManager::Update 0xCBE9A once per logic tick
// (also XMV GetNextFrame/CloseDecoder 0x2646DA/0x264ED9). The only place where finished stream packets reach the
// guest (completed size, status 0, completion event, XMV callback via core::CallGuest after the lock is released),
// plus the engine's device handling (Pump) and the per-tick parameter batch.
void __stdcall DirectSoundDoWorkHle() noexcept {
    std::uint32_t completed = 0;
    std::vector<PendingCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        completed = DoWorkLocked(callbacks);
    }
    RunCallbacks(callbacks);
    XDK_TRACE_CALL(DirectSoundDoWork, "() -> %u stream packets completed", completed);
}
XDK_HOOK(DirectSoundDoWork, &DirectSoundDoWorkHle);

// ==================================================================================================================
// Host-side helpers
// ==================================================================================================================

void SetPendingVoiceTag(const VoiceTag& tag) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.pendingTag = tag;
    g_state.pendingTag.valid = true;
}

void ClearPendingVoiceTag() noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.pendingTag.valid = false;
}

void NoteOrphanedObject(const void* object, std::uint32_t voiceId) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    Object* o = Find(object, Kind::Buffer);
    if (o == nullptr) {
        o = Find(object, Kind::Stream);
    }
    if (o == nullptr) {
        return;
    }
    if (g_state.stats.orphans++ < 4) {
        FUT_LOGI(Snd, "DSOUND: second Play on SoundVoice #%u replaces live object 0x%08X (\"%s\"); it is never "
                      "released (original behaviour, m5_audio.md 3.3)",
                 voiceId, o->Addr(), o->tag.name);
    }
    Emit(SoundEventKind::OrphanedObject, *o);
}

bool IsDsoundDevice(const void* p) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return p != nullptr && p == g_state.device;
}
bool IsDsoundBuffer(const void* p) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return Find(p, Kind::Buffer) != nullptr;
}
bool IsDsoundStream(const void* p) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return Find(p, Kind::Stream) != nullptr;
}
std::uint32_t DsoundStreamQueuedPackets(const void* stream) noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    const Object* s = Find(stream, Kind::Stream);
    return s != nullptr ? static_cast<std::uint32_t>(s->packets.size()) : 0;
}

ObjectView InspectObject(const void* object) noexcept {
    ObjectView v;
    std::lock_guard<std::mutex> lock(g_state.mutex);
    Object* o = Find(object, Kind::Buffer);
    if (o == nullptr) {
        o = Find(object, Kind::Stream);
    }
    if (o == nullptr) {
        return v;
    }
    v.found = true;
    v.hasVoice = o->voice != nullptr;
    v.category = o->tag.valid ? o->tag.category : Category::Movie;
    v.is3D = o->is3D;
    v.cursor = o->kind == Kind::Buffer ? BufferPosition(*o) : 0;
    v.frames = o->frames;
    v.frequencyRatio = o->appliedRatio;
    v.outputChannels = o->appliedChannels;
    std::memcpy(v.matrix, o->appliedMatrix, sizeof(v.matrix));
    v.reverbSend = o->appliedSend;
    v.startPending = o->startPending;
    return v;
}

Listener3D CommittedListener() noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return g_state.committedListener;
}
I3dl2Listener CommittedReverb() noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return g_state.committedReverb;
}

DsoundStatistics GetDsoundStatistics() noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return g_state.stats;
}

void LogDsoundSummary(core::Level level) noexcept {
    LogEngineSummary();  // "audio summary: ..." (the engine registers no exit reporter of its own)
    DsoundStatistics s;
    {
        // try_lock: also the crash reporter "xdk", possibly while the guest thread holds the DSOUND lock.
        std::unique_lock<std::mutex> lock(g_state.mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            FUT_LOGW(Snd, "DSOUND HLE: statistics not available (DSOUND busy)");
            return;
        }
        s = g_state.stats;
    }
    if (s.buffersCreated == 0 && s.streamsCreated == 0) {
        return;
    }
    if (core::LogEnabled(core::Channel::Snd, level)) {
        core::LogWrite(core::Channel::Snd, level,
                       "DSOUND HLE: buffers %u created (%u live, peak %u, %u tagged objects), %u plays, %u orphaned "
                       "by a double Play; streams %u created (%u live), packets %u submitted, %u completed, %u "
                       "flushed, %u rejected, %u underruns; %u format errors, %u objects without voice",
                       s.buffersCreated, s.buffersLive, s.buffersPeak, s.tagged, s.plays, s.orphans,
                       s.streamsCreated, s.streamsLive, s.packetsSubmitted, s.packetsCompleted, s.packetsFlushed,
                       s.packetsRejected, s.starvations, s.formatErrors, s.voiceErrors);
    }
}

void ResetDsoundForTesting() noexcept {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    std::vector<Object*> all;
    for (auto& [address, o] : g_state.objects) {
        all.push_back(o.get());
    }
    for (Object* o : all) {
        DeleteObject(o);
    }
    {
        std::lock_guard<std::mutex> evLock(g_eventMutex);
        g_events.clear();
    }
    if (g_state.device != nullptr) {
        g_state.pool.Free(g_state.device);
        g_state.device = nullptr;
    }
    g_state.deviceRefs = 0;
    g_state.pendingListener = {};
    g_state.committedListener = {};
    g_state.pendingReverb = {};
    g_state.committedReverb = {};
    g_state.reverbPending = false;
    g_state.pendingTag = {};
    g_state.stats = {};
    g_state.firstPlayLogged = false;
    g_state.firstPacketLogged = false;
    g_state.outputChannels = 0;
    g_state.outputMask = 0;
}

void SetSoundEventSink(SoundEventSink* sink) noexcept {
    g_sink.store(sink);
}

}  // namespace snd
