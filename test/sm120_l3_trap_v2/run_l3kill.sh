#!/bin/bash
# run_l3kill.sh - reproducible sm120 trap kill (standalone prototype).
#
# Target:  applies the 0xd04 client share to the agent, patches the live
#          .text.traphandler (+0x390 -> cave), and runs a plain spin kernel.
# Agent:   builds the 0x83de session and arms the cave via shm; the target's
#          helper rewrites the cave to the "redirect TRAP_RETURN_PC to EXIT"
#          stub just before the agent sends 0317{0,0}.
#
# Expected: an 8 s victim exits in ~0.2-0.5 s with cudaSuccess (clean block
# kill), and the handler is restored afterwards.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SHM=${SHM:-/l3kill}
SPIN_MS=${SPIN_MS:-8000}
export CUXTRA_CUDA_LIB=${CUXTRA_CUDA_LIB:-/usr/lib/x86_64-linux-gnu/libcuda.so.1}
rm -f "/dev/shm${SHM}" /tmp/l3kill_target.log /tmp/l3kill_agent.log

L3X_POLICY=1 L3X_PATCH=1 L3X_ARM_PATCH=1 timeout $((SPIN_MS / 1000 + 40)) "$HERE/bin/l3x" \
    --role target --shm "$SHM" --spin-ms "$SPIN_MS" --blocks 1 > /tmp/l3kill_target.log 2>&1 &
TP=$!
sleep 1
L3X_POLICY=1 L3X_PATCH_DEVFLAG=1 L3X_PATCH_PARK=1 timeout $((SPIN_MS / 1000 + 40)) "$HERE/bin/l3x" \
    --role agent --shm "$SHM" > /tmp/l3kill_agent.log 2>&1
wait $TP

echo "=== target ==="; grep -E 'patch handler|helper|launching|victim|signature|restored' /tmp/l3kill_target.log
echo "=== agent ==="; grep -E '\[agent\]|\[pol\]|0317' /tmp/l3kill_agent.log | head -8
rm -f "/dev/shm${SHM}"
