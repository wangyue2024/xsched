# XSched sm120 (Blackwell) Level-3 抢占：全阶段汇总报告


> **措辞修订（2026-10-19）**：本档案早期版本使用了"判死 / 物理不可达 / 能力边界 / 避免重复投入"等定性词。这些结论的适用范围仅限**单机 RTX 5060 + 驱动 595.99.02**，未经跨架构、跨驱动代际对照，现已统一改为"在本机条件下未能走通 / 我没能找到用户态手段"。**所有数据、表格取值、日志引用与实验记录均未修改。**

> 版本：2026-10-15
> 硬件：NVIDIA RTX 5060（sm120 / Blackwell，CC 12.0）
> 系统：Linux + 驱动 595.99.02（CUDA 13.2，开源内核模块）；Windows 探针为 25H2 + 驱动 13030（WDDM）
> 关联文档：`L3_LINUX_REPORT_ZH.md`（开发过程全记录）、`ATTACH_REPLAY_PLAN.md`（Round-1/2 判决）、`README.md`（双语探针原始记录）
> 本文定位：**跨阶段总汇总**——原理、实现清单、证据链、全部结论、规律提炼、遗留问题。
> 整理来源：上述三份文档 + 仓库内全部证据文件（`evidence/`、`attach_replay/`）+ 当前 HEAD 代码。

---

## 0. 摘要

XSched Level-3（执行中内核的抢占）在 sm120（RTX 50 系列）上的落地，经过四个阶段：

| 阶段 | 结果 |
|---|---|
| ① Windows 探针侦察 | 导出表/Blackwell getter（slot 20）可用；触发与 TSG 被 cuxtra 的 Linux-only 模块阻断 |
| ② Linux trap 路径实现 | 全链路**实现成功**（获取/改写/触发逐环字节级验证），但**注入代码不执行**——未走通 |
| ③ 根因定位 + attach 栈复刻 | 定位到"非活动模板"根因；复刻调试器 attach 全栈后：**我没能找到可让运行中内核微秒级停止的用户态手段** |
| ④ TSG 路径落地（当前形态） | sm120 的 L3 改走"边界抢占 + 时间片协同"，实现 + 端到端验证通过 |

一句话结论：**在 sm120 + 595（本机）上，中间断式 L3 未能走通（含 cuda-gdb 对照）；当前可用形态 = L1/L2 排空 + TSG 时间片协同（`CudaQueueLv3Tsg`）。**

---

## 1. 原理底座

### 1.1 XSched 四组件与三层抢占模型

XSched（OSDI'25）的四个核心组件：

- **XShim（shim）**：冒充 `nvcuda.dll` / `libcuda.so` 拦截驱动 API，透明接管应用命令；
- **XPreempt（preempt/）**：XQueue 可抢占命令队列 + SchedAgent/Executor（队列状态事件上报与操作执行）；
- **XAL（platforms/cuda/hal）**：硬件适配层，实现多级硬件模型（guardian / trap / TSG）；
- **XScheduler（xserver）+ xcli**：全局调度服务与命令行监控。

三级抢占模型（`xsched/cuda/hal/common/levels.h`）：

| 级别 | 名称 | 机制 | 时延量级 |
|---|---|---|---|
| L1 | Block | 软件缓冲命令，控制物理下发时机 | 提交级 |
| L2 | Deactivate | 撤回"已提交未执行"的命令（CUDA 用 guardian 注入内核入口） | 毫秒级 |
| L3 | Interrupt | 中断/换出**正在执行**的内核 | 微秒级（目标） |

### 1.2 L3 悬停的完整调用链

```
XScheduler 下发 Suspend 操作
  → preempt/src/sched/executor.cpp:75  xq->Suspend(flags)
  → preempt/src/xqueue/async_xqueue.cpp:111
       if (level_ >= kPreemptLevelInterrupt) kHwQueue->Interrupt();
  → platforms/cuda/hal 的队列实现（CudaQueueLv3Trap / CudaQueueLv3Tsg）
```

### 1.3 CUDA 平台 L3 的两条路线

