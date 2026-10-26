#!/bin/bash
# run_l3policy.sh - reproduce the cuda-gdb client-share handshake and try to
# hold-suspend a running kernel entirely through RM (no cuda-gdb).
#
#   ./run_l3policy.sh              # grant + hold suspend sweep (may kill context)
#   L3X_PRIME_ONLY=1 ./run_l3policy.sh   # only the 0317{0,0} polling stop
#   L3X_TGTDBG=1 ./run_l3policy.sh       # set target-side cudbg* flags first
#
# Topology (agent must initialise CUDA after the target publishes its pid):
#   target: creates the client, applies 0xd04 inherited share policy naming the
#           agent client, creates the context/channels, launches a spin kernel
#   agent:  own libcuda init, allocates class 0x83de with
#           {hAppClient=target client, hClass3dObject=target KGO}, then
#           drives 031f/0307/032a + 0317.  Evidence: FINDINGS_ZH.md v1.4.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SHM=${SHM:-/l3pol}
SPIN_MS=${SPIN_MS:-40000}
rm -f "/dev/shm${SHM}" /tmp/l3pol_target.log /tmp/l3pol_agent.log

L3X_POLICY=1 timeout $((SPIN_MS / 1000 + 50)) "$HERE/bin/l3x" \
    --role target --shm "$SHM" --spin-ms "$SPIN_MS" --blocks 1 \
    > /tmp/l3pol_target.log 2>&1 &
TP=$!
sleep 1
L3X_POLICY=1 timeout $((SPIN_MS / 1000 + 50)) "$HERE/bin/l3x" \
    --role agent --shm "$SHM" > /tmp/l3pol_agent.log 2>&1
wait $TP

echo "=== target ==="; cat /tmp/l3pol_target.log
echo "=== agent ==="; grep -E '\[agent\]|\[pol\]' /tmp/l3pol_agent.log
rm -f "/dev/shm${SHM}"
