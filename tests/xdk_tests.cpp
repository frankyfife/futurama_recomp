// Unit tests of the XDK stubs (DSOUND/XMV/XPP, M2; xdk agent). Run without the XBE: the stubs are called
// directly from host code with host buffers standing in for guest memory (same address space).
#include "doctest.h"

#include "audio/dsound_stubs.h"
#include "audio/xdk_entries.h"
#include "core/guest.h"
#include "input/xpp_stubs.h"
#include "video/xmv_stubs.h"

#include <cstring>
#include <set>
#include <string>

namespace {

std::int64_t QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
std::int64_t QpcFrequency() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

// Guest vtable slot of an XDK dummy object (+0 = guest pointer to the vtable).
template <class Fn>
Fn VtableSlot(const void* object, std::uint32_t slot) {
    const std::uint32_t vtable = guest::Read<std::uint32_t>(guest::Addr(object));
    return reinterpret_cast<Fn>(static_cast<std::uintptr_t>(guest::Read<std::uint32_t>(vtable + 4 * slot)));
}

// Two distinct call sites of the same stub (not inlined, no tail call: the result is used).
__declspec(noinline) std::uint32_t CallXGetDevicesSiteA() {
    return inp::XGetDevicesHle(nullptr) + 1;
}
__declspec(noinline) std::uint32_t CallXGetDevicesSiteB() {
    return inp::XGetDevicesHle(nullptr) + 2;
}

using StreamProcessFn = std::int32_t(__stdcall*)(void*, const snd::XMediaPacket*, const snd::XMediaPacket*);
using StreamGetStatusFn = std::int32_t(__stdcall*)(void*, std::uint32_t*);
using StreamRefFn = std::uint32_t(__stdcall*)(void*);

}  // namespace

TEST_CASE("XDK entry table: 54 stdcall entries from avi_api_surface.tsv, all stubbed") {
    const auto entries = xdk::Entries();
    REQUIRE(entries.size() == 54);
    std::uint32_t perLib[3] = {};
    std::set<guest::GuestAddr> addresses;
    for (const xdk::EntryInfo& e : entries) {
        ++perLib[static_cast<int>(e.library)];
        addresses.insert(e.address);
        CHECK(e.purgeBytes % 4 == 0);
        CHECK(e.sitesGame + e.sitesXmv > 0);
        CHECK(std::string(e.hookName).rfind(std::string(xdk::LibraryName(e.library)) + "::", 0) == 0);
        CHECK(guest::InXbeImage(e.address, 5));
        INFO(e.hookName);
        CHECK(xdk::RegisteredStub(e.entry) != nullptr);  // XDK_HOOK compiled the purge check
    }
    CHECK(perLib[0] == 40);  // DSOUND
    CHECK(perLib[1] == 7);   // XMV
    CHECK(perLib[2] == 7);   // XPP
    CHECK(addresses.size() == 54);
    // Spot checks against avi.md 2.2/3.1/4.1 (ret N) and design Anhang B (addresses).
    static_assert(xdk::gen::DirectSoundCreate::address == 0x0024935C &&
                  xdk::gen::DirectSoundCreate::purgeBytes == 12);
    static_assert(xdk::gen::IDirectSoundStream_FlushEx::purgeBytes == 16);  // stream + REFERENCE_TIME + flags
    static_assert(xdk::gen::IDirectSound_SetOrientation::purgeBytes == 32);
    static_assert(xdk::gen::DirectSoundUseFullHRTF::purgeBytes == 0);
    static_assert((xdk::gen::IDirectSoundStream_Pause::flags & xdk::kFlagJmpThunk) != 0);
    static_assert(xdk::gen::XMVDecoder_CreateDecoderForPackets::address == 0x00264360 &&
                  xdk::gen::XMVDecoder_CreateDecoderForPackets::purgeBytes == 24);
    static_assert(xdk::gen::XMVDecoder_GetNextFrame::address == 0x00264962);
    static_assert(xdk::gen::XInitDevices::address == 0x0028CFE1 && xdk::gen::XInitDevices::purgeBytes == 8);
    static_assert(xdk::gen::XGetDeviceChanges::purgeBytes == 12 && xdk::gen::XInputSetState::purgeBytes == 8);
    static_assert(xdk::stream::Process::slot == 4 && xdk::stream::Process::purgeBytes == 12);
    static_assert(xdk::stream::GetStatus::slot == 3 && xdk::stream::GetStatus::purgeBytes == 8);
    static_assert(xdk::stream::Release::calledByGame && !xdk::stream::Flush::calledByGame);
    CHECK(std::string(xdk::Info(xdk::Entry::XInputOpen).hookName) == "XPP::XInputOpen");
}

