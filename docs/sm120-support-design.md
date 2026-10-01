# XSched sm120（Blackwell）支持设计文档

> 版本：v1.0
> 范围：CUDA 平台 HAL（`platforms/cuda`）+ cuxtra 依赖
> 目标硬件：NVIDIA Blackwell 消费级/工作站 GPU（sm_120，CC 12.0：RTX 5070/5080/5090、RTX PRO 6000 Blackwell 等；GB10 为 CC 12.1 可顺带覆盖）

---

## 1. 背景与问题定义

### 1.1 现状

`platforms/cuda/hal/src/arch/arch.cpp` 按 `major*10+minor` 分派队列实现：

| 架构 | Guardian (L2) | TrapHandler (L3) | 队列类型 | 实际抢占级别 |
|---|---|---|---|---|
| sm35 | ✅ | ✗ | `CudaQueueLv2` | L1+L2 |
| sm70 | ✅ | ✅ (offset 0x260) | `CudaQueueLv3Trap` | L1+L2+L3 |
| sm86 | ✅ | ✅ (offset 0x3e0) | `CudaQueueLv3Trap` / `CudaQueueLv3Tsg` | L1+L2+L3 |
| **sm120** | ✗ | ✗ | **fallback `CudaQueueLv1`** | **仅 L1** |
| Windows（全部架构） | ✗ | ✗ | 强制 `CudaQueueLv1` | 仅 L1 |

sm120 用户当前只能获得 Level-1（停发射 + 等待在飞命令完成）抢占，长 kernel 无法被及时打断。

### 1.2 目标

为 sm120 补齐三级抢占能力，按投入产出比分阶段交付：

- **P0（阶段一）**：TSG 路径 Level-3 —— 改动最小、无 SASS 依赖、天然兼容 CUDA Graph；
- **P1（阶段二）**：`GuardianSM120` Level-2 指令级插桩；
- **P2（阶段三）**：`TarpHandlerSM120` Level-3 trap 注入；
- **P3（贯穿）**：Windows 平台解锁 L2/L3、通用健壮性增强、自动化工具链。

### 1.3 外部约束事实（技术调研结论）

| 事实 | 影响 |
|---|---|
| sm_120 需要 CUDA Toolkit ≥ 12.8、驱动 ≥ R570 | 编译/运行环境基线 |
| SASS 指令集跨 CC 不二进制兼容；Blackwell 与 Ampere 编码有实质差异（新增 `LDCU`、`SETLMEMBASE` 等指令；NVBit 内部资料显示 sm120 HAL 解码表约 54KB，复杂度远超前代） | guardian/trap 指令序列**必须为 sm_120 重新编译生成**，不能复用 sm86 数组 |
| ptxas 对 sm_100/120 编译倾向使用更多寄存器（示例 255 regs）、局部使用 local memory（`STL/LDL`）且 trap handler 现场保存结构可能变化 | guardian 寄存器/barrier 下限、trap handler 偏移定位逻辑需重新验证 |
| nvdisasm（CUDA 12.8+）支持 JSON 格式 SASS 反汇编输出 | **可构建自动化指令提取工具链**，大幅降低未来新架构维护成本 |
| NVIDIA 驱动的 channel/TSG（Time Slice Group）模型跨架构存在：CUDA context ↔ TSG 一一映射，硬件调度器在 TSG 间轮转 timeslice；`cuXtraSetTimeslice` 走 RM control 通道，与 SASS 无关 | TSG 路径理论上架构无关，sm120 主要工作是**验证**而非编码 |
| cuxtra 以**预编译静态库**分发（`3rdparty/cuxtra/lib/libcuxtra_{linux_x86_64,linux_aarch64,windows_amd64}.a`），无源码 | 最大的不可控依赖：其内部依赖驱动私有结构（`cuXtraSetEntryPoint`/`SetDebuggerParams` 操纵 CUfunction 内部字段），在 Blackwell + R570 上是否仍然有效必须先验证 |

---

## 2. 抢占机制回顾（设计依据）

### 2.1 三级抢占的软件栈

