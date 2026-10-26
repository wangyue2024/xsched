# sm120 Trap 研究封存档案（索引与导读）


> **措辞修订（2026-10-19）**：本档案早期版本使用了"判死 / 物理不可达 / 能力边界 / 避免重复投入"等定性词。这些结论的适用范围仅限**单机 RTX 5060 + 驱动 595.99.02**，未经跨架构、跨驱动代际对照，现已统一改为"在本机条件下未能走通 / 我没能找到用户态手段"。**所有数据、表格取值、日志引用与实验记录均未修改。**

> 封存日期：2026-10-16　｜　状态：**已封存**（研究结论已定，资料冻结）
> 主题：NVIDIA sm120（Blackwell，RTX 50 系列）上 XSched Level-3 **trap 路径**的完整尝试、证据与未走通结论
> 硬件/驱动：RTX 5060（sm120 / CC 12.0）+ Linux 595.99.02（CUDA 13.2，开源内核模块）；探针阶段为 Windows 25H2 + 驱动 13030（WDDM）

→ **先读本文档，再按下方导览进入详细材料。**

---

## 1. 一句话结论

**sm120（+595 驱动）上，中断式 Level-3（trap 路径）未能走通：我没能找到可用的用户态手段。**

trap 全链路实现成功（handler 获取、改写、触发均逐环字节级验证），但硬件 trap 的执行流**不经过**用户可写的 handler 内存对象（"非活动模板"）；真正的 trap 激活收敛进 CUDA 调试器 attach 栈，复刻后实测——包括 cuda-gdb 本体在内，"运行中内核的中段停止"均未能实现。全部负面路径均已证据化留档（见 §3 证据速查表）。

## 2. 研究脉络（三阶段）

| 阶段 | 内容 | 关键产出 |
|---|---|---|
| ① Windows 探针 | 导出表扫描 / slot20 深挖 / handler 信息获取 | 确认 slot 19 缺 Blackwell 分支（error 101）；slot 20 = 事实上的 GetInfoBlackwell（memObjHandle + 0x880 stub 偏移） |
| ② Linux trap 实现 | GetInfoBlackwell 绕过、handler 逆向（4864B）、patch@0x880 返回桩、RM 触发（0x419e84） | **逐环成功、唯独"执行"不落**：签名载荷（0x0/0x60/0x880 三处）、真实 brkpt、cuda-gdb 会话三路验证均不执行 → 未走通 |
| ③ attach 复刻 | class 0x83de（KernelSMDebuggerSession）33 方法字典、l3ctl 免调试器复刻、判决实验全表 | **本机实测**：我没能找到可实现 mid-kernel 停止的用户态手段（含 cuda-gdb 实测对照） |

核心 commits：`ff14d06`（探针 workspace）→ `2eb2e63`（Linux 实现与根因）→ `2db49a5`（attach 复刻证据 + 汇总）。

## 3. 核心证据速查表

| 结论 | 证据文件 |
|---|---|
| slot 19 报 error 101；slot 20 = GetInfoBlackwell | `evidence/probe_slot20_20261003_225053.log`、`evidence/probe_export_table_run2.err.log` |
| handler 4864B 结构与布局（A/B 变体 + 0x880 共享返回桩） | `evidence/linux_2026-10-09/handler_sm120.bin`、`handler_880_disasm.txt` |
| 补丁写入字节级一致（patch site 0x880，JMP 回写验证） | `evidence/linux_2026-10-09/final_writeback.log` |
| RM 触发可调用（idle/busy 瞬时返回、上下文健康） | `evidence/linux_2026-10-09/final_trigger_probe.log` |
| **强制触发不执行注入代码**（三处签名载荷全不落） | `evidence/linux_2026-10-09/mini7_off_{0x0,0x60,0x880}.log` |
| **真实 trap（brkpt）不走该 handler**（700 报错回收） | `evidence/linux_2026-10-09/mini9.cu` |
| 调试器 attach 栈 = 真激活机制（专用 RM client / 0x83de 方法族） | `evidence/linux_2026-10-09/ioctl_{normal,cudbg,2_cudbg}.log` |
| 会话 suspend（0317）不冻结内核；空闲态门控 = 单向 kill | `attach_replay/e10b2.txt`、`e21.txt`、`e23b.txt` |
| stop-trigger 全家族不冻结 | `attach_replay/e20.txt` |
| cuda-gdb 本体的 SIGINT 也停不住内核（SIGINT 时刻计数器平滑） | `attach_replay/sigint_abs.log`、`pause_cap.log` |
| TSG 时间片写入有效（旁证，非本主题） | `attach_replay/tsg_smoke.cu`、`evidence/linux_2026-10-09/mini13.cu` |

