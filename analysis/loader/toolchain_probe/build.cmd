@echo off
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /O2 probe.cpp /link /BASE:0x40000000 /FIXED /LARGEADDRESSAWARE /DYNAMICBASE:NO
