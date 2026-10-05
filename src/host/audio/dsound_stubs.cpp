// DSOUND stubs (M2-M4): silent replacements of the 40 DSOUND entry points (see dsound_stubs.h for the
// summary of returned values). Every function below documents address, convention, arguments and the
// game code it was checked against. Disassembly references are VAs of the retail EU XBE; "voice" is
// the game's SoundVoice (0xBC bytes, vtable 0x2A3B18: +0x74 IDirectSoundStream*, +0x7C..+0xA0 packet
// status DWORDs, +0xA4 IDirectSoundBuffer*), "mgr" the SoundManager (0x7C bytes, +0x48 IDirectSound*),
// see avi.md 2.4.1.
#include "audio/dsound_stubs.h"

#include "audio/xdk_entries.h"
#include "core/config.h"
#include "core/guest.h"
#include "core/log.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <utility>

namespace snd {
namespace {

std::uint32_t A(const volatile void* p) noexcept {
    return guest::Addr(p);
}

// ---- dummy IDirectSound -------------------------------------------------------------------------
// Guest-visible part of the device handle: +0 trap vtable (the game never calls it virtually).
struct DummyDevice {
    std::uint32_t vtable;
    std::uint32_t magic;
};
constexpr std::uint32_t kDeviceMagic = 0x56534458;  // 'XDSV'
static_assert(sizeof(DummyDevice) <= xdk::kGuestObjectSize);

std::mutex g_deviceMutex;
DummyDevice* g_device = nullptr;
std::uint32_t g_deviceRefs = 0;

// ---- stub streams ([audio] dummy_streams = 1) ---------------------------------------------------
struct GuestStream {  // guest-visible: +0 vtable (XMediaObject layout), the rest is host bookkeeping
    std::uint32_t vtable;
    std::uint32_t magic;
};
constexpr std::uint32_t kStreamMagic = 0x52545358;  // 'XSTR'
static_assert(sizeof(GuestStream) <= xdk::kGuestObjectSize);

struct QueuedPacket {
    std::uint32_t pdwStatus;
    std::uint32_t pdwCompletedSize;
    std::uint32_t size;
    std::int64_t durationQpc;
};
struct StreamState {
    std::uint32_t refs = 1;
    std::uint32_t maxPackets = 1;
    double bytesPerSecond = 0;  // 0 = complete packets at the next pump
    bool paused = false;
    bool synchPaused = false;
    std::int64_t headStart = 0;  // QPC when the head packet started "playing"
    std::deque<QueuedPacket> queue;
    XWaveFormatEx format{};
    std::uint32_t submitted = 0;  // packets accepted by Process
    std::uint32_t completed = 0;  // packets completed with kPacketSuccess
};

// Process-wide stub stream statistics (GetStubStreamStatistics()).
std::atomic<std::uint32_t> g_streamsCreated{0};
std::atomic<std::uint32_t> g_packetsSubmitted{0};
std::atomic<std::uint32_t> g_packetsCompleted{0};
std::atomic<std::uint32_t> g_packetsFlushed{0};
std::atomic<std::uint32_t> g_packetsRejected{0};

std::mutex g_streamMutex;
std::map<std::uint32_t, StreamState>& Streams() {
    static std::map<std::uint32_t, StreamState> streams;
    return streams;
}
std::atomic<int> g_dummyStreams{-1};  // -1 = not read from the configuration yet

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

// Playback rate of a stream format. PCM: rate * nBlockAlign; Xbox ADPCM (0x69): 64 samples per
// 36-byte block and channel (avi.md 2.4.3: the game's nAvgBytesPerSec of ADPCM formats is wrong).
double BytesPerSecond(const XWaveFormatEx& f) noexcept {
    if (f.wFormatTag == kWaveFormatXboxAdpcm) {
        return static_cast<double>(f.nSamplesPerSec) * 36.0 * f.nChannels / 64.0;
    }
    if (f.wFormatTag == kWaveFormatPcm && f.nBlockAlign != 0) {
        return static_cast<double>(f.nSamplesPerSec) * f.nBlockAlign;
    }
    return static_cast<double>(f.nAvgBytesPerSec);
}

void WriteGuestDword(std::uint32_t address, std::uint32_t value) noexcept {
    if (address != 0) {
        guest::Write<std::uint32_t>(address, value);
    }
}

// Completes the head packet of `s` (status, completed size). Caller holds g_streamMutex.
void CompleteHead(StreamState& s, std::uint32_t status) noexcept {
    const QueuedPacket p = s.queue.front();
    s.queue.pop_front();
    WriteGuestDword(p.pdwCompletedSize, status == kPacketSuccess ? p.size : 0);
    WriteGuestDword(p.pdwStatus, status);
    if (status == kPacketSuccess) {
        if (s.completed++ == 0) {
            FUT_LOGD(Snd, "DSOUND: stub stream: first packet completed (%u bytes; %u submitted so far)",
                     p.size, s.submitted);
        }
        g_packetsCompleted.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_packetsFlushed.fetch_add(1, std::memory_order_relaxed);
    }
}

void FlushQueue(StreamState& s) noexcept {
    while (!s.queue.empty()) {
        CompleteHead(s, kPacketFlushed);
    }
}

std::uint32_t PumpOne(StreamState& s, std::int64_t now) noexcept {
    std::uint32_t done = 0;
    while (!s.paused && !s.queue.empty() && now - s.headStart >= s.queue.front().durationQpc) {
        s.headStart += s.queue.front().durationQpc;
        CompleteHead(s, kPacketSuccess);
        ++done;
    }
    if (s.queue.empty()) {
        s.headStart = now;
    }
    return done;
}

StreamState* FindStream(const void* p) noexcept {
    auto& streams = Streams();
    const auto it = streams.find(A(p));
    return it != streams.end() ? &it->second : nullptr;
}

std::uint32_t StreamStatusBits(const StreamState& s) noexcept {
    std::uint32_t status = 0;
    if (s.queue.size() < s.maxPackets) {
        status |= kStreamStatusReady;
    }
    if (s.paused) {
        status |= kStreamStatusPaused;
    } else if (!s.queue.empty()) {
        status |= kStreamStatusPlaying;
    }
    return status;
}

// ---- XMediaObject methods of a stub stream (vtable slots, `this` on the stack) ------------------
// Slot order and purges: guest vtable 0x2A5F5C (generated xdk::stream::*, verified against the XBE).
// Only reachable with [audio] dummy_streams = 1. Callers: Release - voice destructor 0xCC107 and movie
// player 0xEE47D; Process - stream pump 0xCB6B7; AddRef/GetStatus/Discontinuity/GetInfo/Flush - only
// XMV (replaced) or nobody.

// XMediaObject::AddRef - slot 0, stdcall(this), ULONG, ret 4.
std::uint32_t __stdcall StreamAddRef(void* self) noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    StreamState* s = FindStream(self);
    return s != nullptr ? ++s->refs : 0;
}

// XMediaObject::Release - slot 1, stdcall(this), ULONG, ret 4. The last reference flushes queued
// packets (status kPacketFlushed while the owner is still alive: the voice destructor 0xCC030 releases
// the stream at 0xCC107 before freeing the voice) and frees the guest object.
std::uint32_t __stdcall StreamRelease(void* self) noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    StreamState* s = FindStream(self);
    if (s == nullptr) {
        return 0;
    }
    if (--s->refs != 0) {
        return s->refs;
    }
    FlushQueue(*s);
    Streams().erase(A(self));
    xdk::FreeGuestObject(self);
    return 0;
}

