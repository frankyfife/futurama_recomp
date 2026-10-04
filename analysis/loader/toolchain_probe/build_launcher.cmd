@echo off
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
ml /nologo /c /coff placeholder.asm || exit /b 1
cl /nologo /O1 /GS- launcher.c placeholder.obj /link /NODEFAULTLIB /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE kernel32.lib user32.lib
dumpbin /nologo /headers launcher.exe | findstr /R /C:"SECTION HEADER" /C:"virtual address" /C:"virtual size" /C:"image base" /C:"^ *[0-9A-F]* name"
