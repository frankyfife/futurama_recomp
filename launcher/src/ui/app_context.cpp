#include "ui/app_context.h"

#include "core/cli_cache.h"
#include "core/game_cli.h"
#include "core/l10n.h"
#include "ui/screen_info.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace fl::ui {
namespace {

QByteArray ReadFile(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = f.errorString();
        }
        return QByteArray();
    }
    return f.readAll();
}

std::unique_ptr<QSettings> OpenLauncherSettings(const QString& path) {
    if (path.isEmpty()) {
        return nullptr;
    }
    return std::make_unique<QSettings>(path, QSettings::IniFormat);
}

}  // namespace

QString AppContext::userdata() const {
    const KeyDef* key = model.schema().find(QStringLiteral("paths"), QStringLiteral("userdata"));
    const QString value = key != nullptr
                              ? model.value(*key)
                              : model.ini()
                                    .effectiveValue(QStringLiteral("paths"), QStringLiteral("userdata"))
                                    .value_or(QString());
    return UserdataDir(value, paths);
}

QString AppContext::logFile() const {
    return LogFile(userdata());
}

QString AppContext::screenshotDir() const {
    const KeyDef* key = model.schema().find(QStringLiteral("render"), QStringLiteral("screenshot_dir"));
    return ScreenshotDir(key != nullptr ? model.value(*key) : QString(), userdata(), paths.configDir);
}

QStringList AppContext::newIniHeader() const {
    return {T("ini.header1"), T("ini.header2")};
}

QString AppContext::setting(const QString& key, const QString& fallback) const {
    return settings ? settings->value(key, fallback).toString() : fallback;
}

void AppContext::setSetting(const QString& key, const QString& value) {
    if (settings) {
        settings->setValue(key, value);
        settings->sync();
    }
}

void InitPaths(AppContext& ctx, const StartupOptions& options) {
    // launcher.ini that may name the game directory: next to an explicit INI, else the default location.
    QString lookup;
    if (!options.config.isEmpty()) {
        lookup = QDir(QFileInfo(NativeAbsolute(options.config)).absolutePath())
                     .filePath(QLatin1String(kLauncherIniName));
    } else if (!LocalAppDataBase().isEmpty()) {
        lookup =
            QDir(QDir::fromNativeSeparators(LocalAppDataBase())).filePath(QLatin1String(kLauncherIniName));
    }
    QString configured;
    if (!lookup.isEmpty() && QFileInfo::exists(lookup)) {
        configured = QSettings(lookup, QSettings::IniFormat).value(QStringLiteral("game/dir")).toString();
    }
    const QString gameDir = FindGameDir(options.gameDir, ctx.launcherDir, configured);
    ctx.paths = ResolveGamePaths(gameDir, options.config);
    ctx.settings = OpenLauncherSettings(ctx.paths.launcherIni());
}

void SetGameDir(AppContext& ctx, const QString& gameDir, const StartupOptions& options) {
    ctx.paths = ResolveGamePaths(gameDir, options.config);
    ctx.settings = OpenLauncherSettings(ctx.paths.launcherIni());
    if (ctx.paths.gameFound()) {
        ctx.setSetting(QStringLiteral("game/dir"), ctx.paths.gameDir);
    }
}

bool LoadSchema(AppContext& ctx, const StartupOptions& options, QString* error) {
    ConfigSchema schema;
    QStringList warnings;
    QString parseError;
    if (!options.schemaFile.isEmpty()) {
        QString readError;
        const QByteArray json = ReadFile(options.schemaFile, &readError);
        if (json.isEmpty()) {
            *error = T("startup.schema_file_unreadable")
                         .arg(QDir::toNativeSeparators(options.schemaFile), readError);
            return false;
        }
        if (!ParseSchema(json, &schema, &parseError, &warnings)) {
            *error = T("startup.schema_invalid").arg(parseError);
            return false;
        }
        ctx.schemaSource = QStringLiteral("file");
    } else {
        if (!ctx.paths.gameFound()) {
            *error = T("startup.game_missing");
            return false;
        }
        const CliCache cache(ctx.paths.cacheDir());
        const QString fingerprint = CliCache::Fingerprint(ctx.paths.gameExe, ctx.paths.hostDll);
        bool loaded = false;
        if (!options.refresh) {
            if (const auto cached = cache.loadSchema(fingerprint)) {
                QStringList cachedWarnings;
                if (ParseSchema(*cached, &schema, &parseError, &cachedWarnings)) {
                    warnings = cachedWarnings;
                    ctx.schemaSource = QStringLiteral("cache");
                    loaded = true;
                }
            }
        }
        if (!loaded) {
            const GameCli cli(ctx.paths.gameExe, ctx.paths.cacheDir());
            const CliResult r = cli.printConfigSchema();
            if (!r.hasJson() || r.exitCode != 0) {
                QString detail = r.error;
                if (detail.isEmpty()) {
                    detail = T("cli.exit_code").arg(r.exitCode);
                }
                *error = T("startup.schema_failed").arg(QDir::toNativeSeparators(ctx.paths.gameExe), detail);
                return false;
            }
            if (!ParseSchema(r.json, &schema, &parseError, &warnings)) {
                *error = T("startup.schema_invalid").arg(parseError);
                return false;
            }
            cache.storeSchema(fingerprint, schema.build, r.json);
            ctx.schemaSource = QStringLiteral("exe");
        }
    }
    ctx.schemaWarnings = warnings;
    ctx.model.setSchema(std::move(schema));
    return true;
}

void LoadGpu(AppContext& ctx, const StartupOptions& options) {
    ctx.gpu = GpuInfo{};
    ctx.gpuFromScreens = false;
    ctx.gpuNote.clear();
    QString parseError;
    if (!options.gpuFile.isEmpty()) {
        QString readError;
        const QByteArray json = ReadFile(options.gpuFile, &readError);
        if (!json.isEmpty() && ParseGpuInfo(json, &ctx.gpu, &parseError)) {
            return;
        }
        ctx.gpuNote = T("startup.gpu_file_unreadable").arg(QDir::toNativeSeparators(options.gpuFile));
    } else if (ctx.paths.gameFound()) {
        const CliCache cache(ctx.paths.cacheDir());
        const QString screens = ScreenSignature();
        const QString build = ctx.model.schema().build;
        if (!options.refresh) {
            if (const auto cached = cache.loadGpu(build, screens)) {
                if (ParseGpuInfo(*cached, &ctx.gpu, &parseError)) {
                    return;
                }
            }
        }
        const GameCli cli(ctx.paths.gameExe, ctx.paths.cacheDir());
        const CliResult r = cli.queryGpu();
        if (r.hasJson() && r.exitCode == 0 && ParseGpuInfo(r.json, &ctx.gpu, &parseError)) {
            cache.storeGpu(build, screens, r.json);
            return;
        }
        ctx.gpuNote = T("startup.gpu_failed").arg(r.error.isEmpty() ? parseError : r.error);
    } else {
        ctx.gpuNote = T("startup.game_missing");
    }
    ctx.gpu = GpuInfoFromScreens();
    ctx.gpuFromScreens = true;
}

bool LoadIni(AppContext& ctx, QString* error) {
    if (ctx.paths.iniPath.isEmpty()) {
        *error = T("startup.no_ini_location");
        return false;
    }
    QString loadError;
    if (!ctx.model.load(ctx.paths.iniPath, &loadError)) {
        *error = T("startup.ini_unreadable").arg(ctx.paths.iniPath, loadError);
        return false;
    }
    return true;
}

}  // namespace fl::ui
