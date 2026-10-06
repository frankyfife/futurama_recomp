// Where the game, the player configuration and the user data live (shared contract item 4):
//   - default:  config %LOCALAPPDATA%\FuturamaPC\futurama.ini, user data %LOCALAPPDATA%\FuturamaPC\
//   - portable: a file "portable.txt" next to futurama.exe -> config and user data next to the exe
//   - --config=<file> always overrides the INI location (the launcher always passes it to the game)
// The launcher keeps its own small state (UI language, game directory, CLI cache) in the directory of
// the INI it edits: launcher.ini and launcher_cache\ - so a test run with --config=<run dir>\x.ini
// never touches %LOCALAPPDATA%.
#pragma once

#include <QString>
#include <QStringList>

namespace fl {

inline constexpr char kGameExeName[] = "futurama.exe";
inline constexpr char kHostDllName[] = "futurama_host.dll";
inline constexpr char kPortableMarker[] = "portable.txt";
inline constexpr char kIniName[] = "futurama.ini";
inline constexpr char kLauncherIniName[] = "launcher.ini";
inline constexpr char kCacheDirName[] = "launcher_cache";

struct GamePaths {
    QString gameDir;  // directory of futurama.exe (native separators), empty if not found
    QString gameExe;
    QString hostDll;
    bool portable = false;
    QString configDir;        // directory of the INI
    QString iniPath;          // the player INI the launcher edits and passes with --config
    QString defaultUserdata;  // user data when [paths] userdata is not set

    bool gameFound() const { return !gameExe.isEmpty(); }
    QString launcherIni() const;  // <configDir>\launcher.ini
    QString cacheDir() const;     // <configDir>\launcher_cache
};

// %LOCALAPPDATA%\FuturamaPC (native separators); empty if LOCALAPPDATA is not set.
QString LocalAppDataBase();

// True if `dir` holds futurama.exe.
bool HasGameExe(const QString& dir);
bool IsPortableGameDir(const QString& gameDir);

// First candidate directory that holds futurama.exe, in this order: `explicitDir` (--game-dir),
// `launcherDir` (the launcher's own directory), `configuredDir` (launcher.ini game_dir). Empty if none.
QString FindGameDir(const QString& explicitDir, const QString& launcherDir, const QString& configuredDir);

// Resolves all paths for the game in `gameDir` (may be empty = not found) and an optional explicit INI.
GamePaths ResolveGamePaths(const QString& gameDir, const QString& explicitIni);

// User data directory the game will use: the INI value of [paths] userdata (relative to the INI
// directory) or `paths.defaultUserdata`.
QString UserdataDir(const QString& iniUserdataValue, const GamePaths& paths);
QString LogDir(const QString& userdata);   // <userdata>\logs
QString LogFile(const QString& userdata);  // <userdata>\logs\futurama.log
QString ScreenshotDir(const QString& iniValue, const QString& userdata, const QString& iniDir);

// Native, cleaned absolute path.
QString NativeAbsolute(const QString& path);

}  // namespace fl
