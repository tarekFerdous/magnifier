@echo off
rem Configures, builds and tests from any shell (no Developer Prompt needed).
rem Usage: build.bat [x64-release|x64-debug]
setlocal
set PRESET=%~1
if "%PRESET%"=="" set PRESET=x64-release

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found - install Visual Studio 2019+ with C++ tools & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with the C++ x64 toolset not found & exit /b 1)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
ctest --preset %PRESET% || exit /b 1
echo.
echo Built: build\%PRESET%\McScope.exe