```
Scheduler (sched/) ──Operation──> SchedExecutor ──> AsyncXQueue::Suspend/Resume
                                                        │  [preempt/src/xqueue/async_xqueue.cpp]
                    ┌───────────────────────────────────┼───────────────────────────┐
                    ▼ L1                                ▼ L2                        ▼ L3
             LaunchWorker::Pause                HwQueue::Deactivate          HwQueue::Interrupt
             （停发射线程）                    （置 global_exit_flag=1）    （cuXtraTriggerTrap / SetTimeslice(0)）
                                                        │                           │
                                              Guardian 插桩 kernel            trap handler 注入
                                              在入口检查点自愿退出           （sm70@0x260 / sm86@0x3e0）
```

### 2.2 Level-2 关键数据通路

1. **插桩**（`InstrumentContext::Instrument`）：guardian 指令前缀 + 原 kernel 二进制 → `cuXtraInstrMemcpyHtoD` 写入指令内存（`InstrMemAllocator`，块式只增分配）；按 `CUfunction` 去重。
2. **发射**（`InstrumentContext::Launch`）：
   - `cuXtraSetDebuggerParams(func, args_buf, 28)`：注入 4 个参数（preempt buffer 指针 / guardian 入口 / kernel idx / killable），kernel 内通过常量区 `c[0x0][0x1880..0x1898]` 以 `LDC` 读取；
   - `cuXtraSetEntryPoint(func, ep)`：入口重定向到 guardian/resume 入口。
3. **抢占**（`InstrumentManager::Deactivate`）：仅 `MemsetD32Async(global_exit_flag, 1)`。
4. **恢复**（`Reactivate`）：读 `preempt_idx` → 清 header → 命令日志中从该 idx 起重发射，首个 kernel 走 `entry_point_resume_`。

preempt buffer 布局（VMM API 原地扩展，VA 不变）：

```
| global_exit_flag(u32) | reserved(u32) | preempt_idx(u64) | exit_flag_b0 | restore_flag_b0 | ... per-block |
```

### 2.3 Level-3 两条路径

- **Trap 路径**（`CudaQueueLv3Trap`，默认）：dump 驱动 trap handler → 在固定偏移（保存现场指令 `IADD3 R1,R1,-0x10,RZ` 处）注入跳转 → 注入代码做 idempotent 检查 → 被打断 kernel 在安全点 `exit`。抢占延迟最低。
- **TSG 路径**（`CudaQueueLv3Tsg`，`XSCHED_CUDA_LV3_IMPL=TSG`）：`cuXtraSetTimeslice(ctx, 0)` 令硬件在 timeslice 边界强制上下文切换。**零插桩、零 SASS 依赖、兼容 CUDA Graph**。注意：TSG 分支在 `arch.cpp` 中位于架构 switch **之前**，即任何架构设了该环境变量都会走 TSG——sm120 理论上今天就能用，但从未在 Blackwell 上验证。

---

## 3. 总体方法论

**原则：从"硬件原生接口"到"软件构造接口"递进，每阶段独立交付、独立验证，前置风险最早暴露。**

```
阶段零 环境与依赖验证（gate） ──失败──> 升级 cuxtra / 自研替代（附录 A）
   │ 通过
阶段一 TSG Level-3（验证为主 + 小改动）──────────────> P0 交付
   │ 并行
阶段二 GuardianSM120（L2）+ 指令提取自动化工具链 ────> P1 交付
   │
阶段三 TarpHandlerSM120（L3 trap，深度逆向）────────> P2 交付
   │ 贯穿
阶段四 Windows 解锁 / 健壮性 / 测试 / 文档
```

核心风险排序（高→低）：
1. cuxtra 二进制在 Blackwell 驱动上的兼容性（阶段零 gate）；
2. debugger params 常量区机制（`c[0x0][0x1880]`）在 R570+ 是否维持（阶段二早期验证）；
3. Blackwell trap handler 结构变化导致偏移/注入序列失效（阶段三）；
4. TSG timeslice=0 在消费级 Blackwell 上的语义（阶段一验证）。

---

## 4. 阶段零：环境与依赖验证（Gate，3~5 人日）

### 4.1 工作项

**Z1. 基线环境**
- 硬件：RTX 5090/5080（GB202/GB203）至少一块；有条件加 GB10（CC 12.1）。
- 软件：CUDA Toolkit 12.8+（建议 12.9）、驱动 R570+（建议最新稳定分支）。
- 用 `nvidia-smi --query-gpu=compute_cap` 确认 12.0。

