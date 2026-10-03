# =====================================================================
#  run.ps1 - T7 integration test runner (sm120 / Windows, non-invasive)
#
#  Layout & environment (per-app DLL proxying, no system files touched):
#    work/
#      app_l2.exe      built by build.ps1 (links output/lib/nvcuda.lib)
#      nvcuda.dll      copied from output/bin  (the XSched shim)
#    env:
#      XSCHED_CUDA_LIB  = C:\Windows\System32\nvcuda.dll (real driver
#                         used by the HAL inside the shim)
#      CUXTRA_CUDA_LIB  = C:\Windows\System32\nvcuda.dll (real driver
#                         used by the cuxtra library inside the shim)
#      XSCHED_SCHEDULER = APP (application-managed scheduling)
#
#  cudart loads "nvcuda.dll" from the exe directory (DLL search order),
#  so every CUDA call of the app enters the shim; the shim's HAL loads
#  the real driver by the env path above.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File run.ps1            # full
#    powershell -ExecutionPolicy Bypass -File run.ps1 -SkipBuild
#    powershell -ExecutionPolicy Bypass -File run.ps1 -Tag run2
# =====================================================================
param(
    [switch]$SkipBuild,
    [string]$Tag = 'run1'
)

$ErrorActionPreference = 'Stop'
$Dir = $PSScriptRoot
Set-Location $Dir
$RepoRoot = (Resolve-Path (Join-Path $Dir '..\..')).Path

if (-not $SkipBuild) {
    & (Join-Path $Dir 'build.ps1')
}

$work = Join-Path $Dir 'work'
if (-not (Test-Path "$work\app_l2.exe")) { throw 'work/app_l2.exe missing (run build.ps1)' }

Write-Host '[setup] staging shim nvcuda.dll next to the test executable'
Copy-Item "$RepoRoot\output\bin\nvcuda.dll" "$work\nvcuda.dll" -Force

$env:XSCHED_CUDA_LIB  = 'C:\Windows\System32\nvcuda.dll'
$env:CUXTRA_CUDA_LIB  = 'C:\Windows\System32\nvcuda.dll'
$env:XSCHED_SCHEDULER = 'APP'
$env:XLOG_LEVEL       = 'INFO'   # quiet the per-call DEBG logging of the shim
Remove-Item Env:\XSCHED_AUTO_XQUEUE -ErrorAction SilentlyContinue
Remove-Item Env:\XSCHED_POLICY -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path "$Dir\evidence" | Out-Null
$log = Join-Path $Dir "evidence\app_l2_$Tag.log"

Write-Host '[run] app_l2.exe (full integration suite)'
# XSched logs to stderr; keep native stderr from aborting the script
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$out = & "$work\app_l2.exe" 2>&1 | Out-String
$exit = $LASTEXITCODE
$ErrorActionPreference = $prevEap
[IO.File]::WriteAllText($log, $out, (New-Object Text.UTF8Encoding($false)))

$pass = ([regex]::Matches($out, '\[PASS\]')).Count
$fail = ([regex]::Matches($out, '\[FAIL\]')).Count
$err  = ([regex]::Matches($out, '\[ERR \]')).Count
Write-Host "[run] exit=$exit PASS=$pass FAIL=$fail ERR=$err  log=$log"

if ($fail -ne 0 -or $err -ne 0 -or $exit -ne 0) {
    Write-Host '[run] FAILED' -ForegroundColor Red
    exit 1
}
Write-Host '[run] all checks passed' -ForegroundColor Green
exit 0
