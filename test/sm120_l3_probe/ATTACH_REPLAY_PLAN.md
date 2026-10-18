# sm120 Linux L3：调试器 attach 子集复刻 —— 开发流程（2026-10-14）


> **措辞修订（2026-10-19）**：本档案早期版本使用了"判死 / 物理不可达 / 能力边界 / 避免重复投入"等定性词。这些结论的适用范围仅限**单机 RTX 5060 + 驱动 595.99.02**，未经跨架构、跨驱动代际对照，现已统一改为"在本机条件下未能走通 / 我没能找到用户态手段"。**所有数据、表格取值、日志引用与实验记录均未修改。**

> 承接 `L3_LINUX_REPORT_ZH.md` §6。本轮目标：在 Linux 上打通 L3 抢占的**活跃机制**——
> 复刻 CUDA 调试器 attach 栈的最小必要子集，把 sm120 的 trap/挂起能力变成可在 xsched 内使用的原语。
> 本文件同时是**设计文档 + 逆向成果落盘**。

---

## 0. 本轮核心突破（静态逆向，全部可复现）

### 0.1 解码出 class 0x83de 的完整方法字典（内核 RM 侧已实锤）

从 `/usr/src/nvidia-595.99.02/nvidia/nv-kernel.o_binary`（ELF，未 strip，21673 符号）解析
`__nvoc_exported_method_def_KernelSMDebuggerSession`（.rodata @0xa870e0，记录 0x28B 步长）得到：

| 方法 | 名称 | 本地IMPL | 说明 |
|---|---|---|---|
| 0x83de0301/0302 | SMDebugModeEnable / Disable | - | 会话调试模式开关（GSP 转发） |
| 0x83de0307/0308 | DebugSetModeMMUDebug / Get | - | MMU 调试模式 |
| 0x83de0309 | DebugSetExceptionMask | - | 异常掩码 |
| 0x83de030b/0310 | DebugRead/ClearAllSmErrorStates | ✓ | SM 错误状态（030c=All-clear/Read） |
| 0x83de030c | DebugReadAllSmErrorStates | ✓ | 4824B 缓冲（实测载荷吻合） |
| 0x83de0313 | DebugSetNextStopTriggerType | - | 停止触发类型 |
| 0x83de0314 | DebugSetSingleStepInterruptHandling | - | 单步中断 |
| **0x83de0315** | **DebugReadMemory** | ✓ | 24B 参数（实测吻合） |
| **0x83de0316** | **DebugWriteMemory** | ✓ | 24B 参数（实测吻合） |
| **0x83de0317** | **DebugSuspendContext** | - | **暂停运行中的上下文（GSP 转发）；8B 参数** |
| **0x83de0318** | **DebugResumeContext** | - | **恢复；无参数** |
| 0x83de031a/031b | ReadSurface / WriteSurface | ✓ | |
| 0x83de031c | GetMappings | ✓ | |
| 0x83de031d | DebugExecRegOps | ✓ | 寄存器批量读写（1048B） |
| 0x83de031f/0320 | DebugSet/GetModeErrbarDebug | - | errbar 调试模式（attach 首尾 =1/=0） |
| 0x83de0321-0325 | SingleSm 系列（单步/停/跑触发、跳过空闲warp、调试器状态） | - | |
| 0x83de0326/0327 | DebugRead/WriteBatchMemory | ✓ | |
| 0x83de0328 | DebugReadMMUFaultInfo | ✓ | |
| 0x83de0329 | DebugSetDropDeferredRC | - | |
| 0x83de032a/032b | DebugSet/GetModeMMUGccDebug | - | attach 中 =1 |

**关键结论：cuda-gdb 暂停运行中内核 = `DebugSuspendContext(0317)` 一条 RM_CONTROL！恢复 = `0318`。**

> ⚠️ **更正（见下文 R1.3）**：此结论已被推翻。`0317` 挂起期间运行中的永旋内核计数器持续增长（`0315` 实测），cuda-gdb 完整 attach 会话下同样不停止运行中内核（spin2 对照）。**mid-kernel 停止不是 `DebugSuspendContext` 的语义**，真正的停止机制在本机未能找到。

