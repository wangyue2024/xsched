# =====================================================================
#  seal_t7.ps1 - T7 integration evidence sealing (SHA256SUMS generator)
#
#  Hashes: integration test sources/scripts/docs/logs, the changed
#  platform files (guardian.h / sm120.h / sm120.cpp / instrument.cpp /
#  arch.cpp), the environment-boundary investigation tools and the
#  MVE logs produced during the T7 session.
#  Emits evidence/SHA256SUMS_T7.txt (sha256sum format, re-runnable).
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File seal_t7.ps1
# =====================================================================
$ErrorActionPreference = 'Stop'
$IntDir = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $IntDir '..\..')).Path
$sumsPath = Join-Path $IntDir 'evidence\SHA256SUMS_T7.txt'

function Get-RepoRel([string]$full) {
    return $full.Substring($RepoRoot.Length + 1).Replace('\', '/')
}

$rows = New-Object System.Collections.Generic.List[string]

# ---- A: files under test/sm120_integration (sources + logs) ----
Get-ChildItem -Recurse -File $IntDir |
    Where-Object { $_.Name -ne 'SHA256SUMS_T7.txt' -and $_.FullName -notlike '*\work\*' } |
    Sort-Object FullName | ForEach-Object {
        $h = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
        $rows.Add("$h  $(Get-RepoRel $_.FullName)")
    }

# ---- B: changed platform files (the integration itself) ----
$platform = @(
    'platforms/cuda/hal/include/xsched/cuda/hal/level2/guardian.h',
    'platforms/cuda/hal/include/xsched/cuda/hal/arch/sm120.h',
    'platforms/cuda/hal/src/arch/sm120.cpp',
    'platforms/cuda/hal/src/arch/arch.cpp',
    'platforms/cuda/hal/src/level2/instrument.cpp'
)

# ---- C: environment-boundary investigation tools + MVE session logs ----
$investigation = @(
    'test/sm120_mve/pure_cuda_check.cu',
    'test/sm120_mve/mve_m0_repro.cu',
    'test/sm120_mve/mve_main.cpp',
    'test/sm120_mve/run_mve.ps1'
)
$mveLogs = Get-ChildItem -File (Join-Path $RepoRoot 'test\sm120_mve\evidence') -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'mve_(t7_regression|stability|diag|preflight)' } |
    Sort-Object FullName | ForEach-Object { Get-RepoRel $_.FullName }

foreach ($p in ($platform + $investigation + @($mveLogs))) {
    $full = Join-Path $RepoRoot $p
    if (Test-Path -LiteralPath $full) {
        $h = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLower()
        $rows.Add("$h  $p")
    } else {
        $rows.Add("# MISSING: $p")
    }
}

Set-Content -LiteralPath $sumsPath -Value $rows -Encoding ascii
Write-Host "sealed: $($rows.Count) entries -> $sumsPath"
