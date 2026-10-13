# =====================================================================
#  run_coop.ps1 - coop-hazard demo runner with watchdog
#
#  Runs coop_main.exe in a child process with a hard timeout; if the
#  process does not exit in time it is killed (the in-kernel barrier is
#  time-bounded, so a watchdog kill should never be needed -- this is a
#  second line of defense, mirroring the "never hang the GPU" policy).
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File run_coop.ps1 -Mode s1
#    powershell -ExecutionPolicy Bypass -File run_coop.ps1 -Mode e1
#    powershell -ExecutionPolicy Bypass -File run_coop.ps1 -Mode e2 -Iterations 240
# =====================================================================
param(
    [ValidateSet('s1', 'e1', 'e2', 'all')]
    [string]$Mode = 'e1',
    [int]$Iterations = 240,
    [int]$TimeoutSec = 300,
    [string]$Tag = 'run',
    [int]$Threads = 0,
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'
$Dir = $PSScriptRoot
Set-Location $Dir

if (-not (Test-Path 'coop_main.exe')) { & (Join-Path $Dir 'build.ps1') }
if (-not (Test-Path 'coop_kernel.cubin')) { throw 'coop_kernel.cubin missing (build device stage)' }

# cuxtra needs the real driver library (same convention as the MVE runner)
$env:CUXTRA_CUDA_LIB = 'C:\Windows\System32\nvcuda.dll'

New-Item -ItemType Directory -Force -Path "$Dir\evidence" | Out-Null
$log = Join-Path $Dir "evidence\coop_${Tag}_${Mode}.log"
$err = Join-Path $Dir "evidence\coop_${Tag}_${Mode}.err.log"

$modeArg = switch ($Mode) {
    's1'  { '--only-s1' }
    'e1'  { '--only-e1' }
    'e2'  { '--only-e2' }
    'all' { '' }
}
$threadsArg = @()
if ($Threads -gt 0) { $threadsArg = @('--threads', "$Threads") }
$runArgs = @('coop_kernel.cubin') + @($modeArg) + @('--iters', "$Iterations") + $threadsArg + $ExtraArgs |
    Where-Object { $_ -ne '' }

Write-Host "[run] coop_main.exe $($runArgs -join ' ')  (watchdog ${TimeoutSec}s)"
$p = Start-Process -FilePath ".\coop_main.exe" -ArgumentList $runArgs `
    -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError $err

$exited = $p.WaitForExit($TimeoutSec * 1000)
if (-not $exited) {
    Write-Host '[run] WATCHDOG TRIGGERED: killing the process' -ForegroundColor Yellow
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) {
        Write-Host '[run] kill FAILED - process stuck in a driver-level wait' -ForegroundColor Red
    }
    Write-Host '[run] --- log tail ---'
    Get-Content $log -Tail 30 -ErrorAction SilentlyContinue
    exit 3
}

Write-Host "[run] exit=$($p.ExitCode) log=$log"
$pass = ([regex]::Matches((Get-Content $log -Raw), '\[PASS\]')).Count
$fail = ([regex]::Matches((Get-Content $log -Raw), '\[FAIL\]')).Count
$hits = ([regex]::Matches((Get-Content $log -Raw), '\[HIT\]')).Count
Write-Host "[run] PASS=$pass FAIL=$fail PARTIAL_HITS=$hits"
Get-Content $log -Tail 25
exit $p.ExitCode