**Z2. L1 基线回归**：在 sm120 上跑通现有透明调度示例（`examples/*/1_transparent_sched`），确认 Lv1 路径（纯 API 拦截，无 cuxtra 深度依赖）正常。

**Z3. cuxtra API 探针（关键）**

新建 `test/cuxtra_probe_sm120.cpp`（参考 `test/test_cuda_driver.cpp` 风格），逐个验证：

| API | 验证方法 | 影响的阶段 |
|---|---|---|
| `cuXtraGetEntryPoint` / `SetEntryPoint` | 取入口→改入口→launch→恢复→launch，验证两次结果一致 | 阶段二 |
| `cuXtraSetDebuggerParams` | 注入 28B 参数，kernel 内用 `LDC c[0x0][0x1880]` 读回比对 | 阶段二/三 |
| `cuXtraGetBinary` / `InstrMemBlockAlloc` / `InstrMemcpyHtoD` / `InvalInstrCache` | 复制一个小 kernel 的二进制到指令内存并跳转执行 | 阶段二/三 |
| `cuXtraGetLocalRegsPerThread` / `SetLocalRegsPerThread` / `BarrierCnt` | 读写后 Get 校验 | 阶段二 |
| `cuXtraGetTrapHandlerInfo` | 返回非空 handler 地址与合理 size | 阶段三 |
| `cuXtraTriggerTrap` | 空上下文触发（预期无效果/可恢复） | 阶段三 |
| `cuXtraGetTimeslice` / `SetTimeslice` | 读取→设 0→读回→恢复，观察行为 | **阶段一** |

**Z4. 决策点**
- 全部通过 → 按计划推进；
- Trap/入口类 API 失败 → 跳过阶段三，阶段二视情况；向 cuxtra 上游（XpuOS 生态）请求 Blackwell 适配版本，或启动附录 A 的自研替代；
- `SetTimeslice` 失败 → 阶段一降级为纯验证性实验，同时评估 `nvidia-smi compute-policy --set-timeslice`（GPU 级，非 context 级）能否作为粗糙替代。

---

## 5. 阶段一：TSG Level-3（P0，3~5 人日）

### 5.1 原理

`TsgContext::Interrupt()` = `cuXtraSetTimeslice(ctx, 0)`。TSG 是驱动的 channel group 抽象，硬件调度器按 timeslice 轮转 TSG；timeslice 置 0 使该 context 的通道组不再获得时间片，实现近似立即的硬件级挂起。该机制完全不触及 SASS。

### 5.2 代码改动清单

1. **`platforms/cuda/hal/src/arch/arch.cpp`**
   - `CudaQueueCreate()` / `DirectLaunch()`：在 TSG 分支之前增加 sm120 的**默认策略**：
     ```cpp
     // 对未在 guardian/trap 表中的新架构（如 sm120），Level-3 默认回落 TSG
     if (GetCudaLv3Implementation() == kCudaLv3ImplementationTsg) { ... }  // 现状保留
     // 新增：arch == 120 且未显式指定实现时，默认选 TSG 并 XINFO 提示
     ```
   - 方案 B（更干净）：引入 `kCudaLv3ImplementationAuto`（默认值），按 arch 表决定：70/86→Trap，120→Tsg。环境变量仍可强制覆盖。
2. **`platforms/cuda/hal/include/xsched/cuda/hal/common/options.h/.cpp`**：`CudaLv3Implementation` 枚举增加 `Auto`；`GetCudaLv3Implementation()` 解析逻辑同步。
3. **`CudaQueueLv3Tsg`（level3/cuda_queue.h/.cpp）**：
   - `GetMaxSupportedLevel()` 确认返回 `kPreemptLevelInterrupt`（当前继承自 Lv1 应为已正确，检查）；
   - `OnHwCommandSubmit()`：TSG 路径对 `CudaGraphCommand` 无需降级（这是 TSG 的核心卖点），确认无 Lv2 基类的插桩副作用——`CudaQueueLv3Tsg` 直接继承 `CudaQueueLv1`，已无副作用 ✅。
4. **`TsgContext`（level3/tsg.cpp）**：
   - 验证 `timeslice_` 初始值语义（Blackwell 上 `cuXtraGetTimeslice` 返回值）；
   - 处理 **timeslice=0 语义风险**：若实验发现 0 在消费级 Blackwell 上被解释为"默认值"而非"挂起"，备选方案为把 timeslice 设为一个极小值（如 1μs），语义变为"最饥饿调度"，配合上层 policy 仍可工作，但恢复时要写回原值；该实验结论直接写入实现注释。