| 路线 | 机制 | 架构相关性 |
|---|---|---|
| **Trap** | 经隐藏接口 `cuGetExportTable` 拿到驱动 trap handler，patch 后触发硬件 trap → 杀 warp | 架构相关（sm70/sm86 配方，sm120 未走通） |
| **TSG** | `cuXtraGet/SetTimeslice` 调整上下文时间片实现抢占 | 架构无关（sm120 的最终形态） |

---

## 2. 演进史（提交记录）

| commit | 内容 |
|---|---|
| `ff14d06` | `feat(sm120): Level-3 trap-handler export-table probe workspace` —— Windows 探针 workspace（静态逆向 + 6 个探针） |
| `392e7da` | `feat(cuda): add sm120 (Blackwell) Level-2 preemption support` —— L2 guardian（50+32 条指令数组），已实机验证 |
| `2eb2e63` | `feat(sm120): Linux Level-3 trap-path implementation, evidence and root cause` —— trap 路径实现 + 根因定位 |
| `2db49a5` | `feat(level3): support level3 preemption for Linux in sm120`（2026-10-14，**当前 HEAD**）—— sm120 L3 转向 TSG，含 Round-1/2 全部探针与证据 |

核心代码转折（`2db49a5` 对 `platforms/cuda/hal/src/arch/arch.cpp` 仅 9 行）：

- `CudaQueueCreate`（arch.cpp:93-97）：`case 120` 由 `CudaQueueLv3Trap` 改为 `CudaQueueLv3Tsg`；
- `DirectLaunch`（arch.cpp:138-139）：同步改为 `CudaQueueLv3Tsg::DirectLaunch`。

---

## 3. 阶段一：Windows 探针（commit ff14d06 的 workspace）

环境：Windows 25H2 + WDDM、RTX 5060、驱动 13030（CUDA 12.9）。

### 3.1 静态逆向成果（`tools/analyze_cuxtra.py` / `analyze_nvcuda.py`）

- `nvcuda.dll` 导出 `cuGetExportTable`（ordinal 204）；
- cuxtra 静态库（`libcuxtra_windows_amd64.a`，427992B）内解析出 5 个 ETID GUID：

| 符号 | 成员 | GUID |
|---|---|---|
| `CU_ETID_ToolsTrapHandler` | trap.cpp.obj | `cc529e94-5c4e-9d46-837c-9625868334e4` |
| `CU_ETID_ToolsModule` | function.cpp.obj | `6e163fbe-b958-444d-835c-e182aff1991e` |
| `CU_ETID_ToolsMemory` | instrmem.cpp.obj | `bfdb432d-bf3c-5a4a-945e-b34029e81e75` |
| `CU_ETID_ToolsContext` | memcpy.cpp.obj | `21318c60-9714-3248-8ca6-41ff7324c8f2` |
| `CU_ETID_ToolsPushBufferHal` | cache.cpp.obj | `a6b1ff99-ecc4-c94f-92f9-1928663d5585` |

- cuxtra 的 trap 表面**只有 `EtblTrapHandler::GetInfoPascal`（slot 19）**——Blackwell getter 缺失，即后来补齐的"缺口"。

### 3.2 实机探针结论

| 项 | 结果 | 证据 |
|---|---|---|
| TrapHandler 导出表 | ✅ 184B（23 槽）；**slot 19 报 error 101**（缺 Blackwell 分支） | `probe_export_table_*.log` |
| **slot 20 = 事实上的 GetInfoBlackwell** | ✅ 40B 输出：`+0x10=memObjHandle`、`+0x18=0x880`、`+0x20=0x940`、`size=0x28` | `probe_slot20_*.log` |
| `ObjGetPc(ctx, handle)` | ✅ pc = 0x880（相对 stub 偏移） | 同上 |
| 触发（`cuXtraTriggerTrap`） | ⛔ `CudaKernelModule is not supported on Windows` @ kmod.cpp:63（cuxtra 实现缺口，非驱动限制） | `probe_trigger_*.log` |
| `cuCheckpointProcess` 全家族 | 存在（ordinals 13-18）但返回 801（NOT_SUPPORTED） | `probe_win_trigger2_*.log` |
| TSG | ⛔ 同被 kmod 阻断 | 同上 |
| `attr90` COMPUTE_PREEMPTION_SUPPORTED | = 0（WDDM） | `probe_export_table` |
| 其他槽位（1/5/8/10/16/22） | 调用成功但输出全 0（无操作查询） | slot sweep |