> 注：`attach_replay/` 下 6 个原始抓包日志（`attach_cap1/2/4.log`、`pause_cap.log`、`sigint_abs.log`、`smoke_ioctl.log`）为**本地留存，未纳入云端备份**（体积原因）。云端可用的对应证据为精选抓包 `e*.txt` 系列（含 e10b2/e15b/e17c/e18/e19/e20/e21/e22b/e23b）。

## 4. 文档导览

| 文档 | 定位 |
|---|---|
| `README.md`（英文） | **Windows 探针原始记录**：workspace 设计（T0–T4 步骤）、静态逆向（cuxtra ETID GUID 表、nvcuda 导出）、探针结果矩阵、Linux follow-up 与 Round 2（排除过程） |
| `L3_LINUX_REPORT_ZH.md`（中文） | **Linux trap 开发全记录**：环境搭建 → GetInfoBlackwell 绕过 → handler 逆向 → 实现 → 验证矩阵 → 根因定位（§5）→ 复现指南（§7）+ Round-1/2 附录 |
| `ATTACH_REPLAY_PLAN.md`（中文） | **attach 栈复刻计划 + Round-1/2 判决**：class 0x83de 33 方法字典、l3ctl 实测、停止机制判决全表（R2.1）、平台能力矩阵（R2.3）、设计结论（R2.4） |
| `L3_FINAL_SUMMARY_ZH.md`（中文） | **全阶段汇总报告**（写于 2026-10-15）：原理/实现清单/证据链/判决/规律提炼。注意其 §7 的 TSG 叙事存在后续认知更新（见本文 §5） |
| `src/`、`include/`、`tools/`、`build.ps1`、`run.ps1` | Windows 探针源码（6 个 probe）与静态分析工具（analyze_cuxtra.py / analyze_nvcuda.py），可独立复现探针实验 |
| `evidence/` | Windows 探针日志（probe_*.log）+ cuxtra/nvcuda 静态分析文本 + Linux 全部实验证据（`linux_2026-10-09/`） |
| `attach_replay/` | attach 复刻工具（`l3ctl.cpp` 515 行、`arm_modes.c`、`ioctl_log3/4/5.c`）与抓包证据（`e*.txt`）、测试内核（`spin*.cu`、`mini7b.cu`、`k.cu`、`tsg_smoke.cu`） |

## 5. 认知更新（对早期文档的勘误/定性）

1. **TSG 时间片路径不再作为 Level-3 成果记录。** 早期工作（`L3_LINUX_REPORT_ZH.md`、`L3_FINAL_SUMMARY_ZH.md` §7）曾把时间片机制作为 sm120 的 L3 落地形态记录；后续核查确认准确定性是"trap 未走通后的降级/替代机制"：
   - 语义 = 时间片**换出**（非中断），粒度 = **进程/TSG 级**；
   - 限制：同进程多队列场景会**全进程冻结**（任一队列 suspend → `SetTimeslice(ctx, 0)` 影响整个上下文）；仅适用于进程间场景；
   - 效果（内核实际让出、跨进程抢占收益）**从未完成验证**；
   - 启用方式：环境变量 `XSCHED_CUDA_LV3_IMPL=TSG`（上游 1.2.0 既有能力，非本工作新增）。
2. **`release/sm120-TSG` 分支已撤销**（2026-10-16）：该分支曾为 sm120 增加"arch 默认路由到 TSG"的改动；经裁决"改默认引入与上游的行为分叉及使用陷阱"，已从云端与本地删除，回归 env-only 启用方式。
3. **本封存档案以 trap 研究为唯一主题**。时间片、会话门控等其他机制的记录仅作为"排除过程/旁证材料"保留，不再从成果角度叙述。

## 6. 复现指引

- **Windows 探针**：见 `README.md`（`build.ps1` / `run.ps1` 用法）
- **Linux trap 全链路**（构建、补丁、触发、签名载荷实验、cuda-gdb 对照）：见 `L3_LINUX_REPORT_ZH.md` §7
- **attach 复刻**：见 `ATTACH_REPLAY_PLAN.md` 阶段 A–D 与 `attach_replay/l3ctl.cpp` 头注释

## 7. 给后来者（本机条件下未走通，记录走过的弯路）

- ⛔ 不要再套用 sm86 配方（patch trap handler memobj + 写 RM 位触发）——已在 3 处 offset、真实 trap、cuda-gdb 会话三种条件下均不执行；
- ⛔ 不要再把该 handler 内存对象当"活体"使用——595 上它是非活动模板，执行流不经过它；
- ⚠️ 调试器 attach 栈已完整复刻（33 方法字典 + 调用配方），结论是"该栈同样不提供 mid-kernel 停止"——除非驱动版本变化，无需重做；
- 🔭 尚未穷尽的用户态方向（低优先级）：GSP 固件逆向（stop-trigger 参数语义、CILP 接口）、cudbg API 直调（`CUDBGAPI.suspendDevice`）；
- ♻️ **重启条件**：若未来驱动版本重新开放用户态 trap 激活路径，`platforms/cuda/hal/src/arch/sm120.cpp` 中保留的 `TarpHandlerSM120`（GetTrapHandlerInfo / Instrument / SetJumpInstruction）可直接接回（路由在 `arch.cpp`，参照 commit `2eb2e63` 的接法）。

