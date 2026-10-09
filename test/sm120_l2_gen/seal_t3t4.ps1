# =====================================================================
#  seal_t3t4.ps1 - T3/T4 evidence sealing (SHA256SUMS generator)
#
#  Hashes: every file under evidence/ (recursive), the extract_sass.py
#  tool + README, the T3/T4 reports and scripts, the inject toolchain
#  artifacts (sm86/sm120 baseline cubes & disassembly) and the official
#  sm86.cpp arrays.  Emits evidence/SHA256SUMS_T3T4.txt in sha256sum
#  format ("<hash>  <repo-relative path>").  Re-runnable; the output
#  file itself is excluded from the manifest.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File seal_t3t4.ps1
# =====================================================================
$ErrorActionPreference = 'Stop'
$GenDir = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $GenDir '..\..')).Path
$sumsPath = Join-Path $GenDir 'evidence\SHA256SUMS_T3T4.txt'

function Get-RepoRel([string]$full) {
    return $full.Substring($RepoRoot.Length + 1).Replace('\', '/')
}

$rows = New-Object System.Collections.Generic.List[string]

# ---- A: every file under evidence/ (except the sums file itself) ----
Get-ChildItem -Recurse -File (Join-Path $GenDir 'evidence') |
    Where-Object { $_.Name -ne 'SHA256SUMS_T3T4.txt' } |
    Sort-Object FullName | ForEach-Object {
        $h = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
        $rows.Add("$h  $(Get-RepoRel $_.FullName)")
    }

# ---- B: tool + docs + scripts ----
$sources = @(
    'tools/instrument/extract_sass.py',
    'tools/instrument/README.md',
    'test/sm120_l2_gen/T3_REPORT.md',
    'test/sm120_l2_gen/T4_REPORT.md',
    'test/sm120_l2_gen/run_all_t3t4.ps1',
    'test/sm120_l2_gen/run_all_t3t4.sh',
    'test/sm120_l2_gen/seal_t3t4.ps1',
    'test/sm120_l2_gen/seal_t3t4.sh'
)

# ---- C: inject toolchain artifacts (baseline + official + derived) ----
$artifacts = @(
    'platforms/cuda/hal/inject/Makefile',
    'platforms/cuda/hal/inject/make_msvc.bat',
    'platforms/cuda/hal/inject/inject.cu',
    'platforms/cuda/hal/inject/inject_120.cubin',
    'platforms/cuda/hal/inject/inject_120.asm',
    'platforms/cuda/hal/inject/inject_120_cc.asm',
    'platforms/cuda/hal/inject/inject_120.json',
    'platforms/cuda/hal/inject/inject_86.cubin',
    'platforms/cuda/hal/inject/inject_86.asm',
    'platforms/cuda/hal/inject/inject_86_cc.asm',
    'platforms/cuda/hal/inject/inject_86.json',
    'platforms/cuda/hal/inject/inject_sm86.asm',
    'platforms/cuda/hal/src/arch/sm86.cpp'
)

foreach ($p in ($sources + $artifacts)) {
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
