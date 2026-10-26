# sm120 Level-3 Trap for Linux — 阶段报告（2026-10-25）

> 机器：RTX 5060（sm_120），驱动 595.99.02，CUDA 12.9
> 本文是这一阶段的总报告；滚动实验细节见 `FINDINGS_ZH.md`（v0.1–v2.1），ABI 见 `RM_REVERSE_ZH.md`。

---

## 1. 一句话结论

**sm120 上的 trap 注入与"trap 内杀 block"机制已经打通并可复现**：
RM `0x83de0317` 能把 trap 送进运行中的 warp，活体 `.text.traphandler` 会执行我们注入的 SASS，
载荷可以在 handler 恢复 trap 状态后把 warp 重定向到 EXIT，从而干净地杀掉 block，
**context 存活、内核从 8 s 缩短到 ~0.24 s 结束**。
把它接进 XSched 的 L3 路径还差最后一步：运行中改写 handler 会被驱动同步阻塞；
预置改写又会让带 debugger-params 的内核的自发 trap 走错路径（715/719）。
因此 `XSCHED_SM120_L3_BACKEND=TRAP` 目前标为 **experimental**；默认仍是 TSG，
可用的 L3 抢占仍是 `XSCHED_SM120_L3_BACKEND=GDB`（已复测）。

---

## 2. 已验证的能力（含证据）

| # | 能力 | 证据 |
|---|---|---|
| 1 | `0xd04 ClientSetInheritedSharePolicy` 让跨进程 0x83de 会话从 `0x1b` 变 `status 0`；**0xcec0 创建之后再授权也有效** | `exp_policy_grant_*`、`exp_hal_e2e_*`（policy=0/0/0, session status=0） |
| 2 | 活体 `.text.traphandler` 定位（arena base + 0x15f0f00），10 条指纹校验 | `exp_hal_e2e_*`（fingerprint MATCH） |
| 3 | 在 +0x390 注入 `CALL.ABS`，cave 写签名/ reason 后正常返回，内核继续 | `exp_live_handler_sig_*`（sig 0x5aa5, reason 1, cudaSuccess） |
| 4 | trap handler 可**写**用户设备内存，**读恒为 0** | `exp_trap_ld_returns_zero.log`（in=0xdead ldout=0 ctrl=0x5aa5） |
| 5 | `LDC c[0x0][0x170]` 在 trap 内能读到内核 debugger params | `exp_ldc_param_write.log` |
| 6 | **trap kill 闭环（原语）**：armed cave 把 TRAP_RETURN_PC 重定向到 EXIT trampoline；8 s 内核 0.24–0.27 s 结束、`cudaSuccess` | `exp_arm_patch_kill_*`、`run_l3kill.sh` |
| 7 | XSched HAL 集成基础设施：ioctl 捕获 client/device/KGO、0xd04 晚授权、agent 会话、live patch | `exp_hal_e2e_*` |
| 8 | 默认 TSG 路径与 GDB 后端无回归 | `exp_hal_default_tsg.log`、本轮 l3ctl 冻结/恢复复测 |

复现：

```bash
cd xsched/test/sm120_l3_trap_v2
./build.sh l3x l3agent
./run_l3kill.sh          # 期望：victim 0.2-0.3 s 结束、cudaSuccess
./run_l3trap.sh          # 签名 0x5aa5 / reason 0x1
./run_l3policy.sh        # 0xd04 授权 + 0x83de 会话
```

---

## 3. XSched 集成状态

代码（全部在源码树内，opt-in）：

- `platforms/cuda/hal/include/xsched/cuda/hal/level3/trap_sm120_shm.h`（target↔agent 控制块）
- `platforms/cuda/hal/{include,src}/.../level3/trap_sm120.{h,cpp}`
  （ioctl 拦截、0xd04 授权、arena/live-handler 探测、arm/disarm、trigger）
- `platforms/cuda/hal/src/level3/interrupt.cpp`（sm120 走新后端）
- `platforms/cuda/hal/src/arch/arch.cpp`（`XSCHED_SM120_L3_BACKEND=TRAP` → `CudaQueueLv3Trap`）
- `test/sm120_l3_trap_v2/probe/l3agent.cpp`（独立 agent）

