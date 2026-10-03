# =====================================================================
#  run.ps1 - execute a compiled probe under the cuxtra driver contract.
#
#  Responsibilities:
#    1. Export CUXTRA_CUDA_LIB so the static libcuxtra loads the real
#       driver (nvcuda.dll) instead of guessing.
#    2. Resolve the probe executable (src\<name>.exe by default).
#    3. Run it, streaming stdout+stderr to the console AND archiving a
#       timestamped copy under evidence\ for the record.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File run.ps1 -Probe probe_export_table
#    powershell -ExecutionPolicy Bypass -File run.ps1 -Probe probe_trap -ProbeArgs --p6,--scan
#    powershell -ExecutionPolicy Bypass -File run.ps1 -Probe src\probe_x.exe -CudaLib C:\Windows\System32\nvcuda.dll
#    powershell -ExecutionPolicy Bypass -File run.ps1 -List
# =====================================================================
param(
    [string]$Probe = '',                 # probe base name, .exe, or path
    [string[]]$ProbeArgs = @(),          # args forwarded verbatim to the probe
    [string]$CudaLib = 'C:\Windows\System32\nvcuda.dll',
    [switch]$List                        # list available probes and exit
)

$ErrorActionPreference = 'Stop'
$Root    = $PSScriptRoot
$SrcDir  = Join-Path $Root 'src'
$EvidDir = Join-Path $Root 'evidence'
New-Item -ItemType Directory -Force -Path $EvidDir | Out-Null

function Write-Step($m) { Write-Host "[run ] $m" -ForegroundColor Cyan }

# --- enumerate available probes ----------------------------------------
$Exes = Get-ChildItem -Path $SrcDir -Filter '*.exe' -ErrorAction SilentlyContinue
if ($List) {
    Write-Step 'available probes in src\:'
    if ($Exes) { $Exes | ForEach-Object { Write-Host "        $($_.BaseName)" } }
    else       { Write-Host '        (none built yet - run build.ps1 first)' }
    return
}

if (-not $Probe) {
    throw 'no probe specified. Use -Probe <name> (or -List to see what is built).'
}

# --- resolve the executable path ---------------------------------------
$ExePath = $null
if (Test-Path $Probe -PathType Leaf) {
    $ExePath = (Resolve-Path $Probe).Path
} else {
    $name = $Probe -replace '\.exe$',''
    $cand = Join-Path $SrcDir "$name.exe"
    if (Test-Path $cand) { $ExePath = $cand }
}
if (-not $ExePath) {
    throw "probe executable not found: '$Probe' (looked in $SrcDir). Build it first."
}

# --- export the cuxtra driver contract ---------------------------------
if (-not (Test-Path $CudaLib)) { Write-Host "[warn] CUDA lib not present: $CudaLib" -ForegroundColor Yellow }
$env:CUXTRA_CUDA_LIB = $CudaLib
Write-Step "CUXTRA_CUDA_LIB = $env:CUXTRA_CUDA_LIB"
Write-Step "probe           = $ExePath"
if ($ProbeArgs) { Write-Step "args            = $($ProbeArgs -join ' ')" }

# --- run, tee to console + evidence ------------------------------------
$stamp  = Get-Date -Format 'yyyyMMdd_HHmmss'
$base   = [IO.Path]::GetFileNameWithoutExtension($ExePath)
$logFile = Join-Path $EvidDir ("{0}_{1}.log" -f $base, $stamp)

Write-Host ('-' * 68)
if ($ProbeArgs) {
    & $ExePath @ProbeArgs 2>&1 | Tee-Object -FilePath $logFile
} else {
    & $ExePath 2>&1 | Tee-Object -FilePath $logFile
}
$code = $LASTEXITCODE
Write-Host ('-' * 68)

Write-Step "exit code = $code"
Write-Step "evidence  = $logFile"
exit $code