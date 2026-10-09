#!/usr/bin/env bash
# =====================================================================
#  run_tests.sh - XSched local test runner (Linux)
#
#  Linux counterpart of run_tests.ps1:
#    build test_cuda_driver (driver API, linked against the shim) and
#    run it under several scheduler/policy configurations.
#
#  Interception model on Linux (vs. DLL proxying on Windows):
#    A private dir with "libcuda.so.1 -> the shim" is prepended to
#    LD_LIBRARY_PATH.  This is required because cudart dlopens
#    "libcuda.so.1" by name: LD_PRELOAD only intercepts symbols the
#    executable itself references, so a cudart-based workload (PyTorch)
#    would bypass a preloaded shim.  The shim loads the real driver via
#    XSCHED_CUDA_LIB / CUXTRA_CUDA_LIB - they MUST point at the real
#    libcuda.so.1, never at the shim (the shim dir is on the search path,
#    so an unset XSCHED_CUDA_LIB would resolve to the shim itself).
#
#  Usage:
#    ./run_tests.sh [all|driver|pytorch|hpf|cfs|global|l2]
# =====================================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LIB_DIR="$REPO_ROOT/output/lib"
SHIM="$LIB_DIR/libshimcuda.so"
TEST_BIN="$SCRIPT_DIR/test_cuda_driver"

TARGET="${1:-all}"

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
    echo "FATAL: real libcuda.so.1 not found; set XSCHED_CUDA_LIB to the driver library" >&2
    exit 2
}

if [ ! -f "$SHIM" ]; then
    echo "FATAL: $SHIM missing - build XSched first: make cuda" >&2
    exit 2
fi

# ---- non-invasive local test environment (mirrors run_tests.ps1) ----
SHIM_DIR="${XSCHED_SHIM_DIR:-${TMPDIR:-/tmp}/xsched-shim}"
mkdir -p "$SHIM_DIR"
if ! ln -sf "$SHIM" "$SHIM_DIR/libcuda.so.1" 2>/dev/null; then
    cp -f "$SHIM" "$SHIM_DIR/libcuda.so.1"
fi

export XSCHED_CUDA_LIB="$REAL_CUDA"
export CUXTRA_CUDA_LIB="$REAL_CUDA"
export XSCHED_AUTO_XQUEUE="ON"
export XLOG_LEVEL="${XLOG_LEVEL:-INFO}"     # quiet the per-call DEBG logging
export LD_LIBRARY_PATH="$SHIM_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

build_driver_test() {
    echo ">>> [build] test_cuda_driver (g++)"
    g++ -O2 -std=c++17 \
        -I "$REPO_ROOT/include" \
        -I "$REPO_ROOT/platforms/cuda/hal/include" \
        -o "$TEST_BIN" "$SCRIPT_DIR/test_cuda_driver.cpp" \
        -L "$LIB_DIR" -lshimcuda -Wl,-rpath,"$LIB_DIR"
}

run_driver_test() {
    local policy="$1"
    echo
    echo ">>> [Test: CUDA Driver API (Policy: ${policy})]"
    if [ "$policy" = "APP" ]; then
        export XSCHED_SCHEDULER="APP"
        unset XSCHED_POLICY
    else
        export XSCHED_POLICY="$policy"
        unset XSCHED_SCHEDULER
    fi
    unset XSCHED_AUTO_XQUEUE_LEVEL
    ( cd "$SCRIPT_DIR" && "$TEST_BIN" )
    if [ $? -eq 0 ]; then
        echo ">>> [Driver Test PASSED!]"
    else
        echo ">>> [Driver Test FAILED!]"; return 1
    fi
}

run_driver_l2() {
    echo
    echo ">>> [Test: CUDA Driver API (Policy: HPF, auto-XQueue level 2)]"
    export XSCHED_POLICY="HPF"
    unset XSCHED_SCHEDULER
    export XSCHED_AUTO_XQUEUE_LEVEL=2
    ( cd "$SCRIPT_DIR" && "$TEST_BIN" )
    if [ $? -eq 0 ]; then
        echo ">>> [Driver L2 Test PASSED!]"
    else
        echo ">>> [Driver L2 Test FAILED!]"; return 1
    fi
}

run_pytorch_test() {
    local policy="$1"
    echo
    echo ">>> [Test: PyTorch Workload (Policy: ${policy})]"
    if ! python3 -c "import torch" 2>/dev/null; then
        echo ">>> [PyTorch not installed - SKIPPED]"
        return 0
    fi
    export XSCHED_POLICY="$policy"
    unset XSCHED_SCHEDULER
    unset XSCHED_AUTO_XQUEUE_LEVEL
    python3 "$SCRIPT_DIR/test_pytorch.py"
    if [ $? -eq 0 ]; then
        echo ">>> [PyTorch Test PASSED!]"
    else
        echo ">>> [PyTorch Test FAILED!]"; return 1
    fi
}

run_global_test() {
    echo
    echo ">>> [Test: CUDA Driver API (Scheduler: GLB)]"
    export XSCHED_SCHEDULER="GLB"
    unset XSCHED_POLICY
    unset XSCHED_AUTO_XQUEUE_LEVEL
    ( cd "$SCRIPT_DIR" && "$TEST_BIN" )
    if [ $? -eq 0 ]; then
        echo ">>> [Global Scheduler Test PASSED!]"
    else
        echo ">>> [Global Scheduler Test FAILED!]"; return 1
    fi
}

echo "=================================================="
echo "   XSched Local Test Suite (Linux / CUDA)         "
echo "=================================================="
echo "  real driver : $REAL_CUDA"
echo "  shim        : $SHIM"
echo "=================================================="

FAILED=0
case "$TARGET" in
    "driver")  build_driver_test && run_driver_test "HPF" || FAILED=1 ;;
    "pytorch") build_driver_test && run_pytorch_test "HPF" || FAILED=1 ;;
    "hpf")     build_driver_test && run_driver_test "HPF" && run_pytorch_test "HPF" || FAILED=1 ;;
    "cfs")     build_driver_test && run_driver_test "CFS" && run_pytorch_test "CFS" || FAILED=1 ;;
    "global")  build_driver_test && run_global_test || FAILED=1 ;;
    "l2")      build_driver_test && run_driver_l2 || FAILED=1 ;;
    "all")
        build_driver_test \
            && run_driver_test "APP" \
            && run_driver_test "HPF" \
            && run_driver_test "CFS" \
            && run_global_test \
            && run_pytorch_test "HPF" \
            || FAILED=1
        ;;
    *) echo "unknown target: $TARGET"; exit 2 ;;
esac

echo
echo "=================================================="
if [ $FAILED -eq 0 ]; then
    echo "            Test Execution Finished (OK)          "
    echo "=================================================="
    exit 0
else
    echo "            Test Execution FAILED                 "
    echo "=================================================="
    exit 1
fi