已跑通：`level 3` 启动 → 首个 Suspend → Init 握手（handles/policy/session）→ live patch → agent 0317。
**未跑通**：patch 之后的内核执行。

### 3.1 阻塞（三个，均已隔离定位）

0. **外部 0x83de 会话本身会破坏 XSched 的 instrumented 内核**：不设模式、不打 patch、
   不触发，仅建会话，下一次内核 launch 就 700/715；不建会话则 20 s 无错。
   这是 v1.4 已指出的 target 侧注入缺失（cuda-gdb 的 libcudadebugger）——只复刻 RM 不够。
1. **运行中改写 arena 会被驱动阻塞**：`cuXtraMemcpyHtoD(cave/0x390)` 在 kernel 运行时
   不返回（`arm write cave start` 之后无下文）。因此不能"只在抢占窗口 patch"。
2. **预置 patch 会让 debugger-params 内核的自发 trap 走错路径**：
   XSched 给内核设 `c[0x0][0x170]` 后，launch 期会出现自发 trap；
   我们的 patch 改变其处理，导致 `715 illegal instruction`（先于任何 0317）。
   只写 cave、不改 0x390 时 20 s 无错（`CAVE_ONLY`），所以问题在 handler patch。

### 3.2 下一步（明确、可执行）

1. **拿到 trap handler memobj 的 RM hMemory**，用 `0x83de0316 DebugWriteMemory`
   在运行中改写 handler（debugger 写内存不会同步 context）。当前 `slot20` 的
   `ti.handle` 是 tools 结构指针（0316 返回 0x1e），需要解出结构里的 RM handle，
   或在 `class 0x83de` 的 memory-access 路径里找到可用句柄。
1.5 或者**不再依赖运行时写**：把"请求"编码在 trap 可读的驱动 arena（preempt_buf 命令字/
   reason）里，并用保持 R12 的大偏移 BRA 做 benign 返回。当前已被"会话存在时写/启动被
   串行化"挡住，需要先解决注入状态。
2. 或者**换 patch 点**：用 GUARD cave 分别记录 launch trap 与 0317 trap 的
   `R2/reason` 和入口，找到只被 0317 经过、且可安全改写的指令。
3. 或者**周期化 guardian**：把 sm120 的 guardian 从"仅入口"扩成循环回边检查，
   这样只需 Deactivate 置 flag，不需要 trap kill（会退化为 L2+）。
4. 任何一条打通后：接回 `TrapSm120Context`，用 `l3work` 做 bit-exact 验证。

---

## 4. 复用资产

- `RM_REVERSE_ZH.md`：RM escape/结构/0x83de 全命令/权限模型/cuda-gdb 黄金时序。
- `FINDINGS_ZH.md`：v0.1–v2.1 的全过程、SASS 编码表、失败矩阵。
- `probe/l3x.cu`：live handler 定位/注入/各种 cave/触发/agent 的原型。
- `probe/l3agent.cpp`：跨进程 trap agent（0xd04 + 0x83de0317）。
- `evidence/`：所有实验日志（含 SHA256 记录）。

---

## 5. 交付判定

| 目标 | 状态 |
|---|---|
| sm120 上 trap 可送达、handler 执行注入代码 | ✅ 已证 |
| trap 内 kill block、context 存活 | ✅ 已证（独立原型，可复现） |
| 接进 XSched `CudaQueueLv3Trap` 并端到端抢占 | ⚠️ 基础设施全通，卡在运行中 handler 改写/自发 trap |
| sm120 上可用的 L3 抢占（生产） | ✅ GDB 后端（freeze/resume，已复测） |

---

## 6. 封存（2026-10-25）：能力边界与唯一剩余路径

### 6.1 失败矩阵（全部有日志）

