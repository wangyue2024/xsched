#!/bin/bash
# run_l3trap.sh - prove that the *live* driver trap handler executes and that
# our SASS payload runs inside it.  This is the sm120 "trap injection" primitive.
#
# Topology (same as run_l3policy.sh):
#   target: 0xd04 client share -> agent, creates context, finds the live
#           .text.traphandler (arena base + 0x15f0f00), patches +0x390
#           ("MOV R3,RZ") with CALL.ABS into a cave at slot20+0xA00 that writes
#           0x5aa5 (sig) and the trap reason to host-mapped shm, then returns.
#   agent:  0x83de session bound to {target client, target KGO}, 031f/0307/032a,
#           0317{0,0}.  The trap fires, the cave runs, the kernel continues.
#
# Expected: target prints "signature: 0x5aa5/0 reason=0x1 (LIVE HANDLER EXECUTED)"
# and "victim finished: cudaSuccess" (context survives).
#
#   ./run_l3trap.sh            # default 15 s victim
#   SPIN_MS=30000 ./run_l3trap.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SHM=${SHM:-/l3trap}
SPIN_MS=${SPIN_MS:-15000}
export CUXTRA_CUDA_LIB=${CUXTRA_CUDA_LIB:-/usr/lib/x86_64-linux-gnu/libcuda.so.1}
rm -f "/dev/shm${SHM}" /tmp/l3trap_target.log /tmp/l3trap_agent.log

L3X_POLICY=1 L3X_PATCH=1 timeout $((SPIN_MS / 1000 + 50)) "$HERE/bin/l3x" \
    --role target --shm "$SHM" --spin-ms "$SPIN_MS" --blocks 1 \
    > /tmp/l3trap_target.log 2>&1 &
TP=$!
sleep 1
L3X_POLICY=1 L3X_PATCH=1 timeout $((SPIN_MS / 1000 + 50)) "$HERE/bin/l3x" \
    --role agent --shm "$SHM" > /tmp/l3trap_agent.log 2>&1
wait $TP

echo "=== target ==="; grep -E '\[tgt\]|\[target\] (pid|policy|compute|launching|victim|trap-handler)|restored' /tmp/l3trap_target.log
echo "=== agent ==="; grep -E '\[agent\]|\[pol\]' /tmp/l3trap_agent.log
rm -f "/dev/shm${SHM}"
