#!/bin/bash
# run_l3mi.sh [seconds] [cycles] [hold_ms] [gap_ms]
# Level-3 interrupt via the cuda-gdb MI agent (probe/l3mi) - the target is an
# ordinary CUDA program, it only needs PR_SET_PTRACER (L3_ALLOW_PTRACE=1).
set -u
HERE=$(cd "$(dirname "$0")" && pwd); OUT=$HERE/evidence/live
SECS=${1:-25}; CYCLES=${2:-3}; HOLD=${3:-1000}; GAP=${4:-1500}
SHM=/sm120_l3mi; TAG=l3mi
rm -f "/dev/shm$SHM"
L3_ALLOW_PTRACE=1 "$HERE/bin/shm_spin" "$SHM" "$SECS" > "$OUT/${TAG}_app.log" 2>&1 &
APP=$!
for i in $(seq 1 200); do [ -e "/dev/shm$SHM" ] && break; sleep 0.05; done
sleep 2
"$HERE/bin/shm_watch" "$SHM" 20 $((SECS + 15)) > "$OUT/${TAG}_watch.log" 2>&1 &
W=$!
"$HERE/bin/l3mi" --pid $APP --cycles "$CYCLES" --hold-ms "$HOLD" --gap-ms "$GAP" \
    > "$OUT/${TAG}_agent.log" 2>&1
echo "[l3mi] agent exit=$?"
# keep observing until the target itself finishes, otherwise the later freeze
# windows fall outside the observer's lifetime and get missed
wait $APP 2>/dev/null
sleep 1
kill $W 2>/dev/null; wait $W 2>/dev/null
rm -f "/dev/shm$SHM"
echo "--- agent ---"; grep -E "attached|cycle|detach" "$OUT/${TAG}_agent.log"
echo "--- external observer freeze windows (20 ms sampling) ---"
grep -o "counter=[0-9]*" "$OUT/${TAG}_watch.log" | cut -d= -f2 > /tmp/l3miv.txt
python3 -c "
v=[int(x) for x in open('/tmp/l3miv.txt')]
z=0; tot=0
for i in range(1,len(v)):
    if v[i]==v[i-1]: z+=1
    else:
        if z>=3: print('  FROZEN ~%.2f s at counter=%d'%(z*0.02,v[i-1])); tot+=1
        z=0
print('  samples=%d  freeze windows=%d'%(len(v),tot))"
