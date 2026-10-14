extern "C" __global__ void spin_until(unsigned long long deadline) {
    long long start = clock64();
    while ((unsigned long long)(clock64() - start) < deadline) { }
}
extern "C" __global__ void spin_counter(volatile unsigned long long *c) {
    while (true) { *c = *c + 1; }
}
