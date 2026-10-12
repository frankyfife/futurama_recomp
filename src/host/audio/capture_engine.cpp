// Capture glue for the audio engine (see capture_engine.h).
#include "audio/capture_engine.h"

#include "core/config.h"
#include "core/log.h"

#include <cstdio>

namespace snd::capture {

Stem StemForTapPoint(TapPoint point) noexcept {
    switch (point) {
    case TapPoint::Master:
        return Stem::Mix;
    case TapPoint::Effects:
        return Stem::Effects;
    case TapPoint::Speech:
        return Stem::Speech;
    case TapPoint::Music:
        return Stem::Music;
    case TapPoint::Movie:
        return Stem::Movie;
    case TapPoint::Reverb:
        return Stem::Reverb;
    case TapPoint::Count:
        break;
    }
    return Stem::Count;
}

const char* SoundEventKindName(SoundEventKind kind) noexcept {
    switch (kind) {
    case SoundEventKind::Create:
        return "create";
    case SoundEventKind::Release:
        return "release";
    case SoundEventKind::Play:
        return "play";
    case SoundEventKind::Stop:
        return "stop";
    case SoundEventKind::StreamPause:
        return "pause";
    case SoundEventKind::StreamResume:
        return "resume";
    case SoundEventKind::Flush:
        return "flush";
    case SoundEventKind::Volume:
        return "volume";
    case SoundEventKind::Frequency:
        return "frequency";
    case SoundEventKind::Position3D:
        return "position";
    case SoundEventKind::Ended:
        return "end";
    case SoundEventKind::PacketsLate:
        return "late";
    case SoundEventKind::OrphanedObject:
        return "orphan";
    case SoundEventKind::Level3D:
        return "level";
    }
    return "?";
}

std::string SoundEventDetail(const SoundEvent& e) {
    char text[320];
    const int n = std::snprintf(text, sizeof(text), "%s%s%s voice=%u mB=%d hz=%u pos=%.3f,%.3f,%.3f frame=%llu",
                                e.isStream ? "stream" : "buffer", e.is3D ? " 3d" : "", e.looping ? " loop" : "",
                                e.voiceId, e.millibel, e.frequency, static_cast<double>(e.position.x),
                                static_cast<double>(e.position.y), static_cast<double>(e.position.z),
                                static_cast<unsigned long long>(e.outputFrame));
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(text)) {
        // Applied level (3D: volume + DSOUND distance term) and listener distance (3D only).
        if (e.is3D) {
            std::snprintf(text + n, sizeof(text) - static_cast<std::size_t>(n),
                          " level=%.1f dist=%.1f min=%g max=%g curve=%u", static_cast<double>(e.levelDb),
                          static_cast<double>(e.distance), static_cast<double>(e.minDistance),
                          static_cast<double>(e.maxDistance), e.rolloffPoints);
        } else {
            std::snprintf(text + n, sizeof(text) - static_cast<std::size_t>(n), " level=%.1f",
                          static_cast<double>(e.levelDb));
        }
    }
    return text;
}

void SessionMixTap::OnMix(TapPoint point, const float* samples, std::uint32_t frames, std::uint32_t channels,
                          std::uint32_t sampleRate) noexcept {
    const Stem stem = StemForTapPoint(point);
    if (stem == Stem::Count || !session_.Active()) {
        return;
    }
    // A no-op after the first block of the stem (its ring is allocated once).
    session_.DeclareStem(stem, channels, sampleRate);
    session_.Push(stem, samples, frames, channels);
}

void SessionEventSink::OnSoundEvent(const SoundEvent& e) noexcept {
    if (!session_.Active()) {
        return;
    }
    try {
        const std::string detail = SoundEventDetail(e);
        Event ev;
        ev.kind = SoundEventKindName(e.kind);
        ev.object = e.object;
        ev.category = static_cast<int>(e.category);
        ev.name = e.name != nullptr ? e.name : "";
        ev.detail = detail;
        session_.RecordEvent(ev);
    } catch (...) {
    }
}

bool AttachCapture(Session& session, const Settings& settings, AudioEngine& engine, SessionMixTap& tap,
                   SessionEventSink& sink) noexcept {
    const Source source = engine.HasDevice() ? Source::XAudio2 : Source::SoftwareMix;
    if (!session.Active() && !session.Start(settings, source)) {
        return false;
    }
    session.DeclareStem(Stem::Mix, engine.OutputChannels(), engine.OutputSampleRate(), engine.OutputChannelMask());
    // Every other stem now as well: its ring (about 1 s) is allocated here and not by the first block on the mixer
    // thread (real-time path: no allocation, no log line).
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(TapPoint::Count); ++i) {
        const auto point = static_cast<TapPoint>(i);
        std::uint32_t channels = 0;
        std::uint32_t rate = 0;
        if (point != TapPoint::Master && engine.TapFormat(point, channels, rate)) {
            session.DeclareStem(StemForTapPoint(point), channels, rate);
        }
    }
    engine.SetTap(&tap);
    if (session.settings().eventLog) {
        SetSoundEventSink(&sink);
    }
    FUT_LOGI(Snd, "audio capture: attached to the %s engine (%u ch, %u Hz)", engine.BackendName(),
             engine.OutputChannels(), engine.OutputSampleRate());
    return true;
}

namespace {

// The process observer: Session::Instance() + the configuration.
class CaptureObserver final : public EngineObserver {
public:
    void OnEngineCreated(AudioEngine& engine) noexcept override {
        Settings settings;
        try {
            settings = SettingsFromConfig(core::GetConfig());
        } catch (...) {
            return;
        }
        if (settings.Enabled()) {
            AttachCapture(Session::Instance(), settings, engine, tap_, sink_);
        }
    }
    void OnBackendChanged(AudioEngine& engine) noexcept override {
        Session::Instance().NoteSource(engine.HasDevice() ? Source::XAudio2 : Source::SoftwareMix);
    }

private:
    SessionMixTap tap_{Session::Instance()};
    SessionEventSink sink_{Session::Instance()};
};

CaptureObserver& ProcessObserver() noexcept {
    static CaptureObserver observer;
    return observer;
}

const bool g_observerRegistered = (SetEngineObserver(&ProcessObserver()), true);

}  // namespace

}  // namespace snd::capture
