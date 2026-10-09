#!/usr/bin/env bash
# XSched CUDA shim test runner (Linux). Mirrors run_tests.ps1: builds the
# suites, then runs them in XSCHED_AUTO_XQUEUE ON/OFF modes against the shim.
#
# Usage:    ./run_tests.sh [--bench] [--run-only]
# Requires: XSched built with `make cuda`, the NVIDIA driver (libcuda.so.1),
#           g++ with C++20.

set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
cd "$script_dir"

RUN_BENCH=0
RUN_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --bench)    RUN_BENCH=1 ;;
        --run-only) RUN_ONLY=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

# XSCHED_CUDA_LIB / CUXTRA_CUDA_LIB are left unset: both XSched and cuxtra
# default to libcuda.so.1. Override from the environment if the driver lives
# elsewhere (must be an absolute file path).

if [ -z "${XSCHED_SHIM_PATH:-}" ]; then
    for candidate in \
        "$repo_root/build/platforms/cuda/libshimcuda.so" \
        "$repo_root/output/lib/libshimcuda.so"; do
        if [ -f "$candidate" ]; then XSCHED_SHIM_PATH="$candidate"; break; fi
    done
fi
if [ -z "${XSCHED_SHIM_PATH:-}" ] || [ ! -f "$XSCHED_SHIM_PATH" ]; then
    echo "ERROR: shim library not found; build it first (make cuda) or set XSCHED_SHIM_PATH" >&2
    exit 1
fi
export XSCHED_SHIM_PATH
echo "Shim library:      $XSCHED_SHIM_PATH"
echo "XSCHED_CUDA_LIB:   ${XSCHED_CUDA_LIB:-(default: libcuda.so.1)}"

TESTS=(
    test_unit_registry
    test_driver_shim
    test_isolation
    test_multithread_stress
    test_destroy_inflight
    test_ptds_features
)

CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--O2 -std=c++20 -Wall}"
LDFLAGS="${LDFLAGS:--ldl -lpthread}"
PORTABILITY="-I$script_dir/compat -include portability.h"

if [ "$RUN_ONLY" -eq 0 ]; then
    echo ""
    echo "=== Building test binaries ==="
    for t in "${TESTS[@]}" test_perf_benchmark; do
        # shellcheck disable=SC2086
        $CXX $CXXFLAGS $PORTABILITY "$t.cpp" -o "$t" $LDFLAGS || exit 1
        echo "  built $t"
    done
fi

failed=0
total=0
for mode in ON OFF; do
    echo ""
    echo "================================================================"
    echo "  XSCHED_AUTO_XQUEUE = $mode"
    echo "================================================================"
    export XSCHED_AUTO_XQUEUE="$mode"
    for t in "${TESTS[@]}"; do
        echo ""
        echo "--- $t ---"
        total=$((total + 1))
        "./$t"
        rc=$?
        if [ $rc -ne 0 ]; then
            echo "!!! $t FAILED (exit $rc)"
            failed=$((failed + 1))
        fi
    done
done

if [ "$RUN_BENCH" -eq 1 ]; then
    echo ""
    echo "================================================================"
    echo "  Performance benchmark (XSCHED_AUTO_XQUEUE = ON)"
    echo "================================================================"
    export XSCHED_AUTO_XQUEUE=ON
    ./test_perf_benchmark || failed=$((failed + 1))
fi

echo ""
if [ $failed -eq 0 ]; then
    echo "ALL $total TEST RUNS PASSED"
else
    echo "$failed/$total test run(s) FAILED"
    exit 1
fi
