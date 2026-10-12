// XMV HLE (M6, see xmv_hle.h). Callers are the game's movie player (avi.md 3.2): Open 0xEE220 (create, descriptor,
// audio, texture), Update FUN_000EDE90 (next frame, playback time; draws the quad between YuvEnable(1)/(0)), Close
// FUN_000EE080 (close; also from the player destructor when App slot 7 destroys a finished or skipped movie).
// "player" = the 0x48-byte player object (FUN_000EE050 ctor): +0x00 done flag, +0x0C file object, +0x10 texture,
// +0x18 decoder, +0x1C..+0x28 video descriptor, +0x2C/+0x30 packet buffers, +0x34 loaded / +0x38 free / +0x3C
// current packet buffer, +0x41 "no frame yet" flag.
//
// Threads: every entry runs on the guest thread; the packet callbacks are guest code called from GetNextFrame
// exactly like the original XMV library did (the player's file Read may switch fibers - no host lock is held across
// a guest call). Audio goes through the DSOUND HLE on the same thread.
#include "video/xmv_hle.h"

#include "audio/codec_adpcm.h"
#include "audio/dsound_hle.h"
#include "audio/xdk_entries.h"
#include "core/guest.h"
#include "core/guest_call.h"
#include "core/log.h"
#include "gfx/d3d_shim.h"
#include "video/movie_clock.h"
#include "video/movie_core.h"
#include "video/yuv_pack.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace vid {
namespace {

std::uint32_t A(const volatile void* p) noexcept {
    return guest::Addr(p);
}

constexpr std::int32_t kS_OK = 0;
constexpr std::int32_t kE_Fail = static_cast<std::int32_t>(0x80004005);
constexpr std::int32_t kE_InvalidArg = static_cast<std::int32_t>(0x80070057);
constexpr std::int32_t kE_OutOfMemory =
    static_cast<std::int32_t>(0x8007000E);  // original OOM code (0x2643A4)

constexpr std::uint32_t kDecoderMagic = 0x56444D58;  // 'XMDV'
constexpr std::uint32_t kAudioSlots = 32;            // DSSTREAMDESC dwMaxAttachedPackets of a movie stream
constexpr std::uint64_t kStartLeadMs = 600;          // audio demuxed before the first frame
constexpr std::uint64_t kAudioLeadMs = 1200;         // audio kept ahead of the clock (submitted + queued)
constexpr std::uint32_t kMaxPacketsPerCall = 32;     // read-ahead bound per GetNextFrame
constexpr std::int64_t kAudioStallMs = 1000;         // audio clock frozen this long -> wall clock

// Guest-visible decoder handle; the game treats the pointer as opaque (only passed to XMV entries: 0xEE389,
// 0xEE412, 0xEE424, 0xEDEDB, 0xEDFB7, 0xEE11F), so +0 is the trap vtable.
struct GuestHandle {
    std::uint32_t vtable;
    std::uint32_t magic;
    std::uint32_t id;
};
static_assert(sizeof(GuestHandle) <= xdk::kGuestObjectSize);

std::int64_t Qpc() noexcept {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
std::int64_t QpcFrequency() noexcept {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
}

// Packets through the movie player's callbacks (guest.h kMovieGetNextPacket / kMovieReleasePreviousPacket; the
// game passes them to Create). GetNextPacket swaps the player's buffers and returns the loaded packet plus the size
// of the next one (its first DWORD); ReleasePreviousPacket(nextSize) reads that packet into the freed buffer. The
// returned packet stays valid until the second ReleasePreviousPacket after it, so it is copied at once and the
// next read is started right away (the original XMV library calls both from GetNextFrame the same way).
class GamePacketSource final : public PacketSource {
public:
    GamePacketSource(std::uint32_t context, std::uint32_t getNext, std::uint32_t release, std::uint32_t maxPacket)
        : context_(context), getNext_(getNext), release_(release), maxPacket_(maxPacket) {}

    bool Read(std::uint32_t size, std::vector<std::uint8_t>& out) override {
        if (getNext_ == 0 || release_ == 0 || (maxPacket_ != 0 && size > maxPacket_)) {
            return false;
        }
        std::uint32_t packet = 0;
        std::uint32_t nextSize = 0;
        core::CallGuest(getNext_, {context_, A(&packet), A(&nextSize)});
        if (packet == 0 || !guest::IsReadable(packet, size)) {
            return false;
        }
        const auto* p = guest::Ptr<const std::uint8_t>(packet);
        out.assign(p, p + size);
        offset_ += size;
        // stdcall (ctx, LONGLONG offset (low, high), DWORD nextSize); the player ignores the offset.
        core::CallGuest(release_, {context_, static_cast<std::uint32_t>(offset_),
                                   static_cast<std::uint32_t>(offset_ >> 32), nextSize});
        return true;
    }

private:
    std::uint32_t context_, getNext_, release_, maxPacket_;
    std::uint64_t offset_ = 0;
};

struct TrackStream {
    std::uint32_t track = 0;
    void* stream = nullptr;  // DSOUND HLE stream (our reference)
    snd::XWaveFormatEx wfx{};
    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
    std::array<std::uint32_t, kAudioSlots> status{};     // XMEDIAPACKET pdwStatus targets (host memory)
    std::array<std::uint32_t, kAudioSlots> completed{};  // pdwCompletedSize targets
    std::uint64_t framesSubmitted = 0;
    bool discontinuity = false;
};

struct Movie {
    GuestHandle* handle = nullptr;
    std::uint32_t id = 0;
    xmv::FileHeader header;
    std::unique_ptr<GamePacketSource> source;
    MovieCore core;
    std::vector<std::unique_ptr<TrackStream>> tracks;  // enabled tracks (the game enables one)
    MovieClock clock;
    bool started = false;
    bool ended = false;
    bool failed = false;
    bool audioClock = false;
    std::uint64_t lastAudioFrames = 0;
    std::int64_t lastAudioChange = 0;
    std::int64_t freq = 1;
    std::uint32_t lastShownPts = 0;
    std::uint32_t framesShown = 0;
    std::uint32_t framesDropped = 0;
    std::uint32_t maxLateMs = 0;
    std::uint32_t surfaceErrors = 0;
    I420Frame picture;
};

std::mutex g_mutex;  // the map only (never held across guest calls)
std::unordered_map<std::uint32_t, std::unique_ptr<Movie>> g_movies;  // by guest handle address
std::atomic<std::uint32_t> g_nextId{1};
std::atomic<std::uint32_t> g_opened{0}, g_finished{0}, g_closedEarly{0}, g_failed{0}, g_framesShown{0},
    g_framesDropped{0}, g_maxLateMs{0}, g_audioPackets{0}, g_audioRejected{0};
std::atomic<bool> g_loggedDecoderMissing{false};

Movie* Find(void* decoder) noexcept {
    if (decoder == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_movies.find(A(decoder));
    return it != g_movies.end() ? it->second.get() : nullptr;
}

TrackStream* FindTrack(Movie& m, std::uint32_t track) noexcept {
    for (auto& t : m.tracks) {
        if (t->track == track) {
            return t.get();
        }
    }
    return nullptr;
}

std::uint64_t FramesPlayed(const TrackStream& t) noexcept {
    std::uint64_t frames = 0;
    snd::StreamFramesPlayed(t.stream, frames);
    return frames;
}

// Audio ahead of playback for the clock track (demuxed but not yet played), in ms; "infinite" without audio.
std::uint64_t AudioAheadMs(Movie& m) noexcept {
    if (m.tracks.empty() || m.tracks.front()->rate == 0) {
        return ~0ull;
    }
    const TrackStream& t = *m.tracks.front();
    const std::uint64_t demuxed = m.core.AudioFramesDemuxed(t.track);
    const std::uint64_t played = m.started ? FramesPlayed(t) : 0;
    return demuxed > played ? (demuxed - played) * 1000u / t.rate : 0;
}

// Submits queued ADPCM chunks of every enabled track into free stream slots, keeping about kAudioLeadMs ahead.
void SubmitAudio(Movie& m) noexcept {
    for (auto& tp : m.tracks) {
        TrackStream& t = *tp;
        auto& queue = m.core.Audio(t.track);
        const std::uint64_t played = m.started ? FramesPlayed(t) : 0;
        while (!queue.empty() && t.stream != nullptr) {
            if (t.rate != 0 && t.framesSubmitted > played &&
                (t.framesSubmitted - played) * 1000u / t.rate >= kAudioLeadMs) {
                break;
            }
            std::uint32_t slot = kAudioSlots;
            for (std::uint32_t i = 0; i < kAudioSlots; ++i) {
                if (t.status[i] != snd::kPacketPending) {
                    slot = i;
                    break;
                }
            }
            if (slot == kAudioSlots) {
                break;
            }
            xmv::AudioChunk& c = queue.front();
            snd::XMediaPacket packet{};
            packet.pvBuffer = A(c.data.data());
            packet.dwMaxSize = static_cast<std::uint32_t>(c.data.size());
            packet.pdwCompletedSize = A(&t.completed[slot]);
            packet.pdwStatus = A(&t.status[slot]);
            const std::int32_t hr = snd::DsStreamProcess(t.stream, &packet, nullptr);
            if (hr < 0) {
                g_audioRejected.fetch_add(1, std::memory_order_relaxed);
                break;  // queue full: try again at the next call
            }
            t.framesSubmitted += snd::codec::XboxAdpcmFrames(c.data.size(), t.channels, true);
            g_audioPackets.fetch_add(1, std::memory_order_relaxed);
            queue.pop_front();  // DSOUND decoded (copied) the packet in Process
        }
        if (queue.empty() && m.core.Eof() && !t.discontinuity && t.stream != nullptr) {
            snd::DsStreamDiscontinuity(t.stream);  // no packet follows (the original at the end of the data)
            t.discontinuity = true;
        }
    }
}

// Reads packets until a video frame after `nowMs` is known (or the end), and the audio lead is reached.
void ReadAhead(Movie& m, std::uint64_t nowMs) {
    for (std::uint32_t i = 0; i < kMaxPacketsPerCall && !m.core.Eof() && !m.core.Failed(); ++i) {
        const bool needVideo = m.core.Video().empty() || m.core.Video().back().ptsMs <= nowMs;
        const bool needAudio = AudioAheadMs(m) < kAudioLeadMs;
        if (!needVideo && !needAudio) {
            break;
        }
        if (!m.core.ReadPacket()) {
            break;
        }
    }
}

// Total audio length of the clock track in ms once the file is demuxed completely (0 = no audio).
std::uint64_t AudioEndMs(Movie& m) noexcept {
    if (m.tracks.empty() || m.tracks.front()->rate == 0) {
        return 0;
    }
    return m.core.AudioFramesDemuxed(m.tracks.front()->track) * 1000u / m.tracks.front()->rate;
}

std::uint64_t ClockNow(Movie& m) noexcept {
    const std::int64_t now = Qpc();
    if (!m.audioClock) {
        return m.clock.NowMs(0, false, now);
    }
    const TrackStream& t = *m.tracks.front();
    const std::uint64_t played = FramesPlayed(t);
    bool finished = m.core.Eof() && m.core.Audio(t.track).empty() && t.framesSubmitted != 0 &&
                    played >= t.framesSubmitted;
    if (played != m.lastAudioFrames) {
        m.lastAudioFrames = played;
        m.lastAudioChange = now;
    } else if (m.clock.UsesAudio() && (now - m.lastAudioChange) * 1000 / m.freq > kAudioStallMs) {
        // The audio output does not advance (no engine progress, or everything played): continue on the wall clock.
        if (!finished) {
            FUT_LOGW(Vid, "XMV: movie #%u: audio clock stalled at %llu frames for %lld ms - wall clock from here", m.id,
                     static_cast<unsigned long long>(played), static_cast<long long>(kAudioStallMs));
        }
        finished = true;
    }
    return m.clock.NowMs(played, finished, now);
}

void Account(Movie& m, std::uint32_t pts, std::uint64_t now, std::uint32_t dropped) noexcept {
    m.framesShown += 1;
    m.framesDropped += dropped;
    g_framesShown.fetch_add(1, std::memory_order_relaxed);
    g_framesDropped.fetch_add(dropped, std::memory_order_relaxed);
    const std::uint32_t late = now > pts ? static_cast<std::uint32_t>(now - pts) : 0u;
    if (m.started && late > m.maxLateMs) {
        m.maxLateMs = late;
    }
    std::uint32_t prev = g_maxLateMs.load(std::memory_order_relaxed);
    while (m.started && late > prev && !g_maxLateMs.compare_exchange_weak(prev, late)) {
    }
    m.lastShownPts = pts;
}

// Writes the picture as YUY2 into the game's surface (the original FUN_00265B0B via D3DSurface_LockRect).
void WritePicture(Movie& m, void* surface) noexcept {
    gfx::d3d::ResourceInfo info;
    if (surface == nullptr || !gfx::d3d::LookupResource(A(surface), info) || info.format != gfx::d3d::kFormatYuy2 ||
        info.width != m.picture.width || info.height != m.picture.height || info.pitch < 2u * info.width ||
        info.pixels == 0) {
        if (m.surfaceErrors++ == 0) {
            FUT_LOGW(Vid, "XMV: movie #%u: surface 0x%08X is not a %ux%u YUY2 shim surface - frame not written", m.id,
                     A(surface), m.picture.width, m.picture.height);
        }
        return;
    }
    PackI420ToYuy2(m.picture, guest::Ptr<std::uint8_t>(info.pixels), info.pitch);
    gfx::d3d::NoteResourceWritten(A(surface));
}

// Decodes every due frame (P-frames need their predecessors) and keeps the newest picture. Returns the number of
// frames decoded; `pts` = pts of the newest.
std::uint32_t DecodeDue(Movie& m, std::uint64_t nowMs, bool first, std::uint32_t& pts) {
    std::uint32_t decoded = 0;
    while (!m.core.Video().empty() && (first ? decoded == 0 : m.core.Video().front().ptsMs <= nowMs)) {
        const xmv::VideoFrame f = std::move(m.core.Video().front());
        m.core.Video().pop_front();
        bool produced = false;
        if (!m.core.Decode(f, m.picture, produced) && m.core.Failed()) {
            return decoded;
        }
        if (produced) {
            pts = f.ptsMs;
            ++decoded;
        }
        if (first) {
            break;
        }
        if (m.core.Video().empty()) {
            ReadAhead(m, nowMs);
        }
    }
    return decoded;
}

void LogClose(const Movie& m, const char* how) {
    FUT_LOGI(Vid,
             "XMV: movie #%u %s at %llu ms: %u frames shown of %u demuxed, %u dropped (late), max %u ms late, "
             "%u decode errors, clock %s",
             m.id, how, static_cast<unsigned long long>(m.clock.LastMs()), m.framesShown,
             m.core.Stats().framesDemuxed, m.framesDropped, m.maxLateMs, m.core.Stats().decodeErrors,
             m.audioClock ? "audio" : "wall");
}

bool StartPlayback(Movie& m) {
    // Prime: the first frame and some audio (all streams start together like the original's Pause(2) +
    // SynchPlayback; their engine voices then run in one operation set).
    for (std::uint32_t i = 0; i < 4 * kMaxPacketsPerCall && !m.core.Eof() && !m.core.Failed(); ++i) {
        if (!m.core.Video().empty() && AudioAheadMs(m) >= kStartLeadMs) {
            break;
        }
        if (!m.core.ReadPacket()) {
            break;
        }
    }
    if (m.core.Failed()) {
        return false;
    }
    SubmitAudio(m);
    if (!m.tracks.empty()) {
        void* ds = nullptr;
        snd::DirectSoundCreateHle(nullptr, &ds, nullptr);
        snd::IDirectSound_SynchPlaybackHle(ds);
        snd::IDirectSound_ReleaseHle(ds);
    }
    std::uint64_t probe = 0;
    m.audioClock = !m.tracks.empty() && m.tracks.front()->rate != 0 &&
                   snd::StreamFramesPlayed(m.tracks.front()->stream, probe);
    m.freq = QpcFrequency();
    m.clock.Start(m.audioClock ? m.tracks.front()->rate : 0, Qpc(), m.freq);
    m.lastAudioChange = Qpc();
    m.started = true;
    const TrackStream* t = m.tracks.empty() ? nullptr : m.tracks.front().get();
    FUT_LOGI(Vid, "XMV: movie #%u playing: %u x %u, %s%u Hz %u ch (track %u), clock %s", m.id, m.header.width,
             m.header.height, t != nullptr ? "audio " : "no audio", t != nullptr ? t->rate : 0u,
             t != nullptr ? t->channels : 0u, t != nullptr ? t->track : 0u, m.audioClock ? "audio" : "wall (QPC)");
    return true;
}

std::int32_t Fail(Movie& m, std::int32_t* result, const char* what) {
    if (!m.failed) {
        m.failed = true;
        g_failed.fetch_add(1, std::memory_order_relaxed);
        FUT_LOGE(Vid, "XMV: movie #%u: %s: %s - movie ends", m.id, what, m.core.Error().c_str());
    }
    if (result != nullptr) {
        *result = kXmvFailure;
    }
    return kS_OK;
}

}  // namespace

bool IsXmvDecoder(const void* decoder) noexcept {
    return Find(const_cast<void*>(decoder)) != nullptr;
}

std::uint32_t LiveXmvDecoders() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<std::uint32_t>(g_movies.size());
}

MovieStatistics GetMovieStatistics() noexcept {
    MovieStatistics s;
    s.opened = g_opened.load();
    s.finished = g_finished.load();
    s.closedEarly = g_closedEarly.load();
    s.failed = g_failed.load();
    s.framesShown = g_framesShown.load();
    s.framesDropped = g_framesDropped.load();
    s.maxLateMs = g_maxLateMs.load();
    s.audioPackets = g_audioPackets.load();
    s.audioRejected = g_audioRejected.load();
    return s;
}

void LogMovieSummary() noexcept {
    const MovieStatistics s = GetMovieStatistics();
    FUT_LOGI(Vid,
             "XMV HLE: %u movies opened, %u played to the end, %u closed early (skipped), %u failed; %u frames shown, "
             "%u dropped, max %u ms late; %u audio packets (%u rejected)",
             s.opened, s.finished, s.closedEarly, s.failed, s.framesShown, s.framesDropped, s.maxLateMs,
             s.audioPackets, s.audioRejected);
}

// XMV::XMVDecoder_CreateDecoderForPackets 0x264360 (name assumed, avi.md 3.1) - stdcall (DWORD dwFlags, const
// header* pvFirstPacketHeader, void* context, pfnGetNextPacket, pfnReleasePreviousPacket, XMVDecoder** ppDecoder),
// HRESULT, ret 24. Only caller: movie Open 0xEE35C with (0, &hdr (0x1000 bytes read from "Video\<name>.xmv"), player,
// 0xEDBA0, 0xEDE60, &player+0x18); `test eax,eax; jge 0xEE373` continues on success, otherwise Close + return 0
// (0xEE365). The original reads W/H at +0x14/+0x18 and the audio stream count (u16) at +0x20 and writes *ppDecoder
// only on success. Nothing is read through the callbacks here: Open allocates the packet buffers and reads the first
// packet only after Create (0xEE395..0xEE3CE); the first GetNextFrame (end of Open) starts reading.
std::int32_t __stdcall XMVDecoder_CreateDecoderForPacketsHle(std::uint32_t flags, const XmvFileHeader* header,
                                                             void* context, const void* getNextPacket,
                                                             const void* releasePreviousPacket,
                                                             void** ppDecoder) noexcept {
    std::int32_t hr = kS_OK;
    GuestHandle* handle = nullptr;
    std::uint32_t id = 0;
    xmv::FileHeader parsed;
    if (header == nullptr || !guest::IsReadable(A(header), xmv::kFileHeaderFixedBytes)) {
        hr = kE_Fail;
    } else {
        std::string error;
        const std::uint32_t tracks = header->audioStreamCount;
        const std::uint32_t bytes = xmv::kFileHeaderFixedBytes + xmv::kAudioTrackBytes * (tracks & 0xFFFF);
        if (!guest::IsReadable(A(header), bytes) ||
            !xmv::ParseFileHeader({reinterpret_cast<const std::uint8_t*>(header), bytes}, parsed, error)) {
            hr = kE_Fail;
            FUT_LOGW(Vid, "XMV: first packet header rejected (%s) - movie skipped", error.c_str());
        }
    }
    if (hr == kS_OK) {
        try {
            auto m = std::make_unique<Movie>();
            handle = static_cast<GuestHandle*>(xdk::AllocGuestObject(sizeof(GuestHandle)));
            if (handle == nullptr) {
                hr = kE_OutOfMemory;
            } else {
                id = g_nextId.fetch_add(1);
                handle->vtable = A(xdk::TrapVtable());
                handle->magic = kDecoderMagic;
                handle->id = id;
                m->handle = handle;
                m->id = id;
                m->header = parsed;
                m->source = std::make_unique<GamePacketSource>(A(context), A(getNextPacket),
                                                               A(releasePreviousPacket), parsed.maxPacketSize);
                m->core.Open(m->source.get(), parsed.firstPacketSize, 0);
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_movies[A(handle)] = std::move(m);
                }
                if (ppDecoder != nullptr) {
                    *ppDecoder = handle;
                }
                g_opened.fetch_add(1);
            }
        } catch (...) {
            if (handle != nullptr) {
                xdk::FreeGuestObject(handle);
                handle = nullptr;
            }
            hr = kE_OutOfMemory;
        }
    }
    if (hr == kS_OK) {
        std::string rates;
        for (const xmv::AudioTrackInfo& t : parsed.tracks) {
            if (rates.empty()) {
                rates = std::to_string(t.sampleRate) + " Hz " + std::to_string(t.channels) + " ch";
            }
        }
        FUT_LOGI(Vid, "XMV: movie #%u: %ux%u, %u ms, %u audio tracks%s%s, packets up to 0x%X bytes", id,
                 parsed.width, parsed.height, parsed.durationMs, static_cast<std::uint32_t>(parsed.tracks.size()),
                 rates.empty() ? "" : " of ", rates.c_str(), parsed.maxPacketSize);
    }
    XDK_TRACE_CALL(
        XMVDecoder_CreateDecoderForPackets,
        "(flags=0x%X, header=0x%08X, ctx=0x%08X, next=0x%08X, release=0x%08X, ppDecoder=0x%08X) -> 0x%08X, "
        "*pp=0x%08X",
        flags, A(header), A(context), A(getNextPacket), A(releasePreviousPacket), A(ppDecoder),
        static_cast<std::uint32_t>(hr), A(handle));
    return hr;
}
XDK_HOOK(XMVDecoder_CreateDecoderForPackets, &XMVDecoder_CreateDecoderForPacketsHle);

