# =====================================================================
#  run_all_t3t4.ps1 - T3/T4 one-click reproduction
#
#  Re-runs the complete T3/T4 pipeline and asserts every success
#  criterion:
#    1. review x3               (sm120 / sm86-new / sm86-official census)
#    2. golden regression       (must print 9/9 PASS, exit 0)
#    3. generate sm120          (guardian 50 + resume 32, exit 0)
#    4. nvdisasm independent re-verification of patched_120.cubin
#       (LDC @0x170 series present, BPT.TRAP == 18)
#    5. reproducibility         (run2 outputs bit-identical)
#
#  Prerequisites (see T3_REPORT.md §2):
#    * CUDA 12.9 toolkit on PATH (nvcc/cuobjdump/nvdisasm)
#    * inject_120.cubin / inject_86.cubin already built via
#      platforms/cuda/hal/inject/Makefile (make_msvc.bat ARCH=... bin dump cc)
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File run_all_t3t4.ps1
# =====================================================================
$ErrorActionPreference = 'Stop'
$GenDir = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $GenDir '..\..')).Path
$Inj = Join-Path $RepoRoot 'platforms\cuda\hal\inject'
$Tool = Join-Path $RepoRoot 'tools\instrument\extract_sass.py'
$Ev = Join-Path $GenDir 'evidence'
$failed = 0

function Step([string]$name) { Write-Host "`n=== $name ===" -ForegroundColor Cyan }
function Fail([string]$msg) { Write-Host "FAIL: $msg" -ForegroundColor Red; $script:failed++ }

# ---- 0. prerequisites ----
Step "0. prerequisites"
foreach ($f in @("$Inj\inject_120.cubin", "$Inj\inject_86.cubin", $Tool)) {
    if (-not (Test-Path -LiteralPath $f)) { Fail "missing $f"; exit 1 }
}
Write-Host "inputs OK"

# ---- 1. review x3 ----
Step "1. review (T3 census)"
python $Tool review --asm "$Inj\inject_120.asm"            --out "$Ev\review_inject_120.md"            | Out-Null
python $Tool review --asm "$Inj\inject_86.asm"             --out "$Ev\review_inject_86_new.md"        | Out-Null
python $Tool review --asm "$Inj\inject_sm86.asm"           --out "$Ev\review_inject_sm86_official.md" | Out-Null
$t120 = Get-Content "$Ev\review_inject_120.md" -Raw
if ($t120 -notmatch 'check_preempt \| 64 \| 20 \| 3 \| 3') { Fail "sm120 census mismatch (check_preempt row)" } else { Write-Host "sm120 census OK (check_preempt: 64 instrs / BPT 4)" }

# ---- 2. golden regression ----
Step "2. golden regression (sm86, expect 9/9 PASS)"
$gout = python $Tool golden --cubin-86 "$Inj\inject_86.cubin" --source "$Inj\inject.cu" `
    --official-asm "$Inj\inject_sm86.asm" --official-cpp "$RepoRoot\platforms\cuda\hal\src\arch\sm86.cpp" `
    --out-cubin "$Ev\patched_86.cubin" --report "$Ev\golden_report.json"
$gout | Write-Host
$passCount = ($gout | Select-String -Pattern '^\[PASS\]').Count
if ($passCount -ne 9) { Fail "golden: $passCount/9 PASS" }
if ($LASTEXITCODE -ne 0) { Fail "golden exit=$LASTEXITCODE" } else { Write-Host "golden 9/9 PASS, exit 0" }

# ---- 3. generate sm120 ----
Step "3. generate sm120 (guardian 50 + resume 32)"
$nout = python $Tool generate --cubin "$Inj\inject_120.cubin" --source "$Inj\inject.cu" `
    --offset-map sm120 --nvcc "nvcc 12.9.41 / ptxas 12.9.41" `
    --out-cubin "$Ev\patched_120.cubin" --out-cpp "$Ev\preview_sm120.cpp" --report "$Ev\gen_sm120_report.json"
