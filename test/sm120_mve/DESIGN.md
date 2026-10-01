# T6 MVE 设计文档：sm120 Level-2 最小可行实验

> **目标**：在接入 XSched 框架之前，用 driver API 直调（不经过 shim /
> preempt / sched 框架）在 sm120 真机上独立验证 Level-2 机制的完整
> 闭环：**guardian 挂载 → Deactivate（拦停）→ Reactivate（恢复）**。
> 这是 L2-sm120 的 go-live 判据（设计文档 T6）。
>
> 日期：2026-10-01 · 状态：设计已实施并全部通过（见 `REPORT.md`）

---

## 1. 被测契约（与框架逐条对应）

MVE 的每一段逻辑都直接镜像 `platforms/cuda/hal/src/level2/instrument.cpp`
与 `arch/sm120.cpp`（数组），不引入任何框架代码：

| # | 框架逻辑 | MVE 对应实现 |
|---|---|---|
| C1 | `InstrumentContext::Instrument()`：GetEntryPoint → GetBinary → Alloc(guardian+kernel) → 两次 HtoD → regs≥32 / barrier≥1 → InvalInstrCache | `instrumentKernel()`（mve_main.cpp） |
| C2 | `InstrumentContext::Launch()`：28B args（buf/guardian/idx/killable）→ SetDebuggerParams → SetEntryPoint → LaunchKernel → 复位入口 | `mveLaunch()` |
| C3 | `InstrumentManager::Deactivate()`：`global_exit_flag = 1` | `drvMemcpyHtoD(d_buf, &one, 4)` |
| C4 | `InstrumentManager::Reactivate()`：读 preempt_idx → 清 16B header | `clearHeader()` + resume launch |
| C5 | `InstrumentManager::Launch()` 的 launch_type 决策（idx==preempt_idx → resume） | 用例显式选择 guardian / resume 入口 |
| C6 | 窗口偏移（T1 实证 0x170 系列） | sm120.cpp 数组内的 patched LDC 直接携带 |

**指令数组的唯一来源**：`gen_arrays.py` 从 T5 审查过的
`platforms/cuda/hal/src/arch/sm120.cpp` 切片生成 `mve_arrays.h`——
无手抄、无漂移。

## 2. 机制模型（MVE 对每个 block 的可观测状态）

preempt buffer（16 + 8×N 字节，N = block 数）：

```
|<-- u32 global_exit_flag -->|<-- u32 rsvd -->|<-- u64 preempt_idx -->|
|<-- u32 exit_b0 -->|<-- u32 restore_b0 -->|<-- u32 exit_b1 -->| ...
```

- **完成标记**：`out[bid] != 0xCC…`（host 预填 0xCC，块完成后写
  `0xA5A5A5A5… ^ bid ^ acc`）；
- **被拦块**：`restore_flag[bid] == 1` 且 `out[bid]` 未写；
- **通过检查点**：`exit_flag[bid] == 0`（flag=0 路径主动清零）。

## 3. dummy kernel 的设计约束（为何这样写）

| 约束 | 原因 |
|---|---|
| 普通编译（无 astoolspatch） | 必须是可 launch 的 entry function——guardian 拼接的输入形态 |
| 无 device 全局变量 / printf / runtime 调用 | `cuXtraGetBinary` 取回的 image 必须**不含绝对地址**——拼接（前移 800B）只平移相对分支，语义不变 |
| 输出为 (bid, spin) 的确定性函数 | 允许与未插桩基线做 **bit-exact** 比较 |
| 纯 LCG 忙循环 | 时长可调（校准），数据依赖链不可被优化器消除 |
| 每块独立（无块间通信） | block 粒度即抢占粒度；部分在飞场景可精确核对每块状态 |

## 4. 用例矩阵与判据