**判定**：handler 信息路径双平台可用；**触发必须在 Linux 验证**（cuxtra 的 RM 通道是 Linux ioctl 实现）。

---

## 4. 阶段二：Linux trap 路径——完整实现，"执行"未走通（commit 2eb2e63）

环境：Linux + RTX 5060、驱动 595.99.02（开源内核模块）、CUDA 12.9.41 工具链（`-ccbin g++-13`）、`/dev/nvidiactl` 0666。

### 4.1 实现清单（代码层）

| 文件 | 内容 |
|---|---|
| `arch/sm120.h` | `TarpHandlerSM120` 声明：patch 点 = 返回桩偏移 0x880（由驱动 slot 20 的 `+0x18` 报告，所有出口路径必经，起始为两个 NOP） |
| `arch/sm120.cpp` | ① `GetTrapHandlerInfo`（sm120.cpp:213-279）：绕过 cuxtra slot 19——`GetExportTable(ToolsTrapHandler)` → slot20 `fn(ctx,out40)` → `GetExportTable(ToolsMemory)` → slot24 `ObjGetPc(ctx,handle,&pc)` / slot25 `ObjGetSize(handle,&sz)`；断言校验 `stub_offset==0x880`（驱动布局变更时响亮失败）；② `SetJumpInstruction`：48 位绝对 JMP（sm86 同款编码，`0x345678900000794a` + 补 `0x7f12` 位）；③ `Instrument`：注入区 = [40 条 `trap_inject_instrs`] + [被替换指令] + [跳回指令]，再在 0x880 写入 JMP；④ env 门控签名载荷钩子（`XSCHED_SM120_TRAP_TEST_ADDR/OFFSET`） |
| `level3/trap.h` | `TarpHandler` 新增虚接口 `GetTrapHandlerInfo`（默认走 cuxtra Pascal 路径；sm120 覆写）——架构缺口的标准扩展点 |
| `level3/interrupt.cpp` | `InterruptContext` 走虚接口；`InstrumentTrapHandler()`：DtoH 读 handler → Instrument → `cuXtraInstrMemcpyHtoD`（注入区）→ `cuXtraMemcpyHtoD`（handler 回写）→ `cuXtraInvalInstrCache`；`Interrupt()` = `cuXtraTriggerTrap(ctx)`；`DumpTrapHandler()` 诊断 |
| `level3/cuda_queue.cpp` | `CudaQueueLv3Trap : CudaQueueLv2`（构造 Instancetrap InterruptContext；Interrupt 前 StreamSynchronize OpStream；OnPreemptLevelChange 时 InstrumentTrapHandler） |
| `arch/arch.cpp` | sm120（Linux）→ `CudaQueueLv3Trap`；`TarpHandler::Instance` 增加 case 120 |
| 工具链 | `inject_120` 由 nvcc 12.9.41 重新生成；**guardian(50)/resume(32) 与已提交版逐位（bit-exact）一致**；`check_preempt_trap`(56 条，裁后 40) 与 `exit_if_idempotent`(40) 生成报告 |

### 4.2 trap handler 结构逆向（本阶段最重要的结构性成果）

对 `pc` 处 4864B（0x1300）做"cubin 包装 + nvdisasm"反汇编（`handler_sm120.bin`、`handler_880_disasm.txt`）：

```
偏移           内容
0x000–0x880    变体 A：handler 主体
               入口: LOP3 P0 = R2 & 0x20; @P0 BRA.U 0x880（快速返回路径）
               保存序列: S2R/LDC/LDL 采集 VIRTID/SMEMSZ/CTAID/ESR_PC → ST.E [R4+…]
0x880–0xA00    共享返回桩（两条路径必经）:
               NOP, NOP, CCTLL.IVALL, CCTL.IVALL, MEMBAR.SYS, …, RET.ABS R12 0x20
0xA00–0xA80    零填充
0xA80–0x1300   变体 B：CALL.ABS.NOINC 到共享例程的另一处理流程
```

