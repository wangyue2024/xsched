// latency.cpp - decompose the level-3 suspend latency.
//
// Prints absolute CLOCK_MONOTONIC timestamps so an external, higher-resolution
// observer of the kernel's progress counter can tell exactly when the device
// actually stopped.  (The sampler cannot live in this process: -target-attach
// stops every thread of the target, including a local sampler.)
//
// Build: see tools/measure_latency.sh

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include "xsched/cuda/hal/level3/gdb_interrupt.h"

using namespace xsched::cuda;

static double Now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    pid_t pid = -1;
    int hold_ms = 600;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pid") && i + 1 < argc) pid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc) hold_ms = atoi(argv[++i]);
    }
    if (pid <= 0) { fprintf(stderr, "usage: %s --pid N [--hold-ms MS]\n", argv[0]); return 2; }

    auto &ctx = GdbInterruptContext::Instance();

    // Pre-warm so the measurement only contains the attach + device-quiesce path.
    double tp0 = Now();
    bool prepped = ctx.Prepare(pid);
    printf("PREPARE %.4f %.4f\n", tp0, Now());

    usleep(200000);                      // let the target settle

    double t0 = Now();
    bool ok = ctx.Suspend(pid);
    double t1 = Now();
    printf("SUSPEND %.4f %.4f ok=%d\n", t0, t1, (int)ok);

    usleep((useconds_t)hold_ms * 1000);

    double t2 = Now();
    bool rok = ctx.Resume(pid);
    printf("RESUME %.4f %.4f ok=%d\n", t2, Now(), (int)rok);

    (void)prepped;
    return 0;
}
