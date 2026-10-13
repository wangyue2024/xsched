# =====================================================================
#  build.ps1 - coop-hazard demo build script (sm120 / Windows)
#
#  Stages:
#    device : nvcc -cubin coop_kernel.cu -> coop_kernel.cubin (+ SASS dump)
#    host   : g++ coop_main.cpp + libcuxtra -> coop_main.exe
#             (reuses ../sm120_mve/mve_arrays.h for the guardian arrays)
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File build.ps1
#    powershell -ExecutionPolicy Bypass -File build.ps1 -Stage host
# =====================================================================
param(
    [ValidateSet('device', 'host', 'all')]
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

if ($Stage -in @('device', 'all')) {
    Write-Host '[device] activating MSVC host environment'
    Enter-MsvcEnvironment

    Write-Host '[device] nvcc -cubin coop_kernel.cu (sm_120)'
    nvcc -cubin coop_kernel.cu -o coop_kernel.cubin -arch=sm_120
    if ($LASTEXITCODE -ne 0) { throw 'nvcc failed' }

    Write-Host '[device] cuobjdump -sass -> coop_kernel.sass.txt'
    $sass = cuobjdump -sass coop_kernel.cubin | Out-String
    [IO.File]::WriteAllText("$Dir\coop_kernel.sass.txt", $sass, [Text.Encoding]::ASCII)
}

if ($Stage -in @('host', 'all')) {
    Write-Host '[host] g++ coop_main.cpp (link libcuxtra + shlwapi)'
    g++ -O1 -std=c++17 -I "$RepoRoot\3rdparty\cuxtra\include" `
        -I "$Dir\..\sm120_mve" -I $Dir coop_main.cpp `
        "$RepoRoot\3rdparty\cuxtra\lib\libcuxtra_windows_amd64.a" `
        -lshlwapi -lpthread -static-libgcc -static-libstdc++ -o coop_main.exe
    if ($LASTEXITCODE -ne 0) { throw 'g++ host build failed' }
}

Write-Host 'build.ps1: done'
