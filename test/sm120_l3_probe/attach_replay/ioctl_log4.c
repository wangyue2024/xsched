// v4b: robust memory dumper via /proc/self/mem pread (no signal tricks).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdint.h>
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static FILE *logf = NULL;
static int memfd = -1;
static void dumpmem(FILE *f, uint64_t addr, int n, const char *tag) {
    if (!addr) { return; }
    if (memfd < 0) memfd = open("/proc/self/mem", O_RDONLY);
    if (memfd < 0) { fprintf(f, " %s=noproc", tag); return; }
    unsigned char buf[256];
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    ssize_t got = pread(memfd, buf, n, (off_t)addr);
    if (got <= 0) {
        // /proc/self/mem returns EIO for unmapped but still readable maps differ; try up to n bytes around
        fprintf(f, " %s<%#llx>=ERR(%zd)", tag, (unsigned long long)addr, got);
        return;
    }
    fprintf(f, " %s<%#llx>=", tag, (unsigned long long)addr);
    for (int i = 0; i < got; ++i) fprintf(f, "%02x", buf[i]);
}
static int is_nvidia_fd(int fd) {
    char path[64], link[128];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, link, sizeof(link)-1);
    if (n <= 0) return 0; link[n]=0;
    return strstr(link, "nvidia") != NULL;
}
int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void*); va_end(ap);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (!is_nvidia_fd(fd)) return real_ioctl(fd, req, arg);
    if (!logf) { const char *p = getenv("IOCTL_LOG"); logf = fopen(p ? p : "/tmp/l3work/ioctl4.log", "w"); }
    if (!logf) return real_ioctl(fd, req, arg);
    unsigned char pre[512]; memset(pre, 0, sizeof pre);
    int size = (req>>16)&0x3fff; if (size > (int)sizeof(pre)) size = sizeof(pre);
    if (((req>>30)&3) != 0 && arg && size > 0) memcpy(pre, arg, size);
    int r = real_ioctl(fd, req, arg);
    if (req == 0xc030462b) {
        uint32_t hclass; uint64_t palloc;
        memcpy(&hclass, pre+12, 4); memcpy(&palloc, pre+16, 8);
        uint32_t post_hclass, post_ho; memcpy(&post_hclass, (char*)arg+12, 4); memcpy(&post_ho, (char*)arg+8, 4);
        fprintf(logf, "[alloc] fd=%d r=%d class=%#x palloc=%#llx post_hObj=%#x post_class=%#x", fd, r, hclass, (unsigned long long)palloc, post_ho, post_hclass);
        if (hclass == 0x83de && palloc) {
            uint64_t pD = 0, pA = 0, pB = 0, pC = 0;
            if (pread(memfd < 0 ? (memfd = open("/proc/self/mem", O_RDONLY)) : memfd, &pD, 8, palloc+0x40) != 8) pD = 0;
            char tmp[8];
            if (pread(memfd, tmp, 8, palloc+0x10) == 8) memcpy(&pA, tmp, 8);
            if (pread(memfd, tmp, 8, palloc+0x18) == 8) memcpy(&pB, tmp, 8);
            if (pread(memfd, tmp, 8, palloc+0x20) == 8) memcpy(&pC, tmp, 8);
            dumpmem(logf, palloc, 0x60, "p0");
            dumpmem(logf, pD, 0x60, "pD");
            dumpmem(logf, pA, 0x30, "pA");
            dumpmem(logf, pB, 0x30, "pB");
            dumpmem(logf, pC, 0x30, "pC");
        }
        fprintf(logf, "\n"); fflush(logf);
    } else if (req == 0xc020462a && arg) {
        unsigned int cmd; unsigned long long params; unsigned int psize;
        memcpy(&cmd, pre+8, 4); memcpy(&params, pre+16, 8); memcpy(&psize, pre+24, 4);
        uint32_t hC; memcpy(&hC, pre, 4);
        fprintf(logf, "[ctrl] fd=%d r=%d hC=%#x cmd=%#x psize=%u", fd, r, hC, cmd, psize);
        if (params && psize && psize <= 16384) dumpmem(logf, params, psize, "params");
        fprintf(logf, "\n"); fflush(logf);
    }
    return r;
}