### 0.2 内核侧机制（nv-kernel.o 符号 + 反汇编）

- 分配 0x83de 对象 → `ksmdbgssnConstruct_IMPL`（0x219030）：
  - 从 alloc 参数读取 2 个对象 ID（parms+4/+8）做两次全局表查找（绑定**上下文/通道组**）；
  - 调用 `kgrctxRegisterKernelSMDebuggerSession_IMPL`（0x211660）= 把会话挂到
    `grctx->[+0x148]->+0x680` 链表；析构时 `kgrctxDeregister...`（0x211690）移除。
- `gpuIsDebuggerActive_*` 等标志函数存在（调试中标志）。
- 带本地 IMPL 的方法在 `ksmdbgssnCtrlCmd*_IMPL`（0x21a7xx-0x21b0xx）；
  Suspend/Resume 无本地 IMPL → `InternalControlForward` 转发 GSP 固件执行。

### 0.3 实机 attach 报文（两轮 cuda-gdb 抓包，ioctl2_cudbg.log / ioctl_cudbg.log）

**决定性事实：应用自己的 RM client 就能分配 0x83de 并调用全部方法**（v1 日志 [385]-[411]，
hClient=0xc1d00376 即应用 client；无需调试器专用权限）。attach 序列（应用 client 版）：

```
RM_ALLOC class=0x83de (parent=device)         → 会话对象
RM_ALLOC class=0x3e   (parent=device)         → 模块调试区内存对象(0x5eb010B)
RM_MAP_MEMORY(0x4e)   映射该区域
0x83de0309 DebugSetExceptionMask (4B)
0x20801210 (subdevice, 32B)
0xA06C0103 SET_TIMESLICE on 0x5c000013 (8B)   ← 时间片写入也是 attach 流程一部分
...
```
（agent client 版另有：0x20803601 subdevice 接管 → 会话分配 → 031f=1 → 事件 → 0307/032a=1 →
0317/0318 悬停/恢复循环（含 0315/0316 内存读写、030c 状态读取）→ 收尾 031f=0 + 释放。）

### 0.4 缺口与补抓计划

现有日志未抓 **RM_ALLOC 的 pAlloc 参数缓冲**（会话分配的两 ID 绑定参数）与**非 83de 命令的
params 内容** → 需 v3 拦截器补抓一轮（`ioctl_log3.c`）。

---

## 1. 开发流程设计（四阶段）

### 阶段 A — 报文补抓与模板固化（工具：ioctl_log3.c + spin.cu）
1. v3 拦截器：抓 RM_ALLOC 的 pAlloc 内容、所有 RM_CONTROL 的 params、post 状态。
2. `spin.cu`：上下文 + 永旋内核（用于验证 suspend）；cuda-gdb -batch 会话下运行，抓全序列。
3. 产出：`attach_template.json` —— 会话分配参数 + 方法调用模板（含真实载荷）。
   **验收：与内核 construct 反汇编的字段语义互相印证（2 个绑定 ID 的来历明确）。**

### 阶段 B — 免调试器复刻（工具：l3ctl，独立 C 探针）
1. 进程内学习句柄（拦截 cuInit 期 ALLOC：client/device/subdevice/TSG）。
2. 自行分配会话（0x83de，用模板参数）→ 最小序列：031f=1 / 0307=1 / 032a=1。
3. **关键实验：0317 悬停** —— 观察永旋内核是否停止（计时/计数器观测），0318 恢复。
   **验收：suspend/resume 在无 cuda-gdb 的普通进程中生效（内核停止推进、恢复后继续）。**

### 阶段 C — 最小子集搜索 + trap 路径复验
1. 逐项开关会话方法，找出"最小可 suspend 子集"。
2. 会话活跃状态下重跑 mini7（签名载荷 + RM trigger）：验证 trap handler 是否被激活为"活体"。
   **验收：观察到注入载荷执行（trap 路径复活）或得出"trap 不需要、suspend 版 L3 已够用"的结论。**

