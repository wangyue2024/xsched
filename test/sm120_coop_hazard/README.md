# Cooperative-groups × XSched Level-2 Hazard Demo (sm120)

## 1. Purpose

XSched Level-2 preemption intercepts a kernel at an **entry checkpoint**
("guardian"): if the global exit flag is set, blocks that have not passed the
checkpoint `EXIT` immediately and are marked for replay; blocks that already
passed run to completion (block granularity, exactly-once replay).

A kernel launched as a **cooperative launch** may synchronize the whole grid
(`cooperative_groups::this_grid().sync()`). All blocks of the grid must arrive
at the barrier. If a Deactivate lands **while some blocks have already passed
the checkpoint and others have not**, the grid becomes **partial**:

* the entered blocks wait at the grid barrier for peers that will never come;
* the skipped peers exited at the checkpoint and, on resume, are treated as
  "already completed" (restore=0) and skipped again.

A real `grid.sync()` has no timeout: this state is a **permanent deadlock**,
and the resume path (Reactivate) reproduces it instead of repairing it.

Current mainline behavior (verified by code inspection, evidence branch):

| Launch path | Intercepted? | L2 instrumented? | Hazard |
|---|---|---|---|
| `cuLaunchCooperativeKernel` / `_ptsz` | export only, direct passthrough to the driver | no | scheduling blind spot (bypasses Suspend, ordering) |
| `cuLaunchKernelEx` + `CU_LAUNCH_ATTRIBUTE_COOPERATIVE` | yes (XQueue) | **yes, no attribute check anywhere** | **partial-grid deadlock window** |
| CUDA Graph containing cooperative kernels | graph falls back to L1 (existing `XWARN`) | no | safe |

This test proves the hazard state exists, reproduces it deterministically, and
quantifies the natural race window — **without ever risking a real hang**.

## 2. Safety design (never hang the GPU)

1. **Bounded grid barrier** (`coop_kernel.cu`): sense-reversing arrival
   semantics identical to `grid_group::sync()`, but waiters give up after a
   deadline (`%globaltimer`), record a diagnostic, and exit. A blocked partial
   grid therefore reports as data instead of deadlocking.
2. **Bounded companion**: the flag-setting companion kernel has a wait cap
   (2 s) and a pure-spin bounded phase.
3. **Watchdog runner**: `run_coop.ps1` runs the process with a hard timeout and
   kills it if it does not exit (second line of defense).

## 3. Files

| File | Role |
|---|---|
| `coop_kernel.cu` | test kernel (bounded grid barrier) + companion "Deactivate amplifier" |
| `coop_main.cpp` | host driver: guardian/resume splice (mirrors `InstrumentContext`), launch, experiments |
| `build.ps1` | device (`nvcc -cubin`) + host (`g++` + libcuxtra) stages |
| `run_coop.ps1` | watchdog runner, writes logs to `evidence/` |
| `../sm120_mve/mve_arrays.h` | guardian (50 instrs) / resume (32 instrs) arrays from the reviewed `sm120.cpp` |

## 4. Experiments

* **S1 — sanity / window measurement**: guarded full run (flag=0) through the
  real guardian splice, with `cuLaunchKernelEx` + cooperative attribute.
  Asserts: all blocks enter, barrier releases the full grid, no timeout, all
  complete. Also **measures the entry window** = spread of per-block
  body-entry timestamps (`%globaltimer`).
* **E1 — deterministic partial grid (the proof)**: craft the preempt buffer
  exactly as `Reactivate()` would read it after a mid-entry Deactivate
  (K blocks blocked: exit=1,restore=1; N-K blocks "entered": clean), relaunch
  through the **resume entry**. Expected: the K blocked blocks re-enter, the
  "entered" ones are skipped, and every re-entered block waits past its
  deadline on the missing N-K peers. Control (E1b): all-N restore flags set —
  the full grid re-enters, the barrier passes.
* **E2 — race amplifier**: companion kernel on a second stream waits for the
  first body-entry marker, dwells a swept delay (0..3 µs, ~40 ns steps), then
  writes the exit flag — a device-timed `Deactivate`. Partial outcomes
  (`0 < entered < N`) are counted as hits; result is classified as
  all-entered / all-blocked / no-flag per iteration.