// XMV::XMVDecoder_CloseDecoder 0x264677 (name assumed) - stdcall (XMVDecoder*), void, ret 4. Only caller: movie Close
// 0xEE11F, only for player+0x18 != 0, result ignored. The original flushes its streams (FlushEx async), waits for
// their status and releases them, closes its file handle and frees the decoder (avi.md 3.1); the same here. A movie
// closed before its end is a skip (Intro::Update / FUN_000D5700 -> App slot 6, or the attract mode on input).
void __stdcall XMVDecoder_CloseDecoderHle(void* decoder) noexcept {
    std::unique_ptr<Movie> m;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_movies.find(A(decoder));
        if (it != g_movies.end()) {
            m = std::move(it->second);
            g_movies.erase(it);
        }
    }
    if (m != nullptr) {
        for (auto& t : m->tracks) {
            if (t->stream != nullptr) {
                snd::IDirectSoundStream_FlushExHle(t->stream, 0, snd::kFlushExAsync);
                snd::DsStreamRelease(t->stream);
                t->stream = nullptr;
            }
        }
        if (!m->ended && !m->failed) {
            g_closedEarly.fetch_add(1);
            LogClose(*m, m->started ? "closed early (skipped)" : "closed before its first frame");
        }
        m->handle->magic = 0;
        xdk::FreeGuestObject(m->handle);
    }
    XDK_TRACE_CALL(XMVDecoder_CloseDecoder, "(decoder=0x%08X)%s", A(decoder),
                   m != nullptr ? "" : " (unknown decoder)");
}
XDK_HOOK(XMVDecoder_CloseDecoder, &XMVDecoder_CloseDecoderHle);

