# =====================================================================
#  build.ps1 - T7 integration test build (sm120 / Windows)
#
#  Produces work/app_l2.exe linked against:
#    - output/lib/nvcuda.lib   (the shim DLL-proxy import library)
#    - cudart (CUDA runtime)
#
#  Prerequisites:
#    - XSched built & installed (output/bin/nvcuda.dll, output/lib)
#    - CUDA 12.9 toolkit (nvcc, cudart)
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
    throw "output/lib/nvcuda.lib missing - build & install XSched first (make.bat + make install)"
}

New-Item -ItemType Directory -Force -Path "$Dir\work" | Out-Null

Enter-MsvcEnvironment

Write-Host '[build] nvcc app_l2.cu -> work/app_l2.exe'
nvcc -O2 -std=c++17 -Xcompiler "/utf-8" -o "$Dir\work\app_l2.exe" app_l2.cu `
    -I "$RepoRoot\output\include" `
    -I "$RepoRoot\platforms\cuda\hal\include" `
    -L "$RepoRoot\output\lib" -lnvcuda `
    -L "$env:CUDA_PATH\lib\x64" -lcudart
if ($LASTEXITCODE -ne 0) { throw 'nvcc failed' }

Write-Host 'build.ps1: done'
