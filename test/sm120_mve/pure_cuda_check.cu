// =====================================================================
//  pure_cuda_check.cu - system-level sanity probe (no XSched, no cuxtra)
//
//  Replicates the MVE workload shape with the plain CUDA runtime only:
//  2048 blocks x 128 threads busy-spin ~50 ms each; thread (0,0,0) of
//  every block writes its own marker (out[blockIdx.x] = MAGIC + bid).
//
//  Runs N rounds of [fill 0xCC -> launch -> sync -> count markers] and
//  reports any incompleteness and the per-round wall time.  If THIS
//  probe shows missing markers or wild runtime swings, the machine
//  (driver / WDDM / foreign GPU load) is the culprit, not the XSched
//  level-2 mechanism.
// =====================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>

#include <cuda_runtime.h>

#define BLOCKS   2048
#define THREADS  128
#define SLOTS    4096
#define MAGIC    0xA250000000000000ull
#define FILL     0xCC

__global__ void marker_kernel(unsigned long long *out, long long spin)
{
    unsigned long long acc = 0x9E3779B97F4A7C15ull ^ (unsigned long long)blockIdx.x;
    for (long long i = 0; i < spin; ++i) {
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        acc ^= (acc >> 29);
    }
    if ((threadIdx.x | threadIdx.y | threadIdx.z) == 0) {
        out[blockIdx.x] = MAGIC + blockIdx.x;
        if (acc == 0x1234567890ABCDEFull) out[blockIdx.x] = 0;  /* never mind */
    }
}

static double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv)
{
    int rounds = (argc > 1) ? atoi(argv[1]) : 10;

    unsigned long long *d_out = nullptr;
    cudaMalloc(&d_out, SLOTS * 8);

    /* calibrate ~50 ms/block */
    long long spin = 1 << 20;
    marker_kernel<<<BLOCKS, THREADS>>>(d_out, spin);
    cudaDeviceSynchronize();
    double t0 = nowMs();
    marker_kernel<<<BLOCKS, THREADS>>>(d_out, spin);
    cudaDeviceSynchronize();
    double ms = nowMs() - t0;
    if (ms < 1) ms = 1;
    spin = (long long)((double)spin * 50.0 / ms);
    std::printf("[pure] spin=%lld -> %.1f ms for 2048 blocks (per-wave ~%.1f ms)\n",
                spin, ms, ms);

    int bad = 0;
    for (int r = 0; r < rounds; ++r) {
        cudaMemset(d_out, FILL, BLOCKS * 8);
        double t = nowMs();
        marker_kernel<<<BLOCKS, THREADS>>>(d_out, spin);
        cudaError_t e = cudaDeviceSynchronize();
        double wall = nowMs() - t;
        std::vector<unsigned long long> h(BLOCKS);
        cudaMemcpy(h.data(), d_out, BLOCKS * 8, cudaMemcpyDeviceToHost);
        int done = 0;
        for (int i = 0; i < BLOCKS; ++i)
            if (h[i] != 0xCCCCCCCCCCCCCCCCull) ++done;
        bool ok = (e == cudaSuccess) && (done == BLOCKS);
        if (!ok) ++bad;
        std::printf("[pure] round %2d: done=%4d/%d wall=%8.1f ms %s%s\n",
                    r, done, BLOCKS, wall, ok ? "OK" : "*** MISSING ***",
                    (e == cudaSuccess) ? "" : cudaGetErrorString(e));
        std::fflush(stdout);
    }
    std::printf("[pure] RESULT: %d bad round(s) out of %d\n", bad, rounds);
    cudaFree(d_out);
    return bad;
}
