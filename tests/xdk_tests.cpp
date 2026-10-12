// Unit tests of the XDK replacements (XMV stubs, M2; DSOUND HLE smoke test, M5; xdk agent) and an end-to-end check of the XPP HLE (M4). Run without
// the XBE: the stubs are called directly from host code with host buffers standing in for guest memory (same address space).
#include "doctest.h"

#include "audio/audio_engine.h"
#include "audio/dsound_hle.h"
#include "audio/xdk_entries.h"
#include "core/guest.h"
#include "input/xpp_hle.h"
#include "video/xmv_hle.h"

#include <cstring>
#include <memory>
#include <set>
#include <string>

namespace {

// Two distinct call sites of the same stub (not inlined, no tail call: the result is used).
__declspec(noinline) std::uint32_t CallXGetDevicesSiteA() {
    return inp::XGetDevicesHle(nullptr) + 1;
}
__declspec(noinline) std::uint32_t CallXGetDevicesSiteB() {
    return inp::XGetDevicesHle(nullptr) + 2;
}

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
    CHECK_FALSE(snd::IsDsoundDevice(nullptr));
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

TEST_CASE("DSOUND HLE (M5): device singleton, NULL handles tolerated, GetStatus always written") {
    // The DSOUND entries are HLE since M5 (audio/dsound_hle.cpp); their behaviour is tested in
    // tests/audio_dsound_tests.cpp. Here only the guarantees the old stub tests covered, on a software engine.
    snd::EngineConfig config;
    config.enabled = false;
    config.softwareRealtime = false;
    const std::unique_ptr<snd::AudioEngine> engine = snd::CreateSoftwareEngine(config);
    REQUIRE(engine != nullptr);
    snd::SetEngineForTesting(engine.get());
    snd::ResetDsoundForTesting();
    void* ds1 = nullptr;
    void* ds2 = reinterpret_cast<void*>(1);
    REQUIRE(snd::DirectSoundCreateHle(nullptr, &ds1, nullptr) == snd::kS_OK);
    REQUIRE(snd::DirectSoundCreateHle(nullptr, &ds2, nullptr) == snd::kS_OK);
    CHECK(ds1 != nullptr);
    CHECK(ds1 == ds2);  // singleton (avi.md 2.3)
    CHECK(snd::IsDsoundDevice(ds1));
    CHECK(static_cast<std::uint64_t>(guest::Addr(ds1)) < guest::kGuestMemoryLimit);
    CHECK(guest::Read<std::uint32_t>(guest::Addr(ds1)) == guest::Addr(xdk::TrapVtable()));
    const std::uint32_t refs = snd::IDirectSound_ReleaseHle(ds2);
    CHECK(snd::IDirectSound_ReleaseHle(ds1) + 1 == refs);
    CHECK(snd::IsDsoundDevice(ds1));  // never torn down
    std::uint32_t status = 0xFFFFFFFFu;
    CHECK(snd::IDirectSoundBuffer_GetStatusHle(nullptr, &status) == snd::kS_OK);
    CHECK(status == 0);  // voice destructor 0xCC030 spins otherwise
    CHECK(snd::IDirectSoundBuffer_PlayHle(nullptr, 0, 0, 1) == snd::kS_OK);
    CHECK(snd::IDirectSoundBuffer_SetBufferDataHle(nullptr, nullptr, 0) == snd::kS_OK);
    CHECK(snd::IDirectSoundBuffer_SetPositionHle(nullptr, 1.0f, 2.0f, 3.0f, 1) == snd::kS_OK);
    CHECK(snd::IDirectSoundBuffer_SetI3DL2SourceHle(nullptr, nullptr, 1) == snd::kS_OK);  // 3D streams pass NULL
    CHECK(snd::IDirectSoundBuffer_ReleaseHle(nullptr) == 0);
    CHECK(snd::IDirectSoundStream_PauseHle(nullptr, snd::kStreamPause) == snd::kS_OK);
    CHECK(snd::IDirectSoundStream_FlushExHle(nullptr, 0, 1) == snd::kS_OK);
    std::uint32_t desc = 0xFFFFFFFFu;
    CHECK(snd::IDirectSound_DownloadEffectsImageHle(ds1, &status, sizeof(status), nullptr, &desc) == snd::kS_OK);
    CHECK(desc == 0);
    snd::DirectSoundDoWorkHle();
    snd::DirectSoundUseFullHRTFHle();
    snd::ResetDsoundForTesting();
    snd::SetEngineForTesting(nullptr);
}

TEST_CASE("XMV HLE: decoder from the header; without packet callbacks the movie fails cleanly") {
    vid::XmvFileHeader hdr{0x6000, 0x6000, 0x6000, vid::kXmvMagic, 2, 640, 480, 5917, 1, 0};
    // One audio track descriptor follows the fixed header (avi.md 3.3): Xbox ADPCM, 2 ch, 22050 Hz, 4 bit.
    struct {
        vid::XmvFileHeader h;
        std::uint16_t tag, channels;
        std::uint32_t rate;
        std::uint16_t bits, flags;
    } file{hdr, 0x69, 2, 22050, 4, 0};
    void* decoder = nullptr;
    REQUIRE(vid::XMVDecoder_CreateDecoderForPacketsHle(0, &file.h, nullptr, nullptr, nullptr, &decoder) == 0);
    REQUIRE(decoder != nullptr);
    CHECK(vid::IsXmvDecoder(decoder));
    CHECK(static_cast<std::uint64_t>(guest::Addr(decoder)) < guest::kGuestMemoryLimit);
    const std::uint32_t live = vid::LiveXmvDecoders();
    CHECK(live >= 1);

    vid::XmvVideoDescriptor desc{0, 0, 0xDEADBEEFu, 0};
    vid::XMVDecoder_GetVideoDescriptorHle(decoder, &desc);
    CHECK(desc.width == 640);
    CHECK(desc.height == 480);
    CHECK(desc.notWritten == 0xDEADBEEFu);  // like the original 0x264759
    CHECK(desc.audioStreamCount == 1);

    void* audio = reinterpret_cast<void*>(0x1234);
    CHECK(vid::XMVDecoder_EnableAudioStreamHle(decoder, 3, 0, nullptr, &audio) < 0);  // no track 3
    CHECK(audio == nullptr);
    audio = reinterpret_cast<void*>(0x1234);
    vid::XMVDecoder_GetAudioStreamHle(decoder, 0, &audio);
    CHECK(audio == nullptr);  // not enabled: movie player 0xEE429 skips SetVolume/Release

    // No callbacks: the first GetNextFrame cannot read a packet -> error result 3 (the player sets its done flag).
    std::int32_t result = vid::kXmvNewFrame;
    std::uint32_t timestamp = 0xCAFEu;
    CHECK(vid::XMVDecoder_GetNextFrameHle(decoder, nullptr, &result, &timestamp) == 0);
    CHECK(result == vid::kXmvFailure);
    CHECK(timestamp == 0xCAFEu);  // only written with a new frame
    CHECK(vid::XMVDecoder_GetNextFrameHle(decoder, nullptr, &result, nullptr) == 0);
    CHECK(result == vid::kXmvFailure);
    CHECK(vid::XMVDecoder_GetPlaybackTimeMsHle(decoder) == 0);

    vid::XMVDecoder_CloseDecoderHle(decoder);
    CHECK_FALSE(vid::IsXmvDecoder(decoder));
    CHECK(vid::LiveXmvDecoders() == live - 1);

    // A header without 'xobX' fails like a broken file; *ppDecoder stays untouched.
    file.h.magic = 0;
    void* none = reinterpret_cast<void*>(0x1234);
    CHECK(vid::XMVDecoder_CreateDecoderForPacketsHle(0, &file.h, nullptr, nullptr, nullptr, &none) < 0);
    CHECK(none == reinterpret_cast<void*>(0x1234));
}

// M4: the XPP entries are no longer stubs (input/xpp_hle.h). The detailed tests are in tests/input_router_tests.cpp; this
// case only checks the hooked entries end to end with the process-wide input layer (default settings, no window:
// keyboard/mouse keeps port 1 connected, so the result does not depend on a real controller).
TEST_CASE("XPP HLE: port 1 connected, handles, synchronous rumble status") {
    const void* gamepad = guest::Ptr<const void>(guest::kXppDeviceTypeGamepad);
    const void* memoryUnit = guest::Ptr<const void>(guest::kXppDeviceTypeMemoryUnit);
    inp::XInitDevicesHle(1, nullptr);
    CHECK((inp::XGetDevicesHle(gamepad) & 1u) == 1u);
    CHECK(inp::XGetDevicesHle(memoryUnit) == 0);
    std::uint32_t ins = 0xFFFFFFFFu, rem = 0xFFFFFFFFu;
    CHECK(inp::XGetDeviceChangesHle(memoryUnit, &ins, &rem) == FALSE);
    CHECK(ins == 0);
    CHECK(rem == 0);
    void* handle = inp::XInputOpenHle(gamepad, 0, 0, nullptr);
    REQUIRE(handle != nullptr);
    CHECK(guest::Addr(handle) < 0x80000000u);
    inp::XInputCloseHle(nullptr);  // ignored

    inp::XInputState state;
    std::memset(&state, 0xAB, sizeof(state));
    CHECK(inp::XInputGetStateHle(nullptr, &state) == inp::kErrorInvalidParameter);
    CHECK(state.Gamepad.wButtons == 0);
    CHECK(inp::XInputGetStateHle(handle, &state) == inp::kErrorSuccess);

    inp::XInputFeedback fb{};
    fb.dwStatus = inp::kErrorIoPending;
    CHECK(inp::XInputSetStateHle(nullptr, &fb) == inp::kErrorInvalidParameter);
    CHECK(fb.dwStatus != inp::kErrorIoPending);  // rumble 0x9C229 can send again
    fb.dwStatus = inp::kErrorIoPending;
    CHECK(inp::XInputSetStateHle(handle, &fb) == inp::kErrorSuccess);  // motors 0: no real controller vibrates
    CHECK(fb.dwStatus == inp::kErrorSuccess);
    inp::XInputCloseHle(handle);
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
