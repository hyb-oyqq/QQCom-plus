@echo off
REM Set up MSVC environment
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

REM Add Qt to PATH
set PATH=C:\MyProgram\Qt\6.10.1\msvc2022_64\bin;%PATH%

REM Clean previous build
if exist Makefile del Makefile
if exist Makefile.Debug del Makefile.Debug
if exist Makefile.Release del Makefile.Release

REM Run qmake
qmake easyDebug.pro

REM Build
nmake

echo.
echo Build finished!
