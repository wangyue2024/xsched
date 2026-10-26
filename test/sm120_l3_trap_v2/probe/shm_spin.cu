// shm_spin.cu - spin kernel whose progress counter lives in POSIX shared memory,
// so an *external* process can watch it while a debugger has us stopped.
//
// R0 ground truth: v1 concluded "even cuda-gdb cannot stop a running kernel"
// from `info cuda threads` output and from a counter read with cudaMemcpy inside
// the same process - both of which are unusable while the inferior is stopped.
// Here the counter is in /dev/shm (cudaHostRegister'd, mapped), the kernel
// publishes to it directly, and shm_watch reads it from another process, so a
// device freeze is observable independently of our own host threads.
//
// `stop_here()` is called once, a few seconds after launch, as the breakpoint
// target for a scripted cuda-gdb session.
//
// Build: ../build.sh shm_spin    Run:
//   ./shm_spin /sm120_l3_ctl [seconds]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <sys/prctl.h>
#include <string>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cuda_runtime.h>

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

// ctl layout (4 KB): [0]=counter, [1]=completed flag, [2]=kernel start ns,
// [3]=last publish ns, [4]=pid, [5]=iterations marker
__global__ void spin_publish(volatile unsigned long long *ctl, unsigned long long cycles)
{
    unsigned long long start = clock64(), i = 0;
    while (clock64() - start < cycles) {
        i++;
        if ((i & 0xff) == 0) ctl[0] = i;
    }
    ctl[0] = i;
    ctl[1] = 1;
}

__attribute__((noinline)) static void stop_here(volatile unsigned long long *ctl)
{
    ctl[5] = 1;   // observable marker: the app reached the breakpoint site
}

static double now_s()
{
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// compute-bound variant: saturates the FMA pipes so two kernels genuinely
// contend for the SMs (the plain clock64 spin barely uses issue slots).
__global__ void burn_publish(volatile unsigned long long *c, unsigned long long cycles)
{
    unsigned long long start = clock64(), i = 0;
    float a = 1.000001f, b = 0.999999f, x = threadIdx.x * 1e-6f;
    while (clock64() - start < cycles) {
#pragma unroll 16
        for (int k = 0; k < 64; k++) x = fmaf(x, a, b);
        i++;
        if ((i & 0x3f) == 0) *c = (unsigned long long)(x * 1e9f) ^ i;
    }
    *c = i | (1ULL << 63);
}

int main(int argc, char **argv)
{
    // An XSched runtime sits inside the application, so it can declare itself
    // traceable; /proc/sys/kernel/yama/ptrace_scope=1 otherwise makes
    // "cuda-gdb -p" fail with EPERM.
    if (getenv("L3_ALLOW_PTRACE")) {

        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
        fprintf(stderr, "[shm_spin] PR_SET_PTRACER_ANY set\n");
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string shm = argc > 1 ? argv[1] : "/sm120_l3_ctl";
    long seconds = argc > 2 ? atol(argv[2]) : 120;

    int fd = shm_open(shm.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, 4096)) { perror("ftruncate"); return 1; }
    void *hp = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (hp == MAP_FAILED) { perror("mmap"); return 1; }
    memset(hp, 0, 4096);
    volatile unsigned long long *ctl = (volatile unsigned long long *)hp;

    RT_CHECK(cudaFree(0));
    RT_CHECK(cudaHostRegister(hp, 4096, cudaHostRegisterMapped));
    unsigned long long *ctl_dev = nullptr;
    RT_CHECK(cudaHostGetDevicePointer((void **)&ctl_dev, hp, 0));

    cudaStream_t s;
    RT_CHECK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
    cudaDeviceProp prop;
    RT_CHECK(cudaGetDeviceProperties(&prop, 0));
    unsigned long long cycles = (unsigned long long)prop.clockRate * 1000ULL * seconds;

    ctl[4] = (unsigned long long)getpid();
    printf("[app] pid=%d shm=%s ctl_dev=%p clock=%d kHz spin=%lds\n",
           (int)getpid(), shm.c_str(), (void *)ctl_dev, prop.clockRate, seconds);

    // warmup so lazy module loading cannot serialize anything later.
    // L3_STREAMS=N launches the warmup on N streams, so the process ends up with
    // N channels - used to check whether the device-suspend latency is per channel.
    const char *md = getenv("L3_MODE");
    const char *st = getenv("L3_STREAMS");
    bool burn = md && !strcmp(md, "fma");
    int nstreams = st ? atoi(st) : 1;
    if (nstreams < 1) nstreams = 1;
    static cudaStream_t sv[64];
    if (nstreams > 1 && nstreams <= 64) {
        for (int i = 0; i < nstreams; i++) RT_CHECK(cudaStreamCreateWithFlags(&sv[i], cudaStreamNonBlocking));
        for (int i = 0; i < nstreams; i++) {
            if (burn) burn_publish<<<1, 32, 0, sv[i]>>>(ctl_dev, 1000);
            else      spin_publish<<<1, 32, 0, sv[i]>>>(ctl_dev, 1000);
        }
        for (int i = 0; i < nstreams; i++) RT_CHECK(cudaStreamSynchronize(sv[i]));
        printf("[app] warmed up %d streams\n", nstreams);
    } else {
        if (burn) burn_publish<<<1, 32, 0, s>>>(ctl_dev, 1000);
        else      spin_publish<<<1, 32, 0, s>>>(ctl_dev, 1000);
        RT_CHECK(cudaStreamSynchronize(s));
    }
    memset(hp, 0, 4096);
    ctl[4] = (unsigned long long)getpid();

    const char *bl = getenv("L3_BLOCKS");
    const char *th = getenv("L3_THREADS");
    int nblk = bl ? atoi(bl) : 8;
    int nthr = th ? atoi(th) : 32;
    if (burn) burn_publish<<<nblk, nthr, 0, s>>>(ctl_dev, cycles);
    else      spin_publish<<<nblk, nthr, 0, s>>>(ctl_dev, cycles);
    RT_CHECK(cudaGetLastError());
    printf("[app] kernel launched (%d blocks x %d threads)\n", nblk, nthr);

    bool stopped = false;
    double last = now_s();
    unsigned long long lastc = 0;
    while (!ctl[1]) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        unsigned long long c = ctl[0];
        double t = now_s();
        printf("[app] t=%7.2f counter=%-14llu rate=%.1f M/s%s\n", t, c,
               (c - lastc) / 1e6 / (t - last + 1e-9), stopped ? " (after stop_here)" : "");
        lastc = c; last = t;
        if (!stopped && t > 3.0) { stop_here(ctl); stopped = true; printf("[app] stop_here() called\n"); }
        if (t > seconds + 30) { printf("[app] timeout\n"); break; }
    }
    printf("[app] kernel done, counter=%llu\n", (unsigned long long)ctl[0]);
    cudaStreamSynchronize(s);
    return 0;
}
