# =====================================================================
#  run_full_scan.ps1 - exhaustive sm120 constant-bank sweep (T1).
#
#  Generates the batch base list covering [Start, End] with point stride
#  `Step` and `Count` points per batch, then drives run_scan.ps1 for all
#  batches (archived under evidence/scan_v2/).
#
#  Coverage: span = Step * Count bytes per batch
#  (default: 8 x 62 = 496 bytes per batch, gapless).
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File run_full_scan.ps1
#    powershell -ExecutionPolicy Bypass -File run_full_scan.ps1 `
#               -Start 0x0 -End 0x4200 -Step 8 -Count 62 -Tag full
# =====================================================================
param(
    [int]$Start = 0x0,
    [int]$End   = 0x4200,
    [int]$Step  = 8,
    [int]$Count = 62,
    [string]$Tag = 'full'
)

$ErrorActionPreference = 'Stop'
$span = $Step * $Count
$bases = @()
for ($b = $Start; $b -le $End; $b += $span) {
    $bases += ('0x{0:x}' -f $b)
}
Write-Host ("run_full_scan.ps1: {0} batches, span=0x{1:x} bytes each, range 0x{2:x}..0x{3:x}" `
    -f $bases.Count, $span, $Start, $End)

& (Join-Path $PSScriptRoot 'run_scan.ps1') -Bases $bases -Step $Step -Count $Count -Tag $Tag
Write-Host 'run_full_scan.ps1: done'
