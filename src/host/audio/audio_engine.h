// Audio engine internals (M5 DSOUND core agent; analysis/loader/m5_audio.md 6.1, 6.6, 6.8, 7). The public API is
// audio/audio_api.h (AudioEngine, SourceVoice, CreateAudioEngine, Engine()); this header only adds helpers for
// the [audio] keys, the software mixer factory and logs.
//
// Structure (audio_engine.cpp):
//   EngineFacade (the AudioEngine handed out) owns one backend and wraps every voice in a proxy that remembers its
//   parameters and queued buffers, so that the facade can move all voices to another backend on the guest thread
//   (Pump): XAudio2 -> software mixer when the device disappears (OnCriticalError, e.g. the TV is switched off),
//   software mixer -> XAudio2 when an endpoint is available again, XAudio2 -> XAudio2 when the [audio] device comes
//   back or (default device in use) Windows has a new default device. The buffer playing at such a switch continues
//   at its play position; the queued ones follow ([audio] device_test simulates loss and return). The slow part -
//   looking for endpoints, creating the XAudio2 backend, destroying the old one - runs on the host thread "audio
//   device worker" (design E10), which also registers an IMMNotificationClient; Pump only hands over (asks again
//   every 3 s while no device is open, at once on a device notification; after failed opens of a present device
//   3, 6, 12 .. 30 s).
//   XAudio2 backend: XAudio2 2.9, mastering voice 48 kHz (channels from [audio] speakers or the device) with one
//   effect: 120-Hz LFE low-pass + tap Master + frame counter + master stage (own peak limiter at -0.1 dBFS, master
//   volume); one submix per
//   category (effects, speech, music, movie; each with a tap) and the reverb submix (mono in, XAudio2 reverb from the
//   I3DL2 preset via reverb::ToNative, then a tap; effect channels mixed to the output by ReverbOutputMatrix). The
//   taps are pass-through XAPOs that hand every block to the MixTap (AudioEngine::SetTap; the capture session).
//   Software backend: the same graph without device (reverb through reverb::SoftwareReverb, the same XAPO), 16-tap
//   windowed-sinc resampler, rendered in 10 ms blocks by a host thread in real time (or by RenderSoftware in tests);
//   same MixTap calls and master stage. While nobody listens (no tap, no output buffer: no device and no
//   capture_wav) it only advances the play positions and buffer ends - same frames, same OnBufferEnd moments.
// Master volume ([audio] master_volume) is applied in the master stage after the Master tap (XAudio2 applies a
// mastering voice's own volume BEFORE its effect chain, measured): the limiter and every tap (capture_wav, stems)
// see the mix before it, so a quiet test run still captures the full level.
#pragma once

#include "audio/audio_api.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace snd {

// Software mixer engine (never opens a device). Tests use it with softwareRealtime = false.
std::unique_ptr<AudioEngine> CreateSoftwareEngine(const EngineConfig& config) noexcept;

// Speaker mask for a channel count (1 mono, 2 stereo, 4 quad, 6 5.1, 8 7.1; else the first n bits).
std::uint32_t DefaultChannelMask(std::uint32_t channels) noexcept;
// [audio] speakers: "auto" -> 0, "stereo" -> 2, "5.1" -> 6 (case-insensitive). False for other text.
bool ParseSpeakers(std::string_view text, std::uint32_t& channels) noexcept;

// One INFO line about the process engine (backend, device, layout, counters); called by snd::LogDsoundSummary
// (exit and crash reporter "xdk"). Uses try-locks only.
void LogEngineSummary() noexcept;

// Tests (engines from CreateAudioEngine / CreateSoftwareEngine): marks an XAudio2 backend as lost, as
// OnCriticalError(XAUDIO2_E_DEVICE_INVALIDATED) would (the next Pump switches to the software mixer);
// RetryDeviceNowForTesting asks the device worker at once and waits for its answer (the next Pump hands it over).
void SimulateDeviceLossForTesting(AudioEngine& engine) noexcept;
// Moves every voice to a new software backend (same config) like a device switch, without a device (tests).
void SwitchToNewSoftwareBackendForTesting(AudioEngine& engine) noexcept;
void RetryDeviceNowForTesting(AudioEngine& engine) noexcept;
// The engine's description as in the "audio:" log line (backend, device name and how it was chosen, layout).
std::string EngineDescriptionForTesting(const AudioEngine& engine);

}  // namespace snd
