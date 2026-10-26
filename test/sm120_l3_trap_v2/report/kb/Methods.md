# Methods：sm120 trap 研究的方法、证据标准与自更正

> 最后验证：2026-10-27　主驱动：595.99.02（5060）；595.71.05（5090）另注。
> 本文回答"**我们怎么知道**"：证据标准、方法、证据账本、旧结论更正、坑。
> 事实/地址/编码 → `ABI.md`；构建/运行/故障 → `Runbook.md`；结论/论点 → `../03`、`../04`、`../05`。

---

## 1. 证据标准（先看这一节）

### 1.1 证据等级

| 等级 | 含义 | 例子 |
|---|---|---|
| **E1 实测** | 有日志/计数器/host 回读，可重复 | `suspend 27.3 ms`、签名 `0x5aa5` |
| **E2 代码/反汇编** | 二进制/源码/反汇编直接可见 | `0x2dd122 cmpb $0,0x911` |
| **E3 论文/文档** | 论文原文、NVIDIA 文档 | OSDI'25 §6.3 |
| **E4 推断** | 由 E1–E3 推出，明确标注 | "开闸可能恢复论文语义" |

**写结论时必须标注等级**；E4 不能当 E1 用。

### 1.2 "停住"的三判据（缺一不可）

| 判据 | 说明 |
|---|---|
| **计数冻结** | host-mapped 计数器在 suspend 时刻停止，保持整个 hold |
| **`Synchronize` 状态** | stream/queue 的同步状态与"停住/取消/重放"一致 |
| **host 回读一致** | 设备侧结果与 host 回读一致（防止"只是采样没看到"） |

**反例**：只看 `Suspend()` 返回、只看 stream 判完成、只看单次采样，都不算。

### 1.3 采样口径

| 口径 | 用途 | 注意 |
|---|---|---|
| 1 ms 采样 | 延迟分解（v1.3） | 与 5 ms 采样不可直接比 |
| 5 ms 采样 | 早期头条（116 ms） | 头条值与 1 ms 分解有差异 |
| 时间线 `L3Q_TIMELINE` | 0317 的 freeze vs `Suspend` 返回 | 0.204 ms vs 22.9 ms 是两个量 |

### 1.4 单变量与副作用判据

- **一次只改一个变量**：session、`InstrMemAllocator`、`NO_ICTX`、pre-attach、writer/host arm；
- **副作用判据**：RM 控制返回 0 **不代表执行**；要看寄存器回读/计数/签名；
  `status=0` 但 `value=0` 往往是"op 被跳过"的签名（gate 早退）；
- **对照**：没有对照的"成功"不可信（例：B 单独跑 0.36 s vs B+A 33.65 s）。

---

## 2. 方法目录

### 2.1 静态方法

| ID | 方法 | 步骤 | 判据/产出 | 限制 |
|---|---|---|---|---|
| **M-01** | 符号/反汇编 | `objdump -d`, `nm`, `readelf` 于 `nv-kernel.o_binary` | 函数地址/闸门/返回码 | 只对未 strip 的对象 |
| **M-02** | 插件对象提取 | 从 libcuda 的 ELF 头切 `.text.traphandler`/`do_pause`/`do_read` | 段大小/符号/重定位 | 需要 ELF/节表工具 |
| **M-03** | 指纹 | 取 live 入口前 0xa0 的 10 条 opcode 比对 | `MATCH/MISMATCH` | 换驱动可能变 |
| **M-04** | 字节扫描 | `grep -abo $'\x84\x9e\x41\x00'` | 常量在哪个二进制 | 只能是"存在性" |
| **M-05** | nvdisasm | `nvdisasm -b SM120a` 反汇编 cubin/对象 | SASS 语义/编码 | 需要正确架构 |

### 2.2 动态方法

| ID | 方法 | 步骤 | 判据/产出 | 限制 |
|---|---|---|---|---|
| **M-06** | ioctl 拦截 | `LD_PRELOAD`/hook `NV_ESC_RM_ALLOC/CONTROL` | client/device/subdev/channel/compute handles | 只拦当前进程 |
| **M-07** | regops 读写 | `0x20800122` 读 PTIMER/PMC；写 `0x419e84` | 返回码 + 回读值 | 被 `0x911` gate |
| **M-08** | cave 探针 | `+0x390` CALL → 8 槽 cave → ST 到 host 页 | 签名/值/是否执行 | 8 槽/寄存器约束 |
| **M-09** | sentinel | host 写 magic，cave 覆盖低 32 位、高 32 位保留 | 证明 cave 执行 | 读仍可能 0 |
| **M-10** | host 映射记录页 | `cuMemHostAlloc(DEVICEMAP)` 4 KB | park 状态也能读回 | trap 写需可达 |
| **M-11** | 时间线 | `L3Q_TIMELINE`，1 ms 阈值 | req→freeze / req→返回 分离 | 需 host-mapped 计数 |
| **M-12** | GDB MI2 | `-target-attach` → `*stopped` → detach | 冻结/恢复时间 | 需 ptrace 权限 |
| **M-13** | A/B | 3090/570、A100/550、5060/5090 同码对照 | 平台差异 | 驱动/架构混杂 |

