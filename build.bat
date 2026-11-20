@echo off
setlocal
if not defined QT_BIN set "QT_BIN=C:\MyProgram\Qt\6.10.1\msvc2022_64\bin"
if not defined VCVARS64 set "VCVARS64=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%QT_BIN%\qmake.exe" (
    echo Qt not found. Set QT_BIN to the matching MSVC kit's bin directory.
    exit /b 1
)
if not exist "%VCVARS64%" (
    echo MSVC not found. Set VCVARS64 to vcvars64.bat.
    exit /b 1
)
call "%VCVARS64%"
if errorlevel 1 exit /b 1
set "PATH=%QT_BIN%;%PATH%"
if not exist "%~dp0build\release" mkdir "%~dp0build\release"
pushd "%~dp0build\release"
if errorlevel 1 exit /b 1
qmake "%~dp0QQCom.pro" -spec win32-msvc "CONFIG+=release" "CONFIG-=debug"
if errorlevel 1 goto failed
nmake /nologo release
if errorlevel 1 goto failed
echo Built: %~dp0build\release\release\QQCom-plus.exe
popd
exit /b 0
:failed
popd
exit /b 1
