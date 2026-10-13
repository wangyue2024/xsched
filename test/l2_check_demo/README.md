# l2_check_demo - Level-2 preemption verification, taught end-to-end

A minimal, self-contained program that answers one question with evidence:

> **"Is XSched Level-2 preemption working correctly on this machine?"**

It runs the *real* product path (shim nvcuda.dll + libpreempt + HAL), in
application-managed mode with an explicitly created Level-2 XQueue, and
checks the four observable invariants of Level-2. It is also a worked
example of *how to write and debug such a test* (see "Harness lessons").

## The four invariants

| # | Invariant | How it is observed |
|---|---|---|
| baseline | no preemption -> all blocks complete, values recorded as golden | sync + snapshot |
| I1 "withdraw" | suspend mid-flight leaves the kernel **partially done**: `0 < done < total` (some blocks finished, the rest were blocked at the guardian entry checkpoint) | `XQueueSuspend(kQueueSuspendFlagSyncHwQueue)`, then count markers |
| I2 "frozen" | while suspended, **no further block completes** | wait 20 ms, count again (equal) |
| I3a "correct" | after resume, results are **bit-exact** vs the baseline | element-wise compare |
| I3b "exactly-once" | every block executed its body **exactly once** (no loss from the withdrawal, no double execution from the replay) | per-block atomic execution counter == 1 |

Workload: 8 sequential kernels x 256 blocks (2048 block-units total), each
block writes `done[i] = 0xA5A5... | digest` and bumps `exe_cnt[i]`.

## Why it is built this way (harness lessons, all measured)

Writing this test surfaced four non-obvious pitfalls; each one is now a
design rule in this file (and worth remembering for any XSched test):

1. **Calibrate on a stream WITHOUT an XQueue.**
   A calibration launch routed through the queue measured 258 ms for work
   that takes ~4 ms of compute - the WDDM/queue round-trip overhead
   dominated. Calibration now runs on a plain passthrough stream
   (`s_cal`), with a two-round fixpoint (`calibrate()`).

2. **Sampling must not wait for the kernel - and the kernel must run on a
   NON-BLOCKING stream.**
   - With a default (blocking) stream, legacy-stream copies (`cuMemcpyDtoH`)
     implicitly wait for the kernel.
   - Even with a non-blocking stream, a *synchronous* `cuMemcpyDtoH` was
     observed to stall ~300 ms (WDDM flush variance), destroying mid-flight
     sampling.
   Fix: main stream created with `cudaStreamNonBlocking`; all snapshots use
   `cuMemcpyDtoHAsync_v2` on a dedicated observation stream (`s_obs`) +
   `cudaStreamSynchronize(s_obs)`.

3. **One giant kernel has a too-narrow "blockable tail".**
   The Deactivate flag's GPU-side landing time under WDDM is a lottery
   (microseconds to hundreds of milliseconds; MVE measured 288 ms - 2 s
   equivalents). With a single 2048-block kernel, if the flag lands late
   there is nothing left to block and I1 fails *flakily* (observed
   repeatedly). With 8 sequential kernels, any landing time still leaves
   un-started kernels that get blocked at the guardian -> I1 becomes
   robust. (T7 uses the same philosophy: many small kernels.)

4. **Do not stage helper work through the XQueue right before the
   workload.**
   - Resetting the observation arrays with host memsets showed a rare
     submission race (one run lost 180 counters -> false "exactly-once"
     failure). Fix: reset with a device kernel, ordered before the
     workload.
   - The *first* fix put that reset kernel through the XQueue; an early
     investigation concluded that this broke the subsequent Deactivate
     (exit flag landing after the whole workload). **That conclusion is
     WITHDRAWN**: the runs behind it turned out to be produced by a
     STALE binary (a later source edit failed to compile, the build
     error was hidden by log filtering, and the exe predated the edit
     by 1.5 h - the old version marker in the logs proved it). The
     queue-reset variant was never actually tested. The reset still
     runs on the plain observation stream + host sync (simplest hard
     ordering); with the fixed build, the multi-kernel workload was
     then truly validated: 3/3 runs, deterministic partial (256/2048).

