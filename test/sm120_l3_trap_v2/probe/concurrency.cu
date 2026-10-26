// concurrency.cu - do two streams of one context actually overlap on this GPU?
//
// freeze.cu measured a 17.09 s cudaStreamSynchronize() for a one-instruction
// helper kernel launched while an 8-block spin kernel was running, i.e. the
// helper did not get scheduled until the victim finished.  This isolates that:
// same victim, but the helper is trivial and we also try (a) a second context in
// this process and (b) a victim that leaves most SMs free.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <chrono>
#include <thread>
#include <cuda_runtime.h>

#define RT_CHECK(x) do { cudaError_t _e = (x); if (_e != cudaSuccess) { \
    fprintf(stderr, "[FATAL] %s -> %s @%d\n", #x, cudaGetErrorName(_e), __LINE__); exit(1);} } while (0)

__global__ void spin(unsigned long long cycles, volatile unsigned long long *hc)
{
    unsigned long long start = clock64(), i = 0;
    while (clock64() - start < cycles) { i++; if ((i & 0xffff) == 0 && hc) *hc = i; }
}

__global__ void tick(unsigned long long *out)
{
    if (out) *out = clock64();
}

static double now_s()
{
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    long blocks = argc > 1 ? atol(argv[1]) : 8;
    long victim_ms = argc > 2 ? atol(argv[2]) : 3000;

    int concurrent = 0, sms = 0, asyncEngine = 0;
    RT_CHECK(cudaFree(0));
    
    cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount, 0);
    cudaDeviceGetAttribute(&asyncEngine, cudaDevAttrAsyncEngineCount, 0);
    cudaDeviceProp prop;
    RT_CHECK(cudaGetDeviceProperties(&prop, 0));
    concurrent = prop.concurrentKernels;
    printf("[dev] %s SMs=%d concurrentKernels=%d asyncEngine=%d clock=%d kHz\n",
           prop.name, sms, concurrent, asyncEngine, prop.clockRate);

    unsigned long long cycles = (unsigned long long)prop.clockRate * 1000ULL * victim_ms / 1000ULL;
    unsigned long long *dev = nullptr, *hdev = nullptr;
    RT_CHECK(cudaMalloc(&dev, 16));
    RT_CHECK(cudaMemset(dev, 0, 16));

    cudaStream_t a, b;
    RT_CHECK(cudaStreamCreateWithFlags(&a, cudaStreamNonBlocking));
    RT_CHECK(cudaStreamCreateWithFlags(&b, cudaStreamNonBlocking));

    printf("\n[test 1] victim %ld blocks x 32 threads (~%ld ms) on stream A, trivial kernel on stream B\n",
           blocks, victim_ms);
    spin<<<(int)blocks, 32, 0, a>>>(cycles, nullptr);
    RT_CHECK(cudaGetLastError());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    double t0 = now_s();
    tick<<<1, 1, 0, b>>>(hdev);
    cudaError_t le = cudaGetLastError();
    cudaError_t sy = cudaStreamSynchronize(b);
    printf("[test 1] helper launch=%s sync=%s elapsed=%.3f s  (victim was %ld ms)\n",
           cudaGetErrorName(le), cudaGetErrorName(sy), now_s() - t0, victim_ms);
    RT_CHECK(cudaStreamSynchronize(a));

    printf("\n[test 2] repeat of test 1 (variance check)\n");
    spin<<<(int)blocks, 32, 0, a>>>(cycles, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    t0 = now_s();
    tick<<<1, 1, 0, b>>>(nullptr);
    sy = cudaStreamSynchronize(b);
    printf("[test 2] helper elapsed=%.3f s sync=%s\n", now_s() - t0, cudaGetErrorName(sy));
    RT_CHECK(cudaStreamSynchronize(a));

    printf("\n[test 3] victim with 1 block (leaves nearly all SMs free)\n");
    spin<<<1, 32, 0, a>>>(cycles, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    t0 = now_s();
    tick<<<1, 1, 0, b>>>(nullptr);
    sy = cudaStreamSynchronize(b);
    printf("[test 3] helper elapsed=%.3f s sync=%s\n", now_s() - t0, cudaGetErrorName(sy));
    RT_CHECK(cudaStreamSynchronize(a));

    printf("\n[test 4] no victim at all (reference)\n");
    t0 = now_s();
    tick<<<1, 1, 0, b>>>(nullptr);
    sy = cudaStreamSynchronize(b);
    printf("[test 4] helper elapsed=%.3f s sync=%s\n", now_s() - t0, cudaGetErrorName(sy));
    return 0;
}
