#!/bin/bash
# reuse_session_test.sh - can a second freeze reuse an already-attached gdb
# session (via -exec-continue / -exec-interrupt) instead of re-attaching?
# All timestamps are CLOCK_MONOTONIC (a previous version mixed in epoch time).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
pkill -9 -x shm_spin 2>/dev/null; pkill -9 -x cuda-gdb 2>/dev/null; sleep 0.5
rm -f /dev/shm/lat_r /tmp/reuse_fifo; mkfifo /tmp/reuse_fifo
export PATH="$HERE/bin:$PATH"
L3_ALLOW_PTRACE=1 L3_BLOCKS=8 L3_THREADS=32 "$HERE/bin/shm_spin" /lat_r 12 >/dev/null 2>&1 &
P=$!
for j in $(seq 1 200); do [ -e /dev/shm/lat_r ] && break; sleep 0.02; done
sleep 1
python3 "$HERE/tools/sample1ms.py" lat_r 12 > /tmp/reuse_s.txt 2>&1 &
S=$!
cuda-gdb --interpreter=mi2 -q -nx < /tmp/reuse_fifo > /tmp/reuse_mi.txt 2>&1 &
G=$!
: > /tmp/reuse_t.txt
python3 - "$P" <<'PY'
import sys, time, os
pid = sys.argv[1]
fd = os.open('/tmp/reuse_fifo', os.O_WRONLY)
log = open('/tmp/reuse_t.txt', 'w')
def say(cmd):
    t = time.clock_gettime(time.CLOCK_MONOTONIC)
    os.write(fd, (cmd + '\n').encode())
    log.write("%.4f %s\n" % (t, cmd)); log.flush()
    time.sleep(0.05)
time.sleep(0.5)
say("1-target-attach %s" % pid)
time.sleep(1.0); say("2-exec-continue")
time.sleep(1.0); say("3-exec-interrupt")
time.sleep(1.0); say("4-exec-continue")
time.sleep(1.0); say("5-exec-interrupt")
time.sleep(1.0); say("6-exec-continue")
time.sleep(1.0); say("7-target-detach"); say("8-gdb-exit")
log.close()
PY
wait $S 2>/dev/null
kill -9 $P $G 2>/dev/null
python3 - <<'PY'
ev = [(float(l.split()[0]), l.split()[1]) for l in open('/tmp/reuse_t.txt') if len(l.split()) == 2]
s = []
for l in open('/tmp/reuse_s.txt'):
    p = l.split()
    if len(p) == 2: s.append((float(p[0]), int(p[1])))
def onset(after, want_frozen):
    i0 = next((i for i in range(1, len(s)) if s[i][0] > after), None)
    if i0 is None: return None
    for i in range(i0, len(s)):
        same = (s[i][1] == s[i-1][1])
        if same == want_frozen:
            if i+3 < len(s) and (s[i+3][1] == s[i][1]) == want_frozen: return s[i][0]
    return None
for ts, cmd in ev:
    name = cmd.split('-', 1)[1]
    if 'attach' in cmd or 'interrupt' in cmd:
        f = onset(ts, True)
        print("%-22s -> %s" % (name, ("FROZEN in %.1f ms" % ((f-ts)*1e3)) if f else "NOT FROZEN"))
    elif 'continue' in cmd:
        r = onset(ts, False)
        print("%-22s -> %s" % (name, ("RUNNING in %.1f ms" % ((r-ts)*1e3)) if r else "DID NOT RESUME"))
PY
rm -f /dev/shm/lat_r /tmp/reuse_fifo
