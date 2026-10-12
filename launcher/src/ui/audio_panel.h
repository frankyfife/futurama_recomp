// "Audio" tab extras (M5): the output device the game will use (from [audio] device and the Windows device list),
// a warning when no device is active or the configured one is not connected, the test tone button (a generated
// chime per speaker through XAudio2 on that device, with the tab's speaker layout and master volume; no game data)
// and "Geräte neu suchen". The keys of the tab come from the schema (SettingsTab); this panel only reads them.
#pragma once

#include "core/audio_output.h"
#include "ui/app_context.h"

#include <QWidget>

class QLabel;
class QPushButton;

namespace fl::ui {

class AudioPanel : public QWidget {
    Q_OBJECT
public:
    AudioPanel(AppContext& ctx, QWidget* parent = nullptr);

    // Re-reads the device key and updates the device line (after an edit).
    void refresh();
    // The request the test button would send now (tests).
    ToneRequest currentRequest() const;
    TonePlayer* player() const { return player_; }
    QPushButton* testButton() const { return testButton_; }

signals:
    void refreshDevicesRequested();

private:
    QString keyValue(const char* name) const;
    void playTone();
    void onToneFinished(const ToneResult& result);

    AppContext& ctx_;
    TonePlayer* player_ = nullptr;
    QLabel* deviceInfo_ = nullptr;
    QLabel* deviceWarning_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* testButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
};

}  // namespace fl::ui
