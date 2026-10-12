// The input router of the PC port (input_api.h InputRouter; analysis\loader\m4_input.md 8.1).
//
// - Sources: the global registry (RegisterSource: XInput controllers and keyboard/mouse of input_system.cpp, the
//   input script, injected devices) or an explicit list (tests). Each source feeds port DefaultPort().
// - A port is connected while one of its sources is connected (keyboard/mouse is always connected, so port 1 never
//   reports a removal - no "connect the controller" pause without a controller), unless SimulateUnplug holds it off.
//   PortEpoch counts its (re)connections.
// - SampleGamePort (inside the game's XInputGetState, once per tick and port): samples every source of the port once
//   and merges (MergePad). Physical sources count only while the game window has the focus ([input] background_input
//   = 1: always); after the focus comes back, controls that are still held count only after they were released
//   once. A diverted port gives the merged state to its InputConsumer and a neutral, connected pad to the game; when
//   the port returns to the game the same release-first rule applies (no false press edge, m4_input.md 8.1).
// - Rumble (SetGameRumble): scaled by rumble / rumble_strength, muted without focus and on diverted ports, sent to the
//   most recently used source with motors of the port (0 to its other motor sources); sources without motors get the
//   game's values unscaled. Changes only.
#pragma once

#include "input/input_api.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace inp {

class HostRouter final : public InputRouter {
public:
    struct Options {
        bool rumble = true;
        std::uint32_t rumbleStrength = 100;  // percent
        bool backgroundInput = false;        // physical sources also without focus
    };
    using FocusQuery = bool (*)() noexcept;

    HostRouter() noexcept = default;

    void Configure(const Options& options) noexcept;
    // nullptr = always focused (no window).
    void SetFocusQuery(FocusQuery query) noexcept;
    // Explicit source list (tests); `useRegistry` = true returns to the global registry.
    void UseSources(const std::vector<InputSource*>& sources) noexcept;
    void UseRegistry() noexcept;
    // M4b: called in SampleGamePort with the merged state of `port` before the divert check (freecam hotkeys, held
    // back chord buttons). It may change the state and call Divert. nullptr = none.
    using SampleFilter = void (*)(int port, PadState& merged) noexcept;
    void SetSampleFilter(SampleFilter filter) noexcept;

    // ---- InputRouter -------------------------------------------------------------------------------------
    std::uint32_t ConnectedPorts() const noexcept override;
    bool SampleGamePort(int port, PadState& out) noexcept override;
    void SetGameRumble(int port, const RumbleState& rumble) noexcept override;
    void Divert(int port, InputConsumer* consumer) noexcept override;
    std::uint32_t PortEpoch(int port) const noexcept override;
    void SimulateUnplug(int port, bool unplugged) noexcept override;

    // Re-evaluates focus and rumble now (focus change outside a sample, tests).
    void RefreshFocus() noexcept;
    // Motors of every source off (exit, crash); later game requests are sent again.
    void StopAllMotors() noexcept;

    struct PortStats {
        std::uint64_t samples = 0;         // SampleGamePort calls with the port connected
        std::uint64_t activeSamples = 0;   // ... with a non-neutral state for the game
        std::uint64_t divertedSamples = 0;
        std::uint64_t suppressedSamples = 0;  // samples where a held control was kept back
        std::uint64_t rumbleRequests = 0;  // SetGameRumble calls
        std::uint64_t rumbleNonZero = 0;   // ... with a motor above 0
        RumbleState maxRumble;             // largest request of the game
        std::uint32_t epoch = 0;
        bool connected = false;
    };
    PortStats Stats(int port) const noexcept;
    std::uint32_t FocusChanges() const noexcept;

private:
    struct Suppress {
        std::uint16_t buttons = 0;
        std::uint8_t analog = 0;  // bit per AnalogButton
        std::uint8_t axes = 0;    // bit per StickAxis
        bool pending = false;     // take the held controls of the next sample
        bool Any() const noexcept { return buttons != 0 || analog != 0 || axes != 0; }
    };
    struct SentRumble {
        InputSource* source;
        int port;
        RumbleState value;
    };

    std::vector<InputSource*> SourcesLocked() const;
    bool PortConnectedLocked(int port) const noexcept;
    void UpdateConnectionsLocked() const noexcept;
    bool FocusedLocked() const noexcept;
    void ApplyRumbleLocked(int port) noexcept;
    void SendLocked(InputSource* source, int port, const RumbleState& value) noexcept;
    static bool ApplySuppress(Suppress& s, PadState& state) noexcept;  // true if something was kept back

    mutable std::recursive_mutex mutex_;
    Options options_{};
    FocusQuery focusQuery_ = nullptr;
    SampleFilter sampleFilter_ = nullptr;
    bool explicitSources_ = false;
    std::vector<InputSource*> sources_;

    std::array<InputConsumer*, kPortCount> consumer_{};
    std::array<bool, kPortCount> forcedOff_{};
    std::array<std::uint64_t, kPortCount> tick_{};
    std::array<RumbleState, kPortCount> requested_{};
    std::array<InputSource*, kPortCount> motorTarget_{};
    std::array<Suppress, kPortCount> suppressPhysical_{};
    std::array<Suppress, kPortCount> suppressAll_{};
    std::vector<SentRumble> sent_;
    bool lastFocused_ = true;
    std::uint32_t focusChanges_ = 0;
    bool loggedFirstRumble_ = false;
    mutable std::array<bool, kPortCount> lastConnected_{};
    mutable std::array<std::uint32_t, kPortCount> epoch_{};
    mutable std::array<PortStats, kPortCount> stats_{};
};

}  // namespace inp
