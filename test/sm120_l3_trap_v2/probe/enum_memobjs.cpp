// enum_memobjs.cpp - enumerate every device memory object of the context through
// the tools export table and fingerprint-match them against the trap-handler
// plugin routines extracted from libcuda.
//
// cuxtra resolves two ToolsMemory slots that the xsched trap path never used:
//   slot 26  CtxEnumerateMemBlocks(ctx, cb(void* ud, MemBlock*, size_t i), ud)
//   slot 27  CtxEnumerateMemObjs (block, cb(void* ud, MemObj*,  size_t i), ud)
// combined with the already-known
//   slot 24  ObjGetPc (ctx, obj, &pc)
//   slot 25  ObjGetSize(obj, &size)
// that gives a complete, *non-fatal* inventory of the driver-internal device
// arena - which blind scanning cannot provide (cuxtra's memcpy exits the process
// on an unmapped VA, and the arena is invisible to cuMemcpyDtoH/cuMemGetAddressRange).
//
// Build: ../build.sh enum_memobjs     Run:
//   CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1 \
//   ./enum_memobjs --fpdir ../evidence/fp --dump ../evidence/live [--max-read 65536]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>

#include <cuda.h>
extern "C" {
#include <cuxtra/cuxtra.h>
}
extern "C" CUresult cuGetExportTable(const void **ppTable, const CUuuid *pTableId);

struct CUtoolsMemObjHandle_st;
struct CUtoolsMemBlockHandle_st;

typedef CUresult (*EnumMemBlocksFn)(CUcontext, CUresult (*)(void *, CUtoolsMemBlockHandle_st *, size_t), void *);
typedef CUresult (*EnumMemObjsFn)(CUtoolsMemBlockHandle_st *, CUresult (*)(void *, CUtoolsMemObjHandle_st *, size_t), void *);
typedef CUresult (*ObjGetPcFn)(CUcontext, CUtoolsMemObjHandle_st *, uint64_t *);
typedef CUresult (*ObjGetSizeFn)(CUtoolsMemObjHandle_st *, uint64_t *);

static const char *kToolsMemoryEtid = "bfdb432dbf3c5a4a945eb34029e81e75";
static const size_t kSlotObjGetPc = 24, kSlotObjGetSize = 25;
static const size_t kSlotEnumBlocks = 26, kSlotEnumObjs = 27;

struct Fp
{
    std::string name;
    std::vector<uint8_t> words;
    size_t nwords() const { return words.size() / 8; }
};

struct State
{
    CUcontext ctx = nullptr;
    CUstream stream = nullptr;
    const void **tbl = nullptr;
    std::vector<Fp> *fps = nullptr;
    std::string dumpdir;
    size_t max_read = 1u << 20;
    size_t blocks = 0, objs = 0, readable = 0, hits = 0;
    std::vector<uint8_t> buf;
};

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
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    size_t rd = n ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return rd == (size_t)n;
}

