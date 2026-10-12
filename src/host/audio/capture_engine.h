// Glue between the audio engine of audio/audio_api.h and the capture session (M5 tooling agent, src/INTERFACES.md
// 4.30): an EngineObserver registered at static initialisation (snd::SetEngineObserver) that, when the process
// engine is created and [audio] capture_wav is set,
//   * starts Session::Instance() (source "xaudio2" when the engine has a device, else "software-mix"),
//   * declares the mix stem with the engine's output format and registers a MixTap (AudioEngine::SetTap) that forwards
//     every TapPoint block to the session stems (Master -> mix, Effects/Speech/Music/Movie -> the category stems,
//     Reverb -> reverb),
//   * with [audio] event_log = 1 registers a SoundEventSink (snd::SetSoundEventSink) that writes the event list;
// and notes backend switches (device lost / available again) as a source change. The session itself is finalized by
// its exit reporter "audio capture". Nothing here touches guest memory.
#pragma once

#include "audio/audio_api.h"
#include "audio/capture_session.h"

namespace snd::capture {

// TapPoint -> stem (Master -> Mix, Effects -> Effects, ...; Count for an unknown point).
Stem StemForTapPoint(TapPoint point) noexcept;
// SoundEventKind -> event list word: create, release, play, stop, pause, resume, flush, volume, frequency, position,
// end, late, orphan.
const char* SoundEventKindName(SoundEventKind kind) noexcept;
// The "detail" column of an event: buffer|stream [3d] [loop], voice, mB, hz, position, engine output frame.
std::string SoundEventDetail(const SoundEvent& event);

// MixTap forwarding into a session (DeclareStem on the first block of a tap point, then Push). Mixer thread.
class SessionMixTap final : public MixTap {
public:
    explicit SessionMixTap(Session& session) noexcept : session_(session) {}
    void OnMix(TapPoint point, const float* samples, std::uint32_t frames, std::uint32_t channels,
               std::uint32_t sampleRate) noexcept override;

private:
    Session& session_;
};

// SoundEventSink writing Session::RecordEvent lines (guest thread).
class SessionEventSink final : public SoundEventSink {
public:
    explicit SessionEventSink(Session& session) noexcept : session_(session) {}
    void OnSoundEvent(const SoundEvent& event) noexcept override;

private:
    Session& session_;
};

// Attaches `session` (started from `settings`) to `engine`; false if the capture is off. Used by the process observer
// with Session::Instance() and the configuration, and by tests with their own session and engine.
bool AttachCapture(Session& session, const Settings& settings, AudioEngine& engine, SessionMixTap& tap,
                   SessionEventSink& sink) noexcept;

}  // namespace snd::capture
