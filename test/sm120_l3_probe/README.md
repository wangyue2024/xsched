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

---

## Linux follow-up session (2026-10-09) — sm120 L3 implementation + findings

Environment: same RTX 5060 (sm120), **Linux**, driver 595.99.02 (CUDA 13.2,
open kernel module), `/dev/nvidiactl` accessible. CUDA 12.9.41 toolkit used
for all device compilation (`nvcc -ccbin g++-13`; the local CUDA copy needed
a glibc-C23 `noexcept` compat patch for `sinpi/cospi/rsqrt` declarations).

### What was built (branch worktree, not committed)

| Area | Change |
|---|---|
| `hal/arch/sm120.cpp/.h` | `TarpHandlerSM120`: sm120 trap-inject array (40 instrs, generated from `check_preempt_trap` with the sm86 composition recipe), `SetJumpInstruction`, `Instrument` at stub offset **0x880**, and a `GetTrapHandlerInfo` override |
| `hal/level3/trap.h` | `TarpHandler::GetTrapHandlerInfo` virtual (default = cuxtra Pascal path) |
| `hal/level3/interrupt.cpp` | uses the virtual getter; L3 diagnostics via XINFO |
| `hal/arch/arch.cpp` | sm120 → `CudaQueueLv3Trap` on Linux (Windows still Lv2) |
| `tools/instrument` | inject_120 regenerated with 12.9.41: guardian(50)/resume(32) arrays **bit-identical** to the committed ones; trap arrays generated (`check_preempt_trap` 56, `exit_if_idempotent` 40) |

### GetInfoBlackwell workaround (works, live-verified)

`cuXtraGetTrapHandlerInfo` aborts on sm120 (error 101, same as Windows).
Replacement implemented in `TarpHandlerSM120::GetTrapHandlerInfo`:
`cuGetExportTable(CU_ETID_ToolsTrapHandler)` → slot 20 `fn(ctx, out40)`
→ `ObjGetPc(ctx, out.handle, &pc)` / `ObjGetSize(out.handle, &sz)` via the
`CU_ETID_ToolsMemory` table (slots 24/25, handle passed **by pointer value**).
Yields `pc = 0x…5f5300`, `size = 0x1300`, stub offset field `+0x18 = 0x880`.

### Handler reverse engineering (evidence/linux_2026-10-09)

- Handler container = 0x1300 bytes: variant A code `[0,0x880)`, shared return
  stub `[0x880,0xA00)` (`CCTL.IVALL/MEMBAR.SYS/RET.ABS R12 0x20`), zero pad,
  variant B at `[0xA80,0x1300)`.
- The stub starts with two NOPs; every variant-A path reaches it
  (`@P0 BRA.U 0x880` after the `LOP3 P0=R2&0x20` dispatch, and by fall-through).
- Patch site chosen: **0x880** (first NOP). Write path verified end-to-end:
  DtoH → patch → `cuXtraMemcpyHtoD` → read-back is byte-exact.

### Trigger on Linux (works)

`cuXtraTriggerTrap` (RM `GlobalRegsWrite32(0x419e84, bit31)` via nvidiactl
ioctl) returns instantly for **idle and busy** GPU and leaves the context
healthy (`final_trigger_probe.log`). This is the piece that is fatally
blocked on Windows.

### The blocker found: forced trap does not reach the tools handler

With the correctly patched handler (verified read-back) and a successful
trigger, spinning wait-flag kernels are **not** affected: the injected code
never runs. A diagnostic minimal payload (store signature to a host-visible
address) patched at **0x0 / 0x60 / 0x880** all show `sig == 0` on trigger
(`mini7_off_*.log`), i.e. the trap handler at `pc` is not entered at all.

Hypotheses (next step, T4-style gap):
1. The slot-20 memobj may be a **template**; the driver may need an explicit
   arm/install step for forced traps (the zero-returning slots 1/5/8/10/16/22
   from the Windows sweep are prime candidates for that call).
2. Driver 595 (CUDA 13.2) may route `TRIGGER_TRAP` differently than 13030.
3. Trap-context memory-access restrictions could mask a faulted payload —
   less likely: a faulting payload would kill the warps and unblock suspend.

