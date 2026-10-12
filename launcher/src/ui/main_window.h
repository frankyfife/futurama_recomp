// Main window: neutral typographic header with the original icon, the result panel of the last game
// run, the tabs Spiel / Grafik / Audio / Steuerung / Pfade / Erweitert built from the schema, and the footer with the
// unsaved/invalid state, "Speichern" and the big "Spielen" button. "Spielen" saves, starts
// futurama.exe --config=<ini>, hides the launcher and shows it again with the result when the game exits.
#pragma once

#include "core/game_session.h"
#include "ui/app_context.h"
#include "ui/key_editors.h"

#include <QList>
#include <QMainWindow>
#include <QPointer>

class QComboBox;
class QLabel;
class QPushButton;
class QTabWidget;

namespace fl::ui {

class AudioPanel;
class BindingsPanel;
class IsoStatusBox;
class PadPoller;
class PadTestPanel;
class SettingsTab;
class StatusPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(AppContext& ctx, StartupOptions options, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Screenshots / automation.
    QTabWidget* tabWidget() const { return tabs_; }
    void selectTab(UiTab tab);
    void showRunResult(const GameRunResult& result);
    void hideRunResult();
    bool saveSettings(bool interactive);
    // Opens the first-run wizard (modal); true if the user finished it (settings saved).
    bool runWizard();
    void startGame();
    // Screenshots: finishes background work that changes the picture (the ISO check of the game tab).
    void prepareScreenshot();
    // M4: the controls tab's binding tables (nullptr if the schema has none) and controller test.
    BindingsPanel* bindingsPanel() const { return bindingsPanel_; }
    PadTestPanel* padTestPanel() const { return padTest_; }
    PadPoller* padPoller() const { return padPoller_; }
    // M5: the audio tab's device line and test tone (nullptr before buildUi).
    AudioPanel* audioPanel() const { return audioPanel_; }

signals:
    void gameFinished(const fl::GameRunResult& result);

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void rebuildUi();
    QWidget* buildHeader();
    QWidget* buildGameExtras();
    QWidget* buildGraphicsExtras();
    QWidget* buildPathsExtras();
    QWidget* buildAdvancedExtras();
    void buildControlsExtras(SettingsTab* page);
    void onValueEdited();
    void updateFooter();
    void updatePresetCombo();
    void applyPreset(int index);
    bool checkBeforePlay();
    void fitToScreen(bool center);  // review M3c finding 12
    void onGameFinished(const GameRunResult& result);
    void refreshCapabilities();
    void setUiLanguage(Language language);
    bool keyVisible(const KeyDef& key) const;
    QWidget* buildDeveloperToggle();

    AppContext& ctx_;
    StartupOptions options_;
    EditorContext editorContext_;
    GameSession session_;
    bool hasResult_ = false;
    GameRunResult lastResult_;
    bool rebuilding_ = false;

    StatusPanel* statusPanel_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QList<SettingsTab*> settingsTabs_;
    QComboBox* languageCombo_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QComboBox* frameRateCombo_ = nullptr;  // display only until M3d
    IsoStatusBox* isoBox_ = nullptr;
    PadPoller* padPoller_ = nullptr;  // lives as long as the window (survives rebuildUi)
    BindingsPanel* bindingsPanel_ = nullptr;
    PadTestPanel* padTest_ = nullptr;
    AudioPanel* audioPanel_ = nullptr;
    QLabel* footerStatus_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* playButton_ = nullptr;
};

}  // namespace fl::ui
