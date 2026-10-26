// l3hal.cpp - end-to-end test of the XSched level-3 GDB backend
// (platforms/cuda/hal/src/level3/gdb_interrupt.cpp).
//
// Scenario: a hog kernel occupies the whole GPU; a tiny high-priority kernel is
// launched; the scheduler asks GdbInterruptContext to suspend the hog, the tiny
// kernel finishes, then the hog is resumed.  Same measurement as
// tools/handover_test.sh, but driven through the XSched HAL API.
//
// Build: see run_l3hal.sh (links /tmp/xsbuild/platforms/cuda/libhalcuda.so)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xsched/cuda/hal/level3/gdb_interrupt.h"

using namespace xsched::cuda;

static double Now()
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static pid_t SpawnSpin(const char *shm, const char *seconds, const char *blocks, const char *threads)
{
    pid_t pid = fork();
    if (pid == 0) {
        setenv("L3_ALLOW_PTRACE", "1", 1);
        setenv("L3_BLOCKS", blocks, 1);
        setenv("L3_THREADS", threads, 1);
        execlp("shm_spin", "shm_spin", shm, seconds, nullptr);
        // fall back to the build directory used by the test scripts
        execl("./bin/shm_spin", "shm_spin", shm, seconds, nullptr);
        _exit(127);
    }
    return pid;
}

static bool WaitForShm(const char *name, double timeout_s)
{
    std::string path = std::string("/dev/shm") + name;
    double t0 = Now();
    while (Now() - t0 < timeout_s) {
        if (access(path.c_str(), F_OK) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool do_suspend = (argc > 1 && !strcmp(argv[1], "--suspend"));

    printf("[l3hal] available=%d\n", (int)GdbInterruptContext::IsAvailable());
    if (!GdbInterruptContext::IsAvailable()) return 2;

    unlink("/dev/shm/l3hal_a"); unlink("/dev/shm/l3hal_b");
    pid_t a = SpawnSpin("/l3hal_a", "20", "20000", "256");
    if (!WaitForShm("/l3hal_a", 10.0) || a <= 0) { fprintf(stderr, "[l3hal] hog failed\n"); return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    printf("[l3hal] hog pid=%d running\n", (int)a);

    // Pre-warm the agent so the measured Suspend() only pays for the attach.
    if (do_suspend) {
        double tp = Now();
        bool ok = GdbInterruptContext::Instance().Prepare(a);
        printf("[l3hal] Prepare(hog) = %d  (%.3f s, off the critical path)\n",
               (int)ok, Now() - tp);
    }

    double t0 = Now();
    pid_t b = SpawnSpin("/l3hal_b", "0", "1", "32");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bool suspended = false;
    if (do_suspend) {
        suspended = GdbInterruptContext::Instance().Suspend(a);
        printf("[l3hal] Suspend(hog) = %d  (%.3f s)\n", (int)suspended, Now() - t0);
    }

    int status = 0;
    double tb = Now();
    waitpid(b, &status, 0);
    printf("[l3hal] high-priority kernel finished after %.2f s%s\n",
           Now() - t0, do_suspend ? "  (with hog suspended)" : "  (hog running)");

    if (suspended) {
        bool ok = GdbInterruptContext::Instance().Resume(a);
        printf("[l3hal] Resume(hog) = %d\n", (int)ok);
    }
    kill(a, SIGKILL);
    waitpid(a, nullptr, 0);
    unlink("/dev/shm/l3hal_a"); unlink("/dev/shm/l3hal_b");
    (void)tb;
    return 0;
}