### 2.3 对照配置

| 配置 | 目的 |
|---|---|
| 有/无 0x83de session | 会话对 launch/写代码的影响 |
| 有/无 `InstrMemAllocator` | 0317 的 cancel vs no-op |
| 有/无 `NO_ICTX` | 判据/停止语义 |
| 有/无 pre-attach/tracer | park 入口 |
| host arm vs 设备 writer | 运行期写 cave 的可行性 |
| 有/无 shim（3090） | 三段耦合的依赖 |

---

## 3. 证据账本

> 只列**关键结论**；每条给证据与置信度。完整数据在 `evidence/`。

| ID | 结论 | 证据 | 置信度 | 反例/边界 |
|---|---|---|---|---|
| **E-01** | 5060/5090 的 `0x20800122` 被 `pGpu+0x911` 在写前拦截 | 反汇编 + PTIMER/PMC 读 0 + 写 `0x419e84` status 0x3a | 高 | 只对 595 两版；其他驱动未验 |
| **E-02** | 3090/570 三段齐全 → 229 µs、bad=0 | `battery_3090/level3_*`、`correct_sm86.log` | 高 | 单流单 kernel；A100 不适用 |
| **E-03** | sm120 live handler 可定位/可 patch；cave 可执行 | fingerprint、`+0x390` 回读、签名 `0x5aa5` | 高 | 8 槽约束；R1/R2 |
| **E-04** | 0317=0x1、launch=0x8（当前配置） | `exp_arm_sentinel_{host,dev}.log` | 高 | 0x8 跨配置未保证 |
| **E-05** | 0317 陷阱 pbuf `+0/+4/+8` 全 0；store 不共享 | `exp_arm_pbuf_*` | 高 | 私有/快照映射解释 |
| **E-06** | GDB 冻结 116/106 ms；attach 23、CILP 80；恢复 8–10 ms | FINDINGS v1.3；`latency` | 高 | 5060；5090 只验能力 |
| **E-07** | 0317 设备停 0.204 ms；`Suspend` 返回 22.9–30 ms | `exp_timeline{1,2,3}` | 高 | 懒建会话 +5 ms |
| **E-08** | kill+replay 8 s → 0.234 s（standalone） | `run_l3kill.sh` | 高 | 5060；5090 未复现 |
| **E-09** | 0317 四态：benign/cancel/no-op/park | `exp_sig_rm0317`、`exp_managed_spin1`、`exp_no_ictx_instrmem`、`exp_strace_park1` | 高 | park 不可释放 |
| **E-10** | 5090 handler 布局与 595.99 一致；GDB/0317 可用；managed kill 未复现 | `evidence/rtx5090_595_71/` | 高/部分 | kill 3.4 s、devflag 失败 |
| **E-11** | 显存不释放；host `cudaMalloc` 可幂等；device malloc 红灯 | 624→634 MiB 实测；机制 | 中高 | 一次样本 |
| **E-12** | 0318 ≠ 重放；重放主语是 XQueue | 代码 + `trap reactivate` 日志 | 高 | 无 XQueue 则无自动重放 |
| **E-13** | 受管+InstrMemAllocator 下 0317 陷阱里 cave 可执行（R2=0x1），但驱动仍 cancel/replay | `exp_arm_sentinel_host.log`（record 0x1 + `trap reactivate`） | 高 | 不能把 cave 执行当决策权；仅观察/记录 |

---

## 4. 自更正日志（旧结论 → 反例 → 更正）

| ID | 旧结论 | 反例 | 更正 | 影响 |
|---|---|---|---|---|
| **C-01** | v1：sm120 trap 不可用（判死） | 活体 handler 定位 + 注入执行 | 判死作废 | 启动 v2 |
| **C-02** | v2.6：外部 0x83de 会话破坏 launch | `NO_PATCH` 语义 bug（设置它反而预置 armed 桩） | 会话本身不破坏 launch | 重测 launch |
| **C-03** | v3.5：launch-stop trap（reason 0） | 探针 `ST.E` 编码写错（源寄存器放偏移） | 不是 launch trap；首次抓到 reason 0x8 | 修正探针 |
| **C-04** | v4.3：reason bit0 无法区分 0317/launch | 当前配置 0317=0x1、launch=0x8 | 事件可分（仍需精确值/复验） | 更新判据 |
| **C-05** | “trap 上下文 ST 写 pbuf 必 700” | 0317 陷阱 ST 不 fault（但不共享） | 只对 launch trap 必 700 | 修正写侧结论 |
| **C-06** | “论文 handler 读全局 flag” | OSDI'25 §6.3 + 代码 | 论文 L3 无 host→handler buffer；target=queue+kernel_idx | 修正论文理解 |

