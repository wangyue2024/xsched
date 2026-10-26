// l3app.cu - the level-3 proof-of-concept target.
//
// It runs an ordinary, unmodified-style spin kernel and a host-side poller
// thread that calls l3_poll() every 200 us.  A cuda-gdb session attaches and
// puts a conditional breakpoint on l3_poll, so the GPU is frozen exactly when
// the external scheduler asks for it (see run_l3poc.sh).
//
// The scheduler talks to us through /dev/shm/l3poc_req ('0' = run, '1' =
// suspend); we acknowledge through /dev/shm/l3poc_state, which cuda-gdb writes.
// The kernel's progress counter lives in /dev/shm so that a third process can
// verify the freeze without being affected by our own ptrace stop.
//
// Build: ../build.sh l3app    Run: L3_ALLOW_PTRACE=1 ./l3app /sm120_l3poc 40

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <cuda_runtime.h>

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

struct Shm {
    volatile unsigned long long counter;
    volatile uint32_t done;
    volatile uint32_t pad;
};

__global__ void spin_publish(volatile unsigned long long *c, unsigned long long cycles)
{
    unsigned long long start = clock64(), i = 0;
    while (clock64() - start < cycles) { i++; if ((i & 0xff) == 0) *c = i; }
    *c = i | (1ULL << 63);
}

// ---- the instrumented polling point the debugger breaks on ----------------
volatile int l3_want_suspend = 0;
volatile int l3_stop = 0;

// The body must not be optimisable away: gcc deletes calls to an empty
// function even with noinline, which silently removed every breakpoint hit.
__attribute__((noinline)) void l3_poll(void) { __asm__ __volatile__("" ::: "memory"); }

static int req_fd = -1;
static bool req_suspend()
{
    char b = '0';
    if (req_fd < 0) return false;
    if (pread(req_fd, &b, 1, 0) != 1) return false;
    return b == '1';
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (getenv("L3_ALLOW_PTRACE")) {
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
        fprintf(stderr, "[l3app] PR_SET_PTRACER_ANY set\n");
    }
    const char *shmname = argc > 1 ? argv[1] : "/sm120_l3poc";
    long seconds = argc > 2 ? atol(argv[2]) : 40;

    req_fd = open("/dev/shm/l3poc_req", O_RDONLY);
    fprintf(stderr, "[l3app] req_fd=%d\n", req_fd);

    int fd = shm_open(shmname, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, sizeof(Shm))) { perror("ftruncate"); return 1; }
    Shm *sh = (Shm *)mmap(nullptr, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sh == MAP_FAILED) { perror("mmap"); return 1; }
    memset((void *)sh, 0, sizeof *sh);

    void *hp = sh;                        // publish straight into /dev/shm
    RT_CHECK(cudaHostRegister(hp, sizeof *sh, cudaHostRegisterMapped));
    unsigned long long *hc_dev = nullptr;
    RT_CHECK(cudaHostGetDevicePointer((void **)&hc_dev, hp, 0));

    cudaDeviceProp prop;
    RT_CHECK(cudaGetDeviceProperties(&prop, 0));
    unsigned long long cycles = (unsigned long long)prop.clockRate * 1000ULL * seconds;

    cudaStream_t vs;
    RT_CHECK(cudaStreamCreateWithFlags(&vs, cudaStreamNonBlocking));
    spin_publish<<<1, 32, 0, vs>>>(hc_dev, 1000);      // warmup
    RT_CHECK(cudaStreamSynchronize(vs));
    sh->counter = 0;

    printf("[l3app] pid=%d shm=%s launch %ld s\n", getpid(), shmname, seconds);

    std::thread poller([] {
        fprintf(stderr, "[l3app] poller thread up\n");
        // Edge triggered: l3_poll() is only called when the scheduler asks for
        // a freeze, so the debugger can use an *unconditional* breakpoint there
        // (cuda-gdb 13.2 has no Python and its breakpoint conditions on host
        // globals never fired in our tests).
        while (!l3_stop) {
            if (req_suspend()) {
                l3_want_suspend = 1;
                fprintf(stderr, "[l3app] poller: requesting suspend\n");
                l3_poll();
                l3_want_suspend = 0;
                fprintf(stderr, "[l3app] poller: resumed\n");
                while (req_suspend() && !l3_stop)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    spin_publish<<<8, 32, 0, vs>>>(hc_dev, cycles);
    RT_CHECK(cudaGetLastError());
    cudaError_t se = cudaStreamSynchronize(vs);

    l3_stop = 1;
    poller.join();
    printf("[l3app] victim done: %s counter=%llu\n", cudaGetErrorName(se),
           (unsigned long long)(sh->counter & ~(1ULL << 63)));
    sh->done = 1;
    return 0;
}
