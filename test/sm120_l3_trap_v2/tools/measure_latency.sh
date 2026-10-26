#!/bin/bash
# measure_latency.sh <blocks> <threads> [runs]
# Answers: how much of the suspend latency is gdb, and does it depend on size?
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
export PATH="$HERE/bin:$PATH"
export LD_LIBRARY_PATH="/tmp/xssvc/platforms/cuda:/tmp/xssvc/preempt:${LD_LIBRARY_PATH:-}"
BLK=${1:-8}; THR=${2:-32}; RUNS=${3:-3}
for i in $(seq 1 $RUNS); do
    pkill -9 -x shm_spin 2>/dev/null; sleep 0.4
    rm -f /dev/shm/lat_a
    L3_ALLOW_PTRACE=1 L3_BLOCKS=$BLK L3_THREADS=$THR L3_STREAMS=${L3_STREAMS:-1} "$HERE/bin/shm_spin" /lat_a 8 >/dev/null 2>&1 &
    P=$!
    for j in $(seq 1 200); do [ -e /dev/shm/lat_a ] && break; sleep 0.02; done
    sleep 1
    python3 "$HERE/tools/sample1ms.py" lat_a 4 > /tmp/lat_s.txt 2>&1 &
    S=$!
    "$HERE/bin/latency" --pid $P --hold-ms 800 > /tmp/lat_t.txt 2>&1
    wait $S 2>/dev/null
    python3 - "$BLK" "$THR" "$i" <<'PY'
import sys
blk, thr, run = sys.argv[1], sys.argv[2], sys.argv[3]
ts = {}
for line in open('/tmp/lat_t.txt'):
    p = line.split()
    if p[0] in ('PREPARE','SUSPEND','RESUME'):
        ts[p[0]] = (float(p[1]), float(p[2]))
s = []
for line in open('/tmp/lat_s.txt'):
    p = line.split()
    if len(p) == 2: s.append((float(p[0]), int(p[1])))
t0, t1 = ts['SUSPEND']; t2 = ts['RESUME'][0]
# the last instant the counter still advanced before t1
idx = None
for i in range(1, len(s)):
    if s[i][0] > t2: break
    if s[i][1] != s[i-1][1]: idx = i
freeze = s[idx][0] if idx is not None else float('nan')
# first instant the counter moves again after the resume request
resume = None
for i in range(1, len(s)):
    if s[i][0] > t2 and s[i][1] != s[i-1][1]:
        resume = s[i][0]; break
r = (resume - t2) * 1e3 if resume else float('nan')
print("run=%s blocks=%-6s threads=%-5s | gdb-attach=%.1f | request->frozen=%.1f | ack->frozen=%.1f | resume->running=%.1f ms"
      % (run, blk, thr, (t1-t0)*1e3, (freeze-t0)*1e3, (freeze-t1)*1e3, r))
PY
    pkill -9 -x shm_spin 2>/dev/null
done
