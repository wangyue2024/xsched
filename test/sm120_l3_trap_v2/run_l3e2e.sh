#!/bin/bash
# run_l3e2e.sh - the actual level-3 preemption scenario.
#
# Two independent CUDA processes contend for the GPU (30 blocks each = all SMs):
#   A = long low-priority kernel (25 s), B = short high-priority kernel (4 s).
# Phase 1 (baseline): both run, B's wall time is measured while A hogs the GPU.
# Phase 2 (L3):       A is frozen with the cuda-gdb agent right after B starts,
#                     so B gets the whole GPU; A is resumed afterwards and must
#                     still finish correctly.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); OUT=$HERE/evidence/live
BLK=60
THR=1024
run_pair() {   # $1=tag  $2=freeze(A|none)
    local tag=$1 mode=$2
    rm -f "/dev/shm/sm120_l3_A_$tag" "/dev/shm/sm120_l3_B_$tag"
    L3_ALLOW_PTRACE=1 L3_BLOCKS=$BLK L3_THREADS=$THR "$HERE/bin/shm_spin" "/sm120_l3_A_$tag" 25 \
        > "$OUT/e2e_${tag}_A.log" 2>&1 &
    local PA=$!
    for i in $(seq 1 200); do [ -e "/dev/shm/sm120_l3_A_$tag" ] && break; sleep 0.05; done
    sleep 2
    local t0=$(date +%s.%N)
    L3_ALLOW_PTRACE=1 L3_BLOCKS=$BLK L3_THREADS=$THR "$HERE/bin/shm_spin" "/sm120_l3_B_$tag" 4 \
        > "$OUT/e2e_${tag}_B.log" 2>&1 &
    local PB=$!
    if [ "$mode" = "freeze" ]; then
        sleep 0.4
        "$HERE/bin/l3mi" --pid $PA --cycles 1 --hold-ms 12000 --gap-ms 0 \
            > "$OUT/e2e_${tag}_agent.log" 2>&1 &
        local PG=$!
    fi
    wait $PB 2>/dev/null
    local t1=$(date +%s.%N)
    echo "[$tag] B (4 s kernel) wall time = $(echo "$t1 - $t0" | bc) s"
    [ "$mode" = "freeze" ] && { wait $PG 2>/dev/null; }
    wait $PA 2>/dev/null
    rm -f "/dev/shm/sm120_l3_A_$tag" "/dev/shm/sm120_l3_B_$tag"
}
rm -f /dev/shm/l3poc_req
echo "=== phase 1: baseline (A and B contend) ==="
run_pair base none
echo "=== phase 2: L3 (A frozen while B runs) ==="
run_pair l3 freeze
echo "--- A kernel result ---"; grep -h "kernel done" "$OUT"/e2e_*_A.log
