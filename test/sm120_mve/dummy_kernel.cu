// =====================================================================
//  dummy_kernel.cu - T6 MVE workload kernel (sm_120)
//
//  Design constraints (see DESIGN.md §3):
//    * plain compilation (no astoolspatch): stays a launchable entry
//      function, exactly what the guardian splice mechanism consumes;
//    * NO device-scope globals, NO printf, NO cuda runtime calls: the
//      kernel image grabbed by cuXtraGetBinary must be free of absolute
//      addresses so that relocating it behind the guardian only shifts
//      relative branch targets (which stay constant);
//    * deterministic output as a function of (bid, spin_iters) so the
//      host can bit-compare instrumented vs. baseline runs;
//    * one block = one independent unit: thread (0,0,0) writes the
//      completion marker after the busy loop.
//
//  Layout of out[]: 2048 * 8 bytes, one u64 per block:
//      out[bid] = 0xA5A5A5A500000000 ^ (bid << 32) ^ acc(bid, iters)
//  Unwritten blocks keep their host-side 0xCC... pre-fill.
// =====================================================================

extern "C" __global__ void mve_kernel(unsigned long long *out, int spin_iters)
{
    unsigned int bid = blockIdx.x * gridDim.y * gridDim.z
                     + blockIdx.y * gridDim.z
                     + blockIdx.z;

    // deterministic busy work: an LCG chain (data-dependent, cannot be
    // optimized away because `acc` feeds the final store)
    unsigned long long acc = 0x9E3779B97F4A7C15ull ^ (unsigned long long)bid;
    for (int i = 0; i < spin_iters; ++i) {
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        acc ^= (acc >> 29);
    }

    if ((threadIdx.x | threadIdx.y | threadIdx.z) == 0) {
        out[bid] = 0xA5A5A5A500000000ull
                 ^ ((unsigned long long)bid << 32)
                 ^ acc;
    }
}
