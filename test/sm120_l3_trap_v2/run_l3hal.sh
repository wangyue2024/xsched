#!/bin/bash
# run_l3hal.sh - drives the XSched level-3 GDB backend through its HAL API.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH="$HERE/bin:$PATH"
export LD_LIBRARY_PATH="/tmp/xsbuild/platforms/cuda:/tmp/xsbuild/preempt:${LD_LIBRARY_PATH:-}"
pkill -9 -x shm_spin 2>/dev/null; pkill -9 -x cuda-gdb 2>/dev/null; sleep 1
echo "=== baseline: hog runs, high-priority kernel waits ==="
"$HERE/bin/l3hal" 2>&1 | tail -5
sleep 1
echo "=== L3: hog suspended through GdbInterruptContext ==="
"$HERE/bin/l3hal" --suspend 2>&1 | tail -6
