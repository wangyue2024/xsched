// find_live_handler.cpp - locate the *live* sm120 trap-handler plugin in device memory.
//
// Why this exists (see FINDINGS_ZH.md): the tools export-table (slot 20) image
// is a 0x1300-byte container whose first 0xA00 bytes are the relocated
// `do_pause` plugin routine and whose tail is a trampoline.  The trap *entry*
// (`.text.traphandler`) and `do_read_trap_reason` are NOT in that container, yet
// they are what a hardware trap actually enters, and they carry the absolute
// address of the CUDA preemption buffer whose command word (+4) selects
// save/pause/restore.  This probe:
//
//   1. resolves the tools image (export table -> slot 20 -> ObjGetPc/ObjGetSize)
//   2. verifies it byte-wise against `.text.do_pause` extracted from libcuda
//   3. decodes the relocated immediates -> scratchpad / reason tables / arena base
//   4. fingerprint-scans the arena for the installed plugin routines
//   5. decodes CUDA_PREEMPTION_BUFFER_PATCH_ADDR out of every hit
//
// Reads must go through cuxtra: the arena is invisible to cuMemcpyDtoH_v2 /
// cuMemGetAddressRange / cuPointerGetAttribute (measured by read_probe), and
// cuxtra exits the process on a failed read - hence the scan is confined to the
// arena span derived from the image relocs, and --start/--end allow resuming.
//
// Build: see ../build.sh   Run:
//   CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1 \
//   ./find_live_handler --fpdir ../evidence/fp --sections ../evidence/sections \
//       --dump ../evidence/live [--dry] [--start 0x100000] [--end 0x1a00000]

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

#define CU_CHECK(x) do { CUresult _r = (x); if (_r != CUDA_SUCCESS) { \
    const char *_s = nullptr; cuGetErrorName(_r, &_s); \
    fprintf(stderr, "[FATAL] %s -> %d (%s) @%d\n", #x, (int)_r, _s ? _s : "?", __LINE__); \
    exit(1);} } while (0)

static const char *kTrapHandlerEtid = "cc529e945c4e9d46837c9625868334e4";
static const char *kToolsMemoryEtid = "bfdb432dbf3c5a4a945eb34029e81e75";
static const size_t kTrapSlotGetInfo = 20;
static const size_t kMemSlotObjGetPc = 24;
static const size_t kMemSlotObjGetSize = 25;
// 595.99.02 arena layout: trap_reason_table == arena_base + 0xc38300
static const uint64_t kTrapReasonTableOff = 0xc38300;

struct BlackwellTrapInfo { uint64_t flags, struct_size; void *handle; uint64_t stub, hsize, rsv; };

static CUuuid uuid_of(const char *h)
{
    CUuuid u;
    for (int i = 0; i < 16; i++) { unsigned v; sscanf(h + i * 2, "%2x", &v); u.bytes[i] = (char)v; }
    return u;
}

static bool file_slurp(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    size_t rd = n ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return rd == (size_t)n;
}

// A Blackwell instruction is 16 bytes: [0:8] = opcode word (LE u64 whose high
// 32 bits carry a MOV/UMOV immediate), [8:16] = control word.
static uint32_t instr_imm32(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)(w >> 32); }
static uint32_t instr_opcode(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return (uint32_t)w; }

static bool decode_mov_pair(const uint8_t *lo, const uint8_t *hi, uint64_t *addr)
{
    uint64_t a = instr_imm32(lo), b = instr_imm32(hi);
    if (b > 0xffff) return false;
    *addr = a | (b << 32);
    return true;
}

struct Fp
{
    std::string name;
    std::vector<uint8_t> words; // n * 8 bytes of opcode words
    size_t nwords() const { return words.size() / 8; }
};

static bool match_at(const uint8_t *mem, const Fp &fp)
{
    for (size_t i = 0; i < fp.nwords(); i++)
        if (memcmp(mem + i * 16, &fp.words[i * 8], 8) != 0) return false;
    return true;
}

