# XSched Guardian Instruction Generation Toolchain (tools/instrument)

> **Purpose**: automatically replace the `brkpt` placeholders (SASS `BPT.TRAP`)
> in the compiled artifacts of `platforms/cuda/hal/inject/inject.cu` with the
> target instructions declared in the source comments, and emit the `uint64_t`
> instruction arrays consumed by `arch/sm{N}.cpp` — i.e., the engineering asset
> that **fully automates** the manual editing pipeline of the sm86 era
> (design task T4).
>
> Status: **T4 complete** (sm86 golden regression 9/9 PASS; 19/19 encodings
> bit-identical to the official manual edits; the sm120 hardware window at
> 0x170 was verified by T1 and is auto-addressed by this tool). Evidence:
> `test/sm120_l2_gen/evidence/`.

---

## 1. Background: the official (sm86) generation pipeline

```
inject.cu  ──nvcc -cubin --keep-device-functions -Xptxas -astoolspatch──▶  inject_<arch>.cubin
    │  each nop() (= asm volatile("brkpt;")) becomes a 16-byte BPT.TRAP placeholder
    ▼
cuobjdump -sass ──▶ inject_<arch>.asm   (manually reviewed file)
    │  manually replace each placeholder 1:1 in place with the SASS instruction
    │  written in the comment ("slot count = instruction count" constraint:
    │  a 64-bit pointer = two placeholders = two LDC instructions)
    ▼
arch/sm86.cpp   (uint64_t array, copied into instruction memory at runtime via cuXtraInstrMem*)
```

This tool automates the "manual replacement" step and adds triple verification
(round-trip self-check, K1 equal-length replacement assertion, control-flow
invariance assertion).

## 2. Subcommands

### 2.1 review — compilation baseline census (T3)

```powershell
python tools/instrument/extract_sass.py review --asm platforms/cuda/hal/inject/inject_120.asm [--out report.md]
```

Outputs a per-function instruction census table (instruction count / max
register number / BSSY / BSYNC / BRA / BREAK / BAR / LDC / STL / LDL / LD.ST.E /
BPT / NOP / EXIT / RET.ABS). Used for: the L2-path "no local memory
(STL/LDL=0)" contract check, register upper-bound estimation, and placeholder
count verification.

### 2.2 generate — placeholder replacement and array generation

```powershell
# sm120 (current target of this repository): debugger window 0x170..0x188 (verified by T1)
python tools/instrument/extract_sass.py generate `
    --cubin platforms/cuda/hal/inject/inject_120.cubin `
    --source platforms/cuda/hal/inject/inject.cu `
    --offset-map sm120 `
    --out-cubin evidence/patched_120.cubin `
    --out-cpp   evidence/preview_sm120.cpp `
    --report    evidence/gen_sm120_report.json

# sm86 (reference / regression)
python tools/instrument/extract_sass.py generate `
    --cubin inject_86.cubin --source inject.cu --offset-map sm86 ...
```

Parameters:

| Parameter | Description |
|---|---|
| `--cubin` | nvcc artifact (containing BPT.TRAP placeholders) |
| `--source` | `inject.cu` (insertion specs extracted from `nop(); // <spec>` comments) |
| `--offset-map` | `identity` (as-is) / `sm86` (identity eight slots) / `sm120` (0x170 series mapping) |
| `--func` | repeatable; defaults to `check_preempt restore_exec` (L2 main path) |
| `--trim` | `auto` (by function type: guardian=converge-and-trim / resume=keep exit) / `guardian` / `resume` / `none` |
| `--out-cubin` | replaced cubin (for independent nvdisasm/cuobjdump re-inspection) |
| `--out-cpp` | array preview (**PREVIEW**; the official version is committed after T5 review) |
| `--report` | JSON report (before/after encodings of each replacement, trim details, verification results) |

### 2.3 golden — sm86 golden regression