| 路线 | 结果 |
|---|---|
| `0x419e84` / `cuXtraTriggerTrap` | 有活体 patch 时签名可见，但无法与自发 trap 区分；且不建立驱动 preempt 状态 |
| 外部 0x83de 会话（0xd04 授权） | 会话可建（status 0），但**一旦存在，XSched instrumented 内核下一次 launch 700/715** |
| 运行中 host 写 handler/cave | `cuXtraMemcpyHtoD` 阻塞；`InvalInstrCache` 阻塞到 context 空闲 |
| 运行中 device kernel 写 handler 代码页 | 无会话时可行（回读一致）；**有会话时 launch/sync 被阻塞** |
| 预置 armed patch | 自发 trap 命中 → 700（只有 0317 trap 能干净 EXIT） |
| cudbg BSS 标志置 1 | 不改变上述行为 |
| 同 client 自触发 0317 | status 0 但不送 trap |
| RS_ACCESS_DEBUG | 已打通（0xd04），不是当前瓶颈 |

### 6.2 唯一剩余路径

**在 target 侧复刻 cuda-gdb 的调试器注入**：
`CUDA_INJECTION64_PATH=<libcudadebugger.so>`、`/cuda_injection_path_shm` 握手、
`cudbgApiInit/Attach`、`DebugAgentMain`。有了注入状态后：
- 外部 0x83de 会话才可能与 XSched 内核共存（消除 700/715）；
- 再考虑把 armed cave 预置 + 用 RM 0317 触发（trap 内读驱动 arena 的请求字，
  或维持"会话下写代码不阻塞"的新假设需重测）。

工作量在驱动/代理层（R2 路线完整版），不是单次实验能收尾的。

静态线索（2026-10-25）：`libcudadebugger.so` 的 `InitializeInjection@0x1246d0`
先做版本/状态检查（状态全局：`b220b0`、`b220b8`、`b220bc`、`b2fbb0`），
失败路径 `raise(SIGTRAP)`；调用链里还有 `DebugAgentMain` 与注入 shm 相关代码。
target 侧的 `cudbgApiInit(1)` 可返回 0 且不破坏内核，但单独调用不能替代注入协议。

**协议取证（2026-10-25，`evidence/live/gdb_agent_protocol.log`）**：
- `NvDebugAgent` 是薄启动器（带 DWARF）：`argc==9`，`dlopen(argv[1])` +
  `dlsym("DebugAgentMain")`，把 `argv[2..8]` 逐个 `strtol` 后调用
  `DebugAgentMain(debugger_pid, a3, a4, a5, a6, a7, a8)`；
- 实测 argv：`libcudadebugger.so.1 56358 142 1 1 63 72 -1`
  ⇒ `DebugAgentMain(56358, 142, 1, 1, 63, 72, -1)`；
- gdb↔agent 消息格式：`u64 total_len | u32 type(=0xc) | u32 cmd | args…`，
  应答为 `u64 len | payload`；实测命令序：`0x2,0x3c,0x1c..0x20,0x4f,0x55,0x58,
  0x1b,0x52,0x1a,0x64,0x4,0x65,0x41,0x60,0x54,0x37,0x5,0x42…`
  （含设备名/sm_120 查询、句柄解析等）；
- 复刻 gdb 侧只需实现该请求/应答循环 + 正确的 fd/args 语义（142/63/72 的含义
  尚未确定），即可让 NvDebugAgent 自行建立 target 的调试态与 0x83de 会话。

### 6.3 可交付/可复现清单

```bash
cd xsched/test/sm120_l3_trap_v2
./build.sh l3x l3agent
./run_l3kill.sh     # 普通内核 trap kill：8 s -> ~0.24 s, cudaSuccess
./run_l3trap.sh     # 活体 handler 执行注入 SASS（sig 0x5aa5, reason 1）
./run_l3policy.sh   # 0xd04 授权 + 0x83de 会话
# XSched GDB L3（生产）：
xcli l3 -x <pid> -s / -q / -r      # 或 l3ctl
```

文档：本报告、`FINDINGS_ZH.md` v0.1–v2.3、`RM_REVERSE_ZH.md`、根 next-step §10。

---

## 2026-10-25 更新（v2.7）：上游 trap payload 首次在 sm120 上装机并触发