- 所有出口路径必经 0x880 → **patch 点选 0x880（首个 NOP）**；
- 注入组合（镜像 sm86 已审阅配方）：40 条抢占代码（去外层 BSSY/BSYNC、去 RET 尾）+ 被替换指令 + 跳回 0x890 的绝对 JMP。

### 4.3 逐环验证矩阵

| 验证项 | 方法 | 结果 |
|---|---|---|
| 指令数组可复现 | 与提交版逐位对比 | ✅ 一致 |
| handler 可获取 | DtoH 读回、首指令与反汇编核对 | ✅（`pc=0x…5f5300`、`size=0x1300`） |
| 补丁写入 | 补丁后 DtoH 读回比对 | ✅ 字节级一致（`final_writeback.log`） |
| JMP 编码合法 | nvdisasm 解码 + 与真实 `CALL.ABS` 编码交叉验证 | ✅ |
| 触发可调用 | idle/busy 两阶段（带看门狗） | ✅ 瞬时返回（0.000s）、上下文健康（`final_trigger_probe.log`） |
| **执行注入代码** | 签名载荷 + 真实 trap + cuda-gdb 会话 | ⛔ **全不执行** |

### 4.4 根因链（未走通的完整证据）

1. **排除触发常量问题**：cuxtra（Linux）与 `libcuda.so.595` 内部同款助手（文件偏移 0x487560 附近）反汇编对比——同为 `{reg=0x419e84, val=0x80000000}`。触发地址/位值未变。
2. **签名载荷全覆盖不落**：最小载荷（`MOV R7,0x5AA5; ST.E`）贴 0x0（dispatch）/0x60（save 路径）/0x880（共享桩）三处，64 个永久自旋 block + RM 触发——`sig==0`（`mini7_off_*.log`）。
3. **真实 trap 也不走它**：内核内 `brkpt`（mini9）→ `cudaErrorIllegalAddress(700)`，驱动内部处理回收，载荷不执行。
4. **官方激活态也不走它**：cuda-gdb 会话下重跑以上流程，仍不执行。
5. **发现真机制**：自研 LD_PRELOAD RM-ioctl 拦截器差分"普通 vs cuda-gdb"——调试器专属的 class 0x83de 方法族、专用 RM client、libcuda `cudbg*` 钩子、libcudadebugger protobuf agent（"DebuggeeAttach" 等）。

**结论：slot20 memobj 在 595 上是"非活动模板"——驱动保留它（读写合法），但硬件 trap 的执行流不经过它。sm86 时代的"patch memobj + 写 RM 位触发"配方前提（memobj=活体）在 595 上不成立。**

---

## 5. cuXtra 分析（贯穿性资产）

### 5.1 静态结构（`evidence/cuxtra_analysis.txt`，Windows .a + Linux .so 双版本）

- 对象构成：`trap.cpp.obj`（仅 `EtblTrapHandler::GetInfoPascal`）、`instrmem.cpp.obj`（ToolsMemory）、`timeslice.cpp.obj`（→ kmod）、`kmod.cpp.obj`（`CudaKernelModule`，**Linux-only**）、`memobj.cpp.obj`、`cache.cpp.obj`（PushBufferHal）；
- 通用取槽函数 `GetEtblFunc(GUID, idx)`；
- 符号确认：`cuXtraGetEntryPoint`、`CudaKernelModule::CtxGet/SetTimeslice`、`EtblMemory::ObjGetPc/ObjGetSize`。

### 5.2 运行机制规律（反汇编 + 实测确认）