**方法教训**：任何"不可用/恒 0/必 700"的结论，都要问三件事：
① 探针编码对吗？② 地址是对的状态吗？③ 有没有对照？

---

## 5. 坑清单（症状 → 根因 → 修复 → 验证）

| ID | 症状 | 根因 | 修复 | 验证 |
|---|---|---|---|---|
| **P-01** | cave 执行后 700 | 动了 R1 | 只用 R4–R11 | 计数/签名 |
| **P-02** | handler 重执行 fault 指令 | 清零 R2（reason） | 保留 R2 | 签名/reason |
| **P-03** | 后续分派异常 | 未恢复 `MOV R3,RZ` | 返回前恢复 R3 | 回读/trap |
| **P-04** | 死循环/回到 CALL | 忘记 `IADD R12,0x10` | 加 IADD + RET.ABS | 回读 |
| **P-05** | 700/越界 | `ST.E` 源寄存器/偏移写错 | 源在 w1 低字节，偏移 w0 高 32 | 小步验证 |
| **P-06** | 指针错误 | `LDC` 只读低半 | 读 `c[0x170]`+`c[0x174]` | 打印两半 |
| **P-07** | JMP 后 700 | 用了 sm86 JMP 编码 | 用 CALL.ABS/已验证 BRA | 回读/执行 |
| **P-08** | launch 时 700 | 预置读 pbuf 的 cave 被 launch trap（0x8）命中 | 0317 后再读/按 reason 门控 | 日志 |
| **P-09** | pbuf 读/写错 | 地址重定位 | 每次读 `do_read+0x120/0x130` | 地址打印 |
| **P-10** | scratchpad 盲读 700 | 只覆盖活动 per-warp 窗口 | 只读驱动布局内 | 活动 warp |
| **P-11** | 写 table 后 trap 700 | `trap_reason_table` 是驱动逻辑 | 不写 | 复测 |
| **P-12** | 运行期写 cave 阻塞 | session 串行化 | Init 预装/设备 writer | 0.2 ms |
| **P-13** | 0317 阻塞/0318 阻塞 | 无应答/调试器状态 | cuda-gdb/agent 应答；或接受 stop-only | 状态 |
| **P-14** | 重放失败/700 | 裸 EXIT 破坏 trap 状态机 | 用驱动原生 cancel/guard | replay 日志 |
| **P-15** | A100 带 shim 崩 | `cuXtraKernelGetFunction`/550 兼容 | 不带 shim 或换平台 | SIGSEGV |
| **P-16** | 5090 kill 3.4 s | devflag/pbuf 未校准 | 重读 pbuf/timing | `devflag` ack |
| **P-17** | cave 有记录但内核仍被杀/重放 | 驱动 context cancel 优先于 cave | 把 cave 当记录器；恢复用 XQueue/GDB/开闸 | record + `trap reactivate` 日志 |

---

## 6. 新增条目模板

### 6.1 新方法 `M-xx`

```
M-xx | 名称 | 目的 | 前置 | 步骤 | 判据 | 限制 | 证据
```

### 6.2 新证据 `E-xx`

```
E-xx | 结论 | 证据（日志/反汇编/数字） | 置信度 | 反例/边界 | applies_to | last_verified
```

### 6.3 新更正 `C-xx`

```
C-xx | 旧结论 | 反例 | 更正 | 影响文档 | 证据
```

### 6.4 新坑 `P-xx`

```
P-xx | 症状 | 根因 | 修复 | 验证 | 证据
```

---

## 7. 机器索引（关键词 → ID）

| 关键词 | 条目 |
|---|---|
| gate / 0x911 / regops | E-01、M-07、P-08 |
| reason / 0x1 / 0x8 / 0x30 | E-04、C-04、ABI §2.2 |
| pbuf / 重定位 / 全 0 / 不共享 | E-05、P-09、ABI §5 |
| cave / R1 / ST.E / CALL | E-03、P-01..P-07、ABI §3 |
| GDB / 116 / 106 / 23 / 80 | E-06、M-12 |
| 0317 / 0.204 / 22.9 / 四态 / park | E-07、E-09、P-13 |
| kill / replay / 0.234 / XQueue | E-08、E-12、P-14 |
| 3090 / 229 / A100 / 5090 | E-02、E-10、M-13 |
| self-correction / 旧结论 | C-01..C-06 |
| 700 / 715 / 719 | P-01..P-16、`Runbook.md` §5 |