static CUresult obj_cb(void *ud, CUtoolsMemObjHandle_st *obj, size_t idx)
{
    State *st = (State *)ud;
    st->objs++;
    uint64_t pc = 0, sz = 0;
    CUresult r1 = ((ObjGetPcFn)st->tbl[kSlotObjGetPc])(st->ctx, obj, &pc);
    CUresult r2 = ((ObjGetSizeFn)st->tbl[kSlotObjGetSize])(obj, &sz);
    if (r1 != CUDA_SUCCESS || r2 != CUDA_SUCCESS || sz == 0) {
        printf("  obj[%3zu] %-22p pc=? size=? (r1=%d r2=%d)\n", idx, (void *)obj, (int)r1, (int)r2);
        return CUDA_SUCCESS;
    }
    size_t want = (size_t)std::min<uint64_t>(sz, st->max_read);
    if (want > st->buf.size()) st->buf.resize(want);
    size_t got = cuXtraMemcpyDtoH(st->buf.data(), pc, want, st->stream);
    if (got != want) {
        printf("  obj[%3zu] %-22p pc=%#014llx size=%#010llx READ %zu/%zu\n", idx, (void *)obj,
               (unsigned long long)pc, (unsigned long long)sz, got, want);
        return CUDA_SUCCESS;
    }
    st->readable++;
    std::string match;
    for (size_t off = 0; off + 16 <= got; off += 16) {
        for (auto &fp : *st->fps) {
            if (off + 16 * fp.nwords() > got) continue;
            if (match_at(&st->buf[off], fp)) {
                char b[256];
                snprintf(b, sizeof b, "%s@+%#zx", fp.name.c_str(), off);
                if (!match.empty()) match += " ";
                match += b;
                st->hits++;
            }
        }
    }
    printf("  obj[%3zu] %-22p pc=%#014llx size=%#010llx%s%s\n", idx, (void *)obj,
           (unsigned long long)pc, (unsigned long long)sz,
           match.empty() ? "" : "   <<< MATCH ", match.c_str());
    if (!match.empty() && want <= 1u << 20) {
        char nm[512];
        snprintf(nm, sizeof nm, "%s/obj_%llx_%llx.bin", st->dumpdir.c_str(),
                 (unsigned long long)pc, (unsigned long long)sz);
        FILE *f = fopen(nm, "wb");
        if (f) { fwrite(st->buf.data(), 1, got, f); fclose(f); printf("      dumped -> %s\n", nm); }
    }
    return CUDA_SUCCESS;
}

static CUresult block_cb(void *ud, CUtoolsMemBlockHandle_st *blk, size_t idx)
{
    State *st = (State *)ud;
    st->blocks++;
    printf("[block %3zu] %p\n", idx, (void *)blk);
    CUresult r = ((EnumMemObjsFn)st->tbl[kSlotEnumObjs])(blk, obj_cb, ud);
    if (r != CUDA_SUCCESS) printf("  CtxEnumerateMemObjs -> %d\n", (int)r);
    return CUDA_SUCCESS;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    State st;
    std::string fpdir = "../evidence/fp";
    st.dumpdir = "../evidence/live";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--fpdir") fpdir = next();
        else if (a == "--dump") st.dumpdir = next();
        else if (a == "--max-read") st.max_read = strtoull(next().c_str(), nullptr, 0);
    }

    CUresult r = cuInit(0);
    if (r) { fprintf(stderr, "cuInit %d\n", (int)r); return 1; }
    CUdevice dev; cuDeviceGet(&dev, 0);
    if (cuCtxCreate(&st.ctx, 0, dev)) return 1;
    if (cuStreamCreate(&st.stream, CU_STREAM_NON_BLOCKING)) return 1;

    CUuuid u;
    for (int i = 0; i < 16; i++) { unsigned v; sscanf(kToolsMemoryEtid + i * 2, "%2x", &v); u.bytes[i] = (char)v; }
    if (cuGetExportTable((const void **)&st.tbl, &u)) { fprintf(stderr, "no tools-memory table\n"); return 1; }
    size_t slots = ((uint64_t)st.tbl[0]) / 8;
    printf("[etbl] tools-memory %p slots=%zu\n", (void *)st.tbl, slots);
    if (slots <= kSlotEnumObjs) { fprintf(stderr, "enum slots missing\n"); return 1; }

    std::vector<Fp> fps;
    if (DIR *d = opendir(fpdir.c_str())) {
        while (struct dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() < 4 || n.compare(n.size() - 3, 3, ".fp")) continue;
            Fp fp;
            fp.name = n.substr(0, n.size() - 3);
            if (file_slurp(fpdir + "/" + n, fp.words) && fp.words.size() >= 32) fps.push_back(fp);
        }
        closedir(d);
    }
    st.fps = &fps;
    printf("[fp] %zu fingerprints, max-read=%#zx, dump=%s\n", fps.size(), st.max_read, st.dumpdir.c_str());

    CUresult rr = ((EnumMemBlocksFn)st.tbl[kSlotEnumBlocks])(st.ctx, block_cb, &st);
    printf("\n[done] CtxEnumerateMemBlocks=%d blocks=%zu objs=%zu readable=%zu hits=%zu\n",
           (int)rr, st.blocks, st.objs, st.readable, st.hits);
    return 0;
}
