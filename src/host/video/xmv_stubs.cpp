// XMV stubs (M2-M5): every movie ends on its first frame (see xmv_stubs.h). Callers are the game's
// movie player (avi.md 3.2): Open FUN_000EE220 (create, descriptor, audio), Update FUN_000EDE90 (next
// frame, playback time), Close FUN_000EE080 (close). "player" = the 0x48-byte player object
// (FUN_000EE050 ctor): +0x00 done flag, +0x18 decoder, +0x1C..+0x28 video descriptor.
#include "video/xmv_stubs.h"

#include "audio/xdk_entries.h"
#include "core/guest.h"
#include "core/log.h"

#include <atomic>
#include <cstring>

namespace vid {
namespace {

std::uint32_t A(const volatile void* p) noexcept {
    return guest::Addr(p);
}

constexpr std::int32_t kS_OK = 0;
constexpr std::int32_t kE_Fail = static_cast<std::int32_t>(0x80004005);
constexpr std::int32_t kE_OutOfMemory =
    static_cast<std::int32_t>(0x8007000E);  // original OOM code (0x2643A4)

// Guest-visible dummy decoder; the game treats the decoder pointer as opaque (only passed to XMV entries:
// 0xEE389, 0xEE412, 0xEE424, 0xEDEDB, 0xEDFB7, 0xEE11F), so +0 is the trap vtable.
struct StubDecoder {
    std::uint32_t vtable;
    std::uint32_t magic;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t audioStreamCount;
    std::uint32_t frameCalls;
};
constexpr std::uint32_t kDecoderMagic = 0x56444D58;  // 'XMDV'
static_assert(sizeof(StubDecoder) <= xdk::kGuestObjectSize);

std::atomic<std::uint32_t> g_liveDecoders{0};
std::atomic<std::uint32_t> g_moviesSkipped{0};

StubDecoder* AsDecoder(void* p) noexcept {
    if (p == nullptr || !xdk::IsGuestObject(p)) {
        return nullptr;
    }
    auto* d = static_cast<StubDecoder*>(p);
    return d->magic == kDecoderMagic ? d : nullptr;
}

}  // namespace

bool IsStubDecoder(const void* decoder) noexcept {
    return AsDecoder(const_cast<void*>(decoder)) != nullptr;
}

std::uint32_t LiveStubDecoders() noexcept {
    return g_liveDecoders.load();
}

// XMV::XMVDecoder_CreateDecoderForPackets 0x264360 (name assumed, avi.md 3.1) - stdcall (DWORD dwFlags,
// const header* pvFirstPacketHeader, void* context, pfnGetNextPacket, pfnReleasePreviousPacket,
// XMVDecoder** ppDecoder), HRESULT, ret 24. Only caller: movie Open 0xEE35C with (0, &hdr (0x1000 bytes
// read from "Video\<name>.xmv"), player, 0xEDBA0, 0xEDE60, &player+0x18); `test eax,eax; jge 0xEE373`
// continues on success, otherwise Close + return 0 (0xEE365). The original reads W/H at +0x14/+0x18 and
// the audio stream count (u16) at +0x20 (0x264360 decomp) and writes *ppDecoder only on success.
// Success with a NON-NULL decoder is required: Update 0xEDE9F returns without setting the done flag when
// player+0x18 == 0, the movie would never end. W/H come from the header because Open creates the
// texture with them (D3DDevice_CreateTexture2(W, H, 1, 1, 0, 0x24, 3), shim) and fails the movie for a
// NULL texture. The packet callbacks are never called (no packet beyond the first is read).
// A header without the 'xobX' magic (all 47 PAL movies have it, avi.md 3.3) gets E_FAIL -> movie skipped
// through Open's failure path.
std::int32_t __stdcall XMVDecoder_CreateDecoderForPacketsHle(std::uint32_t flags, const XmvFileHeader* header,
                                                             void* context, const void* getNextPacket,
                                                             const void* releasePreviousPacket,
                                                             void** ppDecoder) noexcept {
    std::int32_t hr = kS_OK;
    StubDecoder* decoder = nullptr;
    XmvFileHeader h{};
    if (header == nullptr || !guest::IsReadable(A(header), sizeof(h))) {
        hr = kE_Fail;
    } else {
        std::memcpy(&h, header, sizeof(h));
        if (h.magic != kXmvMagic) {
            hr = kE_Fail;
            FUT_LOGW(Vid, "XMV: first packet header without 'xobX' magic (0x%08X) - movie skipped", h.magic);
        }
    }
    if (hr == kS_OK) {
        decoder = static_cast<StubDecoder*>(xdk::AllocGuestObject(sizeof(StubDecoder)));
        if (decoder == nullptr) {
            hr = kE_OutOfMemory;
        } else {
            decoder->vtable = A(xdk::TrapVtable());
            decoder->magic = kDecoderMagic;
            decoder->width = h.width;
            decoder->height = h.height;
            decoder->audioStreamCount = h.audioStreamCount;
            g_liveDecoders.fetch_add(1);
            if (ppDecoder != nullptr) {
                *ppDecoder = decoder;
            }
        }
    }
    if (hr == kS_OK) {
        const std::uint32_t n = g_moviesSkipped.fetch_add(1) + 1;
        if (n == 1) {
            FUT_LOGI(Vid,
                     "XMV: stub decoder - movies are skipped (GetNextFrame reports end of file) until M6");
        }
        FUT_LOGD(Vid, "XMV: movie #%u skipped: %ux%u, %u ms, %u audio tracks, version %u", n, h.width,
                 h.height, h.durationMs, h.audioStreamCount, h.version);
    }
    XDK_TRACE_CALL(
        XMVDecoder_CreateDecoderForPackets,
        "(flags=0x%X, header=0x%08X, ctx=0x%08X, next=0x%08X, release=0x%08X, ppDecoder=0x%08X) -> 0x%08X, "
        "*pp=0x%08X",
        flags, A(header), A(context), A(getNextPacket), A(releasePreviousPacket), A(ppDecoder),
        static_cast<std::uint32_t>(hr), A(decoder));
    return hr;
}
XDK_HOOK(XMVDecoder_CreateDecoderForPackets, &XMVDecoder_CreateDecoderForPacketsHle);

// XMV::XMVDecoder_CloseDecoder 0x264677 (name assumed) - stdcall (XMVDecoder*), void, ret 4. Only caller:
// movie Close 0xEE11F, only for player+0x18 != 0, result ignored (next: mov [esi+0x18], ebx). Frees the
// dummy decoder (the original flushes and releases its DSOUND streams first - there are none).
void __stdcall XMVDecoder_CloseDecoderHle(void* decoder) noexcept {
    StubDecoder* d = AsDecoder(decoder);
    if (d != nullptr) {
        d->magic = 0;
        xdk::FreeGuestObject(d);
        g_liveDecoders.fetch_sub(1);
    }
    XDK_TRACE_CALL(XMVDecoder_CloseDecoder, "(decoder=0x%08X)%s", A(decoder),
                   d != nullptr ? "" : " (unknown decoder)");
}
XDK_HOOK(XMVDecoder_CloseDecoder, &XMVDecoder_CloseDecoderHle);

// XMV::XMVDecoder_GetVideoDescriptor 0x264759 (name assumed) - stdcall (XMVDecoder*, desc*), void, ret 8.
// Original (decomp 0x264759): desc[0] = W, desc[1] = H, desc[3] = audio stream count, desc[2] untouched.
// Only caller: Open 0xEE389 with &player+0x1C; Open then uses +0x28 (count: tracks only for 1..5,
// 0xEE3D1..0xEE3E7) and +0x1C/+0x20 (texture size, 0xEE4AA). Values of the header passed to Create.
void __stdcall XMVDecoder_GetVideoDescriptorHle(void* decoder, XmvVideoDescriptor* desc) noexcept {
    const StubDecoder* d = AsDecoder(decoder);
    if (desc != nullptr) {
        desc->width = d != nullptr ? d->width : 0;
        desc->height = d != nullptr ? d->height : 0;
        desc->audioStreamCount = d != nullptr ? d->audioStreamCount : 0;
    }
    XDK_TRACE_CALL(XMVDecoder_GetVideoDescriptor, "(decoder=0x%08X, desc=0x%08X) -> %ux%u, %u audio streams",
                   A(decoder), A(desc), d != nullptr ? d->width : 0u, d != nullptr ? d->height : 0u,
                   d != nullptr ? d->audioStreamCount : 0u);
}
XDK_HOOK(XMVDecoder_GetVideoDescriptor, &XMVDecoder_GetVideoDescriptorHle);

// XMV::XMVDecoder_EnableAudioStream 0x2647C6 (name assumed) - stdcall (XMVDecoder*, DWORD iStream, DWORD
// dwFlags, LPCDSMIXBINS, LPDIRECTSOUNDSTREAM* ppStream), HRESULT, ret 20. Only caller: Open 0xEE412 with
// (dec, track (language for 5 tracks, FUN_000EDBE0), 0, 0, 0); result ignored (next: mov ebx,[ebx]).
// No audio: S_OK, *ppStream = NULL if requested (same "*pp = NULL" rule as DirectSoundCreateStream).
std::int32_t __stdcall XMVDecoder_EnableAudioStreamHle(void* decoder, std::uint32_t stream,
                                                       std::uint32_t flags, const void* mixBins,
                                                       void** ppStream) noexcept {
    if (ppStream != nullptr) {
        *ppStream = nullptr;
    }
    XDK_TRACE_CALL(XMVDecoder_EnableAudioStream,
                   "(decoder=0x%08X, stream=%u, flags=0x%X, mixbins=0x%08X, ppStream=0x%08X) -> S_OK",
                   A(decoder), stream, flags, A(mixBins), A(ppStream));
    return kS_OK;
}
XDK_HOOK(XMVDecoder_EnableAudioStream, &XMVDecoder_EnableAudioStreamHle);

// XMV::XMVDecoder_GetAudioStream 0x264943 (name assumed) - stdcall (XMVDecoder*, DWORD iStream,
// LPDIRECTSOUNDSTREAM* ppStream), void, ret 12 (original: *pp = streams[i] + AddRef, eax not set).
// Only caller: Open 0xEE424 with a zeroed local; `cmp [esp+0x10], edi; je 0xEE4A3` skips SetVolume and
// the virtual Release for NULL. *ppStream = NULL.
void __stdcall XMVDecoder_GetAudioStreamHle(void* decoder, std::uint32_t stream, void** ppStream) noexcept {
    if (ppStream != nullptr) {
        *ppStream = nullptr;
    }
    XDK_TRACE_CALL(XMVDecoder_GetAudioStream, "(decoder=0x%08X, stream=%u, ppStream=0x%08X) -> *pp=NULL",
                   A(decoder), stream, A(ppStream));
}
XDK_HOOK(XMVDecoder_GetAudioStream, &XMVDecoder_GetAudioStreamHle);

// XMV::XMVDecoder_GetNextFrame 0x264962 (name assumed) - stdcall (XMVDecoder*, D3DSurface*, XMVRESULT*
// pResult, DWORD* pTimestamp), HRESULT, ret 16. Only caller: Update 0xEDEDB (called once at the end of
// Open and per frame) with the texture's level-0 surface, &local, 0; the HRESULT is ignored (next: push
// edi; call D3DResource_Release). *pResult == 2 (end of file; the original returns 2 without touching
// pTimestamp, decomp 0x264962 lines "*param_3 = 2; return 0") makes Update set the done flag
// (0xEDEEA -> player+0 = 1), after which App slot 7 (0xA24CD FUN_000EDBD0 = done flag) destroys the
// player - the movie is skipped without drawing a frame (design 6 M2: "XMV (Ergebnis 2)").
std::int32_t __stdcall XMVDecoder_GetNextFrameHle(void* decoder, void* surface, std::int32_t* result,
                                                  std::uint32_t* timestamp) noexcept {
    if (StubDecoder* d = AsDecoder(decoder)) {
        ++d->frameCalls;
    }
    if (result != nullptr) {
        *result = kXmvEndOfFile;
    }
    XDK_TRACE_CALL(XMVDecoder_GetNextFrame,
                   "(decoder=0x%08X, surface=0x%08X, pResult=0x%08X, pTimestamp=0x%08X) -> S_OK, "
                   "*pResult=2 (end of file)",
                   A(decoder), A(surface), A(result), A(timestamp));
    return kS_OK;
}
XDK_HOOK(XMVDecoder_GetNextFrame, &XMVDecoder_GetNextFrameHle);

// XMV::XMVDecoder_GetPlaybackTimeMs 0x264FDF (name assumed) - stdcall (XMVDecoder*), DWORD ms, ret 4.
// Only caller: Update 0xEDFB7 for subtitles (player+0x44), reached only for results 0/1 - never with this
// stub. The original returns 0 before playback started (decomp 0x264FDF). Returns 0.
std::uint32_t __stdcall XMVDecoder_GetPlaybackTimeMsHle(void* decoder) noexcept {
    XDK_TRACE_CALL(XMVDecoder_GetPlaybackTimeMs, "(decoder=0x%08X) -> 0", A(decoder));
    return 0;
}
XDK_HOOK(XMVDecoder_GetPlaybackTimeMs, &XMVDecoder_GetPlaybackTimeMsHle);

}  // namespace vid
