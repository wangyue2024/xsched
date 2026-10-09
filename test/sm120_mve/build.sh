#!/usr/bin/env bash
# =====================================================================
#  build.sh - T6 MVE build script (sm120 / Linux)
#
#  Linux counterpart of build.ps1.  Stages:
#    gen    : python3 gen_arrays.py          -> mve_arrays.h
#             (arrays sliced out of the reviewed sm120.cpp -- no drift)
#    device : nvcc -cubin dummy_kernel.cu    -> mve_kernel.cubin
#             requires a CUDA toolkit >= 12.8 for -arch=sm_120; if the
#             local nvcc is older, the checked-in cubin is reused
#    host   : g++ mve_main.cpp + libcuxtra_linux -> mve_main
#
#  Usage:
#    ./build.sh [gen|device|host|all]
# =====================================================================
set -eu

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$DIR/../.." && pwd)"
STAGE="${1:-all}"

NVCC="${NVCC:-nvcc}"
CXX="${CXX:-g++}"

# Ubuntu >= 25.10 (glibc >= 2.41) declares cospi/sinpi/rsqrt with
# noexcept(true), which clashes with CUDA <= 12.9's math_functions.h
# ("exception specification is incompatible").  The local CUDA headers
# must carry the small noexcept compat patch -- see
# test/SM120_L2_LINUX.md (CUDA 12.9 setup).  Check and warn early.
NVCC_REAL="$(readlink -f "$(command -v "$NVCC")" 2>/dev/null || true)"
CRT_MATH_H="${NVCC_REAL%/bin/*}/targets/x86_64-linux/include/crt/math_functions.h"
if [ -f "$CRT_MATH_H" ] && ! grep -q "__XGLIBC_NOEXCEPT_COMPAT" "$CRT_MATH_H"; then
    echo "[build] WARN: $CRT_MATH_H lacks the glibc>=2.41 noexcept compat patch;"
    echo "[build]       nvcc will fail with 'exception specification is incompatible'."
    echo "[build]       Fix per test/SM120_L2_LINUX.md (CUDA 12.9 setup)."
fi

# CUDA 12.x rejects host compilers newer than gcc 14; pick g++-13 when the
# system default is too new (override with NVCC_CCBIN).
CCBIN_ARGS=()
if [ -n "${NVCC_CCBIN:-}" ]; then
    CCBIN_ARGS=( -ccbin "$NVCC_CCBIN" )
else
    HOST_CXX_MAJOR="$(g++ --version | head -1 | awk '{print $NF}' | cut -d. -f1)"
    if [ "${HOST_CXX_MAJOR:-0}" -ge 15 ] && [ -x /usr/bin/g++-13 ]; then
        echo "[build] default g++ $HOST_CXX_MAJOR too new for this nvcc; using g++-13"
        CCBIN_ARGS=( -ccbin /usr/bin/g++-13 )
    fi
fi

nvcc_supports_sm120() {
    local list
    list="$($NVCC "${CCBIN_ARGS[@]}" --list-gpu-arch 2>/dev/null || true)"
    case "$list" in
        *compute_120*|*sm_120*) return 0 ;;
        *) return 1 ;;
    esac
}

if [ "$STAGE" = "gen" ] || [ "$STAGE" = "all" ]; then
    echo "[gen] gen_arrays.py -> mve_arrays.h (from sm120.cpp)"
    ( cd "$DIR" && python3 gen_arrays.py )
fi

if [ "$STAGE" = "device" ] || [ "$STAGE" = "all" ]; then
    if nvcc_supports_sm120; then
        echo "[device] $NVCC -cubin dummy_kernel.cu (sm_120, plain compile)"
        ( cd "$DIR" && "$NVCC" "${CCBIN_ARGS[@]}" -cubin dummy_kernel.cu -o mve_kernel.cubin -arch=sm_120 )
        echo "[device] cuobjdump -sass -> mve_kernel.sass.txt"
        ( cd "$DIR" && cuobjdump -sass mve_kernel.cubin > mve_kernel.sass.txt )
    elif [ -f "$DIR/mve_kernel.cubin" ]; then
        echo "[device] SKIPPED: local nvcc does not support sm_120"
        echo "[device] reusing checked-in mve_kernel.cubin"
    else
        echo "[device] FATAL: nvcc < 12.8 and no checked-in mve_kernel.cubin" >&2
        exit 1
    fi
fi

if [ "$STAGE" = "host" ] || [ "$STAGE" = "all" ]; then
    if [ ! -f "$DIR/mve_arrays.h" ]; then
        echo "[host] FATAL: mve_arrays.h missing (run: ./build.sh gen)" >&2
        exit 1
    fi
    echo "[host] $CXX mve_main.cpp (link libcuxtra_linux + dl)"
    CUXTRA_LIB="$REPO_ROOT/3rdparty/cuxtra/lib/libcuxtra_linux_$(uname -m).a"
    if [ ! -f "$CUXTRA_LIB" ]; then
        echo "[host] FATAL: $CUXTRA_LIB not found (git submodule update --init)" >&2
        exit 1
    fi
    "$CXX" -O1 -std=c++17 \
        -I "$REPO_ROOT/3rdparty/cuxtra/include" \
        -I "$DIR" \
        "$DIR/mve_main.cpp" \
        "$CUXTRA_LIB" -ldl -lpthread \
        -o "$DIR/mve_main"
fi

echo "build.sh: done"
