// arm_modes.so — 在 0x83de 会话被创建时立即武装调试模式（031f/0307/032a=1）
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static int armed = 0;
struct NVOS54 { uint32_t hClient, hObject, cmd, flags; uint64_t params; uint32_t paramsSize, status; };
static void fire(int fd, uint32_t hC, uint32_t hO, uint32_t cmd) {
    uint32_t one = 1;
    struct NVOS54 r = { hC, hO, cmd, 0, (uint64_t)(uintptr_t)&one, 4, 0 };
    int rc = real_ioctl(fd, 0xc020462aUL, &r);
    fprintf(stderr, "[arm_modes] %#x rc=%d status=%#x\n", cmd, rc, r.status);
}
int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void* arg = va_arg(ap, void*); va_end(ap);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    int r = real_ioctl(fd, req, arg);
    if (req == 0xc030462b && arg && !armed) {
        uint32_t hClass; memcpy(&hClass, (char*)arg+12, 4);
        if (hClass == 0x83de) {
            uint32_t hClient; memcpy(&hClient, (char*)arg, 4);
            uint32_t hObj; memcpy(&hObj, (char*)arg+8, 4);
            uint32_t postc; memcpy(&postc, (char*)arg+12, 4);
            if (hObj && postc == 0x83de) {
                armed = 1;
                fprintf(stderr, "[arm_modes] session %#x of client %#x created; arming modes...\n", hObj, hClient);
                fire(fd, hClient, hObj, 0x83de031f);
                fire(fd, hClient, hObj, 0x83de0307);
                fire(fd, hClient, hObj, 0x83de032a);
            }
        }
    }
    return r;
}
