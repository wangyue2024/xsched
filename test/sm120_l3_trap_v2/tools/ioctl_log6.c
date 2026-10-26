// ioctl_log6.c - LD_PRELOAD RM ioctl interceptor with timestamps and full
// parameter decoding, for capturing what a *real* debugger suspend looks like.
//
// Improvements over v1's ioctl_log5 (test/sm120_l3_probe/attach_replay):
//   * monotonic + wall clock timestamps on every record (so the suspend moment
//     can be correlated with the gdb log / an external counter observer)
//   * NVOS21 (RM_ALLOC) decode: hRoot/hParent/hObject/hClass + pAllocParms dump
//     (v1 never captured the 0x83de session's alloc params - the suspected gap)
//   * NVOS54 (RM_CONTROL) decode: cmd/paramsSize + params dumped *after* the
//     call so out-params are visible
//   * NVOS00 (RM_FREE) decode
//   * explicit markers via IOCTL_MARK=<text> file writes are not needed: the
//     app's own writes appear as normal ioctls in order
//
// Build: gcc -shared -fPIC -O2 ioctl_log6.c -o ioctl_log6.so -ldl
// Use:   IOCTL_LOG=/path/out.log LD_PRELOAD=/path/ioctl_log6.so <prog>
//        (under cuda-gdb: set exec-wrapper env LD_PRELOAD=... IOCTL_LOG=...)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static long (*real_syscall)(long, ...) = NULL;
static FILE *logf = NULL;
static __thread int in_hook = 0;
static int memfd = -1;
static struct timespec t_start;
static int have_start = 0;

#define NV_ESC_RM_FREE          0xc0104629UL
#define NV_ESC_RM_CONTROL       0xc020462aUL
#define NV_ESC_RM_ALLOC         0xc030462bUL
#define NV_ESC_RM_MAP_MEMORY    0xc020464eUL
#define NV_ESC_RM_UNMAP_MEMORY  0xc020464fUL
#define NV_ESC_RM_ALLOC_MEMORY  0xc02c4627UL
#define NV_ESC_RM_DUP_OBJECT    0xc01c4634UL
#define NV_ESC_RM_VID_HEAP_CONTROL 0xc028464aUL

static double ts(void)
{
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    if (!have_start) { t_start = n; have_start = 1; }
    return (n.tv_sec - t_start.tv_sec) + (n.tv_nsec - t_start.tv_nsec) * 1e-9;
}

static void hexdump(FILE *f, uint64_t addr, int n, const char *tag)
{
    if (!addr || n <= 0) return;
    if (memfd < 0) memfd = open("/proc/self/mem", O_RDONLY);
    if (memfd < 0) { fprintf(f, " %s=noproc", tag); return; }
    unsigned char buf[1024];
    if (n > (int)sizeof buf) n = sizeof buf;
    ssize_t got = pread(memfd, buf, n, (off_t)addr);
    if (got <= 0) { fprintf(f, " %s<%#llx>=ERR", tag, (unsigned long long)addr); return; }
    fprintf(f, " %s<%#llx>=", tag, (unsigned long long)addr);
    for (ssize_t i = 0; i < got; i++) fprintf(f, "%02x", buf[i]);
}

static int is_nvidia_fd(int fd)
{
    char path[64], link[128];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n <= 0) return 0;
    link[n] = 0;
    if (strstr(link, "nvidia")) return 2;
    if (strstr(link, "/dev/") || strstr(link, "char/")) return 1;
    return 0;
}

static char fdpath[64][128];
static const char *fd_path(int fd)
{
    if (fd >= 0 && fd < 64) {
        if (!fdpath[fd][0]) {
            char p[64];
            snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
            ssize_t n = readlink(p, fdpath[fd], sizeof fdpath[fd] - 1);
            if (n <= 0) fdpath[fd][0] = '?', fdpath[fd][1] = 0;
            else fdpath[fd][n] = 0;
            fprintf(stderr, "[ioctl6] fd=%d -> %s\n", fd, fdpath[fd]);
        }
        return fdpath[fd];
    }
    return "?";
}

static const char *esc_name(unsigned long req)
{
    switch (req) {
    case NV_ESC_RM_FREE: return "RM_FREE";
    case NV_ESC_RM_CONTROL: return "RM_CONTROL";
    case NV_ESC_RM_ALLOC: return "RM_ALLOC";
    case NV_ESC_RM_MAP_MEMORY: return "RM_MAP_MEMORY";
    case NV_ESC_RM_UNMAP_MEMORY: return "RM_UNMAP_MEMORY";
    case NV_ESC_RM_ALLOC_MEMORY: return "RM_ALLOC_MEMORY";
    case NV_ESC_RM_DUP_OBJECT: return "RM_DUP_OBJECT";
    case NV_ESC_RM_VID_HEAP_CONTROL: return "RM_VID_HEAP_CONTROL";
    default: return "";
    }
}

