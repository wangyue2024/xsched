// shm_watch.c - external observer for shm_spin's counter (no CUDA, no ptrace).
//
// Prints counter + monotonic timestamp + inter-sample delta every N ms so a
// device freeze is visible even while the producer process is stopped in a
// debugger.  Usage: shm_watch /sm120_l3_ctl [interval_ms] [seconds]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    const char *shm = argc > 1 ? argv[1] : "/sm120_l3_ctl";
    long interval_ms = argc > 2 ? atol(argv[2]) : 100;
    long seconds = argc > 3 ? atol(argv[3]) : 60;
    setvbuf(stdout, NULL, _IONBF, 0);

    int fd = shm_open(shm, O_RDONLY, 0);
    if (fd < 0) { perror("shm_open"); return 1; }
    void *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    volatile unsigned long long *ctl = (volatile unsigned long long *)p;

    double t0 = now_s(), last_t = t0;
    unsigned long long last = ctl[0];
    printf("[watch] shm=%s pid=%llu interval=%ldms\n", shm, ctl[4], interval_ms);
    while (now_s() - t0 < seconds) {
        usleep(interval_ms * 1000);
        unsigned long long c = ctl[0];
        double t = now_s();
        printf("[watch] t=%7.3f counter=%-14llu delta=%-12llu rate=%6.1f M/s done=%llu stop=%llu\n",
               t - t0, c, c - last, (c - last) / 1e6 / (t - last_t + 1e-9), ctl[1], ctl[5]);
        last = c; last_t = t;
        if (ctl[1]) { printf("[watch] kernel completed\n"); break; }
    }
    return 0;
}
