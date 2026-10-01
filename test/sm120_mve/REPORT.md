# T6 报告：MVE 真机闭环验证结果（sm120 Level-2）

> **文档定位**：记录 T6 最小可行实验（guardian 挂载 → Deactivate →
> Reactivate）在 sm120 真机（RTX 5060）上的**执行结果、关键数据、
> 问题修复与结论**。设计见 `DESIGN.md`；T5 审查见 `T5_REVIEW.md`。
>
> 日期：2026-10-01 · 状态：**T6 完成**（45 项检查 ×3 轮全 PASS，
> exit=0；含 1000 轮压力循环）

---

## 1. 结论（一句话）

**XSched Level-2 的三步核心闭环在 sm120 上全部物理跑通**：
guardian 拼接执行 bit-exact、Deactivate 全拦/部分在飞拦截精确、
Reactivate 断点恢复 bit-exact、1000 轮循环零故障。L2-sm120 在机制
层面具备 go-live 条件。

## 2. 执行环境

- GPU：NVIDIA GeForce RTX 5060（sm_120 / CC 12.0），驱动 610.88；
- 工具链：nvcc 12.9.41（设备侧）、g++ 14.2.0 + MinGW（宿主侧）、
  libcuxtra_windows_amd64.a（经 `CUXTRA_CUDA_LIB` 绑定真驱动）；
- 用例全量运行 3 次（`evidence/mve_full_run{1,2,3}.log`）。

## 3. 结果总表（45 PASS / 0 FAIL / 0 ERR，三轮一致）

| 用例 | 检查项数 | 结果 | 关键实测数据（run3） |
|---|---|---|---|
| 环境与装载 | 3 | 全 PASS | CC 12.0；kernel image 4992B；入口 0x…（ASLR 随机） |
| Instrument（C1） | 5 | 全 PASS | regs 16→32；barrier 0→1；拼接 [50×16B][4992B] |
| M0 基线 | 1 | PASS | 1583.7 ms / 2048 块（≈50ms/块 × 32 波） |
| M1 正常路径 | 3 | 全 PASS | **bit-exact**；1536.9 ms（相对基线 ≈0 开销） |
| M2 全拦 | 5 | 全 PASS | out 未动；preempt_idx==idx；双标志全 1；返回 ~0ms（vs 1584ms） |
| M3 恢复 | 4 | 全 PASS | **bit-exact**；1541.1 ms；preempt/restore/exit 全清 |
| M4 部分在飞 | 4 | 全 PASS | **653 / 2048 完成，1395 被拦**；restore 数 == 被拦数（精确） |
| M5 断点恢复 | 2 | 全 PASS | **bit-exact**（补全 1395 块）；1052.7 ms |
| M7 三分支（K13） | 7 | 全 PASS | ①记 idx+restore ②只记 restore ③都不记；端到端 bit-exact |
| M8 形状 1×1×1 | 3 | 全 PASS | 单线程块全链路 |
| M8 形状 2×2×2 | 3 | 全 PASS | 多维 grid 全链路 |
| M8 形状 1024×256t | 3 | 全 PASS | 大 grid 全链路 |
| M6 压力 | 1 | PASS | **1000 轮 A→B→C1 收敛，6.5 s，0 故障** |

## 4. 关键能力验证明细

### 4.1 拦截精度（M4：部分在飞）

- 时序：guardian 入口异步 launch → 10ms 时 host 写入 flag=1
  （NON_BLOCKING stream，与 kernel 真并发）；
- 语义精确复现：“已过检查点的块继续完成；未上 SM 的块在入口被拦”：
  - run1：360 完成 / 1688 拦；run2：1208 / 840；run3：**653 / 1395**；
  - 三轮均满足 `restore_flags == 被拦块数`（逐块核对，不重不漏）。
- 波动来源：首波宽度随驱动调度（合法）。

### 4.2 断点恢复完整性（M3/M5）

resume 入口对每个块按 restore_flag 判定：已完成块 exit、被拦块跳回
guardian → flag=0 → 执行 body。恢复后与未插桩基线 **逐字节一致**
（2048×8B），完成后 restore/exit 标志全零（幂等）。

### 4.3 停转/恢复时延（参考数据）

| 指标 | 数值 | 说明 |
|---|---|---|
| 全拦停转（M2） | ~几毫秒（vs 完整 1584ms） | 2048 块入口检查+退出的纯开销 |
| 部分拦停转（M4） | ≈50ms + 尾波（首波 1 个 block 周期） | L2 语义：等待在飞块自然完成 |
| 恢复（M3 全量） | 1541ms | 与正常执行同量级（bit-exact 全量重跑） |
| 恢复（M5 增量） | 1052.7ms | 只重跑 1395 个被拦块（360 快退） |
| 插桩启动开销（M1） | ≈0（1537 vs 1584，±3%） | guardian 50 条前缀在 50ms 负载下不可测 |