static void log_call(const char *how, int fd, unsigned long req, void *arg, long r)
{
    int cls = is_nvidia_fd(fd);
    if (!cls) return;
    if (cls == 1 && !esc_name(req)[0]) return; // noise reduction on non-nvidia fds
    if (!logf) {
        const char *p = getenv("IOCTL_LOG");
        char path[512];
        if (p) snprintf(path, sizeof path, "%s.%d", p, (int)getpid());
        else   snprintf(path, sizeof path, "/tmp/ioctl6.%d.log", (int)getpid());
        logf = fopen(path, "w");
        if (!logf) return;
        setvbuf(logf, NULL, _IOLBF, 0);
    }
    int size = (req >> 16) & 0x3fff;
    fprintf(logf, "[t=%9.6f] %s fd=%d(%s) req=%#lx %s size=%d r=%ld",
            ts(), how, fd, fd_path(fd), req, esc_name(req), size, r);

    unsigned char raw[64];
    if (arg && size > 0 && size <= (int)sizeof raw) {
        if (memfd < 0) memfd = open("/proc/self/mem", O_RDONLY);
        if (pread(memfd, raw, size, (off_t)(uintptr_t)arg) == size) {
            if (req == NV_ESC_RM_CONTROL && size >= 32) {
                unsigned hC, hO, cmd, flags, psize;
                uint64_t params;
                int32_t status;
                memcpy(&hC, raw + 0, 4); memcpy(&hO, raw + 4, 4);
                memcpy(&cmd, raw + 8, 4); memcpy(&flags, raw + 12, 4);
                memcpy(&params, raw + 16, 8); memcpy(&psize, raw + 24, 4);
                memcpy(&status, raw + 28, 4);
                fprintf(logf, " hClient=%#x hObject=%#x cmd=%#x flags=%#x psize=%u status=%#x",
                        hC, hO, cmd, flags, psize, status);
                hexdump(logf, params, psize > 512 ? 512 : (int)psize, "params");
            } else if (req == NV_ESC_RM_ALLOC && size >= 48) {
                unsigned hRoot, hParent, hObj, hClass, psize, flags;
                uint64_t pAlloc, pRights;
                int32_t status;
                memcpy(&hRoot, raw + 0, 4); memcpy(&hParent, raw + 4, 4);
                memcpy(&hObj, raw + 8, 4); memcpy(&hClass, raw + 12, 4);
                memcpy(&pAlloc, raw + 16, 8); memcpy(&pRights, raw + 24, 8);
                memcpy(&psize, raw + 32, 4); memcpy(&flags, raw + 36, 4);
                memcpy(&status, raw + 40, 4);
                fprintf(logf, " hRoot=%#x hParent=%#x hObject=%#x hClass=%#x pAlloc=%#llx psize=%u flags=%#x status=%#x",
                        hRoot, hParent, hObj, hClass, (unsigned long long)pAlloc, psize, flags, status);
                fprintf(logf, " raw=");
                for (int i = 0; i < size && i < 48; i++) fprintf(logf, "%02x", raw[i]);
                /* psize==0 means "use the class default": RM still dereferences pAlloc,
                   so dump a fixed window to see what the caller actually passed. */
                int n = psize ? (int)psize : (pAlloc ? 64 : 0);
                hexdump(logf, pAlloc, n > 512 ? 512 : n, "alloc");
            } else if (req == NV_ESC_RM_DUP_OBJECT && size >= 28) {
                unsigned hRoot, hParent, hObj, hSrcClient, hSrcObj, flags;
                int32_t status;
                memcpy(&hRoot, raw + 0, 4); memcpy(&hParent, raw + 4, 4);
                memcpy(&hObj, raw + 8, 4); memcpy(&hSrcClient, raw + 12, 4);
                memcpy(&hSrcObj, raw + 16, 4); memcpy(&flags, raw + 20, 4);
                memcpy(&status, raw + 24, 4);
                fprintf(logf, " DUP hRoot=%#x hParent=%#x hObject=%#x hClientSrc=%#x hObjSrc=%#x flags=%#x status=%#x",
                        hRoot, hParent, hObj, hSrcClient, hSrcObj, flags, status);
            } else if (req == NV_ESC_RM_FREE && size >= 16) {
                unsigned hRoot, hParent, hObj;
                memcpy(&hRoot, raw + 0, 4); memcpy(&hParent, raw + 4, 4); memcpy(&hObj, raw + 8, 4);
                fprintf(logf, " hRoot=%#x hParent=%#x hObject=%#x", hRoot, hParent, hObj);
            } else {
                fprintf(logf, " raw=");
                for (int i = 0; i < size && i < 64; i++) fprintf(logf, "%02x", raw[i]);
            }
        }
    }
    fprintf(logf, "\n");
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (in_hook) return real_ioctl(fd, req, arg);
    in_hook = 1;
    int r = real_ioctl(fd, req, arg);
    log_call("ioctl", fd, req, arg, r);
    in_hook = 0;
    return r;
}

long syscall(long number, ...)
{
    va_list ap; va_start(ap, number);
    void *a1 = va_arg(ap, void *), *a2 = va_arg(ap, void *), *a3 = va_arg(ap, void *);
    va_end(ap);
    if (!real_syscall) real_syscall = dlsym(RTLD_NEXT, "syscall");
    if (in_hook) return real_syscall(number, a1, a2, a3);
    long r;
    if (number == 16 /* SYS_ioctl */) {
        in_hook = 1;
        r = real_syscall(number, a1, a2, a3);
        log_call("syscall", (int)(intptr_t)a1, (unsigned long)(uintptr_t)a2, a3, r);
        in_hook = 0;
    } else {
        r = real_syscall(number, a1, a2, a3);
    }
    return r;
}
