// rmdev.cpp - diagnostic: why does RM_ALLOC(class 0x80 NV01_DEVICE_0) return
// NV_ERR_INVALID_OBJECT_HANDLE (0x33) for a client we create ourselves?
//
// cuda-gdb's debug agent manages to do exactly this (evidence/live/r3_ioctl.log,
// pid 20144): SYS_PARAMS + CARD_INFO on a fresh /dev/nvidiactl, RM_ALLOC 0x41,
// a few client-level RM_CONTROLs, RM_ALLOC class 0xcb33, then RM_ALLOC 0x80 with
// params {hClientShare=0, hTargetClient=<own client>, rest 0}.
//
// This probe replays that prefix step by step and reports the status of each
// variant, so the missing prerequisite shows up as the first difference.
//
// Build: ../build.sh rmdev    Run: CUXTRA_CUDA_LIB=... ./rmdev

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <vector>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <cuda.h>

#define CU_CHECK(x) do { CUresult _r = (x); if (_r != CUDA_SUCCESS) { \
    const char *s = nullptr; cuGetErrorName(_r, &s); \
    fprintf(stderr, "[FATAL] %s -> %d %s @%d\n", #x, (int)_r, s ? s : "?", __LINE__); exit(1);} } while (0)

struct NVOS21 { uint32_t hRoot, hParent, hObject, hClass;
                uint64_t pAllocParms, pRightsRequested;
                uint32_t paramsSize, flags; int32_t status; };
struct NVOS54 { uint32_t hClient, hObject, cmd, flags;
                uint64_t params; uint32_t paramsSize; int32_t status; };
static_assert(sizeof(NVOS21) == 48 && sizeof(NVOS54) == 32, "RM ABI");

static const unsigned long NV_ESC_RM_ALLOC   = 0xc030462bUL;
static const unsigned long NV_ESC_RM_CONTROL = 0xc020462aUL;
static const unsigned long NV_ESC_SYS_PARAMS = 0xc00846d6UL;
static const unsigned long NV_ESC_CARD_INFO  = 0xc90046c8UL;

static const uint32_t C_ROOT = 0x41, C_DEVICE = 0x80, C_SUBDEV = 0x2080,
                      C_0xcb33 = 0xcb33, C_COMPUTE = 0xcec0, C_SMDBG = 0x83de;

struct Learned {
    int fd = -1;
    std::vector<uint32_t> clients;
    uint32_t device = 0, subdev = 0, compute = 0, app_client = 0, app_session = 0, cb33 = 0;
} L;

static int (*real_ioctl)(int, unsigned long, ...) = nullptr;
static __thread int in_hook = 0;

static void learn(int fd, void *arg)
{
    NVOS21 a; memcpy(&a, arg, sizeof a);
    if (a.status) return;
    switch (a.hClass) {
    case C_ROOT:    if (L.fd < 0) L.fd = fd; L.clients.push_back(a.hObject); break;
    case C_DEVICE:  if (!L.device) { L.device = a.hObject; L.app_client = a.hRoot; } break;
    case C_SUBDEV:  if (!L.subdev) L.subdev = a.hObject; break;
    case C_COMPUTE: if (!L.compute) L.compute = a.hObject; break;
    case C_SMDBG:   if (!L.app_session) L.app_session = a.hObject; break;
    case C_0xcb33:  if (!L.cb33) L.cb33 = a.hObject; break;
    }
}

extern "C" int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, req, arg);
    if (in_hook || r || !arg || req != NV_ESC_RM_ALLOC) return r;
    in_hook = 1; learn(fd, arg); in_hook = 0;
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

