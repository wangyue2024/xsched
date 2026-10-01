# =====================================================================
#  seal_evidence.ps1 - T1/T2 evidence sealing (SHA256SUMS generator)
#
#  Hashes every file under evidence/ plus the probe sources and the T2
#  inject artifacts, emitting evidence/SHA256SUMS.txt in sha256sum
#  format ("<hash>  <repo-relative path>").  Re-runnable; the output
#  file itself is excluded from the manifest.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File seal_evidence.ps1
# =====================================================================
$ErrorActionPreference = 'Stop'
$ProbeDir = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $ProbeDir '..\..')).Path
$sumsPath = Join-Path $ProbeDir 'evidence\SHA256SUMS.txt'

function Get-RepoRel([string]$full) {
    return $full.Substring($RepoRoot.Length + 1).Replace('\', '/')
}

$rows = New-Object System.Collections.Generic.List[string]

# ---- A: every file under evidence/ (except the sums file itself) ----
Get-ChildItem -Recurse -File (Join-Path $ProbeDir 'evidence') |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName | ForEach-Object {
        $h = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
        $rows.Add("$h  $(Get-RepoRel $_.FullName)")
    }

# ---- B: probe sources & deliverables ----
$sources = @(
    'test/sm120_l2_probe/probe_kernels.cu',
    'test/sm120_l2_probe/probe_main.cpp',
    'test/sm120_l2_probe/patch_cubin.py',
    'test/sm120_l2_probe/build.ps1',
    'test/sm120_l2_probe/run_scan.ps1',
    'test/sm120_l2_probe/run_full_scan.ps1',
    'test/sm120_l2_probe/seal_evidence.ps1',
    'test/sm120_l2_probe/ASSUMPTIONS.md',
    'test/sm120_l2_probe/VERIFICATION.md'
)

# ---- C: T2 inject toolchain artifacts ----
$artifacts = @(
    'platforms/cuda/hal/inject/Makefile',
    'platforms/cuda/hal/inject/make_msvc.bat',
    'platforms/cuda/hal/inject/inject.cu',
    'platforms/cuda/hal/inject/inject_120.cubin',
    'platforms/cuda/hal/inject/inject_120.asm',
    'platforms/cuda/hal/inject/inject_120_cc.asm',
    'platforms/cuda/hal/inject/inject_86.cubin',
    'platforms/cuda/hal/inject/inject_86.asm',
    'platforms/cuda/hal/inject/inject_86_cc.asm',
    'platforms/cuda/hal/inject/trap_handler_sm86.asm'
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