// XMediaObject::GetInfo - slot 2, stdcall(this, XMEDIAINFO*), HRESULT, ret 8. Not called by the game.
// ASSUMPTION: dwInputSize = one block of the format (XDK: minimum input packet size).
std::int32_t __stdcall StreamGetInfo(void* self, XMediaInfo* info) noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    const StreamState* s = FindStream(self);
    if (info != nullptr) {
        std::memset(info, 0, sizeof(*info));
        info->dwInputSize = s != nullptr ? s->format.nBlockAlign : 0;
    }
    return kS_OK;
}

// XMediaObject::GetStatus - slot 3, stdcall(this, DWORD*), HRESULT, ret 8. XMV CloseDecoder spins while
// bit 0x10000 is set (avi.md 2.3); after Flush the queue is empty, so the bit is clear at once.
std::int32_t __stdcall StreamGetStatus(void* self, std::uint32_t* status) noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    StreamState* s = FindStream(self);
    if (s != nullptr) {
        PumpOne(*s, Qpc());
    }
    if (status != nullptr) {
        *status = s != nullptr ? StreamStatusBits(*s) : 0;
    }
    return kS_OK;
}

// XMediaObject::Process - slot 4, stdcall(this, const XMEDIAPACKET* in, const XMEDIAPACKET* out), HRESULT,
// ret 12. Game: pump 0xCB5B0 submits `Process(&pkt, NULL)` (0xCB6AB..0xCB6B7) for every slot whose
// status is not E_PENDING and never writes E_PENDING itself (avi.md 2.3), so the status is set
// synchronously like the original (0x24B1D9) and completed after the packet's play time. A full queue
// returns DSERR_INVALIDCALL without touching the status (original behaviour, 0x88780032).
std::int32_t __stdcall StreamProcess(void* self, const XMediaPacket* in, const XMediaPacket* out) noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    StreamState* s = FindStream(self);
    if (s == nullptr || in == nullptr) {
        return kE_InvalidArg;
    }
    static_cast<void>(out);  // render stream: no output packet (the game passes NULL)
    const std::int64_t now = Qpc();
    PumpOne(*s, now);
    if (s->queue.size() >= s->maxPackets) {
        g_packetsRejected.fetch_add(1, std::memory_order_relaxed);
        return kDsErrInvalidCall;
    }
    QueuedPacket p{in->pdwStatus, in->pdwCompletedSize, in->dwMaxSize, 0};
    if (s->bytesPerSecond > 0) {
        p.durationQpc = static_cast<std::int64_t>(static_cast<double>(p.size) * static_cast<double>(Qpf()) /
                                                  s->bytesPerSecond);
    }
    if (s->queue.empty()) {
        s->headStart = now;
    }
    try {
        s->queue.push_back(p);
    } catch (...) {
        return static_cast<std::int32_t>(0x8007000E);  // E_OUTOFMEMORY, status untouched
    }
    WriteGuestDword(p.pdwCompletedSize, 0);
    WriteGuestDword(p.pdwStatus, kPacketPending);
    g_packetsSubmitted.fetch_add(1, std::memory_order_relaxed);
    if (s->submitted++ == 0) {
        FUT_LOGD(
            Snd,
            "DSOUND: stub stream 0x%08X: first packet %u bytes (%.0f bytes/s -> %.0f ms), status at 0x%08X",
            A(self), p.size, s->bytesPerSecond,
            1000.0 * static_cast<double>(p.durationQpc) / static_cast<double>(Qpf()), p.pdwStatus);
    }
    return kS_OK;
}

// XMediaObject::Discontinuity - slot 5, stdcall(this), HRESULT, ret 4. XMV only; nothing to do.
std::int32_t __stdcall StreamDiscontinuity(void* self) noexcept {
    static_cast<void>(self);
    return kS_OK;
}

// XMediaObject::Flush - slot 6, stdcall(this), HRESULT, ret 4. Not called by the game.
std::int32_t __stdcall StreamFlush(void* self) noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    if (StreamState* s = FindStream(self)) {
        FlushQueue(*s);
    }
    return kS_OK;
}

