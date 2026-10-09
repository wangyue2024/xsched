# =====================================================================
#  seal_t5t6.ps1 - T5/T6 evidence sealing (SHA256SUMS generator)
#
#  Hashes: every file under evidence/ (recursive), the MVE sources and
#  docs, T5 platform deliverables (sm120.h / sm120.cpp) and the T5/T6
#  scripts.  Emits evidence/SHA256SUMS_T5T6.txt in sha256sum format.
#  Re-runnable; the output file itself is excluded from the manifest.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File seal_t5t6.ps1
# =====================================================================
$ErrorActionPreference = 'Stop'
$MveDir = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $MveDir '..\..')).Path
$sumsPath = Join-Path $MveDir 'evidence\SHA256SUMS_T5T6.txt'

function Get-RepoRel([string]$full) {
    return $full.Substring($RepoRoot.Length + 1).Replace('\', '/')
}

$rows = New-Object System.Collections.Generic.List[string]

# ---- A: every file under evidence/ (except the sums file itself) ----
Get-ChildItem -Recurse -File (Join-Path $MveDir 'evidence') |
    Where-Object { $_.Name -ne 'SHA256SUMS_T5T6.txt' } |
    Sort-Object FullName | ForEach-Object {
        $h = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
        $rows.Add("$h  $(Get-RepoRel $_.FullName)")
    }

# ---- B: T5 platform deliverables ----
$platform = @(
    'platforms/cuda/hal/include/xsched/cuda/hal/arch/sm120.h',
    'platforms/cuda/hal/src/arch/sm120.cpp'
)

# ---- C: MVE sources, scripts and docs ----
$sources = @(
    'test/sm120_mve/mve_main.cpp',
    'test/sm120_mve/dummy_kernel.cu',
    'test/sm120_mve/mve_arrays.h',
    'test/sm120_mve/gen_arrays.py',
    'test/sm120_mve/build.ps1',
    'test/sm120_mve/run_mve.ps1',
    'test/sm120_mve/build.sh',
    'test/sm120_mve/run_mve.sh',
    'test/sm120_mve/t5_verify.py',
    'test/sm120_mve/seal_t5t6.ps1',
    'test/sm120_mve/seal_t5t6.sh',
    'test/sm120_mve/DESIGN.md',
    'test/sm120_mve/T5_REVIEW.md',
    'test/sm120_mve/REPORT.md'
)

# ---- D: MVE build artifacts (the exact objects that were run) ----
$artifacts = @(
    'test/sm120_mve/mve_kernel.cubin',
    'test/sm120_mve/mve_kernel.sass.txt'
)

foreach ($p in ($platform + $sources + $artifacts)) {
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