5. **`protocol/include/xsched/protocol/def.h`** 及文档：`XSCHED_CUDA_LV3_IMPL` 说明更新；README XPU 支持矩阵为 sm120 行标注 "L1 ✅ / L2 🔘 / L3 ✅(TSG)"。

### 5.3 验证方案

- 功能：双进程 HPF 抢占实验（`test/run_threshold_test.ps1` 框架复用）：低优先级长 kernel 运行中，高优先级进程提交任务，测量高优任务首个 kernel 启动延迟（应 ≈ TSG 切换开销，量级 ms 内）；
- CUDA Graph：构造含长 kernel 的 graph，重复上述实验（Trap 路径下 graph 是降级 L1 的，TSG 路径应可抢占）；
- 正确性：抢占/恢复 N 轮后结果比对（bit-exact）；
- 压力：`kQueueSuspendFlagSyncHwQueue` 开启下反复 Suspend/Resume 检查 Xid 109（context switch timeout）类驱动报错。

### 5.4 交付物

sm120 TSG Level-3 可用 + 文档更新。工作量：编码 1~2 人日，验证 2~3 人日。

---

## 6. 阶段二：GuardianSM120 Level-2（P1，15~25 人日）

### 6.1 前置验证（1~2 人日，可与阶段一并行）

在真实插桩开发前，先做**最小可行实验**：手工把一个 trivial kernel 的 `cuXtraGetBinary` 二进制 + sm86 guardian 数组拼进指令内存，入口跳 guardian——若 Blackwell 上 guardian 的 SASS 非法（预期如此，编码不同会立即 GPU fault），确认故障模式；然后用下方工具链生成 sm120 guardian 替换后重试。此实验同时验证 Z3 中入口/参数/指令内存四类 API 的**组合可用性**，比单 API 探针更真实。

### 6.2 指令提取自动化工具链（5~8 人日）——本阶段核心工程资产

**动机**：sm70/sm86/sm35 的 guardian/resume/trap 数组是手工从反汇编拷贝的裸 `uint64_t` 数组（见 `arch/sm86.cpp` 的 119 条 guardian、65 条 resume、80+ 条 trap 注入指令），维护成本高且易错。Blackwell 编码复杂度更高，手工不可持续；且未来 sm130+ 还会再来一次。

**设计**：新增 `tools/instrument/extract_sass.py`

```
输入:  .cu 源（guardian/restore/trap 相关 device 函数，来源 platforms/cuda/hal/inject/inject.cu
       及 tools/instrument/ 下的注入函数源）
流程:  1. nvcc -arch=sm_120 -cubin 编译（可参数化 -arch 支持 sm_130 等）
       2. nvdisasm -c -b SM120 --emit-JSON 解析（CUDA 12.8+ 特性）
       3. 从 JSON 提取目标函数的 SASS 指令字（含 control code 双字，128-bit/条）
       4. 模板渲染输出 arch/sm{N}.cpp（GetGuardianInstructions/GetResumeInstructions/
          trap_inject_instrs 静态数组）
输出:  platforms/cuda/hal/src/arch/sm120.cpp（可直接编译）
```

关键细节：
- 指令长度：Volta+ 为 16B（128-bit，`INSTR_LEN`）；Blackwell 仍为 16B/条（静态配对 VLIW 是"下一代"，暂不处理，但工具按字段解析而非定长拷贝，为未来留余量）；
- 函数定位：按 ELF symbol 找到 `.text._Z14check_preempt...` 段；`__noinline__` + `EXPORT_C_FUNC` 保证独立函数体；
- **替换与校验（考古修正）**：L2 guardian/resume 的手工操作是“等长 1:1 原地替换”（brkpt 占位 NOP ↔ LDC）加“LDC 内部 offset 立即数改写”（asm 中 "changed" 标记），不改函数长度，故相对分支（BRA/BSSY）编码从未被人工修正——sm86.cpp 数组与反汇编的分支编码逐字节一致即为证；真正需修地址的场景在运行时（`TarpHandler::SetJumpInstruction` 把 40 位绝对地址补进 JMP 模板，inject.cu 的 jmp_to_target/trampoline 即为此模板而编译）与 trap 摘录的头尾裁剪（人工核对裁剪点不落在“分支→目标”区间）；工具因此以分支距离校验为主、重写为兜底；
- 校验：生成后自动回灌 `nvdisasm` 反汇编，与源函数反汇编做语义 diff（寄存器/立即数一致），不一致即 fail。