#define SND_CHECK_STREAM_METHOD(Method, fn)                                                                  \
    static_assert(::xdk::detail::StdcallFnOf<decltype(fn)>::valid &&                                         \
                      ::xdk::detail::StdcallFnOf<decltype(fn)>::purgeBytes ==                                \
                          ::xdk::stream::Method::purgeBytes,                                                 \
                  "stream method " #Method ": convention/purge differs from vtable 0x2A5F5C")
SND_CHECK_STREAM_METHOD(AddRef, &StreamAddRef);
SND_CHECK_STREAM_METHOD(Release, &StreamRelease);
SND_CHECK_STREAM_METHOD(GetInfo, &StreamGetInfo);
SND_CHECK_STREAM_METHOD(GetStatus, &StreamGetStatus);
SND_CHECK_STREAM_METHOD(Process, &StreamProcess);
SND_CHECK_STREAM_METHOD(Discontinuity, &StreamDiscontinuity);
SND_CHECK_STREAM_METHOD(Flush, &StreamFlush);
#undef SND_CHECK_STREAM_METHOD

// Stream vtable in guest memory: 7 XMediaObject methods, the remaining slots trap.
std::uint32_t StreamVtable() noexcept {
    static const std::uint32_t vtable = []() noexcept -> std::uint32_t {
        auto* table = static_cast<std::uint32_t*>(xdk::AllocGuestObject(xdk::kGuestObjectSize));
        if (table == nullptr) {
            return 0;
        }
        const void* const* trap = xdk::TrapVtable();
        for (std::uint32_t i = 0; i < xdk::kGuestObjectSize / 4; ++i) {
            table[i] = A(trap[i % xdk::kTrapVtableSlots]);
        }
        table[xdk::stream::AddRef::slot] = A(core::detail::CodeAddress(&StreamAddRef));
        table[xdk::stream::Release::slot] = A(core::detail::CodeAddress(&StreamRelease));
        table[xdk::stream::GetInfo::slot] = A(core::detail::CodeAddress(&StreamGetInfo));
        table[xdk::stream::GetStatus::slot] = A(core::detail::CodeAddress(&StreamGetStatus));
        table[xdk::stream::Process::slot] = A(core::detail::CodeAddress(&StreamProcess));
        table[xdk::stream::Discontinuity::slot] = A(core::detail::CodeAddress(&StreamDiscontinuity));
        table[xdk::stream::Flush::slot] = A(core::detail::CodeAddress(&StreamFlush));
        return A(table);
    }();
    return vtable;
}

void* CreateStubStream(const XDsStreamDesc& desc) noexcept {
    const std::uint32_t vtable = StreamVtable();
    auto* obj = static_cast<GuestStream*>(xdk::AllocGuestObject(sizeof(GuestStream)));
    if (vtable == 0 || obj == nullptr) {
        xdk::FreeGuestObject(obj);
        return nullptr;
    }
    obj->vtable = vtable;
    obj->magic = kStreamMagic;
    try {
        StreamState s;
        s.maxPackets = desc.dwMaxAttachedPackets != 0 ? desc.dwMaxAttachedPackets : 1;
        if (desc.lpwfxFormat != 0 && guest::IsReadable(desc.lpwfxFormat, sizeof(XWaveFormatEx))) {
            std::memcpy(&s.format, guest::Ptr<const void>(desc.lpwfxFormat), sizeof(s.format));
        }
        s.bytesPerSecond = BytesPerSecond(s.format);
        s.headStart = Qpc();
        std::lock_guard<std::mutex> lock(g_streamMutex);
        Streams()[A(obj)] = std::move(s);
        g_streamsCreated.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        xdk::FreeGuestObject(obj);
        return nullptr;
    }
    return obj;
}

bool ReadDummyStreamsConfig() noexcept {
    int v = g_dummyStreams.load();
    if (v < 0) {
        bool enabled = false;
        try {
            enabled = core::GetConfig().GetBool("audio", "dummy_streams", false);
        } catch (...) {
        }
        v = enabled ? 1 : 0;
        int expected = -1;
        g_dummyStreams.compare_exchange_strong(expected, v);
        v = g_dummyStreams.load();
    }
    return v == 1;
}

// Applies Pause(0/1/2) to a stub stream (no-op for NULL / unknown handles).
void PauseStubStream(void* stream, std::uint32_t pause) noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    StreamState* s = FindStream(stream);
    if (s == nullptr) {
        return;
    }
    const std::int64_t now = Qpc();
    PumpOne(*s, now);
    const bool wasPaused = s->paused;
    s->paused = pause != kStreamResume;
    s->synchPaused = pause == kStreamPauseSynch;
    if (wasPaused && !s->paused) {
        s->headStart = now;  // the head packet restarts (stub timing only)
    }
}

}  // namespace

// ---- host-side helpers ----------------------------------------------------------------------------
void SetDummyStreams(bool enabled) noexcept {
    g_dummyStreams.store(enabled ? 1 : 0);
}

bool DummyStreamsEnabled() noexcept {
    return ReadDummyStreamsConfig();
}

bool IsDummyDevice(const void* p) noexcept {
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    return p != nullptr && p == g_device;
}

bool IsStubStream(const void* p) noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    return p != nullptr && FindStream(p) != nullptr;
}

std::uint32_t StubStreamCount() noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    return static_cast<std::uint32_t>(Streams().size());
}

std::uint32_t StubStreamQueuedPackets(const void* stream) noexcept {
    std::lock_guard<std::mutex> lock(g_streamMutex);
    const StreamState* s = FindStream(stream);
    return s != nullptr ? static_cast<std::uint32_t>(s->queue.size()) : 0;
}

StubStreamStatistics GetStubStreamStatistics() noexcept {
    StubStreamStatistics st;
    st.created = g_streamsCreated.load();
    st.live = StubStreamCount();
    st.packetsSubmitted = g_packetsSubmitted.load();
    st.packetsCompleted = g_packetsCompleted.load();
    st.packetsFlushed = g_packetsFlushed.load();
    st.packetsRejected = g_packetsRejected.load();
    return st;
}

std::uint32_t PumpStubStreams(std::int64_t nowQpc) noexcept {
    const std::int64_t now = nowQpc != 0 ? nowQpc : Qpc();
    std::lock_guard<std::mutex> lock(g_streamMutex);
    std::uint32_t done = 0;
    for (auto& [address, s] : Streams()) {
        done += PumpOne(s, now);
    }
    return done;
}

// =================================================================================================
// The 40 entry points (alphabetical by object: IDirectSound, IDirectSoundBuffer, IDirectSoundStream,
// global functions). All __stdcall, `ret N` = generated purge (static_assert in XDK_HOOK). Callers and
// sites: avi.md 2.2 table, avi_rawcalls.out; "result ignored" = the instruction after the call does not
// read eax (checked at every site).
// =================================================================================================

// ---- creation ------------------------------------------------------------------------------------

