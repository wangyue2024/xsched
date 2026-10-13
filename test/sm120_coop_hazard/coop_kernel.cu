/*
 * =====================================================================
 *  coop_kernel.cu - cooperative-style hazard kernels for XSched Level-2
 *
 *  WHY THIS FILE EXISTS
 *  --------------------
 *  A kernel launched as a cooperative launch may synchronize ACROSS its
 *  whole grid (cooperative_groups::this_grid().sync()).  XSched Level-2
 *  intercepts kernels at an ENTRY CHECKPOINT ("guardian"): when the
 *  global exit flag is set, a block that has not passed the checkpoint
 *  EXITs immediately (and is marked for replay).  If the flag lands
 *  while only SOME blocks have passed the checkpoint, the grid becomes
 *  PARTIAL: the blocks that entered the body wait at the grid barrier
 *  for peers that exited and will never arrive.
 *
 *  A real grid.sync() has no timeout -> permanent deadlock.
 *  This test kernel therefore uses a TIME-BOUNDED sense-reversing grid
 *  barrier with identical arrival semantics: every block must arrive,
 *  but waiters give up after a deadline, record a diagnostic, and exit.
 *  That converts the would-be deadlock into OBSERVABLE data while
 *  guaranteeing the GPU cannot hang.
 *
 *  ABI (must match coop_main.cpp)
 *  ------------------------------
 *  coop_kernel params:
 *      (u64* out,                       // out[block] written after barrier
 *       int  spin,                      // busy workload after barrier
 *       u32* barrier_state,             // [0] arrivals, [1] generation
 *       u32* diag,                      // [0] entry cnt, [1] timeout cnt,
 *                                       // [2] synced cnt
 *       u64* entry_ns,                  // per-block %globaltimer at entry
 *       u64  timeout_ns)                // barrier deadline
 *  setflag_kernel params ("Deactivate" amplifier):
 *      (u64* entry_ns, u32* global_flag, u64 dwell_ns, u64* set_ns,
 *       u64  wait_cap_ns)
 *      waits for the first body-entry marker, dwells `dwell_ns`, then
 *      writes global_flag = 1 (the same effect as the XSched Deactivate
 *      memset, but timed from inside the GPU); gives up after
 *      `wait_cap_ns` for safety.
 * =====================================================================
 */

#include <cstdint>

__device__ __forceinline__ unsigned long long gtimer_ns()
{
    unsigned long long t;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
    return t;
}

/* Bounded sense-reversing grid barrier; thread 0 of every block calls it.
 * Returns 0 = released normally, 1 = deadline expired (missing peers). */
__device__ int bounded_grid_barrier(unsigned int *arrive_count,
                                    unsigned int *generation,
                                    unsigned int nblocks,
                                    unsigned long long timeout_ns)
{
    unsigned int gen = *(volatile unsigned int *)generation;
    __threadfence();
    unsigned int prev = atomicAdd(arrive_count, 1u);
    if (prev == nblocks - 1u) {
        /* last arriver: reset and flip the generation (full grid arrived) */
        *arrive_count = 0u;
        __threadfence();
        *(volatile unsigned int *)generation = gen + 1u;
        __threadfence();
        return 0;
    }
    /* wait for the flip, but never longer than the deadline */
    unsigned long long t0 = gtimer_ns();
    while (*(volatile unsigned int *)generation == gen) {
        if (gtimer_ns() - t0 > timeout_ns) return 1; /* missing peers */
        __nanosleep(2000);
    }
    return 0;
}

extern "C" __global__ void coop_kernel(unsigned long long *out,
                                       int spin,
                                       unsigned int *barrier_state,
                                       unsigned int *diag,
                                       unsigned long long *entry_ns,
                                       unsigned long long timeout_ns)
{
    unsigned int tid = threadIdx.x;
    unsigned long long t;

    if (tid == 0) {
        t = gtimer_ns();
        atomicExch(&entry_ns[blockIdx.x], t); /* body-entry marker */
        atomicAdd(&diag[0], 1u);              /* entered-block counter */
    }

    __shared__ unsigned int shared_rc;
    if (tid == 0) {
        shared_rc = (unsigned int)bounded_grid_barrier(
            &barrier_state[0], &barrier_state[1],
            gridDim.x * gridDim.y * gridDim.z, timeout_ns);
        if (shared_rc == 1) atomicAdd(&diag[1], 1u); /* timed out: peers missing */
        else                atomicAdd(&diag[2], 1u); /* released: full grid */
    }
    __syncthreads();

    /* small deterministic workload so state is stable before we read it */
    unsigned long long acc = blockIdx.x + 1u;
    for (int i = 0; i < spin; ++i)
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;

    if (tid == 0)
        out[blockIdx.x] = 0x1111111100000000ull | (acc & 0xffffffffull);
}

/* Companion ("Deactivate amplifier"): runs concurrently on a second stream.
 * Waits for the FIRST coop block to enter the body, dwells a sweepable
 * amount of time, then sets the global exit flag -- landing the flag
 * INSIDE the entry window of the still-dispatching cooperative grid. */
extern "C" __global__ void setflag_kernel(unsigned long long *entry_ns,
                                          unsigned int *global_flag,
                                          unsigned long long dwell_ns,
                                          unsigned long long *set_ns,
                                          unsigned long long wait_cap_ns)
{
    unsigned long long t0 = gtimer_ns();
    while (*(volatile unsigned long long *)&entry_ns[0] == 0ull) {
        if (gtimer_ns() - t0 > wait_cap_ns) return; /* safety exit */
    }
    unsigned long long t1 = gtimer_ns();
    while (gtimer_ns() - t1 < dwell_ns) { } /* tight spin: ~100 ns precision */
    atomicExch(&set_ns[0], gtimer_ns());
    atomicExch(global_flag, 1u); /* == InstrumentManager::Deactivate */
}
