#!/usr/bin/env bash
# =====================================================================
#  run_mve.sh - T6 MVE one-click: build (if needed) + run + evidence
#               (Linux counterpart of run_mve.ps1)
#
#  Produces UTF-8 logs under evidence/ and verifies the pass/fail
#  counters: expected "RESULT: 0 failed check(s)" with 0 FAIL / 0 ERR.
#
#  Usage:
#    ./run_mve.sh [-q|--quick] [--skip-build] [--no-preflight] [--tag NAME]
# =====================================================================
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

QUICK=0
SKIP_BUILD=0
PREFLIGHT=1
TAG="linux"

while [ $# -gt 0 ]; do
    case "$1" in
        -q|--quick) QUICK=1 ;;
        --skip-build) SKIP_BUILD=1 ;;
        --no-preflight) PREFLIGHT=0 ;;
        --tag) shift; TAG="${1:-linux}" ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# find the real driver (must NOT be the shim when run under the shim tests)
find_real_cuda() {
    if [ -n "${XSCHED_CUDA_LIB:-}" ] && [ -e "${XSCHED_CUDA_LIB}" ]; then
        echo "$XSCHED_CUDA_LIB"; return 0
    fi
    local p
    p="$(ldconfig -p 2>/dev/null | awk '/libcuda\.so\.1 /{print $NF; exit}')"
    if [ -n "$p" ] && [ -e "$p" ]; then echo "$p"; return 0; fi
    for p in /usr/lib/x86_64-linux-gnu/libcuda.so.1 /usr/lib64/libcuda.so.1; do
        [ -e "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

REAL_CUDA="$(find_real_cuda)" || {
    echo "FATAL: real libcuda.so.1 not found; set XSCHED_CUDA_LIB" >&2
    exit 2
}
export CUXTRA_CUDA_LIB="$REAL_CUDA"

if [ $SKIP_BUILD -eq 0 ]; then
    "$DIR/build.sh" all
fi

[ -f "$DIR/mve_kernel.cubin" ] || { echo "FATAL: mve_kernel.cubin missing" >&2; exit 1; }
[ -x "$DIR/mve_main" ] || { echo "FATAL: mve_main missing (host stage)" >&2; exit 1; }

RUN_ARGS=( "$DIR/mve_kernel.cubin" )
[ $QUICK -eq 1 ] && RUN_ARGS+=( --quick )
[ $PREFLIGHT -eq 1 ] && RUN_ARGS+=( --preflight )

echo "[run] mve_main ${RUN_ARGS[*]}"
mkdir -p "$DIR/evidence"
LOG="$DIR/evidence/mve_${TAG}.log"
"$DIR/mve_main" "${RUN_ARGS[@]}" >"$LOG" 2>&1
EXIT=$?
cat "$LOG"
echo "[run] exit=$EXIT log=$LOG"

PASS=$(grep -c '\[PASS\]' "$LOG" || true)
FAIL=$(grep -c '\[FAIL\]' "$LOG" || true)
ERR=$(grep -c '\[ERR \]' "$LOG" || true)
WARN=$(grep -c 'preflight round(s) incomplete' "$LOG" || true)
echo "[run] PASS=$PASS FAIL=$FAIL ERR=$ERR"

if [ "$WARN" -gt 0 ]; then
    echo "[run] ENVIRONMENT NOT CLEAN: another heavy GPU consumer is running."
    echo "[run] Close it (game / heavy app) and re-run for a trustworthy verdict."
fi

if [ "$FAIL" -ne 0 ] || [ "$ERR" -ne 0 ] || [ "$EXIT" -ne 0 ]; then
    echo "[run] FAILED"
    exit 1
fi
echo "[run] all checks passed"
exit 0