// XMV::XMVDecoder_GetVideoDescriptor 0x264759 (name assumed) - stdcall (XMVDecoder*, desc*), void, ret 8.
// Original (decomp 0x264759): desc[0] = W, desc[1] = H, desc[3] = audio stream count, desc[2] untouched.
// Only caller: Open 0xEE389 with &player+0x1C; Open then uses +0x28 (count: tracks only for 1..5,
// 0xEE3D1..0xEE3E7) and +0x1C/+0x20 (texture size, 0xEE4AA). Values of the header passed to Create.
void __stdcall XMVDecoder_GetVideoDescriptorHle(void* decoder, XmvVideoDescriptor* desc) noexcept {
    const Movie* m = Find(decoder);
    if (desc != nullptr) {
        desc->width = m != nullptr ? m->header.width : 0;
        desc->height = m != nullptr ? m->header.height : 0;
        desc->audioStreamCount = m != nullptr ? static_cast<std::uint32_t>(m->header.tracks.size()) : 0;
    }
    XDK_TRACE_CALL(XMVDecoder_GetVideoDescriptor, "(decoder=0x%08X, desc=0x%08X) -> %ux%u, %u audio streams",
                   A(decoder), A(desc), m != nullptr ? m->header.width : 0u, m != nullptr ? m->header.height : 0u,
                   m != nullptr ? static_cast<std::uint32_t>(m->header.tracks.size()) : 0u);
}
XDK_HOOK(XMVDecoder_GetVideoDescriptor, &XMVDecoder_GetVideoDescriptorHle);