// one attempt at building client -> device; returns 0 on success
static int try_stack(const char *tag, int fd, bool cardinfo, bool sysparams, bool cb33,
                     bool ctl_prefix, uint32_t use_client, uint32_t dev_handle,
                     bool target_client, uint32_t psize_mode, uint32_t *out_client,
                     uint32_t *out_dev, int idx)
{
    uint32_t client = use_client;
    const uint32_t HCLI = 0xc1d0f001u + idx * 16, HCB = 0x5c00f001u + idx * 16;
    if (!client) {
        if (sysparams) { uint64_t mb = 0x08000000ULL; real_ioctl(fd, NV_ESC_SYS_PARAMS, &mb); }
        if (cardinfo)  { static char ci[2304]; real_ioctl(fd, NV_ESC_CARD_INFO, ci); }
        int st = rm_alloc(fd, 0, 0, HCLI, C_ROOT, nullptr, 0);
        if (st) { printf("  [%s] client -> %#x\n", tag, st); return st; }
        client = HCLI;
        if (ctl_prefix) {
            uint32_t p8 = 8;          rm_control(fd, client, client, 0x1f0, &p8, 4);
            uint32_t q[3] = {client, 1, 0x40000}; rm_control(fd, client, client, 0xd04, q, 12);
        }
        if (cb33) {
            uint8_t blob[64] = {0};
            int st2 = rm_alloc(fd, client, client, HCB, C_0xcb33, blob, 0);
            printf("  [%s] 0xcb33 -> %#x\n", tag, st2);
        }
    }
    uint32_t dp[16]; memset(dp, 0, sizeof dp);
    if (target_client) dp[1] = client;      // hTargetClient
    uint32_t psize = psize_mode == 0 ? 0 : psize_mode == 1 ? 56 : 4;
    int st = rm_alloc(fd, client, client, dev_handle, C_DEVICE, dp, psize);
    printf("  [%s] fd=%d client=%#x dev=%#x psize=%u hTargetClient=%d -> %#x\n",
           tag, fd, client, dev_handle, psize, target_client, st);
    if (out_client) *out_client = client;
    if (out_dev && !st) *out_dev = dev_handle;
    return st;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    CU_CHECK(cuInit(0));
    CUcontext ctx;
    CU_CHECK(cuDevicePrimaryCtxRetain(&ctx, 0));
    CU_CHECK(cuCtxSetCurrent(ctx));
    printf("[learn] fd=%d app_client=%#x device=%#x subdev=%#x compute=%#x app_session=%#x cb33=%#x\n",
           L.fd, L.app_client, L.device, L.subdev, L.compute, L.app_session, L.cb33);
    for (uint32_t c : L.clients) printf(" client %#x\n", c);

    int fresh = open("/dev/nvidiactl", O_RDWR);
    printf("[fd] fresh=%d libcuda=%d\n", fresh, L.fd);
    if (fresh < 0) { perror("open"); return 1; }

    uint32_t cl = 0, dv = 0;
    int fd_of_ok = fresh;
    struct A { const char *tag; int *fd; bool ci, sp, cb, cp; uint32_t cli, dev; bool tc; uint32_t pm; };
    A atts[] = {
        {"A fresh,no-init",       &fresh, false,false,false,false, 0,          0x5c00f002,false,0},
        {"B fresh,cardinfo",      &fresh, true, false,false,false, 0,          0x5c00f012,false,0},
        {"C fresh,+sysparams",    &fresh, true, true, false,false, 0,          0x5c00f022,false,0},
        {"D C+targetClient",      &fresh, false,false,false,false, 0,          0x5c00f032,true, 0},
        {"E D+cb33",              &fresh, false,false,true, false, 0,          0x5c00f042,true, 0},
        {"F E+ctl_prefix",        &fresh, false,false,true, true,  0,          0x5c00f052,true, 0},
        {"G libcuda-fd,new-cli",  &L.fd,  false,false,false,false, 0,          0x5c00f062,true, 0},
        {"H libcuda-cli,2nd-dev", &L.fd,  false,false,false,false, L.app_client,0x5c00f072,true, 0},
        {"I psize=56",            &fresh, false,false,false,false, 0,          0x5c00f082,true, 1},
        {"J psize=4",             &fresh, false,false,false,false, 0,          0x5c00f092,true, 2},
        {"K handle=0x5c000002",   &fresh, false,false,false,false, 0,          0x5c000002,true, 0},
    };
    for (size_t i = 0; i < sizeof atts / sizeof atts[0]; i++) {
        A &a = atts[i];
        uint32_t c2 = 0, d2 = 0;
        int st = try_stack(a.tag, *a.fd, a.ci, a.sp, a.cb, a.cp, a.cli, a.dev, a.tc, a.pm, &c2, &d2, (int)i);
        if (!st && !dv) { dv = d2; cl = c2; fd_of_ok = *a.fd; }
    }
    int fresh_used = fresh;
    fresh = fd_of_ok;
    (void)fresh_used;

    // If any stack came up, continue to subdevice + 0x83de on the last success.
    if (dv) {
        uint32_t sd = 0;
        uint32_t sp = 0;
        int st = rm_alloc(fresh, cl, dv, 0x5c00f003, C_SUBDEV, &sp, 0);
        printf("[sub] 0x2080 client=%#x dev=%#x -> %#x\n", cl, dv, st);
        if (!st) sd = 0x5c00f003;
        uint32_t p83[8] = {0, L.app_client, L.compute, 0, 0, 0, 0, 0};
        st = rm_alloc(fresh, cl, dv, 0x5c00f00c, C_SMDBG, p83, 0);
        printf("[ssn] 0x83de parent=dev params={0,%#x,%#x} -> %#x\n", L.app_client, L.compute, st);
        if (st && sd) {
            st = rm_alloc(fresh, cl, sd, 0x5c00f00d, C_SMDBG, p83, 0);
            printf("[ssn] 0x83de parent=subdev -> %#x\n", st);
        }
    }
    return 0;
}
