#!/bin/bash
# run_l3x.sh - cross-process suspend experiment (target + agent).
#   ./run_l3x.sh [tag] [extra l3x args...]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TAG=${1:-x}; shift || true
OUT=$HERE/evidence/live
SHM=/sm120_l3_$TAG
export CUXTRA_CUDA_LIB=${CUXTRA_CUDA_LIB:-/usr/lib/x86_64-linux-gnu/libcuda.so.1}
rm -f "/dev/shm$SHM"

# "both" forks internally: the agent is the parent of the target so that
# PTRACE_ATTACH is permitted under yama ptrace_scope=1.
"$HERE/bin/l3x" --role both --shm "$SHM" --spin-ms 60000 --blocks 8 "$@" \
    > "$OUT/exp_l3x_${TAG}.log" 2>&1
echo "[l3x] exit=$?"
rm -f "/dev/shm$SHM"
cat "$OUT/exp_l3x_${TAG}.log"
