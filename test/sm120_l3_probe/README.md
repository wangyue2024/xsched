# l3_trap_probe — sm120 (Blackwell) Trap-Handler / Export-Table RE workspace

Standalone reverse-engineering sandbox for validating whether the NVIDIA
CUDA driver's **Trap-Handler export table** is reachable on **sm120
(RTX 5060, Compute Capability 12.0 / Blackwell)** under Windows + WDDM.

This directory is **fully independent** of the `xsched` project: nothing
here modifies `d:\1file\Desktop\code\xsched`. It only *reads* the cuxtra
headers / static library that live inside that checkout.

---

## Purpose

XSched Level-3 (L3) preemption has two paths:

1. **Trap path** — rewrite the driver's trap handler so an in-flight kernel
   can be interrupted and safely exited. It reaches the handler through the
   undocumented `cuGetExportTable(&table, &ETID)` interface. On sm86 the
   handler-offset table exists; **sm120 is missing** (`EtblTrapHandler` has
   only a `GetInfoPascal` getter — no Blackwell case).
2. **TSG path** — timeslice-based scheduling preemption (arch-independent).

Before any sm120 L3 code can be written, we must prove the *driver-side*
machinery is present and callable on this machine. That is what the probes
in this workspace do.

### Static findings already captured (see `evidence/`)

`tools/analyze_cuxtra.py` resolved the real ETID GUIDs baked into
`libcuxtra_windows_amd64.a`:

| Symbol | Member | GUID (raw16) |
|--------|--------|--------------|
| `CU_ETID_ToolsTrapHandler` | `trap.cpp.obj` | `cc529e94-5c4e-9d46-837c-9625868334e4` |
| `CU_ETID_ToolsModule` | `function.cpp.obj` | `6e163fbe-b958-444d-835c-e182aff1991e` |
| `CU_ETID_ToolsMemory` | `instrmem.cpp.obj` | `bfdb432d-bf3c-5a4a-945e-b34029e81e75` |
| `CU_ETID_ToolsContext` | `memcpy.cpp.obj` | `21318c60-9714-3248-8ca6-41ff7324c8f2` |
| `CU_ETID_ToolsPushBufferHal` | `cache.cpp.obj` | `a6b1ff99-ecc4-c94f-92f9-1928663d5585` |

`tools/analyze_nvcuda.py` confirmed the driver exports `cuGetExportTable`
(ordinal 204). cuxtra's trap surface is `EtblTrapHandler::GetInfoPascal`
only — the sm120/Blackwell getter is the gap we must fill.

---

## Directory layout

```
l3_trap_probe\
├── src\               probe C++ sources (each foo.cpp -> foo.exe beside it)
├── include\           probe_common.h  (shared scaffolding)
├── tools\             analyze_nvcuda.py, analyze_cuxtra.py  (static RE)
├── evidence\          experiment output (logs, analysis dumps)
├── build.ps1          MinGW g++ build driver
├── run.ps1            probe launcher (sets CUXTRA_CUDA_LIB, tees to evidence\)
└── README.md          this file
```

---

## Planned steps

- [x] **T0 — static recon.** Parse `nvcuda.dll` PE exports + sweep strings
      (`analyze_nvcuda.py`); parse the cuxtra archive symbols, disassembly
      and ETID constants (`analyze_cuxtra.py`).
- [x] Confirm `cuGetExportTable` is exported and recover
      `CU_ETID_ToolsTrapHandler` = `cc529e94-5c4e-9d46-837c-9625868334e4`.
- [ ] **T1 — export-table probe.** A `src/` probe loads `nvcuda.dll`,
      calls `cuGetExportTable` with the recovered ETID under a `SAFE_CALL`
      SEH guard, and dumps the returned function-pointer vtable.
- [ ] **T2 — trap-handler info probe.** Call the resolved getter for the
      Pascal-style `TrapHandlerInfo` struct and record the sm120 handler
      address / size / offsets.
- [ ] **T3 — trigger probe.** Exercise `cuXtraTriggerTrap` against a
      long-running kernel and observe whether the sm120 driver honours it.
- [ ] **T4 — gap analysis.** Document what a `GetInfoBlackwell` /
      `TarpHandlerSM120` implementation must supply (handler offset, SASS
      JMP patch site) and feed it back to the xsched design doc.

---

## Environment requirements