## 5. Measured results (RTX 5060, sm120, CUDA 12.9, Windows; evidence/final & evidence/*)

| Experiment | Result |
|---|---|
| S1, 288×128t | full grid fine; **entry window ≈ 0.2 µs** |
| S1, 576×32t | full grid fine; **entry window ≈ 0.1 µs** |
| E1a (K=144/288) | entered=144, synced=0, **timedout=144** — every entered block waited on missing peers; skipped blocks provably never ran; kernel completed only thanks to the bounded barrier |
| E1b control | entered=288, synced=288, timeout=0 — safe even for a fully blocked kernel |
| E2 (2976+ iterations, both shapes) | `partial_hits=0`, all-entered=100%, `no_flag=0` — the companion always ran and wrote the flag, but its minimum latency (~1 µs) exceeds the ~0.1–0.2 µs window |

### Interpretation

* The **hazard state is real and permanent** (E1; with a real `grid.sync()`
  the 144 waiters would never resume — Suspend would never drain, and
  Reactivate relaunches a grid that is incomplete by construction).
* The **natural race window is sub-µs** for co-resident cooperative grids
  (all blocks dispatch nearly simultaneously), which makes the one-shot race
  rare (~10⁻³–10⁻⁴ per suspension event at ms-scale flag-landing jitter) —
  but it is nonzero, and the consequence is fatal. By contrast, ordinary
  kernels spread their entries over their whole runtime (waves), which is why
  ordinary partial in-flight is common (MVE M4) and benign (replay repairs
  it) — cooperative grids are the exact case where replay *cannot* repair.

### Harness lesson recorded

An early version (without an explicit cross-stream order between the buffer
reset and the launches) showed ~2% spurious "all-blocked" iterations: the
reset runs on the legacy stream while the launch streams are NON-BLOCKING, so
the companion could read a **stale entry marker** from the previous iteration
and fire the flag prematurely. Fixed by recording an event after the reset and
making both streams wait on it (`resetBuffers()`); 3000+ subsequent
iterations are clean.

## 6. Applicability to other architectures

The hazard is **not sm120-specific**:

* The mechanism is XSched's own L2 design — guardian splice + entry checkpoint
  + block-granular exactly-once replay — shared by all L2 implementations
  (sm86, sm70, sm35 guardians come from the same `inject.cu` pipeline with the
  same checkpoint semantics). Any architecture where L2 instrumentation is
  applied to a cooperative kernel has the identical window.
* The deadlock itself is CUDA semantics (`grid.sync` requires every block),
  independent of SASS. sm86 deployments on Linux that select L2 (e.g., via
  preempt-level configuration) carry the same risk; on Windows only sm120
  reaches L2 today, which is why this demo runs on sm120.
* The same demonstration would need only the corresponding guardian arrays
  (`mve_arrays.h`) swapped per architecture.

## 7. How to run

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1                 # device + host
powershell -ExecutionPolicy Bypass -File run_coop.ps1 -Mode all    # S1+E1+E2, 288 E2 iters
powershell -ExecutionPolicy Bypass -File run_coop.ps1 -Mode e2 -Threads 32 -Iterations 300
```

Requirements: RTX 5060-class sm120 GPU, CUDA 12.9, MSVC (for nvcc), MinGW g++,
`CUXTRA_CUDA_LIB=C:\Windows\System32\nvcuda.dll` (set by the runner).

## 8. Recommendation (aligned with the PR discussion)

Cooperative kernels should **never be L2-instrumented**, whichever API
launched them:

1. `cuLaunchKernelEx` with `CU_LAUNCH_ATTRIBUTE_COOPERATIVE`: detect the
   attribute (it is preserved in `CudaKernelLaunchExCommand::cfg_`) and skip
   instrumentation — fall back to L1 semantics, mirroring the existing
   CUDA-Graph fallback with an `XWARN`;
2. intercept the classic `cuLaunchCooperativeKernel` into the XQueue for
   ordering/coverage (as the context-isolation branch does) while applying the
   same skip;
3. optionally document the limitation in the README until the fix lands.
