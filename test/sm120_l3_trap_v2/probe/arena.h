// arena.h - locate the sm120 driver-internal trap-handler arena (header only).
//
// See FINDINGS_ZH.md v0.1 for how this was derived:
//   * the tools export table (CU_ETID_ToolsTrapHandler slot 20) reports a
//     0x1300-byte image whose [0,0xA00) is the relocated `.text.do_pause`
//     plugin routine from libcuda;
//   * its relocation slots expose scratchpad / trap_reason_table, and the arena
//     base is trap_reason_table - 0xc38300 on driver 595.99.02;
//   * the *live* trap entry (`.text.traphandler`) is installed at
//     base + 0x15f0f00 and carries CUDA_PREEMPTION_BUFFER_PATCH_ADDR at +0x250;
//   * the arena is invisible to cuMemcpyDtoH/cuMemGetAddressRange, so reads and
//     writes must go through cuxtra (or through device code).
#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cuda.h>

extern "C" {
#include <cuxtra/cuxtra.h>
}
extern "C" CUresult cuGetExportTable(const void **ppTable, const CUuuid *pTableId);

namespace cuxtra {
namespace etbl { struct CUtoolsMemObjHandle_st; }
namespace cuda {
bool FindMemObjHandle(CUctx_st *ctx, unsigned long long va,
                      cuxtra::etbl::CUtoolsMemObjHandle_st **obj,
                      unsigned long long *offset, unsigned long *size);
}
}

namespace sm120arena {

inline const char *kTrapHandlerEtid = "cc529e945c4e9d46837c9625868334e4";
inline const char *kToolsMemoryEtid = "bfdb432dbf3c5a4a945eb34029e81e75";
inline const uint64_t kTrapReasonTableOff = 0xc38300;  // trap_reason_table - arena base
inline const uint64_t kLiveTrapHandlerOff = 0x15f0f00; // .text.traphandler - arena base
inline const uint64_t kToolsImageOff      = 0x15f5300; // slot-20 image - arena base
inline const size_t   kPreemptBufRelocOff = 0x250;     // MOV pair inside traphandler
inline const size_t   kTrapHandlerBytes   = 2304;      // sm_120 variant 1

// relocation-free opcode prefix of libcuda's .text.traphandler
inline const uint32_t kTrapHandlerFp[10] = {
    0x0000795c, 0x1a007956, 0xff007387, 0xff007387, 0xff007387,
    0x00047202, 0x00057202, 0xff067803, 0x17077355, 0xff007387,
};

inline uint32_t opcode_at(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)w; }
inline uint32_t imm32_at(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)(w >> 32); }
inline uint64_t pair_at(const uint8_t *p)
{
    return (uint64_t)imm32_at(p) | ((uint64_t)imm32_at(p + 16) << 32);
}

inline CUuuid uuid_of(const char *h)
{
    CUuuid u;
    for (int i = 0; i < 16; i++) { unsigned v; sscanf(h + i * 2, "%2x", &v); u.bytes[i] = (char)v; }
    return u;
}

struct Info
{
    bool ok = false;
    CUcontext ctx = nullptr;
    uint64_t base = 0, scratchpad = 0, trap_reason_table = 0, pause_reason_table = 0;
    uint64_t tools_pc = 0, tools_size = 0;
    uint64_t handler_va = 0, preempt_buf = 0;
    bool handler_is_memobj = false, preempt_is_memobj = false;
    unsigned long preempt_obj_size = 0;
    std::vector<uint8_t> handler;   // live .text.traphandler image
    std::vector<uint8_t> tools;     // slot-20 image
    std::string note;
};