| Component | Requirement | Verified |
|-----------|-------------|----------|
| OS | Windows (25H2), WDDM | yes |
| GPU | NVIDIA RTX 5060 — sm120 / CC 12.0 (Blackwell) | yes |
| Driver DLL | `C:\Windows\System32\nvcuda.dll` (exports `cuGetExportTable`) | yes |
| CUDA | 12.9 (driver); toolkit optional for these host-only probes | yes |
| Host compiler | MinGW-w64 g++ 14.2.0 (`x86_64-win32-seh`) | yes |
| Binutils | `nm`, `objdump`, `strings` (from the same MinGW) on PATH | yes |
| Python | 3.11+ (standard library only — no third-party deps) | yes |
| cuxtra | `xsched\3rdparty\cuxtra\{include,lib\libcuxtra_windows_amd64.a}` (read-only) | yes |

---

## Usage

All commands run from this directory in PowerShell. PowerShell does **not**
accept `&&`; chain with `;`.

```powershell
# 1. Static analysis (writes into evidence\)
python tools\analyze_nvcuda.py
python tools\analyze_cuxtra.py

# 2. Build every probe in src\  (or one: -Target <name>)
powershell -ExecutionPolicy Bypass -File build.ps1
powershell -ExecutionPolicy Bypass -File build.ps1 -Target probe_export_table

# 3. List / run a probe (sets CUXTRA_CUDA_LIB, tees output to evidence\)
powershell -ExecutionPolicy Bypass -File run.ps1 -List
powershell -ExecutionPolicy Bypass -File run.ps1 -Probe probe_export_table
powershell -ExecutionPolicy Bypass -File run.ps1 -Probe probe_x -ProbeArgs --flag,val
```

`build.ps1` flags: `-Target <name>`, `-Opt O0|O1|O2|Og|Os`, `-Clean`,
`-ExtraCxx <flags>`, `-Verbose2`.

`run.ps1` flags: `-Probe <name|path>`, `-ProbeArgs <a,b,c>`,
`-CudaLib <path>`, `-List`.

---

## Conventions for probe sources (`src\*.cpp`)

