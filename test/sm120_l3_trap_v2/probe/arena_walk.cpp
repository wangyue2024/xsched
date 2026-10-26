// arena_walk.cpp - map the driver-internal device arena into tools memory
// objects, then fingerprint-match each object against the trap-handler plugin
// routines extracted from libcuda.
//
// Why not scan blindly: the arena is invisible to cuMemcpyDtoH_v2 /
// cuMemGetAddressRange, and cuxtra's cuXtraMemcpyDtoH calls exit(1) on some
// failures (measured by read_probe / find_live_handler --map).  cuxtra's
// internal FindMemObjHandle() answers "is this VA inside a tools memory object?"
// *without* touching device memory, so it is a safe probe: walking the arena
// with it yields the exact object list, and every object it reports can then be
// read back safely with cuXtraMemcpyDtoH.
//
// Build: ../build.sh arena_walk      Run:
//   CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1 \
//   ./arena_walk --fpdir ../evidence/fp --dump ../evidence/live

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <dirent.h>

#include <cuda.h>
extern "C" {
#include <cuxtra/cuxtra.h>
}
extern "C" CUresult cuGetExportTable(const void **ppTable, const CUuuid *pTableId);

// cuxtra-internal (defined in 3rdparty/cuxtra/lib/libcuxtra_linux_x86_64.a,
// memobj.cpp.o): VA -> tools memory object lookup used by its memcpy wrappers.
namespace cuxtra {
namespace etbl { struct CUtoolsMemObjHandle_st; }
namespace cuda {
bool FindMemObjHandle(CUctx_st *ctx, unsigned long long va,
                      cuxtra::etbl::CUtoolsMemObjHandle_st **obj,
                      unsigned long long *offset, unsigned long *size);
}
}

struct CUtoolsMemObjHandle_st;
typedef CUresult (*ObjGetPcFn)(CUcontext, void *, uint64_t *);
typedef CUresult (*ObjGetSizeFn)(void *, uint64_t *);

static const char *kTrapHandlerEtid = "cc529e945c4e9d46837c9625868334e4";
static const char *kToolsMemoryEtid = "bfdb432dbf3c5a4a945eb34029e81e75";
static const uint64_t kTrapReasonTableOff = 0xc38300; // 595.99.02 arena layout

struct Fp { std::string name; std::vector<uint8_t> words; size_t nwords() const { return words.size() / 8; } };

static uint32_t opcode_at(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)w; }
static uint32_t imm32_at(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)(w >> 32); }

static bool match_at(const uint8_t *mem, const Fp &fp)
{
    for (size_t i = 0; i < fp.nwords(); i++)
        if (opcode_at(mem + i * 16) != ((const uint32_t *)fp.words.data())[i * 2]) return false;
    return true;
}

static bool file_slurp(const std::string &p, std::vector<uint8_t> &out)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n);
    size_t rd = n ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return rd == (size_t)n;
}

