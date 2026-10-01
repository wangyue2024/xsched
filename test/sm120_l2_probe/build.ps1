# =====================================================================
#  build.ps1 - T1 probe build script (sm120 / Windows)
#
#  Stages:
#    device : nvcc -cubin probe_kernels.cu   -> probe_kernels.cubin
#             cuobjdump -sass                -> probe_kernels.sass.txt
#    host   : g++ probe_main.cpp + libcuxtra -> probe_main.exe
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File build.ps1                # all
#    powershell -ExecutionPolicy Bypass -File build.ps1 -Stage device
#
#  Notes:
#    - nvcc on Windows needs the MSVC host environment (cl.exe for the
#      preprocessing phase); activated via Microsoft.VisualStudio.
#      DevShell.dll (Enter-VsDevShell).
#    - The probe kernels must stay launchable entry functions, therefore
#      the inject toolchain flags '--keep-device-functions -Xptxas
#      -astoolspatch' are NOT used here: ptxas rejects entry functions
#      with '--compile-as-tools-patch' ("Entry function is not allowed").
#      The placeholder hypothesis under test is that plain `brkpt;`
#      compiles to the same 16-byte BPT.TRAP pattern regardless of the
#      astoolspatch flag; this is verified from the SASS dump below.
#    - host build links the prebuilt static cuxtra library for Windows:
#        3rdparty/cuxtra/lib/libcuxtra_windows_amd64.a  (+ shlwapi)
# =====================================================================
param(
    [ValidateSet('device', 'host', 'all')]
    [string]$Stage = 'all'
)

$ErrorActionPreference = 'Stop'
$ProbeDir = $PSScriptRoot
Set-Location $ProbeDir
$RepoRoot = (Resolve-Path (Join-Path $ProbeDir '..\..')).Path

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

    Write-Host '[device] nvcc -cubin probe_kernels.cu (sm_120)'
    nvcc -cubin probe_kernels.cu -o probe_kernels.cubin -arch=sm_120
    if ($LASTEXITCODE -ne 0) { throw 'nvcc failed' }

    Write-Host '[device] cuobjdump -sass -> probe_kernels.sass.txt'
    $sass = cuobjdump -sass probe_kernels.cubin | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'cuobjdump failed' }
    # plain ASCII, LF preserved as produced by the tool
    [IO.File]::WriteAllText("$ProbeDir\probe_kernels.sass.txt", $sass,
                            [Text.Encoding]::ASCII)
}

if ($Stage -in @('host', 'all')) {
    if (-not (Test-Path (Join-Path $ProbeDir 'probe_main.cpp'))) {
        Write-Host '[host] probe_main.cpp not present yet - skipping host build'
    } else {
        Write-Host '[host] g++ probe_main.cpp (link libcuxtra + shlwapi)'
        g++ -O1 -std=c++17 -I "$RepoRoot\3rdparty\cuxtra\include" `
            probe_main.cpp `
            "$RepoRoot\3rdparty\cuxtra\lib\libcuxtra_windows_amd64.a" `
            -lshlwapi -lpthread -static-libgcc -static-libstdc++ -o probe_main.exe
        if ($LASTEXITCODE -ne 0) { throw 'g++ host build failed' }
    }
}

Write-Host 'build.ps1: done'