```powershell
python tools/instrument/extract_sass.py golden `
    --cubin-86  platforms/cuda/hal/inject/inject_86.cubin `
    --source    platforms/cuda/hal/inject/inject.cu `
    --official-asm platforms/cuda/hal/inject/inject_sm86.asm `
    --official-cpp platforms/cuda/hal/src/arch/sm86.cpp `
    --out-cubin evidence/patched_86.cubin --report evidence/golden_report.json
```

9 checks (G1–G3b, all must PASS):
1. **G1**: official array == official asm (byte by byte);
2. **G2 / G2b**: every encoder output of this tool (LDC/STL/LDL/IMAD.MOV/@P0 MOV/
   ISETP) can be matched in-order and bit-exactly in the official array/asm —
   proving the encoder is **bit-equivalent** to the manual editing of that era;
3. **G3a**: replaying the replacements on a **freshly compiled** cubin fully
   reproduces the same encoding sequence;
4. **G3b**: aligned-diff summary of old vs new instruction streams
   (attributing differences to "compiler scheduling").

## 3. Core Concepts

### 3.1 Insertion spec

Every placeholder in `inject.cu` carries a comment spec; the tool parses six
kinds:

| Source comment | Semantics |
|---|---|
| `// LDC Rn, c[0x0][0x1880];` | read the debugger window (offset remapped by `--offset-map`) |
| `// STL [0xfffe00], Rn;` | store to stack (trap path only) |
| `// LDL Rn, [0xfffe00];` | load from stack (trap path only) |
| `// IMAD.MOV.U32 Rn, RZ, RZ, RZ;` | register clear |
| `// @P0 MOV Rn, 0x1;` | predicated constant load |
| `// ISETP.NE.AND P0, PT, Rn, RZ, PT;` | predicate comparison |

### 3.2 Encoder (all cross-verified against the official arrays and sm120 hardware)

| Instruction | word0 formula | word1 (control-code template) |
|---|---|---|
| `LDC Rn, c[0x0][off]` | `0xff007b82 \| (n<<16) \| ((off>>2)<<40)` | `0x000fc00000000800` |
| `LDC.64 Rn, c[0x0][off]` | same as above (n must be even) | `0x000fc00000000a00` (bit9=1) |
| `STL [imm], Rn` | `0xff007387 \| (n<<16) \| (imm<<40)` ← **raw byte offset** | `0x000fc00000100800` |
| `LDL Rn, [imm]` | `0xff007983 \| (n<<16) \| (imm<<40)` | `0x000fc00000100800` |
| `IMAD.MOV.U32 R0, RZ, RZ, RZ` | `0x000000ffff007224` | `0x000fe200078e00ff` |
| `@P0 MOV R0, 0x1` | `0x0000000100000802` | `0x000fe20000000f00` |
| `ISETP.NE.AND P0, PT, Rn, RZ, PT` | `0x000000ff0000720c \| (n<<24)` | `0x004fda0003f05270` |

> Note: the LDC offset field is a **word offset** (`off>>2`), while STL/LDL
> store the **byte offset directly** — confirmed in 2026-10 by differential
> analysis of official samples (see T4_REPORT §3). The LDC formula was
> auto-cross-verified on sm120 by 12/12 natural ptxas samples
> (`evidence/crosscheck_sm120_natural_ldc.txt`).

### 3.3 Tail trimming (the K3 contract)

The two splice forms have different tail requirements; `--trim auto` picks the
right one:

- **guardian** (prefix form, spliced in front of the kernel): peel from the
  tail in order `NOP padding → self-spin BRA → function epilogue
  RET.ABS.NODEC R20`, and **must converge on a BSYNC** (physical fallthrough
  then enters the original kernel). Trimming only changes the array length and
  never touches the encoding of any retained instruction, so branch distances
  are naturally preserved.