| 接口 | 底层机制 |
|---|---|
| `cuXtraTriggerTrap` | RM `GlobalRegsWrite32`：全局寄存器 `0x419e84` bit31 = TRIGGER_TRAP |
| `cuXtraGet/SetTimeslice` | NVOS54 RM_CONTROL 到 class `0xa06c`（TSG）对象：`0xa06c0103`=SET_TIMESLICE、`0xa06c0104`=GET（实测）；默认 2048µs |
| `cuXtraGetTrapHandlerInfo` | slot 19 GetInfoPascal → sm120 报 error 101（缺口） |
| handler 内存读写 | `cuXtraMemcpyDtoH/HtoD`（直接对设备 VA）；`cuXtraInstrMemcpyHtoD`（指令区）；`cuXtraInvalInstrCache`（指令缓存失效） |
| Windows 阻断根因 | `CudaKernelModule::GetKMod` FATAL——kmod 基于 `/dev/nvidiactl` ioctl，属**实现缺口而非驱动限制** |

### 5.3 调用约定"踩坑"规律

- `memObjHandle`（slot20 的 +0x10）是**指向驱动分配结构的指针**——必须把"值本身当指针"传入；传其拷贝会得到 0；
- `ObjGetPc` 返回设备 VA（如 `0x…5f5300`），用 `cuXtraMemcpyDtoH/HtoD` 直接读写（不是 handle）。

---

## 6. 阶段三：attach 栈复刻与平台判决（Round-1/2，2026-10-14）

### 6.1 内核侧静态逆向（全部实锤）

从 `/usr/src/nvidia-595.99.02/nvidia/nv-kernel.o_binary`（ELF 未 strip，21673 符号）解析：

- **class 0x83de = KernelSMDebuggerSession**，方法字典（记录 0x28B 步长 @0xa870e0）**33 个方法全解**；
- 构造：`ksmdbgssnConstruct_IMPL` @0x219030 —— 从 alloc 参数读 2 个对象 ID（绑定上下文/通道组）→ `kgrctxRegisterKernelSMDebuggerSession_IMPL` @0x211660 挂到 `grctx->[+0x148]->+0x680` 链；
- 关键方法语义：

| 方法 | 语义 |
|---|---|
| `0x83de0315/0316` | DebugRead/WriteMemory（24B 参数） |
| `0x83de0317/0318` | DebugSuspend/ResumeContext（无本地 IMPL → **GSP 固件转发**） |
| `0x83de031f/0307/032a` | 调试模式位（MMUDebug/MMUGccDebug/ErrbarDebug） |
| `0x83de0309` | DebugSetExceptionMask |
| `0x83de030c` | DebugReadAllSmErrorStates（4824B 缓冲） |
| `0x83de0313/0321/0322/0324` | stop/single-step trigger 家族 |
| `0x83de031d` | DebugExecRegOps（寄存器批量读写 1048B） |

- 带本地 IMPL 的在 `ksmdbgssnCtrlCmd*_IMPL`（0x21a7xx-0x21b0xx）；Suspend/Resume 走 `InternalControlForward` 转发 GSP。

### 6.2 实机复刻成果（l3ctl / arm_modes 探针）

- **决定性发现：`cuCtxCreate` 自动创建 0x83de 会话**（如 0x5c000073）——595 为每个 CUDA 上下文**预置** KernelSMDebuggerSession（CILP 抢占基础设施）。"造会话"变成"用会话"。
- **正确调用配方**（全部 rc=0 实证）：libcuda 的 `/dev/nvidiactl` fd + 第二个零分配 client + 预置会话对象，NVOS54 直接调用；
- 自建会话（`variant0 {0,client,tsg,0}`）：可分配成功（rc=0），但转发 rc=87（功能不完整）；
- `DebugRead/WriteMemory`：写魔数回读一致 ✅；
- 调试模式位（031f/0307/032a=1）+ 异常掩码（0309）✅；
- attach 抓包另发现：**`0xA06C0103`（SET_TIMESLICE）也是调试器 attach 流程的一部分**（对 TSG 对象 0x5c000013），即时间片控制与调试栈同源。

### 6.3 判决性实验全表（每一行都指向同一个结论）