### 4.4 压力（M6）

1000 轮 A→B→C1（64 块 / 2ms 级负载）：每轮 bit-exact + 状态断言，
6.5s 内完成，无 GPU fault、无驱动错误、无状态泄漏（每轮前全量重置）。

### 4.5 被测 kernel 画像（拼接语义的佐证）

`mve_kernel` 经 ptxas 12.9 优化/展开后为 312 条指令（0x1370，
4992B —— 与 `cuXtraGetBinary` 返回的字节数精确一致）：

- 入口用 sm120 的 **uniform 路径**（`S2UR/LDCU/UIMAD` 读 gridDim 与
  参数）——与 inject 产物的 LDC 形态不同，但同属自然编译产物；
- 含运行时循环展开的多分枝调度树（`BRA.U` 到 0x1180/0x1090 等）——
  **全部相对分支在拼接平移 800B 后语义不变**，这是 M1 bit-exact
  的直接含义；
- 全文件无“大立即数 MOV”形式的绝对地址（脚本核验 0 条），
  满足 §3 的拼接前提。

## 5. 可复现性

| 运行 | PASS | FAIL | ERR | 结论 |
|---|---|---|---|---|
| run1 | 45 | 0 | 0 | exit=0 |
| run2 | 45 | 0 | 0 | exit=0 |
| run3 | 45 | 0 | 0 | exit=0 |

check 行序列逐行一致（唯一自然差异为 M4 的运行时调度变量，
如“360 vs 653 vs 1208 完成数”——断言对任意 0<done<N 恒真）。

## 6. 过程中发现并修复的问题（工程记录）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | M4 中“中途写 flag”不生效，全量完成后才写 | 默认 blocking stream：同步 `cuMemcpyHtoD`（legacy default stream 语义）隐式等待 kernel | `CU_STREAM_NON_BLOCKING`（= 框架 InstrumentContext 的做法） |
| 2 | M8/M6“全拦”误判 | 检查前未重置 out，读到了上一段的完成值 | 拦截前 `MemsetD8(out, 0xCC)`（与 M2 同款纪律） |
| 3 | M7 第三分支无法区分“未记录/被保留” | 连续 K1+K2 场景状态叠加 | 改为状态注入隔离验证（forceState 前缀 + 单次 launch 观察写回） |

> 修复 #1 是本轮最有价值的一条：它不是测试瑕疵，而是**MVE 对框架
> 时序契约的真实复现**——正是 InstrumentContext 必须用 NON_BLOCKING
> stream 的原因（快照隔离 K8 的物理前提），MVE 把这个隐含约定显式
> 变成了一次可复现的故障与修复。

## 7. T5+T6 综合交付（对 T7 的输入）

- **sm120.h / sm120.cpp 已落盘并进 halcuda 库**（`-Werror` 全开通过）；
  T5 审查 21 断言 + T6 硬件 45 断言双重背书；
- **指令阵列即 T5 审查版**（gen_arrays.py 自动切片，无手抄）；
- T7 接线仅需：`arch.cpp` Guardianswitch 加 `case 120`（一行）；
  以及（可选，阶段三前）在 `CudaQueueCreate` 明确 sm120 不落
  Lv3Trap 路径（设计文档 T7 注记）；
- `RequiredRegs=32 / RequiredBarriers=1`（T5 §4 已核，与 instrument.cpp
  的保底一致，无需改动 level2 代码）。

## 8. 明确边界

- 本 MVE 验证的是**机制闭环**（单队列 / 单 kernel 函数 / 同 buffer）；
  多队列隔离、混合命令流、L1↔L2 切换属于 T7/T8 集成验证范畴；
- trap 路径（Level-3）与 CUDA Graph 不在本轮范围；
- Windows 平台 L2 的框架启用仍被 `#if defined(_WIN32)` 短路（T7/T8
  决策），MVE 证明的是机制本身在 Windows+标准驱动上已可用。

## 9. 证据索引

| 内容 | 位置 |
|---|---|
| 三份全量运行日志（UTF-8） | `evidence/mve_full_run{1,2,3}.log` |
| 节目录与设计 | `DESIGN.md` · `T5_REVIEW.md` · 本文件 |
| 主程序 / kernel / 数组切片 | `mve_main.cpp` · `dummy_kernel.cu` · `mve_arrays.h` |
| 一键构建 / 运行 | `build.ps1` · `run_mve.ps1` |
| 哈希封存 | `evidence/SHA256SUMS_T5T6.txt` |