// DSOUND::DirectSoundCreate 0x24935C - stdcall (LPCGUID, LPDIRECTSOUND* ppDS, LPUNKNOWN), HRESULT, ret 12.
// Game: SoundManager ctor 0xCCD41 DirectSoundCreate(0, &mgr+0x48, 0); `test eax,eax; jl 0xCCE0A` skips
// the dsstdfx.bin load and the 3D setup on failure, so S_OK keeps the original init path (and its file
// accesses). The handle must be non-NULL: SoundManager::Update 0xCBCB0 returns at once when +0x48 == 0
// (0xCBCBC..0xCBCC7) and would then also skip the voice/data update FUN_000C95F0 (0xCBE8C) that ends
// finished voices (GetStatus = 0 -> voice+0x20 = 1 at 0xCC63E) and deletes released SoundData (0xC9617).
// The handle only reaches C functions (all reads of +0x48 in 0xC9000..0xCD000 push it to a DSOUND entry:
// 0xCBC7E Release, 0xCBE58/0xCBE7F/0xCBE91 Update, 0xCBF49 DownloadEffectsImage, 0xCBFB0..0xCBFFC
// SetI3DL2Listener, 0xCCD69..0xCCDBD init), so its vtable traps. Singleton with a reference count like
// the original (FUN_002492EF / DAT_002641A0, avi.md 2.3). XMV 0x264962 (replaced) is the other caller.
std::int32_t __stdcall DirectSoundCreateHle(const void* deviceGuid, void** ppDirectSound,
                                            void* unkOuter) noexcept {
    if (xdk::detail::InPurgeProbe()) {
        return kS_OK;  // self-test XdkEntryPurgeMeasured: no device, no reference
    }
    std::int32_t hr = kS_OK;
    void* device = nullptr;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        if (g_device == nullptr) {
            g_device = static_cast<DummyDevice*>(xdk::AllocGuestObject(sizeof(DummyDevice)));
            if (g_device != nullptr) {
                g_device->vtable = A(xdk::TrapVtable());
                g_device->magic = kDeviceMagic;
                first = true;
            }
        }
        if (g_device != nullptr) {
            ++g_deviceRefs;
            device = g_device;
        } else {
            hr = static_cast<std::int32_t>(0x8007000E);  // E_OUTOFMEMORY (game: jl -> no 3D init)
        }
    }
    if (ppDirectSound != nullptr) {
        *ppDirectSound = device;
    }
    if (first) {
        FUT_LOGI(Snd, "DSOUND: stub device 0x%08X (no audio output until M5; %s streams)", A(device),
                 ReadDummyStreamsConfig() ? "stub" : "NULL");
    }
    XDK_TRACE_CALL(DirectSoundCreate, "(guid=0x%08X, ppDS=0x%08X, outer=0x%08X) -> 0x%08X, *ppDS=0x%08X",
                   A(deviceGuid), A(ppDirectSound), A(unkOuter), static_cast<std::uint32_t>(hr), A(device));
    return hr;
}
XDK_HOOK(DirectSoundCreate, &DirectSoundCreateHle);

// DSOUND::DirectSoundCreateBuffer 0x2493A3 - stdcall (LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*), HRESULT,
// ret 8. Only caller: voice create 0xCC150 (0xCC296), result ignored (0xCC29B), *pp stored at voice+0xA4.
// Returns S_OK with *ppBuffer = NULL (design 2.1/6 M2, avi.md 6 step 1 "einfachere Variante"). NULL is
// safe at every use of voice+0xA4: SetBufferData/SetMixBins/SetRolloffCurve 0xCC2AF/0xCC35A/0xCC36D,
// Play/3D/volume/GetStatus in the voice update 0xCC5E0 (stubs accept NULL; GetStatus writes 0), Stop
// 0xCB570 and Reset 0xCB58E test for NULL, the Play variants 0xCC3CB/0xCC44B/0xCC512 test for NULL,
// the destructor 0xCC030 skips SetBufferData (0xCC06A), the GetStatus spin (0xCC0B1) and Release
// (0xCC0EF) for NULL.
std::int32_t __stdcall DirectSoundCreateBufferHle(const XDsBufferDesc* desc, void** ppBuffer) noexcept {
    if (ppBuffer != nullptr) {
        *ppBuffer = nullptr;
    }
    XDK_TRACE_CALL(
        DirectSoundCreateBuffer, "(desc=0x%08X flags=0x%08X fmt=0x%08X, ppBuffer=0x%08X) -> S_OK, *pp=NULL",
        A(desc), desc != nullptr ? desc->dwFlags : 0u, desc != nullptr ? desc->lpwfxFormat : 0u, A(ppBuffer));
    return kS_OK;
}
XDK_HOOK(DirectSoundCreateBuffer, &DirectSoundCreateBufferHle);

// DSOUND::DirectSoundCreateStream 0x2493FA - stdcall (LPCDSSTREAMDESC, LPDIRECTSOUNDSTREAM*), HRESULT,
// ret 8. Game caller: voice create 0xCC1C3 (result ignored, 0xCC1C8), *pp stored at voice+0x74; XMV
// EnableAudioStream 0x2647C6 (replaced). Default: S_OK, *ppStream = NULL (design 2.1/6 M2). Every use
// of voice+0x74 tolerates NULL: the pump 0xCB5B6 and the destructor 0xCC100 (virtual Release) test it,
// Stop 0xCB55C and the Play variants 0xCC3B3/0xCC433/0xCC4F9 test it, the remaining calls go to stubs
// (SetMixBins/SetRolloffCurve 0xCC32C/0xCC33C, voice update 0xCC711..0xCC8C4). The movie player's
// stream comes from XMV GetAudioStream (stub: NULL, tested at 0xEE429).
// [audio] dummy_streams = 1: S_OK with a stub stream (XMediaObject vtable, packets complete after their
// play time) - exercises the original streaming pump (fiber file reads) without audio output.
std::int32_t __stdcall DirectSoundCreateStreamHle(const XDsStreamDesc* desc, void** ppStream) noexcept {
    void* stream = nullptr;
    if (desc != nullptr && ReadDummyStreamsConfig()) {
        stream = CreateStubStream(*desc);
    }
    if (ppStream != nullptr) {
        *ppStream = stream;
    }
    XDK_TRACE_CALL(DirectSoundCreateStream,
                   "(desc=0x%08X flags=0x%08X packets=%u fmt=0x%08X callback=0x%08X, ppStream=0x%08X) -> "
                   "S_OK, *pp=0x%08X",
                   A(desc), desc != nullptr ? desc->dwFlags : 0u,
                   desc != nullptr ? desc->dwMaxAttachedPackets : 0u,
                   desc != nullptr ? desc->lpwfxFormat : 0u, desc != nullptr ? desc->lpfnCallback : 0u,
                   A(ppStream), A(stream));
    return kS_OK;
}
XDK_HOOK(DirectSoundCreateStream, &DirectSoundCreateStreamHle);