$nout | Write-Host
if (($nout -join "`n") -notmatch 'check_preempt\] 4 placeholders -> 4 replacements; trim=guardian \(-14\); final 50') { Fail "guardian trim/-count mismatch" }
if (($nout -join "`n") -notmatch 'restore_exec\] 4 placeholders -> 4 replacements; trim=resume \(-0\); final 32') { Fail "resume mismatch" }
if ($LASTEXITCODE -ne 0) { Fail "generate exit=$LASTEXITCODE" } else { Write-Host "generate OK" }

# ---- 4. independent nvdisasm re-verification ----
Step "4. nvdisasm independent re-verification (patched_120.cubin)"
python -c @"
import subprocess, re, sys
out = subprocess.run(['nvdisasm','-c', r'$Ev\patched_120.cubin'], capture_output=True)
open(r'$Ev\patched_120_cc.asm','wb').write(out.stdout)
txt = out.stdout.decode('utf-8','replace')
assert out.returncode == 0, 'nvdisasm rc'
assert txt.count('BPT.TRAP') == 18, 'BPT residue %d != 18' % txt.count('BPT.TRAP')
for probe in ['LDC R4, c[0x0][0x170]', 'LDC R5, c[0x0][0x174]',
              'LDC R6, c[0x0][0x180]', 'LDC R7, c[0x0][0x184]',
              'LDC R20, c[0x0][0x178]', 'LDC R21, c[0x0][0x17c]']:
    assert probe in txt, 'missing: ' + probe
print('nvdisasm reverify OK: rc=0, BPT=18, all six window LDCs present')
"@
if ($LASTEXITCODE -ne 0) { Fail "nvdisasm re-verify" } 

# ---- 5. reproducibility (run2, bit-identical) ----
Step "5. reproducibility (second run, run2/)"
python $Tool generate --cubin "$Inj\inject_120.cubin" --source "$Inj\inject.cu" `
    --offset-map sm120 --nvcc "nvcc 12.9.41 / ptxas 12.9.41" `
    --out-cubin "$Ev\run2\patched_120_run2.cubin" --out-cpp "$Ev\run2\preview_sm120_run2.cpp" `
    --report "$Ev\run2\gen_sm120_report_run2.json" | Out-Null
python $Tool golden --cubin-86 "$Inj\inject_86.cubin" --source "$Inj\inject.cu" `
    --official-asm "$Inj\inject_sm86.asm" --official-cpp "$RepoRoot\platforms\cuda\hal\src\arch\sm86.cpp" `
    --out-cubin "$Ev\run2\patched_86_run2.cubin" --report "$Ev\run2\golden_report_run2.json" | Out-Null
$pairs = @(
    @('patched_120.cubin', 'run2\patched_120_run2.cubin'),
    @('preview_sm120.cpp', 'run2\preview_sm120_run2.cpp'),
    @('gen_sm120_report.json', 'run2\gen_sm120_report_run2.json'),
    @('golden_report.json', 'run2\golden_report_run2.json'),
    @('patched_86.cubin', 'run2\patched_86_run2.cubin')
)
foreach ($p in $pairs) {
    $a = (Get-FileHash -LiteralPath (Join-Path $Ev $p[0]) -Algorithm SHA256).Hash
    $b = (Get-FileHash -LiteralPath (Join-Path $Ev $p[1]) -Algorithm SHA256).Hash
    if ($a -ne $b) { Fail "not reproducible: $($p[0])" } else { Write-Host "MATCH $($p[0])" }
}

# ---- summary ----
Step "summary"
if ($failed -eq 0) {
    Write-Host "ALL T3/T4 CHECKS PASSED (9/9 golden, 50+32 generate, nvdisasm OK, reproducible)" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failed check(s) FAILED" -ForegroundColor Red
    exit 1
}
