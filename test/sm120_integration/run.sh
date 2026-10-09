#!/usr/bin/env bash
# =====================================================================
#  run.sh - T7 integration test runner (sm120 / Linux, non-invasive)
#
#  Linux counterpart of run.ps1.  Interception layout:
#    LD_LIBRARY_PATH = a private dir holding "libcuda.so.1 -> the shim"
#      (the Linux equivalent of the Windows per-exe nvcuda.dll proxy:
#       cudart dlopens "libcuda.so.1" BY NAME, so the shim must own that
#       name on the search path; LD_PRELOAD alone intercepts only calls
#       made by the executable, not cudart's dlopen).
#    XSCHED_CUDA_LIB / CUXTRA_CUDA_LIB = the REAL libcuda.so.1, otherwise
#      the HAL would resolve its own libcuda.so.1 lookup to the shim.
#    XSCHED_SCHEDULER = APP
#
#  The link farm is built under TMPDIR because the checkout may live on
#  a filesystem without symlink support (e.g. exFAT); "make cuda" only
#  creates the libcuda.so.1 softlink in output/lib on normal filesystems
#  (SHIM_SOFTLINK=ON), in which case LD_LIBRARY_PATH could point straight
#  at output/lib instead.
#
#  Usage:
#    ./run.sh [--skip-build] [--tag NAME]
# =====================================================================
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"
REPO_ROOT="$(cd "$DIR/../.." && pwd)"
SHIM="$REPO_ROOT/output/lib/libshimcuda.so"

SKIP_BUILD=0
TAG="linux"
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1 ;;
        --tag) shift; TAG="${1:-linux}" ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

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
    echo "FATAL: real libcuda.so.1 not found; set XSCHED_CUDA_LIB" >&2; exit 2; }

if [ $SKIP_BUILD -eq 0 ]; then
    "$DIR/build.sh"
fi
[ -x "$DIR/work/app_l2" ] || { echo "FATAL: work/app_l2 missing (run build.sh)" >&2; exit 1; }

# link farm: libcuda.so.1 -> the shim (falls back to a copy)
SHIM_DIR="${XSCHED_SHIM_DIR:-${TMPDIR:-/tmp}/xsched-shim}"
mkdir -p "$SHIM_DIR"
if ! ln -sf "$SHIM" "$SHIM_DIR/libcuda.so.1" 2>/dev/null; then
    cp -f "$SHIM" "$SHIM_DIR/libcuda.so.1"
fi
echo "[setup] libcuda.so.1 -> $SHIM (in $SHIM_DIR)"
export XSCHED_CUDA_LIB="$REAL_CUDA"
export CUXTRA_CUDA_LIB="$REAL_CUDA"
export XSCHED_SCHEDULER="APP"
export XLOG_LEVEL="INFO"          # quiet the per-call DEBG logging of the shim
unset XSCHED_AUTO_XQUEUE XSCHED_POLICY 2>/dev/null || true
export LD_LIBRARY_PATH="$SHIM_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p "$DIR/evidence"
LOG="$DIR/evidence/app_l2_${TAG}.log"

echo "[run] app_l2 (full integration suite)"
"$DIR/work/app_l2" >"$LOG" 2>&1
EXIT=$?
cat "$LOG"

PASS=$(grep -c '\[PASS\]' "$LOG" || true)
FAIL=$(grep -c '\[FAIL\]' "$LOG" || true)
ERR=$(grep -c '\[ERR \]' "$LOG" || true)
echo "[run] exit=$EXIT PASS=$PASS FAIL=$FAIL ERR=$ERR  log=$LOG"

if [ "$FAIL" -ne 0 ] || [ "$ERR" -ne 0 ] || [ "$EXIT" -ne 0 ]; then
    echo "[run] FAILED"
    exit 1
fi
echo "[run] all checks passed"
exit 0
