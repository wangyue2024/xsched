# =====================================================================
#  build.ps1 - T6 MVE build script (sm120 / Windows)
#
#  Stages:
#    gen    : python gen_arrays.py            -> mve_arrays.h
#             (arrays sliced out of the reviewed sm120.cpp -- no drift)
#    device : nvcc -cubin dummy_kernel.cu     -> mve_kernel.cubin
#             (plain compile: launchable entry function; the guardian
#              splice consumes exactly this form)
#    host   : g++ mve_main.cpp + libcuxtra    -> mve_main.exe
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File build.ps1            # all
#    powershell -ExecutionPolicy Bypass -File build.ps1 -Stage host
# =====================================================================
param(
    [ValidateSet('gen', 'device', 'host', 'all')]
    [string]$Stage = 'all'
)

$ErrorActionPreference = 'Stop'
$Dir = $PSScriptRoot
Set-Location $Dir
$RepoRoot = (Resolve-Path (Join-Path $Dir '..\..')).Path

function Enter-MsvcEnvironment {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found' }
    $vs = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
    if (-not $vs) { throw 'Visual Studio with VC.Tools.x86.x64 not found' }
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation `
        -DevCmdArguments '-arch=x64 -no_logo' | Out-Null
    $env:NVCC_PREPEND_FLAGS = '-allow-unsupported-compiler'
}

if ($Stage -in @('gen', 'all')) {
    Write-Host '[gen] gen_arrays.py -> mve_arrays.h (from sm120.cpp)'
    python (Join-Path $Dir 'gen_arrays.py')
    if ($LASTEXITCODE -ne 0) { throw 'gen_arrays.py failed' }
}

if ($Stage -in @('device', 'all')) {
    Write-Host '[device] activating MSVC host environment'
    Enter-MsvcEnvironment

    Write-Host '[device] nvcc -cubin dummy_kernel.cu (sm_120, plain compile)'
    nvcc -cubin dummy_kernel.cu -o mve_kernel.cubin -arch=sm_120
    if ($LASTEXITCODE -ne 0) { throw 'nvcc failed' }

    Write-Host '[device] cuobjdump -sass -> mve_kernel.sass.txt'
    $sass = cuobjdump -sass mve_kernel.cubin | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'cuobjdump failed' }
    [IO.File]::WriteAllText("$Dir\mve_kernel.sass.txt", $sass, [Text.Encoding]::ASCII)
}

if ($Stage -in @('host', 'all')) {
    if (-not (Test-Path (Join-Path $Dir 'mve_arrays.h'))) { throw 'mve_arrays.h missing (run -Stage gen)' }
    Write-Host '[host] g++ mve_main.cpp (link libcuxtra + shlwapi)'
    g++ -O1 -std=c++17 -I "$RepoRoot\3rdparty\cuxtra\include" `
        -I $Dir mve_main.cpp `
        "$RepoRoot\3rdparty\cuxtra\lib\libcuxtra_windows_amd64.a" `
        -lshlwapi -lpthread -static-libgcc -static-libstdc++ -o mve_main.exe
    if ($LASTEXITCODE -ne 0) { throw 'g++ host build failed' }
}

Write-Host 'build.ps1: done'
