#!/usr/bin/env bash
# =====================================================================
#  run_all_t3t4.sh - T3/T4 one-click reproduction (Linux)
#
#  Linux counterpart of run_all_t3t4.ps1 -- same steps, same asserts:
#    1. review x3               (sm120 / sm86-new / sm86-official census)
#    2. golden regression       (must print 9/9 PASS, exit 0)
#    3. generate sm120          (guardian 50 + resume 32, exit 0)
#    4. nvdisasm independent re-verification of patched_120.cubin
#       (LDC @0x170 series present, BPT.TRAP == 18)
#    5. reproducibility         (run2 outputs bit-identical)
#
#  Prerequisites:
#    * CUDA 12.9 toolkit on PATH (nvcc/cuobjdump/nvdisasm)
#      NOTE (Ubuntu >= 25.10 / glibc >= 2.41): CUDA 12.9's math_functions.h
#      conflicts with the new C23 math declarations (cospi/sinpi/rsqrt).
#      Build the cubins with:
#        export NVCC_PREPEND_FLAGS='-ccbin g++-13 -Xcompiler -U_GNU_SOURCE'
#      (see test/SM120_L2_LINUX.md for the analysis).
#    * inject_120.cubin / inject_86.cubin built via
#      platforms/cuda/hal/inject/Makefile:  make ARCH=120 bin
#                                           make ARCH=86  bin
#
#  Usage:
#    ./run_all_t3t4.sh
# =====================================================================
set -u

GEN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$GEN_DIR/../.." && pwd)"
INJ="$REPO_ROOT/platforms/cuda/hal/inject"
TOOL="$REPO_ROOT/tools/instrument/extract_sass.py"
EV="$GEN_DIR/evidence"
PY="${PYTHON:-python3}"

