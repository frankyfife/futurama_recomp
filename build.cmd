@echo off
cd /d "%~dp0"
rem Build the loader with MSVC x86 (or clang-cl x86) + Ninja (design 5.2).
rem Usage: build.cmd [builddir] [Debug^|RelWithDebInfo] [target] [msvc^|clang]
rem   builddir   relative to this directory or absolute   (default: build\x86-debug)
rem   config     Debug or RelWithDebInfo                  (default: Debug)
rem   target     optional build target, e.g. fut_host, fut_launcher, fut_gen, all (default: all)
rem   toolchain  msvc (cl.exe, default) or clang (clang-cl 32-bit + MASM ml.exe + MSVC link.exe, second
rem              CI build for warnings, design 5.2). Use a separate build directory per toolchain.
rem              link.exe instead of lld-link: a host DLL linked by lld-link 23 hangs or fails fast
rem              (0xC0000409) inside LoadLibrary (phase 5, not analysed further).
rem Every agent/CI job should use its own build directory. Call this file by its FULL path
rem (cmd AutoRun may change the working directory), e.g. from PowerShell:
rem   cmd /c "D:\FuturamaDecomp\build.cmd" build\core Debug
rem   cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-clang-debug Debug all clang
rem Extra CMake cache options can be passed in the environment variable FUT_CMAKE_ARGS, e.g.
rem   set FUT_CMAKE_ARGS=-DFUT_WERROR=ON -DFUT_TEST_CONFIG=D:/FuturamaDecomp/run/integ/futurama.ini
setlocal EnableExtensions

set "FUT_BUILD_DIR=%~1"
if "%FUT_BUILD_DIR%"=="" set "FUT_BUILD_DIR=build\x86-debug"
set "FUT_CONFIG=%~2"
if "%FUT_CONFIG%"=="" set "FUT_CONFIG=Debug"
if /i "%FUT_CONFIG%"=="debug" set "FUT_CONFIG=Debug"
if /i "%FUT_CONFIG%"=="relwithdebinfo" set "FUT_CONFIG=RelWithDebInfo"
set "FUT_TARGET=%~3"
if /i "%FUT_TARGET%"=="all" set "FUT_TARGET="
set "FUT_TOOLCHAIN=%~4"
if "%FUT_TOOLCHAIN%"=="" set "FUT_TOOLCHAIN=msvc"
if not "%FUT_CONFIG%"=="Debug" if not "%FUT_CONFIG%"=="RelWithDebInfo" (
    echo build.cmd: unknown configuration "%FUT_CONFIG%" - use Debug or RelWithDebInfo
    exit /b 2
)
if /i not "%FUT_TOOLCHAIN%"=="msvc" if /i not "%FUT_TOOLCHAIN%"=="clang" (
    echo build.cmd: unknown toolchain "%FUT_TOOLCHAIN%" - use msvc or clang
    exit /b 2
)

rem x86 toolchain (prints a harmless vswhere message on some machines). clang-cl also needs it for
rem the MSVC headers/libraries, ml.exe and the x86 Windows SDK.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo build.cmd: cl.exe not found - vcvars32.bat failed
    exit /b 1
)

set "FUT_CLANG_CL=C:/Program Files/LLVM/bin/clang-cl.exe"
if /i "%FUT_TOOLCHAIN%"=="clang" (
    if not exist "%FUT_CLANG_CL%" (
        echo build.cmd: %FUT_CLANG_CL% not found - install LLVM or use the msvc toolchain
        exit /b 1
    )
)

set "FUT_RECONFIGURE=0"
if not exist "%FUT_BUILD_DIR%\build.ninja" set "FUT_RECONFIGURE=1"
if exist "%FUT_BUILD_DIR%\CMakeCache.txt" (
    findstr /b /c:"CMAKE_BUILD_TYPE:STRING=%FUT_CONFIG%" "%FUT_BUILD_DIR%\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 set "FUT_RECONFIGURE=1"
    findstr /b /c:"CMAKE_CXX_COMPILER:" "%FUT_BUILD_DIR%\CMakeCache.txt" | findstr /i "clang-cl" >nul 2>&1
    if errorlevel 1 (
        if /i "%FUT_TOOLCHAIN%"=="clang" goto :wrong_toolchain
    ) else (
        if /i "%FUT_TOOLCHAIN%"=="msvc" goto :wrong_toolchain
    )
)
if not "%FUT_CMAKE_ARGS%"=="" set "FUT_RECONFIGURE=1"

if "%FUT_RECONFIGURE%"=="1" (
    if /i "%FUT_TOOLCHAIN%"=="clang" (
        cmake -S . -B "%FUT_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%FUT_CONFIG% "-DCMAKE_C_COMPILER=%FUT_CLANG_CL%" "-DCMAKE_CXX_COMPILER=%FUT_CLANG_CL%" -DCMAKE_C_COMPILER_TARGET=i686-pc-windows-msvc -DCMAKE_CXX_COMPILER_TARGET=i686-pc-windows-msvc -DCMAKE_ASM_MASM_COMPILER=ml.exe -DCMAKE_LINKER=link.exe %FUT_CMAKE_ARGS%
    ) else (
        cmake -S . -B "%FUT_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%FUT_CONFIG% -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl %FUT_CMAKE_ARGS%
    )
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
echo build.cmd: OK - %FUT_BUILD_DIR% [%FUT_CONFIG%, %FUT_TOOLCHAIN%] %FUT_TARGET%
exit /b 0

:wrong_toolchain
echo build.cmd: %FUT_BUILD_DIR% was configured with another toolchain - use a separate build directory for %FUT_TOOLCHAIN%
exit /b 2