TEST_CASE("XDK stubs pop exactly ret N (measured on a probe thread with zero arguments)") {
    // The runtime self-test XdkEntryPurgeMeasured does the same through the installed guest hooks.
    for (const xdk::EntryInfo& e : xdk::Entries()) {
        INFO(e.hookName);
        const std::uint32_t calls = xdk::CallCount(e.entry);
        const xdk::PurgeMeasurement m =
            xdk::MeasureEntryPurge(xdk::RegisteredStub(e.entry), e.purgeBytes / 4);
        REQUIRE(m.ok);
        CHECK(m.purgeBytes == e.purgeBytes);
        CHECK(xdk::CallCount(e.entry) == calls);  // probe calls are not counted
    }
    CHECK_FALSE(snd::IsDummyDevice(nullptr));
}

TEST_CASE("XDK guest object pool: below 0x80000000, freed objects carry the trap vtable") {
    void* a = xdk::AllocGuestObject(16);
    REQUIRE(a != nullptr);
    CHECK(static_cast<std::uint64_t>(guest::Addr(a)) + xdk::kGuestObjectSize <= guest::kGuestMemoryLimit);
    CHECK(xdk::IsGuestObject(a));
    CHECK(xdk::AllocGuestObject(xdk::kGuestObjectSize + 1) == nullptr);
    const void* const* trap = xdk::TrapVtable();
    REQUIRE(trap != nullptr);
    CHECK(static_cast<std::uint64_t>(guest::Addr(trap)) + 4 * xdk::kTrapVtableSlots <=
          guest::kGuestMemoryLimit);
    xdk::FreeGuestObject(a);
    CHECK_FALSE(xdk::IsGuestObject(a));
    CHECK(guest::Read<std::uint32_t>(guest::Addr(a)) == guest::Addr(trap));
    xdk::FreeGuestObject(nullptr);  // tolerated
}

TEST_CASE("DSOUND stubs: device singleton, NULL buffers/streams, GetStatus = 0") {
    snd::SetDummyStreams(false);
    void* ds1 = nullptr;
    void* ds2 = reinterpret_cast<void*>(1);
    REQUIRE(snd::DirectSoundCreateHle(nullptr, &ds1, nullptr) == snd::kS_OK);
    REQUIRE(snd::DirectSoundCreateHle(nullptr, &ds2, nullptr) == snd::kS_OK);
    CHECK(ds1 != nullptr);
    CHECK(ds1 == ds2);  // singleton (avi.md 2.3)
    CHECK(snd::IsDummyDevice(ds1));
    CHECK(static_cast<std::uint64_t>(guest::Addr(ds1)) < guest::kGuestMemoryLimit);
    CHECK(guest::Read<std::uint32_t>(guest::Addr(ds1)) == guest::Addr(xdk::TrapVtable()));
    const std::uint32_t refs = snd::IDirectSound_ReleaseHle(ds2);
    CHECK(snd::IDirectSound_ReleaseHle(ds1) + 1 == refs);
    CHECK(snd::IsDummyDevice(ds1));  // never torn down

    // Buffers: *pp = NULL, every buffer function accepts NULL.
    snd::XDsBufferDesc bd{0x18, 0x60110, 0, 0, 0, 0};
    void* buffer = reinterpret_cast<void*>(0x1234);
    CHECK(snd::DirectSoundCreateBufferHle(&bd, &buffer) == snd::kS_OK);
    CHECK(buffer == nullptr);
    std::uint32_t status = 0xFFFFFFFFu;
    CHECK(snd::IDirectSoundBuffer_GetStatusHle(nullptr, &status) == snd::kS_OK);
    CHECK(status == 0);  // voice destructor 0xCC030 spins otherwise
    CHECK(snd::IDirectSoundBuffer_PlayHle(nullptr, 0, 0, 1) == snd::kS_OK);
    CHECK(snd::IDirectSoundBuffer_SetBufferDataHle(nullptr, nullptr, 0) == snd::kS_OK);
    CHECK(snd::IDirectSoundBuffer_SetPositionHle(nullptr, 1.0f, 2.0f, 3.0f, 1) == snd::kS_OK);
    CHECK(snd::IDirectSoundBuffer_ReleaseHle(nullptr) == 0);

    // Streams (default): *pp = NULL.
    snd::XWaveFormatEx wfx{snd::kWaveFormatPcm, 2, 22050, 88200, 4, 16, 0x12};
    snd::XDsStreamDesc sd{0, 10, guest::Addr(&wfx), 0, 0, 0};
    void* stream = reinterpret_cast<void*>(0x1234);
    CHECK(snd::DirectSoundCreateStreamHle(&sd, &stream) == snd::kS_OK);
    CHECK(stream == nullptr);
    CHECK(snd::IDirectSoundStream_PauseHle(nullptr, snd::kStreamPause) == snd::kS_OK);
    CHECK(snd::IDirectSoundStream_FlushExHle(nullptr, 0, 1) == snd::kS_OK);

    // DownloadEffectsImage: S_OK, *ppDesc = NULL (the game never reads either, avi.md 2.1).
    std::uint32_t desc = 0xFFFFFFFFu;
    CHECK(snd::IDirectSound_DownloadEffectsImageHle(ds1, &wfx, sizeof(wfx), nullptr, &desc) == snd::kS_OK);
    CHECK(desc == 0);
    snd::DirectSoundDoWorkHle();
    snd::DirectSoundUseFullHRTFHle();
}