## 8. 关联分支与提交

| 分支 / 提交 | 内容 | 云端 |
|---|---|---|
| `sm120-level3-probe`（`ff14d06`、`2eb2e63`） | Windows 探针 + Linux trap 实现与根因 | ✅ 已备份 |
| `dev/Linuxlevel3`（`2db49a5` + 本封存提交） | attach 复刻证据、汇总文档、本档案 | ✅ 本提交后备份 |

---

## 附：v2 结论修订（2026-10-16，追加，不改动上文）

> 上文（v1 封存）的核心结论"sm120 上运行中内核不可中断"**已被推翻**。
> v2 的完整记录在 `test/sm120_l3_trap_v2/`（`FINDINGS_ZH.md` v0.1–v0.6、`RM_REVERSE_ZH.md`）。

### 一、v1 判死依据的失效

1. **"cuda-gdb 也停不住运行中内核"** —— 依据是 `info cuda threads` 输出 "No CUDA threads"
   与同进程 `cudaMemcpy` 采样的计数器平滑。前者在设备确实已冻结时仍会打印 `Status=Active`；
   后者在挂起时会**阻塞而不是返回旧值**。改用 `/dev/shm` + 独立观察进程重测：
   cuda-gdb 在 8.22 s 停驻窗口内计数器只前进 7.44 M（≈0.2 s 的量），冻结真实存在。
2. **"slot20 的 4864 B 对象是活体 trap handler"** —— 它其实是重定位后的 `.text.do_pause`
   （handler 的被调用者），不是 trap 入口。真正的活体 handler 是 libcuda 内嵌、运行时下载到
   设备内存的 SASS plugin（arena base+0x15f0f00）。

### 二、v2 新增的可复现能力

`test/sm120_l3_trap_v2/run_l3mi.sh`：外部进程驱动一个 cuda-gdb MI agent，
**按指令冻结/恢复运行中的内核**：

| | |
|---|---|
| 中断延迟（请求→设备静止） | **116 ms** |
| 恢复延迟 | ~115 ms |
| 重复性 | ✅ 三轮冻结窗口 1.40 / 1.42 / 1.40 s（请求 1.5 s） |
| 目标侧要求 | 仅 `prctl(PR_SET_PTRACER, ANY)`；**不需要断点、不需要轮询点** |
| 粒度 | per-context（冻结期间桌面与其它进程不受影响） |

另有 `run_l3poc.sh`（断点式，延迟约 30 ms，但需要在目标里放一个轮询点）。

### 三、仍然不可行的部分（新的能力边界）

- ~~资源让渡~~ → **已证实可行（2026-10-16 更正）**：早期"冻结不让渡"的结论来自一个**不产生竞争**
  的实验配置（两个内核本来就能并存）。把占用拉满后（A = 20000 block × 256 线程霸占 GPU，
  B = 1 block × 32 线程瞬时内核）：
  **不冻结 A 时 B 要 33.65 s，冻结 A 4 s 后 B 只要 0.74 s（45×）**
  ⇒ 冻结确实把 SM 资源让渡给了其它内核。证据：`test/sm120_l3_trap_v2/tools/handover_test.sh`。
  仍不可复现的是 sm70/86 那种 **trap handler 内 kill block** 的语义，但那已不是让渡的必要条件。
- **设备侧 trap**：`cuXtraTriggerTrap`（RM 全局寄存器 0x419e84 bit31）在 595/sm120 上不送 trap；
  自建 `class 0x83de` 会话因缺 `RS_ACCESS_DEBUG`（`os_check_access()` 对该权利硬编码 `return NV_FALSE`）
  而在跨 client 时返回 0x1b、在同 client 时静默空转；`NV_ESC_RM_SHARE`(0x35) 也不授予该权利。
- **调试器改 PC 杀 block**：cuda-gdb 拒写 CUDA PC（`cuda_value_to_register` 断言）。
- **延迟**：116 ms 与 sm70/86 的微秒级差 4–5 个数量级。

### 四、复用价值

`RM_REVERSE_ZH.md` 里恢复出来的 RM ABI 是**可复用的资产**：
RM escape 全表、`class 0x83de`（KernelSMDebuggerSession）全部 32 条控制命令
（名字/参数尺寸/flags/IMPL 符号）、`NV2080_CTRL_CMD_GPU_EXEC_REG_OPS = 0x20800122`、
NV 状态码、RM 权限模型（含 `rsAccessCheckRights`/`_rsAccessGetSharedRights`/`os_check_access` 的代码地址）、
以及 cuda-gdb 真实 suspend 的完整 RM 时序。后续任何 sm120/Blackwell 的 GPU 抢占工作都可以直接用。