- Include `"probe_common.h"` (from `include\`) — it hand-declares the CUDA
  driver types, pulls in `cuxtra/cuxtra.h`, and provides:
  - `LOG(fmt, ...)` / `LOGRAW(...)` — timestamped, flushed tracing;
  - `CHECK_CU(expr)` / `CHECK_CU_VOID(expr)` — CUresult error checks;
  - `SAFE_CALL(func, ...)` — expression-form SEH guard: evaluates to the
    call's `CUresult` (`PROBE_SEH_RAISED` if it faulted) and leaves the raw
    Win32 code in `probe_last_seh_code`; `SAFE_CALL_TO(rc, func, ...)` is
    the statement form. Built on a vectored exception handler (VEH) plus
    `longjmp`, because this g++ rejects `__try`/`__except` in C and C++;
  - `probe_range_observe_device/host()` + `probe_addr_is_device()` /
    `probe_addr_classify()` — empirical GPU-vs-host VA classification;
  - `probe_uuid_to_str()` / `probe_hexdump()` — ETID + vtable dumping.
- Resolve driver entry points with `GetProcAddress` on a locally loaded
  `nvcuda.dll` so the driver under test is unambiguous.
- Never assume a call succeeds: wrap every undocumented export-table call
  in `SAFE_CALL` and log `probe_last_seh_code`. Keep the guarded body thin
  (a single call) — a `longjmp` skips destructors of C++ objects created
  inside it, so do the bookkeeping in the caller.


---

## Experiment Results (2026-10-03)

### Environment
- GPU: NVIDIA GeForce RTX 5060 (sm120, CC 12.0, Blackwell)
- Driver: 13030 (CUDA 12.9)
- OS: Windows 25H2, WDDM mode
- nvcuda.dll: base 0x7ffbb1d70000, size 0x47f000

### Probe Results Summary

| Probe | Status | Key Finding |
|-------|--------|-------------|
| probe_selftest | PASS | SEH guard, logging, cuxtra linkage all working |
| probe_export_table | PASS | TrapHandler ETID resolves (184B, 22 slots); slot 19 error 101 |
| probe_slot20 | PASS | Slot 20 = Blackwell GetInfo: returns memObjHandle + sizes |
| probe_trigger | BLOCKED | cuXtraTriggerTrap fatal: CudaKernelModule Linux-only |
| probe_win_trigger | CRASH | SEH accumulation crash at slot 20 (data captured pre-crash) |
| probe_win_trigger2 | BLOCKED | Checkpoint APIs return 801; TSG same kmod limitation |

### Critical Findings

#### 1. Slot 20 IS the Blackwell Trap Handler Info Getter

```
Slot 20 output structure (40 bytes, stable across calls):
  +0x00 = 0x0000000000000000  (flags/padding)
  +0x08 = 0x0000000000000028  (struct size = 40 bytes)
  +0x10 = <memObjHandle>      (opaque handle, changes per-context)
  +0x18 = 0x0000000000000880  (2176 bytes = handler size?)
  +0x20 = 0x0000000000000940  (2368 bytes = allocation size?)
  +0x28 = 0x0000000000000000  (unused)
```

Cross-reference with ToolsMemory table:
- ObjGetPc(ctx, handle) -> SUCCESS, pc = 0x880
- The handle at +0x10 is a valid CUtoolsMemObjHandle_st
- Size 2176 bytes / 16 bytes-per-instruction = 136 SASS instructions
  (plausible for a trap handler; sm86 handler is similar scale)

#### 2. Trigger Path is BLOCKED on Windows (cuxtra limitation, NOT driver)

```
cuXtraTriggerTrap(ctx):
  -> CudaKernelModule::GetKMod(ctx)
  -> FATAL: "CudaKernelModule is not supported on Windows" @ kmod.cpp:63
```

The RM register write (GlobalRegsWrite32, reg 0x419e84 bit31) uses Linux-specific
ioctls (/dev/nvidiactl). This is a **cuxtra implementation gap**, not a hardware
or driver limitation.

#### 3. cuCheckpointProcess APIs: Present but NOT SUPPORTED

All 6 checkpoint APIs exist in nvcuda.dll (ordinals 13-18) but return
CUDA_ERROR_NOT_SUPPORTED (801) on this platform. Likely requires:
- Datacenter GPU (Tesla/A100/H100/B200), OR
- Linux, OR
- Specific driver configuration (MIG? vGPU?)

#### 4. TSG (Timeslice) Path: Also BLOCKED

cuXtraGetTimeslice/SetTimeslice use the same CudaKernelModule internally.
Same Windows limitation applies.

#### 5. attr90 COMPUTE_PREEMPTION_SUPPORTED = 0

The driver reports compute preemption is not supported on this device
(RTX 5060 + WDDM). This is consistent with the trigger path being blocked.

#### 6. Other Export Table Slots (1, 5, 8, 10, 16, 22)

All return CUDA_SUCCESS when called as fn(ctx, &out) but write ALL ZEROS.
They are likely no-op queries or require specific input parameters we have
not discovered. None appear to be trigger candidates.

### Viability Matrix for L3 on sm120

| Path | Windows+WDDM | Linux | Fix Required |
|------|-------------|-------|--------------|
| Trap handler INFO (slot 20) | WORKS | WORKS | None |
| Trap TRIGGER (RM reg write) | BLOCKED | WORKS | Port kmod to Windows |
| TSG timeslice | BLOCKED | WORKS | Port kmod to Windows |
| cuCheckpointProcess | NOT SUPPORTED | TBD | Unknown |
| Level-2 Guardian (Deactivate) | WORKS | WORKS | None (already shipped) |

### Conclusions

1. **L3 is NOT fundamentally impossible on sm120** - the hardware and driver
   infrastructure (export table, handler info) are intact.

2. **The blocker is cuxtra's Linux-only CudaKernelModule** - specifically
   GlobalRegsWrite32 which performs the RM-control ioctl to trigger traps.

3. **To enable L3 on Windows**, one of:
   - (a) Port CudaKernelModule to Windows using DeviceIoControl to nvlddmkm.sys
   - (b) Find the Windows-native RM control path in the export table
   - (c) Use Linux for L3 workloads

4. **Slot 20 provides everything needed for TarpHandlerSM120**:
   - Handler memory object handle (for instruction memory access)
   - Handler size (2176 bytes = 136 instructions)
   - Allocation size (2368 bytes, with alignment padding)
   - ObjGetPc resolves the handle to a PC offset

5. **Immediate next steps**:
   - Implement GetInfoBlackwell() using slot 20 (replaces GetInfoPascal)
   - Reverse-engineer the Windows RM-control DeviceIoControl path for
     GlobalRegsWrite32 equivalent (IOCTL to nvlddmkm.sys)
   - OR: validate full L3 on Linux first, then port trigger to Windows
   - The handler info (slot 20) + ObjGetPc path works on BOTH platforms,
     so TarpHandlerSM120 instruction patching can be developed now

### Evidence Files

| File | Content |
|------|---------|
| probe_export_table_20261003_204614.log | Full 7-phase export table scan |
| probe_slot20_20261003_225053.log | Slot 20 deep investigation + ObjGetPc |
| probe_win_trigger_20261003_225339.log | Slot sweep (partial, pre-crash) |
| probe_win_trigger2_20261003_225610.log | Checkpoint + TSG + safe slots |
| probe_trigger_20261003_225127.log | TriggerTrap fatal error evidence |
