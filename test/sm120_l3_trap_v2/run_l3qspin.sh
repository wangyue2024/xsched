#!/bin/bash
# run_l3qspin.sh - reproducible sm120 trap-backend experiments on a *managed*
# kernel (submitted through the XSched driver shim, so the QUEUE owns it).
#
#   ./run_l3qspin.sh suspend   # prove the trap stops a running kernel (~30 ms)
#   ./run_l3qspin.sh kill      # + replay the killed kernel on resume
#   ./run_l3qspin.sh probe     # record the trap reason at the handler
#   ./run_l3qspin.sh noinstr   # managed launch without instrumentation
#
# All runs use cuda-gdb only as the 0x83de0317 ack provider (FINDINGS v2.17+).
# Without it 0317 blocks; the trap itself is delivered by RM, not by gdb.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-suspend}
SPIN=${SPIN:-3000}
HOLD=${HOLD:-800}

pkill -9 -x l3qspin 2>/dev/null; pkill -9 -x l3agent 2>/dev/null; pkill -9 -x cuda-gdb 2>/dev/null
rm -f /dev/shm/xsched-sm120-trap

export LD_LIBRARY_PATH=/tmp/xsbuild/platforms/cuda:/tmp/xsbuild/preempt:/tmp/xsbuild/utils
export XSCHED_SM120_L3_BACKEND=TRAP
export XSCHED_SM120_TRAP_SHM=/xsched-sm120-trap
export XSCHED_SM120_TRAP_AGENT=$HERE/bin/l3agent
export L3AGENT_NO_MODES=1 L3AGENT_NO_WARMUP=1 L3AGENT_NO_PTRACE=1
export XSCHED_SM120_TRAP_NO_BARRIER=1

case "$MODE" in
suspend)
    # Trap-stop evidence: counter freezes at the suspend instant and stays
    # frozen for the whole hold (kernel was launched with a longer spin).
    export L3AGENT_HOLD=1 XSCHED_SM120_TRAP_NO_PAYLOAD=1 XSCHED_SM120_TRAP_KEEP_SESSION=1
    ;;
kill)
    # Kill + replay: payload kills, the queue replays the killed kernel.
    export L3AGENT_AUTO_RESUME=1
    ;;
probe)
    # Reason probe: cave records R2 and c[0x170]; reader prints at +4 s.
    export L3AGENT_HOLD=1 XSCHED_SM120_TRAP_CAVE=1 \
           XSCHED_SM120_TRAP_PARAM_PROBE=1 XSCHED_SM120_TRAP_REASON_PROBE=1 \
           XSCHED_SM120_TRAP_KEEP_SESSION=1 L3Q_HOLD_END=6
    ;;
noinstr)
    export XSCHED_SM120_TRAP_NO_INSTR=1 L3AGENT_AUTO_RESUME=1
    ;;
*)
    echo "usage: $0 {suspend|kill|probe|noinstr}" >&2; exit 2;;
esac

LOG=$HERE/evidence/live/qspin_${MODE}_$(date +%H%M%S).log
echo "[run] mode=$MODE spin=${SPIN}ms hold=${HOLD}ms log=$LOG"
timeout 60 cuda-gdb -batch -ex run --args "$HERE/bin/l3qspin" "$SPIN" "$HOLD" > "$LOG" 2>&1
RC=$?
echo "[run] rc=$RC"
grep -E "l3qspin\]|reason probe buf|trap reactivate|ERRO" "$LOG" | grep -v "XSCHED DEBG" | tail -12
