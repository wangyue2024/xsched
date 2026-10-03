@echo off
REM =====================================================================
REM  Windows helper for inject/Makefile:
REM    sets up the MSVC host-compiler environment (cl.exe) required by
REM    nvcc on Windows, then forwards all arguments to mingw32-make.
REM
REM  Usage (can be invoked from anywhere):
REM    make_msvc.bat ARCH=120 bin dump cc
REM    make_msvc.bat bin dump cc          (ARCH auto-detected)
REM
REM  Notes:
REM    - nvcc on Windows requires a host compiler; the latest VS Build
REM      Tools installation is located via vswhere and activated through
REM      vcvars64.bat.
REM    - Newer MSVC toolsets may not be recognized by the installed CUDA
REM      version; NVCC_PREPEND_FLAGS=-allow-unsupported-compiler bypasses
REM      only the version gate. For -cubin (device-only) output the host
REM      compiler is used for preprocessing only; device code is produced
REM      by cudafe++/cicc/ptxas and is not affected by the host toolset.
REM =====================================================================
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [make_msvc] vswhere.exe not found - is Visual Studio Build Tools installed?
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [make_msvc] Visual Studio with C++ tools [VC.Tools.x86.x64] not found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [make_msvc] vcvars64.bat failed.
    exit /b 1
)

set NVCC_PREPEND_FLAGS=-allow-unsupported-compiler

mingw32-make %*
set MAKE_EXIT=%ERRORLEVEL%
endlocal & exit /b %MAKE_EXIT%
