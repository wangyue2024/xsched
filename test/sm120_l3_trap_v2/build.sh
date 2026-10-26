#!/bin/bash
# Build the sm120 L3-trap v2 host probes.
#
#   ./build.sh                 # build everything in probe/
#   ./build.sh find_live_handler
#
# Host-only probes are built with g++-13 (gcc 15 is out of CUDA 12.9's range);
# probes containing device code are built with the local CUDA 12.9 nvcc.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
XSCHED=$(cd "$HERE/../.." && pwd)
CUDA=${CUDA:-/home/wy/cuda-12.9}
CXX=${CXX:-g++-13}
NVCC=${NVCC:-$CUDA/bin/nvcc}
CUXTRA_INC=$XSCHED/3rdparty/cuxtra/include
CUXTRA_LIB=$XSCHED/3rdparty/cuxtra/lib/libcuxtra_linux_x86_64.a

mkdir -p "$HERE/bin"
TARGETS=${*:-$(cd "$HERE/probe" && ls *.cpp *.cu *.c 2>/dev/null | sed 's/\.[^.]*$//' | sort -u)}

for t in $TARGETS; do
    if [ -f "$HERE/probe/$t.cpp" ]; then
        echo "== [host] $t"
        $CXX -O2 -std=c++17 -g "$HERE/probe/$t.cpp" -o "$HERE/bin/$t" \
            -I"$CUDA/include" -I"$CUXTRA_INC" -I"$XSCHED/include" -I"$XSCHED/platforms/cuda/hal/include" "$CUXTRA_LIB" -lcuda -ldl -lpthread -lrt
    elif [ -f "$HERE/probe/$t.c" ]; then
        echo "== [c]    $t"
        gcc -O2 -g -std=gnu11 "$HERE/probe/$t.c" -o "$HERE/bin/$t" -lrt
    elif [ -f "$HERE/probe/$t.cu" ]; then
        echo "== [cuda] $t"
        $NVCC -O2 -std=c++17 -g -arch=sm_120 -ccbin $CXX "$HERE/probe/$t.cu" -o "$HERE/bin/$t" \
            -I"$CUDA/include" -I"$CUXTRA_INC" -I"$XSCHED/include" -I"$XSCHED/platforms/cuda/hal/include" \
            "$CUXTRA_LIB" -lcuda -ldl -lpthread -lrt
    else
        echo "!! no source for $t" >&2
    fi
done
echo "built into $HERE/bin"