### 6.3 代码改动清单

1. **新增 `platforms/cuda/hal/include/xsched/cuda/hal/arch/sm120.h`**：仿 sm86.h，声明 `GuardianSM120`（阶段二）与 `TarpHandlerSM120`（阶段三占位）。
2. **新增 `platforms/cuda/hal/src/arch/sm120.cpp`**：工具生成 + 手工复核。
3. **`arch.cpp`**：
   - `Guardian::Instance()` 增加 `case 120: return std::make_shared<GuardianSM120>();`
   - `CudaQueueCreate()` 的 trap 分派表中增加 `case 120:`（阶段三前先映射到 `CudaQueueLv2`，即 sm120 默认 = L2 能力；TSG 仍可经 env 选择）——**注意与阶段一 Auto 策略的合成**：Auto 时 120 的 L3 实现选 TSG，但 queue 类型若为 `CudaQueueLv3Trap` 需先落 Lv2 能力。推荐最终形态：
     ```
     case 120: return GetCudaLv3Implementation() == TSG ? CudaQueueLv3Tsg : CudaQueueLv2(占位，阶段三后改 Lv3Trap)
     ```
4. **`instrument.cpp` 微调**：
   - guardian 寄存器需求：sm86 硬编码 `if (reg_cnt < 32) Set(32)`、barrier `if (<1) Set(1)`。改为按架构查表（`Guardian` 接口增加 `virtual size_t RequiredRegs()/RequiredBarriers()`），sm120 值由工具链从生成产物统计后填入（Blackwell ptxas 可能给 guardian 分配 >32 regs 或用到 local memory——若用到 local memory（`STL/LDL`），还需确认其 local memory base 在入口重定向场景下有效，这是 Blackwell 新增 `SETLMEMBASE` 语义带来的新风险点，最小实验中一并验证）；
   - debugger params 偏移 `c[0x0][0x1880..0x1898]`：这是 cuXtra 与驱动约定的常量区窗口。**若 Z3 验证发现 R570 上偏移变化**，guardian 源码里的 `LDC` 立即数需同步改（工具链参数化该偏移），并同步更新 `inject.cu` 注释。
5. **`sm120` preempt buffer 机制复用**：布局与 VMM 扩容逻辑（`mm.cpp`）架构无关，无需改动；仅验证 Blackwell 上 `MemAddressReserve/MemMap` 2MB 粒度行为（`CU_MEM_ALLOC_GRANULARITY_RECOMMENDED` 会自动适配）。
6. **`CudaQueueLv2::Reactivate`**：重发射逻辑复用（`entry_point_resume_` 由 `GuardianSM120::GetResumeInstructions` 提供）。

### 6.4 验证方案

- 单元：`test/` 新增 `test_preempt_sm120.cpp`：
  - kernel 每 N 次迭代有幂等检查点（guardian 只在入口，L2 语义为"kernel 间"抢占）→ 提交 1000 个 50ms kernel 的流，中途 Deactivate，验证：(a) 抢占延迟 < 单个 kernel 时长；(b) 恢复后总结果 bit-exact；(c) `preempt_idx` 正确指向被退出的 kernel；
  - 混合命令流（kernel + memcpy + memset）：验证 `launch_worker.cpp` 的 deactivatable/idempotent 一致性约束在 sm120 上不破坏数据；
- 集成：LLM 推理双租户场景（复用 `integration/llama.cpp` 或 Triton 用例），HPF 策略下高优请求 TTFT 改善量化；
- 回归：sm86/sm70 全量测试不回退。

### 6.5 风险与预案

| 风险 | 概率 | 预案 |
|---|---|---|
| `c[0x0][0x1880]` debugger params 窗口失效 | 中 | 由 Z3/最小实验提前暴露；若失效且 cuxtra 无更新，需自研参数注入（经 kernel 参数区尾部追加，参考 `cuXtraGetParamInfo` 拼接）——估计 +10 人日 |
| guardian 产物依赖 local memory/stack | 中低 | 最小实验验证；若不可用，改写 guardian 源码为纯寄存器版（手写 PTX asm 块，避免编译器生成 STL/LDL） |
| Blackwell 指令内存可执行性权限变化 | 低 | `cuXtraInstrMemBlockAlloc` 若失效，尝试 VMM 分配 + `CU_MEM_ALLOCATION_TYPE_PINNED` + 手动设置可执行属性 |