### 阶段 D — 产品化接入 xsched HAL
1. `CudaQueueLv3Dbg`（暂名）：Interrupt=0317 悬停，Restore=0318 恢复；
   或按 xsched L3 语义封装为 kill（若 trap 复活则用 trap）。
2. 全链路测试（shim + xserver 优先级抢占）+ 回归。
3. 文档落盘（更新 `L3_LINUX_REPORT_ZH.md`）。

---

## 2. 判定树（决定最终采用哪条 L3 实现路线）

```
阶段B suspend 生效？
 ├─ 是 → 阶段C 最小子集 → 接入(CudaQueueLv3Dbg) → L3 完成（suspend 版）
 │        └─ 若 trap 同时复活 → 追加 trap 版（更低延迟）
 └─ 否 → 检查转发失败点（RM status / 内核 dmesg / GSP 响应）
          → 补前置（如 0x20803601 接管、异常掩码、模式位）后再试
```

## 3. 风险与对策

| 风险 | 对策 |
|---|---|
| GSP 拒绝无会话的 suspend | 按模板补齐前置（错误码逐项排除） |
| 0x83de 分配被策略拒绝 | v1 已证应用 client 可分配；若新版驱动收紧→走 etbl/cudbg 握手 |
| suspend 影响整机/花屏 | 先在空闲场景测试；TDR 不适用 Linux，观察 dmesg |
| 会话泄漏影响驱动状态 | 探针幂等清理（031f=0 + free），异常时重启会话进程 |

--

# Round-1 结果（2026-10-14 深夜）—— 会话机制全部实测落地

> 工具与证据：`test/sm120_l3_probe/attach_replay/`（l3ctl.cpp / k.cu / ioctl_log3-5.c / 抓包 attach_cap{1,2,4}.log / e10b2.txt / e15b.txt）

## R1.1 重大发现：cuCtxCreate 自动创建 0x83de 会话（无需 attach！）

二分实测：`cuInit` 后无会话；**`cuCtxCreate` 后立即出现会话对象（如 0x5c000073）**；
runtime-API 进程（无 driver-API ctx）不出现。=> 595 驱动为每个 CUDA 上下文**预置**
KernelSMDebuggerSession（CILP 抢占基础设施）。**复刻 attach 从"造会话"变成"用会话"。**

## R1.2 正确调用配方（全部实证，rc=0）

```
fd      = libcuda 的 /dev/nvidiactl fd（自开的新 fd 会 INVALID_CLIENT/fd 绑定问题）
hClient = 第二个零分配 client（本进程 0xc1d000XX；或 timeslice NVOS54 捕获的 hClient）
hObject = 会话对象（cuCtxCreate 自动创建的那个，如 0x5c000073）
RM_CONTROL(NVOS54) 即可：
  0315/0316 DebugRead/WriteMemory  —— rc=0，写魔数回读验证通过！
  031f/0307/032a 三种调试模式置 1  —— rc=0
  0317/0318 Suspend/Resume          —— rc=0
```
（自建会话可 alloc 成功 rc=0 但其转发 rc=87，功能不完整——下一轮再做。）

## R1.3 0317 语义精确定论：门控 ≠ 停止

- 挂起后：新 kernel launch 失败 719（CUDA_ERROR_LAUNCH_FAILED）→ 新提交被门控 ✓
- 挂起期间运行中的永旋内核**不受影响**（0315 计数器连续增长）；flag=0/1 相同；
  三种模式开启后仍不停。
- 对照实验：**cuda-gdb 完整 attach 会话下运行 spin 内核，计数器全程平滑增长**
  （spin2 实测）——即官方调试器自身的 suspend 循环同样不停止运行中内核。
- 结论：**mid-kernel 停止不是 DebugSuspendContext 的语义**。真停止机制待挖。

## R1.4 停止触发器首轮扫描（0313/0321/0322/0324）

