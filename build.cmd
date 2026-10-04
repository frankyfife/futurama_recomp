@echo off
cd /d "%~dp0"
rem Build the loader with MSVC x86 + Ninja (design 5.2).
rem Usage: build.cmd [builddir] [Debug^|RelWithDebInfo] [target]
rem   builddir  relative to this directory or absolute   (default: build\x86-debug)
rem   config    Debug or RelWithDebInfo                  (default: Debug)
rem   target    optional build target, e.g. fut_host, fut_launcher, fut_gen (default: all)
rem Every agent/CI job should use its own build directory. Call this file by its FULL path
rem (cmd AutoRun may change the working directory), e.g. from PowerShell:
rem   cmd /c "D:\FuturamaDecomp\build.cmd" build\core Debug
setlocal EnableExtensions

set "FUT_BUILD_DIR=%~1"
if "%FUT_BUILD_DIR%"=="" set "FUT_BUILD_DIR=build\x86-debug"
set "FUT_CONFIG=%~2"
if "%FUT_CONFIG%"=="" set "FUT_CONFIG=Debug"
if /i "%FUT_CONFIG%"=="debug" set "FUT_CONFIG=Debug"
if /i "%FUT_CONFIG%"=="relwithdebinfo" set "FUT_CONFIG=RelWithDebInfo"
set "FUT_TARGET=%~3"
if not "%FUT_CONFIG%"=="Debug" if not "%FUT_CONFIG%"=="RelWithDebInfo" (
    echo build.cmd: unknown configuration "%FUT_CONFIG%" - use Debug or RelWithDebInfo
    exit /b 2
)

rem x86 toolchain (prints a harmless vswhere message on some machines).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo build.cmd: cl.exe not found - vcvars32.bat failed
    exit /b 1
)

set "FUT_RECONFIGURE=0"
if not exist "%FUT_BUILD_DIR%\build.ninja" set "FUT_RECONFIGURE=1"
if exist "%FUT_BUILD_DIR%\CMakeCache.txt" (
    findstr /b /c:"CMAKE_BUILD_TYPE:STRING=%FUT_CONFIG%" "%FUT_BUILD_DIR%\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 set "FUT_RECONFIGURE=1"
)
if "%FUT_RECONFIGURE%"=="1" (
    cmake -S . -B "%FUT_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%FUT_CONFIG% -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
    if errorlevel 1 (
        echo build.cmd: configure failed
        exit /b 1
    )
)

if "%FUT_TARGET%"=="" (
    cmake --build "%FUT_BUILD_DIR%"
) else (
    cmake --build "%FUT_BUILD_DIR%" --target %FUT_TARGET%
)
if errorlevel 1 (
    echo build.cmd: build failed
    exit /b 1
)
echo build.cmd: OK - %FUT_BUILD_DIR% [%FUT_CONFIG%] %FUT_TARGET%
exit /b 0