5. **Verify the binary under test is the one you just built.**
   A silent compile failure (build output truncated by a log filter)
   once produced an hour of "results" from a stale executable, and the
   old-version marker in its logs proved it. Rules adopted: after each
   build, check the exe timestamp; after each run, check that the
   program's version-marker line matches the current source
   (`[L2] preemption run: 8 kernels x 256 blocks`); keep the marker in
   the program deliberately so staleness is always detectable.

## Files

| File | Role |
|---|---|
| `l2_check.cu` | kernel + host program (the whole test) |
| `build.ps1` | nvcc build, links `output/lib/nvcuda.lib` + cudart |
| `run.ps1` | stages the shim next to the exe, sets env, runs, logs to `evidence/` |
| `evidence/` | canonical validation logs: `l2_check_mc1..3_INFO.log` (post-fix multi-chunk runs, all PASS, deterministic 256/2048) + `l2_check_debug_DEBG.log` (Debug-build DEBG capture) + `SHA256SUMS_L2CHECK.txt` |

## Usage

```powershell
powershell -ExecutionPolicy Bypass -File run.ps1                      # build + run (INFO)
powershell -ExecutionPolicy Bypass -File run.ps1 -SkipBuild -Tag run2
powershell -ExecutionPolicy Bypass -File run.ps1 -SkipBuild -LogLevel DEBG
```

Environment set by `run.ps1` (per-app DLL proxying, no system files):

```
XSCHED_CUDA_LIB  = C:\Windows\System32\nvcuda.dll   # real driver for the shim HAL
CUXTRA_CUDA_LIB  = C:\Windows\System32\nvcuda.dll   # real driver for cuxtra
XSCHED_SCHEDULER = APP                              # we manage XQueues ourselves
XLOG_LEVEL       = INFO (or DEBG)
```

## Reading the result

A healthy run looks like this (numbers vary with machine load):

```
[calibrate] round 1: spin=1048576 -> 16.0 ms; target 15 ms/block -> spin=1048576
[base] baseline run (no suspend)
  [info] baseline wall time: ~350 ms
[L2] preemption run: 8 kernels x 256 blocks -> sleep 10 ms -> Suspend -> Resume
  [info] t=~20 ms (mid-flight): 0~90/2048 blocks done
  [info] Suspend(sync) took ~25-30 ms; 256/2048 blocks done     <- I1: partial
  [info] +20 ms while suspended: 256/2048 blocks done           <- I2: frozen
  [info] Resume -> sync took ~300 ms; total run ~380 ms
  [info] per-block body executions: miss=0 duplicate=0          <- I3b
  RESULT: 0 failed check(s)
```

## Viewing XDEBG / logs

- All XSched logs go to **stderr**, format:
  `[XSCHED <LEVEL> @ T <tid> @ hh:mm:ss.us] message @ file:line`.
- `XLOG_LEVEL` = `ERRO | WARN | INFO | DEBG` (default DEBG when unset).
- **Important:** in `Release` builds `XDEBG` is compiled out
  (`utils/include/xsched/utils/log.h`: `#ifdef RELEASE_MODE`). To see
  per-call debug logs (e.g. every intercepted `XLaunchKernel`), build the
  Debug flavor:
  ```powershell
  mingw32-make cuda BUILD_TYPE=Debug     # then restore with: mingw32-make cuda
  ```
- `run.ps1` writes the merged stdout+stderr to `evidence/` and prints the
  interesting lines (Suspend/Resume/XQueue/...) at the end.
- Quick manual filter:
  ```powershell
  Select-String -Path evidence\l2_check_run1_INFO.log -Pattern 'XSCHED'
  ```
