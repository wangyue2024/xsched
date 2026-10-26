// l3agent.cpp - external sm120 trap agent for XSched's level-3 trap backend.
//
// Topology:
//   target (XSched app)        agent (this process)
//     publishes RM handles  ->   waits for magic
//     applies 0xd04 share   <-   publishes its own client
//     patches live handler  ->   allocates 0x83de {app client, KGO}
//     trigger_req=1         ->   0317{0,0}, then ack
//     resume_req=1          ->   0318, then ack
//
// Build: ../build.sh l3agent
// Run:   ./bin/l3agent [shm-name]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <cuda.h>
#include <ctime>
#include <thread>
#include <atomic>

#include "xsched/cuda/hal/level3/trap_sm120_shm.h"


struct NVOS21 { uint32_t hRoot, hParent, hObject, hClass;
                uint64_t pAllocParms, pRightsRequested;
                uint32_t paramsSize, flags; int32_t status; };
struct NVOS54 { uint32_t hClient, hObject, cmd, flags;
                uint64_t params; uint32_t paramsSize; int32_t status; };
static_assert(sizeof(NVOS21) == 48 && sizeof(NVOS54) == 32, "RM ABI");

static const unsigned long NV_ESC_RM_ALLOC   = 0xc030462bUL;
static const unsigned long NV_ESC_RM_CONTROL = 0xc020462aUL;
static const uint32_t CLASS_ROOT = 0x41, CLASS_DEVICE = 0x80, CLASS_SUBDEV = 0x2080,
                      CLASS_COMPUTE = 0xcec0, CLASS_SMDBGSSN = 0x83de;

struct Learned { int fd = -1; uint32_t client = 0, device = 0, subdev = 0; } L;
static int (*real_ioctl)(int, unsigned long, ...) = nullptr;
static __thread int in_hook = 0;

extern "C" int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, req, arg);
    if (in_hook || r || !arg || req != NV_ESC_RM_ALLOC) return r;
    in_hook = 1;
    NVOS21 a; memcpy(&a, arg, sizeof a);
    if (a.status == 0) {
        if (a.hClass == CLASS_ROOT && L.fd < 0) L.fd = fd;
        if (a.hClass == CLASS_DEVICE && !L.device) { L.device = a.hObject; L.client = a.hRoot; L.fd = fd; }
        if (a.hClass == CLASS_SUBDEV && !L.subdev) L.subdev = a.hObject;
    }
    in_hook = 0;
    return r;
}

static int rm_alloc(int fd, uint32_t root, uint32_t parent, uint32_t obj, uint32_t cls,
                    void *params, uint32_t psize)
{
    NVOS21 a; memset(&a, 0, sizeof a);
    a.hRoot = root; a.hParent = parent; a.hObject = obj; a.hClass = cls;
    a.pAllocParms = (uint64_t)(uintptr_t)params; a.paramsSize = psize;
    int r = real_ioctl(fd, NV_ESC_RM_ALLOC, &a);
    return r ? (0x1000000 | r) : a.status;
}

static int rm_control(int fd, uint32_t client, uint32_t obj, uint32_t cmd, void *p, uint32_t n)
{
    NVOS54 c; memset(&c, 0, sizeof c);
    c.hClient = client; c.hObject = obj; c.cmd = cmd;
    c.params = (uint64_t)(uintptr_t)p; c.paramsSize = n;
    int r = real_ioctl(fd, NV_ESC_RM_CONTROL, &c);
    return r ? (0x1000000 | r) : c.status;
}

static int rm_free(int fd, uint32_t root, uint32_t parent, uint32_t obj)
{
    struct { uint32_t hRoot, hParent, hObject; int32_t status; } f;
    memset(&f, 0, sizeof f);
    f.hRoot = root; f.hParent = parent; f.hObject = obj;
    int r = real_ioctl(fd, 0xc0104629UL, &f);
    return r ? (0x1000000 | r) : f.status;
}