// XMV::XMVDecoder_EnableAudioStream 0x2647C6 (name assumed) - stdcall (XMVDecoder*, DWORD iStream, DWORD dwFlags,
// LPCDSMIXBINS, LPDIRECTSOUNDSTREAM* ppStream), HRESULT, ret 20. Only caller: Open 0xEE412 with (dec, track, 0, 0, 0):
// track = FUN_000EDBE0 (language: english 0, german 1, french 2, spanish 3, italian 4) for 5 tracks, else 0; result
// ignored. The original creates a DSOUND stream {flags, 2 packets, wfx (0x69), callback 0x264775, ctx = decoder}.
// Here: a DSOUND HLE stream (format 0x69 of the track, kAudioSlots packets, no callback - completion is seen through
// the status DWORDs), paused with Pause(2) until the first frame starts all streams with SynchPlayback.
std::int32_t __stdcall XMVDecoder_EnableAudioStreamHle(void* decoder, std::uint32_t stream,
                                                       std::uint32_t flags, const void* mixBins,
                                                       void** ppStream) noexcept {
    std::int32_t hr = kS_OK;
    void* created = nullptr;
    Movie* m = Find(decoder);
    if (m == nullptr || stream >= m->header.tracks.size() || m->started) {
        hr = kE_InvalidArg;
    } else if (TrackStream* existing = FindTrack(*m, stream)) {
        created = existing->stream;
    } else {
        try {
            const xmv::AudioTrackInfo& info = m->header.tracks[stream];
            auto t = std::make_unique<TrackStream>();
            t->track = stream;
            t->channels = info.channels;
            t->rate = info.sampleRate;
            t->wfx.wFormatTag = info.formatTag;
            t->wfx.nChannels = info.channels;
            t->wfx.nSamplesPerSec = info.sampleRate;
            t->wfx.nBlockAlign = static_cast<std::uint16_t>(info.BlockAlign());
            t->wfx.nAvgBytesPerSec = info.sampleRate / 64u * info.BlockAlign();
            t->wfx.wBitsPerSample = info.bitsPerSample;
            t->wfx.cbSize = 2;
            snd::XDsStreamDesc desc{};
            desc.dwFlags = flags & ~snd::kDsFlag3D;
            desc.dwMaxAttachedPackets = kAudioSlots;
            desc.lpwfxFormat = A(&t->wfx);
            desc.lpMixBins = A(mixBins);
            hr = snd::DirectSoundCreateStreamHle(&desc, &t->stream);
            if (hr >= 0 && t->stream != nullptr) {
                snd::IDirectSoundStream_PauseHle(t->stream, snd::kStreamPauseSynch);
                created = t->stream;
                m->core.SetAudioTrackMask(1u << stream);  // the game enables exactly one track
                m->tracks.push_back(std::move(t));
            } else {
                FUT_LOGW(Vid, "XMV: movie #%u: no DSOUND stream for audio track %u (format 0x%X, %u ch, %u Hz) - "
                         "silent, wall clock", m->id, stream, info.formatTag, info.channels, info.sampleRate);
                hr = hr < 0 ? hr : kE_Fail;
            }
        } catch (...) {
            hr = kE_OutOfMemory;
        }
    }
    if (ppStream != nullptr) {
        *ppStream = created;
        if (created != nullptr) {
            snd::DsStreamAddRef(created);
        }
    }
    XDK_TRACE_CALL(XMVDecoder_EnableAudioStream,
                   "(decoder=0x%08X, stream=%u, flags=0x%X, mixbins=0x%08X, ppStream=0x%08X) -> 0x%08X, stream 0x%08X",
                   A(decoder), stream, flags, A(mixBins), A(ppStream), static_cast<std::uint32_t>(hr), A(created));
    return hr;
}
XDK_HOOK(XMVDecoder_EnableAudioStream, &XMVDecoder_EnableAudioStreamHle);

