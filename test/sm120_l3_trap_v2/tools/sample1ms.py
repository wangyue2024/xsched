import mmap, struct, sys, time
name, dur = sys.argv[1], float(sys.argv[2])
f = open('/dev/shm/'+name, 'r+b'); m = mmap.mmap(f.fileno(), 0)
t0 = time.clock_gettime(time.CLOCK_MONOTONIC)
while time.clock_gettime(time.CLOCK_MONOTONIC) - t0 < dur:
    print("%.4f %d" % (time.clock_gettime(time.CLOCK_MONOTONIC), struct.unpack_from('<Q', m, 0)[0]), flush=True)
    time.sleep(0.001)