// Resolve everything we know how to reach, without touching device memory
// outside of verified tools memory objects.
inline Info probe(CUcontext ctx, CUstream stream, uint64_t th_off = kLiveTrapHandlerOff,
                  bool read_handler = true)
{
    Info I;
    I.ctx = ctx;
    const void **tt = nullptr, **mt = nullptr;
    CUuuid te = uuid_of(kTrapHandlerEtid), me = uuid_of(kToolsMemoryEtid);
    if (cuGetExportTable((const void **)&tt, &te) || cuGetExportTable((const void **)&mt, &me)) {
        I.note = "export tables unavailable";
        return I;
    }
    struct { uint64_t flags, ssize; void *handle; uint64_t stub, hsize, rsv; } ti;
    memset(&ti, 0, sizeof ti);
    if (((CUresult(*)(CUcontext, void *))tt[20])(ctx, &ti)) { I.note = "slot20 getter failed"; return I; }
    if (((CUresult(*)(CUcontext, void *, uint64_t *))mt[24])(ctx, ti.handle, &I.tools_pc) ||
        ((CUresult(*)(void *, uint64_t *))mt[25])(ti.handle, &I.tools_size)) {
        I.note = "ObjGetPc/ObjGetSize failed";
        return I;
    }
    I.tools.resize(I.tools_size);
    if (cuXtraMemcpyDtoH(I.tools.data(), I.tools_pc, I.tools_size, stream) != I.tools_size) {
        I.note = "tools image read failed";
        return I;
    }
    I.scratchpad        = pair_at(&I.tools[0x010]);
    I.trap_reason_table = pair_at(&I.tools[0x130]);
    I.pause_reason_table= pair_at(&I.tools[0x170]);
    I.base = I.trap_reason_table - kTrapReasonTableOff;

    I.handler_va = I.base + th_off;
    cuxtra::etbl::CUtoolsMemObjHandle_st *o = nullptr;
    unsigned long long off = 0; unsigned long sz = 0;
    I.handler_is_memobj = cuxtra::cuda::FindMemObjHandle(ctx, I.handler_va, &o, &off, &sz);
    if (read_handler && I.handler_is_memobj) {
        I.handler.resize(kTrapHandlerBytes);
        if (cuXtraMemcpyDtoH(I.handler.data(), I.handler_va, kTrapHandlerBytes, stream)
            == kTrapHandlerBytes)
            I.preempt_buf = pair_at(&I.handler[kPreemptBufRelocOff]);
    }
    o = nullptr; off = 0; sz = 0;
    if (I.preempt_buf) {
        I.preempt_is_memobj = cuxtra::cuda::FindMemObjHandle(ctx, I.preempt_buf, &o, &off, &sz);
        I.preempt_obj_size = sz;
    }
    I.ok = I.base != 0 && I.handler_va != 0;
    return I;
}

inline void report(const Info &I, const char *tag = "")
{
    printf("%s[arena] base=%#llx scratchpad=%#llx trap_reason_table=%#llx pause_reason_table=%#llx\n",
           tag, (unsigned long long)I.base, (unsigned long long)I.scratchpad,
           (unsigned long long)I.trap_reason_table, (unsigned long long)I.pause_reason_table);
    printf("%s[tools ] pc=%#llx size=%#llx (base+%#llx)\n", tag,
           (unsigned long long)I.tools_pc, (unsigned long long)I.tools_size,
           (unsigned long long)(I.tools_pc - I.base));
    printf("%s[handler] va=%#llx (base+%#llx) memobj=%d", tag,
           (unsigned long long)I.handler_va, (unsigned long long)(I.handler_va - I.base),
           (int)I.handler_is_memobj);
    if (I.handler.size() >= 160) {
        bool fp = true;
        for (size_t i = 0; i < 10; i++)
            if (opcode_at(&I.handler[i * 16]) != kTrapHandlerFp[i]) fp = false;
        printf(" fingerprint=%s", fp ? "MATCH" : "FAIL");
    }
    printf("\n%s[preempt-buf] va=%#llx memobj=%d size=%#lx\n", tag,
           (unsigned long long)I.preempt_buf, (int)I.preempt_is_memobj, I.preempt_obj_size);
    if (!I.note.empty()) printf("%s[note] %s\n", tag, I.note.c_str());
}

} // namespace sm120arena