// XMV::XMVDecoder_GetAudioStream 0x264943 (name assumed) - stdcall (XMVDecoder*, DWORD iStream, LPDIRECTSOUNDSTREAM*
// ppStream), void, ret 12 (original: *pp = streams[i] + AddRef). Only caller: Open 0xEE424 with a zeroed local; for a
// non-NULL stream it calls IDirectSoundStream_SetVolume(s, mB of volume category 0) 0xEE471 and s->Release()
// 0xEE47D (vtable slot 1) - both reach the DSOUND HLE. NULL for a track that is not enabled.
void __stdcall XMVDecoder_GetAudioStreamHle(void* decoder, std::uint32_t stream, void** ppStream) noexcept {
    void* s = nullptr;
    if (Movie* m = Find(decoder)) {
        if (const TrackStream* t = FindTrack(*m, stream)) {
            s = t->stream;
        }
    }
    if (s != nullptr) {
        snd::DsStreamAddRef(s);
    }
    if (ppStream != nullptr) {
        *ppStream = s;
    }
    XDK_TRACE_CALL(XMVDecoder_GetAudioStream, "(decoder=0x%08X, stream=%u, ppStream=0x%08X) -> *pp=0x%08X",
                   A(decoder), stream, A(ppStream), A(s));
}
XDK_HOOK(XMVDecoder_GetAudioStream, &XMVDecoder_GetAudioStreamHle);

