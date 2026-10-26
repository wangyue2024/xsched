#!/bin/bash
# run_l3handover.sh - the XSched-shaped API end to end:
#   hog A runs, agent daemon serves it, high-priority B arrives, the scheduler
#   asks the agent to suspend A, B completes, then A is resumed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
# hard pre-clean: a leftover target from a previous run keeps the GPU busy and
# makes the next target exit early
pkill -9 -x shm_spin 2>/dev/null; pkill -9 -x l3mi 2>/dev/null; pkill -9 -x cuda-gdb 2>/dev/null; 
sleep 1
rm -f /dev/shm/ho2_a /dev/shm/ho2_b /dev/shm/sm120_l3ctrl /tmp/ho2_tb
L3_ALLOW_PTRACE=1 L3_BLOCKS=20000 L3_THREADS=256 "$HERE/bin/shm_spin" /ho2_a 20 >/tmp/ho2_a.log 2>&1 &
PA=$!
for i in $(seq 1 200); do [ -e /dev/shm/ho2_a ] && break; sleep 0.05; done
sleep 3
if ! ps -o stat= -p $PA >/dev/null 2>&1; then
    echo "[FATAL] target A (pid $PA) died at startup"; cat /tmp/ho2_a.log 2>/dev/null; exit 1
fi
echo "[sched] A pid=$PA alive ($(ps -o stat= -p $PA))"
"$HERE/bin/l3mi" --pid $PA --serve /sm120_l3ctrl > /tmp/ho2_agent.log 2>&1 &
PD=$!
sleep 0.5
echo "[sched] state before: $("$HERE/tools/l3ctl.py" sm120_l3ctrl state)"
T0=$(date +%s.%N)
( L3_ALLOW_PTRACE=1 L3_BLOCKS=1 L3_THREADS=32 "$HERE/bin/shm_spin" /ho2_b 0 >/dev/null 2>&1
  date +%s.%N > /tmp/ho2_tb ) &
PB=$!
sleep 0.5
echo "[sched] suspend: $("$HERE/tools/l3ctl.py" sm120_l3ctrl suspend)"
for i in $(seq 1 900); do [ -s /tmp/ho2_tb ] && break; sleep 0.05; done
TB=$(cat /tmp/ho2_tb 2>/dev/null || echo "")
python3 -c "
print('[result] B completed %.2f s after launch'%(${TB:-0} - $T0)) if '$TB' else print('[result] B never completed')"
echo "[sched] resume: $("$HERE/tools/l3ctl.py" sm120_l3ctrl resume)"
"$HERE/tools/l3ctl.py" sm120_l3ctrl quit
wait $PD 2>/dev/null
kill -9 $PA $PB 2>/dev/null
rm -f /dev/shm/ho2_a /dev/shm/ho2_b /dev/shm/sm120_l3ctrl /tmp/ho2_tb
echo "--- agent ---"; cat /tmp/ho2_agent.log