// ---- IDirectSound (device handle from DirectSoundCreate) ------------------------------------------

// DSOUND::IDirectSound_Release 0x246E7D - stdcall (LPDIRECTSOUND), ULONG, ret 4. Game: SoundManager dtor
// 0xCBC7F (only if +0x48 != 0, result ignored). Reference count only; the dummy device is never freed
// (TSV: "never tear down the device (singleton)"; XMV creates/releases it per sync).
std::uint32_t __stdcall IDirectSound_ReleaseHle(void* ds) noexcept {
    std::uint32_t refs = 0;
    {
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        if (ds != nullptr && ds == g_device && g_deviceRefs > 0) {
            refs = --g_deviceRefs;
        }
    }
    XDK_TRACE_CALL(IDirectSound_Release, "(ds=0x%08X) -> %u", A(ds), refs);
    return refs;
}
XDK_HOOK(IDirectSound_Release, &IDirectSound_ReleaseHle);

// DSOUND::IDirectSound_DownloadEffectsImage 0x247911 - stdcall (LPDIRECTSOUND, LPCVOID pvImage, DWORD
// dwSize, LPCDSEFFECTIMAGELOC, LPDSEFFECTIMAGEDESC* ppDesc), HRESULT, ret 20. Only caller FUN_000CBEB0
// 0xCBF67 with the bytes of XBOX\dsstdfx.bin, loc {0, 1} and ppDesc = &local; the result and *ppDesc
// are never read and the image is freed right after (0xCBF6C, avi.md 2.1). S_OK, *ppDesc = NULL.
std::int32_t __stdcall IDirectSound_DownloadEffectsImageHle(void* ds, const void* image, std::uint32_t size,
                                                            const void* imageLoc,
                                                            std::uint32_t* ppImageDesc) noexcept {
    if (ppImageDesc != nullptr) {
        *ppImageDesc = 0;
    }
    XDK_TRACE_CALL(IDirectSound_DownloadEffectsImage,
                   "(ds=0x%08X, image=0x%08X, size=%u, loc=0x%08X, ppDesc=0x%08X) -> S_OK", A(ds), A(image),
                   size, A(imageLoc), A(ppImageDesc));
    return kS_OK;
}
XDK_HOOK(IDirectSound_DownloadEffectsImage, &IDirectSound_DownloadEffectsImageHle);

// DSOUND::IDirectSound_SynchPlayback 0x247938 - stdcall (LPDIRECTSOUND), HRESULT, ret 4. Only XMV
// GetNextFrame 0x264E1B (replaced). Starts stub streams paused with Pause(2) (avi.md 2.5). S_OK.
std::int32_t __stdcall IDirectSound_SynchPlaybackHle(void* ds) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_streamMutex);
        const std::int64_t now = Qpc();
        for (auto& [address, s] : Streams()) {
            if (s.synchPaused) {
                s.paused = false;
                s.synchPaused = false;
                s.headStart = now;
            }
        }
    }
    XDK_TRACE_CALL(IDirectSound_SynchPlayback, "(ds=0x%08X) -> S_OK", A(ds));
    return kS_OK;
}
XDK_HOOK(IDirectSound_SynchPlayback, &IDirectSound_SynchPlaybackHle);

// DSOUND::IDirectSound_CommitDeferredSettings 0x248380 - stdcall (LPDIRECTSOUND), HRESULT, ret 4.
// SoundManager::Update 0xCBE95 once per frame, result ignored. No 3D mixer -> S_OK.
std::int32_t __stdcall IDirectSound_CommitDeferredSettingsHle(void* ds) noexcept {
    XDK_TRACE_CALL(IDirectSound_CommitDeferredSettings, "(ds=0x%08X) -> S_OK", A(ds));
    return kS_OK;
}
XDK_HOOK(IDirectSound_CommitDeferredSettings, &IDirectSound_CommitDeferredSettingsHle);

// DSOUND::IDirectSound_SetDistanceFactor 0x248D60 - stdcall (ds, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Init 0xCCD98 (0.2, deferred), result ignored. S_OK.
std::int32_t __stdcall IDirectSound_SetDistanceFactorHle(void* ds, float factor,
                                                         std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSound_SetDistanceFactor, "(ds=0x%08X, %g, apply=%u) -> S_OK", A(ds), factor, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetDistanceFactor, &IDirectSound_SetDistanceFactorHle);

// DSOUND::IDirectSound_SetDopplerFactor 0x248D84 - stdcall (ds, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Init 0xCCD73 (2.0, immediate), result ignored. S_OK.
std::int32_t __stdcall IDirectSound_SetDopplerFactorHle(void* ds, float factor,
                                                        std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSound_SetDopplerFactor, "(ds=0x%08X, %g, apply=%u) -> S_OK", A(ds), factor, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetDopplerFactor, &IDirectSound_SetDopplerFactorHle);

// DSOUND::IDirectSound_SetOrientation 0x248DA8 - stdcall (ds, FLOAT xFront, yFront, zFront, xTop, yTop,
// zTop, DWORD dwApply), HRESULT, ret 32. SoundManager::Update 0xCBE85 per frame, result ignored. S_OK.
std::int32_t __stdcall IDirectSound_SetOrientationHle(void* ds, float xFront, float yFront, float zFront,
                                                      float xTop, float yTop, float zTop,
                                                      std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSound_SetOrientation,
                   "(ds=0x%08X, front=(%g, %g, %g), top=(%g, %g, %g), apply=%u) -> S_OK", A(ds), xFront,
                   yFront, zFront, xTop, yTop, zTop, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetOrientation, &IDirectSound_SetOrientationHle);