**做了什么**

1. 修正 `NO_PATCH` 语义（旧实现里设置它反而会预置 armed 桩），重做隔离实验：
   0x83de 会话本身不破坏 launch；之前的 700 来自 cave 无条件解引用 debugger param。
2. 删掉 `InstrumentTrapHandler()` 里对 sm120 的短路，在 `InterruptContext`
   构造（任何 kernel 之前）调用它 —— XSched 自带的 `trap_inject_instrs`
   被写到 handler 返回桩 `+0x880`（实测 `patch ... -> inject ... (672 B)`）。
3. `TrapSm120Context` 默认改为"上游 payload"模式，自己的 +0x390 cave 保留在
   `XSCHED_SM120_TRAP_CAVE=1`。
4. `0317` 改为 fire-and-forget；agent 增加 `L3AGENT_AUTO_RESUME` / 异步触发线程。
5. 交叉验证：同一条命令在 cuda-gdb 下，`0317` **立即返回 status 0**（两次），
   证明 RM 触发链路本身没问题；随后 `Reactivate()` 报 700，payload 的
   越界/空参路径仍需修。

**当前状态**

| 能力 | 状态 |
|---|---|
| 活体 handler 定位/patch/签名执行 | ✅ |
| 普通 kernel 上 trap → kill（`run_l3kill.sh`） | ✅ |
| XSched payload 装机到 +0x880 | ✅（新） |
| RM `0317` 触发 payload | ✅ 有 cuda-gdb 应答时；无应答时阻塞 |
| XSched 端到端 suspend/resume 稳定 | ❌ 700（payload 越界/空参待修） |
| 生产用 L3 | GDB backend |

**下一步（按优先级）**

1. 给 payload 的 block 标志写入加边界检查，或核对 `kKernelLaunchOriginal`
   的 args 布局（该路径只填 preempt_buf，其余为 0）；
2. 用 `XSCHED_SM120_TRAP_PARAM_PROBE` 在真实 trap 里 dump `c[0x170..0x188]`，
   确认打到的是哪种 launch；
3. 把 cuda-gdb 的应答替换成进程内 `NvDebugAgent` 协议（样本
   `evidence/live/gdb_agent_protocol.log`），去掉外部 gdb 依赖。

---

## 目标达成审计（2026-10-25，对照用户目标逐条）

用户目标：**在 Linux（sm120 / RTX 5060 / 595.99.02）上开发出 Level-3 trap 抢占**，
并要求自旋推进、逐步自主验证。

| # | 要求 | 状态 | 证据 |
|---|---|---|---|
| 1 | 运行中内核可被中断（非 block 边界） | ✅ | RM 0317 trap：`L3_SPIN_ONLY`/`l3qspin` 在 **27–30 ms** 内计数冻结；GDB backend 116 ms |
| 2 | 中断不破坏 context、能恢复 | ⚠️ 部分 | GDB backend：suspend→resume 后内核继续（`l3hal` 119 s→0.74 s，Resume=1）；RM trap 路径：resume 后新内核只跑几毫秒就被杀/偶发 700 |
| 3 | 在 XSched 内可用（HAL/queue 集成） | ✅ 集成 / ⚠️ 语义 | `XSCHED_SM120_L3_BACKEND=GDB` 生产可用；`=TRAP` 可 suspend、可重放启动，但 resume 后不能持续运行 |
| 4 | 不依赖 cuda-gdb（纯 trap） | ❌ | RM 0317 需要调试器侧应答；target 内 `cudbgApiInit(2)`=91、`cudbgApiAttach`=0 都不够；必须实现 NvDebugAgent 协议 |
| 5 | 可复现、可交接 | ✅ | `run_l3kill.sh`（8 s→0.234 s）、`run_l3hal.sh`（回归）、`l3qspin`、FINDINGS §1–§16、本报告 |