---

## 7. 阶段三：TarpHandlerSM120 Level-3 Trap（P2，20~40 人日）

### 7.1 工作流

**T1. Dump 与分析（5~10 人日，不确定性最高）**

- 复用 `InterruptContext::DumpTrapHandler()`（level3/interrupt.cpp 已有，被注释的调试路径）导出 sm120 trap handler 二进制；
- 用 `nvdisasm -b SM120` 反汇编，完成三项分析：
  1. **保存现场序列**：sm70/86 在固定偏移有 `IADD3 R1, R1, -0x10, RZ`（栈顶回收 16B 现场区）。Blackwell 上需找到等价指令（注意可能换成新栈基址机制，`SETLMEMBASE` 类指令提示 local memory 寻址模型有变）；
  2. **preempt buffer 参数读取路径**：trap 上下文中 guardian 参数从哪来（sm86 注入序列开头从 `c[0x0][0x1880]` 系列 LDC 取参）——需确认 trap handler 内核态下 constant bank 是否仍映射当前 kernel 的参数；
  3. **handler 尾部返回路径**：注入代码 `jmp` 回 `(handler + replaced_offset + 16)` 的跳转编码在 Blackwell 上的立即数域。
- **改进（强烈建议同时落地）**：把 sm70/86 的硬编码偏移（0x260/0x3e0）替换为**运行时模式匹配定位**：扫描 handler 指令流，匹配"第一条对 R1 做负立即数 IADD3 且后随 STL"的模式。这样驱动小版本更新导致的 handler 布局漂移不再致命，且 sm120 可直接复用该逻辑。模式匹配基于 nvdisasm 无法在运行时用（那是离线工具），需要内置一个**轻量 SASS 解码器**（只解码 2~3 个 opcode 的位域），工作量约 +3 人日，建议做。

**T2. 注入序列生成（5~10 人日）**

- `trap_inject_instrs` 同样由 6.2 工具链从 `check_preempt_trap`/`exit_if_idempotent`（inject.cu）的 sm_120 编译产物生成；
- `TarpHandlerSM120::Instrument()`：替换偏移由 T1 的模式匹配结果传入（接口改造：`Instrument()` 增加参数或 `TarpHandler` 增加成员 `replaced_offset_`）；
- `SetJumpInstruction()`：Blackwell `BRA`/`JMP` 64-bit 立即数编码重写（对照 sm86 的两字构造 `0x345678900000794a` + `0x000fea0003807f12`，其中目标地址低 40bit 拆进两个字的字节 4~9）。

**T3. killable 语义补全（3~5 人日，顺带偿还技术债）**

- 落实 `level3/cuda_queue.cpp` 的 `TODO: assign kernel_command->killable`：当前恒置 true。设计：默认 killable=true，提供 Hint（复用 `sched/protocol/hint.h` 框架，新增 `kHintTypeKillable`）或 env 开关让用户标记不可打断 kernel（如持有全局锁的 kernel、原子更新中的 kernel）；
- 修复 `Interrupt()` 的 `FIXME: what if multiple threads call Interrupt()?`：为 `InterruptContext` 增加 `std::atomic<size_t> interrupt_count_`（照抄 tsg.cpp 的引用计数模式）。

**T4. 集成与开关（2~5 人日）**

- `arch.cpp`：`TarpHandler::Instance()` 增加 `case 120`；`CudaQueueCreate()` 的 120 分派从阶段二的 `CudaQueueLv2` 升级为 `CudaQueueLv3Trap`；
- Auto 策略更新：sm120 默认实现从 TSG 切换为 Trap（若验证性能更优）或保留 TSG（若 trap 稳定性存疑），由实测数据决定，配置化。

### 7.2 验证方案

