#!/bin/bash
# run_gt2.sh - can cuda-gdb attach to an ALREADY RUNNING cuda process and
# freeze a mid-kernel spin?  (R0 only proved the launch-under-gdb case.)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/evidence/live
SHM=/sm120_l3_gt2
rm -f "/dev/shm$SHM"

L3_ALLOW_PTRACE=1 "$HERE/bin/shm_spin" "$SHM" 30 > /dev/null 2>&1 &
APP=$!
echo "[gt2] victim pid=$APP"
for i in $(seq 1 200); do [ -e "/dev/shm$SHM" ] && break; sleep 0.05; done
sleep 2

"$HERE/bin/shm_watch" "$SHM" 100 25 > "$OUT/gt2_watch.log" 2>&1 &
WATCH=$!

cat > /tmp/gt2.cmds <<'CMDS'
set pagination off
set confirm off
printf "\n===== ATTACHED, holding 8 s =====\n"
shell sleep 8
printf "\n===== detaching =====\n"
detach
quit
CMDS
cuda-gdb -batch -p $APP -x /tmp/gt2.cmds > "$OUT/gt2_gdb.log" 2>&1
echo "[gt2] cuda-gdb exit=$?"
sleep 2
kill $WATCH 2>/dev/null; wait $WATCH 2>/dev/null
kill $APP 2>/dev/null; wait $APP 2>/dev/null
rm -f "/dev/shm$SHM"
echo "--- frozen window analysis ---"
awk '{ if ($3 ~ /counter=/) { split($3,a,"="); v=a[2]; if (prev!="") { d=v-prev; if (d==0) z++; else if (z>0) { printf "frozen for ~%.1f s (counter %s)\n", z*0.1, prev; z=0 } } prev=v } } END { if (z>0) printf "frozen at end for ~%.1f s\n", z*0.1 }' "$OUT/gt2_watch.log"
tail -25 "$OUT/gt2_gdb.log"