// XMV::XMVDecoder_GetNextFrame 0x264962 (name assumed) - stdcall (XMVDecoder*, D3DSurface*, XMVRESULT* pResult,
// DWORD* pTimestamp), HRESULT, ret 16. Only caller: Update 0xEDEDB (once at the end of Open and once per frame) with
// the texture's level-0 surface (GetSurfaceLevel2, released right after), &local, 0; the HRESULT is ignored.
// *pResult: 1 = new frame (Update clears player+0x41 and draws), 0 = none due (draws the previous frame; before the
// first frame it clears instead), 2 = end, 3 = error (both set the done flag 0xEDEEA; App slot 7 destroys the
// player). The original drives DSOUND itself (DoWork, Pause(2) + SynchPlayback) and writes the frame through
// D3DSurface_LockRect (FUN_00265B0B); here the YUY2 frame goes straight into the shim surface's pixels.
std::int32_t __stdcall XMVDecoder_GetNextFrameHle(void* decoder, void* surface, std::int32_t* result,
                                                  std::uint32_t* timestamp) noexcept {
    std::int32_t res = kXmvFailure;
    Movie* m = Find(decoder);
    try {
        if (m == nullptr) {
            res = kXmvFailure;
        } else if (m->ended) {
            res = kXmvEndOfFile;
        } else if (m->failed) {
            res = kXmvFailure;
        } else {
            snd::DirectSoundDoWorkHle();  // finished audio packets -> free slots (the original calls DoWork too)
            if (!m->started) {
                if (!StartPlayback(*m)) {
                    Fail(*m, &res, "start");
                } else {
                    std::uint32_t pts = 0;
                    if (DecodeDue(*m, 0, true, pts) == 0) {
                        if (m->core.Failed()) {
                            if (!g_loggedDecoderMissing.exchange(true)) {
                                FUT_LOGE(Vid, "XMV: WMV2 decoder unavailable: %s", m->core.Error().c_str());
                            }
                            Fail(*m, &res, "decoder");
                        } else {
                            m->ended = true;  // no picture at all
                            g_finished.fetch_add(1);
                            res = kXmvEndOfFile;
                        }
                    } else {
                        WritePicture(*m, surface);
                        Account(*m, pts, 0, 0);
                        res = kXmvNewFrame;
                        if (timestamp != nullptr) {
                            *timestamp = pts;
                        }
                    }
                }
            } else {
                const std::uint64_t now = ClockNow(*m);
                ReadAhead(*m, now);
                SubmitAudio(*m);
                if (m->core.Failed()) {
                    Fail(*m, &res, "demux");
                } else if (!m->core.Video().empty() && m->core.Video().front().ptsMs <= now) {
                    std::uint32_t pts = 0;
                    const std::uint32_t decoded = DecodeDue(*m, now, false, pts);
                    if (decoded == 0 && m->core.Failed()) {
                        Fail(*m, &res, "decoder");
                    } else if (decoded == 0) {
                        res = kXmvNoFrame;  // rejected frames: keep the previous picture
                    } else {
                        WritePicture(*m, surface);
                        Account(*m, pts, now, decoded - 1);
                        res = kXmvNewFrame;
                        if (timestamp != nullptr) {
                            *timestamp = pts;
                        }
                    }
                } else if (m->core.Eof() && m->core.Video().empty() &&
                           now >= MovieEndMs(m->core.Stats().lastPts, m->core.Stats().lastFrameGapMs,
                                             AudioEndMs(*m))) {
                    m->ended = true;
                    g_finished.fetch_add(1);
                    LogClose(*m, "ended");
                    res = kXmvEndOfFile;
                } else {
                    res = kXmvNoFrame;
                }
            }
        }
    } catch (...) {
        if (m != nullptr) {
            Fail(*m, &res, "out of memory");
        }
        res = kXmvFailure;
    }
    if (result != nullptr) {
        *result = res;
    }
    XDK_TRACE_CALL(XMVDecoder_GetNextFrame,
                   "(decoder=0x%08X, surface=0x%08X, pResult=0x%08X, pTimestamp=0x%08X) -> S_OK, *pResult=%d",
                   A(decoder), A(surface), A(result), A(timestamp), res);
    return kS_OK;
}
XDK_HOOK(XMVDecoder_GetNextFrame, &XMVDecoder_GetNextFrameHle);

// XMV::XMVDecoder_GetPlaybackTimeMs 0x264FDF (name assumed) - stdcall (XMVDecoder*), DWORD ms, ret 4. Only caller:
// Update 0xEDFB7 for subtitles (player+0x44; there are no Video/*.txt on the PAL disc, avi.md 3.2), reached only for
// results 0/1. The original returns ms since the playback start (QPC), 0 before it. The movie clock here.
std::uint32_t __stdcall XMVDecoder_GetPlaybackTimeMsHle(void* decoder) noexcept {
    const Movie* m = Find(decoder);
    const std::uint32_t ms = m != nullptr ? static_cast<std::uint32_t>(m->clock.LastMs()) : 0u;
    XDK_TRACE_CALL(XMVDecoder_GetPlaybackTimeMs, "(decoder=0x%08X) -> %u", A(decoder), ms);
    return ms;
}
XDK_HOOK(XMVDecoder_GetPlaybackTimeMs, &XMVDecoder_GetPlaybackTimeMsHle);

}  // namespace vid