TEST_CASE("DSOUND stub streams ([audio] dummy_streams): XMediaObject vtable and packet status") {
    snd::SetDummyStreams(true);
    snd::XWaveFormatEx wfx{snd::kWaveFormatPcm, 2, 22050, 0, 4, 16, 0x12};  // 88200 bytes/s
    snd::XDsStreamDesc sd{0, 2, guest::Addr(&wfx), 0, 0, 0};
    void* stream = nullptr;
    REQUIRE(snd::DirectSoundCreateStreamHle(&sd, &stream) == snd::kS_OK);
    REQUIRE(stream != nullptr);
    CHECK(snd::IsStubStream(stream));
    CHECK(static_cast<std::uint64_t>(guest::Addr(stream)) < guest::kGuestMemoryLimit);

    const auto process = VtableSlot<StreamProcessFn>(stream, xdk::stream::Process::slot);
    const auto getStatus = VtableSlot<StreamGetStatusFn>(stream, xdk::stream::GetStatus::slot);
    const auto addRef = VtableSlot<StreamRefFn>(stream, xdk::stream::AddRef::slot);
    const auto release = VtableSlot<StreamRefFn>(stream, xdk::stream::Release::slot);

    std::uint32_t status[3] = {0, 0, 0};
    std::uint32_t completed[3] = {7, 7, 7};
    snd::XMediaPacket pkt[3] = {};
    for (int i = 0; i < 3; ++i) {
        pkt[i].dwMaxSize = 8820;  // 0.1 s
        pkt[i].pdwStatus = guest::Addr(&status[i]);
        pkt[i].pdwCompletedSize = guest::Addr(&completed[i]);
    }
    const std::int64_t t0 = QpcNow();
    CHECK(process(stream, &pkt[0], nullptr) == snd::kS_OK);
    CHECK(status[0] == snd::kPacketPending);  // synchronous E_PENDING (pump 0xCB5D4)
    CHECK(completed[0] == 0);
    CHECK(process(stream, &pkt[1], nullptr) == snd::kS_OK);
    // Queue full (dwMaxAttachedPackets = 2): DSERR_INVALIDCALL, status untouched (original 0x24B1C4).
    CHECK(process(stream, &pkt[2], nullptr) == snd::kDsErrInvalidCall);
    CHECK(status[2] == 0);
    CHECK(snd::StubStreamQueuedPackets(stream) == 2);
    std::uint32_t st = 0;
    CHECK(getStatus(stream, &st) == snd::kS_OK);
    CHECK((st & snd::kStreamStatusPlaying) != 0);

    // 0.15 s later: the first packet has played, the second not yet.
    const std::int64_t f = QpcFrequency();
    CHECK(snd::PumpStubStreams(t0 + f * 15 / 100) == 1);
    CHECK(status[0] == snd::kPacketSuccess);
    CHECK(completed[0] == 8820);
    CHECK(status[1] == snd::kPacketPending);

    // Paused streams keep their packets; FlushEx completes them as flushed and clears PLAYING.
    CHECK(snd::IDirectSoundStream_PauseHle(stream, snd::kStreamPause) == snd::kS_OK);
    CHECK(snd::PumpStubStreams(t0 + f * 10) == 0);
    CHECK(status[1] == snd::kPacketPending);
    CHECK(snd::IDirectSoundStream_FlushExHle(stream, 0, 1) == snd::kS_OK);
    CHECK(status[1] == snd::kPacketFlushed);
    CHECK(completed[1] == 0);
    CHECK(getStatus(stream, &st) == snd::kS_OK);
    CHECK((st & snd::kStreamStatusPlaying) == 0);  // XMV CloseDecoder spin ends

    // Reference counting through the vtable; the last Release frees the guest object.
    CHECK(addRef(stream) == 2);
    CHECK(release(stream) == 1);
    CHECK(snd::IsStubStream(stream));
    CHECK(release(stream) == 0);
    CHECK_FALSE(snd::IsStubStream(stream));
    CHECK_FALSE(xdk::IsGuestObject(stream));
    const snd::StubStreamStatistics stats = snd::GetStubStreamStatistics();
    CHECK(stats.created >= 1);
    CHECK(stats.packetsSubmitted >= 2);
    CHECK(stats.packetsCompleted >= 1);
    CHECK(stats.packetsFlushed >= 1);
    CHECK(stats.packetsRejected >= 1);
    snd::SetDummyStreams(false);
}