- **resume** (trampoline form): `RET.ABS.NODEC R20` is the functional exit
  (R20:R21 are loaded by `LDC R20/R21 @ c[0x0][window slot 2]`, jumping back to
  the per-kernel guardian) and must be kept; the following BRA/NOP are also
  kept (consistent with the official array; unreachable).

### 3.4 Debugger window offset mapping (--offset-map sm120)

| Source slot (sm86 ABI) | Semantics | sm120 ABI (verified by T1) |
|---|---|---|
| `c[0x0][0x1880]/[0x1884]` | preempt_buf pointer | `c[0x0][0x170]/[0x174]` |
| `c[0x0][0x1888]/[0x188c]` | guardian entry (jump-back target) | `c[0x0][0x178]/[0x17c]` |
| `c[0x0][0x1890]/[0x1894]` | kernel_idx | `c[0x0][0x180]/[0x184]` |
| `c[0x0][0x1898]` | killable flag | `c[0x0][0x188]` |

## 4. Known Boundaries (must read)

1. **The sm120 encodings of the trap path are not hardware-verified**:
   `check_preempt_trap` / `exit_if_idempotent` use STL/LDL/IMAD.MOV/@P0 MOV/
   ISETP and currently emit the **sm86-verified encodings**; whether sm120
   needs new variants (e.g., the `HFMA2` MOV form) belongs to stage three
   (Level-3 trap) and must be re-verified per the SOP in §5 at that time.
   **The L2 main path (check_preempt/restore_exec) depends only on LDC and has
   been fully verified on hardware.**
2. The `--out-cpp` output is a **PREVIEW**: the official `arch/sm120.cpp` must
   be committed after the T5 instruction-by-instruction manual review
   (including the `RequiredRegs/RequiredBarriers` values).
3. Control codes (word1) come from templates rather than runtime inheritance;
   the L2 path keeps the official `0x000fc00000000800` template (hardware
   verified by T1). If a scheduling hazard is ever found, switch to inheriting
   control codes from natural samples of the same function.
4. The tool does not relocate branches: with equal-length 1:1 replacement the
   branch encodings necessarily stay unchanged (asserted). If non-equal-length
   editing is ever introduced, the Pass verification must be extended first.

## 5. New-Architecture Adaptation SOP (the next sm generation)

1. `cd platforms/cuda/hal/inject && make_msvc.bat ARCH=<NN> bin dump cc`
   (Windows; on Linux use `make ARCH=<NN> bin dump cc`);
2. `python tools/instrument/extract_sass.py review --asm inject_<NN>.asm`
   — check: functions complete, placeholder count (26), L2 path STL/LDL=0,
   register upper bound, BAR count;
3. **Resolve the window addressing first**: use the T1 probe methodology
   (`test/sm120_l2_probe/`) to locate the debugger-parameters window of the
   new architecture and derive the new `--offset-map`;
4. `generate --offset-map <new mapping>` → `patched` cubin →
   `nvdisasm -c` re-inspect the LDC text;
5. Run `golden` once against the current latest architecture to confirm the
   tool has not regressed;
6. T5 manual review → commit `arch/sm<N>.cpp` → MVE (T6).

## 6. Files & Evidence Index

| File | Description |
|---|---|
| `tools/instrument/extract_sass.py` | this tool (about 960 lines, pure standard library) |
| `test/sm120_l2_gen/evidence/golden_report.json` | machine-readable golden regression report |
| `test/sm120_l2_gen/evidence/preview_sm120.cpp` | sm120 array preview (guardian 50 + resume 32) |
| `test/sm120_l2_gen/evidence/patched_{86,120}.cubin` | replaced cubins (independently disassemblable) |
| `test/sm120_l2_gen/T3_REPORT.md` / `T4_REPORT.md` | phase reports |
| `test/sm120_l2_gen/run_all_t3t4.ps1` | one-click reproduction script |
| `test/sm120_l2_gen/seal_t3t4.ps1` | evidence hash sealing script |
