# XSched sm120 Level-3 Trap 研究总汇

> 日期：2026-10-26 ｜ 硬件：RTX 5060（sm_120）｜ 驱动：595.99.02（开源内核模块 + GSP）
> 软件：CUDA 12.9、g++-13、cuda-gdb 12.4（系统包）、Linux 7.0.0-38-generic
> 原始滚动日志：`../FINDINGS_ZH.md`（§1–§52，v6.8）｜ 证据：`../evidence/live/`、`../evidence/disasm/`
> 本文档是**整理后的总汇**，按主题分册；每条结论都标注了 FINDINGS 章节或证据日志。

---

## 一页结论

1. **生产可用的 L3 已交付**：GDB backend（`GdbInterruptContext`）实现 suspend/resume，
   设备静止 **116 ms**、真实 attach ~23 ms（`Prepare` 预热后）；测试脚本口径的
   `Suspend()` 返回 0.52–0.54 s（含脚本自身 0.5 s sleep），`Resume=1`，三轮抖动 < 3 ms。
2. **trap backend 的"停住"已打通**：RM `0317` 在受管上下文里要么 27–30 ms cancel
   （stream 判完成），要么真 park（计数冻结、`Synchronize` 不返回）；测得"设备真正停住"
   最快 0.204 ms（预建会话），22.9 ms 的尾巴是驱动 teardown。
3. **kill + replay 已闭环**：trap payload 杀掉 warp、队列记录并在 resume 时重放；
   8 s 内核 0.234 s 完成、结果正确。
4. **"停住且可恢复"在纯用户态不可达**：park 后 `0318` 在 app/agent 两个 client 上都阻塞；
   写命令字的 preemption buffer 设备侧只读、CPU 侧是 PROT_NONE 零页；
   写它的只有 `libcudadebugger` 的调试 agent。
5. **park 的真正入口**：CUDA init 前进程已在外部 ptrace 下（`strace -f` 或 plain `gdb`，
   **不需要 cuda-gdb**）+ RM `0317` → 真 park；cuda-gdb 在场时 0317 返回 0、结果是
   27.3 ms cancel（推断：其调试 agent 应答了这次 suspend）。
6. **handler 的"输入协议"已逆清**：`base-0x2000000`（会话态 `base-0x8000000`）的驱动
   preemption buffer，`+0` exit flag、`+4` 命令字（2=save/pause，1=restore）、
   `+8` preempt_idx，用 `LD.E.CONSTANT` 读；用户态没有任何写入口。v6.6/v6.8 补测：
   0317 陷阱里 pbuf 可读但 `+0/+4/+8` 全 0；trap 内 store 不 fault 但不与驱动共享。
7. **论文方法不可复现的根因**：论文靠"带内、可识别来源"的中断（ioctl 写 GPU 控制寄存器
   → trap reason 区分）；sm120 上该路径被启动期能力位 `pGpu+0x911` 关闭，唯一可触发的
   中断是 debugger-suspend。判据旧结论为"reason 恒 0x1"；v6.8 实测当前配置
   **0317=0x1、launch=0x8**（仅事件可分，仍无 target）。
8. **handler 数据读通路整体不通**：trap 上下文读普通全局内存/UVA 恒 0，读代码页/LDG
   直接 700；写全局内存正常。所以"host 写 flag、handler 读"这条路不存在。
9. **scratchpad 是唯一 host rw-s + trap 可见的区域**，但布局是驱动的 per-warp 记录
   （stride 0x2a0，字段 +0x290..+0x29e），盲读盲写即 700，不能当通用 mailbox。
10. **下一步唯一有意义的工程**：进程内调试 agent（`libcudadebugger` /
    `DebugAgentMain` / `cudbgApi*` / `IPreemptionBufferAccessor`），或者换平台
    （Ampere/V100 + 可控制 GSP 的驱动）做 A/B 验证"架构 vs 策略"。
11. **A/B 已实测（2026-10-26）**：上游 `XpuOS/xsched` 在 **RTX 3090（sm86/570）** 上
    跑通论文 L3：`level 3` preempt p50 ≈ **229 µs**、p99 ≈ 310–2233 µs、restore ≈ 90 µs；
    路径 = `TarpHandlerSM86::Instrument` + `cuXtraTriggerTrap`。同一套代码在
    **sm120/595** 上因 `pGpu+0x911` 关闭 regops 而无法触发；A100（sm80/550）regops 开放，
    但 sm80 不在上游 TarpHandler 支持列表。**论文方法可在 sm86 复现，在 sm120 不可。**
    证据/构建见 FINDINGS §49、`../evidence/ab_ampere/`。
12. **快路径是三段耦合**：managed launch（shim/debugger params）+ instrumented handler
    （+0x3e0）+ 触发（`0x20800122`）。3090 带 shim 229 µs、**去掉 shim 退化到 6–40 ms**；
    正确性 200 次抢占后 16M 元素 `bad=0`；A100/550 带 shim 因 `cuXtraKernelGetFunction`
    崩溃、无 shim 慢路径 30–55 ms；sm120 缺触发（`pGpu+0x911`）。详见 §50 与
    [07-A-B-完整实测.md](07-A-B-完整实测.md)。
