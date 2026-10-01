# XSched Local Test Runner for Windows / RTX 5060
param (
    [ValidateSet("all", "driver", "pytorch", "hpf", "cfs", "global")]
    [string]$Target = "all"
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir

Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "   XSched Local Test Suite (RTX 5060 / Windows)   " -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan

# Ensure output DLL is present in tests_local
Copy-Item "$RepoRoot\output\bin\nvcuda.dll" "$ScriptDir\nvcuda.dll" -Force

# Base Environment Variables for non-invasive local testing
$env:XSCHED_CUDA_LIB = "C:\Windows\System32\nvcuda.dll"
$env:CUXTRA_CUDA_LIB = "C:\Windows\System32\nvcuda.dll"
$env:XSCHED_AUTO_XQUEUE = "ON"

function Run-DriverTest($policy) {
    Write-Host "`n>>> [Test: CUDA Driver API (Policy: $policy)]" -ForegroundColor Green
    if ($policy -eq "APP") {
        $env:XSCHED_SCHEDULER = "APP"
        Remove-Item Env:\XSCHED_POLICY -ErrorAction SilentlyContinue
    } else {
        $env:XSCHED_POLICY = $policy
    }
    Push-Location $ScriptDir
    try {
        .\test_cuda_driver.exe
        if ($LASTEXITCODE -eq 0) {
            Write-Host ">>> [Driver Test PASSED!]" -ForegroundColor Green
        } else {
            Write-Host ">>> [Driver Test FAILED!]" -ForegroundColor Red
        }
    } finally {
        Pop-Location
    }
}

function Run-PyTorchTest($policy) {
    Write-Host "`n>>> [Test: PyTorch Workload (Policy: $policy)]" -ForegroundColor Green
    $env:XSCHED_POLICY = $policy
    
    # Temporarily place nvcuda.dll next to python.exe or run with path
    $pythonExe = (Get-Command python).Source
    $pythonDir = Split-Path -Parent $pythonExe
    $destDll = Join-Path $pythonDir "nvcuda.dll"
    Copy-Item "$ScriptDir\nvcuda.dll" $destDll -Force
    
    try {
        python "$ScriptDir\test_pytorch.py"
        if ($LASTEXITCODE -eq 0) {
            Write-Host ">>> [PyTorch Test PASSED!]" -ForegroundColor Green
        } else {
            Write-Host ">>> [PyTorch Test FAILED!]" -ForegroundColor Red
        }
    } finally {
        Remove-Item $destDll -Force -ErrorAction SilentlyContinue
    }
}

switch ($Target) {
    "driver"  { Run-DriverTest "HPF" }
    "pytorch" { Run-PyTorchTest "HPF" }
    "hpf"     { Run-DriverTest "HPF"; Run-PyTorchTest "HPF" }
    "cfs"     { Run-DriverTest "CFS"; Run-PyTorchTest "CFS" }
    "global"  {
        $env:XSCHED_SCHEDULER = "GLB"
        Remove-Item Env:\XSCHED_POLICY -ErrorAction SilentlyContinue
        Push-Location $ScriptDir
        .\test_cuda_driver.exe
        Pop-Location
    }
    "all" {
        Run-DriverTest "APP"
        Run-DriverTest "HPF"
        Run-DriverTest "CFS"
        Run-PyTorchTest "HPF"
    }
}

Write-Host "`n==================================================" -ForegroundColor Cyan
Write-Host "            Test Execution Finished               " -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
