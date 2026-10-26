#!/bin/bash
# run_l3poc.sh - Level-3 interrupt PoC driven by an external scheduler.
#
# Topology:
#   l3app (target)  <- ptrace-attached by ->  cuda-gdb (the "debug agent")
#   l3sched (this script) flips /dev/shm/l3poc_req and watches the kernel's
#   counter in /dev/shm from a completely separate process.
#
# cuda-gdb stops the inferior while it sits at the l3_poll breakpoint, which
# freezes the GPU kernel mid-instruction (verified in run_gt2.sh).  The gdb
# command script holds that stop until the scheduler withdraws the request.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/evidence/live
SHM=/sm120_l3poc
REQ=/dev/shm/l3poc_req
ST=/dev/shm/l3poc_state
SECS=${1:-60}
mkdir -p "$OUT"
rm -f "$REQ" "$ST" "/dev/shm$SHM"
echo 0 > "$REQ"; echo 0 > "$ST"
chmod 666 "$REQ" "$ST"

cat > /tmp/l3poc.cmds <<'CMDS'
set pagination off
set confirm off
set debuginfod enabled off
break l3_poll
commands
  silent
  shell echo 1 > /dev/shm/l3poc_state
  printf "[l3poc] FROZEN at l3_poll\n"
  shell while [ "$(cat /dev/shm/l3poc_req 2>/dev/null || echo 0)" != "0" ]; do sleep 0.005; done
  shell echo 0 > /dev/shm/l3poc_state
  printf "[l3poc] RESUMING\n"
  continue
end
run
CMDS

# Launch under cuda-gdb: breakpoints on an *attached* process never fired in our
# tests, while the launch path is proven (run_r0.sh / run_gt2.sh).
cuda-gdb -batch -x /tmp/l3poc.cmds --args "$HERE/bin/l3app" "$SHM" "$SECS" \
    > "$OUT/l3poc_gdb.log" 2>&1 &
GDB=$!

for i in $(seq 1 200); do [ -e "/dev/shm$SHM" ] && break; sleep 0.05; done
sleep 2
"$HERE/bin/shm_watch" "$SHM" 50 $((SECS + 20)) > "$OUT/l3poc_watch.log" 2>&1 &
WATCH=$!
sleep 1
APP=0

# ---- the scheduler: three suspend/resume cycles -------------------------
cyc=0
for pause in 1.5 2.0 1.0; do
    cyc=$((cyc+1))
    echo "[l3sched] cycle $cyc: request suspend (hold ${pause}s)"
    echo 1 > "$REQ"
    for i in $(seq 1 400); do [ "$(cat $ST)" = "1" ] && break; sleep 0.005; done
    A=$(grep -o "counter=[0-9]*" "$OUT/l3poc_watch.log" | tail -1)
    sleep "$pause"
    B=$(grep -o "counter=[0-9]*" "$OUT/l3poc_watch.log" | tail -1)
    echo "[l3sched]   held ${pause}s, observer counter $A -> $B"
    echo 0 > "$REQ"
    for i in $(seq 1 400); do [ "$(cat $ST)" = "0" ] && break; sleep 0.005; done
    sleep 1.0
done
echo "[l3sched] done; waiting for app"
echo 0 > "$REQ"
wait $GDB 2>/dev/null
kill $WATCH 2>/dev/null; wait $WATCH 2>/dev/null
echo "--- freeze windows seen by the external observer ---"
awk -F'[ =]+' '/counter=/{ v=$3; if (prev!="" && v==prev) z++; else { if (z>=3) printf "  frozen ~%.2f s (counter %s)\n", z*0.05, prev; z=0 } prev=v }' "$OUT/l3poc_watch.log"
rm -f "$REQ" "$ST" "/dev/shm$SHM"
echo "--- gdb ---"; tail -20 "$OUT/l3poc_gdb.log"
