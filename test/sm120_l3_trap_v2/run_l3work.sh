#!/bin/bash
# run_l3work.sh - interrupt a running kernel mid-flight and prove the result is
# still bit-exact (sum over a fixed iteration count is deterministic).
set -u
HERE=$(cd "$(dirname "$0")" && pwd); OUT=$HERE/evidence/live
N=${1:-1500000000}; HOLD=${2:-2000}
SHM=/sm120_l3work
rm -f "/dev/shm$SHM"
L3_ALLOW_PTRACE=1 "$HERE/bin/l3work" "$SHM" "$N" > "$OUT/l3work_app.log" 2>&1 &
APP=$!
for i in $(seq 1 200); do [ -e "/dev/shm$SHM" ] && break; sleep 0.05; done
python3 "$HERE/tools/watch_prog.py" "$SHM" 30 > "$OUT/l3work_watch.log" 2>&1 &
W=$!
sleep 1
"$HERE/bin/l3mi" --pid $APP --cycles 1 --hold-ms "$HOLD" --gap-ms 0 > "$OUT/l3work_agent.log" 2>&1
wait $APP; RC=$?
sleep 1; kill $W 2>/dev/null; wait $W 2>/dev/null
rm -f "/dev/shm$SHM"
echo "--- agent ---"; grep -E "FREEZE|RESUME" "$OUT/l3work_agent.log"
echo "--- app ---";  cat "$OUT/l3work_app.log"
echo "--- progress samples during/after the freeze (5 ms) ---"
python3 - <<'PY'
import sys
v=[]
for l in open('/run/media/wy/Share/code/xsched/Level3forLinux/xsched/test/sm120_l3_trap_v2/evidence/live/l3work_watch.log'):
    p=l.split()
    if len(p)==2: v.append((float(p[0]),int(p[1])))
z=0
for i in range(1,len(v)):
    if v[i][1]==v[i-1][1]: z+=1
    else:
        if z>=10: print("  progress frozen ~%.2f s at %d"%(z*0.005,v[i-1][1]))
        z=0
print("  samples=%d  final progress=%d"%(len(v), v[-1][1] if v else -1))
PY
echo "exit code (0 = checksum correct) = $RC"
