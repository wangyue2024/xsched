# =====================================================================
#  build.ps1 - L2 check demo build (sm120 / Windows)
#
#  Produces work/l2_check.exe linked against:
#    - output/lib/nvcuda.lib   (the shim DLL-proxy import library)
#    - cudart (CUDA runtime)
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File build.ps1
# =====================================================================
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

if (-not (Test-Path "$RepoRoot\output\lib\nvcuda.lib")) {
    throw 'output/lib/nvcuda.lib missing - build XSched first (mingw32-make cuda)'
}

New-Item -ItemType Directory -Force -Path "$Dir\work" | Out-Null

Enter-MsvcEnvironment

Write-Host '[build] nvcc l2_check.cu -> work/l2_check.exe'
nvcc -O2 -std=c++17 -Xcompiler "/utf-8" -o "$Dir\work\l2_check.exe" l2_check.cu `
    -I "$RepoRoot\output\include" `
    -I "$RepoRoot\platforms\cuda\hal\include" `
    -L "$RepoRoot\output\lib" -lnvcuda `
    -L "$env:CUDA_PATH\lib\x64" -lcudart
if ($LASTEXITCODE -ne 0) { throw 'nvcc failed' }

Write-Host 'build.ps1: done'
