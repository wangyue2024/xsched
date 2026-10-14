// v5: hook ioctl() AND syscall() (SYS_ioctl) to catch raw-syscall ioctls.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <stdint.h>
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static long (*real_syscall)(long, ...) = NULL;
static FILE *logf = NULL;
static __thread int in_hook = 0;
static int memfd = -1;
static void dumpmem(FILE *f, uint64_t addr, int n, const char *tag) {
    if (!addr) return;
    if (memfd < 0) memfd = open("/proc/self/mem", O_RDONLY);
    if (memfd < 0) { fprintf(f, " %s=noproc", tag); return; }
    unsigned char buf[1024];
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    ssize_t got = pread(memfd, buf, n, (off_t)addr);
    if (got <= 0) { fprintf(f, " %s<%#llx>=ERR", tag, (unsigned long long)addr); return; }
    fprintf(f, " %s<%#llx>=", tag, (unsigned long long)addr);
    for (int i = 0; i < got; ++i) fprintf(f, "%02x", buf[i]);
}
static int is_nvidia_fd(int fd) {
    char path[64], link[128];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, link, sizeof(link)-1);
    if (n <= 0) return 0; link[n]=0;
    if (strstr(link, "nvidia")) return 2;
    /* catch alternate device paths for the same device (e.g. /dev/char/195:...) */
    if (strstr(link, "/dev/") || strstr(link, "char/")) return 1;
    return 0;
}
static void log_call(const char* how, int fd, unsigned long req, void* arg, long r) {
    int cls = is_nvidia_fd(fd);
    if (!cls) return;
    // class 1 (non-nvidia /dev): only log RM-class requests to cut noise
    if (cls == 1 && req != 0xc030462b && req != 0xc020462a && req != 0xc0104629 && req != 0xc01c4634 && req != 0xc00446c9)
        return;
    if (!logf) { const char *p = getenv("IOCTL_LOG"); logf = fopen(p ? p : "/tmp/l3work/ioctl5.log", "w"); }
    if (!logf) return;
    if (!((req>>30)&3) && req != 0x46 && req != 0x17 && req != 0x21 && req != 0x49 && req != 0x25 && req != 0x27) return;
    int size = (req>>16)&0x3fff;
    { char path[64], link[128]; snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
      ssize_t n2 = readlink(path, link, sizeof(link)-1); if (n2>0){link[n2]=0;} else {link[0]=0;}
      fprintf(logf, "[%s] fd=%d(%s) req=%#lx size=%d r=%ld", how, fd, link, req, size, r); }
    if (arg && size > 0) dumpmem(logf, (uint64_t)(uintptr_t)arg, size > 512 ? 512 : size, "pre");
    if (req == 0xc020462a && arg) {
        unsigned int cmd; unsigned long long params; unsigned int psize;
        char tmp[32];
        if (memfd < 0) memfd = open("/proc/self/mem", O_RDONLY);
        if (pread(memfd, tmp, 32, (off_t)(uintptr_t)arg) == 32) {
            memcpy(&cmd, tmp+8, 4); memcpy(&params, tmp+16, 8); memcpy(&psize, tmp+24, 4);
            fprintf(logf, " cmd=%#x psize=%u", cmd, psize);
        }
    }
    fprintf(logf, "\n"); fflush(logf);
}
int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void*); va_end(ap);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (in_hook) return real_ioctl(fd, req, arg);
    in_hook = 1;
    int r = real_ioctl(fd, req, arg);
    log_call("ioctl", fd, req, arg, r);
    in_hook = 0;
    return r;
}
long syscall(long number, ...) {
    va_list ap; va_start(ap, number);
    void *a1 = va_arg(ap, void*); void *a2 = va_arg(ap, void*); void *a3 = va_arg(ap, void*);
    va_end(ap);
    if (!real_syscall) real_syscall = dlsym(RTLD_NEXT, "syscall");
    if (in_hook) return real_syscall(number, a1, a2, a3);
    long r;
    if (number == 16 /*SYS_ioctl*/) {
        in_hook = 1;
        r = real_syscall(number, a1, a2, a3);
        log_call("syscall", (int)(intptr_t)a1, (unsigned long)(uintptr_t)a2, a3, r);
        in_hook = 0;
    } else {
        r = real_syscall(number, a1, a2, a3);
    }
    return r;
}
