// Controller access of the launcher's "Steuerung" tab (binding capture and the live controller test): XInput
// loaded at run time (xinput1_4.dll, Windows 8 and later; xinput9_1_0.dll as fallback) so the launcher never
// fails to start without it, plus a simulated reader for tests and screenshots.
//
// The launcher only reads states and sends a short rumble for the test button; the game has its own input layer.
#pragma once

#include "core/bindings.h"

#include <QString>

#include <array>
#include <functional>
#include <memory>

namespace fl {

class PadReader {
public:
    virtual ~PadReader() = default;
    // False if no XInput library could be loaded (then every slot reads as not connected).
    virtual bool available() const = 0;
    // "xinput1_4.dll", "simulated", ...
    virtual QString backendName() const = 0;
    virtual PadState read(int slot) = 0;
    // Motor speeds 0..1 (left = low-frequency motor). False if the slot is not connected.
    virtual bool setRumble(int slot, double left, double right) = 0;
    virtual bool simulated() const { return false; }
};

std::unique_ptr<PadReader> CreateXInputReader();

// Fixed states set by the caller; records the last rumble per slot.
class SimulatedPadReader final : public PadReader {
public:
    bool available() const override { return true; }
    QString backendName() const override { return QStringLiteral("simulated"); }
    PadState read(int slot) override;
    bool setRumble(int slot, double left, double right) override;
    bool simulated() const override { return true; }

    void setState(int slot, const PadState& state);
    struct Rumble {
        double left = 0.0;
        double right = 0.0;
        int calls = 0;
    };
    Rumble rumble(int slot) const;
    int readCount(int slot) const;

private:
    std::array<PadState, kPadSlots> states_{};
    std::array<Rumble, kPadSlots> rumble_{};
    std::array<int, kPadSlots> reads_{};
};

// Factory used by the controls tab (tests and the screenshot mode install a simulated reader). An empty function
// restores the XInput default.
using PadReaderFactory = std::function<std::unique_ptr<PadReader>()>;
void SetPadReaderFactory(PadReaderFactory factory);
std::unique_ptr<PadReader> CreatePadReader();

}  // namespace fl