**结论**：要求 1、3（GDB 路径）、5 达成；要求 2/4 未达成——RM trap 命中的内核无法在
context 内恢复到"继续跑新内核"的状态（`done=0`：内核被再次 trap 杀掉；偶发 700）。
cuda-gdb 的"stop→continue"能工作，是因为它的 **agent 在 target 内主动应答每一次
launch-stop / resume**；我们目前只把它当 0317 的应答器，gdb 自身处于 passive 状态，
新 launch 会被它拦下/杀掉。

**唯一的收敛路径（下一阶段）**：实现 target 内 `NvDebugAgent` 的 gdb 侧 RPC
（帧格式 `u64 len | u32 type 0xc | u32 cmd | args`，样本 `gdb_agent_protocol.log`），
替代"外部 cuda-gdb 只做应答"的模式。完成后 `l3qspin` 的判据是：
`post-resume` 计数持续增长、`final sync` 的 `done=1`。

### 审计更新（2026-10-25 深夜，FINDINGS §26）

新增两条硬结论，已并入上文判定：

1. **`InterruptContext` 的 `InstrMemAllocator` 是 0317 语义的开关**
   （`XSCHED_SM120_TRAP_NO_ICTX_INSTRMEM=1` 可复现）：
   - 默认（分配）：0317 → **取消在飞工作**（27 ms 返回、stream 判完成、无法恢复）；
   - 跳过分配：0317 → **无操作**（status 0，但内核不被停），resume 后的重放内核
     **全程健康**（计数持续增长）。
2. **launch trap 的 reason 也包含 bit0**，因此不能用 reason 区分"0317 请求"与
   "启动 stop"（`exp_reasonarm1.log`：一启动就被杀）。

⇒ 审计表第 2 行（中断不破坏 context、能恢复）仍为 ⚠️：
现在有了"context 全程健康"的基线，但**停住内核**与**健康恢复**尚未同时成立。
下一步优先级：
- A. 在 `NO_ICTX_INSTRMEM` 基线里寻找"能停住"的触发方式（不依赖已被排除的 instr mem）；
- B. 搞清 `InstrMemAllocator` 到底改变了上下文的哪个驱动标志（cuXtraInstrMemBlockAlloc
  的副作用），据此判断能否"只取停住所需的那部分"；
- C. 调试器 agent 路线（成本最高）。

### 审计更新 2（FINDINGS §29–§30）

- 要求 1（运行中中断）：✅ 依旧成立（RM 0317 停住 27–30 ms；预 attach 后是 **park**，
  不丢在飞工作）。
- 要求 2（中断后可恢复）：❌ 仍未达成，但原因已完全定位：
  park 之后，`0318` 在 **agent client 和 app client 上都被阻塞**；对已 park 的上下文
  再次 `l3mi attach` 会挂死（rc=124）。释放 park 只能由调试器 agent 完成。
- 要求 4（不依赖 cuda-gdb）：❌ 同上；一次预 attach 可以"解锁 park 能力"，但后续
  停/恢复两个方向都需要调试器在场。

**最终一句话**：sm120/595.99.02 上，用户态可达的"停住"有两条：
(a) RM 0317（27–30 ms，但要么 cancel 要么 park 且不可释放）；
(b) 调试器 attach（~116 ms，可 park 可恢复，即现有 GDB backend）。
"停住且可恢复"目前只有 (b)。

### 审计更新 3（2026-10-26，FINDINGS §42–§46）

本轮把"park 从哪来、命令字能不能自己写"这两件事查到了底：

| # | 要求 | 状态 | 说明 |
|---|---|---|---|
| 1 | 中断运行中内核 | ✅ | 新增更快的入口：**外部 tracer 在 CUDA init 前存在**（`strace -f` 或 plain `gdb`）+ RM 0317 → 真 park（计数冻结、`Synchronize` 不返回）；不需要 cuda-gdb |
| 2 | 中断后可恢复（同 context） | ❌ | park 后 `0318` 仍阻塞；无 agent 时没人写命令字 1；CPU 侧 mprotect 读到的 pbuf 是零页，写也到不了 GPU |
| 3 | 不破坏在飞工作 / 结果正确 | ⚠️ | park 不丢工作但不可释放；cancel 路径仍靠 replay 保证结果 |
| 4 | 不依赖 cuda-gdb | ⚠️ | park 侧已可摆脱 cuda-gdb（plain gdb/strace 仅作 tracer）；**resume 侧仍必须由调试 agent（libcudadebugger）完成** |
| 5 | 可接入 XSched | ✅ | GDB backend 生产可用（0.74 s handover、Resume=1）；trap backend 具备 park/cancel/kill/replay |

