// ioctl_log3.c — v3 RM ioctl interceptor for the L3 attach-replay round.
// 新增（相对 v2）：
//   1) RM_ALLOC(0xc030462b): dump pAlloc 参数缓冲内容（会话对象的绑定参数就在这里！）
//   2) RM_CONTROL(0xc020462a): dump 所有命令的 params（不再只抓 0x83de）
//   3) 每次调用输出 pre 与 post 状态（捕获驱动写回的 hObject/status 字段）
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
    if (!logf) { const char *p = getenv("IOCTL_LOG"); logf = fopen(p ? p : "/tmp/l3work/ioctl3.log", "w"); }
    if (!logf) return real_ioctl(fd, req, arg);

    unsigned char pre[512]; memset(pre, 0, sizeof pre);
    int size = (req>>16)&0x3fff; if (size > (int)sizeof(pre)) size = sizeof(pre);
    if (((req>>30)&3) != 0 && arg && size > 0) memcpy(pre, arg, size);

    int r = real_ioctl(fd, req, arg);

    fprintf(logf, "[ioctl] fd=%d req=%#lx dir=%lu size=%lu r=%d", fd, req, (req>>30)&3, (req>>16)&0x3fff, r);
    if (((req>>30)&3) != 0 && arg && size > 0) safe_hex(logf, pre, size, "pre");
    // post (driver 写回后的 512B 内内容)
    if (((req>>30)&3) == 3 && arg && size > 0) safe_hex(logf, (unsigned char*)arg, size > 512 ? 512 : size, "post");

    if (req == 0xc020462a && arg) {
        unsigned int cmd = 0; unsigned long long params = 0; unsigned int psize = 0;
        memcpy(&cmd, pre+8, 4); memcpy(&params, pre+16, 8); memcpy(&psize, pre+24, 4);
        fprintf(logf, " cmd=%#x psize=%u", cmd, psize);
        if (params && psize && psize <= 16384) safe_hex(logf, (const unsigned char*)(uintptr_t)params, psize, "params");
    }
    if (req == 0xc030462b && arg) {  // RM_ALLOC: pAlloc @ +0x10
        unsigned long long palloc = 0; unsigned int hclass = 0;
        memcpy(&hclass, pre+12, 4); memcpy(&palloc, pre+16, 8);
        fprintf(logf, " alloc_class=%#x palloc=%#llx", hclass, palloc);
        if (palloc) safe_hex(logf, (const unsigned char*)(uintptr_t)palloc, 256, "palloc");
    }
    fprintf(logf, "\n"); fflush(logf);
    return r;
}