| 调用 | 结果 |
|---|---|
| 0313 type=1/2/3（8B）| rc=31（拒绝）|
| 0322 {sm=0, on=1} / {sm=all, on=1} | rc=0，**未冻结** |
| 0321 {sm=0, on=1}（单步）| rc=0，未冻结；{sm=all}→rc=87 |
| 0324 {sm=0, skip=1} | rc=0，未冻结 |

参数语义未知（每方法 paramSize=8），需从内核 IMPL 反汇编定参数结构后再扫。

## R1.5 下一轮实验清单（优先级）

1. **反汇编 0321/0322（ksmdbgssnCtrlCmd*_IMPL 与 DISPATCH）定参数结构** → 复扫 stop-trigger；
   同时看 0309 SetExceptionMask 与 trap 的耦合。
2. **trap 路径复活**：在"会话模式已开（031f/0307/032a）+ 可选 0317 门控"状态下重跑
   mini7(签名载荷+cuXtraTriggerTrap) 与 mini9(brkpt)：验证 trap handler 是否活体化。
3. cudbg API 路线评估（GetCUDADebuggerAPI → CUDBGAPI.suspendDevice 等）。
4. 自建会话完整性：定位我们 alloc 的转发 rc=87 与 libcuda 的差异（extended params 或
   client-share 前置）。

--

# Round-2 阶段小结与 L3 设计（2026-10-14 夜）—— 本机条件下的验证结果

> 证据：attach_replay/e17c.txt e18.txt e19.txt e20.txt e21.txt e22b.txt e23b.txt、
> mini7b（触发判定）、mini9（brkpt 判定）、arm_modes.so、fifo SIGINT/cuda-gdb 对照。

## R2.1 全部停止机制的判决性实验（无一可用于 mid-kernel 停止）

| 候选机制 | 实验 | 结果 |
|---|---|---|
| 0317/0318 Suspend/Resume（会话） | 计数器+PIN 采样（e10b/e16） | rc=0 但**不冻结**运行中内核；与 cuda-gdb 自身 suspend 循环行为一致（spin2 对照全程平滑） |
| Stop/Run Trigger 系列（0313/0321/0322/0324） | 参数尺寸修正后全扫（e20） | 0313{1,2} 受理、0314/0321/0322/0324 rc=0——均**不冻结** |
| RM 触发（cuXtraTriggerTrap/0x419E84） | mini7b 武装/基线×3 offsets | **无任何效应**（连 cuda-gdb 会话下亦然，与 10/09 结论一致） |
| trap handler 注入（sm120 partial） | mini9 武装/基线 | 武装后 brkpt 从 **700 崩溃→err=0 干净终止**（异常路径被模式激活），但**注入载荷不执行**（handler 非活体） |
| cuda-gdb 真中断 | SIGINT + `info cuda threads` | **"No CUDA threads"×kernel 仍 Active**——官方调试器同样无法停止运行中内核；SIGINT 时刻仅有 suspend/读SM状态/resume 巡查（pause_cap.log t+5.94s） |
| 硬件计算抢占属性 | attr90 | =1（Linux 支持 CILP）→ 抢占由**硬件调度器**驱动，非用户态可强制 |

## R2.2 关键正向发现：上下文"门控"（真实且确定）

- **语义**：对**空闲**上下文发 `0317`（会话 0x5c0000xx，如 0x73）→ **~<300ms 后激活** →
  之后该上下文**所有新 launch 返回 719（LAUNCH_FAILED）**（E21/E23b 确定性复现，含 300ms 激活窗口）。
- 对**忙碌**上下文发 `0317` → 无效（不延后生效，E17b/E18/E19）。
- **恢复**：尝试 0318×N(+等待)、0317(1)+0318、031f(0)、0309 重置、0x801909/0x20802209、
  0302 ——**全部失败，门控单向不可逆**（E23b）。=> 语义 = "上下文击杀"（kill），非 suspend。
- 历史之谜闭环：10-14 早期矩阵的 719 = 空闲态 0317 门控遗留（当时上下文空闲）。