// cuxtra reports a short count when the request crosses out of a mapped range
// (and, for some ranges, exits the process outright), so shrink until it works.
static size_t read_shrink(void *dst, uint64_t va, size_t want, CUstream s, size_t min = 64)
{
    for (size_t n = want;; ) {
        size_t got = cuXtraMemcpyDtoH(dst, va, n, s);
        if (got == n) return got;
        if (n <= min) return 0;
        n >>= 1;
        if (n < min) n = min;
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string fpdir = "../evidence/fp", secdir = "../evidence/sections", dumpdir = "../evidence/live";
    bool dry = false, map_mode = false;
    uint64_t start_off = 0, end_off = 0, map_step = 0x10000;
    size_t chunk = 1u << 20;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--fpdir") fpdir = next();
        else if (a == "--sections") secdir = next();
        else if (a == "--dump") dumpdir = next();
        else if (a == "--dry") dry = true;
        else if (a == "--start") start_off = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--end") end_off = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--chunk") chunk = strtoull(next().c_str(), nullptr, 0);
        else if (a == "--map") { map_mode = true; map_step = strtoull(next().c_str(), nullptr, 0); }
        else { fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }

    CU_CHECK(cuInit(0));
    CUdevice dev; CU_CHECK(cuDeviceGet(&dev, 0));
    CUcontext ctx; CU_CHECK(cuCtxCreate(&ctx, 0, dev));
    CUstream stream; CU_CHECK(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING));
    printf("[ctx] %p dev=%d\n", (void *)ctx, (int)dev);

    CUuuid trap_etid = uuid_of(kTrapHandlerEtid), mem_etid = uuid_of(kToolsMemoryEtid);
    const void **trap_table = nullptr, **mem_table = nullptr;
    CU_CHECK(cuGetExportTable((const void **)&trap_table, &trap_etid));
    CU_CHECK(cuGetExportTable((const void **)&mem_table, &mem_etid));
    size_t trap_slots = ((uint64_t)trap_table[0]) / 8;
    size_t mem_slots = ((uint64_t)mem_table[0]) / 8;
    printf("[etbl] trap=%p slots=%zu  tools-memory=%p slots=%zu\n",
           (void *)trap_table, trap_slots, (void *)mem_table, mem_slots);
    if (trap_slots <= kTrapSlotGetInfo || mem_slots <= kMemSlotObjGetSize) return 1;

    BlackwellTrapInfo info; memset(&info, 0, sizeof info);
    CU_CHECK(((CUresult(*)(CUcontext, void *))trap_table[kTrapSlotGetInfo])(ctx, &info));
    uint64_t pc = 0, sz = 0;
    CU_CHECK(((CUresult(*)(CUcontext, void *, uint64_t *))mem_table[kMemSlotObjGetPc])(ctx, info.handle, &pc));
    CU_CHECK(((CUresult(*)(void *, uint64_t *))mem_table[kMemSlotObjGetSize])(info.handle, &sz));
    printf("[slot20] handle=%p stub=%#llx hsize=%#llx\n", info.handle,
           (unsigned long long)info.stub, (unsigned long long)info.hsize);
    printf("[tools-image] pc=%#llx size=%#llx\n", (unsigned long long)pc, (unsigned long long)sz);

    std::vector<uint8_t> img(sz);
    if (cuXtraMemcpyDtoH(img.data(), pc, sz, stream) != sz) { fprintf(stderr, "read failed\n"); return 1; }

    // ---- verify against libcuda's do_pause
    std::vector<uint8_t> dopause;
    if (file_slurp(secdir + "/plugin_120_0.do_pause.bin", dopause) && dopause.size() <= img.size()) {
        size_t diff = 0, n = dopause.size() / 16;
        printf("[verify] mismatch offsets:");
        for (size_t i = 0; i < n; i++)
            if (instr_opcode(&img[i * 16]) != instr_opcode(&dopause[i * 16])) { diff++; printf(" %#zx", i * 16); }
        printf("\n[verify] tools-image[0,%#zx) vs .text.do_pause: %zu/%zu opcode mismatches\n",
               dopause.size(), diff, n);
    }

    // ---- decode relocated immediates
    struct Slot { size_t off; const char *name; };
    static const Slot slots[] = {
        {0x010, "scratchpad"}, {0x130, "trap_reason_table"}, {0x170, "pause_reason_table"},
        {0x250, "scratchpad#2"}, {0x350, "scratchpad#3"},
    };
    uint64_t scratchpad = 0, trap_reason_table = 0;
    for (auto &s : slots) {
        if (s.off + 32 > img.size()) continue;
        uint64_t a = 0;
        if (!decode_mov_pair(&img[s.off], &img[s.off + 16], &a)) continue;
        printf("[reloc] %-18s @%#05zx = %#014llx\n", s.name, s.off, (unsigned long long)a);
        if (!strcmp(s.name, "scratchpad")) scratchpad = a;
        if (!strcmp(s.name, "trap_reason_table")) trap_reason_table = a;
    }
    if (!scratchpad || !trap_reason_table) { fprintf(stderr, "reloc decode failed\n"); return 1; }
    uint64_t base = trap_reason_table - kTrapReasonTableOff;
    printf("[arena] base=%#llx scratchpad=%#llx span=%#llx tools_image=base+%#llx\n",
           (unsigned long long)base, (unsigned long long)scratchpad,
           (unsigned long long)(scratchpad - base), (unsigned long long)(pc - base));

    if (dry) { printf("[dry] stop\n"); return 0; }

    // ---- fingerprints
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
    std::sort(fps.begin(), fps.end(), [](const Fp &a, const Fp &b) { return a.name < b.name; });
    printf("[fp] %zu fingerprints\n", fps.size());
    if (fps.empty()) return 1;
    size_t maxwords = 0;
    for (auto &f : fps) maxwords = std::max(maxwords, f.nwords());

    // ---- scan the arena
    uint64_t lo = base + start_off;
    uint64_t hi = end_off ? base + end_off : scratchpad + 0x100000;

    if (map_mode) {
        printf("[map] readability of [base+%#llx, base+%#llx) step %#llx\n",
               (unsigned long long)start_off, (unsigned long long)(hi - base),
               (unsigned long long)map_step);
        std::vector<uint8_t> probe(4096);
        uint64_t first_bad = 0, readable = 0, total = 0;
        for (uint64_t at = lo; at < hi; at += map_step) {
            size_t got = read_shrink(probe.data(), at, 64, stream);
            total++;
            if (got) readable++;
            printf("%c", got ? 'R' : '.');
            if (((at - lo) / map_step) % 64 == 63)
                printf("  base+%#llx\n", (unsigned long long)(at - base));
        }
        printf("\n[map] %llu/%llu readable probes (first_bad=%#llx)\n",
               (unsigned long long)readable, (unsigned long long)total, (unsigned long long)first_bad);
        return 0;
    }
    printf("[scan] %#llx - %#llx (%.1f MB, chunk %#zx)\n", (unsigned long long)lo,
           (unsigned long long)hi, (hi - lo) / 1048576.0, chunk);

    std::vector<uint8_t> buf(chunk + 16 * maxwords);
    size_t hits = 0, done = 0;
    for (uint64_t at = lo; at < hi; at += chunk) {
        size_t want = (size_t)std::min<uint64_t>(chunk, hi - at);
        size_t n = read_shrink(buf.data(), at, want, stream, 16 * maxwords);
        if (n < want) {
            printf("[scan] base+%#llx: only %zu/%zu bytes readable, skipping to next chunk\n",
                   (unsigned long long)(at - base), n, want);
            if (n < 16 * maxwords) continue;
        }
        for (size_t off = 0; off + 16 * maxwords <= n; off += 16) {
            for (auto &fp : fps) {
                if (off + 16 * fp.nwords() > n || !match_at(&buf[off], fp)) continue;
                uint64_t va = at + off;
                hits++;
                printf("\n[HIT] %-44s va=%#014llx (base+%#llx)\n", fp.name.c_str(),
                       (unsigned long long)va, (unsigned long long)(va - base));
                std::vector<uint8_t> body(8192);
                size_t g2 = read_shrink(body.data(), va, body.size(), stream);
                char nm[512];
                snprintf(nm, sizeof nm, "%s/live_%s_%llx.bin", dumpdir.c_str(), fp.name.c_str(),
                         (unsigned long long)(va - base));
                if (g2) {
                    FILE *f = fopen(nm, "wb");
                    if (f) { fwrite(body.data(), 1, g2, f); fclose(f); }
                    printf("      dumped %zu bytes -> %s\n", g2, nm);
                }
                for (size_t c = 0; c + 32 <= g2; c += 16) {
                    uint64_t a2 = 0;
                    if (!decode_mov_pair(&body[c], &body[c + 16], &a2)) continue;
                    if (a2 < base || a2 > base + 0x4000000) continue;
                    printf("      imm@%#06zx = %#014llx (base+%#llx)%s\n", c,
                           (unsigned long long)a2, (unsigned long long)(a2 - base),
                           (c == 0x250 || c == 0x130) ? "   <== CUDA_PREEMPTION_BUFFER?" : "");
                }
            }
        }
        done++;
        if (done % 4 == 0)
            printf("  ... base+%#llx (%.0f%%, %zu hits)\n", (unsigned long long)(at - base),
                   100.0 * (at - lo) / (hi - lo), hits);
    }
    printf("\n[scan] done: %zu chunks, %zu hits\n", done, hits);
    return 0;
}