**新增的硬结论**
1. 驱动 preempt buffer 在进程 VA 里只是 PROT_NONE 占位；`mprotect(RW)` 成功但 CPU
   读到的是零页，写入对 GPU/trap 不可见（`exp_hostpoke_park1.log`）。
2. scratchpad 是唯一 host 映射（rw-s）且 trap 可见的区域，但布局是驱动的
   per-warp 记录（stride 0x2a0，字段 +0x290..+0x29e）；盲读/盲写会 700，不能当通道
   （`exp_scratch_read_fault1/2.log`）。
3. 命令字由 `libcudadebugger` 的 `IPreemptionBufferAccessor` 写（字符串与 RTTI 证据），
   0315/0316 只能访问 RM memobj 且有边界检查——纯 RM 路线写不到 preempt buffer。
4. 因此完整 park+resume 的唯一入口是**进程内调试 agent**；其余组合均已排除。

**给后续工程的最短路径**
- 若只需要"停住后不恢复"：用外部 tracer（自写 PTRACE_SEIZE 守护）+ RM 0317。
- 若需要"停住后可恢复"：实现/复用 `libcudadebugger`（`DebugAgentMain` /
  `cudbgApi*` / preemption-buffer accessor）的进程内 agent，而不是继续找 RM 命令。

### 审计更新 4（2026-10-26，FINDINGS §49）— **论文 L3 在 sm86 上复现成功**

**实测**：上游 `XpuOS/xsched`（commit `f49289f`）在 **RTX 3090（sm86，570.124.04，
GSP on）** 上原样构建并运行 `level 3`：

| 配置 | preempt avg | p50 | p95 | p99 | restore avg |
|---|---|---|---|---|---|
| sm86 / level 1 | 11.7–13.2 ms | 12.5–13.2 ms | 13.7 ms | 13.8 ms | 6.9 µs |
| **sm86 / level 3** | **230.6–257.2 µs** | **227.3–228.9 µs** | 242–250 µs | 310–2233 µs | **90–97 µs** |

- 路径：sm86 → `CudaQueueLv3Trap`；`InterruptContext::InstrumentTrapHandler()` 用
  `TarpHandlerSM86::Instrument` 改写活体 handler → `cuXtraTriggerTrap`（`0x419e84`
  bit31，经 `0x20800122`）。dead-loop 内核也能停住并恢复。
- **只写寄存器不插桩 handler 不够**：A100（sm80/550）上 regops 开放（可读
  `0x200510`/`0x419e84`，写 bit31 返回 0），但未插桩 handler，内核照跑；且 sm80 不在
  上游 TarpHandler 支持列表（只 sm70/sm86）。cuda-gdb 在 A100 仍走 0317/0318。
- **sm120/595**：`0x20800122` 被 `pGpu+0x911` 早退，`cuXtraTriggerTrap` 无法投递；
  只能 0317（27 ms）/ GDB（116 ms）。

**结论**：论文的 queue-based L3 **可以在 sm86/570 复现**；sm120 的差别是 **595 驱动关闭了
regops 能力位**，不是 handler 机制本身。"架构 vs 策略"的 A/B 因此有了答案：同样的上游
代码在 sm86 上工作，sm120 不可用；sm80 不在上游支持列表，不能作为 sm86 的反例。

**证据**：`evidence/ab_ampere/logs/level1_sm86.log`、`level3_sm86.log`、`smi.txt`、
`uname.txt`；构建/复现步骤见 `compendium/05 §8`。

### 审计更新 5（2026-10-26）— 完整 battery：快路径是**三段耦合**

