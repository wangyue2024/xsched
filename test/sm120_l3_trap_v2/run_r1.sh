#!/bin/bash
# run_r1.sh - capture the RM ioctl stream of a *real* cuda-gdb device suspend.
#
# R0 (run_r0.sh) proved the device does freeze: during an 8.22 s cuda-gdb stop
# the victim's counter advanced only 7.44 M iterations (~0.2 s worth) instead of
# ~300 M, and `info cuda kernels` still claimed "Active" - which is exactly the
# output v1 misread as "cuda-gdb cannot stop a running kernel".
#
# This run repeats it with tools/ioctl_log6.so preloaded into the inferior so
# every RM_ALLOC / RM_CONTROL (with parameters and timestamps) around the
# suspend is recorded, plus an external /dev/shm observer that timestamps the
# freeze from another process.
#
# Usage: ./run_r1.sh [tag] [app-seconds]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TAG=${1:-r1}
APPSEC=${2:-8}
SHM=/sm120_l3_$TAG
OUT=$HERE/evidence/live
mkdir -p "$OUT"
rm -f "/dev/shm$SHM" "$OUT/${TAG}_ioctl.log".*

gcc -shared -fPIC -O2 "$HERE/tools/ioctl_log6.c" -o "$HERE/bin/ioctl_log6.so" -ldl \
    || { echo "interceptor build failed"; exit 1; }

cat > /tmp/gdb_$TAG.cmds <<'EOF'
set pagination off
set confirm off
set exec-wrapper env LD_PRELOAD=INTERCEPTOR IOCTL_LOG=IOCTLLOG
set args ARGS
break stop_here
run
printf "\n===== STOPPED AT stop_here =====\n"
info cuda kernels
printf "\n===== 10 s observation window =====\n"
shell sleep 10
printf "\n===== resuming =====\n"
continue
shell sleep 2
detach
quit
EOF
sed -i "s|INTERCEPTOR|$HERE/bin/ioctl_log6.so|; s|IOCTLLOG|$OUT/${TAG}_ioctl.log|; s|ARGS|$SHM $APPSEC|" /tmp/gdb_$TAG.cmds

echo "[r1] cuda-gdb + interceptor -> $OUT/${TAG}_ioctl.log"
IOCTL_LOG="$OUT/${TAG}_ioctl.log" \
cuda-gdb -batch -x /tmp/gdb_$TAG.cmds "$HERE/bin/shm_spin" "$SHM" "$APPSEC" \
    > "$OUT/${TAG}_gdb.log" 2>&1 &
GDB=$!

# start the observer as soon as the producer has created the shm segment
for i in $(seq 1 400); do
    [ -e "/dev/shm$SHM" ] && break
    sleep 0.2
done
echo "[r1] observer starting (shm present after ~$((i/5))s)"
"$HERE/bin/shm_watch" "$SHM" 100 90 > "$OUT/${TAG}_watch.log" 2>&1 &
WATCH=$!

wait $GDB
echo "[r1] cuda-gdb finished"
sleep 1
kill $WATCH 2>/dev/null
wait $WATCH 2>/dev/null
rm -f "/dev/shm$SHM"
echo "[r1] logs: ${TAG}_gdb.log ${TAG}_watch.log ${TAG}_ioctl.log"
wc -l "$OUT/${TAG}_gdb.log" "$OUT/${TAG}_watch.log" "$OUT/${TAG}_ioctl.log".* 2>/dev/null
