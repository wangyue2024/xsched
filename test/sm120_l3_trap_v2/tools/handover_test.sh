#!/bin/bash
# handover_test.sh [freeze_secs] - does freezing the GPU hog let a second kernel through?
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
F=${1:-0}
rm -f /dev/shm/ho_a /dev/shm/ho_b /tmp/ho_tb
L3_ALLOW_PTRACE=1 L3_BLOCKS=20000 L3_THREADS=256 "$HERE/bin/shm_spin" /ho_a 16 >/dev/null 2>&1 &
PA=$!
for i in $(seq 1 200); do [ -e /dev/shm/ho_a ] && break; sleep 0.05; done
sleep 3
T0=$(date +%s.%N)
( L3_ALLOW_PTRACE=1 L3_BLOCKS=1 L3_THREADS=32 "$HERE/bin/shm_spin" /ho_b 0 >/dev/null 2>&1
  date +%s.%N > /tmp/ho_tb ) &
PB=$!
if [ "$F" != "0" ]; then
    sleep 0.5
    "$HERE/bin/l3mi" --pid $PA --cycles 1 --hold-ms $((F*1000)) --gap-ms 0 > /tmp/ho_agent.log 2>&1
fi
for i in $(seq 1 900); do [ -s /tmp/ho_tb ] && break; sleep 0.05; done
TB=$(cat /tmp/ho_tb 2>/dev/null || echo "")
kill -9 $PA $PB 2>/dev/null
rm -f /dev/shm/ho_a /dev/shm/ho_b /tmp/ho_tb
if [ -z "$TB" ]; then echo "[freeze=${F}s] B did NOT complete within 45 s"; exit 1; fi
python3 -c "print('[freeze=${F}s] B completion = %.2f s'%($TB - $T0))"
