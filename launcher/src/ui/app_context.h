// State shared by the launcher's windows and automation modes, and the start-up steps that fill it:
// locate futurama.exe, resolve the INI location (shared contract item 4), load the schema and the GPU
// capabilities (futurama.exe CLI, cached per build) and the player INI.
#pragma once

#include "core/game_paths.h"
#include "core/gpu_info.h"
#include "core/settings_model.h"

#include <QSettings>
#include <QString>
#include <QStringList>

#include <memory>

namespace fl::ui {

struct StartupOptions {
    QString gameDir;       // --game-dir
    QString config;        // --config
    QString schemaFile;    // --schema-file (development: a hand-written schema instead of the exe)
    QString gpuFile;       // --gpu-file
    bool refresh = false;  // --refresh: ignore the CLI cache
};

struct AppContext {
    QString launcherDir;
    GamePaths paths;
    SettingsModel model;
    GpuInfo gpu;
    bool gpuFromScreens = false;  // query-gpu unavailable, outputs from QScreen
    QString gpuNote;              // why (localized), empty if query-gpu worked
    QString schemaSource;         // "exe", "cache" or "file"
    QStringList schemaWarnings;
    std::unique_ptr<QSettings> settings;  // launcher.ini (UI language, game directory)

    QString userdata() const;
    QString logFile() const;
    QString screenshotDir() const;
    QStringList newIniHeader() const;  // comment lines of a newly created futurama.ini
    QString setting(const QString& key, const QString& fallback = QString()) const;
    void setSetting(const QString& key, const QString& value);
};

// Finds the game (explicit dir, launcher dir, launcher.ini game_dir) and resolves the INI location;
// opens launcher.ini next to the INI.
void InitPaths(AppContext& ctx, const StartupOptions& options);
// Uses `gameDir` from now on (the user picked it) and remembers it in launcher.ini.
void SetGameDir(AppContext& ctx, const QString& gameDir, const StartupOptions& options);
// Schema from --schema-file, the cache or `futurama.exe --print-config-schema`. On failure `error`
// (localized) explains why.
bool LoadSchema(AppContext& ctx, const StartupOptions& options, QString* error);
// GPU info from --gpu-file, the cache or `futurama.exe --query-gpu`; falls back to QScreen.
void LoadGpu(AppContext& ctx, const StartupOptions& options);
bool LoadIni(AppContext& ctx, QString* error);

}  // namespace fl::ui