13. **判据推进（v6.5–v6.8）**：handler 能读 `c[0x180]=kernel_idx`、`c[0x188]=killable`；
    当前配置 `0317=0x1`、`launch=0x8`，**事件可分但仍无 target**；驱动 pbuf 在 0317
    陷阱里全 0，trap 内 store 不共享；**裸 EXIT 会破坏重放**（`exp_reason_arm_exit.log`）。
    证据与口径见 FINDINGS §51–§52。

---

## 分册索引

| 文件 | 内容 | 适合谁看 |
|---|---|---|
| [01-研究过程.md](01-研究过程.md) | v0.1 → v5.9 的完整时间线：每阶段目标、做法、结论、证据 | 想了解"试过什么、为什么转向" |
| [02-路径与方法.md](02-路径与方法.md) | R1/R2/R3/R3'、trap backend、GDB backend、park-only 六条路径的方法与判定 | 想复用/继续某条路线 |
| [03-机制与逆向.md](03-机制与逆向.md) | trap handler 布局、SASS 编码、内存地图、preemption buffer 协议、scratchpad、RM ABI、调试栈 | 需要改代码或继续逆向 |
| [04-通道矩阵.md](04-通道矩阵.md) | host↔trap 所有读写尝试的结果矩阵，逐条解释"为什么不行" | 想找新的信息交互入口 |
| [05-工具与复现.md](05-工具与复现.md) | 构建、探针、agent、环境开关、证据日志索引、常见坑 | 要在本机/新机器复跑 |
| [06-结论与下一步.md](06-结论与下一步.md) | 需求审计、延迟账本、论文对照、Ampere A/B 方案、死路清单、风险 | 做决策/写报告 |
| [07-A-B-完整实测.md](07-A-B-完整实测.md) | 两台租用机的完整 battery：level1/2/3/TSG、去 shim、正确性、handler 实物、cuda-gdb 对照、A100 失败模式 | 想看全部实测数据 |
| [../report/01-成果汇报.md](../report/01-成果汇报.md) | 对外汇报版：一页结论、三个核心观点、需求审计、决策、8 页讲法 | 汇报/答辩/决策 |
| [../report/02-中断入口.md](../report/02-中断入口.md) | 观点一证据链：入口被关、0x20800122/0x911、0317 四态、5090 | 核对证据 |
| [../report/03-核心结论.md](../report/03-核心结论.md) | 观点一结论：论文带内 L3 在当前 sm120/595 不可实现 | 引用结论 |
| [../report/04-替代形态.md](../report/04-替代形态.md) | 观点二：GDB/0317 结果层替代、四态/park、恢复语义 | 讲"做了什么" |
| [../report/05-下一步论点.md](../report/05-下一步论点.md) | 观点三：2.5 段、target 通道缺口、选择性≠延迟、路线 | 讨论下一步 |
| [../report/06-问答.md](../report/06-问答.md) | Q&A：本轮 5 个问题 + 12 条延伸问答，逐条详细回答 | 想快速理解细节与边界 |

---

## 术语

| 术语 | 含义 |
|---|---|
| trap handler | 驱动加载的 `.text.traphandler`，GPU 异常/调试陷门时执行 |
| cave | 我们在 slot-20 指令对象 `+0xA00` 挖的 8 条 SASS（128 B）代码洞 |
| pbuf | 驱动 patch 进 handler 的 preemption buffer（`base-0x2000000`） |
| scratchpad | 驱动 CILP save/restore 用的 per-warp 记录区（`base+0x1a00000`） |
| 0x83de | `KernelSMDebuggerSession` RM class（调试器会话） |
| 0317/0318 | `DebugSuspendContext` / `DebugResumeContext` |
| park | 内核停住且工作仍在（`Synchronize` 不返回） |
| cancel | 在飞工作被取消、stream 判完成（`Synchronize` 返回） |
| 带内中断 | 论文的 ioctl 写 GPU 控制寄存器触发的、驱动不参与状态机的即时中断 |

---

## 证据根目录

```
../evidence/live/         ~280 个实验日志（exp_*.log）
../evidence/ab_ampere/    A/B 实测：3090 level1/3 日志、A100/3090 regops、A100 ioctl 序列
../evidence/disasm/       plugin_120_0.* 的 SASS 反汇编与符号
../evidence/sections/     各 handler 函数的原始二进制
../evidence/fp/           handler 指纹
../probe/                 l3x/l3qspin/l3agent/rmstack/regops/selftrap/...
../run_l3*.sh             复现脚本
```

关键日志（本总汇引用最多）：

| 日志 | 证明什么 |
|---|---|
| `exp_strace_park1/2.log`、`exp_plaingdb_park1.log` | 外部 tracer 在 init 前存在 → 真 park |
| `exp_gdb_cancel1.log` | cuda-gdb 的 agent 应答 → 27.3 ms cancel |
| `exp_hostpoke_park1.log` | pbuf 的 CPU 映射是零页，写入不可见 |
| `exp_scratch_read_fault1/2.log` | scratchpad 盲访在 launch trap 即 700 |
| `exp_timeline1/2/3.log` | 0.204 ms 停住 / 22.9 ms 收尾 |
| `exp_pre_attach1/2.log` | 预 attach 后 0317 从 cancel 变 park |
| `exp_self_resume1.log` | 自 session 发 0318 也阻塞 |
| `exp_drvbuf_4.log` | pbuf 在 trap 上下文可读、CPU/设备写均不可 |
| `exp_flag_probe2.log` | 读 flag=1 的 pbuf 在 trap 中 700 |