failed=0
step() { printf '\n=== %s ===\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1"; failed=$((failed + 1)); }

# ---- 0. prerequisites ----
step "0. prerequisites"
for f in "$INJ/inject_120.cubin" "$INJ/inject_86.cubin" "$TOOL"; do
    if [ ! -f "$f" ]; then fail "missing $f"; echo "inputs incomplete"; exit 1; fi
done
command -v nvcc >/dev/null    || { fail "nvcc not on PATH"; exit 1; }
command -v nvdisasm >/dev/null || { fail "nvdisasm not on PATH"; exit 1; }
echo "inputs OK (nvcc: $(nvcc --version | grep -o 'release [0-9.]*'))"

# ---- 1. review x3 ----
step "1. review (T3 census)"
"$PY" "$TOOL" review --asm "$INJ/inject_120.asm"  --out "$EV/review_inject_120.md"            >/dev/null
"$PY" "$TOOL" review --asm "$INJ/inject_86.asm"   --out "$EV/review_inject_86_new.md"        >/dev/null
"$PY" "$TOOL" review --asm "$INJ/inject_sm86.asm" --out "$EV/review_inject_sm86_official.md" >/dev/null
if grep -q 'check_preempt | 64 | 20 | 3 | 3' "$EV/review_inject_120.md"; then
    echo "sm120 census OK (check_preempt: 64 instrs / BPT 4)"
else
    fail "sm120 census mismatch (check_preempt row)"
fi

# ---- 2. golden regression ----
step "2. golden regression (sm86, expect 9/9 PASS)"
gout="$("$PY" "$TOOL" golden --cubin-86 "$INJ/inject_86.cubin" --source "$INJ/inject.cu" \
    --official-asm "$INJ/inject_sm86.asm" --official-cpp "$REPO_ROOT/platforms/cuda/hal/src/arch/sm86.cpp" \
    --out-cubin "$EV/patched_86.cubin" --report "$EV/golden_report.json" 2>&1)"
rc=$?
echo "$gout"
pass_count=$(printf '%s\n' "$gout" | grep -c '^\[PASS\]')
[ "$pass_count" -eq 9 ] || fail "golden: $pass_count/9 PASS"
[ "$rc" -eq 0 ] || fail "golden exit=$rc"
[ "$rc" -eq 0 ] && [ "$pass_count" -eq 9 ] && echo "golden 9/9 PASS, exit 0"

# ---- 3. generate sm120 ----
step "3. generate sm120 (guardian 50 + resume 32)"
nout="$("$PY" "$TOOL" generate --cubin "$INJ/inject_120.cubin" --source "$INJ/inject.cu" \
    --offset-map sm120 --nvcc "nvcc 12.9.41 / ptxas 12.9.41" \
    --out-cubin "$EV/patched_120.cubin" --out-cpp "$EV/preview_sm120.cpp" --report "$EV/gen_sm120_report.json" 2>&1)"
rc=$?
echo "$nout"
printf '%s\n' "$nout" | grep -q 'check_preempt\] 4 placeholders -> 4 replacements; trim=guardian (-14); final 50' \
    || fail "guardian trim/count mismatch"
printf '%s\n' "$nout" | grep -q 'restore_exec\] 4 placeholders -> 4 replacements; trim=resume (-0); final 32' \
    || fail "resume mismatch"
[ "$rc" -eq 0 ] || fail "generate exit=$rc"
[ "$rc" -eq 0 ] && echo "generate OK"

# ---- 4. independent nvdisasm re-verification ----
step "4. nvdisasm independent re-verification (patched_120.cubin)"
nvdisasm -c "$EV/patched_120.cubin" > "$EV/patched_120_cc.asm"
if [ $? -ne 0 ]; then
    fail "nvdisasm re-verify (rc)"
else
    bpt=$(grep -c 'BPT.TRAP' "$EV/patched_120_cc.asm")
    [ "$bpt" -eq 18 ] || fail "BPT residue $bpt != 18"
    missing=""
    for probe in 'LDC R4, c[0x0][0x170]' 'LDC R5, c[0x0][0x174]' \
                 'LDC R6, c[0x0][0x180]' 'LDC R7, c[0x0][0x184]' \
                 'LDC R20, c[0x0][0x178]' 'LDC R21, c[0x0][0x17c]'; do
        grep -qF "$probe" "$EV/patched_120_cc.asm" || missing="$missing, $probe"
    done
    [ -z "$missing" ] || fail "missing window LDC:$missing"
    [ "$bpt" -eq 18 ] && [ -z "$missing" ] && \
        echo "nvdisasm reverify OK: rc=0, BPT=18, all six window LDCs present"
fi

# ---- 5. reproducibility (run2, bit-identical) ----
step "5. reproducibility (second run, run2/)"
mkdir -p "$EV/run2"
"$PY" "$TOOL" generate --cubin "$INJ/inject_120.cubin" --source "$INJ/inject.cu" \
    --offset-map sm120 --nvcc "nvcc 12.9.41 / ptxas 12.9.41" \
    --out-cubin "$EV/run2/patched_120_run2.cubin" --out-cpp "$EV/run2/preview_sm120_run2.cpp" \
    --report "$EV/run2/gen_sm120_report_run2.json" >/dev/null
"$PY" "$TOOL" golden --cubin-86 "$INJ/inject_86.cubin" --source "$INJ/inject.cu" \
    --official-asm "$INJ/inject_sm86.asm" --official-cpp "$REPO_ROOT/platforms/cuda/hal/src/arch/sm86.cpp" \
    --out-cubin "$EV/run2/patched_86_run2.cubin" --report "$EV/run2/golden_report_run2.json" >/dev/null

for pair in "patched_120.cubin run2/patched_120_run2.cubin" \
            "preview_sm120.cpp run2/preview_sm120_run2.cpp" \
            "gen_sm120_report.json run2/gen_sm120_report_run2.json" \
            "golden_report.json run2/golden_report_run2.json" \
            "patched_86.cubin run2/patched_86_run2.cubin"; do
    set -- $pair
    a="$(sha256sum "$EV/$1" | cut -d' ' -f1)"
    b="$(sha256sum "$EV/$2" | cut -d' ' -f1)"
    if [ "$a" = "$b" ]; then echo "MATCH $1"; else fail "not reproducible: $1"; fi
done

# ---- summary ----
step "summary"
if [ "$failed" -eq 0 ]; then
    echo "ALL T3/T4 CHECKS PASSED (9/9 golden, 50+32 generate, nvdisasm OK, reproducible)"
    exit 0
fi
echo "$failed check(s) FAILED"
exit 1