| 用例 | 操作 | 判据（全 PASS 才通过） |
|---|---|---|
| M0 基线 | 不插桩直接 launch（2048 块） | 全部块完成；记录 golden 与 wall time |
| M1 正常路径 | guardian 入口，flag=0 | out bit-exact；preempt_idx==0；标志全清 |
| M2 全拦 | flag=1 后 guardian 入口 | out 全 0xCC；preempt_idx==idx；exit/restore 全 1；快速返回（<500ms） |
| M3 恢复 | 清 header → resume 入口 | out bit-exact；preempt_idx==0；restore/exit 全清 |
| M4 部分在飞 | guardian 入口（异步）→ 10ms 时 host 写 flag=1 → sync | 0 < 完成数 < 2048；preempt_idx==idx；restore 数 == 2048−完成数 |
| M5 断点恢复 | 清 header → resume 入口 | out bit-exact；restore 全清 |
| M6 压力 | 1000 轮 A→B→C1（64 块小规模） | 每轮 bit-exact + 状态断言；无 fault |
| M7 三分支（K13） | 隔离注入状态：①preempt_idx=0 ②=idx ③=更晚 | ①记 idx+restore ②只记 restore ③都不记；+端到端 block→resume bit-exact |
| M8 边界形状 | 1×1×1/1t、2×2×2/128t、1024/256t | 每种：基线完整、全拦、resume bit-exact |

## 5. 关键设计决策（过程记录）

1. **stream 必须 NON_BLOCKING**（M4 的物理前提）：host 中途写 flag 用
   同步 `cuMemcpyHtoD`——若 kernel 跑在默认（blocking）stream 上，
   legacy-default-stream 语义会让该拷贝隐式等待 kernel 完成，
   “中途”变“事后”。此问题在首轮实测中被捕获并修正；修正后
   M4 立即复现“部分完成”语义（360/2048、653/2048、1208/2048 三次
   运行，均满足 0<done<N）。
2. **flags 检查前必须重置 out**（M2 同款纪律推广到 M8/M6）：否则上
   一段的完成值会把“被拦块数”读数污染。首轮 M8/M6 的全部失败均
   源于此，修正后消失。
3. **三分支用状态注入隔离验证**：连续 K1+K2 无法区分“K2 未记录”
   与“K1 已记录被 K2 保留”；改为注入确定前缀状态（flag=1 + 选
   preempt_idx + 干净 flags）后观察单次 launch 的写回，逻辑封闭。
4. **时序策略**：单块时长 ≈50ms（校准），中途写 flag 延迟 10ms——
   写入点距“首波启动”与“首波完成”各有 ~5× 余量；完成/被拦数
   随调度波动（360~1208），断言只依赖恒真不等式。
5. **忙循环防优化**：LCG 链 + 结果 XOR 进最终写值；`spin` 由校准
   生成（101 万 ~ 445 万轮），无 `volatile` 需求。

## 6. 环境与复现

```powershell
cd test\sm120_mve
powershell -ExecutionPolicy Bypass -File build.ps1     # gen + device + host
powershell -ExecutionPolicy Bypass -File run_mve.ps1   # 全量（含 1000 轮）
powershell -ExecutionPolicy Bypass -File run_mve.ps1 -Quick   # 快速（跳压力）
```

前置：CUDA 12.9 工具链、MSVC Build Tools（nvcc host）、g++（MinGW）、
`CUXTRA_CUDA_LIB` 由脚本自动指向 `C:\Windows\System32\nvcuda.dll`。

## 7. 目录清单

| 文件 | 作用 |
|---|---|
| `dummy_kernel.cu` | 被抢占的负载 kernel（sm_120，普通编译） |
| `mve_main.cpp` | MVE 主程序（driver 直调 + cuxtra） |
| `mve_arrays.h` | **生成物**：从 sm120.cpp 切片的指令数组 |
| `gen_arrays.py` | mve_arrays.h 生成器（防手抄漂移） |
| `build.ps1` / `run_mve.ps1` | 构建 / 运行（UTF-8 证据日志） |
| `t5_verify.py` | T5 数组结构审查器（21 断言） |
| `T5_REVIEW.md` / `REPORT.md` | 审查报告 / 结果报告 |
| `evidence/` | 运行日志与封存哈希 |
