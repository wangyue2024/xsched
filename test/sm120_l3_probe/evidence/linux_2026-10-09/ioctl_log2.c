// v2: also captures RM_CONTROL params buffers (0x83de class) + post-call buffers.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/ioctl.h>
#include <stdint.h>
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static FILE *logf = NULL;
static sigjmp_buf jb;
static void segv_h(int s){ siglongjmp(jb, 1); }
static void safe_hex(FILE *f, const unsigned char *p, int n, const char *tag) {
    struct sigaction sa, old; memset(&sa,0,sizeof sa); sa.sa_handler = segv_h; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old); sigaction(SIGBUS, &sa, &old);
    if (sigsetjmp(jb, 1) == 0) {
        fprintf(f, " %s=", tag);
        for (int i = 0; i < n; ++i) fprintf(f, "%02x", p[i]);
    } else { fprintf(f, " %s=UNREADABLE", tag); }
    sigaction(SIGSEGV, &old, NULL); sigaction(SIGBUS, &old, NULL);
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
    if (!logf) { const char *p = getenv("IOCTL_LOG"); logf = fopen(p ? p : "/tmp/l3work/ioctl2.log", "w"); }
    unsigned char pre[256]; memset(pre, 0, sizeof pre);
    int size = (req>>16)&0x3fff; if (size > 256) size = 256;
    if (((req>>30)&3) != 0 && arg) memcpy(pre, arg, size);
    int r = real_ioctl(fd, req, arg);
    fprintf(logf, "[ioctl] req=%#lx dir=%lu size=%lu r=%d", req, (req>>30)&3, (req>>16)&0x3fff, r);
    if (((req>>30)&3) != 0 && arg && size) safe_hex(logf, pre, size, "in");
    // params capture for RM_CONTROL
    if (req == 0xc020462a && pre[10] == 0xde && pre[11] == 0x83) { // cmd low bytes: 0x83de03xx LE
        unsigned int cmd = pre[8] | (pre[9]<<8) | (pre[10]<<16) | (pre[11]<<24);
        unsigned long long params = 0; memcpy(&params, pre+16, 8);
        unsigned int psize = 0; memcpy(&psize, pre+24, 4);
        fprintf(logf, " cmd=%#x params_psize=%u", cmd, psize);
        if (params && psize && psize <= 8192) safe_hex(logf, (const unsigned char*)(uintptr_t)params, psize, "params");
    }
    fprintf(logf, "\n"); fflush(logf);
    return r;
}
