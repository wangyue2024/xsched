#!/bin/bash
# repro_ab.sh - rebuild the upstream XSched CUDA L3 benchmark from the local
# source snapshot and run level 1/2/3 (TRAP) + TSG.
#
# Usage:  ./repro_ab.sh [sm_86|sm_80] [/root/xsched-up]
#   sm_86  -> RTX 3090 (driver 570)  : fast L3 expected (p50 ~230 us with shim)
#   sm_80  -> A100 (driver 550)      : shim may crash in cuXtraKernelGetFunction;
#                                     see UPSTREAM_INFO.md for the no-shim slow path
#
# Requires: CUDA toolkit (nvcc/g++/cmake), nvidia driver, root or writable /root.
set -e
ARCH=${1:-sm_86}
ROOT=${2:-/root/xsched-up}
HERE=$(cd "$(dirname "$0")" && pwd)
TAR="$HERE/xsched-ab-a100-sm80.tar.gz"
PARENT=$(dirname "$ROOT")

[ -f "$TAR" ] || { echo "missing $TAR"; exit 1; }
export PATH=/usr/local/cuda/bin:$PATH

echo "== extract $TAR -> $PARENT =="
rm -rf "$ROOT"
tar xzf "$TAR" -C "$PARENT"

cd "$ROOT"
echo "== configure ($ARCH) =="
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ROOT/output" \
  -DSHIM_SOFTLINK=ON -DBUILD_TEST=ON -DBUILD_SERVICE=OFF -DPLATFORM_CUDA=ON \
  -DCMAKE_CUDA_STANDARD=17 -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=g++ \
  -DCMAKE_CUDA_FLAGS="-arch=$ARCH -I/usr/local/cuda/include" \
  -DCMAKE_CXX_FLAGS="-I/usr/local/cuda/include" \
  -DCUDA_INCLUDE_DIRS=/usr/local/cuda/include \
  -DCUDA_LIBRARIES=/usr/local/cuda/lib64/libcudart.so

echo "== build =="
cmake --build build --target install -- -j"$(nproc)"

export LD_LIBRARY_PATH="$ROOT/output/lib"
echo "== level 1 =="
timeout 180 ./output/bin/cuda/level 1 || echo "level1 rc=$?"
echo "== level 3 (TRAP) =="
timeout 180 ./output/bin/cuda/level 3 || echo "level3 rc=$?"
echo "== level 2 =="
timeout 180 ./output/bin/cuda/level 2 || echo "level2 rc=$?"
echo "== level 3 (TSG) =="
XSCHED_CUDA_LV3_IMPL=TSG timeout 180 ./output/bin/cuda/level 3 || echo "tsg rc=$?"

echo "== done; logs are in $ROOT/output and above =="