// DSOUND::IDirectSound_SetPosition 0x248DF2 - stdcall (ds, FLOAT x, y, z, DWORD dwApply), HRESULT, ret 20.
// Init 0xCCDB8 and Update 0xCBE42 (listener position), result ignored. S_OK.
std::int32_t __stdcall IDirectSound_SetPositionHle(void* ds, float x, float y, float z,
                                                   std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSound_SetPosition, "(ds=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(ds), x, y, z,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetPosition, &IDirectSound_SetPositionHle);

// DSOUND::IDirectSound_SetRolloffFactor 0x248E27 - stdcall (ds, FLOAT, DWORD dwApply), HRESULT, ret 12.
// Init 0xCCDA7 (1.0), result ignored. S_OK.
std::int32_t __stdcall IDirectSound_SetRolloffFactorHle(void* ds, float factor,
                                                        std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSound_SetRolloffFactor, "(ds=0x%08X, %g, apply=%u) -> S_OK", A(ds), factor, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetRolloffFactor, &IDirectSound_SetRolloffFactorHle);

// DSOUND::IDirectSound_SetVelocity 0x248E4B - stdcall (ds, FLOAT x, y, z, DWORD dwApply), HRESULT, ret 20.
// Update 0xCBE5C per frame, result ignored. S_OK.
std::int32_t __stdcall IDirectSound_SetVelocityHle(void* ds, float x, float y, float z,
                                                   std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSound_SetVelocity, "(ds=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(ds), x, y, z,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetVelocity, &IDirectSound_SetVelocityHle);

// DSOUND::IDirectSound_SetI3DL2Listener 0x248E80 - stdcall (ds, LPCDSI3DL2LISTENER, DWORD dwApply),
// HRESULT, ret 12. Init 0xCCDC7 and SetEnvironment 0xCBFA0 (5 sites, presets 0x2D6884..0x2D69BC), all
// results ignored (`ret 4` / next store). S_OK.
std::int32_t __stdcall IDirectSound_SetI3DL2ListenerHle(void* ds, const void* listener,
                                                        std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSound_SetI3DL2Listener, "(ds=0x%08X, preset=0x%08X, apply=%u) -> S_OK", A(ds),
                   A(listener), apply);
    return kS_OK;
}
XDK_HOOK(IDirectSound_SetI3DL2Listener, &IDirectSound_SetI3DL2ListenerHle);

// ---- IDirectSoundBuffer (always NULL from DirectSoundCreateBuffer) ---------------------------------

// DSOUND::IDirectSoundBuffer_Release 0x246E93 - stdcall (buffer), ULONG, ret 4. Voice dtor 0xCC0F2,
// only for a non-NULL buffer (0xCC0EF), result ignored. Returns 0.
std::uint32_t __stdcall IDirectSoundBuffer_ReleaseHle(void* buffer) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_Release, "(buffer=0x%08X) -> 0", A(buffer));
    return 0;
}
XDK_HOOK(IDirectSoundBuffer_Release, &IDirectSoundBuffer_ReleaseHle);

// DSOUND::IDirectSoundBuffer_SetVolume 0x247950 - stdcall (buffer, LONG mB), HRESULT, ret 8. Voice update
// 0xCC91D, result ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetVolumeHle(void* buffer, std::int32_t volume) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetVolume, "(buffer=0x%08X, %d mB) -> S_OK", A(buffer), volume);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetVolume, &IDirectSoundBuffer_SetVolumeHle);

// DSOUND::IDirectSoundBuffer_SetMixBins 0x24796C - stdcall (buffer, LPCDSMIXBINS), HRESULT, ret 8.
// Voice create 0xCC35A and update 0xCC9D3, results ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetMixBinsHle(void* buffer, const void* mixBins) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetMixBins, "(buffer=0x%08X, mixbins=0x%08X) -> S_OK", A(buffer),
                   A(mixBins));
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetMixBins, &IDirectSoundBuffer_SetMixBinsHle);

// DSOUND::IDirectSoundBuffer_Play 0x247988 - stdcall (buffer, DWORD dwReserved1, DWORD dwReserved2, DWORD
// dwFlags), HRESULT, ret 16. Sites 0xCC3DB/0xCC45D (result ignored), 0xCC548 (`test eax,eax; je 0xCC5AE`:
// S_OK takes the normal path), voice update 0xCC72E (ignored). The Play variants only call it for a
// non-NULL buffer. S_OK; the buffer then reports "not playing" (GetStatus = 0).
std::int32_t __stdcall IDirectSoundBuffer_PlayHle(void* buffer, std::uint32_t reserved1,
                                                  std::uint32_t reserved2, std::uint32_t flags) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_Play, "(buffer=0x%08X, %u, %u, flags=0x%X) -> S_OK", A(buffer),
                   reserved1, reserved2, flags);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_Play, &IDirectSoundBuffer_PlayHle);

// DSOUND::IDirectSoundBuffer_Stop 0x2479AC - stdcall (buffer), HRESULT, ret 4. Voice Stop 0xCB575 (only
// for a non-NULL buffer), result ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_StopHle(void* buffer) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_Stop, "(buffer=0x%08X) -> S_OK", A(buffer));
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_Stop, &IDirectSoundBuffer_StopHle);

// DSOUND::IDirectSoundBuffer_GetStatus 0x2479C4 - stdcall (buffer, LPDWORD pdwStatus), HRESULT, ret 8.
// Voice update 0xCCA27 reads bit 0 of the stack DWORD (0xCCA2C `test byte [esp+0x20],1`; uninitialised
// before the call, so the stub must always write it, also for a NULL buffer): 0 = finished, the voice
// ends (0xCC63E voice+0x20 = 1). Voice dtor 0xCC0BF spins while the DWORD != 0 (avi.md 2.3).
// *pdwStatus = 0 (not playing), S_OK.
std::int32_t __stdcall IDirectSoundBuffer_GetStatusHle(void* buffer, std::uint32_t* status) noexcept {
    if (status != nullptr) {
        *status = 0;
    }
    XDK_TRACE_CALL(IDirectSoundBuffer_GetStatus, "(buffer=0x%08X, pdwStatus=0x%08X) -> S_OK, *pdwStatus=0",
                   A(buffer), A(status));
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_GetStatus, &IDirectSoundBuffer_GetStatusHle);

// DSOUND::IDirectSoundBuffer_SetFrequency 0x248398 - stdcall (buffer, DWORD Hz), HRESULT, ret 8. Voice
// update 0xCC910, result ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetFrequencyHle(void* buffer, std::uint32_t frequency) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetFrequency, "(buffer=0x%08X, %u Hz) -> S_OK", A(buffer), frequency);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetFrequency, &IDirectSoundBuffer_SetFrequencyHle);