TEST_CASE("DSOUND stub streams: Xbox ADPCM packets last 64 samples per 36-byte block") {
    snd::SetDummyStreams(true);
    snd::XWaveFormatEx wfx{snd::kWaveFormatXboxAdpcm, 1, 22050, 0, 36, 4, 0x12};  // 12403 bytes/s
    snd::XDsStreamDesc sd{0, 10, guest::Addr(&wfx), 0, 0, 0};
    void* stream = nullptr;
    REQUIRE(snd::DirectSoundCreateStreamHle(&sd, &stream) == snd::kS_OK);
    REQUIRE(stream != nullptr);
    std::uint32_t status = 0;
    snd::XMediaPacket pkt{};
    pkt.dwMaxSize = 12403;  // ~1 s
    pkt.pdwStatus = guest::Addr(&status);
    const std::int64_t t0 = QpcNow();
    const auto process = VtableSlot<StreamProcessFn>(stream, xdk::stream::Process::slot);
    REQUIRE(process(stream, &pkt, nullptr) == snd::kS_OK);
    const std::int64_t f = QpcFrequency();
    CHECK(snd::PumpStubStreams(t0 + f / 2) == 0);
    CHECK(status == snd::kPacketPending);
    CHECK(snd::PumpStubStreams(t0 + f * 2) == 1);
    CHECK(status == snd::kPacketSuccess);
    const auto release = VtableSlot<StreamRefFn>(stream, xdk::stream::Release::slot);
    CHECK(release(stream) == 0);
    snd::SetDummyStreams(false);
}