- 抢占延迟基准：运行 10s 长 kernel，测量 Suspend→队列真正停止的时间分布（P50/P99）。预期：L1 ≈ kernel 剩余时长；L2 ≈ kernel 时长（入口检查，长 kernel 无法中途退出，是"kernel 间"抢占）；L3 trap ≈ ms 级；L3 TSG ≈ timeslice 粒度。产出四种路径的量化对比表（该表同时反哺调度策略论文/文档）；
- 正确性：killable kernel 被 trap 打断 → 恢复后从 resume 入口重执行 → 结果 bit-exact；非 killable kernel trap 后必须不受影响（`check_preempt_trap` 的 idempotent 分支）；
- 稳定性：10000 次抢占循环 + CUPTI/其他工具并发运行（trap handler 被多处 hook 的冲突检测）；
- 驱动版本矩阵：R570 / 最新两个稳定分支各跑一遍回归。

### 7.3 风险与预案

| 风险 | 概率 | 预案 |
|---|---|---|
| Blackwell trap handler 结构大改，无 `IADD3 R1,R1,-0x10` 等价物 | 中 | 模式匹配定位失效则回退硬编码 offset + 驱动版本指纹校验（启动时比对 handler hash，不匹配则拒绝启用 L3 并降级 TSG）；最坏情况放弃 trap 路径，sm120 L3 仅保留 TSG |
| trap 上下文中 constant bank 不可用 | 中低 | 注入序列改为从 trap handler 已有的寄存器现场推导参数地址，或把 preempt buffer 指针编码进注入指令立即数（每 context 重写一次注入代码） |
| 驱动更新频繁破坏 handler | 高（长期） | handler hash 校验 + 自动降级机制（同上）；工具链产出多驱动版本 offset 对照表 |

---

## 8. 阶段四：Windows 解锁与通用增强（5~10 人日，可与阶段一~三并行）

用户主环境为 Windows，`arch.cpp` 中 `#if defined(_WIN32) return Lv1` 直接短路了全部 L2/L3。cuxtra 已分发 `libcuxtra_windows_amd64.a` 且 shim 已有 `intercept_windows.cpp`，说明 Windows 路径是"未启用"而非"不可能"。

工作项：
1. **W1（2~3 人日）**：移除 Windows 短路，将 `CudaQueueCreate`/`DirectLaunch` 的 Windows 分支改为与 Linux 相同的 Auto+arch 分派；阶段一验证 `cuXtraSetTimeslice` Windows 实现的可用性（TSG 是 Windows 上最容易落地的 L3）；
2. **W2（随阶段二）**：`InstrMemAllocator`/VMM 路径的 Windows 适配（`win32HandleMetaData` 字段已预留，`mm.cpp` 已按 Windows 结构体编写，主要是实测）；
3. **W3（随阶段三）**：Windows 下 trap handler 获取路径验证（`cuXtraGetTrapHandlerInfo`）；
4. **W4（1 人日）**：`launch_mtx_` 全局锁优化评估（L2 发射路径的性能瓶颈，sm120 上 AI 推理高并发场景收益大；方案：per-CUfunction 缓存 debugger params 中不变字段，仅动态字段（preempt buf 指针/idx）每次写）；
5. **W5（1 人日）**：`InstrMemAllocator` 内存回收策略（当前只增不回收）。

---

## 9. 工作量汇总

| 阶段 | 内容 | 人日 | 累计 | 不确定性 |
|---|---|---|---|---|
| 零 | 环境 + cuxtra 探针（gate） | 3~5 | 5 | 低 |
| 一 | TSG L3（验证为主） | 3~5 | 10 | 低（依赖 Z3 结论） |
| 二 | Guardian L2 + 指令提取工具链 | 15~25 | 35 | 中 |
| 三 | Trap L3（含 killable/FIXME 偿还） | 20~40 | 75 | 高 |
| 四 | Windows 解锁 + 通用增强 | 5~10 | 85 | 低~中 |
| **合计** | | **46~85 人日** | | 约 2~4 人月（1 人全职），2 人并行可压缩到 1.5~2 个月 |

建议排期（单人 + 硬件到位前提下）：

```
W1      阶段零（探针结论决定后续范围）
W2~W3   阶段一 TSG 交付（第一个可用里程碑：sm120 L1+L3）
W3~W7   阶段二工具链 + GuardianSM120
W8~W15  阶段三 trap 逆向（期间 W9 起 Windows 解锁并行）
W16     收尾：矩阵更新、文档、论文性延迟基准数据
```

---

## 10. 验收标准

