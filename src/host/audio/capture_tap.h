// Capture tap (M5 tooling agent, src/INTERFACES.md 4.29): an in-place XAPO that copies every processing block of
// the voice it sits on into a capture session stem and passes the audio on unchanged, plus the lock-free ring the
// session uses between the producer (XAudio2 engine thread / software mixer) and its writer thread.
//
// Registration properties: in place required, channels/frame rate/bits/buffer count must match, exactly one input
// and one output buffer, 32-bit float only (XAudio2's internal format). XAPO_BUFFER_SILENT input is captured as
// zeros. Process never blocks and never allocates.
//
// Not used by the M5 engine (audio_engine.cpp has its own TapXapo feeding Session::Push through its MixTap); kept
// for other XAudio2 graphs and the tests. Usage (e.g. on a mastering voice, last effect of the chain):
//   IUnknown* tap = snd::capture::Session::Instance().CreateTap(snd::capture::Stem::Mix);
//   XAUDIO2_EFFECT_DESCRIPTOR d{tap, TRUE, outputChannels};      // after the limiter
//   XAUDIO2_EFFECT_CHAIN chain{n, descriptors}; master->SetEffectChain(&chain); tap->Release();
#pragma once

#include "audio/capture_session.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace snd::capture {

// Single producer / single consumer ring of floats. Write is all-or-nothing (blocks keep their frame alignment).
class FloatRing {
public:
    // Capacity is rounded up to a power of two (samples). Not thread safe against Write/Read.
    bool Allocate(std::size_t minSamples) noexcept;
    std::size_t Capacity() const noexcept { return mask_ + 1; }
    bool Allocated() const noexcept { return data_ != nullptr; }
    // Producer: copies n samples (nullptr = zeros); false (nothing written) if they do not fit.
    bool Write(const float* samples, std::size_t n) noexcept;
    // Consumer: copies up to max samples, returns the number copied.
    std::size_t Read(float* out, std::size_t max) noexcept;
    std::size_t Available() const noexcept;

private:
    std::unique_ptr<float[]> data_;
    std::size_t mask_ = 0;
    std::atomic<std::size_t> head_{0};  // written (producer)
    std::atomic<std::size_t> tail_{0};  // read (consumer)
};

// The tap XAPO for `stem` of `session` (refcount 1); `session` must outlive it. Normally created through
// Session::CreateTap. The returned object implements IXAPO (QueryInterface IID_IXAPO / IID_IUnknown).
IUnknown* CreateCaptureTap(Session& session, Stem stem) noexcept;

// Statistics of all taps created in this process (tests, log): Process calls and frames seen.
struct TapCounters {
    std::uint64_t processCalls = 0;
    std::uint64_t frames = 0;
    std::uint64_t silentBlocks = 0;
    std::uint32_t lockedTaps = 0;
};
TapCounters GetTapCounters() noexcept;

}  // namespace snd::capture