static CUuuid uuid_of(const char *h)
{
    CUuuid u;
    for (int i = 0; i < 16; i++) { unsigned v; sscanf(h + i * 2, "%2x", &v); u.bytes[i] = (char)v; }
    return u;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string fpdir = "../evidence/fp", dumpdir = "../evidence/live";
    uint64_t walk_lo = 0, walk_hi = 0, step = 0x1000;
    size_t max_read = 1u << 22;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--fpdir") fpdir = next();
        else if (a == "--dump") dumpdir = next();
        else if (a == "--step") step = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--max-read") max_read = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--range") { sscanf(next().c_str(), "%lx-%lx", &walk_lo, &walk_hi); }
    }

    if (cuInit(0)) return 1;
    CUdevice dev; cuDeviceGet(&dev, 0);
    CUcontext ctx; if (cuCtxCreate(&ctx, 0, dev)) return 1;
    CUstream stream; if (cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING)) return 1;
    printf("[ctx] %p\n", (void *)ctx);

    CUuuid te = uuid_of(kTrapHandlerEtid), me = uuid_of(kToolsMemoryEtid);
    const void **tt = nullptr, **mt = nullptr;
    if (cuGetExportTable((const void **)&tt, &te) || cuGetExportTable((const void **)&mt, &me)) return 1;
    struct { uint64_t flags, ssize; void *handle; uint64_t stub, hsize, rsv; } info;
    memset(&info, 0, sizeof info);
    if (((CUresult(*)(CUcontext, void *))tt[20])(ctx, &info)) { fprintf(stderr, "slot20 failed\n"); return 1; }
    uint64_t pc = 0, sz = 0;
    if (((ObjGetPcFn)mt[24])(ctx, info.handle, &pc) || ((ObjGetSizeFn)mt[25])(info.handle, &sz))
        { fprintf(stderr, "ObjGetPc/Size failed\n"); return 1; }
    printf("[tools-image] pc=%#llx size=%#llx handle=%p\n",
           (unsigned long long)pc, (unsigned long long)sz, info.handle);

    std::vector<uint8_t> img(sz);
    if (cuXtraMemcpyDtoH(img.data(), pc, sz, stream) != sz) { fprintf(stderr, "image read failed\n"); return 1; }
    uint64_t scratchpad = (uint64_t)imm32_at(&img[0x010]) | ((uint64_t)imm32_at(&img[0x020]) << 32);
    uint64_t trt        = (uint64_t)imm32_at(&img[0x130]) | ((uint64_t)imm32_at(&img[0x140]) << 32);
    uint64_t base = trt - kTrapReasonTableOff;
    printf("[arena] base=%#llx trap_reason_table=%#llx scratchpad=%#llx tools_image=base+%#llx\n",
           (unsigned long long)base, (unsigned long long)trt,
           (unsigned long long)scratchpad, (unsigned long long)(pc - base));

    if (!walk_lo && !walk_hi) { walk_lo = base; walk_hi = scratchpad + 0x200000; }
    printf("[walk] %#llx - %#llx step %#llx\n", (unsigned long long)walk_lo,
           (unsigned long long)walk_hi, (unsigned long long)step);

    // ---- walk: VA -> memobj, skipping whole objects once found
    struct Obj { uint64_t va_hint, off, size; };
    std::map<void *, Obj> objs;
    uint64_t probes = 0;
    for (uint64_t va = walk_lo; va < walk_hi; ) {
        cuxtra::etbl::CUtoolsMemObjHandle_st *h = nullptr;
        unsigned long long off = 0;
        unsigned long size = 0;
        probes++;
        bool found = cuxtra::cuda::FindMemObjHandle(ctx, va, &h, &off, &size);
        if (!found || !h) { va += step; continue; }
        if (!objs.count(h)) {
            objs[h] = {va, off, size};
            printf("[obj] %-22p first_va=%#014llx (base+%#llx) off=%#llx size=%#x\n", (void *)h,
                   (unsigned long long)va, (unsigned long long)(va - base),
                   (unsigned long long)off, size);
        }
        uint64_t advance = off + size > 0 ? (off + size) : step;
        va = std::max<uint64_t>(va + step, (va - off) + advance);
    }
    printf("[walk] %llu probes, %zu distinct objects\n", (unsigned long long)probes, objs.size());

    // ---- fingerprints
    std::vector<Fp> fps;
    if (DIR *d = opendir(fpdir.c_str())) {
        while (struct dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() < 4 || n.compare(n.size() - 3, 3, ".fp")) continue;
            Fp fp; fp.name = n.substr(0, n.size() - 3);
            if (file_slurp(fpdir + "/" + n, fp.words) && fp.words.size() >= 32) fps.push_back(fp);
        }
        closedir(d);
    }
    printf("[fp] %zu fingerprints\n\n", fps.size());

    // ---- read + match each object
    size_t total_hits = 0;
    for (auto &kv : objs) {
        void *h = kv.first;
        uint64_t opC = 0, osz = 0;
        CUresult r1 = ((ObjGetPcFn)mt[24])(ctx, h, &opC);
        CUresult r2 = ((ObjGetSizeFn)mt[25])(h, &osz);
        if (r1 || r2 || !osz) { printf("[read] %-22p pc/size unavailable (%d/%d)\n", h, (int)r1, (int)r2); continue; }
        size_t want = (size_t)std::min<uint64_t>(osz, max_read);
        std::vector<uint8_t> buf(want);
        size_t got = cuXtraMemcpyDtoH(buf.data(), opC, want, stream);
        printf("[read] %-22p pc=%#014llx (base+%#llx) size=%#llx read=%zu/%zu\n", h,
               (unsigned long long)opC, (unsigned long long)(opC - base),
               (unsigned long long)osz, got, want);
        if (got != want) continue;
        std::vector<std::string> hits;
        for (size_t off = 0; off + 16 <= got; off += 16)
            for (auto &fp : fps) {
                if (off + 16 * fp.nwords() > got) continue;
                if (!match_at(&buf[off], fp)) continue;
                char b[256];
                snprintf(b, sizeof b, "%s@+%#zx", fp.name.c_str(), off);
                hits.push_back(b);
                total_hits++;
            }
        if (hits.empty()) continue;
        printf("       <<< MATCHES:");
        for (auto &x : hits) printf(" %s", x.c_str());
        printf("\n");
        char nm[512];
        snprintf(nm, sizeof nm, "%s/obj_%llx_%llx.bin", dumpdir.c_str(),
                 (unsigned long long)opC, (unsigned long long)osz);
        FILE *f = fopen(nm, "wb");
        if (f) { fwrite(buf.data(), 1, got, f); fclose(f); printf("       dumped -> %s\n", nm); }
        for (size_t c = 0; c + 32 <= got; c += 16) {
            uint64_t a = (uint64_t)imm32_at(&buf[c]) | ((uint64_t)imm32_at(&buf[c + 16]) << 32);
            if (imm32_at(&buf[c + 16]) > 0xffff) continue;
            if (a < base || a > base + 0x4000000) continue;
            printf("       imm@%#08zx = %#014llx (base+%#llx)%s\n", c, (unsigned long long)a,
                   (unsigned long long)(a - base),
                   (c == 0x250 || c == 0x130 || c == 0x020 || c == 0x010) ? "  <== PREEMPT BUFFER?" : "");
        }
    }
    printf("\n[done] objects=%zu matches=%zu\n", objs.size(), total_hits);
    return 0;
}