TEST_CASE("XMV stubs: decoder from the header, end of file on the first frame") {
    vid::XmvFileHeader hdr{0x6000, 0x6000, 0x6000, vid::kXmvMagic, 2, 640, 480, 5917, 5, 0};
    void* decoder = nullptr;
    REQUIRE(vid::XMVDecoder_CreateDecoderForPacketsHle(0, &hdr, nullptr, nullptr, nullptr, &decoder) == 0);
    REQUIRE(decoder != nullptr);
    CHECK(vid::IsStubDecoder(decoder));
    CHECK(static_cast<std::uint64_t>(guest::Addr(decoder)) < guest::kGuestMemoryLimit);
    const std::uint32_t live = vid::LiveStubDecoders();
    CHECK(live >= 1);

    vid::XmvVideoDescriptor desc{0, 0, 0xDEADBEEFu, 0};
    vid::XMVDecoder_GetVideoDescriptorHle(decoder, &desc);
    CHECK(desc.width == 640);
    CHECK(desc.height == 480);
    CHECK(desc.notWritten == 0xDEADBEEFu);  // like the original 0x264759
    CHECK(desc.audioStreamCount == 5);

    void* audio = reinterpret_cast<void*>(0x1234);
    CHECK(vid::XMVDecoder_EnableAudioStreamHle(decoder, 1, 0, nullptr, &audio) == 0);
    CHECK(audio == nullptr);
    audio = reinterpret_cast<void*>(0x1234);
    vid::XMVDecoder_GetAudioStreamHle(decoder, 1, &audio);
    CHECK(audio == nullptr);  // movie player 0xEE429 skips SetVolume/Release

    std::int32_t result = vid::kXmvFailure;
    std::uint32_t timestamp = 0xCAFEu;
    CHECK(vid::XMVDecoder_GetNextFrameHle(decoder, nullptr, &result, &timestamp) == 0);
    CHECK(result == vid::kXmvEndOfFile);  // player 0xEDEEA sets its done flag
    CHECK(timestamp == 0xCAFEu);          // only written with a new frame (original)
    CHECK(vid::XMVDecoder_GetPlaybackTimeMsHle(decoder) == 0);

    vid::XMVDecoder_CloseDecoderHle(decoder);
    CHECK_FALSE(vid::IsStubDecoder(decoder));
    CHECK(vid::LiveStubDecoders() == live - 1);

    // A header without 'xobX' fails like a broken file; *ppDecoder stays untouched.
    hdr.magic = 0;
    void* none = reinterpret_cast<void*>(0x1234);
    CHECK(vid::XMVDecoder_CreateDecoderForPacketsHle(0, &hdr, nullptr, nullptr, nullptr, &none) < 0);
    CHECK(none == reinterpret_cast<void*>(0x1234));
}

TEST_CASE("XPP stubs: no devices, not-connected results, rumble status never pending") {
    const void* gamepad = guest::Ptr<const void>(guest::kXppDeviceTypeGamepad);
    inp::XInitDevicesHle(1, nullptr);
    CHECK(inp::XGetDevicesHle(gamepad) == 0);
    std::uint32_t ins = 0xFFFFFFFFu, rem = 0xFFFFFFFFu;
    CHECK(inp::XGetDeviceChangesHle(gamepad, &ins, &rem) == FALSE);
    CHECK(ins == 0);
    CHECK(rem == 0);
    CHECK(inp::XInputOpenHle(gamepad, 0, 0, nullptr) == nullptr);
    inp::XInputCloseHle(nullptr);

    inp::XInputState state;
    std::memset(&state, 0xAB, sizeof(state));
    CHECK(inp::XInputGetStateHle(nullptr, &state) == inp::kErrorDeviceNotConnected);
    CHECK(state.dwPacketNumber == 0);
    CHECK(state.Gamepad.wButtons == 0);
    CHECK(state.Gamepad.sThumbRY == 0);

    inp::XInputFeedback fb{};
    fb.dwStatus = inp::kErrorIoPending;
    fb.wLeftMotorSpeed = 0xFFFF;
    CHECK(inp::XInputSetStateHle(nullptr, &fb) == inp::kErrorDeviceNotConnected);
    CHECK(fb.dwStatus != inp::kErrorIoPending);  // rumble 0x9C229 can send again
}

TEST_CASE("XDK call tracing counts calls and call sites") {
    const std::uint32_t calls0 = xdk::CallCount(xdk::Entry::XGetDevices);
    const std::uint32_t sites0 = xdk::CallSiteCount(xdk::Entry::XGetDevices);
    volatile std::uint32_t sink = 0;
    for (int i = 0; i < 3; ++i) {
        sink = sink + CallXGetDevicesSiteA();  // one call site
    }
    sink = sink + CallXGetDevicesSiteB();  // a second call site
    CHECK(xdk::CallCount(xdk::Entry::XGetDevices) == calls0 + 4);
    CHECK(xdk::CallSiteCount(xdk::Entry::XGetDevices) == sites0 + 2);
    xdk::LogCallSummary(core::Level::Debug);
}
