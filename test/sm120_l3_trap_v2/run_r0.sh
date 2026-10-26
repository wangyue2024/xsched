#!/bin/bash
# run_r0.sh - R0 ground truth: does cuda-gdb actually freeze a running kernel?
#
# The producer (shm_spin) publishes its iteration counter into /dev/shm, so the
# observer (shm_watch) can see a freeze from another process while cuda-gdb has
# the inferior stopped - which is exactly what v1's methodology could not do.
#
# Timeline: kernel launched at t~1s, stop_here() breakpoint at t~4s, then an 8 s
# observation window inside cuda-gdb (`shell sleep 8`), then continue.
#
# Usage: ./run_r0.sh [tag]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TAG=${1:-r0}
SHM=/sm120_l3_$TAG
OUT=$HERE/evidence/live
mkdir -p "$OUT"
rm -f "/dev/shm$SHM"

cat > /tmp/gdb_$TAG.cmds <<'EOF'
set pagination off
set confirm off
break stop_here
run
printf "\n===== STOPPED AT stop_here =====\n"
info cuda kernels
info cuda contexts
info cuda sms
printf "\n===== 8 s observation window (device should be suspended) =====\n"
shell sleep 8
printf "\n===== after window =====\n"
info cuda kernels
continue
shell sleep 3
printf "\n===== detaching =====\n"
detach
quit
EOF

echo "[r0] starting producer under cuda-gdb (shm=$SHM)"
cuda-gdb -batch -x /tmp/gdb_$TAG.cmds "$HERE/bin/shm_spin" "$SHM" 30 \
    > "$OUT/${TAG}_gdb.log" 2>&1 &
GDB=$!

# let the kernel get running before the observer starts
sleep 5
echo "[r0] starting external observer"
"$HERE/bin/shm_watch" "$SHM" 100 30 > "$OUT/${TAG}_watch.log" 2>&1 &
WATCH=$!

wait $GDB
echo "[r0] cuda-gdb finished (rc=$?)"
sleep 1
kill $WATCH 2>/dev/null
wait $WATCH 2>/dev/null
rm -f "/dev/shm$SHM"
echo "[r0] logs: $OUT/${TAG}_gdb.log  $OUT/${TAG}_watch.log"