| 配置 | p50 | 说明 |
|---|---|---|
| 3090 level 2 | 1.257 ms | Lv2 flushing，restore 57 µs |
| 3090 level 3 ×3 | 230–241 µs | p99 0.42–2.8 ms；restore 93–127 µs |
| 3090 level 3 TSG | 13.35 ms | dead-loop 180 s 超时 |
| 3090 level 3 **去 shim** | 6–40 ms | 快路径依赖 managed launch |
| 3090 正确性 | — | 200 次抢占后 16M 元素 `bad=0 -> OK` |
| A100 level 3 带 shim | — | SIGSEGV in `cuXtraKernelGetFunction`（550 兼容性） |
| A100 level 3 无 shim | 33.6–46.4 ms | 驱动级慢路径；TSG 0.58–55.7 ms |
| A100 sm80→SM86 arch 补丁 | — | 同一个 cuxtra 崩溃；不能证明 SASS 兼容 |

- handler 实物：sm86/sm80 都是 0x1200，`+0x3e0` 都是 `IADD3 R1,R1,-0x10,RZ`；上游
  `TarpHandlerSM86::Instrument` 在 +0x3e0 做 JMP + inject + 跳回。
- cuda-gdb：3090/A100 都是 `0307 → 0317 → 0315/030c/0316 → 0318`，与 cuxtra 触发是两条
  独立路径。
- **结论**：论文快路径 = managed launch（shim/debugger params）+ instrumented handler
  （+0x3e0）+ 触发（`0x20800122`）。3090 三段齐全 → 229 µs；3090 去 shim → 6–40 ms；
  A100 缺 cuxtra/550 兼容 → 带 shim 崩、无 shim 30–55 ms；sm120 缺触发（`pGpu+0x911`）
  → 0317 27 ms/GDB 116 ms。对 sm120 的启示：只补 handler 或只找触发都不够，必须同时
  解决"managed launch + 插桩 + 触发"，或走调试 agent。

**完整整理**：`compendium/07-A-B-完整实测.md`；FINDINGS §50；证据
`evidence/ab_ampere/`（battery_3090、correct_sm86.log、sm86_handler*、
level3_*noshim*.log、*_agent_ioctl_0317.log）。


### 审计更新 6（2026-10-26，FINDINGS §52）— 驱动 pbuf 的 cmd/idx 实测：**全 0**

用户提出的最后一条 mailbox 候选（0317 时驱动写 `pbuf+4=2`、`+8=preempt_idx`，cave 读出来
即有 request/target）已完成可控实测：

| 项 | 结果 |
|---|---|
| cave 载荷装入时机 | `Arm()`（launch 之后、lazy 会话之前），此后到 0317 无 launch |
| 回读通道 | `cuMemHostAlloc(DEVICEMAP)` 记录页，CPU 直接读（park 下也可靠） |
| 0317 陷阱 `R2` | **0x1**（sentinel 实测） |
| 设备 writer 自己的 launch trap | **0x8** |
| `pbuf+0/+4/+8` | **0 / 0 / 0**，可读不 fault（`base−0x8000000`） |
| 0317 语义 | 通道级挂起 + 队列重放（计数冻结后从 ~1M 重跑），非 CILP 原地恢复 |

- cmd=2 若被写入会让 reason 变成 `0x30`（`0x10|0x20`），实测 `0x1` ⇒ 驱动自己读到的
  命令字也是 0。**agent 发起的 `0317{1,0}` 不写 pbuf**；只有真调试器 suspend
  （cuda-gdb attach + target 注入）才写。⇒ pbuf 不能做 request/target 通道。
- 副产品：**reason 可区分**（0317=`0x1`、launch=`0x8`），v4.3 的 bit0 门控成立；但
  0317 没有 target 数据，只能全体 abort，不能选择性 abort。
- 证据：`evidence/live/exp_arm_sentinel_{dev,host}.log`、
  `exp_arm_pbuf_{cmd4,idx8,flag0}.log`；探针 env `XSCHED_SM120_TRAP_ARM_PBUF_PROBE`/
  `..._ARM_SENTINEL`/`..._HOST_ARM`。