- [ ] sm120 默认配置下：L1 + L3(TSG) 可用，透明调度示例全通过；
- [ ] `XSCHED_CUDA_LV3_IMPL` 支持在 TSG/Trap 间切换（阶段三完成后）；
- [ ] L2/L3 抢占-恢复循环 ≥1000 次结果 bit-exact；
- [ ] 四路径（L1/L2/L3-trap/L3-TSG）抢占延迟量化表产出；
- [ ] sm35/70/86 全量回归通过；
- [ ] 驱动版本漂移防护：handler/env 校验失败时自动降级而非崩溃；
- [ ] README XPU 支持矩阵、`platforms/cuda/README.md`、`protocol/README.md`（env 说明）更新；
- [ ] `tools/instrument/extract_sass.py` 可在 CI 中对 sm70/86/120 三架构生成代码并校验一致（sm70/86 用现有数组做金标准回归）。

---

## 附录 A：cuxtra 失效时的自研替代路径（ contingency ）

若 Z3 探针发现 cuxtra 二进制在 Blackwell 上大面积失效且上游无更新，替代实现优先级：

1. **入口重定向**（`Set/GetEntryPoint`）：Linux 上可经 CUfunction 内部结构偏移（open-gpu-kernel-modules 的 `gsp` 推导 + gdb 探测）实现；Windows 上需逆向 `nvcuda.dll` 内部函数表。预计各 +15 人日/平台。
2. **debugger params**：本质是驱动为调试器预留的 kernel 参数窗口，可退化为"参数区尾部追加"方案（`cuXtraGetParamInfo` 遍历原参数，在 kernel 参数 buffer 末尾拼接 XSched 参数，并同步改写 guardian 的 LDC 偏移为参数区内偏移）——此方案反而更不依赖驱动内部结构，**可作为 Blackwell 的首选稳健方案**评估（+10 人日，但消除一个长期风险点）。
3. **TSG timeslice**：Linux 上经 `/dev/nvidiactl` 的 RM control ioctl（`NV_IOCTL...`，open-gpu-kernel-modules 中 `NV2080_CTRL_CMD_GPU_SET_CHANNEL_TIMESLICE` 类控制码，cuxtra 的 `cuXtraGetRmControlFd` 暴露的正是该 fd）；Windows 上走 `NvAPI`/服务接口。预计 +10 人日。

## 附录 B：命名与分派终态（目标）

```
Guardian::Instance(dev):     35→SM35  70→SM70  86→SM86  120→SM120
TarpHandler::Instance(dev):  70→SM70  86→SM86  120→SM120
CudaQueueCreate(stream):
    Windows: Auto(arch) —— 不再强制 Lv1
    TSG(显式)            → CudaQueueLv3Tsg      (任意架构)
    Auto/Trap + 35       → CudaQueueLv2
    Auto/Trap + 70/86    → CudaQueueLv3Trap
    Auto        + 120    → CudaQueueLv3Trap (阶段三后) / CudaQueueLv3Tsg (阶段三前)
```

## 附录 C：关键文件索引

| 文件 | 角色 |
|---|---|
| `platforms/cuda/hal/src/arch/arch.cpp` | 架构分派总入口（L11/35/48/80/112 处有 `NEW_CUDA_ARCH` 扩展点注释） |
| `platforms/cuda/hal/src/arch/sm86.cpp` | guardian/resume/trap 指令数组参照实现 |
| `platforms/cuda/hal/src/level2/instrument.cpp` | 插桩核心、debugger params 注入、preempt buffer |
| `platforms/cuda/hal/src/level2/mm.cpp` | VMM 可扩展 buffer + 指令内存分配器 |
| `platforms/cuda/hal/src/level3/interrupt.cpp` | trap handler dump/注入、`DumpTrapHandler()` 调试工具 |
| `platforms/cuda/hal/src/level3/tsg.cpp` | TSG 引用计数实现（Interrupt 并发修复的模板） |
| `platforms/cuda/hal/inject/inject.cu` | guardian/trap 注入函数的 CUDA C++ 源（工具链输入） |
| `preempt/src/xqueue/async_xqueue.cpp` | Suspend/Resume 三级路由 |
| `preempt/src/xqueue/launch_worker.cpp` | deactivatable/idempotent 一致性约束 |
| `3rdparty/cuxtra/` | 逆向驱动接口（预编译静态库，sm120 最大外部依赖） |
