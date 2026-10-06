@echo off
cd /d "%~dp0"
rem Build FuturamaLauncher.exe (Qt 6 Widgets, MSVC x64, Ninja) and deploy the Qt runtime next to it.
rem Usage: build_launcher.cmd [builddir] [Debug^|Release] [test]
rem   builddir  relative to this directory or absolute       (default: build\launcher)
rem   config    Debug or Release                            (default: Debug)
rem   test      optional: run ctest afterwards (unit tests + offscreen screenshot smoke test)
rem Call it by its FULL path (cmd AutoRun may change the working directory), e.g. from PowerShell:
rem   cmd /c "D:\FuturamaDecomp\build_launcher.cmd" build\launcher-rel Release test
rem Environment: FUT_QT_DIR (default D:\Qt\6.8.3\msvc2022_64), LAUNCHER_CMAKE_ARGS (extra cache options,
rem e.g. -DLAUNCHER_WERROR=ON). Output: <builddir>\bin\FuturamaLauncher.exe + windeployqt files; Release
rem builds also get the app-local MSVC runtime DLLs (players need no VC++ redistributable).
rem The game itself (x86) is built separately with build.cmd.
setlocal EnableExtensions

set "LB_DIR=%~1"
if "%LB_DIR%"=="" set "LB_DIR=build\launcher"
set "LB_CONFIG=%~2"
if "%LB_CONFIG%"=="" set "LB_CONFIG=Debug"
if /i "%LB_CONFIG%"=="debug" set "LB_CONFIG=Debug"
if /i "%LB_CONFIG%"=="release" set "LB_CONFIG=Release"
if not "%LB_CONFIG%"=="Debug" if not "%LB_CONFIG%"=="Release" (
    echo build_launcher.cmd: unknown configuration "%LB_CONFIG%" - use Debug or Release
    exit /b 2
)
set "LB_TEST=%~3"
if not "%LB_TEST%"=="" if /i not "%LB_TEST%"=="test" (
    echo build_launcher.cmd: unknown third argument "%LB_TEST%" - use test or nothing
    exit /b 2
)
if "%FUT_QT_DIR%"=="" set "FUT_QT_DIR=D:\Qt\6.8.3\msvc2022_64"
if not exist "%FUT_QT_DIR%\bin\windeployqt.exe" (
    echo build_launcher.cmd: Qt not found at %FUT_QT_DIR% - set FUT_QT_DIR
    exit /b 1
)

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo build_launcher.cmd: cl.exe not found - vcvars64.bat failed
    exit /b 1
)
set "CMAKE_PREFIX_PATH=%FUT_QT_DIR%"

set "LB_RECONFIGURE=0"
if not exist "%LB_DIR%\build.ninja" set "LB_RECONFIGURE=1"
if exist "%LB_DIR%\CMakeCache.txt" (
    findstr /b /c:"CMAKE_BUILD_TYPE:STRING=%LB_CONFIG%" "%LB_DIR%\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 set "LB_RECONFIGURE=1"
)
if not "%LAUNCHER_CMAKE_ARGS%"=="" set "LB_RECONFIGURE=1"
if "%LB_RECONFIGURE%"=="1" (
    cmake -S launcher -B "%LB_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%LB_CONFIG% -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_PREFIX_PATH=%FUT_QT_DIR%" %LAUNCHER_CMAKE_ARGS%
    if errorlevel 1 (
        echo build_launcher.cmd: configure failed
        exit /b 1
    )
)

cmake --build "%LB_DIR%"
if errorlevel 1 (
    echo build_launcher.cmd: build failed
    exit /b 1
)

rem Qt runtime next to the exe. No translations (the UI has its own de/en tables), no software OpenGL,
rem no D3D/DXC shader compilers (Widgets only). windeployqt 6.8.3 still copies the generic/imageformats/
rem tls/networkinformation plugins and Qt6Network (--skip-plugin-types works only with --dry-run); the
rem launcher does not load them and tools\package.py takes an allow-list. The offscreen platform plugin is added for the headless
rem --screenshots mode (without it "-platform offscreen" ends in Qt's blocking "no platform plugin" box).
set "LB_DEPLOY_MODE=--debug"
if "%LB_CONFIG%"=="Release" set "LB_DEPLOY_MODE=--release"
rem Review M3c finding 22: windeployqt 6.8.3 prints "Warning: Translations will not be available ... Cannot open
rem .../translations/catalogs.json" even with --no-translations (this Qt installation has no translations, and the
rem launcher brings its own de/en tables). That expected message is filtered; everything else is shown, and the
rem exit code of windeployqt decides.
set "LB_DEPLOY_LOG=%LB_DIR%\windeployqt.log"
"%FUT_QT_DIR%\bin\windeployqt.exe" %LB_DEPLOY_MODE% --no-translations --no-system-d3d-compiler --no-opengl-sw --no-system-dxc-compiler --no-compiler-runtime --no-quick-import --include-plugins qoffscreen --verbose 0 "%LB_DIR%\bin\FuturamaLauncher.exe" > "%LB_DEPLOY_LOG%" 2>&1
set "LB_DEPLOY_RC=%errorlevel%"
findstr /v /c:"Translations will not be available due to the following error." /c:"translations/catalogs.json" "%LB_DEPLOY_LOG%"
if not "%LB_DEPLOY_RC%"=="0" (
    echo build_launcher.cmd: windeployqt failed ^(exit code %LB_DEPLOY_RC%, see %LB_DEPLOY_LOG%^)
    exit /b 1
)

rem App-local MSVC runtime (Release only; the debug runtime is not redistributable).
if "%LB_CONFIG%"=="Release" (
    if not exist "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\vcruntime140.dll" (
        echo build_launcher.cmd: MSVC runtime not found under "%VCToolsRedistDir%x64\Microsoft.VC143.CRT"
        exit /b 1
    )
    copy /y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\*.dll" "%LB_DIR%\bin\" >nul
    if errorlevel 1 (
        echo build_launcher.cmd: copying the MSVC runtime failed
        exit /b 1
    )
)

if /i "%LB_TEST%"=="test" (
    ctest --test-dir "%LB_DIR%" --output-on-failure
    if errorlevel 1 (
        echo build_launcher.cmd: tests failed
        exit /b 1
    )
)
echo build_launcher.cmd: OK - %LB_DIR% [%LB_CONFIG%]
exit /b 0
