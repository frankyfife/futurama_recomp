// M3d: game code that runs once per displayed frame instead of once per fixed step and misbehaves when frames are
// shown between two ticks (frame rate above the tick rate; analysis/loader/m3d_frameclock.md 1.3 and 4.2). Owned by
// the frame-clock/pacing agent. Every fix acts only while xapi::SubTickFramesActive() (xapi/game_timing.h) - at
// 60 fps with the 60 Hz tick the probes pass everything through and the replacement computes exactly what the
// original computes, so that mode stays bit-identical.
//
//   F2 probe:Death::Render      0x2BF10 (Death context vf3; thiscall, plain ret, prologue 56 8B F1 8B 4E 3C)
//      [this+0x3C]->vf1([this+0x20]) adds the seconds since the last Death::Update ([this+0x20], written by
//      Death::Update 0x2C1EB `fstp [esi+0x20]`) to the death screen's time and ends it after 2.5/3.0 s. Rendered
//      twice per tick, the time ran twice as fast. After the original the probe sets [this+0x20] = 0.0f: every Update
//      time is added once, the screen is still drawn every frame.
//   F3 probe:Session::OccluderFade  FUN_000CD340 (fastcall ecx, plain ret 0xCD5E0, prologue 83 EC 3C 56 8B F1; only
//      caller FUN_000CE2B0 from Session::Render): fades an object between camera and player by a fixed amount per
//      call, without dt. Not called in a frame that ran no fixed step: the fade keeps the 60 Hz raster.
//   F4 hook:Session::RandomFlicker  FUN_00045EF0 (fastcall ecx, plain ret; replacement - the prologue's `call rand`
//      at +3 cannot go into a trampoline): [this+0x24] = rand() & 1 with the compiler's sign fix, [this+0xA8] = 1.
//      Called once per Session::Render (0xC92F1), so at 120 fps the flicker ran twice as often and the logic's
//      global rand() sequence (CRT 0x1668CC, state in the per-thread block +0x14) depended on the frame rate. A frame
//      without a fixed step keeps [this+0x24] and does not call rand (it still sets [this+0xA8] = 1 like the tick
//      frame did); otherwise the hook calls the guest's rand through CallGuest and stores the same values.
// Not changed (m3d_frameclock.md 4.2 F5, optional): the 5 cleared frames after an intermission. F1 (frames without a
// step that draw nothing) is the frame pacer's (platform/frame_pacer.h repeated picture).
// Exit line "frame fixes: ..." (exit reporter "frame fixes", registered by the stage ApplyGameTiming).
#pragma once

#include <cstdint>

namespace xapi {

// The value FUN_00045EF0 stores at +0x24 for a rand() result: r & 0x80000001, negative values -> ((v - 1) | ~1) + 1
// (the compiler's signed modulo 2; rand() is 0..0x7FFF, so in practice r & 1).
std::uint32_t FlickerValue(std::uint32_t randResult) noexcept;

struct FrameFixStats {
    std::uint64_t deathTimeCleared = 0;    // F2: Death::Render calls followed by +0x20 = 0
    std::uint64_t occluderFadeCalls = 0;   // F3: calls of FUN_000CD340
    std::uint64_t occluderFadeSkipped = 0; // ... skipped in frames without a fixed step
    std::uint64_t flickerCalls = 0;        // F4: calls of FUN_00045EF0
    std::uint64_t flickerKept = 0;         // ... in frames without a fixed step (no rand call)
};
FrameFixStats GetFrameFixStats() noexcept;
void LogFrameFixSummary() noexcept;  // "frame fixes: ..." (GAME/info)

}  // namespace xapi
