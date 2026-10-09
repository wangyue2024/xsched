#!/usr/bin/env bash
# =====================================================================
#  build.sh - T7 integration test build (sm120 / Linux)
#
#  Produces work/app_l2 linked against:
#    - output/lib/libshimcuda.so  (the XSched shim)
#    - cudart (CUDA runtime)
#
#  Prerequisites:
#    - XSched built & installed (output/lib/libshimcuda.so)
#    - CUDA toolkit (nvcc + cudart)
#
#  Notes:
#    - nvcc 12.x defaults to an older virtual arch; the driver JITs the
#      embedded PTX for sm_120 at load time (same as the Windows run).
#    - host compiler: CUDA < 12.8 does not accept gcc >= 14; set
#      NVCC_CCBIN=/usr/bin/g++-13 if the default host g++ is too new.
#
#  Usage:
#    ./build.sh
# =====================================================================
set -eu

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$DIR/../.." && pwd)"
WORK="$DIR/work"
mkdir -p "$WORK"

SHIM="$REPO_ROOT/output/lib/libshimcuda.so"
if [ ! -f "$SHIM" ]; then
    echo "build.sh: FATAL: $SHIM missing - build & install XSched first (make cuda)" >&2
    exit 1
fi

NVCC="${NVCC:-nvcc}"
CXX="${CXX:-g++}"
CXX11="${NVCC_CCBIN:-}"

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
CCBIN_ARGS=()
if [ -n "$CXX11" ]; then
    CCBIN_ARGS=( -ccbin "$CXX11" )
else
    # pick a host compiler CUDA 12.x accepts if the default is too new
    HOST_CXX="$(g++ --version | head -1 | awk '{print $NF}' | cut -d. -f1)"
    if [ "${HOST_CXX:-0}" -ge 14 ] && [ -x /usr/bin/g++-13 ]; then
        echo "[build] default g++ $HOST_CXX too new for this nvcc; using g++-13"
        CCBIN_ARGS=( -ccbin /usr/bin/g++-13 )
    fi
fi

# On Linux the XSched management API lives in the HAL/preempt libraries
# (the shim only depends on them): CudaQueueCreate -> halcuda,
# XQueue* -> preempt.  Link all three explicitly.
#
# Compile and link are split on purpose: the shim was built with the
# system g++ (needs its newer libstdc++ / CXXABI), while nvcc needs an
# older host compiler for the device code.  nvcc -c with g++-13, then
# link the object with the system g++.
echo "[build] $NVCC -c app_l2.cu -> work/app_l2.o"
"$NVCC" -O2 -std=c++17 "${CCBIN_ARGS[@]}" -c "$DIR/app_l2.cu" -o "$WORK/app_l2.o" \
    -I "$REPO_ROOT/output/include" \
    -I "$REPO_ROOT/platforms/cuda/hal/include"

echo "[build] $CXX app_l2.o -> work/app_l2"
"$CXX" -O2 -std=c++17 -o "$WORK/app_l2" "$WORK/app_l2.o" \
    -L "$REPO_ROOT/output/lib" -lshimcuda -lhalcuda -lpreempt -lcudart \
    -Wl,-rpath,"$REPO_ROOT/output/lib"

echo "build.sh: done"
