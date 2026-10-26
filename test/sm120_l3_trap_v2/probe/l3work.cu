// l3work.cu - correctness-under-interruption test for the sm120 L3 interrupt.
//
// The kernel runs a *fixed* number of iterations and accumulates a checksum, so
// the expected final value is deterministic: sum(0..N-1) = N*(N-1)/2.  Freezing
// the process mid-flight (run_l3work.sh uses the cuda-gdb agent) must not change
// the result - it must come out bit-exact, and the progress counter must stop
// while the freeze is in effect and continue afterwards.
//
// Build: ../build.sh l3work     Run: L3_ALLOW_PTRACE=1 ./l3work /sm120_l3work 2000000000

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <cuda_runtime.h>

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

struct Shm {
    volatile unsigned long long progress;   // updated every 65536 iterations
    volatile unsigned long long result;     // final checksum
    volatile unsigned int done;
    unsigned int pad;
};

__global__ void accumulate(volatile unsigned long long *progress,
                           volatile unsigned long long *result,
                           unsigned long long n)
{
    // one thread does the whole sum so the iteration order (and therefore the
    // value) is identical no matter when the warp is stopped and resumed
    if (blockIdx.x || threadIdx.x) return;
    unsigned long long sum = 0;
    for (unsigned long long i = 0; i < n; i++) {
        sum += i;
        if ((i & 0xffff) == 0) *progress = i;
    }
    *result = sum;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (getenv("L3_ALLOW_PTRACE")) {
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
        fprintf(stderr, "[l3work] PR_SET_PTRACER_ANY set\n");
    }
    const char *name = argc > 1 ? argv[1] : "/sm120_l3work";
    unsigned long long n = argc > 2 ? strtoull(argv[2], nullptr, 0) : 2000000000ULL;

    int fd = shm_open(name, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, sizeof(Shm))) { perror("ftruncate"); return 1; }
    Shm *sh = (Shm *)mmap(nullptr, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sh == MAP_FAILED) { perror("mmap"); return 1; }
    memset((void *)sh, 0, sizeof *sh);
    RT_CHECK(cudaHostRegister(sh, sizeof *sh, cudaHostRegisterMapped));
    unsigned long long *d_prog = nullptr, *d_res = nullptr;
    RT_CHECK(cudaHostGetDevicePointer((void **)&d_prog, (void *)&sh->progress, 0));
    RT_CHECK(cudaHostGetDevicePointer((void **)&d_res, (void *)&sh->result, 0));

    printf("[l3work] pid=%d n=%llu\n", getpid(), n);
    accumulate<<<1, 32>>>(d_prog, d_res, n);           // warmup
    RT_CHECK(cudaDeviceSynchronize());
    memset((void *)sh, 0, sizeof *sh);

    accumulate<<<1, 32>>>(d_prog, d_res, n);
    RT_CHECK(cudaGetLastError());
    cudaError_t se = cudaStreamSynchronize(0);
    unsigned long long expected = n * (n - 1) / 2;
    printf("[l3work] done: %s  result=%llu expected=%llu  %s\n",
           cudaGetErrorName(se), (unsigned long long)sh->result,
           (unsigned long long)expected,
           sh->result == expected ? "CORRECT" : "MISMATCH");
    sh->done = 1;
    return sh->result == expected ? 0 : 3;
}
