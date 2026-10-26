// dump_handler.cu - dump the live trap handler and confirm the sm86 patch site.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuxtra/cuxtra.h>

int main()
{
    cudaFree(0);
    CUcontext ctx = nullptr;
    CUresult r = cuCtxGetCurrent(&ctx);
    if (r != CUDA_SUCCESS || ctx == nullptr) { fprintf(stderr, "no ctx %d\n", (int)r); return 1; }
    CUdeviceptr dev = 0;
    size_t size = 0;
    cuXtraGetTrapHandlerInfo(ctx, &dev, &size);
    printf("[handler] dev=%#llx size=%#zx\n", (unsigned long long)dev, size);
    if (dev == 0 || size == 0) return 1;
    void *buf = malloc(size);
    size_t got = cuXtraMemcpyDtoH(buf, dev, size, 0);
    printf("[dump] got=%zu\n", got);
    if (got == 0) return 1;
    FILE *f = fopen("sm86_handler.bin", "wb");
    if (f) { fwrite(buf, 1, size, f); fclose(f); printf("[dump] sm86_handler.bin\n"); }
    unsigned char *p = (unsigned char *)buf;
    printf("[bytes] ");
    for (int i = 0; i < 32 && i < (int)size; i++) printf("%02x", p[i]);
    printf("\n[+0x3e0] ");
    for (int i = 0x3e0; i < 0x3f0 && i < (int)size; i++) printf("%02x", p[i]);
    printf("\n");
    // print the 64-bit words at +0x3e0
    uint64_t w0 = 0, w1 = 0;
    if (size >= 0x3f0) { memcpy(&w0, p + 0x3e0, 8); memcpy(&w1, p + 0x3e8, 8); }
    printf("[+0x3e0 words] %016llx %016llx\n",
           (unsigned long long)w0, (unsigned long long)w1);
    return 0;
}
