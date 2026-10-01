# Complete Multi-Scheme Benchmark Runner
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BaseDir = Split-Path -Parent $ScriptDir

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "   XSched vs Native CUDA vs Optimized Comprehensive Benchmark   " -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan

# Set common XSched env
$env:XSCHED_CUDA_LIB = "C:\Windows\System32\nvcuda.dll"
$env:CUXTRA_CUDA_LIB = "C:\Windows\System32\nvcuda.dll"
$env:XSCHED_AUTO_XQUEUE = "ON"
$env:XSCHED_POLICY = "cfs"
$env:XSCHED_LOG_LEVEL = "WARN"

# 1. Native CUDA Driver
Write-Host "`n>>> [RUNNING 1/4] Pure Native NVIDIA Driver (System32 nvcuda.dll)..." -ForegroundColor Yellow
Push-Location "$ScriptDir\native"
try {
    .\test_native.exe "Pure Native NVIDIA Driver" 1> "..\result_native.txt" 2> "..\stderr_native.log"
    Get-Content "..\result_native.txt"
} finally {
    Pop-Location
}

# 2. XSched Legacy (ForEachWaitAll)
Write-Host "`n>>> [RUNNING 2/4] XSched Legacy (Policy: legacy / ForEachWaitAll)..." -ForegroundColor Yellow
$env:XSCHED_MEMFREE_POLICY = "legacy"
Push-Location $ScriptDir
try {
    .\test_comprehensive.exe "XSched Legacy (ForEachWaitAll)" 1> "result_legacy.txt" 2> "stderr_legacy.log"
    Get-Content "result_legacy.txt"
} finally {
    Pop-Location
}

# 3. XSched Optimized (Deferred Free)
Write-Host "`n>>> [RUNNING 3/4] XSched Optimized (Policy: deferred / Non-blocking Epoch)..." -ForegroundColor Yellow
$env:XSCHED_MEMFREE_POLICY = "deferred"
Push-Location $ScriptDir
try {
    .\test_comprehensive.exe "XSched Optimized (Deferred Reclamation)" 1> "result_deferred.txt" 2> "stderr_deferred.log"
    Get-Content "result_deferred.txt"
} finally {
    Pop-Location
}

# 4. XSched Direct (Direct Driver Free)
Write-Host "`n>>> [RUNNING 4/4] XSched Direct (Policy: direct / Theoretical Peak)..." -ForegroundColor Yellow
$env:XSCHED_MEMFREE_POLICY = "direct"
Push-Location $ScriptDir
try {
    .\test_comprehensive.exe "XSched Direct (Bypass WaitAll)" 1> "result_direct.txt" 2> "stderr_direct.log"
    Get-Content "result_direct.txt"
} finally {
    Pop-Location
}

Write-Host "`n================================================================" -ForegroundColor Green
Write-Host "   ALL EXPERIMENTS COMPLETED SUCCESSFULLY!                      " -ForegroundColor Green
Write-Host "================================================================" -ForegroundColor Green