## R2.3 sm120 + 595 Linux 的 L3 能力矩阵（最终）

| 能力 | 状态 | 证据 |
|---|---|---|
| 会话自动存在（cuCtxCreate 即建，如 0x73）| ✅ | R1.1 |
| 会话方法调用（libcuda-fd+client[1] 配方）| ✅ | R1.2 |
| DebugRead/WriteMemory（0315/0316）| ✅ rc=0，写读回一致 | e10b |
| 调试模式（031f/0307/032a）、异常掩码（0309）| ✅ rc=0；武装后 brkpt 干净终止 | arm_modes/mini9 |
| 上下文门控（空闲态 0317）| ✅ 确定性；**单向 kill** | E21/E23b |
| **运行中内核的中段停止/抢占** | ❌ **本机未能找到可用的用户态手段（含 cuda-gdb）** | R2.1 全表 |
| TSG 时间片控制（0xA06C0103）| ✅ 写入有效；硬件调度器公平（用户 mini13）| 10/09 报告 |
| RM trap 触发 + handler 注入 | ❌（handler 非活体；武装不改）| mini7b/mini9 |

## R2.4 L3 最终设计（本平台实现形态）

**设计决策：sm120/Linux（595）上 XSched L3 采用"边界抢占 + 时间片协同"，不依赖中断原语。**

1. **Interrupt()**（由 agent→HAL 调用）：
   - 队伍无运行中内核（空闲）：可选用门控 kill（仅适用于"允许牺牲该上下文"的场景，默认不用）；
   - 有运行中内核：**退化为 L2+ 边界等待**——通过 L1/L2 机制（shim 暂停提交 + 排空），
     并配合 `0xA06C0103` 将受害 TSG 时间片临时调低（提高高优先级轮转公平性）。
2. **Restore()**：恢复提交 + 还原时间片。
3. **不采用**：trap 路径（handler 非活体，证书齐全）、门控 kill（不可逆）。
4. xsched 接线：`CudaQueueLv3Tsg`（已实现且时间片可用）为 sm120 Linux 默认 L3 形态；
   `arch.cpp` 的 sm120 分支由 Trap 改为 Tsg（或保持 env 可选）。
5. 遗留可选探索（投入产出比低）：cudbg API 直调（CUDBGAPI.suspendDevice）在 Linux 的可行性；
   GSP 侧 stop-trigger 参数语义（需固件逆向）。

**结论：在本机（RTX 5060 + 595）上"微秒级中断式 L3"未能走通；当前采用的形态是边界抢占 + 时间片协同，
文档化全部未走通的路径与证据链，供后续参考。**

## R2.5 集成落地与验证（xsched 侧，已完成）

1. `platforms/cuda/hal/src/arch/arch.cpp`：**sm120 默认路由由 `CudaQueueLv3Trap` 改为 `CudaQueueLv3Tsg`**
   （CudaQueueCreate 与 DirectLaunch 两处；含 R2 判定注释）。已增量重建并安装。
2. **端到端冒烟验证（tsg_smoke.cu）**：
   ```
   CudaQueueCreate + XQueueCreate(level=3) rc=0
   XQueueSuspend rc=0 → XQueueResume rc=0，前后 kernel 均正确执行  => SMOKE PASS
   ```
   ioctl 侧实证（LD_PRELOAD 拦截）：出现 `0xa06c0104`（TsgContext 构造的 GET timeslice）
   与 `0xa06c0103`×3（suspend 的 set-0 / resume 还原等）——**TSG 路径确实被选中并且真实下发到设备**。
3. 回归：`xsched/xsched.h` 的 XQueue API 语义（level-3 建队、suspend/resume 无同步标志）全部 rc=0。
   （`platforms/cuda/test/main/level.cu` 在本环境 L1 即挂起的已知 harness 问题与本改动无关，未涉及。）

**至此：Linux sm120 的 XSched L3 完成"设计 + 实现接线 + 验证"闭环（本机验证结果见 R2.3/R2.4）。**
