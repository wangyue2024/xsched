# =====================================================================
#  build.ps1 - MinGW g++ build driver for the sm120 L3 trap-handler probes.
#
#  Mirrors the xsched host toolchain (test/sm120_l2_probe/build.ps1):
#    g++ -std=c++17  +  static libcuxtra  +  -lshlwapi -lpthread
#                    +  -static-libgcc -static-libstdc++
#
#  The cuxtra static library resolves the *real* driver at run time via
#  the CUXTRA_CUDA_LIB environment variable (set by run.ps1), so the host
#  build only needs the archive + shlwapi.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File build.ps1                 # build every src\*.cpp
#    powershell -ExecutionPolicy Bypass -File build.ps1 -Target probe_export_table
#    powershell -ExecutionPolicy Bypass -File build.ps1 -Target probe_trap -Opt O2
#    powershell -ExecutionPolicy Bypass -File build.ps1 -Clean
#
#  Output: each foo.cpp -> foo.exe placed in the SAME directory (src\).
# =====================================================================
param(
    [string]$Target = '',            # probe base name (no ext); empty = all src\*.cpp
    [ValidateSet('O0','O1','O2','Og','Os')]
    [string]$Opt = 'O1',
    [string[]]$ExtraCxx = @(),       # extra flags forwarded to g++
    [switch]$Clean,
    [switch]$Verbose2                # -Verbose is reserved; use -Verbose2 for g++ -v
)

$ErrorActionPreference = 'Stop'
$Root    = $PSScriptRoot
$SrcDir  = Join-Path $Root 'src'
$IncDir  = Join-Path $Root 'include'

# --- paths into the (unmodified) xsched checkout -----------------------
$XschedRoot  = 'd:\1file\Desktop\code\xsched'
$CuxtraInc   = Join-Path $XschedRoot '3rdparty\cuxtra\include'
$CuxtraLib   = Join-Path $XschedRoot '3rdparty\cuxtra\lib\libcuxtra_windows_amd64.a'

function Write-Step($m) { Write-Host "[build] $m" -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "[ ok ] $m"   -ForegroundColor Green }
function Write-Warn2($m){ Write-Host "[warn] $m"   -ForegroundColor Yellow }

# --- locate the compiler ------------------------------------------------
$Gpp = (Get-Command g++ -ErrorAction SilentlyContinue)
if (-not $Gpp) {
    throw 'g++ not found on PATH. Install MinGW-w64 (x86_64-win32-seh) and retry.'
}
Write-Step "compiler : $($Gpp.Source)"
& g++ --version | Select-Object -First 1 | ForEach-Object { Write-Step "version  : $_" }

# --- sanity-check cuxtra inputs ----------------------------------------
if (-not (Test-Path $CuxtraInc)) { Write-Warn2 "cuxtra include not found: $CuxtraInc" }
if (-not (Test-Path $CuxtraLib)) { Write-Warn2 "cuxtra static lib not found: $CuxtraLib" }

# --- clean mode ---------------------------------------------------------
if ($Clean) {
    Write-Step 'removing src\*.exe'
    Get-ChildItem -Path $SrcDir -Filter '*.exe' -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item $_.FullName -Force; Write-Ok "removed $($_.Name)" }
    Write-Host 'build.ps1: clean done'
    return
}

if (-not (Test-Path $SrcDir)) { throw "src directory missing: $SrcDir" }

# --- resolve the set of sources to build -------------------------------
$Sources = @()
if ($Target) {
    $cand = Join-Path $SrcDir "$Target.cpp"
    if (-not (Test-Path $cand)) { throw "source not found: $cand" }
    $Sources += Get-Item $cand
} else {
    $Sources += Get-ChildItem -Path $SrcDir -Filter '*.cpp' -ErrorAction SilentlyContinue
}
if ($Sources.Count -eq 0) {
    Write-Warn2 'no src\*.cpp found - nothing to build (add a probe source first).'
    Write-Host 'build.ps1: done'
    return
}

# --- compile each source into an exe beside it -------------------------
$Failed = @()
foreach ($src in $Sources) {
    $exe = [IO.Path]::ChangeExtension($src.FullName, '.exe')
    Write-Step "compiling $($src.Name) -> $(Split-Path $exe -Leaf)"

    $cxxArgs = @(
        "-$Opt", '-std=c++17', '-Wall', '-Wextra',
        '-I', $IncDir,
        '-I', $CuxtraInc,
        $src.FullName,
        $CuxtraLib,
        '-lshlwapi', '-lpthread',
        '-static-libgcc', '-static-libstdc++',
        '-o', $exe
    )
    if ($Verbose2)      { $cxxArgs += '-v' }
    if ($ExtraCxx)      { $cxxArgs += $ExtraCxx }

    & g++ @cxxArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] g++ returned $LASTEXITCODE for $($src.Name)" -ForegroundColor Red
        $Failed += $src.Name
        continue
    }
    Write-Ok "built $exe"
}

Write-Host ''
if ($Failed.Count -gt 0) {
    Write-Host "build.ps1: FAILED -> $($Failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "build.ps1: done ($($Sources.Count) target(s))" -ForegroundColor Green