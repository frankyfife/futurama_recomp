// The XAudio2 reverb outside an XAudio2 graph, for the device-less software mixer (M5 reverb agent;
// m5_audio.md 6.8).
//
// Without an audio endpoint (the TV off; automated checks) XAudio2 cannot create a mastering voice and the
// engine mixes in software. SoftwareReverb drives the same reverb XAPO (XAudio2CreateReverb) directly through
// IXAPO (Initialize, LockForProcess, Process) with the same parameters (reverb_i3dl2.h ToNative), so a
// headless capture contains the same reverb as a run with a device: mono float input (the I3DL2 send), 1 or 6
// float output channels (ReverbEffectChannels), 20000..48000 Hz, processed in 10 ms blocks.
//
// Threading: Initialize/SetListener/Process/Reset from one thread at a time (the software mixer thread);
// SetListener may also be called from another thread - the parameters are handed over under a lock and
// applied before the next block.
#pragma once

#include "audio/audio_api.h"

#include <cstdint>
#include <memory>

namespace snd::reverb {

class SoftwareReverb {
public:
    SoftwareReverb() noexcept;
    ~SoftwareReverb();
    SoftwareReverb(const SoftwareReverb&) = delete;
    SoftwareReverb& operator=(const SoftwareReverb&) = delete;

    // outputChannels: 1 or 6; sampleRate within 20000..48000. False if the XAPO cannot be created or locked.
    bool Initialize(std::uint32_t sampleRate, std::uint32_t outputChannels) noexcept;
    bool Ready() const noexcept;
    std::uint32_t OutputChannels() const noexcept;
    std::uint32_t SampleRate() const noexcept;
    // Frames of one XAPO block (SampleRate / 100 = 10 ms). Process() should be called with multiples of it:
    // the XAPO always processes whole blocks, a remainder is processed as a zero-padded block (its tail is
    // lost).
    std::uint32_t BlockFrames() const noexcept;
    // Environment (clamped and converted like the XAudio2 path). Before the first call: I3DL2 "Generic".
    void SetListener(const I3dl2Listener& listener) noexcept;
    // `in`: `frames` mono samples (nullptr = silence); `out`: frames * OutputChannels() interleaved,
    // overwritten.
    void Process(const float* in, float* out, std::uint32_t frames) noexcept;
    // Clears the delay lines (tail).
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snd::reverb
