#!/usr/bin/env python3
"""Scheduler-side client for the l3agentd control block.

usage: l3ctl.py <shm-name> suspend|resume|state|quit
Exit code 0 on success.  'suspend'/'resume' block until the agent acks.
"""
import mmap, struct, sys, time, os
name = sys.argv[1]; op = sys.argv[2]
path = '/dev/shm/' + name
for _ in range(400):
    if os.path.exists(path) and os.path.getsize(path) >= 24: break
    time.sleep(0.02)
f = open(path, 'r+b'); m = mmap.mmap(f.fileno(), 0)
CMD, STATE, SEQ = 0, 4, 8

def rd(off): return struct.unpack_from('<I', m, off)[0]
def wr(off, v): struct.pack_into('<I', m, off, v)

if op == 'state':
    print("cmd=%d state=%d seq=%d" % (rd(CMD), rd(STATE), rd(SEQ))); sys.exit(0)
if op == 'quit':
    wr(CMD, 3); sys.exit(0)

want = 1 if op == 'suspend' else 2
expect = 1 if op == 'suspend' else 0
t0 = time.clock_gettime(time.CLOCK_MONOTONIC)
wr(CMD, want)
while time.clock_gettime(time.CLOCK_MONOTONIC) - t0 < 30:
    if rd(STATE) == expect:
        print("%s ack in %.3f s" % (op, time.clock_gettime(time.CLOCK_MONOTONIC) - t0)); sys.exit(0)
    time.sleep(0.001)
print("%s TIMEOUT" % op); sys.exit(1)
