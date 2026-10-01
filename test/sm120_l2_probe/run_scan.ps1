# =====================================================================
#  run_scan.ps1 - T1 probe: constant-bank window scanner driver (v2)
#
#  For each batch base address: patch the probe cubin in --scan mode
#  (Count sample points, 8 bytes each, given stride), run probe_main.exe
#  in --scan mode against it and archive the full log + patched cubin
#  under evidence/scan_v2/.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File run_scan.ps1 -Bases 0x300
#    powershell -ExecutionPolicy Bypass -File run_scan.ps1 `
#               -Bases 0x0,0x1f0 -Step 8 -Count 62 -Tag full
#
#  Sentinel convention: probe_main fills `out` with 0xCC.. before the
#  launch; a sampled cell still reading 0xCC.. was NOT written by the
#  patched kernel (protocol bug), every other value is real cbank
#  content.  MATCH lines are SetDebuggerParams sentinel hits, the
#  self-check line is the out pointer seen at c[0x0][0x380].
# =====================================================================
param(
    [Parameter(Mandatory = $true)][string[]]$Bases,
    [int]$Step = 8,
    [int]$Count = 62,
    [string]$Tag = ''
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$env:CUXTRA_CUDA_LIB = 'C:\Windows\System32\nvcuda.dll'

$evid = Join-Path $PSScriptRoot 'evidence\scan_v2'
New-Item -ItemType Directory -Force -Path $evid | Out-Null

$stepHex = '0x{0:x}' -f $Step
foreach ($base in $Bases) {
    $name = if ($Tag) { "${Tag}_$base" } else { "$base" }
    Write-Host "=== scan_v2 batch base=$base step=$stepHex count=$Count ==="

    $patchlog = python patch_cubin.py probe_kernels.cubin `
        probe_kernels_scan.cubin --scan $base $stepHex $Count
    if ($LASTEXITCODE -ne 0) { throw "patch failed for base $base" }
    $patchlog | Out-File -Encoding ascii (Join-Path $evid "patch_$name.log")

    $out = .\probe_main.exe probe_kernels_scan.cubin --scan $base $stepHex $Count 2>&1
    $out | Out-File -Encoding ascii (Join-Path $evid "run_$name.log")

    $out | Select-String 'MATCH|RESULT-SCAN|self-check|NOT WRITTEN'
    Copy-Item probe_kernels_scan.cubin (Join-Path $evid "cubin_$name.cubin") -Force
}
Write-Host 'run_scan.ps1: done'
