# =====================================================================
#  run.ps1 - L2 check demo runner (per-app DLL proxying, no system
#            files touched; logs go to evidence/)
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File run.ps1                 # DEBG logs
#    powershell -ExecutionPolicy Bypass -File run.ps1 -LogLevel INFO
#    powershell -ExecutionPolicy Bypass -File run.ps1 -SkipBuild -Tag run2
# =====================================================================
param(
    [switch]$SkipBuild,
    [string]$Tag = 'run1',
    [ValidateSet('DEBG', 'INFO', 'WARN', 'ERRO')]
    [string]$LogLevel = 'DEBG'
)

$ErrorActionPreference = 'Stop'
$Dir = $PSScriptRoot
Set-Location $Dir
$RepoRoot = (Resolve-Path (Join-Path $Dir '..\..')).Path

if (-not $SkipBuild) { & (Join-Path $Dir 'build.ps1') }
if (-not (Test-Path "$Dir\work\l2_check.exe")) { throw 'work/l2_check.exe missing (run build.ps1)' }

Write-Host '[setup] staging shim nvcuda.dll next to the test executable'
Copy-Item "$RepoRoot\output\bin\nvcuda.dll" "$Dir\work\nvcuda.dll" -Force

$env:XSCHED_CUDA_LIB  = 'C:\Windows\System32\nvcuda.dll'   # real driver for the shim HAL
$env:CUXTRA_CUDA_LIB  = 'C:\Windows\System32\nvcuda.dll'   # real driver for cuxtra
$env:XSCHED_SCHEDULER = 'APP'                              # we manage XQueues ourselves
$env:XLOG_LEVEL       = $LogLevel
Remove-Item Env:\XSCHED_POLICY -ErrorAction SilentlyContinue
Remove-Item Env:\XSCHED_AUTO_XQUEUE -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path "$Dir\evidence" | Out-Null
$log = Join-Path $Dir "evidence\l2_check_${Tag}_${LogLevel}.log"

Write-Host "[run] work\l2_check.exe (XLOG_LEVEL=$LogLevel)"
# XSched logs to stderr; keep native stderr from aborting the script
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$out = & "$Dir\work\l2_check.exe" 2>&1 | Out-String
$exit = $LASTEXITCODE
$ErrorActionPreference = $prevEap
[IO.File]::WriteAllText($log, $out, (New-Object Text.UTF8Encoding($false)))

$pass = ([regex]::Matches($out, '\[PASS\]')).Count
$fail = ([regex]::Matches($out, '\[FAIL\]')).Count
$err  = ([regex]::Matches($out, '\[ERR \]')).Count
Write-Host "[run] exit=$exit PASS=$pass FAIL=$fail ERR=$err  log=$log"

# show the interception / suspend-resume log lines (great for learning)
Write-Host ''
Write-Host '--- interesting log lines (Suspend/Resume/Deactivate/Reactivate/XQueue) ---'
Select-String -Path $log -Pattern 'Suspend|Resume|Deactivate|Reactivate|XQueue|guardian|preempt' `
    | Select-Object -First 30 | ForEach-Object { '  ' + $_.Line.Trim() }
Write-Host '--------------------------------------------------------------------------'

if ($fail -ne 0 -or $err -ne 0 -or $exit -ne 0) {
    Write-Host '[run] FAILED' -ForegroundColor Red
    exit 1
}
Write-Host '[run] all checks passed' -ForegroundColor Green
exit 0
