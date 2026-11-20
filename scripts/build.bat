@echo off
rem Builds build\WindowsSystemStatusMonitor.exe  ("build.bat test" builds build\sample_test.exe instead).
rem Needs the MSVC C++ x64 toolchain. Run it from an "x64 Native Tools Command Prompt", or let this script
rem locate Visual Studio / Build Tools through vswhere. Output goes to build\ (git-ignored).
setlocal
cd /d "%~dp0.."

rem Already inside a Developer Command Prompt (INCLUDE/LIB set up)? Then just build.
if defined VSCMD_VER goto build
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" (
  echo vswhere.exe not found. Install Visual Studio or "Build Tools for Visual Studio" with the "Desktop development with C++" workload.
  exit /b 1
)
set "VCVARS="
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
if not defined VCVARS (
  echo MSVC C++ x64 build tools not found. Install the "Desktop development with C++" workload.
  exit /b 1
)
call "%VCVARS%" >nul 2>&1 || exit /b 1

:build
if not exist build mkdir build
set "CF=/nologo /O1 /MT /EHsc /utf-8 /W3 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /I src /Fo:build\"
set "LIBS=pdh.lib dxgi.lib iphlpapi.lib user32.lib gdi32.lib shell32.lib advapi32.lib comctl32.lib ole32.lib oleaut32.lib uuid.lib"
if "%1"=="test" (
  cl %CF% tests\sample_test.cpp src\sample.cpp /Fe:build\sample_test.exe /link /SUBSYSTEM:CONSOLE %LIBS% || exit /b 1
) else (
  cl %CF% src\main.cpp src\sample.cpp /Fe:build\WindowsSystemStatusMonitor.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:resources\app.manifest %LIBS% || exit /b 1
)
echo Build OK.
