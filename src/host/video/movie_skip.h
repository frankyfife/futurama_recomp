// Movie skipping (M6, user requirement "ALL videos must be skippable"): [game] skip_videos = always | original.
//
// The game skips movies on A/START in three places (m4_input.md 4.4, 4.5):
//   - Intro (logos fox/sci/uds, intro movie): Intro::Update 0x2B9B0, always skippable.
//   - Attract mode (Attract-PAL.xmv): any input returns to the front end.
//   - Cut scenes of the level scripts (1-1-0n.avi -> Video\1-1-0n-PAL.xmv, ...): the session update asks the skip
//     condition FUN_000D3120 (guest::kGameCutsceneSkipAllowed, only call 0x2ABD4) after PressedFree(START/A). The
//     original refuses the first viewing of 1-1-01/1-1-02 and any cut scene in its first 2.5 s (unless the cheat
//     flag 0x2EB7D4 is set).
// `always` (default) makes FUN_000D3120 return TRUE through a probe hook (the original runs for `original`); the
// skip itself stays the game's (FUN_000D5700 -> App slot 6 -> the movie player closes, XMV CloseDecoder).
#pragma once

#include <cstdint>
#include <string_view>

namespace vid {

enum class SkipVideos : std::uint8_t { Always, Original };

// "always" / "original" (ASCII case-insensitive). False for anything else.
bool ParseSkipVideos(std::string_view text, SkipVideos& out) noexcept;
const char* SkipVideosName(SkipVideos mode) noexcept;
// [game] skip_videos of the active configuration (default Always; an invalid value logs once and uses Always).
SkipVideos SkipVideosFromConfig() noexcept;

// The decision of the hooked skip condition: `original` = what FUN_000D3120 returned.
inline bool CutsceneSkipAllowed(SkipVideos mode, bool original) noexcept {
    return mode == SkipVideos::Always || original;
}

struct SkipStats {
    std::uint32_t asked = 0;       // calls of the skip condition (A/START pressed during a cut scene)
    std::uint32_t originalNo = 0;  // the original condition said "not yet / not on first viewing"
    std::uint32_t forced = 0;      // ... and skip_videos = always allowed it anyway
};
SkipStats GetSkipStats() noexcept;

}  // namespace vid