| 候选机制 | 实验 | 结果 |
|---|---|---|
| `0317` Suspend（会话） | 计数器观测（e10b2：v1=3575079→v4=10702927） | rc=0 但**运行中内核持续增长**——不冻结；VERDICT: suspended=no |
| `0317` 对空闲上下文 | E21/E23b 确定性复现 | ~**300ms 异步激活**后新 launch 全部 719（LAUNCH_FAILED）；**单向不可逆**（恢复手段全部失败）——语义=**kill 门控**，非 suspend |
| stop/run trigger 家族（0313/0321/0322/0324） | 参数尺寸修正后全扫（e20） | 受理、rc=0，**均不冻结** |
| RM 触发（`cuXtraTriggerTrap`/0x419e84） | mini7b 武装/基线×3 offsets | **无任何效应**（cuda-gdb 会话下亦然） |
| trap 注入 + 武装（arm_modes） | mini9 武装/基线 | 武装后 brkpt 从 **700 崩溃 → err=0 干净终止**（异常路径被模式激活），但**注入载荷不执行** |
| **cuda-gdb 真中断** | SIGINT + `info cuda threads` | **"No CUDA threads" × kernel 仍 Active**——官方调试器同样无法停止运行中内核 |
| attach 会话下 spin 内核 | spin2 对照 | 计数器全程平滑增长——调试器自身的 suspend 循环也不冻结内核 |
| 硬件计算抢占属性 | `attr90` | Linux = 1（支持 CILP）→ 抢占由**硬件/GSP 调度器**驱动，非用户态可强制 |

### 6.4 判决与设计（写入 ATTACH_REPLAY_PLAN.md R2）

- **"运行中内核的微秒级中断/停止"在本机（sm120+595）上，我没能用任何用户态手段实现**——含会话 suspend、stop-trigger 全家族、RM 触发、trap 注入、乃至 cuda-gdb 本体；
- 关键判据：官方调试器（拥有完整武装栈的天然使用者）都拿不到 mid-kernel 停止 → 这使我倾向于认为不是"少试了某个偏移"；但单机单驱动条件下无法排除硬件/驱动代际因素，不等于该路径不存在；
- **L3 最终设计（R2.4）**：sm120/Linux 上采用"**边界抢占 + 时间片协同**"，不依赖中断原语——`Interrupt()` 退化为 L1/L2 排空 + TSG 时间片压缩；`Restore()` 恢复提交 + 还原时间片；全部未走通的路径均已文档化，供后续参考。

### 6.5 附：为什么 sm86 行、sm120 不行（机制对比）

| 维度 | sm70/sm86（XSched 论文时代） | sm120 + 595.99.02 |
|---|---|---|
| handler memobj 语义 | **活体**：驱动把该内存对象直接用于硬件 trap 入口，patch 即生效 | **非活动模板**：可读写但不在执行路径上 |
| patch 点 | 入口 IADD3（sm70@0x260 / sm86@0x3e0）单点 | 多 variant 结构（A/B + 共享桩），选 0x880 |
| 触发路径 | RM 写 `0x419e84` bit31 → 硬件送 warp 进 handler（有效） | 同写入成功但**无任何效果**（送进行为消失/改写） |
| 激活机制 | 无需额外步骤 | 收敛进调试器 attach 栈（0x83de 会话 + `cudbg*` + GSP 转发）；武装后仅异常路径变化（700→clean），载荷仍不执行 |
| 控制权归属 | 用户态驱动接口可触达 | GSP 固件/调度器（CILP）持有；~300ms 异步窗口暴露固件 RPC 架构（与"微秒级 handler"是两种执行模型） |
| 官方上限对照 | — | cuda-gdb 自身也停不住运行中内核 |

> 边界说明：测试组合固定（RTX 5060 只能配 595），无法单独隔离"sm120 硬件因素"与"595 驱动因素"；但触发寄存器常量、memobj 布局均未变，证据指向**驱动代际重构**为主因。
> 另注：本仓库 README 支持矩阵中，trap 路径**完整验证的是 sm70（V100）**，sm86 一直处于 🚧；sm120 是第一个在落地过程中未能走通 trap、转向 TSG 的架构。

---

## 7. 阶段四：TSG 路径落地（当前 HEAD 形态）

### 7.1 实现清单