Until that is resolved, `arch.cpp` routes sm120 to `CudaQueueLv3Trap` on
Linux for development, but **e2e preemption is not yet functional**; the TSG
path (`XSCHED_CUDA_LV3_IMPL=TSG`) works on this setup (timeslice ioctl OK).

### Side note

`platforms/cuda/test/main/level.cu` hangs on this Linux setup already at
level 1 in its runner loop (64-launch + stream-sync storm through the shim);
minimal reproductions (single-thread, worker-thread, 64-backlog) all pass,
so the harness issue is separate from L3 and still open.

### Round 2 (same day) — root-cause deep dive: why the forced trap never arrives

Follow-up experiments (evidence in the same directory; test sources `mini9/12/13.cu`,
`ioctl_log*.c`):

1. **Trigger constants are correct.** Disassembly of Linux cuxtra and a byte scan of
   `libcuda.so.595` show both use the identical `{reg=0x419e84, val=0x80000000}`
   write (`cuXtraTriggerTrap` == the driver-internal helper at libcuda+0x487560).
   The trigger is not the problem.

2. **The handler memobj is a NON-LIVE template.** With a minimal signature payload
   patched at three offsets (0x0 dispatch / 0x60 save path / 0x880 stub):
   - RM trigger during 64 spinning blocks: payload never runs (`mini7_off_*.log`).
   - **Genuine trap** (`brkpt` kernel, `mini9`): kernel dies with
     `cudaErrorIllegalAddress (700)`, payload never runs either — the trap is
     handled internally by the driver, not through the slot-20 object.
   - Same result when the whole flow runs under a real `cuda-gdb -batch` session.
   Conclusion: on sm120/driver 595 the tools trap-handler image (variant A + stub +
   variant B) is not the live dispatch target; the sm86-era "patch memobj + write
   RM bit" recipe cannot work as-is here.

3. **The real activation machinery is the CUDA-debugger attach stack.** An
   LD_PRELOAD RM-ioctl logger (`ioctl_log2.c`) was used to diff a normal run vs a
   `cuda-gdb` run of the same binary:
   - The debugger creates a **dedicated RM client**, allocates a **class-0x83de
     object** (hParent = device 0x5c000002), and issues a debugger-only RM-control
     method family `0x83de03xx` (07/0c/15/16/17/18/1f/2a). Payloads captured:
     `0315/0316` = {new RM handle, region size, host pointer} (module debug-region
     registration), `0317/0318/0307/031f/032a` = flags/commands, `030c` = 4824-byte
     structured buffer.
   - libcuda exports `cudbgDebuggerCapabilities`, `cudbgDebuggerInitialized`,
     `cudbgEnablePreemptionDebugging`, `cudbgUseExternalDebugger`,
     `cudbgInitiateDebuggerAttachProcedureFd`; libcudadebugger implements the
     attach via a protobuf agent protocol ("DebuggeeAttach", fd handshake).
   - Re-enabling the sm120 trap path therefore requires reproducing (at least) the
     attach subset of this machinery — a substantially larger RE/implementation
     task than the sm86-era interface. All raw logs are in this directory
     (`ioctl_normal.log`, `ioctl_cudbg.log`, `ioctl2_cudbg.log`).

4. **TSG path status.** `SetTimeslice(ctx,0)` write is real (readback = 0;
   `mini12/13`); however two observations qualify its use on this setup:
   - A suspended context's own sync can never complete for eternally-spinning
     kernels (TSG pauses, never kills) — `XQueueSuspend(SyncHwQueue)` is the wrong
     pattern for TSG L3.
   - With `XSCHED_CUDA_LV3_IMPL=TSG`, cross-context contention tests
     (`mini13`, saturated 2048x256 spinning victim) show the default inter-context
     scheduler already gives a competing context full SM access; timeslice tuning
     produced no measurable delta on this GPU/driver. The TSG preemption value
     (and its per-context vs per-channel granularity) needs a multi-process or
     multi-GPU-device scenario to be exercised meaningfully.