// DSOUND::IDirectSoundBuffer_SetMaxDistance 0x2483B4 - stdcall (buffer, FLOAT, DWORD dwApply), HRESULT,
// ret 12. Voice update 0xCC6CE/0xCC83E, results ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetMaxDistanceHle(void* buffer, float distance,
                                                            std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetMaxDistance, "(buffer=0x%08X, %g, apply=%u) -> S_OK", A(buffer),
                   distance, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetMaxDistance, &IDirectSoundBuffer_SetMaxDistanceHle);

// DSOUND::IDirectSoundBuffer_SetMinDistance 0x2483D8 - stdcall (buffer, FLOAT, DWORD dwApply), HRESULT,
// ret 12. Voice update 0xCC6B9/0xCC829, results ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetMinDistanceHle(void* buffer, float distance,
                                                            std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetMinDistance, "(buffer=0x%08X, %g, apply=%u) -> S_OK", A(buffer),
                   distance, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetMinDistance, &IDirectSoundBuffer_SetMinDistanceHle);

// DSOUND::IDirectSoundBuffer_SetPosition 0x2483FC - stdcall (buffer, FLOAT x, y, z, DWORD dwApply),
// HRESULT, ret 20. Voice update 0xCC6DF/0xCC7F7, results ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetPositionHle(void* buffer, float x, float y, float z,
                                                         std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetPosition, "(buffer=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(buffer),
                   x, y, z, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetPosition, &IDirectSoundBuffer_SetPositionHle);

// DSOUND::IDirectSoundBuffer_SetVelocity 0x248431 - stdcall (buffer, FLOAT x, y, z, DWORD dwApply),
// HRESULT, ret 20. Voice update 0xCC6FC/0xCC814, results ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetVelocityHle(void* buffer, float x, float y, float z,
                                                         std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetVelocity, "(buffer=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(buffer),
                   x, y, z, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetVelocity, &IDirectSoundBuffer_SetVelocityHle);

// DSOUND::IDirectSoundBuffer_SetRolloffCurve 0x248466 - stdcall (buffer, const FLOAT* points, DWORD count,
// DWORD dwApply), HRESULT, ret 16. Voice create 0xCC36D (curve 0x2EAD3C, 6 points, immediate), result
// ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetRolloffCurveHle(void* buffer, const float* points,
                                                             std::uint32_t count,
                                                             std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetRolloffCurve,
                   "(buffer=0x%08X, points=0x%08X, count=%u, apply=%u) -> S_OK", A(buffer), A(points), count,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetRolloffCurve, &IDirectSoundBuffer_SetRolloffCurveHle);

// DSOUND::IDirectSoundBuffer_SetI3DL2Source 0x24848A - stdcall (buffer, LPCDSI3DL2BUFFER, DWORD dwApply),
// HRESULT, ret 12. Voice update 0xCCA0E (lRoom -600), result ignored. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetI3DL2SourceHle(void* buffer, const void* source,
                                                            std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetI3DL2Source, "(buffer=0x%08X, source=0x%08X, apply=%u) -> S_OK",
                   A(buffer), A(source), apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetI3DL2Source, &IDirectSoundBuffer_SetI3DL2SourceHle);

// DSOUND::IDirectSoundBuffer_SetBufferData 0x248EA0 - stdcall (buffer, LPVOID pvData, DWORD dwBytes),
// HRESULT, ret 12. Voice create 0xCC2AF, Reset 0xCB597 and dtor 0xCC06F (both with (0, 0) and only for
// a non-NULL buffer), results ignored. The game keeps owning the data. S_OK.
std::int32_t __stdcall IDirectSoundBuffer_SetBufferDataHle(void* buffer, void* data,
                                                           std::uint32_t size) noexcept {
    XDK_TRACE_CALL(IDirectSoundBuffer_SetBufferData, "(buffer=0x%08X, data=0x%08X, %u bytes) -> S_OK",
                   A(buffer), A(data), size);
    return kS_OK;
}
XDK_HOOK(IDirectSoundBuffer_SetBufferData, &IDirectSoundBuffer_SetBufferDataHle);

// ---- IDirectSoundStream (NULL, or a stub stream with [audio] dummy_streams = 1) ------------------

// DSOUND::IDirectSoundStream_SetVolume 0x2479E0 (5-byte jmp -> CDirectSoundStream_SetVolume) - stdcall
// (stream, LONG mB), HRESULT, ret 8. Voice update 0xCC8C4 and movie player 0xEE471 (only for a non-NULL
// stream from GetAudioStream, 0xEE429), results ignored. S_OK.
std::int32_t __stdcall IDirectSoundStream_SetVolumeHle(void* stream, std::int32_t volume) noexcept {
    XDK_TRACE_CALL(IDirectSoundStream_SetVolume, "(stream=0x%08X, %d mB) -> S_OK", A(stream), volume);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetVolume, &IDirectSoundStream_SetVolumeHle);

// DSOUND::IDirectSoundStream_SetMixBins 0x2479E5 (jmp -> CDirectSoundStream_SetMixBins) - stdcall (stream,
// LPCDSMIXBINS), HRESULT, ret 8. Voice create 0xCC32C, update 0xCC9C0, results ignored. S_OK.
std::int32_t __stdcall IDirectSoundStream_SetMixBinsHle(void* stream, const void* mixBins) noexcept {
    XDK_TRACE_CALL(IDirectSoundStream_SetMixBins, "(stream=0x%08X, mixbins=0x%08X) -> S_OK", A(stream),
                   A(mixBins));
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetMixBins, &IDirectSoundStream_SetMixBinsHle);

// DSOUND::IDirectSoundStream_Pause 0x2479EA (jmp -> CDirectSoundStream_Pause) - stdcall (stream, DWORD
// dwPause), HRESULT, ret 8. Game: Stop 0xCB563 (1), Play variants 0xCC3B9/0xCC43A/0xCC500 (0) - only for
// a non-NULL stream - and voice update 0xCC711 (0, also NULL); results ignored. XMV 0x264A81 (replaced)
// passes 2 and tests the result. Stub streams pause/resume their packet clock. S_OK.
std::int32_t __stdcall IDirectSoundStream_PauseHle(void* stream, std::uint32_t pause) noexcept {
    PauseStubStream(stream, pause);
    XDK_TRACE_CALL(IDirectSoundStream_Pause, "(stream=0x%08X, %u) -> S_OK", A(stream), pause);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_Pause, &IDirectSoundStream_PauseHle);