| 文件 | 内容 |
|---|---|
| `level3/tsg.h/.cpp` | `TsgContext`：构造保存原时间片（`cuXtraGetTimeslice`，默认 2048µs）；`Interrupt()` 原子引用计数 → 首次置 `cuXtraSetTimeslice(ctx, 0)`；`Restore()` 计数归 1 时还原原值 |
| `level3/cuda_queue.h/.cpp` | `CudaQueueLv3Tsg : CudaQueueLv1`；`Interrupt/Restore/OnPreemptLevelChange`；`GetMaxSupportedLevel()=kPreemptLevelInterrupt`、`SupportDynamicLevel()=true` |
| `arch/arch.cpp` | sm120 双路由（`CudaQueueCreate` + `DirectLaunch`）→ `CudaQueueLv3Tsg`（Trap 路线的替换点，含 R2 判定注释） |
| `common/cuda_command.cpp:14` | TSG 模式下所有 CudaCommand 自动标 `kCommandPropertyDeactivatable`（启用 L2 撤回语义与 TSG 协同） |
| `common/options.cpp` | `XSCHED_CUDA_LV3_IMPL=TSG` 全局开关（默认 TRAP 仅影响 sm70/86；sm120 无条件 Tsg） |

### 7.2 验证与实证

- `tsg_smoke.cu` 端到端：建队(level3)/suspend/resume 全 rc=0，前后内核均正确执行 → **SMOKE PASS**；
- LD_PRELOAD ioctl 实证：`0xa06c0104`（TsgContext 构造时 GET）+ `0xa06c0103`×3（suspend set-0 / resume 还原）**真实下发到设备**；
- 时间片写读回：`SetTimeslice(ctx,0)` 立即读回 0；
- `mini13` 双上下文竞争（victim 2048×256 饱和负载）：baseline 106.9ms / fair 107.0ms / ts1=0 107.0ms → **speedup 1.00x**——本机硬件跨上下文调度已高度公平，TSG 收益需多进程/多设备场景才可评估。

### 7.3 语义边界（使用注意）

- TSG **只暂停/换出、不杀死**内核 → L3 suspend 必须**不带同步标志**（`XQueueSuspend(xq, 0)`），否则对自旋内核的死锁不可避免（mini5/mini12 教训）；
- 抢占收益受硬件调度器公平性约束；
- 时间片粒度（按上下文 vs 按通道）待多进程场景确认。

---

## 8. 规律与原理提炼（跨阶段）

1. **"模板 vs 活体"分化律**：595 驱动上，工具 trap handler 内存对象从"执行体"退化为"配置模板"——可读、可写、可触发，但不在执行路径上。这是所有失败的根源。
2. **抢占能力下沉律**：用户态"写一个寄存器位"的强制触发在 595/Blackwell 上失效；控制收敛进**调试会话（0x83de）+ GSP 固件**，且带 ~300ms 异步激活窗口——固件 RPC 架构的特征，与"微秒级 handler"是两种执行模型。
3. **官方调试器对照**：cuda-gdb 拥有完整会话栈仍不能 mid-kernel 停止——这是我倾向于认为"不是少试了某个偏移"的主要依据（单机单驱动条件下不足以断定该路径不存在）。
4. **导出表代际缺口律**：slot 19（Pascal getter）→ slot 20（Blackwell getter）——cuxtra/工具落后驱动半代；缺口用"slot20 + ToolsMemory(24/25)"标准手法补齐。
5. **RM 命令字规律**：`0x419e84` bit31=TRIGGER_TRAP；class `0xa06c` 的 `0xa06c0103/0104`=TSG 时间片 SET/GET；class `0x83de` 的 `0x83de03xx`=会话方法族；`0x2080xxxx`=subdevice。
6. **调用约定规律**：handle 传值即指针（传副本得 0）；ObjGetPc 返回设备 VA；stub 偏移在结构 `+0x18`。
7. **资产复用律**：`tools/instrument/extract_sass.py` 工具链跨 arch 复用——guardian/resume 指令数组在新架构上 **bit-exact 复现**是可行验收标准（L2 的基石）。
8. **平台通道差异**：Windows/WDDM 上一切 RM 直通（触发/TSG）被 cuxtra 的 Linux-only kmod 阻断；Linux（开源内核模块 + `/dev/nvidiactl`）才有完整通道。
9. **边界抢占设计律**：中断原语不可得时，L3 的可用形态 = **排空（L1/L2）+ 时间片协同（TSG）**；"命令 Deactivatable 标志"与"不带同步标志的 suspend"是正确用法的两个关键。
10. **负结论留证**：把走过的弯路变成可复查的证据链（日志/二进制/反汇编/抓包全套在库），供后续参考。

