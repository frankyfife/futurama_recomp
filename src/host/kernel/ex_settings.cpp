// ExQueryNonVolatileSetting (slot 17). Owned by the kernel agent.
//
// Answered indices (design 2.1 "Einstellungen", kernel.md 7.5, files.md 4.5, startup.md 9.2). The
// names follow the XDK XC_* constants (ASSUMPTION); every value is a DWORD (type REG_DWORD = 4):
//   0x007 XC_LANGUAGE               [settings] language (default 1 = English). XGetLanguage 0xF6440;
//                                   the game maps 3 de, 4 fr, 5 es, 6 it, else english (FUN_000d9990).
//   0x008 XC_VIDEO_FLAGS            [settings] video_flags (default 0). Not queried by this XBE
//                                   (files.md 4.5: no XGetVideoFlags); answered for completeness.
//   0x009 XC_AUDIO_FLAGS            [settings] audio_flags (default 0 = stereo). XGetAudioFlags 0x1707A0
//                                   (only DSOUND FUN_00248536).
//   0x00A XC_PARENTAL_CONTROL_GAMES always 0: XGetParentalControlSetting 0xF9C3E is compared with the
//                                   certificate GameRatings 0 in XapiInitProcess (0xF93E8); > 0 would
//                                   boot to the dashboard (kernel.md Verifikation). Not configurable.
//   0x011 XC_MISC_FLAGS             always 0: XQueryValue(0x11) in FUN_000f9b1a, bit 0 = AutoPowerDown
//                                   (kernel.md 8.6); 0 keeps the 6 h shutdown timer disarmed.
//   0x103 XC_FACTORY_AV_REGION      [settings] av_region (default 0x00800300 PAL-I). Not queried by
//                                   this XBE; answered for completeness.
//   0x104 XC_FACTORY_GAME_REGION    [settings] game_region (default 4 = rest of world/Europe).
//                                   XGetGameRegion 0xF8554, used by XGetLanguage only if language > 9.
// Every other index (e.g. 0xFFFF = whole EEPROM, only read by FUN_000f6ffa when XboxHDKey is all
// zero - it is not) -> STATUS_OBJECT_NAME_NOT_FOUND plus a warning (design 2.1).
#include "core/log.h"
#include "kernel/ke_types.h"
#include "kernel/kernel_api.h"
#include "kernel/kernel_exports.h"
#include "kernel/kernel_settings.h"

#include <intrin.h>

#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace krnl {
namespace {

struct SettingValue {
    bool found;
    std::uint32_t value;
    const char* name;
};

SettingValue Lookup(ULONG index) noexcept {
    const KernelSettings& s = Settings();
    switch (index) {
    case 0x007:
        return {true, s.language, "XC_LANGUAGE"};
    case 0x008:
        return {true, s.videoFlags, "XC_VIDEO_FLAGS"};
    case 0x009:
        return {true, s.audioFlags, "XC_AUDIO_FLAGS"};
    case 0x00A:
        return {true, 0, "XC_PARENTAL_CONTROL_GAMES"};
    case 0x011:
        return {true, 0, "XC_MISC_FLAGS"};
    case 0x103:
        return {true, s.avRegion, "XC_FACTORY_AV_REGION"};
    case 0x104:
        return {true, s.gameRegion, "XC_FACTORY_GAME_REGION"};
    default:
        return {false, 0, nullptr};
    }
}

}  // namespace

// ExQueryNonVolatileSetting - slot 17, Ord 24, stdcall, 5 args (purge 20):
// NTSTATUS (ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength OPTIONAL).
// Evidence: kernel_imports.tsv; all callers go through the stub 0xF9CC6: XGetLanguage (7),
// XGetGameRegion (0x104), XGetParentalControlSetting (0xA), XGetAudioFlags (9), XQueryValue 0xF8AC0
// (0x11 from FUN_000f9b1a, 0xFFFF from FUN_000f6ffa); each passes (index, &type, &value, 4, 0).
NTSTATUS __stdcall ExQueryNonVolatileSetting(ULONG ValueIndex, ULONG* Type, void* Value, ULONG ValueLength,
                                             ULONG* ResultLength) noexcept {
    const SettingValue v = Lookup(ValueIndex);
    if (!v.found) {
        FUT_LOGW(Krnl,
                 "ExQueryNonVolatileSetting(0x%lX, len %lu): unknown index -> STATUS_OBJECT_NAME_NOT_FOUND "
                 "(ret=%p)",
                 ValueIndex, ValueLength, _ReturnAddress());
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (ResultLength != nullptr) {
        *ResultLength = sizeof(std::uint32_t);
    }
    if (Value == nullptr || ValueLength < sizeof(std::uint32_t)) {
        FUT_LOGW(Krnl, "ExQueryNonVolatileSetting(0x%lX %s): buffer %p/%lu too small", ValueIndex, v.name,
                 Value, ValueLength);
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Type != nullptr) {
        *Type = guestdef::kRegDword;
    }
    std::memcpy(Value, &v.value, sizeof(v.value));
    FUT_LOGD(Krnl, "ExQueryNonVolatileSetting(0x%lX %s) -> 0x%X", ValueIndex, v.name, v.value);
    return STATUS_SUCCESS;
}
KRNL_FUNC(ExQueryNonVolatileSetting, &ExQueryNonVolatileSetting);

}  // namespace krnl