// DSOUND::IDirectSoundStream_FlushEx 0x2479EF - stdcall (stream, REFERENCE_TIME rtTimeStamp, DWORD
// dwFlags), HRESULT, ret 16. Only XMV CloseDecoder 0x2646A4 (replaced) with (s, 0, 0, 1); it then spins
// on GetStatus & 0x10000 (avi.md 2.3), so a stub stream flushes synchronously. S_OK.
std::int32_t __stdcall IDirectSoundStream_FlushExHle(void* stream, std::int64_t timeStamp,
                                                     std::uint32_t flags) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_streamMutex);
        if (StreamState* s = FindStream(stream)) {
            FlushQueue(*s);
        }
    }
    XDK_TRACE_CALL(IDirectSoundStream_FlushEx, "(stream=0x%08X, rt=%lld, flags=0x%X) -> S_OK", A(stream),
                   static_cast<long long>(timeStamp), flags);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_FlushEx, &IDirectSoundStream_FlushExHle);

// DSOUND::IDirectSoundStream_SetFrequency 0x2484AA (jmp -> CDirectSoundStream_SetFrequency) - stdcall
// (stream, DWORD Hz), HRESULT, ret 8. Voice update 0xCC8BA, result ignored. S_OK.
std::int32_t __stdcall IDirectSoundStream_SetFrequencyHle(void* stream, std::uint32_t frequency) noexcept {
    XDK_TRACE_CALL(IDirectSoundStream_SetFrequency, "(stream=0x%08X, %u Hz) -> S_OK", A(stream), frequency);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetFrequency, &IDirectSoundStream_SetFrequencyHle);

// DSOUND::IDirectSoundStream_SetMaxDistance 0x2484AF - stdcall (stream, FLOAT, DWORD dwApply), HRESULT,
// ret 12. Voice update 0xCC7E9, result ignored. S_OK.
std::int32_t __stdcall IDirectSoundStream_SetMaxDistanceHle(void* stream, float distance,
                                                            std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundStream_SetMaxDistance, "(stream=0x%08X, %g, apply=%u) -> S_OK", A(stream),
                   distance, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetMaxDistance, &IDirectSoundStream_SetMaxDistanceHle);

// DSOUND::IDirectSoundStream_SetMinDistance 0x2484C7 - stdcall (stream, FLOAT, DWORD dwApply), HRESULT,
// ret 12. Voice update 0xCC7D7, result ignored. S_OK.
std::int32_t __stdcall IDirectSoundStream_SetMinDistanceHle(void* stream, float distance,
                                                            std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundStream_SetMinDistance, "(stream=0x%08X, %g, apply=%u) -> S_OK", A(stream),
                   distance, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetMinDistance, &IDirectSoundStream_SetMinDistanceHle);

// DSOUND::IDirectSoundStream_SetPosition 0x2484DF - stdcall (stream, FLOAT x, y, z, DWORD dwApply),
// HRESULT, ret 20. Voice update 0xCC7AB, result ignored. S_OK.
std::int32_t __stdcall IDirectSoundStream_SetPositionHle(void* stream, float x, float y, float z,
                                                         std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundStream_SetPosition, "(stream=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(stream),
                   x, y, z, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetPosition, &IDirectSoundStream_SetPositionHle);

// DSOUND::IDirectSoundStream_SetVelocity 0x248508 - stdcall (stream, FLOAT x, y, z, DWORD dwApply),
// HRESULT, ret 20. Voice update 0xCC7C5, result ignored. S_OK.
std::int32_t __stdcall IDirectSoundStream_SetVelocityHle(void* stream, float x, float y, float z,
                                                         std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundStream_SetVelocity, "(stream=0x%08X, %g, %g, %g, apply=%u) -> S_OK", A(stream),
                   x, y, z, apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetVelocity, &IDirectSoundStream_SetVelocityHle);

// DSOUND::IDirectSoundStream_SetRolloffCurve 0x248531 (jmp -> CDirectSoundStream_SetRolloffCurve) -
// stdcall (stream, const FLOAT* points, DWORD count, DWORD dwApply), HRESULT, ret 16. Voice create
// 0xCC33C, result ignored. S_OK.
std::int32_t __stdcall IDirectSoundStream_SetRolloffCurveHle(void* stream, const float* points,
                                                             std::uint32_t count,
                                                             std::uint32_t apply) noexcept {
    XDK_TRACE_CALL(IDirectSoundStream_SetRolloffCurve,
                   "(stream=0x%08X, points=0x%08X, count=%u, apply=%u) -> S_OK", A(stream), A(points), count,
                   apply);
    return kS_OK;
}
XDK_HOOK(IDirectSoundStream_SetRolloffCurve, &IDirectSoundStream_SetRolloffCurveHle);

// ---- global functions --------------------------------------------------------------------------------

// DSOUND::DirectSoundUseFullHRTF 0x246EA9 - stdcall (void), void, ret 0. Init 0xCCD64 (eax overwritten
// by the next instruction). No-op.
void __stdcall DirectSoundUseFullHRTFHle() noexcept {
    XDK_TRACE_CALL(DirectSoundUseFullHRTF, "() -> no-op");
}
XDK_HOOK(DirectSoundUseFullHRTF, &DirectSoundUseFullHRTFHle);

// DSOUND::DirectSoundDoWork 0x247A07 - stdcall (void), void, ret 0. SoundManager::Update 0xCBE9A once
// per frame (also XMV 0x2646DA/0x264ED9, replaced). Advances the packet clocks of stub streams (no-op
// without them).
void __stdcall DirectSoundDoWorkHle() noexcept {
    const std::uint32_t completed = PumpStubStreams();
    XDK_TRACE_CALL(DirectSoundDoWork, "() -> %u stub stream packets completed", completed);
}
XDK_HOOK(DirectSoundDoWork, &DirectSoundDoWorkHle);

}  // namespace snd
