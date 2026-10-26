# A/B 实测证据（2026-10-26，完整版）

## 机器

| 机器 | GPU | 驱动 | 架构 | GSP | 备注 |
|---|---|---|---|---|---|
| 本地 | RTX 5060 | 595.99.02 | sm120 | on | 显示+计算 |
| 租用 A100 | A100-PCIE-40GB | 550.107.02（专有） | sm80 | on | 容器，无模块/ptrace 权限 |
| 租用 3090 | RTX 3090 | 570.124.04（开源） | sm86 | on | 容器，无模块/ptrace 权限 |

上游 `XpuOS/xsched` main，commit `f49289f`；CUDA 12.1 + g++11 + CMake 3.22。

## 核心结果

### 3090（sm86/570，带 shim）

| 配置 | 2000 µs block p50 | dead-loop p50 | dead-loop p99 | restore |
|---|---|---|---|---|
| level 1 | 13.38 ms | — | — | 8.2 µs |
| level 2 | **1.257 ms** | — | — | 57.3 µs |
| level 3 ×3 | 231.4 / 230.5 / 227.3 µs | 241.1 / 230.8 / 235.2 µs | 452 / 2826 / 420 µs | 93–127 µs |
| level 3 TSG | 13.35 ms | 180 s 超时 | — | 276.8 µs |

### 关键对照

| 实验 | 结果 |
|---|---|
| 3090 L3 **去 shim** | 10 µs block → 7.2 ms；60 µs → 9.7 ms；500 µs → 34.0 ms（restore 26.5 ms） |
| 3090 **正确性** | 16M int `C=A+B`，200 次 suspend/resume，`bad=0 -> OK`，stream no error |
| 3090 vs A100 的 cuda-gdb | 都是 `0307 → 0317{1,0} → 0315/030c/0316 → 0318`；attach 0.047/0.159 s |
| A100 L3 带 shim | SIGSEGV in `cuXtraKernelGetFunction`（libcuda 550） |
| A100 L3 无 shim | 300 µs p50 33.6 ms；400 µs p50 46.4 ms；restore 25.8 ms |
| A100 TSG 无 shim | 10–40 µs p50 0.58–1.49 ms；900 µs p50 55.7 ms |
| A100 sm80→SM86 补丁 | 同一个 cuxtra 崩溃；不能证明 SASS 兼容 |
| cuxtra 版本 | 上游子模块与本地库 md5 相同；崩溃是 550 兼容性，不是拿错版本 |

### handler 实物

| 机器 | size | +0x3e0 words |
|---|---|---|
| 3090 sm86 | **0x1200** | `fffffff001017810 004fe40007ffe0ff`（IADD3 R1,R1,-0x10,RZ） |
| A100 sm80 | **0x1200** | 同上 |

上游 `TarpHandlerSM86::Instrument` 在 `handler+0x3e0` 写 JMP inject_dev，注入
`trap_inject_instrs` + 原指令 + 跳回；注入大小 = `sizeof(trap_inject_instrs)+32`。

## 文件

| 文件/目录 | 内容 |
|---|---|
| `battery_3090/` | level1/2/3×3/TSG 完整日志 |
| `correct_sm86.log` | 200 次抢占正确性测试 |
| `sm86_handler_dump.log` / `sm86_handler.bin` | sm86 handler 实物（0x1200） |
| `a100_handler_dump.log` / `a100_handler.bin` | sm80 handler 实物（0x1200） |
| `level3_sm86_noshim.log` | 3090 去 shim 的慢路径 |
| `level3_sm80_noshim.log` | A100 去 shim 的慢路径 |
| `level3_sm80patch.log` / `level3_sm80patch_gdb.log` | A100 带 shim 的 segfault + 回溯 |
| `level3_tsg_a100.log` / `level3_tsg_a100_noshim.log` | A100 TSG 带/不带 shim |
| `sm86_agent_ioctl_0317.log` | 3090 cuda-gdb attach 的 0317 序列 |
| `logs/` | 早期 level1/3、A100/3090 regops、smi/uname、A100 的 0317 序列 |
| `upstream/` | 上游参考源码 + **完整源码快照**（`xsched-main-f49289f.tar.gz`、`xsched-ab-a100-sm80.tar.gz`），`arch_sm80.patch`、`ipc_disable_tests.patch`、`UPSTREAM_INFO.md`、`repro_ab.sh` |
| `logs/env_a100.txt` / `logs/env_3090.txt` | 两台机器的完整 OS/驱动/GSP/工具链/权限/md5 |
| `drivers/` | 550/570 的 `libcuda` + `libcudadebugger`（静态逆向用；重建 L3 不必需），含 md5 |

## 结论（三段耦合）

```
managed launch（shim/debugger params） + instrumented handler（+0x3e0） + 触发（0x20800122）
  3090: ✅ + ✅ + ✅ -> 229 µs，正确性 OK
  3090: ❌ + ✅ + ✅ -> 6–40 ms
  A100: ⚠️ + ✅ + ✅ -> 带 shim 崩 / 无 shim 30–55 ms
  sm120: ✅ + ✅ + ❌ -> 0317 27 ms / GDB 116 ms
```

论文 L3 在 **3090（sm86/570）** 上完整复现；A100 受 cuxtra/550 兼容性限制；
sm120 受 `pGpu+0x911` 触发闸门限制。详见 `../../FINDINGS_ZH.md §49–§50`、
`../../compendium/07-A-B-完整实测.md`。
