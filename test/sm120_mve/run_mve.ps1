# =====================================================================
#  run_mve.ps1 - T6 MVE one-click: build (if needed) + run + evidence
#
#  Produces UTF-8 logs under evidence/ (PowerShell's native redirect is
#  UTF-16; this script writes UTF-8 explicitly) and verifies the
#  pass/fail counters:
#      expected: N PASS, 0 FAIL, 0 ERR, "RESULT: 0 failed check(s)"
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File run_mve.ps1
#    powershell -ExecutionPolicy Bypass -File run_mve.ps1 -Quick
#    powershell -ExecutionPolicy Bypass -File run_mve.ps1 -SkipBuild
# =====================================================================
param(
    [switch]$Quick,
    [switch]$SkipBuild,
    [string]$Tag = 'run'
)

$ErrorActionPreference = 'Stop'
$Dir = $PSScriptRoot
Set-Location $Dir

if (-not $SkipBuild) {
    & (Join-Path $Dir 'build.ps1')
}

if (-not (Test-Path 'mve_kernel.cubin')) { throw 'mve_kernel.cubin missing (build device stage)' }
if (-not (Test-Path 'mve_main.exe'))     { throw 'mve_main.exe missing (build host stage)' }

$env:CUXTRA_CUDA_LIB = 'C:\Windows\System32\nvcuda.dll'

$runArgs = @('mve_kernel.cubin')
if ($Quick) { $runArgs += '--quick' }

Write-Host "[run] mve_main.exe $($runArgs -join ' ')"
$out = & .\mve_main.exe @runArgs 2>&1 | Out-String
$exit = $LASTEXITCODE

$log = Join-Path $Dir "evidence\mve_$Tag.log"
[IO.File]::WriteAllText($log, $out, (New-Object Text.UTF8Encoding($false)))
Write-Host "[run] exit=$exit log=$log"

$pass = ([regex]::Matches($out, '\[PASS\]')).Count
$fail = ([regex]::Matches($out, '\[FAIL\]')).Count
$err  = ([regex]::Matches($out, '\[ERR \]')).Count
Write-Host "[run] PASS=$pass FAIL=$fail ERR=$err"

if ($fail -ne 0 -or $err -ne 0 -or $exit -ne 0) {
    Write-Host '[run] FAILED' -ForegroundColor Red
    exit 1
}
Write-Host '[run] all checks passed' -ForegroundColor Green
exit 0
