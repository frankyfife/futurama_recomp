// Kernel HLE configuration (values the guest can query). Owned by the kernel agent.
//
// futurama.ini keys read by krnl::StageInitKernel (all optional):
//   [settings]
//   language     = english     ; ExQueryNonVolatileSetting 7 (XGetLanguage): english 1, japanese 2,
//                              ; german 3, french 4, spanish 5, italian 6, korean 7, tchinese 8,
//                              ; portuguese 9 or a number 1..9. The game itself knows en/de/fr/es/it
//                              ; (files.md 4.5: 3 de, 4 fr, 5 es, 6 it, everything else english).
//   game_region  = 4           ; index 0x104 (XGetGameRegion): 1 NA, 2 Japan, 4 rest of world (EU)
//   av_region    = 0x00800300  ; index 0x103: PAL-I (ASSUMPTION: XDK value; not queried by this XBE)
//   video_flags  = 0           ; index 8 (not queried by this XBE, files.md 4.5)
//   audio_flags  = 0           ; index 9 (XGetAudioFlags, only DSOUND): 0 = stereo (ASSUMPTION)
//   av_pack      = 6           ; DATA HalBootSMCVideoMode (XGetAVPack): 6 = standard/composite
//                              ; (design 1.5: value 6 is an ASSUMPTION; must not be 3)
//   [saves]
//   key_mode      = fixed      ; fixed (default) | xbox   (design 4.5)
//   signature_key = <32 hex>   ; fixed mode: overrides the built-in project key
//   xbox_cert_key = <32 hex>   ; xbox mode: XboxCERTKey of a real console (required for xbox mode)
//   [kernel]
//   min_guest_stack = 0x200000 ; PsCreateSystemThreadEx stack reservation minimum (design 1.7)
#pragma once

#include "core/config.h"

#include <array>
#include <cstdint>
#include <string>

namespace krnl {

enum class SaveKeyMode : std::uint8_t { Fixed, Xbox };

struct KernelSettings {
    // ExQueryNonVolatileSetting values (design 2.1 "Einstellungen", files.md 4.5, kernel.md 7.5)
    std::uint32_t language = 1;           // index 7   XC_LANGUAGE (1 = English)
    std::uint32_t gameRegion = 4;         // index 0x104 XC_FACTORY_GAME_REGION (4 = rest of world)
    std::uint32_t avRegion = 0x00800300;  // index 0x103 XC_FACTORY_AV_REGION (PAL-I)
    std::uint32_t videoFlags = 0;         // index 8   XC_VIDEO_FLAGS
    std::uint32_t audioFlags = 0;         // index 9   XC_AUDIO_FLAGS
    std::uint32_t avPack = 6;             // DATA HalBootSMCVideoMode
    // Saves (design 4.5)
    SaveKeyMode keyMode = SaveKeyMode::Fixed;
    bool hasSignatureKeyOverride = false;
    std::array<std::uint8_t, 16> signatureKeyOverride{};
    bool hasXboxCertKey = false;
    std::array<std::uint8_t, 16> xboxCertKey{};
    // Threads (design 1.7)
    std::uint32_t minGuestStack = 0x200000;
};

// Parses the keys above; returns false with a message for invalid values.
bool LoadKernelSettings(const core::Config& config, KernelSettings& out, std::string& error);

// Current settings (defaults until StageInitKernel ran; tests may replace them).
const KernelSettings& Settings() noexcept;
void SetSettings(const KernelSettings& settings) noexcept;

// "english", "german", ... for logs (nullptr for unknown values).
const char* LanguageName(std::uint32_t language) noexcept;

}  // namespace krnl