static double now_s()
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *shmname = argc > 1 ? argv[1] : L3_TRAP_SHM_NAME;

    int fd = -1;
    for (int i = 0; i < 500 && fd < 0; i++) {
        fd = shm_open(shmname, O_RDWR, 0666);
        if (fd < 0) usleep(20000);
    }
    if (fd < 0) { perror("shm_open"); return 1; }
    L3TrapShm *sh = (L3TrapShm *)mmap(nullptr, sizeof(L3TrapShm), PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, 0);
    close(fd);
    if (sh == MAP_FAILED) { perror("mmap"); return 1; }

    CUresult cir = cuInit(0);
    if (cir != CUDA_SUCCESS) { fprintf(stderr, "[agent] cuInit -> %#x\n", cir); return 1; }
    printf("[agent] pid=%d own fd=%d client=%#x device=%#x subdev=%#x\n",
           (int)getpid(), L.fd, L.client, L.device, L.subdev);
    if (!L.client || !L.device) { fprintf(stderr, "[agent] no RM stack\n"); return 1; }
    sh->agent_client = L.client;
    sh->agent_device = L.device;
    sh->agent_subdev = L.subdev;
    __sync_synchronize();
    sh->agent_ready = 1;

    // wait for the target to publish its handles (and for its 0xd04 share)
    for (int i = 0; i < 1000 && sh->magic != L3_TRAP_SHM_MAGIC; i++) usleep(20000);
    if (sh->magic != L3_TRAP_SHM_MAGIC) { fprintf(stderr, "[agent] target never published\n"); return 1; }
    printf("[agent] target pid=%u client=%#x device=%#x subdev=%#x KGO=%#x n=%u\n",
           sh->pid, sh->app_client, sh->device, sh->subdev,
           sh->ncompute ? sh->computes[0] : 0, sh->ncompute);

    bool ptrace_attached = false;
    if (!getenv("L3AGENT_NO_PTRACE")) {
        long pr = ptrace(PTRACE_ATTACH, (pid_t)sh->pid, nullptr, nullptr);
        if (pr == 0) {
            int wst = 0;
            waitpid((pid_t)sh->pid, &wst, 0);
            long cr = ptrace(PTRACE_CONT, (pid_t)sh->pid, nullptr, nullptr);
            ptrace_attached = true;
            printf("[agent] ptrace attach ok, cont=%ld\n", cr);
        } else {
            printf("[agent] ptrace attach failed: %s\n", strerror(errno));
        }
    }
    if (ptrace_attached && getenv("L3AGENT_DETACH_AFTER_MS")) {
        int ms = atoi(getenv("L3AGENT_DETACH_AFTER_MS"));
        pid_t tp = (pid_t)sh->pid;
        printf("[agent] will PTRACE_DETACH after %d ms\n", ms);
        std::thread([tp, ms]() {
            usleep(ms * 1000);
            long r = ptrace(PTRACE_DETACH, tp, nullptr, nullptr);
            printf("[agent] PTRACE_DETACH -> %ld (%s)\n", r, strerror(errno));
        }).detach();
    }

    // build the debugger session on the target's client + first KGO
    if (getenv("L3AGENT_DELAY_MS")) {
        int ms = atoi(getenv("L3AGENT_DELAY_MS"));
        printf("[agent] delaying session creation by %d ms\n", ms);
        usleep(ms * 1000);
    }
    uint32_t kgo = sh->ncompute ? sh->computes[0] : 0;
    const uint32_t H = 0x5c00e000;
    bool eager = getenv("L3AGENT_EAGER") != nullptr;

    auto create_session = [&]() -> int {
        struct { uint32_t hDebuggerClient, hAppClient, hClass3dObject; uint32_t pad[5]; } p83;
        memset(&p83, 0, sizeof p83);
        p83.hAppClient = sh->app_client;
        p83.hClass3dObject = kgo;
        int st = rm_alloc(L.fd, L.client, L.device, H, CLASS_SMDBGSSN, &p83, 0);
        sh->session = H;
        sh->session_status = st;
        __sync_synchronize();
        printf("[agent] session %#x -> %#x\n", H, st);
        if (st) return st;
        if (getenv("L3AGENT_SMD")) {
            unsigned char zbuf[0x48];
            memset(zbuf, 0, sizeof zbuf);
            int a0 = rm_control(L.fd, L.client, H, 0x83de0301, nullptr, 0);
            int a1 = rm_control(L.fd, L.client, H, 0x83de0301, zbuf, sizeof zbuf);
            printf("[agent] 0301 SMDebugModeEnable: psize0 -> %#x, psize0x48 -> %#x\n", a0, a1);
        }
        if (getenv("L3AGENT_0302")) {
            int a = rm_control(L.fd, L.client, H, 0x83de0302, nullptr, 0); // SMDebugModeDisable
            printf("[agent] 0302 SMDebugModeDisable -> %#x\n", a);
        }
        if (!getenv("L3AGENT_NO_MODES")) {
            uint32_t one = 1;
            rm_control(L.fd, L.client, H, 0x83de031f, &one, 4);
            rm_control(L.fd, L.client, H, 0x83de0307, &one, 4);
            rm_control(L.fd, L.client, H, 0x83de032a, &one, 4);
            printf("[agent] modes set\n");
        } else {
            printf("[agent] modes skipped (L3AGENT_NO_MODES)\n");
        }
        if (!getenv("L3AGENT_NO_WARMUP")) {
            // cuda-gdb's attach does more than polling: it sets the exception
            // mask and reads the SM error states.
            uint32_t mask = 0;
            int s309 = rm_control(L.fd, L.client, H, 0x83de0309, &mask, 4);
            static unsigned char smbuf[0x12d8];
            int s30c = rm_control(L.fd, L.client, H, 0x83de030c, smbuf, sizeof smbuf);
            printf("[agent] 0309=%#x 030c=%#x\n", s309, s30c);
            uint64_t z[2] = {0, 0};
            for (int i = 0; i < 3; i++) {
                int s1 = rm_control(L.fd, L.client, H, 0x83de0317, z, 8);
                usleep(10000);
                int s2 = rm_control(L.fd, L.client, H, 0x83de0318, nullptr, 0);
                printf("[agent] warmup %d: 0317{0,0}=%#x 0318=%#x\n", i, s1, s2);
            }
        }
        return 0;
    };
    auto free_session = [&]() {
        int st = rm_free(L.fd, L.client, L.device, H);
        printf("[agent] free session %#x -> %#x\n", H, st);
        sh->session = 0;
        sh->session_status = 0;
    };

    if (eager) {
        create_session();
        if (getenv("L3AGENT_SETUP")) {
            // "cmd[:size]" -- send one zero-filled control on the session
            // before serving triggers (sweep of driver entry points).
            char spec[64]; snprintf(spec, sizeof spec, "%s", getenv("L3AGENT_SETUP"));
            char *colon = strchr(spec, ':');
            uint32_t csize = 0;
            if (colon) { *colon = 0; csize = (uint32_t)strtoul(colon + 1, nullptr, 0); }
            uint32_t cmd = (uint32_t)strtoul(spec, nullptr, 0);
            static unsigned char zbuf[0x2000];
            memset(zbuf, 0, sizeof zbuf);
            if (csize > sizeof zbuf) csize = sizeof zbuf;
            int st = rm_control(L.fd, L.client, H, cmd, csize ? (void *)zbuf : nullptr, csize);
            printf("[agent] setup %#x size %u -> %#x\n", cmd, csize, st);
        }
        __sync_synchronize();
        sh->session_ready = 1;
        printf("[agent] session ready (eager), waiting for triggers\n");
    } else {
        printf("[agent] lazy session mode, waiting for requests\n");
    }

    uint32_t last_trig = 0, last_res = 0, last_sreq = 0, last_sfree = 0;
    bool pump = getenv("L3AGENT_RESUME_PUMP") != nullptr;
    uint32_t pump_n = 0;
    double pump_t0 = now_s();
    while (true) {
        if (!eager && sh->session_req != last_sreq) {
            last_sreq = sh->session_req;
            create_session();
            __sync_synchronize();
            sh->session_ready = last_sreq;
        }
        if (!eager && sh->session_free_req != last_sfree) {
            last_sfree = sh->session_free_req;
            free_session();
            __sync_synchronize();
            sh->session_free_ack = last_sfree;
        }
        if (pump) {
            int s = rm_control(L.fd, L.client, H, 0x83de0318, nullptr, 0);
            if (++pump_n % 200 == 0)
                printf("[agent] resume pump %u last=%#x (%.2fs)\n", pump_n, s, now_s() - pump_t0);
        }
        if (sh->trigger_req != last_trig) {
            last_trig = sh->trigger_req;
            uint64_t z[2] = {getenv("L3AGENT_HOLD") ? 1ULL : 0ULL, 0};
            uint32_t gen = last_trig;
            // 0317 can block until the context is resumed; run it on its own
            // thread so the free requests below keep being served.
            printf("[agent] sending 0317{%llu,0}\n", (unsigned long long)z[0]);
            std::thread([&, gen, z]() {
                int s = rm_control(L.fd, L.client, H, 0x83de0317, (void *)z, 8);
                printf("[agent] 0317{%llu,0} -> %#x\n", (unsigned long long)z[0], s);
                sh->trigger_status = s;
                __sync_synchronize();
                sh->trigger_ack = gen;
            }).detach();
            if (getenv("L3AGENT_AUTO_RESUME")) {
                std::thread([&]() {
                    usleep(20000);
                    int r = rm_control(L.fd, L.client, H, 0x83de0318, nullptr, 0);
                    printf("[agent] auto 0318 -> %#x\n", r);
                }).detach();
            }
        }
        if (sh->resume_req != last_res) {
            last_res = sh->resume_req;
            if (getenv("L3AGENT_RESUME_POLL")) {
                uint64_t z[2] = {0, 0};
                int p = rm_control(L.fd, L.client, H, 0x83de0317, z, 8);
                printf("[agent] resume poll 0317{0,0} -> %#x\n", p);
            }
            int s = rm_control(L.fd, L.client, H, 0x83de0318, nullptr, 0);
            sh->resume_status = s;
            __sync_synchronize();
            sh->resume_ack = last_res;
            printf("[agent] 0318 -> %#x\n", s);
        }
        usleep(500);
    }
    return 0;
}