---

## 9. 最终状态清单

| 项 | 状态 |
|---|---|
| sm120 L2（guardian） | ✅ 实现 + 硬件验证（T6/MVE） |
| sm120 L3 Trap | ⛔ 本机未走通（实现完整保留：可获取/可 patch/可触发，但执行流不经过；默认不再路由） |
| sm120 L3 TSG | ✅ 实现 + 接线 + 端到端验证（当前默认形态） |
| `TarpHandlerSM120::GetTrapHandlerInfo` | ✅ 函数可用（slot20 绕过验证通过），仅服务本机未走通的 trap 路径；默认配置下无调用入口（保留为证据与环境变量实验路径） |
| `cuXtraSetTimeslice`（`TsgContext`） | ✅ 可用，为当前正式 L3 实现路径 |
| 路由总表 | Linux sm120/Lv3 → `CudaQueueLv3Tsg`；`XSCHED_CUDA_LV3_IMPL=TSG` 可全局覆盖；sm70/86 仍为 Trap；Windows sm120 → Lv2 |

---

## 10. 遗留问题

1. `platforms/cuda/test/main/level.cu` 在本 Linux 环境 **L1 即挂起**（LoopRunner 的 64 发射+同步风暴经 shim 触发；最小复现均通过）——与 L3 无关，未解决；
2. `ObjGetPc` 的 pc 字段语义跨平台差异（Linux/Windows 记录不一致）待查（不影响当前路径）；
3. `0x83de030c` 的 4824 字节结构未逐字段解析；
4. TSG 时间片粒度（按上下文 vs 按通道）需双进程场景确认；
5. cudbg API（`CUDBGAPI.suspendDevice`）直调为**唯一未实测的用户态缺口**（预期无果，投入产出比低）；
6. 仓库 README 支持矩阵未更新（sm120 L3 实际已有 TSG 版实现）。

---

## 附录：关键文件索引

| 类别 | 路径 |
|---|---|
| 过程报告（中文） | `test/sm120_l3_probe/L3_LINUX_REPORT_ZH.md` |
| attach 复刻计划 + Round-1/2 判决 | `test/sm120_l3_probe/ATTACH_REPLAY_PLAN.md` |
| 探针原始记录（英文） | `test/sm120_l3_probe/README.md` |
| Windows 探针源码 | `test/sm120_l3_probe/src/probe_*.cpp` |
| attach 复刻工具 | `test/sm120_l3_probe/attach_replay/{l3ctl.cpp, arm_modes.c, ioctl_log*.c, mini7b.cu, tsg_smoke.cu}` |
| attach 抓包证据 | `test/sm120_l3_probe/attach_replay/{e10b2,e15b,e17c,e18,e19,e20,e21,e22b,e23b}.txt` |
| handler 镜像/反汇编 | `test/sm120_l3_probe/evidence/linux_2026-10-09/{handler_sm120.bin, handler_880_disasm.txt}` |
| 写入/触发/实验日志 | `test/sm120_l3_probe/evidence/linux_2026-10-09/{final_writeback.log, final_trigger_probe.log, mini7_off_*.log, mini9_run1.log, mini13_run6.log, ioctl_*.log}` |
| cuxtra 静态分析 | `test/sm120_l3_probe/evidence/cuxtra_analysis.txt` |
| L3 核心代码 | `platforms/cuda/hal/src/level3/{interrupt.cpp, tsg.cpp, cuda_queue.cpp}` |
| sm120 适配代码 | `platforms/cuda/hal/src/arch/{sm120.cpp, arch.cpp}` |
| 指令工具链 | `tools/instrument/extract_sass.py`、`platforms/cuda/hal/inject/` |
