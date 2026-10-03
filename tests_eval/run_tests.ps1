# XSched CUDA Shim test runner (Windows / MinGW)
# Usage:  powershell -ExecutionPolicy Bypass -File run_tests.ps1
#
# Prerequisites:
#   - MinGW binaries (gcc/g++/mingw32-make) on PATH
#   - nvcuda.dll rebuilt:  cd ..\build; mingw32-make -j8
#   - A CUDA-capable GPU with the stock driver at C:\Windows\System32\nvcuda.dll
#
# Note: XSCHED_CUDA_LIB / CUXTRA_CUDA_LIB MUST be visible to the test process
#       at startup (they are resolved by the shim and the cuxtra backend).

$ErrorActionPreference = 'Continue'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

$repoRoot = Split-Path -Parent $scriptDir
$env:XSCHED_CUDA_LIB = 'C:\Windows\System32\nvcuda.dll'
$env:CUXTRA_CUDA_LIB = 'C:\Windows\System32\nvcuda.dll'
if (-not $env:XSCHED_SHIM_PATH) {
    $env:XSCHED_SHIM_PATH = Join-Path $repoRoot 'build\platforms\cuda\nvcuda.dll'
}

$tests = @(
    'test_unit_registry.exe',
    'test_driver_shim.exe',
    'test_isolation.exe',
    'test_multithread_stress.exe',
    'test_destroy_inflight.exe',
    'test_ptds_features.exe'
)

$failed = 0
foreach ($mode in @('ON', 'OFF')) {
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host "  XSCHED_AUTO_XQUEUE = $mode" -ForegroundColor Cyan
    Write-Host "================================================================" -ForegroundColor Cyan
    $env:XSCHED_AUTO_XQUEUE = $mode
    foreach ($t in $tests) {
        Write-Host ""
        Write-Host "--- $t ---" -ForegroundColor Yellow
        & ".\$t" 2>&1 | Select-String -Pattern "PASSED|FAIL|ALL|Step|Phase|latency|\[F[0-9]\]|\[ISOLATION\]|\[T[0-9]\]|errors|delta"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "!!! $t FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
            $failed++
        }
    }
}

Write-Host ""
if ($failed -eq 0) {
    Write-Host "ALL TEST SUITES PASSED (both modes)" -ForegroundColor Green
} else {
    Write-Host "$failed test run(s) FAILED" -ForegroundColor Red
    exit 1
}
