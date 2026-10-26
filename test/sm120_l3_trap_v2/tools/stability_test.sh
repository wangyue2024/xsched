#!/bin/bash
# stability_test.sh [runs] - repeat the HAL-level L3 suspend/resume and collect
# the latency seen by the scheduler process plus the tenant speedup.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
export PATH="$HERE/bin:$PATH"
export LD_LIBRARY_PATH="/tmp/xsbuild/platforms/cuda:/tmp/xsbuild/preempt:${LD_LIBRARY_PATH:-}"
RUNS=${1:-3}
for i in $(seq 1 $RUNS); do
    pkill -9 -x shm_spin 2>/dev/null; sleep 0.5
    echo "--- run $i ---"
    "$HERE/bin/l3hal" --suspend 2>&1 | grep -E "Suspend\(hog\)|finished after|Resume"
done
pkill -9 -x shm_spin 2>/dev/null
