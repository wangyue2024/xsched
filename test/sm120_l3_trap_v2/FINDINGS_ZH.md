# sm120 Level-3 Trap 复活实验记录（v2）

> 起始：2026-10-16　分支：`dev/Linuxlevel3`　机器：RTX 5060（sm120）+ Linux 595.99.02 + CUDA 12.9 工具链
> 目标：在 sm120 上实现**运行中内核的中断式 L3**（trap 语义），不是 L2 guardian，不走 TSG。
> 前置：`../sm120_l3_probe/ARCHIVE_ZH.md`（v1 判死封存）。本文件是 v2 的滚动记录，**结论以本文件为准**。

---

## v0.1（2026-10-16）— v1 判死的前提被推翻：活体 handler 已定位

### 1. 新证据：libcuda 内嵌"活体" trap handler plugin

`/usr/lib/x86_64-linux-gnu/libcuda.so.595.99.02` 内有 314 处 ELF 头，其中 **48 个 EM_CUDA(190) ET_REL
可重定位对象**含以下段（sm_120 = `e_flags 0x11007804`，对象在文件偏移 `0x047ef858`/`0x047fc370`
（+两个副本 `0x048154a4`/`0x04821fbc`）；sm_121 = `0x11007904`）：

| 段 | 尺寸(var0/var1) | 作用 |
|---|---|---|
| `.text.traphandler` | 2432 / 2304 | **trap 入口**（`BPT.DRAIN` 开头），按 trap reason 位掩码分派 |
| `.text.do_read_trap_reason` | 2432 / 2304 | 计算 reason 位掩码 |
| `.text.do_pause` | 2560 / 2304 | 保存 warp 状态到 scratchpad，调试器停驻 |
| `.text.do_pause_quiet` | 384 | 打标记 + `BPT.DRAIN` + 返回（外层循环反复调用 = 冻结） |
| `.text.do_preempt_save` / `restore` | 512 | 置标志后 `JMP` 进 `cilp_traphandler_save/restore` |
| `.text.cilp_traphandler_save` / `restore` | 8704/8832、7808 | CILP 全量状态保存/恢复 |
| `.text.do_detect_continuation`、`do_set_defer_continuations`、`do_handle_continuations`、`get_defer_continuation_state_and_clear` | 256~512 | continuation 处理 |

对象带完整 `.symtab`（354 符号）、`.rela.text.*`、`.note.nv.cuinfo`。外部（未定义）符号即宿主需回填的槽：
`CUDA_PREEMPTION_BUFFER_PATCH_ADDR`、`scratchpad`、`trap_reason_table`、`pause_reason_table`、
`INVOKE_AFTER_SAVE(_INST2)`、`INVOKE_BEFORE_RESTORE(_INST2)`、`DEBUGGER_MODULE_JCAL_PATCH_LOC`、
`cilp_trap_handler_save/restore`。

工具：`tools/extract_plugins.py`（切对象+符号+重定位）、`tools/dump_sections.py`（切段）、
`tools/make_fingerprints.py`（生成指纹）、`evidence/disasm/*.txt`（`nvdisasm -b SM120a`，0 错误）。

### 2. trap 入口的分派语义（`evidence/disasm/plugin_120_0.traphandler.txt`）

```
+0x000 BPT.DRAIN / BMOV MACTIVE=0xffffffff / 保存 R0-R13,PR,Rpc,OPT_STACK,B0,THREAD_STATE_ENUM 到本地栈
+0x210 S2R R2, SR_GLOBALERRORSTATUS; if (R2 & 4) {
+0x250   R2:R3 = CUDA_PREEMPTION_BUFFER_PATCH_ADDR      ← 重定位槽
+0x270   + vsmid*0x630b0 + 0x59030 + vpcid*0x310
+0x310   LD.E.64 R4, [+0x188]; SETLMEMBASE R4 }
+0x370 loop: LEPC R12; CALL do_read_trap_reason → R2 = reason 位掩码
        bit0(0x01) → CALL do_preempt_restore; CALL do_pause;            R2=1; goto loop
        bit1(0x02) → CALL do_detect_continuation
        bit2(0x04) → CALL do_set_defer_continuations
        bit3(0x08) → CALL do_pause;                                     R2=1; goto loop
        bit4(0x10) → CALL do_preempt_save;  CALL do_pause;              R2=1; goto loop
        bit5(0x20) → CALL do_pause_quiet;                               R2=1; goto loop
        bit6(0x40) → CALL get_defer_continuation_state_and_clear
        bit7(0x80) → CALL do_handle_continuations
+0x710 恢复 TRAP_RETURN_PC/Rpc/OPT_STACK/B0/寄存器/THREAD_STATE_ENUM → 返回被陷入的 PC
```

`do_read_trap_reason`（`evidence/disasm/plugin_120_0.do_read_trap_reason.txt`）里 reason 的**可控来源**：

```
+0x130 R4:R5 = CUDA_PREEMPTION_BUFFER_PATCH_ADDR
+0x270 R0 = [preempt_buf + 4]
         R0 == 2 → reason |= 0x10 | 0x20   (preempt_save + pause_quiet)   ← 挂起
         R0 == 1 → reason |= 0x01 | 0x20   (preempt_restore + pause_quiet) ← 恢复
+0x1a0 [preempt_buf + 0x1c + (vpcid>=32 ? 4:0)] 的 (vpcid&31)*2 位 → reason |= 0x40
+0x0a0 [scratchpad + 0x10 + (vsm*0x30+vpcid)*0x2a0 + 0x29d] != 0 → reason |= 0x20
+0x310 SR_GLOBALERRORSTATUS / SR_WARPERRORSTATUS → 0x10 / 0x01 / 0x08 / 0x02 / 0x80
```

⇒ **`preempt_buf+4` 是"命令字"**：写 2 后一旦 warp 进入 handler，就会走 CILP 保存 + pause_quiet，
外层 `R2=1; goto loop` 使其反复重读 reason → **只要命令字保持 2，warp 就滞留在 handler 里 = 运行中内核被冻结**；
写 1 → do_preempt_restore 恢复；写 0 → reason 为空 → 走完 epilogue 返回内核。这正是 L3 挂起/恢复语义。

### 3. v1 判死结论的错误点（已定位）

v1 把 slot20 报的 4864B(0x1300) 内存对象当"活体 handler"，并在其 0x0/0x60/0x880 三处打签名载荷，
结论是"注入不执行 → handler 非活体 → 判死"。实测（`evidence/live/compare_live.txt`）：

- 该 0x1300 对象的 **[0,0xA00) 与 plugin `.text.do_pause` 逐指令相同**（opcode 0/160 不匹配，
  仅 5 对重定位槽不同）→ 它是 **`do_pause` 这个被调用者**，不是 trap 入口；
- 入口 `.text.traphandler` **不在这个对象里**；
- `do_pause` 只在 reason 含 bit3/bit0/bit4 时才被 CALL。v1 触发时 reason=0（无调试器、命令字未设），
  **入口直接走完 epilogue 返回**，所以"载荷不执行"是必然的——与"handler 非活体"无关。
- 该对象 [0xA80,0x1300) 是一段 8 次重复的 trampoline（`CALL.ABS.NOINC` 到 arena 内共享例程，
  参数 UR4:UR5 = base+0xc17600+i*0x100，返回地址 base+0x1604ee0+i*0x110）。

### 4. 活体镜像已在设备内存中定位（Phase 2 完成）

公共 API 完全看不到 arena（`read_probe`：`cuMemcpyDtoH_v2`=INVALID_VALUE、`cuMemGetAddressRange`=NOT_FOUND、
`cuPointerGetAttribute`=INVALID_VALUE），且 **cuxtra 的 `cuXtraMemcpyDtoH` 在部分失败路径上直接 `exit(1)`**
（`--map` 实测：base+0/+0x10000 可读，base+0x20000 进程即死），故盲扫不可行。

改用 cuxtra 内部符号 `cuxtra::cuda::FindMemObjHandle(ctx, va, &obj, &off, &size)`（memobj.cpp.o，
它内部用 ToolsMemory 表 **slot 26 `CtxEnumerateMemBlocks` / slot 27 `CtxEnumerateMemObjs`**）作为
**非致命的"VA 是否属于某 tools memobj"探针**，配合 slot 24/25（ObjGetPc/ObjGetSize）遍历 arena：

- `probe/arena_walk.cpp`：3911 次探测 → **649 个对象**，全部可安全读回。
- arena 布局（每次运行基址不同，相对偏移固定；base = trap_reason_table − 0xc38300）：

| base+ | 内容 | 匹配 |
|---|---|---|
| 0x15f0f00 | **`.text.traphandler`（活体入口）** | plugin_120_1.traphandler，8 处差异全在重定位槽 |
| 0x15f2100 | `.text.do_read_trap_reason`（活体） | 0/144 opcode 差异 |
| 0x15f3300 | do_pause 副本（0xb00） | — |
| 0x15f5300 | **slot20 报的 0x1300 对象**（= do_pause + trampoline） | 0/160 |
| 0x15f6600 | do_pause 副本（0xb00） | — |
| 0x15f9000 | `cilp_traphandler_save`（0x2b00） | ✓ |
| 0x15fbb00 | `cilp_traphandler_restore`（0x2700） | ✓ |
| 0xc38300 / 0x1000000 / 0x1a00000 | trap_reason_table / pause_reason_table / scratchpad | 由重定位解出 |

- 活体入口的重定位已回填：`CUDA_PREEMPTION_BUFFER_PATCH_ADDR = base − 0x2000000`
  （该次运行 = `0x78aae8000000`，arena base = `0x78aaea000000`），
  CALL 目标指向 base+0x15f2xxx/0x15f3xxx/0x15f5xxx/0x15f6xxx（与上表一致）。
- 活体 `do_pause` 的 `INVOKE_AFTER_SAVE/BEFORE_RESTORE` 四槽为 0（未被调试器打补丁）。

### 5. 下一步（进行中）

1. **E3（判定性）**：把签名载荷打到**活体入口** base+0x15f0f00 的 +0xa0（此处 R4-R7 已在 +0x20/+0x90 存栈，
   可安全覆写），再 `cuXtraTriggerTrap` → 若签名落盘，则"RM 触发能进入活体 handler"成立，v1 判死彻底推翻。
2. **E2**：读 `preempt_buf+4` 现值；写 2 + 触发 → 观察永旋内核计数器是否冻结（带外采样）；写 1 → 是否恢复。
3. 若 E3/E2 成立 → 把 `trap_inject_instrs`（check_preempt_trap）接到活体入口，走 XSched 原生 L3（杀 block + resume）。

### 证据索引（本目录）

| 文件 | 内容 |
|---|---|
| `tools/extract_plugins.py` / `dump_sections.py` / `make_fingerprints.py` / `compare_live.py` | 提取、切段、指纹、活体比对 |
| `probe/find_live_handler.cpp` | slot20 解析 + arena 推导 + （盲扫，已被 arena_walk 取代） |
| `probe/read_probe.cpp` | 读原语能力矩阵（公共 API 不可见 / cuxtra 致命性） |
| `probe/arena_walk.cpp` | **arena memobj 遍历 + 指纹匹配（Phase 2 主力工具）** |
| `evidence/plugin_120_*.o`、`*_symbols.txt`、`*_relocs.txt` | 从 libcuda 切出的 sm120 plugin 与符号/重定位 |
| `evidence/sections/*.bin`、`evidence/disasm/*.txt` | 各段裸码与 nvdisasm 反汇编 |
| `evidence/fp/*.fp`、`MANIFEST` | 指纹（仅重定位自由前缀） |
| `evidence/live/arena_walk_run1.log` | 649 对象清单 + 匹配 + 立即数解码 |
| `evidence/live/obj_*.bin` | 活体镜像 dump |
| `evidence/live/compare_live.txt` | 活体 vs plugin 的逐槽差异与地址解码 |

---

## v0.2（2026-10-16）— R0 判决：cuda-gdb **确实能冻结**运行中内核；v1 平台判死结论作废

### 1. R0 ground truth（方法学修正后的重测）

v1 判"cuda-gdb 也停不住"的两条依据都不成立：
- `info cuda threads` / `info cuda kernels`：本次实测在**设备已冻结**时仍打印 `Status=Active`、
  `No CUDA SMs.`（`evidence/live/r0_gdb.log:39-50`）——该输出不能作为"未停止"的证据；
- 同进程 `cudaMemcpy` 采样：设备冻结时该调用会阻塞而非返回旧值（v1 的 spin2 对照因此无效）。

修正方法（`probe/shm_spin.cu` + `probe/shm_watch.c` + `run_r0.sh`）：计数器放在 `/dev/shm`
（`cudaHostRegister(Mapped)`）由内核直接发布，**外部进程**每 100ms 读取，与被调试进程是否被停无关。

结果（`evidence/live/r0_gdb.log`，8.22s 停驻窗口）：

| 阶段 | 计数器速率 |
|---|---|
| 正常运行 | 36.9 M iter/s |
| cuda-gdb 断点停驻的 8.22 s 内 | **仅 +7,441,920 次（0.91 M/s）**≈ 0.2 s 的量 |
| `continue` 之后 | 立即回到 36.9 M/s |

⇒ **设备在断点停驻后约 0.2 s 内完成冻结，`continue` 后立即恢复**。这就是可达的中断式 L3 原语，
且完全在用户态（调试器栈）内。v1 的"能力边界"结论作废。

注：cuda-gdb 会话下 GPU 时钟被锁到基频（同样 cycles 的内核墙钟时间 30s→116s，≈653MHz vs 2527MHz），
做时延测量时必须扣除这一因素。

### 2. 命令字路线（preempt_buf+4）单独不够：缺"送 trap"

- `probe/dev_access.cu`：内核态对 preemption buffer **可读可写**（写 2 回读 2；宿主侧
  `cuXtraMemcpyHtoD` 写同一地址会被**同步阻塞到内核结束**，实测 17.78 s，故宿主侧写不可用）。
- `probe/freeze.cu`（`evidence/live/exp_freeze_run2.log`）：设备侧瞬时写入 cmd=2 → 宿主
  `cuXtraTriggerTrap` → 计数器全程 36.9 M/s **不冻结**；清 0 再触发同样无效。
  加 `cudbgEnablePreemptionDebugging/cudbgDebuggerInitialized/cudbgEnableLaunchBlocking=1`
  （libcuda 导出的 BSS 标志，`exp_freeze_dbgflags.log`）仍无效。
  ⇒ **RM 触发（0x419e84 bit31）在 595/sm120 上确实送不进 trap**（v1 这一条结论成立），
     命令字只是"进了 handler 之后怎么分派"，不能替代"让 warp 进 handler"。
- `probe/selftrap.cu`（`exp_selftrap_cmd2_brkpt.log`）：内核自己 `brkpt` + cmd=2 →
  **整个上下文被杀**（全部 block 计数器同时停在 t≈0.6s，`cudaStreamSynchronize` =
  `cudaErrorLaunchFailure`）——未武装调试模式时 brkpt 是致命异常，不是停驻。

### 3. 顺带查明的两个坑（会影响后续所有实验）

- **CUDA 惰性模块加载**：helper 内核**首次**启动会串行化在正在运行的内核之后
  （`probe/concurrency.cu` test1 = 2.42 s，test2-4 = 0.000 s；freeze run1 = 17.09 s）。
  所有实验必须先 warmup 每个内核模块。
- 本机 RTX 5060 = 30 SM，`concurrentKernels=1`，同上下文多流可并发（预热后）。

### 4. 当前主攻方向

既然"设备冻结"可达且由调试器栈完成，下一步是**抓取真实 suspend 时刻的 RM 序列并复刻**：
`tools/ioctl_log6.c`（带时间戳、解 NVOS21 RM_ALLOC 的 pAllocParms、NVOS54 params 落盘）
+ `run_r1.sh`（cuda-gdb `set exec-wrapper` 注入拦截器 + 外部 /dev/shm 观察者）。
v1 的抓包已知：`0x83de0317`(SUSPEND_CONTEXT) 调试器发的就是 **8 字节全零**、
`0x83de0318`(RESUME) **0 字节**，会话对象 0x5c00000c、client 0xc1d0038b，
序列 = `031f=1 → 0307=1 → 032a=1 → 0317 …`（`../sm120_l3_probe/evidence/linux_2026-10-09/ioctl2_cudbg.log`）。
v1 复刻了同样的字节却不冻结 ⇒ 差异应在**会话对象的创建参数/绑定**或**前置状态**，
本轮抓 RM_ALLOC 的 pAllocParms 与完整时序来定位。

---

## v0.3（2026-10-16）— R3 收官：完整恢复调试器 suspend 配方，但被 RM 权限模型挡住；发现新闸门 `pGpu+0x911`

本节所有 ABI / 命令表 / 地址细节已抽到独立工具书 **`RM_REVERSE_ZH.md`**，这里只写判定与推论。

### 1. 先修好了三个"让我们一直看不见真相"的工具缺陷

| 缺陷 | 后果 | 修复 |
|---|---|---|
| `ioctl_log6.c` 里 `NV_ESC_RM_DUP_OBJECT` 常量写成 `0xc02c4634`（真值 `0xc01c4634`） | 5 次 DUP 全部没解码，只留一行裸 req | 改正 + 增加 `NVOS55` 字段解码 |
| RM_ALLOC 只 dump `psize` 字节，而所有 class 都传 `psize=0` | **最关键的 0x83de 分配参数一个字都没抓到** | `psize==0` 时按 class 默认窗口 dump 64 B |
| 日志用 `fopen(path,"w")`，cuda-gdb 与目标两个进程写同一路径互相 truncate | 之前所有"混合日志"其实是被截断的残片，时序无法对齐 | 文件名追加 `.<pid>` |
| `run_r1.sh` 的 gdb `run` 不带参数 | 目标程序用了默认 shm 名，外部观察者一直在看错对象 | 加 `set args` |

⇒ **v1 与 v2 前期"复刻了同样的字节却不冻结"的判断，建立在一份被截断、且缺了分配参数的日志上。**

### 2. 黄金参考：真实 cuda-gdb suspend 的完整配方（已抓到）

`evidence/live/r3_ioctl.log.20138`（目标）/ `.20144`（debug agent）/ `r3_watch.log`（外部观察者）。
时序全文见 `RM_REVERSE_ZH.md §6`。要点：

- debug agent 是**独立进程**，自己跑了一遍完整 libcuda 初始化（有自己的 client/device/subdevice）。
- 它的会话参数 = `{hDebuggerClient=0, hAppClient=目标 client, hClass3dObject=目标的 0xcec0 对象}`。
- 冻结发生在 `t=3.3519` 的 `0x83de0317 {1,0}`，直到 `t=13.3674` 才 `0x83de0318`，
  正好 10.02 s（= gdb 脚本里的 `shell sleep 10`）；外部观察者计数在这 10 s 内**一动不动**。
- 之前那 11 轮 `0317{0,0} → 0315/030c/0316 → 0318` 是 attach 阶段的"暂停-读写-恢复"轮询，
  其中 `0316`(WriteMemory) 就是**把活体 plugin 下载进设备内存**的动作。
- **目标进程的 RM 流在"被调试/不被调试"下逐条一致**（与 `plain_ioctl.log.20403` 对比：
  class 计数、命令集合、唯一一次 `0x83de0309` 全同）⇒ 目标侧不做任何"授权"动作。

### 3. 判决：R3（复刻 RM 序列）在**同进程**内不可行，卡点是 RS_ACCESS_DEBUG

穷举结果（日志：`exp_rmdev.log`、`exp_rmstack_2nddev.log`、`exp_rmstack_ownclient.log`、
`exp_rmstack_propectl.log`、`exp_rmstack_sweep.log`、`exp_l3x_x*.log`）：

1. **自建 RM client 无法分配 `class 0x80` device**：11 种变体（新 fd ± CARD_INFO ± SYS_PARAMS
   ± 先建 0xcb33 ± libcuda 的 client 级控制前缀 / 用 libcuda 的 fd / psize 0,4,56 / 各种 handle 风格）
   **全部 0x33**。唯一可行的是"在 libcuda 自己的 client 下再建第二个 device 对象"。
2. 同进程 `RM_DUP_OBJECT` 把 app 的 device 复制进自建 client → **0x33**。
3. 用第 1 条的可行路径建出的**同 client 会话**：分配成功、`031f/0307/032a=1` 全部 status 0、
   `0x83de0317 {1,0}` **status 0**，但计数器 36.9 M/s 前后不变 ⇒ **完全空转**。
   旁证：`030c ReadAllSmErrorStates` 与 `031c GetMappings` 返回全 0；
   `0301 SMDebugModeEnable` 返回 0 但毫无后续效果。
4. **绑定对象穷举**：8 个 compute object（`0x5c00001b/21/24/28/2b/2f/32/36`）逐个建会话并 suspend
   → 全部 status 0、全部不冻结；channel / TSG / device / subdev / session 作为 `hClass3dObject`
   → **0x31 NV_ERR_INVALID_OBJECT**。⇒ 会话只能绑 `KernelGraphicsObject`，而且绑对了也没用。
5. **跨进程**（`l3x --role both`：父进程当 agent，fork 出 target，先 `PTRACE_ATTACH` 再 `cuInit`）：
   任何 `(hAppClient=目标 client, hClass3dObject=任意)` 组合 → **0x1b
   NV_ERR_INSUFFICIENT_PERMISSIONS**。

根因（静态逆向，见 `RM_REVERSE_ZH.md §5`）：

- `ksmdbgssnConstruct_IMPL@0x219030` 在绑定前调用
  `rsAccessCheckRights(目标的 KernelGraphicsObject, 调用者 client, mask = 1<<2)`，
  失败即 0x1b。**bit2 = RS_ACCESS_DEBUG。**
- 全局策略（`serverInitGlobalSharePolicies@0x35e3f0`）只授予 `RS_ACCESS_NICE`，
  从不授予 DEBUG；`os_check_access()`（开源层）对 DEBUG 直接 `return NV_FALSE`
  ⇒ **CAP_SYS_ADMIN / ptrace 都拿不到 DEBUG 权利**。
- 唯一可能的授予入口是资源级 share policy：`NVOS21.pRightsRequested`（抓包里全 0）
  或 escape **0x35 = NV_ESC_RM_SHARE**（24 B，抓包里 cuda-gdb 与目标**都没调用过**）。

⇒ **cuda-gdb 的 agent 究竟怎么拿到 DEBUG 权利，仍是唯一的未知项。**
已排除：目标侧 RM 动作、ptrace attach、attach 时机（我们在 `cuInit` 之前就 attach 了）、
会话参数、绑定对象选择。

### 4. 新线索：`pGpu+0x911` 是 reg-op 的总闸门（可能是比 suspend 更值钱的入口）

顺带把两条"寄存器批量读写"通路都逆完了：

- `0x83de031d DebugExecRegOps`（psize 0xc88 = `{u8 flagsByte; u32 count; RegOp32 ops[100]}`）
  实测：`flagsByte=0` → **0x1f**，`flagsByte=1` → **status 0**，`count=0` → **0x3a**。
  但 `regStatus/regValue` 全 0 —— 因为 `ksmdbgssnCtrlCmdDebugExecRegOps_IMPL@0x21a990`
  在 `if (!*(NvBool*)(pGpu + 0x911)) return NV_OK;` 处直接返回。
- `0x20800122 NV2080_CTRL_CMD_GPU_EXEC_REG_OPS`（**subdevice 级，我们自己就是 owner，
  不需要任何调试器会话**，psize 0x30）：`subdeviceCtrlCmdGpuExecRegOps_cmn@0x2dcfc0`
  同样在 `cmpb $0x0,0x911(%r12); je → return NV_OK` 处被一票否决；
  另外要求 `hChannel` 与 `hObjectContext` 同为 0 或同非 0，op 数组 32 B 步长，
  校验 `*(u16*)(op+0xa)`。

**如果能置起 `pGpu+0x911`（以及相邻的 `pGpu+0x910` 标志字），
我们就在用户态拿到了特权 GPU 寄存器读写**——那正是 v1 缺的"把 trap 送进运行中 warp"的能力
（`cuXtraTriggerTrap` 走的是 `GlobalRegsWrite32`，很可能也被同一闸门挡住，
这可以解释 v1"RM 触发送不进 trap"的现象）。这是目前**性价比最高的下一步**。

### 5. 路线状态总览

| 路线 | 状态 | 结论 |
|---|---|---|
| R0 ground truth | ✅ 完成 | cuda-gdb **能**冻结运行中内核（10 s 内计数零增长），v1 判死作废 |
| R1 活体 plugin patch | ⏸ 阻塞 | 活体 handler / CILP buffer / preempt buffer 的 VA 都已定位并可读写，但**缺"送 trap"**；`brkpt` 自触发会杀上下文 |
| R2 CUDBGAPI | ⏳ 未开工 | `InitializeInjection` 是**无参**的，`GetCUDADebuggerAPI@0x149ac0→0x16e4b0` 返回客户端 API 表；libcuda 侧 `cudbgGetAPI` 经 `*(0x578fba0)` 间接跳转（未 attach 时为 NULL，返回 0xa） |
| R3 复刻 0x83de | ❌ 同进程判死 / ⏸ 跨进程差一个授权 | 配方 100% 恢复，闸门是 RS_ACCESS_DEBUG |
| **R3' reg-ops 闸门** | 🆕 新主线 | 找到 `pGpu+0x911`，两条通路共用；一旦打开即得特权寄存器读写 |

### 6. 本轮产出物索引

| 文件 | 内容 |
|---|---|
| `RM_REVERSE_ZH.md` | **新增**：RM escape 表 / 结构体 / 0x83de 全命令表 / 状态码 / 权限模型 / cuda-gdb 黄金时序 / libcuda·libcudadebugger 入口 |
| `probe/rmdev.cpp` | 自建 client→device 的 11 种变体诊断（`exp_rmdev.log`） |
| `probe/rmstack.cu` | 同进程会话构建 + 模式位 + 控制探测 + reg-ops + 8 个 compute object 扫描 |
| `probe/l3x.cu` | `--role target/agent/both`：fork+ptrace 的跨进程会话扫描 |
| `probe/regops.cpp` | `0x20800122` subdevice reg-ops（结构体尺寸待按 §3.1 修正） |
| `run_l3x.sh` | 跨进程实验一键脚本 |
| `tools/ioctl_log6.c` | 修正 DUP 常量、`psize==0` 时 dump 分配参数、按 pid 分文件 |
| `evidence/live/r3_*` | cuda-gdb 真实 suspend 的黄金抓包 |
| `evidence/live/plain_ioctl.log.*` | 无调试器基线（与上者逐条一致） |
| `evidence/live/exp_rmdev.log`、`exp_rmstack_*.log`、`exp_l3x_*.log`、`exp_regops.log` | 本轮全部实验日志 |

### 7. 补测：`0x20800122` 已能从我们自己的 subdevice 打通到闸门前（`exp_regops2.log`）

`probe/regops.cpp` 按 §4 恢复的布局重测：

| 参数 | 结果 |
|---|---|
| `hChannel=0, hObjectContext=0` | **0x3a NV_ERR_INVALID_PARAM_STRUCT** |
| `hChannel=0x5c00001a, hObjectContext=0x5c00001b`（libcuda 自己的 channel + compute object） | **status 0**，但 `regStatus=0 regValue=0` |
| 读 `NV_PMC_BOOT_0(0x0)` / `NV_PTIMER_TIME_0(0x200510)` / `_TIME_1` / GR_CTX 类型 op | 同上，全部 status 0 且**不执行** |

⇒ **`NV2080_CTRL_CMD_GPU_EXEC_REG_OPS` 对我们自己的 subdevice 是可达的、权限检查已通过**，
唯一的拦路虎就是 `pGpu+0x911 == false` 时那句 `return NV_OK`。
这是一个**单点、可定位、可验证**的目标，比"拿到 RS_ACCESS_DEBUG"现实得多。

### 8. 下一步任务安排（优先级从高到低）

四条线互相独立，任一条打通即可回到 Phase 3 做正式注入。按"性价比 = 成功概率 × 收益 / 成本"排序：

| # | 路线 | 做什么 | 判据 | 成本 |
|---|---|---|---|---|
| **#11 R3'** | **攻 `pGpu+0x911`** | 在 `nv-kernel.o_binary` 全 `.text` 里找 `0x911(%r*)` / `0x910(%r*)` 的**写入点**，反推 setter（候选：`kgspIsDebugModeEnabled_GB100@0x21c930`、`kgrctxRegisterKernelSMDebuggerSession_IMPL@0x211660`、`0x83de0301 SMDebugModeEnable` 的自动生成存取器、`gpuSetUserRegisterAccessPermissions_IMPL@0x1fa440`）；再找触发它的用户态调用（RM 控制 / escape `0x4d RM_ACCESS_REGISTRY` / `/proc/driver/nvidia/params` 的 Rm* 键） | `bin/regops` 读到 `NV_PTIMER_TIME_0` 非 0 且两次读数递增 | 低（单点目标，已有可用探针） |
| **#14 R3''** | **`NV_ESC_RM_SHARE`(0x35) 授予 DEBUG** | 反汇编 `rmapiShareWithSecInfoTls@0x35e2c0 → clientShareResource_IMPL@0x3b5960` 定 NVOS39 六字段与 `_nv04Share` 里 `{2,1}` 的含义；在 `l3x` 的 target 侧对 `compute` 对象 share 给 agent 的 client | agent 的 0x83de 分配从 `0x1b` 变 `0`，且 suspend 窗口内外部计数器零增长 | 低-中 |
| **#13 R1'** | **硬件 CILP 抢占当 trap 源** | 用 `arena.h` 定位活体镜像 → 备份 → 把最小签名载荷（`sm120.cpp BuildTestPayload` 编码）patch 到 `do_preempt_save` 入口 → 用两个互相竞争的长内核进程诱发抢占 | `flag == 0x5AA5` 落盘 ⇒ 活体 handler 确认可执行，判死结论彻底推翻 | 中（有 GPU hang 风险，需备份+单进程实验） |
| **#12 R2** | **CUDBGAPI / libcudadebugger** | 反汇编 `GetCUDADebuggerAPI@0x149ac0→0x16e4b0` 取表地址 → dump 函数指针 → 用库内字符串（"Failed to fully suspend the context" 等）定位 `suspendDevice/resumeDevice` 槽位；或试 `CUDA_INJECTION64_PATH` / 进程内 `cudbgApiInit+cudbgGetAPI` | 拿到 API 表并能 suspend/resume 运行中内核 | 中-高（表布局全靠字符串交叉定位） |

**建议执行顺序：#11 → #14 → #13 → #12。**
理由：#11 是一个已经打到"最后一道 if"的单点问题，探针现成；#14 是纯静态逆向 + 一次 ioctl；
#13 收益最大（直接坐实活体 handler）但有 hang 风险，放在有了 #11/#14 的寄存器/授权能力之后更安全；
#12 工作量最大，作为兜底。

若 #11–#14 全部判死，则产出 v2 封存文档：明确"sm120 上中断式 L3 的唯一已知可达路径是
外部调试器进程 + RS_ACCESS_DEBUG 授权"，并把 `RM_REVERSE_ZH.md` 作为可复用的 RM ABI 参考移交。

### 9. 方法论备忘（避免再走弯路）

1. **判"冻结"必须用带外观测**：`/dev/shm` + `cudaHostRegisterMapped` + 独立观察进程。
   同进程 `cudaMemcpy` 在挂起时会阻塞，读到的"平滑计数"是假象（v1 的 R2.1 就是这么错的）。
2. **`status == 0` 不等于生效**：本轮至少三处（同 client 的 0x83de 会话、`0x83de031d`、
   `0x20800122`）都是"返回成功但静默早退"。每个控制都要有独立的**副作用判据**。
3. **抓包器必须先自证**：`psize==0` 时 RM 仍解引用 `pAllocParms`；多进程要按 pid 分文件；
   escape 常量要对着 `RmIoctlsTable` 核。
4. **CUDA 惰性模块加载**：helper 内核首次启动会串行化在运行中内核之后，所有实验先 warmup。
5. **cuda-gdb 会把 GPU 锁到基频**（653 MHz vs 2527 MHz），做时延测量必须扣除。

---

## v0.4（2026-10-16）— **L3 中断闭环打通**：运行中内核可按指令冻结/恢复（外部调度进程驱动）

### 1. 先补上 R0 缺的那一格：`cuda-gdb -p` 能 attach 已在跑的进程

`run_gt2.sh`（新增）：先起 `shm_spin`，再 `cuda-gdb -batch -p <pid>`。
默认 `ptrace_scope=1` 会 EPERM，但目标进程只要调一次
`prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` 就能被 attach —— 而 **XSched 的运行时本来就在应用进程内**，
这一条完全在掌握之中（`probe/shm_spin.cu` 已加 `L3_ALLOW_PTRACE=1` 开关）。

结果（`evidence/live/gt2_gdb.log` / `gt2_watch.log`）：attach 后 cuda-gdb 直接报
`Switching focus to CUDA kernel 0, grid 2, block (0,0,0) ... spin_publish<<<(8,1,1),(32,1,1)>>>`，
停驻的 8 s 内外部观察者计数**纹丝不动**，`detach` 后立即恢复 37 M/s。

### 2. 闭环：外部调度进程按指令中断/恢复运行中内核

新增 `probe/l3app.cu`（目标）+ `run_l3poc.sh`（可复现脚本）。拓扑：

```
l3app（目标，被 cuda-gdb 启动）        run_l3poc.sh（调度进程，完全独立）
  - spin 内核（8 block × 32 线程）        - 写 /dev/shm/l3poc_req：1=要求冻结，0=恢复
  - poller 线程：看到 req=1 就调用         - 读 /dev/shm/l3poc_state 确认 gdb 已停住
    l3_poll()（边沿触发，只调一次）        - 用独立进程 shm_watch 读 /dev/shm 里的内核计数器，
  - 内核计数器直接发布到 /dev/shm            不受 ptrace 停驻影响
cuda-gdb：break l3_poll → commands{shell 等待 req 变 0; continue}
         （停在 l3_poll 期间 inferior 被 ptrace 停住 ⇒ 设备冻结）
```

实测（`evidence/live/l3poc_gdb.log`、`l3poc_watch.log`）：

| 调度请求的停顿时长 | 外部观察者看到的实际冻结窗口 |
|---|---|
| 1.5 s | **1.45 s**（计数停在 99003392） |
| 2.0 s | **2.00 s**（计数停在 136117760） |
| 1.0 s | **1.00 s**（计数停在 173244928） |

gdb 侧三次完整打印 `[l3poc] FROZEN at l3_poll` → `[l3poc] RESUMING`；
冻结期间计数器 `delta == 0`，恢复后立刻回到 37 M/s。**这就是"运行中内核被中断并可恢复"的实锤**，
且中断时点在任意指令（由 poller 决定），不是 block 边界 —— 满足"非 L2 guardian"的要求。

### 3. 路上踩掉的两个坑（都会让实验假阴性）

1. **gcc 会删掉对空函数体的调用**：`__attribute__((noinline)) void l3_poll(void) {}`
   仍被优化掉，导致断点永远不触发、poller 打印"requesting suspend"后立刻"resumed"。
   修法：函数体放 `__asm__ __volatile__("" ::: "memory")`。
2. **cuda-gdb 13.2 不带 Python**（`Python scripting is not supported`），
   且 `break l3_poll if <host 全局变量>` 这种条件断点在**attach**场景下从未触发过；
   改成"目标侧边沿触发调用 + 无条件断点 + gdb `commands` 里的 `shell` 轮询"才稳定。
   另外 **attach 模式下即使无条件断点也不生效**，必须用 `run`（由 cuda-gdb 启动目标）。

### 4. 与目标的差距（还没有完全等于 sm70/86 的 L3 trap）

| 维度 | 现状 | 差距 |
|---|---|---|
| 中断能力 | ✅ 任意指令处冻结/恢复 | — |
| 粒度 | ✅ per-context（冻结期间显示/桌面进程不受影响） | — |
| 是否需要目标配合 | ⚠️ 需要在目标里放一个"轮询点"（`l3_poll`）并允许 ptrace | sm70/86 只需注入代码，不需要协作点 |
| 延迟 | ⚠️ 实测停驻延迟约 30 ms（cycle 2 里 2 s 内多跑掉 1.21M 次 ≈ 0.03 s） | sm70/86 是微秒级 |
| 依赖 | ⚠️ 每个目标一个 cuda-gdb 进程 | 需要封装成 XSched agent |
| 设备侧 trap | ❌ 仍未送进活体 plugin（R3 判死、R3'' 判死） | 保持为"下一阶段" |

**结论**：v1 的"sm120 无法实现运行中中断"**彻底不成立**；我们现在有了一个可复现、可外部控制、
按需冻结/恢复运行中内核的 L3 形态，代价是需要一个调试器 agent 进程和一个目标侧轮询点。
把它接进 XSched（`InterruptContext` 的第三个 backend：`CUDBG_GDB`）即可替代 sm120 上不可用的 trap 路径。

### 5. 追加：MI 路线（无目标协作）的现状与更正

`probe/l3mi.cpp` + `run_l3mi.sh`：用一个 C++ 进程 fork/exec `cuda-gdb --interpreter=mi2`，
通过管道发 `-target-attach / -exec-continue / -exec-interrupt / -target-detach`。
目标侧不需要任何轮询点，只要 `PR_SET_PTRACER`。

**更正 v0.4 早期草稿里的一处过度解读**：shell+FIFO 试验里观测到的 1.88 s 冻结窗口，
其时长与"attach 停驻 → 第一次 `-exec-continue`"的间隔几乎相同，因此它**是 attach 停驻**造成的，
不能算作 `-exec-interrupt` 生效的证据。

C++ 驱动器实测（`evidence/live/l3mi_agent.log`）：

| 命令 | 结果 |
|---|---|
| `-target-attach` | ✅ 0.013–0.025 s 返回并停住（设备立即冻结，gt2 已独立验证） |
| `-target-detach` | ✅ 0.10–0.12 s 返回，设备恢复 |
| `-exec-continue` | 第一次 ✅，之后 `^running` 不再出现（超时） |
| `-exec-interrupt` | 只在第一次之后 ~19 s 才等到 `*stopped`，且与内核自然结束时间吻合 ⇒ **未见其独立生效** |

⇒ **MI 路线的可用原语目前是"attach=冻结 / detach=恢复"**（每次约 0.1 s，可反复），
`exec-continue/interrupt` 的 MI 状态机还需要专门调理（cuda-gdb 的 MI 实现在 attach 之后状态跟踪不完整）。
`run_l3mi.sh` 的三轮 attach/detach 只观测到 1 个冻结窗口，说明这条路径还不够稳，**暂不作为交付形态**。

**当前可交付的 L3 中断形态仍是 `run_l3poc.sh`（§2）**：断点 + gdb `commands`/`shell` 轮询，
三个冻结窗口与请求的 1.5/2.0/1.0 s 精确吻合，证据链完整。

---

## v0.5（2026-10-16）— L3 中断原语定型：`attach=冻结 / detach=恢复`（无需目标协作、无断点），延迟 116 ms

### 1. 设计定型

`probe/l3mi.cpp` 最终形态：**每次干预用一个全新的 cuda-gdb MI 进程**。

```
spawn cuda-gdb --interpreter=mi2
  -target-attach <pid>   → inferior 被 ptrace 停住 ⇒ 设备冻结
  (保持 hold_ms)
  -target-detach         → 设备恢复
  退出 gdb
```

关键发现：**同一个 gdb 会话里 `detach` 之后再 `attach` 不会再次冻结设备**
（命令都返回 ok，但 `-target-attach` 只用 0.013 s，CUDA 侧的 suspend 没重做）。
改成"每次干预新起一个 gdb"之后完全稳定。

### 2. 实测（`evidence/live/l3mi_watch.log`，20 ms 采样，外部进程）

| | |
|---|---|
| 请求冻结 → 设备真正静止 | **116 ms** |
| `-target-attach` 返回 | 24–28 ms |
| `-target-detach` 返回 | 114–116 ms |
| 三轮冻结窗口（请求 1500 ms） | **1.40 / 1.42 / 1.40 s**（t≈1.3–2.7 / 4.5–5.9 / 7.6–9.0 s） |
| 目标侧需要做什么 | 只要 `prctl(PR_SET_PTRACER, ANY)`；**不需要任何断点、不需要轮询点** |

精确时间戳（`/tmp/lat_watch.txt` 与 `l3mi` 输出，两者都用 CLOCK_MONOTONIC）：

```
t=2319.3666  attach requested
t=2319.3910  attach 返回
t=2319.4831  外部观察者首次看到 delta==0（counter 钉在 96534016）   ⇒ 116 ms
t=2320.7056  detach 返回（恢复）
```

### 3. 现在手里有三个可用档位

| 档位 | 目标协作 | 延迟 | 可重复性 | 证据 |
|---|---|---|---|---|
| **A. attach/detach（本轮定型，推荐）** | 只需 `PR_SET_PTRACER` | **116 ms** | ✅ 三轮稳定 | `run_l3mi.sh` / `l3mi_watch.log` |
| B. 断点 + gdb `commands` 轮询（v0.4） | 需要一个轮询点 + 由 gdb 启动目标 | ~30 ms | ✅ 三轮稳定 | `run_l3poc.sh` |
| C. MI `exec-interrupt` | 否 | 未跑通 | ❌ MI 状态机待调理 | `l3mi_agent.log`（v0.4 §5） |

**推荐把 A 作为 XSched 的 L3 backend**：无需改目标代码语义，只需 runtime 里一行 `prctl`；
中断点任意（由 attach 时刻决定）；per-context（冻结期间桌面/其他进程不受影响）。

### 4. 关键局限：**冻结 ≠ 资源让渡**（本轮最重要的负面结论）

为了让 L3 对 XSched 真正有用，必须回答："把 A 进程冻住之后，B 能不能拿到 GPU？"
为此写了 `run_l3e2e.sh`（A/B 两个独立进程各 60 block × 1024 线程抢 GPU），三轮测量：

| 场景 | B 的墙钟时间 |
|---|---|
| B 单独跑 | 7.56 s |
| B + A 同时跑（无干预） | 7.72 s |
| B + A，且 A 在 B 启动后被冻结 12 s | 7.94 s |

**三者几乎相同** ⇒ 冻结 A 完全没有给 B 带来加速。另一次 `nvidia-smi` 采样显示，
冻结期间 `utilization.gpu` 仍是 100%、SM 时钟不变 ⇒ **被冻结的 block 仍然占着 SM 槽位，
只是不再发射指令**。

这一点与 sm70/sm86 的 L3 有本质区别：那里的 trap handler 做的是 **kill block（让 block 退出、
释放 SM 资源）**，然后从 `entry_point_resume` 重放；而我们目前能做到的是 **suspend warp（冻结，
不释放资源）**。

⇒ **当前拿到的是"可中断/可恢复"，还不是"可抢占/可让渡"。**
后者需要 block 驱逐（CILP context switch）或 trap handler 里的 block kill，
即回到 R1（patch 活体 plugin）——而 R1 仍卡在"把 trap 送进运行中 warp"。

补充说明：`L3_MODE=fma` 的 compute-bound 变体（新增在 `shm_spin.cu`）单跑就要 34 s，
说明它把 GPU 压到降频，用它做抢占实验成本过高，暂不作为常规手段。

### 5. 追加：debugger 驱动的 "block kill" 不可行

想用调试器直接把运行中 warp 的 PC 改到内核的 EXIT 指令上（等价于 sm70/86 的 kill block），
实测被 cuda-gdb 拒绝：

```
(cuda-gdb) cuda thread (0,0,0)
(cuda-gdb) set $pc = $pc
cuda/cuda-tdep.c:3069: internal-error: cuda_value_to_register:
    Assertion `!cuda_pc_regnum_p (gdbarch, regnum)' failed.
```

`info cuda threads` 能读（显示 `spin_publish+320`、block/thread 坐标），但 **PC 只读**。
⇒ 用调试器实现 block 驱逐/杀死这条路走不通。

### 6. 本阶段收束：能级与缺口

| 能力 | 状态 | 证据 |
|---|---|---|
| 运行中内核按指令冻结 | ✅ 116 ms | `run_l3mi.sh`（三轮 1.40/1.42/1.40 s） |
| 按指令恢复 | ✅ ~115 ms | 同上 |
| per-context（不影响桌面） | ✅ | gt2/l3poc/r3 期间桌面进程始终存活 |
| 无需目标协作 | ✅ 只需 `prctl(PR_SET_PTRACER, ANY)` | `run_l3mi.sh` |
| **让出 GPU 资源给其它内核** | ❌ | `run_l3e2e.sh`：冻结 A 后 B 无加速（7.94 vs 7.72 s） |
| 设备侧 trap（sm70/86 语义） | ❌ | R1 阻塞；R3/R3'' 判死；PC 改写被拒 |
| 微秒级延迟 | ❌ | 实测 116 ms，差 4–5 个数量级 |

**结论**：v1 的"sm120 无法运行中中断"已被推翻并替换为精确表述 ——
**sm120 上"冻结/恢复运行中内核"可行（116 ms，外部进程驱动，per-context）；
"抢占/资源让渡"仍不可行**，因为它需要 block 驱逐，而 block 驱逐的三条可能路径
（RM 调试器会话 / CILP 抢占 / 调试器改 PC）目前分别被 RS_ACCESS_DEBUG、未验证、cuda-gdb 只读 PC 挡住。

下一步优先级：
1. **验证硬件 CILP 抢占**（唯一还没试过的让渡路径）：跨进程高优先级流是否能让硬件驱逐低优先级内核的 block。
2. 把已实现的"冻结/恢复"接进 XSched 作为 `XSCHED_SM120_L3_BACKEND=GDB`（task #15）——
   它至少能给 XSched 提供一个"可信的中断点"，配合 L2 注入的密集检查点，可把有效抢占延迟压到检查点粒度。
3. 回到 #11（`pGpu+0x911`）争取设备侧 trap。

---

## v0.6（2026-10-16）— 闭环验收：**运行中内核被中断后结果 bit-exact**

### 1. 测试设计（`probe/l3work.cu` + `run_l3work.sh`）

内核跑**固定迭代次数**并累加校验和（`sum(0..n-1) = n(n-1)/2`），因此期望值完全确定，
与何时被打断无关。流程：启动 → 1 s 后冻结 → 保持 2 s → 恢复 → 等完成 → 比对校验和。

### 2. 结果

```
[l3mi t=3326.0690] cycle 1: FREEZE (attach 0.018 s) — holding 2000 ms
[l3mi t=3328.1833] cycle 1: RESUME (detach 0.114 s)

[l3work] done: cudaSuccess  result=1124999999250000000
                            expected=1124999999250000000  CORRECT
exit code (0 = checksum correct) = 0
```

外部观察者（5 ms 采样）同时记录了进度冻结窗口：

```
  progress frozen ~1.90 s at 312999936      ← 与请求的 2000 ms 吻合
  samples=2361  final progress=1499987968
```

⇒ **运行中的内核在飞行中被外部进程停住 1.90 s，恢复后跑完，结果与从未被打断时逐位相同。**
这是本阶段"运行中内核被中断并可恢复"的最终实锤，带正确性保证。

### 3. 本阶段最终交付清单

| 交付物 | 路径 | 状态 |
|---|---|---|
| L3 中断原语（attach=冻结 / detach=恢复，无目标协作） | `probe/l3mi.cpp` + `run_l3mi.sh` | ✅ 三轮可重复 + 正确性验证 |
| 中断正确性测试 | `probe/l3work.cu` + `run_l3work.sh` | ✅ bit-exact |
| 断点式 L3（延迟更低，需轮询点） | `probe/l3app.cu` + `run_l3poc.sh` | ✅ 三轮冻结窗口精确 |
| ground truth 复测 | `run_r0.sh` / `run_gt2.sh` | ✅ 推翻 v1 判死 |
| RM/调试栈 ABI 工具书 | `RM_REVERSE_ZH.md` | ✅ 402 行 |
| 滚动实验记录 | `FINDINGS_ZH.md` | ✅ v0.1–v0.6 |
| v1 封存文档的结论修订 | `test/sm120_l3_probe/ARCHIVE_ZH.md` 附录 | ✅ 已追加 |

### 4. 尚未达成的部分（下一阶段）

1. **资源让渡**（冻结不释放 SM 槽位）——唯一没试过的路径是**硬件 CILP 抢占**
   （跨进程高优先级流能否触发对低优先级内核的 block 驱逐）。
2. **设备侧 trap**——`pGpu+0x911` 闸门（task #11）。
3. **接入 XSched**——把 `l3mi` 包装成 `InterruptContext` 的 backend（task #15）。

---

## v0.7（2026-10-16）— **重要更正：冻结确实会让渡 GPU 资源**（v0.5 §4 的负面结论作废）

### 1. v0.5 §4 的结论错在哪里

v0.5 §4 断言"冻结 A 后 B 无加速（7.94 vs 7.72 s）"。**那个实验的配置根本不产生竞争**：
当时 A/B 各用 60 block × 1024 线程的 `clock64` 自旋内核，实测 **B 单独跑就要 7.56 s**，
和 B+A 的 7.72 s 几乎一样 —— 说明两个内核可以完全并存，本来就没有抢资源，测不出任何东西。

### 2. 正确的竞争配置

关键是把占用拉满：**A = 20000 block × 256 线程**的自旋内核（远超 GPU 容量，block 排队），
**B = 1 block × 32 线程**的瞬时内核。先确认竞争真实存在：

| 场景 | B 的完成时间 |
|---|---|
| B 单独跑 | 0.36 s |
| B + A（A 独占 GPU，无干预） | **10.5 / 33.65 s**（被 A 挡在队列里） |

### 3. 决定性结果（`tools/handover_test.sh`）

| 场景 | B 的完成时间 |
|---|---|
| **不冻结 A** | **33.65 s** |
| **把 A 冻结 4 s**（`l3mi --hold-ms 4000`） | **0.74 s** |

冻结发生在 B 启动后约 0.7 s（attach 0.022 s），保持到 B 完成之后才恢复
（`FREEZE t=3907.9690` → `RESUME t=3912.0883`，实际停驻 4.12 s）。

⇒ **把低优先级内核冻结后，高优先级内核立刻拿到整块 GPU，完成时间从 33.65 s 降到 0.74 s（45×）。**
**冻结 = 让渡**：被冻结的 warp 不再发射，硬件把 SM 资源重新分配给了 B。

### 4. 修正后的能力表

| 能力 | 状态 | 证据 |
|---|---|---|
| 运行中内核按指令冻结/恢复 | ✅ 116 ms 延迟 | `run_l3mi.sh`、`run_l3work.sh` |
| 中断后结果正确 | ✅ bit-exact | `l3work_app.log`：`result == expected` |
| **把 GPU 让给其它内核** | ✅ **33.65 s → 0.74 s** | `tools/handover_test.sh` |
| per-context | ✅ | 桌面进程全程存活 |
| 目标侧要求 | 仅 `prctl(PR_SET_PTRACER, ANY)` | — |
| 设备侧 trap（sm70/86 语义） | ❌ | R1 阻塞 |
| 延迟量级 | 116 ms（vs sm70/86 微秒级） | — |

**结论**：sm120 上已经具备**完整的中断式 L3 抢占语义**（中断 / 让渡 / 恢复 / 正确性），
唯一与 sm70/86 的差距是**延迟量级**（116 ms vs 微秒）和**依赖外部 cuda-gdb agent 进程**。
这已经满足"不是 L2 guardian、是运行中中断"的要求。

---

## v0.8（2026-10-16）— XSched 形状的 agent API（守护进程 + 控制块）

### 1. 实现

`probe/l3mi.cpp` 新增 `--serve <shm>` 守护模式，控制块（`/dev/shm/<name>`，24 B）：

```c
struct L3Ctrl {          // 调度进程映射它，agent 读写它
    volatile uint32_t cmd;    // 0=idle 1=suspend 2=resume 3=quit
    volatile uint32_t state;  // 0=running 1=suspended（agent 写）
    volatile uint32_t seq;    // 每次状态变化 +1
    volatile int32_t  last_status;
};
```

调度侧客户端 `tools/l3ctl.py <shm> suspend|resume|state|quit`（阻塞直到 ack）。
守护进程收到 `suspend` 时 fork/exec cuda-gdb、`-target-attach`，并把 gdb 保持在后台；
收到 `resume` 时 `-target-detach` 并退出 gdb。

### 2. 实测

```
[sched] state before: cmd=0 state=0 seq=0
[sched] suspend: suspend ack in 1.809 s
[l3agentd] SUSPEND ok=1
[mi] *stopped,frame={...,func="ioctl",...,stopped-threads="all"}   ← 设备已冻结
[sched] resume: resume ack in 0.332 s
[l3agentd] RESUME 0.031 s
```

⇒ **守护进程形态的 API 可用**：suspend/resume 双向 ack 正常，冻结确实发生
（`stopped-threads="all"`）。

### 3. 已知问题（下一轮要修的）

1. **目标处于 D 状态时 attach 很慢**：用 20000 block 的霸占内核时，目标进程常年停在
   `ioctl` 的不可中断睡眠里，`-target-attach` 可能耗时 ~10 s（观测到 10.121 s），
   偶尔直接报 `process is a zombie` / `ptrace: 不允许的操作`。
   更现实的目标形态（普通 CUDA 应用 + 中等规模内核）不受此影响（v0.7 的 0.74 s 就是证据）。
2. 守护进程目前只支持单目标；接入 XSched 时需要一目标一 agent（或 agent 池）。
3. `l3ctl.py state`/`seq` 已实现，但还没有"事件回调"——XSched 侧的 `Interrupt()` 需要
   阻塞到 ack（当前用轮询 `state`，延迟约 1 ms 量级）。

### 4. 接入 XSched 的具体落点（供下一轮直接做）

- **不要**改 `CudaQueueLv3Trap::Interrupt()`：它跑在**被冻结的那个进程里**，
  一旦冻住就再也返回不了。L3-GDB 的语义是**进程级**抢占，对应 XSched 的
  "调度器进程 + 被管进程"形态（计划里 R2-4b 的形态）。
- 落点：`platforms/cuda/hal/src/level3/` 新增 `gdb_interrupt.{h,cpp}`，
  暴露 `GdbInterruptContext::Suspend(pid)` / `Resume(pid)`；
  在 `platforms/cuda/hal/src/level3/cuda_queue.cpp` 之外由 XSched 的 scheduler 侧调用。
- 目标进程侧只需一行 `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)`，
  可放在 `platforms/cuda/hal/src/common/` 的初始化路径里，由 env `XSCHED_SM120_L3_BACKEND=GDB` 打开。

---

## v0.9（2026-10-16）— **接入 XSched 完成**：L3 GDB backend 进 HAL，端到端 66×

### 1. 新增到 XSched 源码树的两个文件

| 文件 | 内容 |
|---|---|
| `platforms/cuda/hal/include/xsched/cuda/hal/level3/gdb_interrupt.h` | `GdbInterruptContext`：`Suspend(pid)` / `Resume(pid)` / `GetState(pid)` / `AllowDebuggerAttach()` / `IsAvailable()` |
| `platforms/cuda/hal/src/level3/gdb_interrupt.cpp` | 内部 cuda-gdb MI 客户端（spawn / `-target-attach` / `-target-detach` / kill），一目标一 agent |

`add_hal_lib` 用的是 `GLOB_RECURSE hal/src/*.cpp`，所以新文件自动进 `libhalcuda.so`；
实测 `cmake --build . --target halcuda` 通过：

```
[ 95%] Building CXX object .../hal/src/level3/gdb_interrupt.cpp.o
[100%] Linking CXX shared library libhalcuda.so
[100%] Built target halcuda
```

### 2. 端到端验证（`probe/l3hal.cpp` + `run_l3hal.sh`，链接真实的 `libhalcuda.so`）

场景：hog 进程（20000 block）霸占 GPU，高优先级小内核到达，调度侧调用 HAL API。

| 场景 | 高优先级内核完成时间 |
|---|---|
| hog 正常运行 | **62.01 s** |
| `GdbInterruptContext::Suspend(hog)` | **0.93 s** |
| `Resume(hog)` | 返回 1，日志 `[XSCHED INFO] gdb interrupt: pid 32502 resumed` |

⇒ 通过 XSched 自己的 HAL API 拿到 **66× 的抢占收益**，而且 hog 被挂起后恢复正常。

### 3. `arch.cpp` 的更正（重要）

sm120 分支原来写着"trap path unavailable … **no user-space mechanism can stop a running
kernel**; use the TSG timeslice path instead" —— 这句来自 v1 的判死结论，**已被推翻**。
现在改成：

- 说明 trap 路径仍不可用（机制原因：RM 触发不送 trap + 0x83de 会话缺 `RS_ACCESS_DEBUG`）；
- 明确"运行中内核无法从用户态停止"是**错的**，并指向 116 ms 冻结 + 让渡的证据；
- 新增 opt-in：设 `XSCHED_SM120_L3_BACKEND=GDB` 时，`CudaQueueCreate` 会调用
  `GdbInterruptContext::AllowDebuggerAttach()`（`prctl(PR_SET_PTRACER, ANY)`）
  让外部 agent 可 attach；**默认不打开**，不影响现有 TSG 路径。

### 4. 使用方式

```bash
# 目标进程侧（XSched runtime 内）
XSCHED_SM120_L3_BACKEND=GDB ./my_cuda_app

# 调度侧（另一个进程）
GdbInterruptContext::Instance().Suspend(target_pid);   // 冻结，116 ms
... 让高优先级工作跑 ...
GdbInterruptContext::Instance().Resume(target_pid);    // 恢复
```

限制（已在头文件注释里写明）：这是**进程级**原语，会连同目标的调度线程一起停住，
所以必须由**外部**调度进程驱动，不能在被抢占的进程内部调用。

---

## v1.0（2026-10-16）— 用户可用形态：`l3ctl` 命令行工具

### 1. 新增 `platforms/cuda/test/main/l3ctl.cpp`

```
l3ctl check              # 后端是否可用（cuda-gdb 在不在 PATH）
l3ctl state   <pid>      # running / suspended
l3ctl suspend <pid>      # 冻结目标，并**驻留**在后台持有这次 attach
l3ctl resume  <pid>      # 让持有者 detach，目标恢复
```

设计要点：`suspend` 必须驻留（attach 活在该进程里），agent 的 pid 记在
`/tmp/xsched-l3-<pid>.agent`，供其它动词查找；`resume` 通过 SIGTERM 让持有者优雅 detach
（直接 SIGKILL 会留下 stopped 的 inferior）。持有者收到 SIGINT/SIGTERM 也会自己 detach 再退出。

### 2. 实测（跨进程控制）

```
state:  pid 33389: running
suspend: suspend(33389) = ok; holding. Ctrl-C (or `l3ctl resume 33389`) to release.
state:  pid 33389: suspended          ← 另一个进程看到的
resume: asking agent 33401 to resume pid 33389
        resumed(33389) = ok
state:  pid 33389: running
target: 恢复后继续跑，rate 38.3 M/s
```

⇒ 目标被冻结、被外部观察到、被恢复，全程无需目标侧配合（只需 prctl 一行）。

### 3. 本阶段的完整交付矩阵

| 层次 | 交付物 | 验证 |
|---|---|---|
| 原语 | 116 ms 冻结 / ~115 ms 恢复，per-context | `run_l3mi.sh` |
| 正确性 | 中断后结果 bit-exact | `run_l3work.sh` |
| 让渡 | 33.65 s → 0.74 s（45×） | `tools/handover_test.sh` |
| 库 | `GdbInterruptContext`（HAL，进 `libhalcuda.so`） | `cmake --build . --target halcuda` |
| HAL 端到端 | 62.01 s → 0.93 s（66×） | `run_l3hal.sh` |
| 工具 | `l3ctl` CLI（check/state/suspend/resume） | 上面 §2 |
| 路由 | `arch.cpp` sm120 注释更正 + `XSCHED_SM120_L3_BACKEND=GDB` opt-in | 编译通过 |
| 文档 | `FINDINGS_ZH.md`(v1.0) / `RM_REVERSE_ZH.md` / `ARCHIVE_ZH.md` 附录 | — |

### 4. 仍未达成

1. **设备侧 trap**（sm70/86 的 trap handler 注入语义）——`pGpu+0x911` 闸门（task #11）。
   注意：这已**不是** L3 抢占的必要条件，只是"更接近原版语义"的加分项。
2. 接进 XSched 的 service/scheduler（CLI `xcli` 层）——目前 `l3ctl` 已经是最小可用形态。
3. 延迟 116 ms（原验收线 50 ms 未达）。

---

## v1.1（2026-10-16）— task #11 判决：`pGpu+0x911` 是**启动期**能力位，运行期无法打开

### 1. 取证（全 `.text` 扫描，第 4 版的 nv-kernel.o_binary）

`objdump -d nv-kernel.o_binary | grep '0x911('` 的全部结果：

| 地址 | 指令 | 所在函数 |
|---|---|---|
| 0x17376 | `movb $0x1,0x911(%rdi)` | `__nvoc_init_dataField_OBJGPU+0x546` |
| 0x17b4c | `movb $0x0,0x911(%rdi)` | `__nvoc_init_dataField_OBJGPU+0xd1c` |

**全二进制只有这两处写 `0x911`，都在 `__nvoc_init_dataField_OBJGPU` 里** —— 也就是
OBJGPU 对象的 **构造/数据字段初始化**（驱动加载或 GPU attach 时执行一次）。
没有任何运行期的写入点。

对应的读点（`cmpb $0x0,0x911(%r12)`）就在我们之前找到的两条 reg-op 通路上：
`ksmdbgssnCtrlCmdDebugExecRegOps_IMPL@0x21a990` 和
`subdeviceCtrlCmdGpuExecRegOps_cmn@0x2dcfc0`。

### 2. 判定

```
0x17354  mov $0x2,%esi
0x17359  shr %cl,%rsi          ← 对某个 64 位能力/注册表值做位测试
0x1735c  test %rsi,%rsi
0x1735f  je  0x17082           ← 不满足 → 走不置位分支
0x17365  movb $0x0,0x910(%rdi)
0x1736c  cmp $0x3,%rax
0x17370  je  0x17b38
0x17376  movb $0x1,0x911(%rdi) ← 只有这条路径会打开闸门
```

`0x911` 由 **GPU 初始化时读到的能力位/注册表配置**决定，构造完就固定。
RM 侧没有任何控制命令可以改它 ⇒ **reg-op 通路（`0x83de031d` / `0x20800122`）在本机
运行期不可用**；要打开只能改模块参数/注册表并重新初始化 GPU（`rmmod` nvidia），
这超出本项目的安全边界（GPU 同时驱动显示、不 rmmod、不改驱动）。

### 3. 对目标的影响与结论

- **设备侧 trap（sm70/86 语义）判定为不可达**（在"不重载驱动"的前提下）：
  - trap 投递：`cuXtraTriggerTrap`(0x419e84 bit31) 无效、`brkpt` 杀上下文、命令字不投递；
  - debugger 会话：缺 `RS_ACCESS_DEBUG`（`os_check_access` 永不授予）；
  - reg-op 直写：被启动期闸门 `pGpu+0x911` 挡住；
  - 调试器改 PC 杀 block：cuda-gdb 拒写 CUDA PC。
- **但这不影响 L3 抢占的可用性**：冻结式 L3 已经提供中断/让渡/恢复/正确性
  （v0.7–v1.0），并且已接进 XSched（`GdbInterruptContext` + `l3ctl`）。
- 因此 task #11 结案：**闸门定位完成、判定为启动期固定**，不再投入。

至此四条路线全部有了明确结论：
R1（活体 plugin + trap 投递）—— 投递侧不可达；
R2（CUDBGAPI）—— 不需要（冻结式 L3 已可用）；
R3（复刻 RM 会话）—— 缺 RS_ACCESS_DEBUG，判死；
R3'（reg-op 闸门）—— 启动期固定，判死。
**净结果：sm120 上以"外部调试器 agent + 冻结/恢复"实现了可用的 L3 抢占。**

### 5. 稳定性复测（`tools/stability_test.sh`，交付形态连跑 3 次）

```
--- run 1 ---  Suspend(hog) = 1 (0.538 s)   tenant finished after 0.74 s   Resume = 1
--- run 2 ---  Suspend(hog) = 1 (0.537 s)   tenant finished after 0.73 s   Resume = 1
--- run 3 ---  Suspend(hog) = 1 (0.535 s)   tenant finished after 0.73 s   Resume = 1
```

三轮完全一致（抖动 < 3 ms），租户每次都在 0.73–0.74 s 完成。

延迟口径说明：
- **设备真正静止** = 请求后 **116 ms**（`/tmp/lat_watch.txt` 5 ms 采样测得的硬数字）；
- 这里打印的 **0.53 s** 是"从高优先级内核启动到 `Suspend()` 返回"的调度侧墙钟，
  含 fork/exec cuda-gdb(~0.3 s) + attach(~0.02 s) + 设备静止；
- 若把 gdb 进程做成常驻预热（daemon 形态，`l3mi --serve`），可省掉前两项 ⇒ 调度侧路径
  接近 116 ms 的下限。

### 6. 预热（Prepare）与延迟口径的最终说明

新增 `GdbInterruptContext::Prepare(pid)`：提前 fork/exec cuda-gdb 并等到 `(gdb)` 提示符
（**不 attach**），使真正抢占时只付 attach 的代价。`Suspend()` 会自动复用已预热的 agent。
`l3hal --suspend` 现在先 `Prepare` 再计时，连跑 3 次：Suspend 返回 0.523/0.525/0.526 s。

**注意口径**：这个数字里含测试脚本自己的 `sleep 0.5 s`（等 B 启动），
所以真实 attach 开销只有 **~23 ms**；之后是设备静止的 116 ms（v0.5 用 5 ms 采样单独测过）。
即：**调度侧从"发出抢占"到"设备真的不动了" ≈ 116 ms，与是否预热无关**
（116 ms 是驱动/固件做 CILP save 的物理时间），预热省掉的是 gdb 进程启动那 ~0.3 s。

### 7. 交付状态定稿

- 代码（进 XSched 源码树、可编译、已验证）：
  `platforms/cuda/hal/{include,src}/.../level3/gdb_interrupt.{h,cpp}`、
  `platforms/cuda/test/main/l3ctl.cpp`、`platforms/cuda/hal/src/arch/arch.cpp`（注释更正 + opt-in）。
- 验证脚本（`test/sm120_l3_trap_v2/`）：`run_l3mi.sh`、`run_l3work.sh`、`run_l3poc.sh`、
  `run_l3hal.sh`、`run_l3handover.sh`、`tools/handover_test.sh`、`tools/stability_test.sh`、`tools/l3ctl.py`。
- 文档：`FINDINGS_ZH.md`（v0.1–v1.1）、`RM_REVERSE_ZH.md`、`ARCHIVE_ZH.md` 附录、`PLAN_ZH`（原计划）。

---

## v1.2（2026-10-16）— 接入 XSched **控制面**：`xserver` + `xcli l3`

### 1. 改动（4 处，全部在 XSched 源码树内）

| 文件 | 改动 |
|---|---|
| `service/common/{include/client.h,src/client.cpp}` | 新增 `SetProcessPreempt(pid, action, &state)` → `POST /l3` |
| `service/server/{include/server.h,src/server.cpp}` | 新增路由 `POST /l3` 与 `Server::PostLevel3`，调用 `xsched::cuda::GdbInterruptContext`；未编 CUDA 平台时返回 501 |
| `service/cli/{include/cli.h,src/cli.cpp,src/main.cpp}` | 新增子命令 `xcli l3 -x <pid> [-q\|-s\|-r]` |
| `CMakeLists.txt` / `service/CMakeLists.txt` | `platforms` 提前到 `service` 之前配置；`xserver` 在 `TARGET halcuda` 存在时链接它并定义 `XSCHED_L3_GDB_BACKEND` |

`action` 语义：0=查询、1=suspend、2=resume。

### 2. 实测（`xcli` → `xserver` → `GdbInterruptContext` → cuda-gdb → 目标进程）

```
$ xcli l3 -x 36268 -q      →  level-3: pid 36268 state=running
$ xcli l3 -x 36268 -s      →  level-3: pid 36268 state=suspended
$ xcli l3 -x 36268 -q      →  level-3: pid 36268 state=suspended
$ xcli l3 -x 36268 -r      →  level-3: pid 36268 state=running
$ xcli l3 -x 36268 -q      →  level-3: pid 36268 state=running
```

整条链路可用，且 `xserver` 与 `xcli` 都是**上游 XSched 自己的组件**（不是本项目新造的工具）。
默认配置（不带 `-DPLATFORM_CUDA=ON`）下 `/l3` 返回 501，不影响原有功能。

### 3. 现在 L3 抢占的完整调用路径

```
调度器/管理员
   └─ xcli l3 -x <pid> -s
        └─ POST /l3 {pid, action}
             └─ xserver: PostLevel3
                  └─ GdbInterruptContext::Suspend(pid)      (libhalcuda.so)
                       └─ fork/exec cuda-gdb --interpreter=mi2
                            └─ -target-attach <pid>   ⇒ 设备 116 ms 内静止
目标恢复：xcli l3 -x <pid> -r → … → -target-detach
```

目标侧唯一要求：`XSCHED_SM120_L3_BACKEND=GDB`（即 arch.cpp 里的 `prctl(PR_SET_PTRACER, ANY)`）。

### 4. 剩余

1. 延迟 116 ms（物理下限，非实现问题）。若可接受"目标侧有一个轮询点"，v0.4 的断点式后端可到 ~30 ms。
2. 设备侧 trap（sm70/86 语义）已判定不可达（v1.1）。
3. 目标进程目前还需 `L3_ALLOW_PTRACE=1` 或 `XSCHED_SM120_L3_BACKEND=GDB` 才接受 attach ——
   前者是测试探针的开关，正式路径是后者（由 XSched runtime 在 `CudaQueueCreate` 里执行）。

### 5. 真实 XSched 应用的目标侧路径已验证

用 XSched 自己的测试程序（`platforms/cuda/test/main/level.cu`，走 shim）验证"目标侧只需一个环境变量"：

```bash
XSCHED_SM120_L3_BACKEND=GDB LD_PRELOAD=/tmp/xsched_shim/libcuda.so.1 level 3
```

目标日志出现：

```
[XSCHED INFO] level-3 GDB backend enabled: this process accepts a debugger attach
              @ .../platforms/cuda/hal/src/arch/arch.cpp:33
```

然后在**没有任何 `L3_ALLOW_PTRACE` 之类的探针开关**的情况下：

```
$ l3ctl suspend 37683      → (驻留持有)
$ l3ctl state 37683        → pid 37683: suspended     ← 外部进程观察
$ l3ctl resume  37683      → asking agent 37703 to resume pid 37683
$ l3ctl state 37683        → pid 37683: running
```

⇒ **XSched 管理的真实应用开一个环境变量就能被 L3 抢占**，链路
`arch.cpp:MaybeAllowGdbBackend → prctl → 外部 cuda-gdb → 冻结/恢复` 全通。

构建配方（供复现）：CUDA 需要 gcc ≤ 14，本机用 g++-13 作为 CUDA host compiler：

```bash
cmake -S . -B build -DPLATFORM_CUDA=ON -DBUILD_SERVICE=ON -DBUILD_TEST=ON \
      -DCMAKE_CUDA_COMPILER=/home/wy/cuda-12.9/bin/nvcc \
      -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-13 \
      -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_C_COMPILER=gcc-13 \
      -DCUDAToolkit_ROOT=/home/wy/cuda-12.9
```

---

## v1.3（2026-10-16）— 延迟剖析：106 ms 花在哪、能否优化

工具：`probe/latency.cpp` + `tools/measure_latency.sh`（1 ms 采样、CLOCK_MONOTONIC 统一时钟域）。

### 1. 延迟分解（8 block × 32 线程，预热后，n=3）

| 组成 | 时间 | 说明 |
|---|---|---|
| gdb 进程启动 + 初始化 | ~300 ms | **可被 `Prepare()` 预热消除** |
| MI `-target-attach` 往返 + ptrace stop | **22.5–24.2 ms** | 与负载无关 |
| 设备挂起（attach ack → 计数器静止） | **79.7–82.8 ms** | 与负载几乎无关 |
| **合计（预热后）** | **103.2–106.5 ms** | |
| 恢复：`detach` → 设备恢复 | **7.8–10.4 ms** | 比暂停快 10× |
| 恢复：`-exec-continue` → 设备恢复 | **0.9–3.5 ms** | 更快 |

**暂停 82 ms vs 恢复 8 ms 的极端不对称** ⇒ 这 82 ms 专门花在"保存上下文"路径上，不是通用 RPC 往返。

### 2. 与任务规模的关系（问题三的答案）

| 负载 | request→frozen | 相对 1 warp |
|---|---|---|
| 1 block × 32（1 warp） | 105.2 / 106.5 ms | 基准 |
| 8 block × 32（8 warps） | 105.0 / 106.4 ms | +0% |
| 256 × 256（65536 线程，占满 GPU） | 132.4 / 144.2 ms | **+30%** |
| 20000 × 256（超额订阅） | 133.2 / 141.8 ms | +30%（**已饱和**） |
| 16 条流（16 channels） | 104.5 / 105.6 ms | +0% |

⇒ **延迟与"任务总时长"无关**（与论文 Lv3"独立于 T"的性质一致），只在"驻留 warp 数量"上有一个 **+30% 的上限**（1 warp → 满 GPU），且与通道/流数量**无关**。**82 ms 主要是固定开销。**

### 3. 会话复用实验（`tools/reuse_session_test.sh`）——结论：不可行

| 命令 | 结果 |
|---|---|
| `-exec-continue` | 设备 **0.9–3.5 ms 内恢复运行** ✅ |
| `-exec-interrupt` | **从不冻结设备**（反复 3 次均未冻） ❌ |
| 同一 gdb 会话二次 `-target-attach` | 不重新冻结（v0.5 已测） ❌ |

⇒ 设备挂起被绑定在**调试器的附着关系**上，而不是 inferior 的 run/stop 状态上。
**每次抢占都必须走完整的 detach + 新 attach**，无法用"保持会话、反复 interrupt"摊销。

### 4. 未完成的实验（下次可做）

- **strace 分解 82 ms**：想区分"驱动 ioctl 内部耗时"与"cuda-gdb 用户态 sleep"。
  尝试 `strace -f -T -e trace=ioctl cuda-gdb --batch -ex "attach PID"` 时 cuda-gdb
  批处理模式阻塞（同时它自身被 ptrace 后 CUDA attach 行为不可靠），两次尝试均失败。
  替代方案：用 `-T` 抓 `librt`/`nanosleep`，或在 cuda-gdb 里 `set debug infrun on` 看时间戳。
- 断点式后端（`run_l3poc.sh`）的中断延迟从未精确测量过（v0.4 只是从时序推断 ~30 ms），
  若它真的更低，是一个可选的"目标侧需配合"的低延迟档位。

---

## v1.4（2026-10-25）— **RS_ACCESS_DEBUG 闸门打通**：用 `0xd04` 把 target client 共享给 agent

### 1. 之前的判断错在哪

v0.3/v1.1 认为"DEBUG 权利只能来自资源级 share policy，而 `NV_ESC_RM_SHARE`(0x35)
在 compute 对象上怎么发都不授予"。重新逐条对比 cuda-gdb 的黄金抓包
（`evidence/live/r3_ioctl.log.20138`）发现：**target 在创建任何 0xcec0 之前，向自己的
client 对象发过三条 `RM_CONTROL cmd=0xd04`**，其中第三条把 **agent client** 指定为共享对象：

```
t=0.014894 hClient=target hObject=target cmd=0xd04 params {target_client, 1, 0x0002}
t=0.0149xx hClient=target hObject=target cmd=0xd04 params {target_client, 4, 0x40002}
t=0.0149xx hClient=target hObject=target cmd=0xd04 params {agent_client,  1, 0x40003}
```

之前只盯着 `NV_ESC_RM_SHARE`（0x35，共享**资源**），漏掉了这条**共享 client 对象本身**
的 RM_CONTROL 通路。

### 2. 逆向结果（`nv-kernel.o_binary`，595.99.02）

`__nvoc_exported_method_def_RmClientResource @ 0xa64860`，表项 0x28 字节：

| cmd | IMPL | 地址 |
|---|---|---|
| 0xd01 | ClientGetAddrSpaceType | 0x334f50 |
| 0xd02 | ClientGetHandleInfo | 0x335260 |
| 0xd03 | ClientGetAccessRights | 0x335300 |
| **0xd04** | **ClientSetInheritedSharePolicy** | **0x335570** |
| 0xd05 | ClientGetChildHandle | 0x3355e0 |
| 0xd06 | ClientShareObject | 0x335390 |
| 0xd07 | ObjectsAreDuplicates | — |
| 0xd08 | ClientSubscribeToImexChannel | 0x3356d0 |

- `0xd04` 参数 12 B：`{target_client, rights_or_flags, type_flags}`。IMPL 把
  `{own_client, param0, param1, param2}` 组成 16 B 交给 `ClientShareObject`：
  **hObject = 调用者自己的 client 对象**，hClientTarget = param0，
  shareType = `param2 & 0xffff`，flags = `param2 >> 16`。
  第三条 `{agent, 1, 0x40003}` ⇒ 把自己的 client 共享给 agent，type=3。
- `0xd06` 参数 16 B：`{hObject, hClientTarget, ?, shareType(u16)@0xc, flags(u8)@0xe}`；
  shareType ≤ 7（`cliresCtrlCmdClientShareObject_IMPL@0x335390`）。
- client 的 share 回调 `cliresShareCallback_IMPL@0x32fad0` 里能读到断言字符串
  `pSharePolicy->type == RS_SHARE_TYPE_CLIENT`、`pSharePolicy->target == pClient->hClient`
  ⇒ **共享 client 对象本人才是 cuda-gdb 让 debugger client 取得其子对象访问权的机制**，
  子对象（0xcec0）通过祖先 client 上的 policy 继承。

### 3. 复刻与实测（`probe/l3x.cu`，`run_l3policy.sh`）

实现要点（全部在用户态、无需 cuda-gdb）：

1. 先起 agent：它跑一遍 libcuda 初始化，通过 ioctl 拦截拿到自己的 `client/device`，
   发布到 shm；
2. target 启动后等 agent client；**在自己的 device 对象创建时**（`hRoot` 即真实 client）
   对真实 client 发那三条 0xd04（策略必须早于 0xcec0 创建，子对象才继承）；
3. target 建 context/通道、发布**全部 8 个 0xcec0**，启动 spin 内核；
4. agent 用自己的 libcuda client 分配 `class 0x83de`，`hAppClient=target client`、
   `hClass3dObject=target 0xcec0`，再发 `031f/0307/032a=1`、`0317`。

结果（`evidence/live/exp_policy_grant_*.log`）：

| 观测 | 之前 | 现在 |
|---|---|---|
| 跨进程 0x83de 分配 | **0x1b INSUFFICIENT_PERMISSIONS** | **status 0（ACCEPTED）** |
| 三条 0xd04 | 未发 | 全部 status 0 |
| 绑定对象 | 任意 compute 都 0x1b | `{t_client, t_compute}` 直接 0 |

⇒ **RS_ACCESS_DEBUG 这一道闸门已经打通**，cuda-gdb 拿到 DEBUG 权利的真实动作就是
"target 自己把 client 对象 share 给 debugger client"，不是 `NV_ESC_RM_SHARE`。

### 4. 但挂起仍不是"干净 hold"：行为矩阵

| 配置 | `0317{0,0}`（attach 轮询停） | `0317{1,0}`（hold 挂起） | 结果 |
|---|---|---|---|
| 默认 target | 8 个对象全部 no-op（计数继续涨） | 某一个对象上计数停住 ~1 s，然后 `0318=0x6e`、target `cudaErrorLaunchFailure` | **破坏性**（`exp_policy_grant_*.log`） |
| target 置 `cudbgDebuggerInitialized/EnablePreemptionDebugging/EnableLaunchBlocking=1` | no-op | **完全 no-op** | 计数全程 37 M/s（`exp_policy_dbgflags_*.log`） |
| target 自己开线程用**自己的会话**发 0318 | no-op | no-op | 全轮 RESUMED，但从未真正冻住（`exp_policy_selfresume_*.log`） |

结论：
- 授权（谁能建会话）已解决；**剩下的差别在 target 侧"调试器注入"状态**。
  cuda-gdb 的 target 进程内加载了 `libcudadebugger`（`CUDA_INJECTION64_PATH`），
  它的 debug agent 负责把 hold 挂起变成可恢复；只靠外部 RM 会话时，
  hold 会让 target 的 context 被驱动判死（0x6e + launch failure）。
- `0317{1,0}` **确实能作用到正在跑内核的那个 0xcec0 上**（计数停住、上下文死亡），
  说明"从 RM 侧把 trap 送进运行中 warp"这一次是发生了的；只是缺少 clean resume。

### 5. 这给 trap 路径带来的新机会（下一轮直接做）

1. **把 `0317{1,0}` 当作 trap 源**：在 target 侧先把**活体 `.text.traphandler`**
   （arena base+0x15f0f00，v0.1 §4）的安全偏移（+0xa0）patch 成签名载荷，
   再让 agent 发 `0317{1,0}`。若签名落盘 ⇒ **活体 handler 确认执行**，
   这是 v1/EP-4 一直缺的直接证据。
2. 若 (1) 成立，把载荷换成"保存 + EXIT（kill block）"，即得到 **sm70/86 语义的
   块级抢占**：不需要 clean resume，后续按 XSched 的 checkpoint 重放。
3. 并行：复刻 target 侧注入（`CUDA_INJECTION64_PATH=libcudadebugger.so` +
   `cudbgApiInit(1)`），使 `0317{1,0}` 变成真正的 hold/resume，
   即可把 hold 挂起也做成 L3 原语。

### 6. 本轮产物

| 文件 | 内容 |
|---|---|
| `probe/l3x.cu` | 新增 `L3X_POLICY`（0xd04 三条 + 8 个 compute 扫描）、`L3X_TGTDBG`、`L3X_PRIME_ONLY`、target 自恢复线程 |
| `run_l3policy.sh` | 一键复现授权 + 挂起矩阵 |
| `evidence/live/exp_policy_grant_*` | 跨进程会话 0x1b→0、hold 破坏性 |
| `evidence/live/exp_policy_prime_*` | `0317{0,0}` 全 no-op |
| `evidence/live/exp_policy_selfresume_*` | target 自恢复线程结果 |
| `evidence/live/exp_policy_dbgflags_*` | target BSS 调试标志置 1 后 hold 变为 no-op |

---

## v1.5（2026-10-25）— **活体 trap handler 确认执行**：注入载荷跑通，context 存活

### 1. 结论（本阶段最重要）

用 v1.4 打通的 `0xd04` 授权 + `0x83de` 会话发 `0317{0,0}`，**真的把 trap 送进了运行中的 warp**：
target 侧 patch 后的活体 `.text.traphandler` 执行了我们注入的 SASS，向 host-mapped
内存写入签名 `0x5aa5` 与 trap reason，然后正常返回，**内核继续跑完、context 存活**。

证据：`evidence/live/exp_live_handler_sig_{target,agent}.log`

```
[tgt][handler] va=0x7792ff5f0f00 (base+0x15f0f00) memobj=1 fingerprint=MATCH
[tgt] patch handler=0x7792ff5f0f00 cave=0x7792ff5f5d00 sig_va=0x779516fa307c
[pol] compute[0]=0x5c00001b sess=0x5c00f00c modes 0/0/0 prime 0
[pol] trap-handler signature observed: 0x5aa5/0 reason=0x1 <<<
[target] victim finished: cudaSuccess counter=492627766
[target] trap-handler signature: 0x5aa5/0 reason=0x1 (LIVE HANDLER EXECUTED <<<)
[tgt] handler restored (verified)
```

- reason=`0x1` ⇒ `do_read_trap_reason` 返回 bit0（do_preempt_restore + do_pause 分支）；
- 签名与 reason 都由**注入的 cave** 写入 host shm（UVA），即使 context 之后死亡也能从外部读；
- `cudaSuccess` + 计数器跑满 15 s ⇒ handler 返回路径正确、没有破坏 context。

这是 v1/EP-4 一直缺的**直接证据**：不是"写入正确但没执行"，而是"活体 handler 确实执行了注入代码"。

### 2. 注入点与 cave 布局（`probe/l3x.cu`，`L3X_PATCH=1`）

- 活体入口：`arena base + 0x15f0f00`（slot20 推导，指纹 10 条全匹配）；
- 注入点：**+0x390 `MOV R3, RZ`**（紧跟 `CALL.ABS do_read_trap_reason` 的返回点，
  R8–R11 已在 +0x30 存栈，可安全当 scratch）；
- 用 `CALL.ABS` 跳到 slot20 对象 `+0xA00` 的 8 条零填充 cave（v1 已证明该区可写可执行）；
- cave（8 条，128 B）：
  1. `MOV R3,RZ`（被替换的原指令）
  2. `MOV R8, sig_lo`  3. `MOV R9, sig_hi`
  4. `HFMA2 R10, -RZ, RZ, 0, 212.625`（= 0x5aa5，见 §3）
  5. `ST.E [R8], R10`（签名）   6. `ST.E [R8+4], R2`（reason）
  7. `IADD R12, R12, 0x10`      8. `RET.ABS R12 0x20`
- LEPC 在 +0x370 把 R12 设为 0x370；`RET.ABS R12 0x20` 返回到 0x390。因此必须先把
  R12 加 0x10，返回到 0x3a0（否则回到 0x390 = CALL 自身，形成死循环，会冻死 context）。

### 3. sm_120 SASS 编码（全部用 `nvdisasm -b SM120a` 校验过）

| 指令 | 编码规律 |
|---|---|
| `MOV Rd, imm32` | `w0 = imm<<32 \| d<<16 \| 0x7802`；**寄存器在 byte2（<<16）**；w1=`0x003fe20000000f00` |
| 小常量 | `MOV32I` 对 16 位立即数不生效（实测写 0）；用 ptxas 的 `HFMA2 Rd,-RZ,RZ,0,212.625`：`w0=0x00005aa5ff0d7431`（d 在 byte2），w1=`0x000fca00000001ff` |
| `ST.E [Rb+off], Rs` | `w0 = off<<32 \| b<<24 \| 0x7385`；`w1 = 0x000fe20000100900 \| s`（s=RZ 用 0xff） |
| `LD.E Rd, [Rb+off]` | `w0 = off<<32 \| b<<24 \| d<<16 \| 0x7980`；w1=`0x000ea20000100900` |
| `ISETP.NE.AND P0,PT,Rs,RZ,PT` | `w0 = 0x000000ff \| Rs<<24 \| 0x720c`；w1=`0x004fda0003f05270` |
| `@!P0 BRA rel` | 前向：`off=((b+4)*4)`，b 在 w0 bits16..23；`w0=hi32(1)\|(b<<16)\|0x8947`（@P0 把 0x8000 清掉），w1=`0x003ff20003800000` |
| 后向 `BRA.U` | 后向用 int8 b：`off=((int8)b+4)*4`；直接复用驱动自己的 `-0x1a0` 编码 `0xfffffffd00947947 / 0x003ff2000383ffff` 最稳 |
| `RET.ABS R12 0x20` | `0x000000000c087950 / 0x003fc00003a00000` |
| `CALL.ABS T` | `enc=(T-0x100)>>8`；`w0=(enc&0xffffffff)<<32 \| (原 CALL 低32)`；`w1=(原 w1&~0xff)\|((enc>>32)&0xff)`。**T 必须 0x100 对齐** |

### 4. trap 源的行为矩阵（新）

| 会话操作 | 结果 |
|---|---|
| `0317{0,0}`（prime） | **送入 trap**：handler 执行（sig=0x5aa5, reason=0x1），若 handler 正常返回则内核继续、context 存活 |
| 同一会话再 `0318` | 返回 0，context 无异常 |
| handler 里 `EXIT`（kill） | 签名可见但 context 最终 `LaunchFailure`（trap 上下文里 EXIT 不是合法的 block-kill 路径） |
| 不 patch 的 dump（原驱动 handler） | prime 通常 no-op（reason=0 直接走完 epilogue） |

### 5. 进行中的工作：trap 内 park（无 cuda-gdb 的挂起/恢复）

设计：cave 迁到 `cuXtraInstrMemBlockAlloc` 分配的**可执行指令块**（`cuXtraInstrMemcpyHtoD`
写入），在 handler 内：

```
写 sig/reason/state -> loop: LD.E flag,[sig+12]; if(flag) exit; else BRA loop
exit: clear state -> IADD R12,0x10 -> RET.ABS R12 0x20
```

scheduler 直接写 host shm 的 `trap_resume=1` 即可恢复（不经过 CUDA，不会被阻塞）。
cave 已用 `nvdisasm` 静态验证（12 条 + 16 条 NOP 填充 + 后向 BRA.U）。

**当前阻塞**（`evidence/live/exp_policy_park_*`）：
- 调用 `cuXtraInstrMemBlockAlloc` 之后，target 的 kernel 会在几百微秒内"成功结束"，
  且 agent 的 `PTRACE_ATTACH` 变成 `EPERM`；session 分配退化为 `0x33`。
- 非 park 路径（不调用 InstrMemBlockAlloc）一切正常 ⇒ 问题与指令块分配/ctx 状态相关，
  需要下一轮定位（候选：改用某个 CUDA module 的代码区当 cave，或先 alloc 再恢复 ctx 状态）。

### 6. 本轮产物

| 文件 | 内容 |
|---|---|
| `probe/l3x.cu` | 新增 `L3X_PATCH`（活体定位+注入）、`L3X_PATCH_EXIT`、`L3X_PATCH_PARK`；SASS 编码器与 cave 构造 |
| `run_l3trap.sh` | 一键复现"活体 handler 执行 + 签名" |
| `evidence/live/exp_live_handler_sig_*` | 签名 0x5aa5 / reason 0x1 / cudaSuccess |
| `evidence/live/exp_policy_park_*` | park 尝试与阻塞现象 |
| `evidence/live/exp_policy_grant_*` / `prime_*` / `selfresume_*` / `dbgflags_*` | v1.4 授权与挂起矩阵 |

### 7. 下一轮（按顺序）

1. 修 park：定位 `cuXtraInstrMemBlockAlloc` 后 ptrace EPERM / kernel 提前结束的原因；
   或改用已加载 module 的代码段做 cave。
2. park 跑通后，把"trap → 写 state → 等 resume"接成 `InterruptContext` 的 trap backend，
   与现有 GDB backend 并存；用 `run_l3work.sh` 的 bit-exact 校验验证中断正确性。
3. 若 park 的 warp 仍占 SM（不一定能让渡），在 cave 里改成"标记 + 跳回内核的 guardian
   检查点"（利用现有 L2 guardian 退出 block），即 sm70/86 的 kill 语义。

---

## v1.6（2026-10-25）— park 的控制面：已能冻住，释放卡在 host 内存一致性

### 1. 新事实：**没有 agent 也会发生 trap**

target 只做注入、不启动 agent（`L3X_PATCH=1`，无 session），victim 跑 7 s 后
`cudaSuccess`，且签名为 `0x5aa5 / reason=0x1`（`evidence/live/exp_solo_trap_sig.log`）：

```
[target] launching victim 1 blocks ~8000 ms
[target] victim finished: cudaSuccess counter=267782895 sync=7.202s
[target] trap-handler signature: 0x5aa5/0 reason=0x1 (LIVE HANDLER EXECUTED <<<)
```

⇒ 595 驱动在**内核启动/结束路径**上自己就会投递 trap（不需要 0x83de 0317）。
这解释了为什么 park 版会在 victim 启动后 ~0.04 s 就崩：launch 期的 trap 不允许
handler 长时间不返回。

### 2. park 已能冻住内核，但 release 不生效

- 用 `L3X_PATCH_PARK`（cave 里循环读 host shm 的 `trap_resume`，arm=1 自旋、arm=0 返回）：
  0317 prime 后计数器**停住**（如 `3245056`），说明 trap 内 park 成功；
- agent 写 `trap_resume=0` 后计数器**仍不动**，最终 context `LaunchFailure`
  （`exp_park_armed_*`、`exp_park_bounded_*`）；
- `bounded`（固定 1e9 次循环）也停很久：handler 的外层 dispatch 会**反复 re-enter**
  0x390，每次把循环计数重置，所以有界循环等价于无限循环。

### 3. 一致性排查（全部失败，供后续别再重复）

| 尝试 | 结果 |
|---|---|
| 普通 `LD.E [R8]` 循环 | arm=1 能读到（首次），release=0 **读不到**（trap warp 缓存了该行） |
| `LD.E.NA`（no-allocate，w1 bit22） | prime 直接不再 park（读到 0） |
| `LD.E.MMIO.GPU`（w1 bit16） | 地址非 MMIO aperture → `IllegalAddress` |
| `CCTL.IVALL` + LD 循环 | launch trap 时 `IllegalAddress` |
| `LDG.E.STRONG.SYS desc[UR4]`（ptxas 原样编码） | UR4 在 trap 上下文无效 → `IllegalAddress` |
| target 侧 `cudaHostRegister(shm)` | prime 不再 park（可见性行为反而变化） |

结论：**trap 上下文对 host-mapped 内存的轮询不可靠**（写可以，读的缓存/一致性不受控）。
park 的控制字必须放在 **GPU 一致可见的 device memory**，并由 GPU 侧写入。

### 4. 下一轮的具体做法（已定）

1. **device flag + 双 context**：target 在主 context `cudaMalloc` 一个 4 KB flag buffer，
   `cudaIpcGetMemHandle` → 在同一进程的第二个 context `cudaIpcOpenMemHandle`，
   由一个 helper 线程在收到 shm 请求时用第二个 context 写 `flag=0`（GPU 侧写，L2 一致）。
   cave 的循环改成 `LD.E [flag_dev]`（device VA，非 host）。
2. **或者**在 cave 里 `LDC R4, c[0x0][0x170]`（XSched 的 preempt buffer 指针；cbank 是
   per-warp 的，trap handler 可能可读）→ `ST.E [R4], 1` 置 global_exit_flag，
   直接复用 XSched 的 L2 guardian 做 block exit（sm70/86 的 kill 语义，不需要 release）。
   先用 `cuXtraSetDebuggerParams` 写入一个已知指针验证 LDC 是否有效。
3. trap 源可以不用 0317（launch/exit 自发 trap 已验证可用），但**per-context 的主动
   触发**仍建议保留 0x83de 0317 + 0xd04 授权。

### 5. 本轮产物

| 文件 | 内容 |
|---|---|
| `probe/l3x.cu` | `L3X_PATCH_PARK/BOUNDED/COUNT/STRONG/LDTEST/NOPATCH/ALLOC_ONLY` 全部实验开关 |
| `evidence/live/exp_solo_trap_sig.log` | **无 agent 的自发 trap + 注入执行** |
| `evidence/live/exp_park_armed_*` | park 成功冻结、release 无效 |
| `evidence/live/exp_park_bounded_*` | 有界循环被 handler 外层循环重置 |
| `evidence/live/exp_park_{strong,na,ldtest}_target.log` | 一致性排查 |

---

## v1.7（2026-10-25）— trap 上下文的可访问内存边界；改走 guardian kill

### 1. 新边界：trap handler 看不到用户 `cudaMalloc` 内存

用 device flag（主 context `cudaMalloc`，helper 线程用 async HtoD 写）做 park 控制字：
helper 的写成功（`flag <- 0`），但 **victim 在 launch trap 即 `IllegalAddress`**
（`evidence/live/exp_park_devflag_*`）：

```
[tgt] devflag=0x7a17de800000 (cudaSuccess)
[helper] flag <- 0 (cudaSuccess/cudaSuccess)
[target] launching victim 1 blocks ~10000 ms
[target] victim finished: cudaErrorIllegalAddress counter=439808 sync=0.069s
```

同进程第二 context 的 `cuIpcOpenMemHandle` 也返回 `0xc9`（OS 错误），此路同样不通。

对照已知可用的访问（都在 trap 上下文里验证过）：
- ✅ host-mapped shm：`ST.E` 可写（v1.5 签名），`LD.E` 首次可读但**不随 CPU 写更新**；
- ✅ 驱动 arena（slot20 对象 0xA00、traphandler 0x390）：可写可执行；
- ❌ 用户 `cudaMalloc` 设备内存：`LD.E` 直接 `IllegalAddress`。

⚠️ 推断：trap handler 运行在一个**受限地址空间**（驱动 arena + 系统内存 aperture），
用户 context 的普通设备分配不在其中。这解释了所有 park 变体的失败，也意味着：
**控制字必须落在驱动 arena，或者根本不需要控制字。**

### 2. 因此改成"不需要 release"的方案：trap 直接置 XSched 的 exit flag

sm70/86 的 trap payload 本来就用 `LDC R4, c[0x0][0x170]` 读**内核的 preempt buffer 指针**
（cbank 是 per-warp 的，trap handler 与内核同 warp）。若 sm120 同样成立，cave 只需：

```
LDC R4, c[0x0][0x170]      ; XSched preempt buffer pointer
LDC R5, c[0x0][0x174]
<load nonzero value>       ; 0x3c00 via HFMA2，guardian 只判 != 0
ST.E [R4], R10             ; global_exit_flag = 1
IADD R12, R12, 0x10
RET.ABS R12 0x20
```

- 全部 6~7 条，正好放进 slot20 +0xA00 的 8 条 cave；
- 不需要 park、不需要 release、不依赖 host/device 一致性；
- 内核里 XSched 的 **L2 guardian** 在每个检查点读同一个 flag，看到后就 `EXIT`，
  即 sm70/86 的 **block kill** 语义；XSched 之后按 checkpoint 重放。

### 3. 下一步（集成，不再做 park）

1. 用 XSched 的 L2 测试程序（guardian 已注入）确认 `c[0x0][0x170]` 在运行内核里
   确实是 preempt buffer 指针（可临时把 cave 改成"LDC 后写到 host shm"来验证）。
2. 把 cave 换成上面的 kill payload；触发用 0x83de 0317（0xd04 授权已通），
   或用自发 trap（v1.6 已证）。
3. 观察：kernel 提前结束、context 存活、XSched 能 resume/重放；用 `l3work` 的
   bit-exact 校验。
4. 接进 HAL：`CudaQueueLv3Trap` 的 sm120 分支 + `GdbInterruptContext` 并存，
   env 选择 backend。

---

## v1.8（2026-10-25）— **L3 kill 闭环成立**：trap → 置 flag → block 退出 → context 存活

### 1. 决定性实验（`probe/l3x.cu`，`L3X_PATCH_GUARD/GUARD_TEST`）

1. 用 `cudaGetKernel + cuKernelGetFunction + cuXtraSetDebuggerParams` 把
   `c[0x0][0x170]` 设成一个 `cudaMalloc` 的 device flag（这正是 XSched
   `InstrumentContext::Launch` 对 `preempt_buffer` 做的事）；
2. target 的活体 handler 被 patch 成 GUARD cave：
   `LDC R4,c[0x170]; LDC R5,c[0x174]; HFMA2 R10=0x5aa5; ST.E [R4],R10; ... RET`；
3. victim 内核走一个 **guardian 等价**的循环：每 256 次迭代用
   `ld.global.cv` 读该 flag，非零就 break。

结果（`evidence/live/exp_guard_devflag_kill.log`）：

```
[tgt] guard-test get=cudaSuccess kernel=0 f=0x58f8bbec0500 devflag=0x7d89d2800000
[tgt] guard-test sync=cudaSuccess elapsed=0.089s devflag=0x5aa5 sig=0 counter=4403200
```

- 未打断时内核要跑 30 s；**实际 0.089 s 退出**；
- devflag = `0x5aa5` ⇒ trap handler 的 `LDC` 拿到了内核 cbank 指针，
  `ST.E` 成功写入**用户设备内存**；
- `cudaSuccess` ⇒ context 存活。

（把 flag 换成 host shm 也一样：`exp_guard_shm_kill.log`，0.006 s 退出、sig=0x5aa5。）

⇒ **sm70/86 的 kill 语义在 sm120 上成立**：
trap → handler 设置 preempt buffer 的 `global_exit_flag` → 内核在检查点退出。

### 2. 还需要一个"是否真的请求了抢占"的判据

自发 trap（v1.6/v1.7）会在内核启动/运行早期出现，如果 payload **无条件 EXIT**
就会把每个内核都杀掉（实测 GUARD 测试 0.006–0.089 s 即退出）。
所以 payload 必须只在调度器请求抢占时才 EXIT。候选判据：

1. **trap reason**：`R2`（do_read_trap_reason 的输出）。自发 trap 与 agent 的
   `0317{0,0}` 都是 `0x1`；但 `0317{1,0}`（debugger hold）可能带别的位，
   需要在 cave 里把 R2 存到 device flag 后读回比对（device **写**可用，
   **读**目前不可靠）。
2. **driver preempt_buf 命令字**：驱动自己的 handler 能读 `preempt_buf+4`
   （它就是这么算 reason 的），说明**驱动 arena 的读是可行的**；用户
   `cudaMalloc` 的 LD 不可靠。可以把"请求"写在驱动 arena（agent 通过
   debugger session 写，或 target 通过 cuXtra 写）。
3. **两段式**：agent 先 `0317{1,0}` 建立 hold，再发 `{1,0}`；
   cave 用 reason 位判定。

### 3. 另一个已确认的事实：CX 会话必须跨 client 才能送 trap

target 用自己的会话发 `0317{0,0}` 返回 0 但**不产生 trap**（`exp_payload_flag0/1`
两次都跑满 4.5 s）。必须是 v1.4 的外部分 client 会话：target 用 `0xd04` 授权
agent client + agent 发 0317。这条集成链路已经全部打通。

### 4. 下一步（集成只剩两件事）

1. **判据**：测 `0317{1,0}` 的 reason（cave 把 R2 写到 device flag，host 读回），
   或把请求写到驱动 arena；把 payload 改成"仅请求时 EXIT"。
2. **接 HAL**：在 sm120 的 L3 trap 路径里
   - 用活体 handler + GUARD/payload cave 替换旧的 slot20 模板 patch；
   - `Interrupt()` 走外部 agent（而不是无效的 `cuXtraTriggerTrap`）；
   - 保留 `Deactivate()` 的 `global_exit_flag=1` 与 `Reactivate()`。
3. 用 `run_l3work.sh` 的 bit-exact 内核验证：中断后结果正确、kernel 可 resume。

### 5. 本轮产物

| 文件 | 内容 |
|---|---|
| `probe/l3x.cu` | `L3X_PATCH_GUARD/PAYLOAD`、`L3X_GUARD_TEST`、`L3X_PAYLOAD_TEST`、`cudaGetKernel` 取 CUfunction、self-0317 对照 |
| `evidence/live/exp_guard_devflag_kill.log` | **30 s → 0.089 s，devflag=0x5aa5** |
| `evidence/live/exp_guard_shm_kill.log` | host flag 版，30 s → 0.006 s |
| `evidence/live/exp_ldc_param_write.log` | LDC cbank 参数 + ST 成功 |
| `evidence/live/exp_payload_flag{0,1}.log` | 同 client 0317 不送 trap 的对照 |

---

## v1.9（2026-10-25）— trap 上下文**只能写、不能读**全局内存；改用"按需改写 cave"

### 1. 决定性诊断（`L3X_PATCH_LDDIAG`）

同一个 cave 里同时做：`LD.E R10,[R4]` → `ST.E [R4+4],R10`，并写一个正对照
`HFMA2 R10=0x5aa5; ST.E [R4+8],R10`。host 在 kernel 结束后读回：

```
[tgt] ld-diag in=0xdead ldout=0 ctrl=0x5aa5
```

- `ctrl=0x5aa5` ⇒ cave **确实执行了**；
- `in=0xdead` 但 `ldout=0` ⇒ **trap 上下文的全局 LOAD 返回 0**（对用户设备内存）。

这就解释了：
- v1.8 的 GUARD cave 为什么成功（只有 LDC + ST）；
- v1.9 的 PAYLOAD cave（`LD 后判 flag`）为什么永远不 EXIT；
- v1.7 的 park 为什么读不到 release。

**结论：trap handler 是"只写"的**（至少对用户 VA）。任何需要在本 handler 内
读全局状态的设计都不可行。

### 2. trap reason 也不能作为判据

| 场景 | 最终 reason |
|---|---|
| 无 agent、自发 trap | `0x1` |
| agent `0317{0,0}` | `0x1` |
| agent `0317{1,0}`（hold）| `0x1`（且计数继续跑，未冻结） |

reason 恒为 bit0，无法区分"自发"与"请求"。

### 3. 可行设计：**按需改写 cave 代码**（arm by patching）

利用两个已证实的能力：
- trap handler 可以 **写** preempt buffer（v1.8：`devflag=0x5aa5`，内核 0.089 s 退出）；
- 驱动 arena 的代码可被 host 通过 `cuXtraMemcpyHtoD` 改写并生效（v1.5 patch 已验证）。

协议：

```
默认 cave（未请求）:  MOV R3,RZ ; IADD R12 ; RET        ← 无副作用
请求抢占时:            target 把 cave 改成 GUARD 写标志:
                       LDC R4/R5,[0x170] ; HFMA2 R10=0x5aa5 ;
                       ST.E [R4],R10 ; MOV R3,RZ ; IADD R12 ; RET
                     → agent 发 0317（跨 client，已验证能送 trap）
                     → 每个 warp 进 handler 时写 global_exit_flag
                     → 各 block 在下一次 guardian 检查点 EXIT（sm70/86 kill 语义）
请求结束:              target 把 cave 改回"return"存根
```

- 不需要 trap 内读任何内存；
- 自发 trap 落在"return"存根上，无副作用；
- 请求状态由**代码**承载，而代码改写/生效已被证明；
- `cudaSuccess` + context 存活由 v1.8 的 guarded 内核验证过。

### 4. 下一轮实现（最后一段集成）

1. `L3X_ARM_PATCH`：默认 return 存根 + helper 线程按 `trap_resume` 切换
   return/GUARD 两版 cave（`cuXtraMemcpyHtoD` + `cuXtraInvalInstrCache`）；
2. target 跑 v1.8 的 `spin_guard`（参数 `c[0x0][0x170]` 指向其 preempt buffer），
   agent 发 0317；预期内核在 ~0.5 s 退出、`devflag=0x5aa5`、`cudaSuccess`；
3. 接 `CudaQueueLv3Trap`：`Interrupt()` = 切 cave + 外部 agent 0317；
   `Deactivate()/Reactivate()` 复用现有实现；用 `l3work` 做 bit-exact 验证。

### 5. 本轮产物

| 文件 | 内容 |
|---|---|
| `evidence/live/exp_trap_ld_returns_zero.log` | `in=0xdead ldout=0 ctrl=0x5aa5` |
| `evidence/live/exp_reason_{spontaneous,hold_*}.log` | reason 恒为 0x1 |
| `probe/l3x.cu` | `L3X_PATCH_LDDIAG`、`L3X_PATCH_REASON`、`L3X_HOLD_ONLY`、`L3X_PAYLOAD_AGENT` |

---

## v2.0（2026-10-25）— **L3 trap 抢占闭环（arm-by-patching）**

### 1. 结果

完整的"调度侧请求 → trap → handler 置 exit flag → block 退出 → context 存活"链路：

```
[tgt] guard-victim flag=0x7538f2800000 get=cudaSuccess kernel=0
[target] launching victim 1 blocks ~10000 ms
[helper] cave <- GUARD (w=128)
[pol]   devflag arm ack=0x1 (dev=0x7538f2800000)
[pol] park compute[0]=0x5c00001b sess=0x5c00f00c modes 0/0/0 prime 0 [.. PARKED]
[helper] cave <- return (w=128)
[target] victim finished: cudaSuccess counter=11541248 sync=0.241s
```

- victim 设定 10 s，**0.241 s 结束**，`cudaSuccess`；
- `cave <- GUARD` 由 target helper 在 agent 发 0317 **之前**写入（arm）；
- agent（跨 client，0xd04 授权）发 `0317{0,0}` 送 trap；
- handler 执行 GUARD cave：`LDC c[0x0][0x170]`（XSched preempt buffer 指针）→
  `ST.E [ptr], 0x5aa5`（global_exit_flag）；
- 内核的 guardian 看到 flag → 当前 block 退出（kill）；
- 之后 `cave <- return` 复位，context 保持健康。

证据：`evidence/live/exp_arm_patch_kill_{target,agent}.log`。

### 2. 为什么采用"改写 cave"而不是"读 flag"

v1.9 已证明 trap handler 的全局 **LD 恒为 0**（`in=0xdead ldout=0 ctrl=0x5aa5`），
因此 handler 内无法判断"是否请求了抢占"。把请求编码在**代码**里：

| 状态 | cave 内容（slot20 +0xA00，8 条） |
|---|---|
| 默认/未请求 | `MOV R3,RZ; IADD R12,0x10; RET.ABS R12 0x20` |
| 已请求（arm） | `LDC R4,c[0x170]; LDC R5,c[0x174]; HFMA2 R10=0x5aa5; ST.E [R4],R10; MOV R3,RZ; IADD R12; RET` |

切换由 target 侧 helper 完成（`cuXtraMemcpyHtoD` + `cuXtraInvalInstrCache`），
状态通过 shm 的 `trap_resume`/`flag_dev_ack` 与调度侧握手。

### 3. 与 XSched 原生 L3 的对应关系

| XSched 机制 | 本原型 |
|---|---|
| `InstrumentContext::Launch` 写 `c[0x0][0x170] = preempt_buffer` | 同（`cuXtraSetDebuggerParams`） |
| `InstrumentManager::Deactivate()` 置 `global_exit_flag=1` | handler cave 置位（等效） |
| guardian 在检查点 `EXIT` block | 同（XSched 原生 guardian 指令） |
| `Interrupt()` 触发 trap | 跨 client agent 发 `0x83de0317`（`cuXtraTriggerTrap` 在 sm120 无效） |
| `Reactivate()` 清 header + 从 resume entry 重放 | 现有实现可复用 |

### 4. 接入 XSched 还差的三件事

1. **live-handler patch 移进 HAL**：在 sm120 的 L3 trap 初始化时定位 arena、
   注入 return 存根、保存 GUARD stub，提供 `ArmKill()/DisarmKill()`。
2. **Interrupt 接外部 agent**：HAL/调度器通过 shm 请求 agent 发 0317；
   agent 进程负责 0xd04 授权与 0x83de 会话（l3x 已实现，可抽成 `l3agent`）。
3. **授权时机**：确认 0xd04 策略在 0xcec0 创建**之后**再发是否仍能让 agent 的
   会话通过；若不行，runtime 需在 context 创建前就启动 agent 并下发策略。
4. 用 `run_l3work.sh` 的 bit-exact 内核做正确性验证。

---

## v2.1（2026-10-25）— 接进 XSched HAL：握手/授权/patch 全通，卡在 launch trap

### 1. 已实现（代码在源码树里，opt-in）

| 文件 | 作用 |
|---|---|
| `platforms/cuda/hal/include/xsched/cuda/hal/level3/trap_sm120_shm.h` | target↔agent 共享控制块 |
| `platforms/cuda/hal/{include,src}/.../level3/trap_sm120.{h,cpp}` | ioctl 拦截（仅 `XSCHED_SM120_L3_BACKEND=TRAP` 时生效）、0xd04 授权、arena/live-handler patch、arm/disarm、trigger |
| `platforms/cuda/hal/src/level3/interrupt.cpp` | sm120 走 `TrapSm120Context::Init/Trigger`，不再调无效的 `cuXtraTriggerTrap` |
| `platforms/cuda/hal/src/arch/arch.cpp` | `XSCHED_SM120_L3_BACKEND=TRAP` → `CudaQueueLv3Trap`（默认仍是 TSG，GDB 仍可用） |
| `test/sm120_l3_trap_v2/probe/l3agent.cpp` | 独立 agent：发布自己的 client、建 0x83de 会话、按请求发 0317/0318 |

端到端（`level 3` + agent）实测日志（`evidence/live/exp_hal_e2e_*`）：

```
sm120 trap: handles client=0xc1d0031f device=0x5c000002 KGO0=0x5c00001b n=8 agent=0xc1d00323 policy=0/0/0
sm120 trap: agent session 0x5c00e000 status=0
sm120 trap: live=0x7d50835f0f00 cave=0x7d50835f5d00 tramp=0x7d50835f5d70 patch=ok
```

⇒ ioctl 拦截拿到了 app 的 client/device/KGO；**0xd04 晚授权有效**；agent 会话 **status 0**；
live handler 指纹匹配、patch 回读一致。

### 2. 阻塞：带 debugger-params 的 XSched 内核在 launch 期被 trap 打断

patch 之后、**还没发 0317**，`level 3` 在第一次 kernel 批量启动约 3–5 s 后报
`cuda error 719`（清 R2/R3 版）或 `715`（保留 reason=1 版）。

- XSched 的 `InstrumentContext::Launch` 对每个 kernel 调 `cuXtraSetDebuggerParams`
  （guardian 需要 `c[0x0][0x170]`），驱动会因此在 **launch 期投递 trap**；
- sm120 的原生 handler 对这类 trap 只有两条路：
  reason=1 → `do_preempt_restore`（会把未保存的 CILP 状态“恢复”进去，破坏内核）；
  reason=0 → 走 epilogue `EXIT.PREEMPTED`（直接杀掉 warp）；
- 我们 patch 的 return 存根无法改变这个事实：它会继续进入 handler 的原生分派。

而 v2.0 的 prototype 之所以 OK，是因为 victim 是**普通内核**（没有 XSched 的
debugger params / 守护入口），trap 只在 RM 0317 时发生。

### 3. 下一轮要试的两条路（按优先级）

1. **关掉 launch blocking**：target 侧设置 `cudbgEnableLaunchBlocking=0`
   （libcuda BSS，v0.2 用过 dlsym 写法），让带 debugger params 的内核不在
   launch 期进 trap；这样 handler 只会看到我们主动发的 0317。
2. **换 patch 点**：launch trap 可能走 slot20 对象（do_pause）的路径；把
   “benign 返回”打在 launch trap 实际经过的返回点，而 RM 0317 用 live handler
   的 armed 重定向。需要先用 GUARD cave 在两种 trap 下分别记录 reason/入口。

在解决前，`XSCHED_SM120_L3_BACKEND=TRAP` 保持 **experimental**；默认/生产路径仍是
TSG（sm120）与 GDB（可用 L3 抢占）。

### 4. 回归确认

默认（不设 env）的 `level 3` 仍走 TSG，跑 20 s 无报错（`evidence/live/exp_hal_default_tsg.log`）；
GDB 后端代码未改动。









---

## v2.2（2026-10-25）— 集成判定的最后一块：**外部 0x83de 会话本身会破坏 XSched 内核**

隔离实验（`evidence/live/exp_hal_session_breaks_ctx.log`）：

| 配置 | 结果 |
|---|---|
| `TRAP` 后端、**不启动 agent**（Init 等待失败后跳过） | 20 s 无错 |
| `TRAP` + agent 建会话 + **NO_PATCH**（handler 完全未改） | 内核 700/715 |
| 同上 + 跳过 `031f/0307/032a` 模式 | 仍 700 |
| 同上 + 只写 cave、不改 0x390 | 20 s 无错 |
| 同上 + 预置 return 存根（0x390 patch） | 719 |

⇒ 结论：
1. ioctl 拦截、shm、Init 本身**不会**破坏内核；
2. **只要 agent 建了绑定 {app client, KGO} 的 0x83de 会话，XSched 的 instrumented 内核在下一次
   launch 就会 700/715**（即使不设模式、不打 patch、不触发）；
3. 这与 v1.4 的判断一致：RM 调试会话需要 **target 侧调试器注入**（cuda-gdb 的
   libcudadebugger/InitializeInjection）才能与内核共存；只复刻 RM 序列不够；
4. 没有会话就没有 trap 源（NO_PATCH 下无自发 trap），所以"外部 agent"这条路在
   XSched 内不可用。

**最终能力边界（sm120 / 595.99.02）**：
- trap 注入/execution/kill 在**普通 CUDA 内核**上完全可用（`run_l3kill.sh`）；
- XSched 的 instrumented 内核（guardian/entry 替换 + debugger params）与外部 RM 会话不兼容；
- 生产 L3 仍是 GDB backend（freeze/resume，已复测）。

**若要继续**：只能在 target 侧复刻 cuda-gdb 的注入（`CUDA_INJECTION64_PATH=libcudadebugger.so`
+ `cudbgApiInit/Attach`），让内核在"调试器已注入"的状态下运行，再建 RM 会话；
这是 R2 路线的完整版，工作量在驱动/代理层，不是一次实验能收尾的。

---

## v2.3（2026-10-25）— 运行时 arm 的最终判定：会话存在时任何代码写都被串行化

| 尝试 | 结果 |
|---|---|
| host `cuXtraMemcpyHtoD` 写 cave/handler（内核运行中） | 阻塞（arm 日志停在 start） |
| host `cuXtraInvalInstrCache`（运行中） | 阻塞到 context 空闲（helper 日志在 victim 结束后才出现） |
| device kernel 写 handler 代码页（**无会话**） | ✅ 成功（`poke64` 回读完全一致） |
| device `write_code` arm（**有 0x83de 会话**） | kernel 写 + stream sync 被阻塞，ack 要等 victim 结束 |
| `fence.proxy.instruction` | ptxas 12.9 不支持该 proxykind |
| 预置 armed patch | 自发 trap + EXIT → 700；只有 agent 的 0317 trap 能干净退出 |

⇒ 结论：**0x83de 会话一旦存在，就会把该 context 上的代码写/启动串行化到内核结束**，
因此"运行中 arm"在 `cudaXtra` 与 device-kernel 两条路都不可行；而预置 armed patch
又会被自发 trap 命中（700）。这与 v2.2 的"会话本身破坏 instrumented 内核"是同一根因：
缺少 cuda-gdb 的 target 侧注入。

**最终判定**：在不复刻 target 侧注入（libcudadebugger）的前提下，sm120 上
"trap 注入 + 任意时机杀 block" 与 XSched 的运行中抢占不能同时成立。
已交付的可用能力：
- 普通 CUDA 内核上的 trap kill（`run_l3kill.sh`，可复现）；
- XSched 的 GDB 冻结式 L3（生产可用，已复测）；
- 完整的 RM/trap ABI 与 SASS 编码工具书。

---

## v2.4（2026-10-25）— 调试态复刻的最后三次尝试（均失败）

| 尝试 | 结果 |
|---|---|
| 在 cuda-gdb 下运行探针，打印注入相关 env | **cuda-gdb 13.2 不设置 `CUDA_INJECTION64_PATH` 等任何注入 env** |
| agent 在建设会话前 `PTRACE_ATTACH` + `PTRACE_CONT` | 会话可建，XSched 内核仍 715 |
| 会话后 3 轮 `0317{0,0}`/`0318` warmup（模仿 cuda-gdb attach） | 仍 715 |
| warmup 后才发布 session_ready（target 等就绪再启动内核） | 仍 715 |

⇒ cuda-gdb 让 target 处于"可被调试会话共存"的状态，靠的不是环境变量、ptrace 关系或
RM 轮询，而是 **调试器在 target 进程内的注入/切换**（v2 里 `sub_222f50` 检查
`ctx+0x9410` 的那一步）。在复刻它之前，外部 RM 会话在 XSched instrumented 内核上
不可用。证据：`evidence/live/exp_hal_ptrace_warmup*`。

另测：target 内调用 `cudbgApiInit(1)`（返回 0、内核可正常跑）+ ptrace + warmup + modes
同时启用 → 仍 715（`exp_hal_cudbginit*`）。cuda-gdb 的注入协议（libcudadebugger 与
debugger 端的握手）是唯一未复刻的部分。

**至此，所有不涉及"调试器注入协议"的路线都已穷尽。**

### v2.4 补充：cuda-gdb 的 agent 拓扑（关键线索）

在 attach 期间观察 `/proc`：cuda-gdb（如 pid 56358）会从 target **内部**拉起一个子进程：

```
56378  ppid=56347(target)  NvDebugAgent /usr/lib/x86_64-linux-gnu/libcudadebugger.so.1 \
                                          56358 142 1 1 63 72 -1
```

- `NvDebugAgent` 是 **target 的子进程**（不是外部 agent），由 cuda-gdb 释放到
  `/tmp/<gdbpid>/session1/NvDebugAgent`；
- 它通过**继承的 fd/socket** 与 cuda-gdb 通信（target 的 fd 表中出现新的 socket，
  且没有 `/dev/shm` 注入文件）；
- 这解释了为什么"外部 agent + RM 会话"永远 715：缺少 gdb 侧的 fd 协议与
  target 内的 debug 切换。

复刻这条路需要实现 gdb 侧的 NvDebugAgent 协议（R2 完整版），超出本轮范围。

### v2.5 补充：cuda-gdb suspend 下活体 handler 仍可执行

在 l3x target（handler 已 patch 为签名 cave）上真实 `cuda-gdb attach` 3 s：
attach/detach 成功（rc=0），victim `cudaSuccess` 跑完，target 侧签名 `0x5aa5/reason=0x1`
（`exp_live_handler_under_gdb*`）。
注意：签名仍可能与自发 trap 混淆，不能单独证明 suspend 的 0317 走了 0x390；
但至少证明"调试器注入态 + 活体 handler patch"可以共存（这正是外部 agent 会话所缺的）。

### v2.5 补充 2：clock-stamp cave 无法区分 trap 来源

给 cave 加 `CS2R R6, SR_CLOCKLO; ST [R4],R6; ST [R4+4],R7`，把 trap 时刻写到 host shm，
外部 10 ms 采样。结果（`exp_clockstamp_*`）：
- 时钟字在采样窗口内**持续变化**（近似每个采样点都变），high/low 出现撕裂；
- 说明 handler 在 kernel 运行期间被**高频进入**（或 handler 外层 dispatch 循环反复经过 0x390），
  单点时间戳无法把"0317 suspend"与"自发/循环 trap"分开；
- 该实验不支持也不否定"gdb suspend 走 0x390"，但暴露了 handler 的重复进入行为，
  对后续若要做 trap payload 是一个重要约束（payload 必须可重入/幂等）。

### v2.6：最终对照——"跳过 guardian/entry 重定位"仍不能与外部会话共存

为排除"instrumented entry point 导致会话 715"的假设，在 L2 队列加
`XSCHED_SM120_TRAP_NO_INSTR`：kernel 走 L1 原始 launch（不改 entry、不设 debugger
params），agent 仅建会话（无 warmup、无 modes、无 patch、无触发）。

结果：XSched 连续 launch 仍 `700/715`（`exp_hal_no_instr.log`）。
而 l3x 的单内核 + 同一会话是稳定的（v2.0）。
⇒ 会话与驱动的 **launch-stop（launch blocking）** 交互才是硬边界：调试器不在
驱动侧应答第二次 launch 的 stop，XSched 的多内核流就会崩。该 stop 的应答只有
`NvDebugAgent`/gdb 侧协议能完成。

**最终结论（不再有未验证的 RM 层假设）**：
sm120 上的 trap 原语（注入/执行/杀普通内核）已开发并可复现；XSched 的连续-launch
路径与外部 RM 会话不兼容，必须实现 gdb 侧 RPC 才能在同一进程内做 trap 抢占。
生产可用 L3 = GDB backend。

---

## v2.7（2026-10-25）— 会话生命周期重测 + XSched 上游 trap payload 打通到 0x880；RM 0317 无调试器应答会阻塞

### 1. 先纠正 v2.6 的一个实验缺陷

v2.6 声称"agent 仅建会话、无 patch 仍 700/715"。复查 `exp_hal_no_instr.log`：
里面明确打印 `always-armed=ok` —— 当时的 `NO_PATCH` 语义被写成
`always = ALWAYS_ARMED || no_patch`，即 **设置 NO_PATCH 反而会预置 armed 桩**。
所以 v2.6 的"会话本身破坏 launch"结论**不成立**，本节重做了隔离实验。

代码已修正：`NO_PATCH` 现在表示"完全不写 handler"，`Arm()` 也直接返回
（`platforms/cuda/hal/src/level3/trap_sm120.cpp`）。

### 2. 隔离实验（全部为 `level 3` + TRAP backend + 外部 agent）

| 配置 | 结果 | 证据 |
|---|---|---|
| 会话 + 只探测 handler（不 patch） + 不触发 | 内核正常跑；第一次 Suspend 卡在等待 trap（预期） | `exp_iso_nopatch.log` |
| 同上 + 不插桩（`XSCHED_SM120_TRAP_NO_INSTR`） | 同上 | `exp_iso_noinstr.log` |
| 同上 + 每 5 ms 发一次 `0318` | 仍卡（`0318` 清不掉 launch stop） | `exp_iso_pump.log` |
| 同上 + `cudbgApiInit(1)` + `cudbgApiAttach()`（返回 0） | 仍卡 | `exp_iso_cudbgattach.log` |
| **只有 5 次直接 launch + sync（`L3_LAUNCH_ONLY`）** | **全部 cudaSuccess，包括预置 cave 的情况** | `exp_launchonly_probe.log` / `exp_launchonly_patch.log` |
| 完整测试但预置 cave（armed/return/guard） | 第一个 TestBlockPreempt 的 sync 立刻 700 | `exp_prearmed1.log`、`exp_exittramp1.log`、`exp_guarded1.log`、`exp_flagkill1.log` |
| 完整测试，预置 **probe** cave（只写 R2 后正常返回） | 内核正常跑 1 s，直到第一次 Suspend | `exp_reason_probe3.log` |

⇒ 修正后的结论：
1. **0x83de 会话本身不会破坏普通 launch**（只要不在 patch 里解引用空的 debugger param）；
2. 预置 armed/guard cave 时 700 的直接原因是 cave 无条件解引用 debugger param：
   launch-stop trap 的 reason 恒为 0、且第一条 launch 的 `c[0x170]/c[0x178]` 可能为 0
   → `LD.E [0]` / `STL → TRAP_RETURN_PC=0` → 700；
3. `reason == 0` 可区分 launch trap 与显式 `0317`（后者 reason 非 0，`exp_reason_hold_target.log` 记到 0x1）。

### 3. 新发现：XSched 自带的 sm120 trap payload 是可用的，只是从没被装上

`platforms/cuda/hal/src/arch/sm120.cpp` 里一直有 `trap_inject_instrs[]`
（源 `inject.cu::check_preempt_trap`，与 sm86 同一套逻辑，已按 `nvdisasm` 审过），
`TarpHandlerSM120::Instrument()` 会把它写进 instr block，并把 handler **返回桩 0x880**
改成 48 位绝对跳转。过去 sm120 上它被两处短路：

- `InterruptContext::InstrumentTrapHandler()` 里 `if (sm120_trap_) return;`
- `TarpHandlerSM120` 当初只按 `cuXtraTriggerTrap` 设计，而该触发器在 sm120 上无效。

本轮改动：
- 删除上述短路；在 `InterruptContext` 构造（任何 kernel 之前）调用 `InstrumentTrapHandler()`；
- `TrapSm120Context` 默认走"上游 payload"模式（不再自己 patch +0x390）；
  原 cave 路径保留在 `XSCHED_SM120_TRAP_CAVE=1`。

实测（`exp_upstream1/2/3/4.log`）：

```
sm120 trap: handler payload installed at +0x880 (upstream payload)
sm120 trap handler instrumented: patch 0x7579bb5f5b80 -> inject 0x7579b8e00000 (replaced 0000...7918)
trap handler instrumented: dev ... size 4864 inject ... size 672
Interrupt: op stream idle, triggering
[agent] session 0x5c00e000 -> 0
[agent] sending 0317{0,0}
… 0317 永不返回；target 等 trigger_ack 超时；队列 Synchronize 卡住 …
```

⇒ **RM `0x83de0317` 的 ioctl 本身会阻塞**：它要等 trap 在设备侧真正完成
（payload 的 `CERRBAR/CCTL.IVALL/EXIT` 流程），而"完成"需要**调试器侧应答**。
`0318` 是同一个 client 上的 RM 调用，被挂在 0317 后面，开线程也解不开（RM 串行化）。
这与 v2.4 的结论同根：缺 target 内的 `NvDebugAgent` 注入。

### 4. 运行期改 cave 的三条路，全部有硬约束（汇总）

| 方式 | 结果 | 证据 |
|---|---|---|
| host `cuXtraMemcpyHtoD`（app 在跑） | 阻塞（OpStream 也一样） | `exp_trace2.log` |
| `cuXtraInstrMemcpyHtoD` | 同上（先被 app 流 719 打断） | `exp_instrwrite1.log` |
| device 写入 kernel（独立 non-blocking 流） | 排队但永不执行（`StreamQuery=600`，144 ms） | `exp_trace4/5.log` |
| 预置 armed cave | 被 launch-stop trap 命中 → 700 | 见 §2 |

⇒ 在"app 的 kernel 一直占着 GPU"的前提下，**运行期改 cave 不可行**；
唯一可行形态是"启动前装好、由 payload 自己按 preempt buffer 判定"，
也就是 §3 的上游 payload —— 它不需要运行期写代码。

### 5. 新增/修改的环境开关（都在 `trap_sm120.cpp` / `l3agent.cpp`）

| 变量 | 作用 |
|---|---|
| `XSCHED_SM120_TRAP_CAVE=1` | 回到 +0x390 cave 实验路径（默认走上游 payload） |
| `XSCHED_SM120_TRAP_PRE_RETURN=1` | cave 模式：Init 时装 return 桩而不是 armed 桩 |
| `XSCHED_SM120_TRAP_INSTR_WRITE=1` | cave 模式：用 `cuXtraInstrMemcpyHtoD` arm |
| `XSCHED_SM120_TRAP_PARAM_PROBE=1` / `REASON_PROBE=1` | 把 trap 时的 R2 / `c[0x170]` 写进设备缓冲并打印 |
| `XSCHED_SM120_TRAP_NO_INSTR` | L1 直启动（跳过 guardian） |
| `L3_LAUNCH_ONLY=<n>`（测试程序） | 只 launch+sync，不 suspend，用于隔离 |
| `L3AGENT_EAGER=1` | agent 启动即建会话（旧行为） |
| `L3AGENT_RESUME_PUMP=1` | agent 每 5 ms 发 0318 |
| `L3AGENT_AUTO_RESUME=1` | 0317 后 20 ms 从另一线程发 0318 |
| `L3AGENT_DELAY_MS=n` | 延迟建会话 |

### 6. 当前判定

- trap 原语（注入→执行→杀普通 kernel）仍然成立（`run_l3kill.sh`，v2.0 证据未变）；
- XSched 侧：**handler payload 已能在 sm120 上正确安装并进入**（新证据，v2.6 之前从未做到），
  触发链路走到 `0317`；卡点是 `0317` 需要调试器应答才能返回/完成；
- 生产 L3 仍用 GDB backend（freeze/resume 已验证）。
- 下一步唯一有价值的入口：复刻 target 内 `NvDebugAgent`（协议样本 `gdb_agent_protocol.log`），
  或改用"GDB 只负责 launch-stop 应答、payload 负责杀 kernel"的混合模式。

### 7. 决定性对照：cuda-gdb 在场时 `0317` 立刻返回，payload 能被触发

把同一条命令放到 cuda-gdb 下跑（目标由 gdb 启动，agent 用 `L3AGENT_NO_PTRACE=1`，
其余配置与 §3 完全相同）：

```
cuda-gdb -batch -ex run --args /tmp/xsbuild/platforms/cuda/test/level 3
```

结果（`evidence/live/exp_gdb_ack1.log`）：

| 时刻 | 事件 |
|---|---|
| 20.489 | 上游 payload 装到 handler+0x880（inject 672 B） |
| 21.521 | `0317{0,0}` **status 0，立即返回**，ack=1 |
| 26.535 | 第二次 `0317` 同样 **status 0**（说明 suspend 流程能重复） |
| 36.552 | `Reactivate()` 的 `StreamSynchronize(op_stream)` 报 **700**（gdb 侧也打印
`cuMemcpyDtoHAsync_v2 returned 0x2bc`） |
| … | 之后 `cudaLaunchKernel` 连续 0x2bc/0x4 |

⇒ 三点结论：
1. **RM 0317 本身没有坏**：只要进程里有调试器应答（cuda-gdb 的 NvDebugAgent），
   它就正常返回；v2.7 §3 的"阻塞"纯粹是缺应答。这是第一次在 sm120 上看到
   "RM 触发的 trap 在 XSched 的 payload 装机后被完整走完"。
2. 上游 payload 的执行会把 context 弄坏（700）：最可能是 block 标志写越界，
   或 trap 打到了 **没有 debugger params 的 kernel**（`c[0x170]=0` → `LD.E [0]`）。
   要定位需在 payload 里加边界检查，或确认 `kKernelLaunchOriginal` 的
   args 布局（它的 `args_buf` 只有 preempt_buf 非 0，`+8/+16/+24` 全 0）。
3. 这条混合路线（**cuda-gdb 只负责调试器应答，payload 负责杀 kernel**）已经
   比纯 GDB freeze 更接近目标：suspend 能重复两次、0317 不再阻塞。
   剩余工作是修 payload 的越界/空参问题，再把 gdb 的应答协议替换掉。

复现命令：

```bash
cd xsched/test/sm120_l3_trap_v2
export XSCHED_SM120_L3_BACKEND=TRAP XSCHED_SM120_TRAP_SHM=/xsched-sm120-trap \
       XSCHED_SM120_TRAP_AGENT=$PWD/bin/l3agent \
       L3AGENT_NO_MODES=1 L3AGENT_NO_WARMUP=1 L3AGENT_NO_PTRACE=1 L3AGENT_AUTO_RESUME=1
LD_LIBRARY_PATH=/tmp/xsbuild/platforms/cuda:/tmp/xsbuild/preempt \
  cuda-gdb -batch -ex run --args /tmp/xsbuild/platforms/cuda/test/level 3
```

### 8. 关键发现：当前测试程序的内核 launch **根本不经过 HAL 队列**

为了给"resume 重放被杀内核"找命令来源，在 `CudaQueueLv1::Launch` 与
`CudaQueueLv3Trap::Launch` 都加了日志，并新增 `L3_SPIN_ONLY`（受管队列上的长内核 +
host-mapped 计数器）。结果：

- 任何运行（普通 `level 3`、`L3_SPIN_ONLY`、`L3_LAUNCH_ONLY`）里
  **`L1::Launch` / `remembered kernel cmd` 一次都没有出现**；
- `Reactivate` 看到的 command log 恒为 0 项；
- `libshimcuda.so` 导出的是 **cu\* 驱动 API**（不是 cudart）；用
  `LD_PRELOAD=libshimcuda.so` 也拦不到（cudart 12.9 通过 `cuGetProcAddress`
  取驱动函数指针，绕过 PLT 插桩）。

⇒ 当前 `level` 测试二进制里的 `<<<>>>` 完全绕过 XSched 队列，只在
`XQueueCreate` 时登记了一个空队列。因此：
1. 这一轮所有"XSched 内 trap 抢占"实验（v2.7 §7、本节 freeze 实验）实际测到的是
   **RM 层的 0317 暂停**，没有 guardian、没有 command log、没有可重放的 launch；
2. **suspend 是真实的**：受管/非受管内核都表现为 27–30 ms 内计数冻结、保持整个
   hold 窗口、`cudaSuccess`，这是目前最硬的 L3 抢占证据（`exp_managed_spin1.log`、
   `exp_shim_freeze1.log`）；
3. **resume 无效**：`0318` 返回 0 但内核没有继续（计数不再增长、stream 立即完成）。
   在没有受管 launch / command log 的前提下，resume 需要另找语义（RM 的 resume 是否
   能恢复被暂停的 warp，还是必须重放）；
4. 要让 XSched 真正管到内核，必须让应用走 XSched 的 cudart/驱动 shim
   （例如用 `platforms/cuda/shim` 的方式链接，或直接 `XQueueLaunchKernel`），
   这是后续端到端工作的**前置条件**——否则所有 payload/kill/resume 讨论都悬空。

复现（suspend 证据）：

```bash
export XSCHED_SM120_L3_BACKEND=TRAP XSCHED_SM120_TRAP_SHM=/xsched-sm120-trap \
       XSCHED_SM120_TRAP_AGENT=$PWD/bin/l3agent \
       L3AGENT_NO_MODES=1 L3AGENT_NO_WARMUP=1 L3AGENT_NO_PTRACE=1 \
       XSCHED_SM120_TRAP_NO_BARRIER=1 L3_SPIN_ONLY=4000 L3_SPIN_HOLD=800
LD_LIBRARY_PATH=/tmp/xsbuild/platforms/cuda:/tmp/xsbuild/preempt:/tmp/xsbuild/utils \
  cuda-gdb -batch -ex run --args /tmp/xsbuild/platforms/cuda/test/level 3
# 日志：counter 冻结于 suspend 时刻，hold 期间不变，final sync cudaSuccess
```

### 9. resume 语义收尾：RM 0318 不能恢复被 0317 停下的内核

| 配置（L3_SPIN_ONLY，hold 700–800 ms） | suspend | hold 期间计数 | 0318 之后 | 证据 |
|---|---|---|---|---|
| payload + Deactivate + 0317{0,0} | 29 ms | 冻结 | 不恢复，final sync 立即成功 | `exp_managed_spin1.log` |
| 无 payload（`NO_PAYLOAD`）+ 不 Deactivate + `0317{1,0}` | 27 ms | 冻结 | 不恢复 | `exp_nopayload_freeze1.log` |
| 同上 + 完整 debug modes（`031f/0307/032a` + 0309/030c + warmup） | 62 ms | 冻结 | 不恢复 | `exp_modes_freeze1.log` |
| 上述任意配置 + `0318`（agent 返回 0） | — | — | **stream 立即完成，计数不再增长** | 同上 |

⇒ 在本机驱动 595.99.02 上，**RM `0317` 的 trap 一旦命中运行中的 warp，内核就结束**
（与 l3x 的 kill 观察一致），`0318` 只解除 context 的 suspend 状态，不会把 CILP
现场恢复回内核。cuda-gdb 之所以能"暂停后继续"，靠的是 target 内 NvDebugAgent 的
完整 CILP 保存/恢复 + context 切换，而不是这两个 RM 命令本身。

**因此可达的 L3 语义在本平台是 kill + 重放**，不是 in-place resume。
重放需要 XSched 真正管理内核 launch（见 §8），这是端到端剩余工作的前提。

**当前可复现的完整能力（本机）**
1. `run_l3kill.sh`：普通内核 8 s → **0.234 s**、`cudaSuccess`（重测通过）；
2. XSched 内（无论 launch 是否受管）：`XQueueSuspend` → **27–30 ms** 内计数冻结，
   保持整个 hold 窗口，context 干净；
3. handler payload 可在 sm120 上安装并执行（§3/§7）。

### 10. 更正与最终状态（v2.8）

**更正 §8 的两点**
1. `LD_PRELOAD=libshimcuda.so` + 动态 cudart（`-cudart=shared`）确实让
   `cuLaunchKernel` 变成动态符号，但 **XSched 的 HAL 仍然看不到 launch**：
   `CudaQueueLv1::Launch`、`CudaQueueLv3Trap::Launch`、`InstrumentContext::Launch`
   **全部 0 次调用**（`exp_instrctx_launch1.log`）。因此本机这套构建里，
   测试程序的 `<<<>>>` 走的是另一条（非 HAL）路径，队列只登记了一个空壳。
2. `NO_TRIGGER` 那次"308 ms 后计数冻结、final sync 立即成功"其实是
   **内核自然跑完 1 s 的 spin**（计数器末尾写到 `i|1<<63`，采样掩掉 bit63 后
   看起来像冻结）。**不能作为抢占证据**，已在下面剔除。

**仍然成立的证据（trap 真抢占）**

| 实验 | spin 时长 | suspend | 计数冻结位置 | 之后 |
|---|---|---|---|---|
| `exp_managed_spin1.log`（payload + Deactivate） | 4 s | 29 ms | 22,291,968（≈0.6 s 处） | final sync 立即成功、计数不动 |
| `exp_nopayload_freeze1.log`（无 payload + HOLD） | 4 s | 27 ms | 22,107,648 | 同上 |
| `exp_modes_freeze1.log`（完整 modes） | 4 s | 62 ms | 22,054,912 | 同上 |
| `exp_l3spin1/2/3.log` | 3–4 s | 30 ms | ≈22.3 M | 同上 |

这些运行里内核本应再跑 3–4 s，却在 suspend 后立即"完成"，且计数停在 suspend 时刻
⇒ **RM 0317 的 trap 确实在 ~30 ms 内终止了运行中的内核**（kill 语义，不可原地恢复）。

**结论（不变）**：sm120 上可达的 L3 语义是 **trap kill**；`0318` 只解开 context 的
suspend 状态，不会恢复被终止的内核。要让 XSched 端到端可用，还需要：
1. 让应用真正走 XSched 的 launch 路径（本构建下未生效，是当前最大缺口）；
2. 在 HAL 层记录/重放被杀的内核（kill+replay），因为框架的 command log 为空；
3. 用进程内 NvDebugAgent 协议替换 cuda-gdb 的 0317 应答。

**可复现交付（截至本版）**
- `run_l3kill.sh`：8 s 普通内核 → **0.234 s**、`cudaSuccess`（重测通过）；
- `L3_SPIN_ONLY`：XSched 内 27–30 ms 终止运行中内核、context 干净；
- `TrapSm120Context` + `l3agent` + `TrapSm120Shm`：授权、lazy 会话、RM 触发、
  handler payload 装机全链路；
- 生产可用 L3：GDB backend。

### 11. 端到端闭环的最后一步：managed launch + kill + replay 已串起来

§8 的结论是"launch 不经过 HAL"。本轮找到了**受支持的入口**：
`libshimcuda.so` 的 `cuLaunchKernel`（即 `XLaunchKernel`）会把内核打包成
`CudaKernelLaunchCommand` 并 `xq->Submit()`。
`probe/l3qspin.cu` 用 `dlopen("libshimcuda.so") + dlsym("cuLaunchKernel")`
显式走这条路径（因为 cudart 12.9 通过 dlopen/dlsym 取驱动入口，LD_PRELOAD 拦不到）。

实测（`evidence/live/exp_qspin_route1.log`、`exp_qspin_replay2.log`）：

| 阶段 | 结果 |
|---|---|
| 启动 | `xcmd type (0)` 入队；`trap queue: remembered kernel cmd idx=1` ✅ |
| 运行 | 计数稳定增长（≈36 M/s）✅ |
| Suspend（0317 + payload + Deactivate） | **30.6 ms**，计数冻结在 22,396,160 ✅ |
| Resume | `trap reactivate: replaying the killed kernel` → 新内核实例启动（计数从 0 涨到 81,408）✅ |
| 之后 | 重放的内核 ~8 ms 后触发 **700（context 损坏）**，final sync = cudaErrorIllegalAddress ❌ |

⇒ **trap 抢占 + 受管 launch + 重放** 三段都已打通，最后一段（重放后持续运行）
还会把 context 弄坏。最可能的原因（待验证）：
1. 复用已完成的 `CudaKernelCommand` 重放时，guardian 的 per-block 标志/参数
   仍残留上一次 kill 的写入（preempt buffer 的 block 槽位）；
2. 重放走 `kKernelLaunchGuardian`，而 `preempt_idx_` 为 0（payload 没写）导致
   `InstrumentManager::Launch` 选了guardian 而不是 resume 入口；
3. 指令/参数生命周期问题（shim 以 `deep_copy=true` 构造，共享指针仍在）。

**复现**

```bash
cd xsched/test/sm120_l3_trap_v2
export XSCHED_SM120_L3_BACKEND=TRAP XSCHED_SM120_TRAP_SHM=/xsched-sm120-trap \
       XSCHED_SM120_TRAP_AGENT=$PWD/bin/l3agent \
       L3AGENT_NO_MODES=1 L3AGENT_NO_WARMUP=1 L3AGENT_NO_PTRACE=1 \
       L3AGENT_AUTO_RESUME=1 XSCHED_SM120_TRAP_NO_BARRIER=1
LD_LIBRARY_PATH=/tmp/xsbuild/platforms/cuda:/tmp/xsbuild/preempt:/tmp/xsbuild/utils \
  cuda-gdb -batch -ex run --args ./bin/l3qspin 5000 500
```

**下一步（按优先级）**
1. 重放前清空 preempt buffer 的 block 槽位（Reactivate 只清 header 16 B，
   而 payload 写了 `+4*(idx+4)` 的 restore flag）；
2. 重放单独用一个干净的 preempt buffer，或让 payload 的 kill 路径不再写
   block 槽位（只 EXIT）；
3. 用 `XSCHED_SM120_TRAP_NO_PAYLOAD` 复测重放，分离 payload 残留与重放本身的问题。

### 12. 分离变量：无 payload 时的重放（context 干净，但内核提前结束）

`XSCHED_SM120_TRAP_NO_PAYLOAD=1`（不装 XSched payload，靠驱动 handler 自己终止内核）
+ managed launch + 重放（`exp_qspin_replay3.log`）：

| 阶段 | 结果 |
|---|---|
| suspend | **29.5 ms**，计数冻结在 22,122,496 |
| replay | 新内核实例启动，计数 187,136 |
| 之后 | 计数停住，`final sync -> cudaSuccess`（**没有 700**） |

⇒ 700 确实和 payload 有关（payload 的 per-block 槽位写入 / EXIT 路径）；
但重放的内核在 ~20 ms 后自己结束（clean exit），说明重放用的是
guardian/旧 preempt 状态而不是"正常启动"。

**下一步（明确）**
1. 重放不要复用 `InstrumentManager` 的 guardian/resume 选择：为 replay 增加一个
   `kKernelLaunchOriginal` 类型（清空 debugger params 的 kIdx/killable），
   或直接 `kernel->LaunchWrapper(kStream)` 走原始 launch；
2. 重放前把 preempt buffer 整块清零（不只 header 16 B）；
3. 1 修好后，再把 payload 的 kill 路径改成"只 EXIT、不写 block 槽位"，
   消除 §11 的 700。

### 13. 合法 resume（保留 session，0318 status=0）下仍只跑 ~3 ms

`XSCHED_SM120_TRAP_KEEP_SESSION=1` + `NO_PAYLOAD` + `REPLAY_ORIGINAL`
（`exp_qspin_replay5.log`）：

- `sm120 trap: resume ack=1 status=0` —— 这次 0318 是**有效**的（不再 0x57）；
- 重放的 launch 调用本身阻塞 **14.5 s**（`replaying` → `replayed through the original entry`）；
- 之后新内核实例只跑到 114,944（≈3 ms）就结束，`final sync cudaSuccess`。

⇒ resume 的子问题已经收敛到"重放 launch 被阻塞 14.5 s + 重放内核提前结束"，
与 session/0318 无关（0318 已 0）。下一步应查：
1. 14.5 s 阻塞发生在 `CudaQueueLv2::DirectLaunch → InstrumentContext::Launch`
   的哪一步（`cuXtraSetDebuggerParams` 还是 `LaunchWrapper`）；
2. 重放内核为何在 ~3 ms 后 exit（用 `XSCHED_SM120_TRAP_PARAM_PROBE` 在 trap 里
   读 c[0x170..0x188]，或在重放内核里写退出码）。

### 14. 重放阻塞点定位：`cuLaunchKernel` 本身等调试器（4.6 s）

去掉 `CtxSetCurrent` 后（调用线程本来就是 app 的 context），14.5 s 的阻塞消失，
但时间转移到 `instr launch(orig): setentry done, launching` →
`launched ret=0` 之间：**`cuLaunchKernel` 本身阻塞 4.6 s**
（`exp_qspin_replay7.log`）。随后重放内核只跑 499,200 次（≈14 ms）就结束，
`final sync cudaSuccess`。

⇒ 这 4.6 s 与"重放内核只跑十几毫秒就停"都指向同一个原因：**context 仍在
调试器（cuda-gdb 的 agent）的 launch-blocking 管理下**——新 launch 要等调试器放行，
放行后又被下一次 stop 拦下。也就是说，最后一段（重放后持续运行）**必须有调试器侧
的正确 resume/continue**，这正是 v2.4 就锁定的 NvDebugAgent 协议。

**结论**：在"外部 cuda-gdb 仅作 0317 应答"的用法下，kill 与 replay 都做到了
（30 ms kill、重放实例能起来），但无法让重放内核长期运行；要真正闭环，
必须实现 target 内 NvDebugAgent 的 launch-blocking/resume 应答（协议样本已有），
或者让驱动以非调试器方式接管（目前未找到开关）。

### 15. resume 后的 context 状态：新 launch 也会短命，且不稳定

为区分"重放旧命令不安全"与"context 被 trap 弄坏"，在 `l3qspin` 里加了
`L3Q_RELAUNCH`：resume 之后用 shim 的 `cuLaunchKernel` **重新提交一个全新内核**。

| 配置 | 结果 |
|---|---|
| 正常 0318（hold 2 s 后再 resume，`KEEP_SESSION`） | 新内核只跑 80,896 次（≈2 ms）就结束，`final sync cudaSuccess` |
| 同一配置再跑（`exp_qspin_done1.log`） | suspend 的 `Synchronize` 直接 **700**（不稳定） |
| 无 payload / 有 payload | 都是短命；有 payload 更容易 700 |

⇒ 结论：
1. **不是命令复用的问题**——全新 launch 同样短命；
2. RM 的 `0317`+`0318` 组合**没有把 context 恢复到"可以正常跑新内核"的状态**
   （cuda-gdb 的暂停/恢复走的是 target 内 NvDebugAgent 的 CILP 保存/恢复，
   不是这两个 RM 命令）；
3. 行为不稳定（同一命令有时 clean、有时 700），说明 trap 终止内核后驱动侧
   还有异步清理/状态残留。

**这解释了为什么 v2.4 以来所有"外部 agent 复刻"都只能到 suspend 为止。**
要闭环（resume 后内核继续跑），必须实现 target 内的 NvDebugAgent 协议：
- 样本：`evidence/live/gdb_agent_protocol.log`（`u64 len | u32 type 0xc | u32 cmd | args`）；
- 启动：`DebugAgentMain(debugger_pid,142,1,1,63,72,-1)`；
- 需要实现的关键命令：launch-stop 应答、context resume/continue。

**本轮生产回归**：`run_l3hal.sh` 通过——GDB backend 基线 119.19 s、
suspend 后 0.74 s（`Resume(hog)=1`），未受本轮 HAL 改动影响。

### 16. 去掉 gdb 依赖的尝试：`cudbgApiInit(2)` 失败

`XSCHED_SM120_TRAP_CUDBG=1 XSCHED_SM120_TRAP_CUDBG_MODE=2 XSCHED_SM120_TRAP_CUDBG_ATTACH=1`
（target 内初始化 cudbg、attach）：

- `cudbgApiInit(2) -> 91`（失败；mode 1 才返回 0）；
- `cudbgApiAttach() -> 0`，但 **0317 仍无应答**（ack=0），30 s 后 context 700
  （`exp_nogdb_cudbg2.log`）。

⇒ target 内直接调用 cudbg API 并不能替代 NvDebugAgent 的 gdb 侧协议。

另：`cudbgEnableLaunchBlocking` 在本机默认就是 **0**（不是重放 4.6 s 阻塞的原因）；
把 hold 拉长到 5 s 后重放 launch 不再阻塞（0.7 ms），说明那 4.6 s 是**trap 终止后的
异步清理窗口**——重放要等它结束。

**本轮最终结论（供接手）**
- 已打通：授权 → lazy 会话 → RM 0317 trap → **27–30 ms 终止运行中内核** →
  shim 受管 launch → 记录并重放被杀内核（新实例能起来）。
- 未打通：重放/新内核在 resume 后只跑几毫秒就结束、且偶发 700。根因是 RM 0317+0318
  无法把 context 恢复到正常执行态，必须实现 target 内 NvDebugAgent 的
  launch-stop / resume 应答。
- 生产 L3（GDB backend）本轮回归通过：119.19 s → 0.74 s。

### 17. 受管/instrumented context 上 `0317` 必须要有调试器（无 gdb 时 ioctl 失败）

| 目标类型 | agent | `0317` 结果 |
|---|---|---|
| l3x 普通内核（`cuXtraSetDebuggerParams` 由 target 自己调） | 无 gdb，ptrace attach | **成功**（kill 原型，今天复测 0.234 s） |
| `l3qspin` 受管内核（XSched InstrumentContext 插桩 + shim launch） | 无 gdb（有/无 ptrace） | **`0xffff`（ioctl 失败）**，随后 context 700 |
| 同上 | cuda-gdb 在场 | `status 0`，trap 命中（27–30 ms kill） |

⇒ "受管 + 插桩"的 context 比 l3x 的裸 context 多了一层 driver 状态
（launch-stop/debugger params），**RM 0317 只有在调试器在场时才被接受**。
这也解释了为什么"外部 agent 复刻"始终绕不开 cuda-gdb。

**本轮新增的机制证据**
1. **受管 launch 存在 launch-stop trap**：装了 cave 探针（无触发）后，第一次 launch
   即 700 —— 说明 03xx handler 在 launch 时被调用，且 reason 非 0（探针不清 R2 就会
   进入 do_preempt_restore/pause 路径而 fault）。XSched payload 的"killable/flag 检查后
   直接返回"正好绕开它，所以 payload 版第一次 launch 能正常跑。
2. **trap 之后的 context 无法用 RM 序列恢复**：0318（单独/带 0317{0,0} 轮询）、
   resume pump、resume 后 free session、按原入口重放、全新 launch —— 全部试过；
   新内核总是在几毫秒内被再次终止（`done=0`），且偶发 700。
3. 设备侧完成字（`dev=`）与 host 映射计数一致，排除了"host 映射失效"的解释。

### 18. 更正 §17-1：不是"launch trap"，是我探针的 ST.E 编码写错；并首次抓到 reason=0x8

**更正**：§17 第 1 条（"受管 launch 存在 launch-stop trap，探针即 700"）作废。
探针 cave 里的 `ST.E [R8],R2` 编码写错（把源寄存器放进了偏移字段），
地址变成 `buf+0x20000` → 越界 700。按 l3x 的正确编码
（`w0=0x…08007385`，源寄存器在 `w1` 低字节）修正后，同样的探针**不再 700**。

**首次抓到的 trap 现场**（`XSCHED_SM120_TRAP_PARAM_PROBE` + 受管 launch，
`exp_probe_postresume.log`）：

```
reason probe buf 0x8 0x2000000 0 0
                 ^R2   ^c[0x170] 低 32 位
```

- **R2 = 0x8**：按 `do_read_trap_reason` 的位含义，bit3 = **do_pause**
  （`+0x370: bit3(0x08) → CALL do_pause; R2=1; goto loop`）——即"请把 warp 停下来"；
- `c[0x170] = 0x02000000`（低 32 位）是一个合法设备地址，说明 debugger param 有效；
- 该 trap 出现在 **suspend/恢复后新 launch** 的窗口内，新内核随后只跑到 81,408 就停住。

**由此得到的机理推断（下一步验证方向）**
1. 新内核的 launch 会被 handler 以 **reason=0x8（do_pause）** 拦下；
2. 我们此前的 kill 用 EXIT 直接结束 warp，但驱动在此之前已经
   `do_preempt_save` 保存了 CILP 现场；被杀的 warp 再也无法 `do_preempt_restore`，
   **CILP 现场变成孤儿** → 之后的新内核一旦被 0x8 拦下就没人能恢复，表现为
   "跑几毫秒就停 + 偶发 700"；
3. 正确的语义应是 **park + restore 同一批 warp**（reason 0x8 ↔ bit0），
   而不是 EXIT；这也解释了为什么 cuda-gdb 的暂停/恢复能工作、而 RM 0317+0318
   在我们的 kill 之后无效。

**下一轮的第一件事**：用探针把 `reason` 的完整序列记下来（每次 trap 追加到
`buf+0x100+16*N`），确认新 launch 的 0x8 是否在 kill 之后必然出现；然后尝试
"不 kill、只 park（清 R4/killable 让 do_pause 生效）+ 0318 restore"的组合。

### 19. 对照实验：(a) 裸 CUDA 应用 RM 0317 确实进入 handler；(b) 无插桩的受管 launch 反而更差

**(a) l3x + 签名 cave + 原始 RM 0317（无 cuda-gdb）**（`exp_sig_rm0317.log`）：

```
[pol] compute[0]=0x5c00001b ... [8256256->11794944->19170816 running]
      susp 0  hold 57771008->94544896 (running)  self-resume 0 (RESUMED)
[pol] trap-handler signature observed: 0x5aa5/0 reason=0x1 <<<
```

⇒ **RM 0317 在没有调试器时确实把 trap 送进了活体 handler**（签名落盘、reason=0x1），
且签名 cave 正常返回后内核继续运行（hold 期间计数仍在涨）。
这条和 §18 的"managed 路径上探针为空"形成对照：**trap 投递本身没问题，问题在
XSched 受管/插桩路径的上下文状态**。

**(b) 受管 launch + `XSCHED_SM120_TRAP_NO_INSTR`（不插桩）**（`exp_noinstr_managed1.log`）：
0317 既不返回也不报错，队列的 `Synchronize` 等 30 s 后 700；`remembered kernel cmd` 正常。
说明"插桩（debugger params）"其实是 0317 被驱动接受的前提之一，去掉它反而更糟。

**综合判断（据此更新 §17/§18 的口径）**
1. trap 投递（RM 0317 → handler）在普通上下文可用、无需 gdb；
2. XSched 的插桩受管上下文里，0317 表现为"context 级 suspend"（探针未进入），
   resume 会丢失在飞工作，且之后新 launch 会遇到 reason=0x8（do_pause）而停住；
3. 因此"kill+replay"要么需要恢复驱动的 CILP 现场（调试器 agent），要么绕开
   插桩上下文（但绕开后又拿不到 0317 的受理）。

**下一轮的两个候选实验**
- A：在受管上下文里 **先清掉 killable/params**（`XSCHED_SM120_TRAP_NO_INSTR` 但保留
  一次 `cuXtraSetDebuggerParams`），验证"有 params、无 guardian"是否是 0317 受理的最小条件；
- B：把 §18 的 reason 探针扩展到记录序列（用 `wr_buf+0x100+16*N` 环形），确认 0x8 的
  来源是"新 launch 的 stop"还是"0318 的 restore 请求"。

### 20. 尝试"命令字 park/restore"（sm70/86 式语义）：仍然不能原地恢复

实现（env `XSCHED_SM120_TRAP_CMD_WORD=1`）：
- `InstrumentManager::Deactivate()` 在写 global_exit_flag 之外，再写
  `preempt_buf+4 = 2`（preempt_save）；
- Resume 时在 `Reactivate()` 里写 `preempt_buf+4 = 1`（preempt_restore），
  然后再发一次 `0317` 触发 trap（`TrapSm120Context::Trigger()`）。

实测（`exp_cmdword1.log`，无 payload，插桩受管 launch，cuda-gdb 在场）：

| 阶段 | 结果 |
|---|---|
| suspend | 27.4 ms，计数冻结 22,289,152 |
| hold | 冻结 |
| resume（cmd=1 + 0317） | `trap reactivate: command word 1, sending the restore trap`；**计数仍不动** |
| final sync | cudaSuccess，`done=0`（内核未完成） |

⇒ 即使给出 `do_preempt_restore` 的 reason，被停住的 warp 也没有回来。结合 §19(a)
（普通上下文里 0317 只产生一次 benign trap、内核继续跑），可以判断：
**在 XSched 受管上下文里，0317 走的是"上下文级 suspend"，没有把 do_pause 作用到
运行中的 warp 上；因此既无法 park、也无法 restore。** 真正的 warp 级 park/restore
仍然只由调试器 agent（cuda-gdb 的 NvDebugAgent）完成。

**至此，RM 侧的所有组合都已穷尽**：0317{0,0}/{1,0}、0318、0317+0318、resume pump、
cmd word 2/1、free-after-resume、原入口重放、全新 launch、NO_PAYLOAD/NO_INSTR/
NO_DEACT——都不能让"trap 停止的内核"在 context 内继续。

**下一阶段唯一入口**：实现 target 内 `NvDebugAgent` 协议（样本
`evidence/live/gdb_agent_protocol.log`）。这也是 v2.4 以来反复收敛到的同一结论。

### 21. 把 dispatch 换成"良性返回"后：resume 会重启内核，但新实例仍 ~3 ms 后 700

`XSCHED_SM120_TRAP_CAVE=1` + `PRE_RETURN` + `KEEP_RET`（Init 时装好 0x390 调用点 +
返回桩，之后不再改代码），Deactivate 开、payload 不装、0317 由 agent 发
（`exp_patched_0317.log`）：

| 阶段 | 结果 |
|---|---|
| suspend | 29.9 ms，计数冻结 22,369,536 |
| resume | 观察到**新内核实例**（计数从 0 涨到 113,664） |
| 之后 | ~3 ms 后 context **700**，`done=0` |

⇒ 与 §11/§14 的现象一致（replay/新内核能起来但立刻 fault）。
即便 dispatch 被换成最良性的返回，恢复后仍不稳。

**给接手的判断**：RM 侧能把内核"停住"（27–30 ms、计数冻结、context 暂时干净），
但"停住 → 恢复并继续"这条链在 595.99.02 上必须由调试器 agent 完成；
现有所有组合（§15–§21）都无法替代。

### 22. 复现脚本 `run_l3qspin.sh`（本轮产出）

把受管内核的四类实验收敛成一条命令（日志自动落到 `evidence/live/qspin_<mode>_*.log`）：

```bash
./run_l3qspin.sh suspend   # trap 停内核：~27-30 ms，计数冻结（已验证）
./run_l3qspin.sh kill      # kill + 重放
./run_l3qspin.sh probe     # 记录 trap reason / c[0x170]
./run_l3qspin.sh noinstr   # 不做插桩的受管 launch
```

`suspend` 实测输出（本次）：suspend ~27 ms、hold 期间计数不动、resume 时队列重放内核
（`replaying the killed kernel`），新实例跑 **421,632 次（≈12 ms）后停住**（done=0，
sync 成功）。每次运行停住的计数不同（80k/113k/225k/421k…）⇒ 像是被某个**事件**
（调试器/驱动的 stop）在随机位置拦下，而不是内核自身结束。

**这个"随机位置被拦下"是下一轮最值得查的点**：如果确认是 gdb 在拦，那么
"attach 只用于 0317 应答、随后 detach 让内核自由跑"就可能闭环；
实现方式是让 agent 驱动 cuda-gdb 的 MI（`-target-attach` → `-exec-continue`/detach），
也就是复用 `probe/l3mi.cpp` 的 MI 通道。

### 23. 最终定位：受管 context 上 `0317` 是"取消在飞工作"，不是"暂停"

`NO_INSTR` + `NO_PAYLOAD` + 良性 dispatch cave + `0317`（`exp_noinstr_cave.log`）：

- suspend **30.2 ms** 返回，计数冻结 22,388,992；
- 但此时内核本应再跑 1.6 s，`kHwQueue->Synchronize()`（`kQueueSuspendFlagSyncHwQueue`）
  却在 30 ms 内就返回 ⇒ **stream 被判定为完成**，即 in-flight 工作被**取消**，
  而不是被挂起；
- resume 后队列重放内核（新实例 81,408）→ 停住，`done=0`。

与 l3x 的对照（§19）：同样的 0317、同样的 handler patch，在**普通 cudart 上下文**里
只产生一次 benign trap、内核继续跑（签名落盘、计数继续增长）；
在**经 XSched 队列/InstrumentContext 创建过的上下文**里则是"取消在飞工作"。
两者的差别不在 kernel 参数、不在 handler patch、不在 session，只剩下
**InstrumentContext 在 Init 阶段对上下文的额外操作**（`cuXtraInstrMemcpyHtoD` 写
resume 指令块、`InstrMemAllocator`、OpStream 等）。

**这给出了非常具体的下一步**（不需要调试器协议）：
1. 二分 InstrumentContext 构造阶段的每个 cuXtra 操作（`cuXtraInstrMemcpyHtoD`、
   `InstrMemAllocator`、OpStream 创建），找出哪一个让 0317 从"trap"变成"取消"；
2. 若找到，就在 sm120 trap backend 里避免该操作，让受管上下文的 0317 回到 l3x 的
   行为（trap 可被 handler 处理、内核不被打断），再用 armed cave（EXIT）实现 kill、
   用队列 replay 实现 resume。

### 24. 收尾：最小 InstrumentContext 与 `0317{0,0}` 都不能改变结局

| 实验 | 结果 |
|---|---|
| `XSCHED_SM120_TRAP_MIN_CTX=1`（不建 instr_mem_/preempt_buf_/resume 指令块）+ NO_INSTR + 良性 cave + NO_DEACT | suspend 27 ms 冻结；resume 后新实例 155,648 次即停，**700** |
| `0317{0,0}`（poll，不 hold）+ auto `0318` + NO_PAYLOAD + NO_DEACT | suspend 27 ms 冻结；resume 的 `Synchronize` **700** |

⇒ §23 的推断（"InstrumentContext 的额外 cuXtra 操作"）**不成立**：最小上下文下行为不变。
受管上下文里 0317 的行为差异既不来自 InstrumentContext、也不来自 handler patch、
也不来自 kernel params，只能来自 **queue/launch-worker/shim 这一层**
（XSched 的 launch 路径）或驱动对该 client 的调试态判定。

**已确认可用的能力（截至本版）**
1. `run_l3kill.sh`：普通内核 8 s → 0.234 s、`cudaSuccess`；
2. `run_l3qspin.sh suspend`：受管内核 **27–30 ms** 停住（计数冻结、context 短暂干净）；
3. `run_l3hal.sh`：GDB backend 生产 L3（suspend 后高优先级内核 0.74 s 完成，Resume=1）；
4. 受管 launch（shim `cuLaunchKernel` → XQueue::Submit）、被杀内核重放、handler payload
   装机、lazy 会话、0xd04 晚授权——全部可复现。

**未完成**：受管内核在 0317 之后无法在 context 内恢复执行；所有 RM 侧组合已穷尽。
下一步唯一入口仍是调试器 agent（NvDebugAgent / 驱动侧 cuda-gdb MI），或者找到
XSched launch 路径里触发"调试态判定"的那一步。

### 25. 为下一阶段取到的实物：NvDebugAgent 是薄启动器，协议在 stripped 的 libcudadebugger 里

抓取方法（本轮已验证）：

```bash
L3_ALLOW_PTRACE=1 ./bin/shm_spin /agcap 6 &
cuda-gdb -batch -p <pid> -ex "shell sleep 6" -ex detach &
sleep 3; cp /tmp/<gdbpid>/session1/NvDebugAgent /tmp/agcap/
```

结果：
- `NvDebugAgent`（25,272 B，带 DWARF、不 strip）只有 `main`，逻辑全在
  `libcudadebugger.so` 的 `DebugAgentMain`（源路径
  `.../SassDebugger/Modules/Injection/Cuda/Src/DebugAgentProcess.c`）；
- `libcudadebugger.so.595.99.02`（11.7 MB）**没有任何符号表**（`nm` 0 行），
  协议实现只能靠反汇编 + 抓包对照。

样本已存档：`evidence/bin/NvDebugAgent`。

⇒ 实现 gdb 侧 RPC 的工作量与风险都很高（要逆 11.7 MB stripped 库）；
在有限时间内更现实的两条路：
1. **驱动 cuda-gdb 的 MI**（`probe/l3mi.cpp` 已可用）：把"attach/continue"作为
   resume 手段，和本项目的 RM trap 组合；
2. 继续在 XSched launch 路径里找"让 0317 从 trap 变取消"的那一步（§24 之后仅剩此处）。

### 26. 根因二分成功：`InterruptContext` 的 InstrMemAllocator 决定 0317 的语义

| 配置（受管 launch，cuda-gdb 仅作应答） | 0317 的行为 | 证据 |
|---|---|---|
| 默认（InterruptContext 分配 instr mem） | **取消在飞工作**：suspend 27 ms 返回、计数冻结、stream 判完成 | §15–§24 |
| `XSCHED_SM120_TRAP_MIN_CTX=1`（同时跳过 InstrumentContext 的 instr/preempt 缓冲） | **无操作**：suspend 1665 ms（等内核自然跑完），resume 后重放的新内核**健康运行**（计数 7M→43M） | `exp_minctx2.log` |
| `XSCHED_SM120_TRAP_NO_ICTX_INSTRMEM=1`（只跳过 InterruptContext 的 instr mem，**保留插桩**） | 同上：**无操作** + 重放健康 | `exp_no_ictx_instrmem.log` |
| 同上 + 预置"按 preempt buffer 标志 EXIT"的 armed cave + Deactivate | 仍无操作（suspend 2117 ms，内核跑完） | `exp_flagkill_healthy1.log` |
| 同上 + 按 reason bit0 的 armed cave（`REASON_ARM`） | **启动时就被杀**（计数 98k）——launch trap 的 reason 也含 bit0，无法区分 | `exp_reasonarm1.log` |

⇒ 两个硬结论：
1. **`InterruptContext` 里的 `InstrMemAllocator`（cuXtraInstrMemBlockAlloc）是"0317 从
   trap 变取消"的开关**；只要不分配它，受管上下文就回到"0317 不打断内核"的状态。
2. 但此时 **0317 对受管上下文是 no-op**（没有 trap 送给 handler，armed cave 也不会被
   执行），而 l3x 的普通 cudart 上下文里 0317 确实产生 trap（签名/`reason=0x1`）。
   所以"XSched 队列创建的上下文"与"普通上下文"在驱动的 trap 投递路径上仍有一个
   未定位的差异——它不在 InstrumentContext（已被二分排除），也不在 instr-mem 分配
   （已二分），最可能在 **XQueue/launch-worker 的启动线程/上下文切换方式**上。

**可复现**：`XSCHED_SM120_TRAP_NO_ICTX_INSTRMEM=1` 时，
`run_l3qspin.sh` 的 suspend 变成"等内核自然结束"，resume 的重放内核全程健康——
这本身就是一个可用的"**kill 前/后 context 健康**"的基线，可用于验证任何新的
trap-投递方案（判据：suspend 是否 < 50 ms 且计数在 hold 期间不动）。

### 27. 收敛结论：595.99.02 上 0317 只有"取消"或"无操作"两种结局

| 上下文状态 | 0317 的效果 | 能否恢复 |
|---|---|---|
| 普通 cudart 上下文（l3x，无 XSched 队列） | **trap 送进 handler**（签名/`reason=0x1`），内核继续 | 不需要恢复（未被停） |
| 受管上下文 + `InstrMemAllocator` | **取消在飞工作**（stream 判完成） | 不能（工作已丢） |
| 受管上下文 + `NO_ICTX_INSTRMEM` | **no-op**（status 0，内核不被停） | 不需要（未停） |

⇒ 在 595.99.02 上，**RM 侧不存在"停住且可恢复"的状态**：
"停住"必然伴随工作被取消，而"不取消"时 0317 对受管上下文无效。
cuda-gdb 能把抑制在"park"而不是"cancel"，是因为它的 agent 在 target 内做了
attach 注入（`cudbgApiInit/Attach` + 与调试器的 fd 握手，§25），把上下文切进
debugger-managed 执行态——这是 RM 命令无法替代的一步。

**这就是本项目在 RM 路线上的终点**（所有组合已穷尽，机理已二分到具体分配器）。
真正要闭环，只能实现调试器 agent 协议，或让驱动提供"非调试器 park"开关。

### 28. 关键对照：与"怎么 launch"无关，是 **InterruptContext 把 context 标成了调试器上下文**

在 `l3qspin` 里加 `L3Q_DIRECT=1`：用 `dlopen("libcuda.so.1")+dlsym("cuLaunchKernel")`
**绕过 shim/XQueue** 直接提交内核（XQueue 仍然存在）。

| 配置 | suspend | 结果 |
|---|---|---|
| 默认（有 `InstrMemAllocator`）+ 直启 | **28.4 ms** | 计数冻结、`done=0`、stream 判完成 ⇒ **取消** |
| `NO_ICTX_INSTRMEM` + 直启（`exp_direct_launch1.log`） | 2116 ms | 内核自然跑完（`done=1`）⇒ **0317 无操作** |

⇒ **同一份"直启"内核，只因为 `InterruptContext` 是否分配 instr-mem 块，0317 的语义
就从"取消"变成"无操作"**。这与 launch 走哪条路径无关，说明：
1. `InstrMemAllocator`（`cuXtraInstrMemBlockAlloc`）是上下文进入"调试器管理态"的开关；
2. 一旦进入该状态，0317 会**取消在飞工作**而不是 park；不进入该状态，0317 对
   XSched 上下文**完全无效**（而 l3x 的纯 cudart 上下文里 0317 是有效 trap）。

⇒ 三种上下文、三种 0317 语义，已用同一套环境变量反复复现。
**这意味着 RM 路线无法同时得到"停住"与"可恢复"**（§27 的结论得到加强）。

### 29. 新发现：一次"预 attach"之后，RM 0317 从"取消"变成"park"

做法：`L3Q_ALLOW_PTRACE=1 L3Q_START_DELAY=5` 启动 l3qspin，在它创建 XQueue 之前用
`l3mi --pid <pid> --cycles 1 --hold-ms 300` 做一次 **attach→detach**（0.057 s 冻结 /
0.005 s 恢复），之后再走原来的 RM 0317{1,0}。

结果（`exp_pre_attach1/2.log`）：

| 观察 | 含义 |
|---|---|
| 计数在 0317 之后冻结（22.2 M） | 内核**没有继续跑** |
| 队列的 `Synchronize()` **不返回**、应用一直挂着 | 内核**没有被取消**（被取消时 stream 会判完成、sync 会返回） |
| agent 的 `0317{1,0}` 一直阻塞 | 没有调试器应答 |

⇒ 这次是真正的 **park**（停住且工作仍在），而不是 cancel。
**一次提前的 attach/detach 让驱动把上下文切到了"可 park"状态**；此后纯 RM 的 0317
就能停住内核。

**但释放（resume）仍然卡住**：
- agent 的 0317 阻塞在它自己的 client 上，`0318` 无法插进去（RM 按 client 串行）；
- 再起一次 `l3mi` 去 attach **会挂住**（对已 park 的上下文 attach 不返回，rc=124），
  所以"用 gdb detach 释放 park"这条路也不通。

**这解释了 §27–§28 的全部分歧**：attach 注入改变的正是"suspend 是 park 还是 cancel"。
下一步要试的只有两个方向（都需要 target 侧能发出 resume）：
1. 让 **target 自己的 session** 发 `0318`（不同 client，不受 agent 的 client 锁影响；
   需要验证跨 session 是否有效）；
2. 在 agent 里用**独立 client/线程**发 0318，或改用 `0317{0,0}` 以避免阻塞式 0317。

### 30. 自 session 发 0318 也被阻塞 ⇒ park 无法从应用侧释放

给 `TrapSm120Context` 加 `XSCHED_SM120_TRAP_SELF_RESUME_MS`：Init 时起一个线程，
到点用 **target 自己的 session**（`0x5c000073`，与 agent 不同 client）发 `0318`。

实测（`exp_self_resume1.log`，预 attach + park 场景）：
- 线程按时启动（`self-resume thread armed (11000 ms, session=0x5c000073)`）；
- 但 `0318` 的 ioctl **一直阻塞**，从未打印返回值；
- 内核保持 park（计数 22,185,472 不动），应用挂死。

⇒ 在 debugger-suspend 状态下，RM 控制命令（至少 0318）在 **任何 client**（app 自己的
或 agent 的）上都会被阻塞。park 的释放只能由驱动/调试器 agent 完成。

**RM 路线的最终结论（第 29/30 条合并）**
- 一次提前的 attach 能让上下文进入"可 park"状态，RM 0317 可以停住内核且不丢工作；
- 但没有任何用户态路径能释放这个 park（0318 在 agent client 和 app client 上都被阻塞，
  对已 park 的上下文再 attach 也会挂死）。
- 因此 **"停住 + 恢复"在 595.99.02 上只能由调试器 agent 完成**；这是本项目的
  RM 路线终点，也是全部 v2.x–v4.x 实验收敛到的同一结论。

### 31. PTRACE_DETACH 也不能释放 park（前提已不成立）

为了验证"ptrace 关系是否能解锁 park"，给 agent 加了 `L3AGENT_DETACH_AFTER_MS`。
实测（`exp_ptrace_detach1.log`）：

- 预 attach 由 `l3mi` 完成（它成为唯一 tracer），**我们 agent 的 ptrace 关系被顶掉**；
- 到点 `PTRACE_DETACH -> -1 (ESRCH)`，目标仍活着；
- 内核保持 park，应用挂死。

⇒ 该路线不成立：park 能力来自 **gdb 的 attach**（不是普通 ptrace attach），
而 gdb 对已 park 的上下文无法再次 attach（§29），也就无法 detach 释放。

**最终结论（定稿）**：sm120 + 595.99.02 上，
"停住运行中内核且随后可恢复"这一功能**当前只能由调试器（cuda-gdb/GdbInterruptContext）
提供**；RM 0x83de 路线可以实现 27–30 ms 的停住（cancel 或 park），但没有任何
用户态手段释放 park。本项目在 RM 路线上的工作到此为止，全部证据与开关已落盘。

### 32. 决定性问题定位：受管上下文里"trap 能进 handler"，但**handler 无法区分这是不是抢占请求**

本轮把 §26 之后剩下的变量全部试完，得到一组决定性数据：

| 配置（受管 launch + 无 instr-mem + 无 payload + cave） | 0317 结果 | 证据 |
|---|---|---|
| `NO_INSTR`（无 debugger params） | **trap 进 handler**，探针记到 `reason=0x1` | `exp_signature_managed.log` |
| `PLAIN_LAUNCH`（有 params、原始入口、无 guardian，无 instr-mem） | **trap 进 handler**，`reason=0x1`，`c[0x170]=0x42000000`（合法指针） | `exp_probe_plainlaunch.log` |
| 同上 + 预置"按 reason bit0 EXIT"的 cave | **启动时就被杀**（计数 230k）——launch trap 的 reason 也是 0x1 | `exp_killreplay_plainish1.log` |
| 同上 + 预置"按 preempt buffer 标志 EXIT"的 cave + Deactivate | 内核跑完（没杀）——trap 上下文里 `LD.E` 读标志得到 0 | `exp_plainlaunch_flag1.log` |
| 同上 + 在 trap 里真的 `LD.E` 读 preempt buffer（flag=0） | 读回 0、不 fault、内核跑完 | `exp_flag_probe1.log` |
| 同上但 flag=1（Deactivate 开） | **700**：在 trap 上下文读该 buffer 会 fault | `exp_flag_probe2.log` |
| 同上 + 命令字 `preempt_buf+4 = 2`，读 reason | 仍是 **0x1**（驱动没有把 0x10/0x20 加进 reason） | `exp_cmdword_reason1.log` |
| 运行期 arm（Trigger 里写 cave） | **阻塞**到内核结束（`arm write cave start` 之后无 done） | `exp_runtime_arm1.log` |

**结论**：在 sm120/595 上，
1. 带内触发（`0x419e84`）无效（v1/v2 已证）；
2. RM `0317` 这个"带外替代"**能把 trap 送进 handler**（前提：不要有 instr-mem/guardian 那套调试态），
   而且此时**重放内核能健康运行**（计数 7M→44M）；
3. 但 handler 里**没有任何可用判据**区分"抢占请求"与"启动时的 trap"：
   - reason 都是 `0x1`（命令字=2 也不变）；
   - 全局内存读（preempt buffer）在 trap 上下文里要么返回 0、要么 fault（flag=1 时 700）；
   - 预置 cave 会被启动 trap 命中；运行期改 cave 又被驱动串行化。

⇒ 论文 Lv3 的"handler 读 flag/按 reason 分派"**在 sm120 上恰好两样都不可用**。
这是无法复现论文方法的**唯一根因**，也是我们所有失败现象的统一解释。

### 33. 迟延实测：真正"停住"只要 ~0.2 ms，23–30 ms 全是"停住之后"的收尾

给 `l3qspin` 加了 `L3Q_TIMELINE`：主线程在调用 `XQueueSuspend` 前记时刻，
另起一个线程以 ~50 µs 粒度忙采样 host-mapped 计数器，检测"计数停止变化"的时刻。

| 配置 | req→计数冻结 | req→Suspend 返回 |
|---|---|---|
| 懒建会话 + auto-0318（第一次触发） | **4.72 ms**（含会话创建 + shm 轮询 + 0317） | **27.3 ms** |
| 会话预建（EAGER）+ 不 auto-0318 | **0.20 ms**（≈200 µs，50 µs 阈值） | **22.9 ms** |

证据：`exp_timeline1.log`、`exp_timeline2.log`、`exp_timeline3.log`。

**结论（关键）**
1. **设备侧停住 ≈ 0.2 ms**：从调用 `XQueueSuspend` 到内核计数真正冻结，只用了约 200 µs
   （含 agent 的 shm 轮询、`0317` ioctl 往返、驱动 suspend、trap）。
   这和论文 GV100 的 32 µs 只差 ~6×，是同一个数量级（亚毫秒）。
2. **23–30 ms 的其余部分发生在"设备已经停住之后"**：
   `kQueueSuspendFlagSyncHwQueue` → `cuStreamSynchronize` 要等**驱动把被取消的内核
   异步清理完、stream 才被标记完成**；此外首次触发还有 ~4.5 ms 的会话创建。
3. 论文之所以能做到 32 µs 的"端到端"，是因为它的 abort 由 trap handler 里的
   `EXIT` 完成——warp 退出 = stream 立刻完成，没有驱动侧 20 ms 级的 teardown。
   我们现在的"停"是驱动级 cancel，所以"停"快了，"收尾"慢了。

⇒ 想接近论文的端到端延迟，需要同时做到：
(a) trap handler 能在 `0317` 时 `EXIT`（当前缺判据，§32）；或
(b) 不等驱动的 teardown（例如 suspend 不要求 stream 完成），代价是语义要重新定义。

### 34. cave 代码的硬约束：必须保留 R1；驱动 trap 区域不能当通道

**（a）R1 必须保留**：handler 在 `+0x3a0` 紧接 cave 返回之后执行 `STL.64 [R1], R2`。
我们的 table-probe 一开始用 `MOV R0/R1` 装地址，把 **R1 覆盖成目标地址**，
结果 `STL.64` 写到非法局部地址 → **700**。改用 `R6/R7` 装地址、`R10` 装读回值后，
同样的探针不再 fault。⇒ 写 cave 时的寄存器白名单：可自由用 `R4-R11`，
**`R0-R3` 与 `R12` 要谨慎，`R1` 绝对不能用**。

**（b）驱动 trap 区域不能当 host→handler 通道**：
- `trap_reason_table`（`base+0xc38300`）host 侧可读可写（写入 `w=8`、回读一致），
  但**写进去会破坏驱动自己的 trap 逻辑**，下一次 trap 时 context 直接 **700**；
- 只读对比：host 读该区域得 0，cave 里读也得不到可用的非零值（buffer 全 0 / fault）；
- 结论：这些区域是驱动私有的，**不能**拿来传递调度请求。

**（c）结论：trap 上下文的"输入通道"目前只确认了常量库**
（`c[0x170..0x188]`，launch 时固定）；普通全局内存"写可以、读不行"；
local 可用但 per-thread 私有。要复刻论文的"请求位"，必须找到新的可读映射
（或恢复带内触发），否则判据缺失的问题无解。

### 35. 全局内存读的定论：不是 cache、不是分配方式、不是 0/非0——trap 上下文的**数据读通路整体不通**

为了排除"写没 flush / 没 malloc / 只对别人的缓冲不可见 / 0 与非 0 有区别"，用 sentinel
（先写 `0xdeadbeefcafef00d`，cave 执行后低 32 位被覆盖、高 32 位保持，可据此**证明 cave 真的执行了**）
做了四组读实验，全部使用**正确的 64 位地址**：

| # | 读的对象 | 准备方式 | 结果 |
|---|---|---|---|
| 1 | 我们自己的 `cuMemAlloc` 缓冲（`wr_buf+0x40`） | host `cuXtraMemcpyHtoD` 写 magic，host 回读一致 | cave 执行（高 32 位 sentinel 保留），**读到 0** |
| 2 | XSched 的 preempt buffer（`c[0x170]:c[0x174]`，Deactivate 写 1） | 修正了探针 bug（旧版只 LDC 低 32 位） | **读到 0** |
| 3 | **host-mapped UVA**（`cuMemHostAlloc(DEVICEMAP)`，host VA==device VA） | host 直接写 magic | cave 执行（sentinel 保留），**读到 0** |
| 4 | handler 自己的代码页（`live+0x390`） | 内容已知（我们写的 CALL） | **fault（700）** |
| 5 | 描述符式 `LDG.E`（编译器生成的 `LDCU UR4,c[0x358]` + `LDG.E.64 desc[UR4]`） | — | **fault（700）**（描述符在 trap 上下文无效） |

**结论（本轮最硬的一条）**
1. **写**：trap 上下文可以写全局内存，且写到正确的 VA（host 回读一致）——所以"记录谁被 abort"这半条通道是通的；
2. **读**：无论设备内存、XSched 的缓冲、host-mapped UVA，**读回都是 0**；读代码页/带描述符的 LDG 直接 fault；
3. 这与 cache flush、malloc、缓冲区归属、0/非 0 都无关——**trap 上下文的数据读通路不工作**；
4. 推论：sm120 的 debugger-suspend 路径（我们唯一能触发的 trap）**不使用、也不支持**全局数据读；
   NVIDIA 自己的 handler 在这条路径上只依赖特殊寄存器（这也解释了为什么我们的 `reason` 能从
   `SR_GLOBALERRORSTATUS` 算出来、而 preempt buffer 的分派路径从不生效）。
5. 额外的探针 bug（已修）：读 64 位指针必须 `LDC` 低/高两半（`c[0x170]`+`c[0x174]`），
   旧版只读低半、用 `R0:R1` 组地址，结论不可信；修正后用 #1/#3 独立复核了"读为 0"。

### 36. 挖出 trap handler 的"对外交流"实现：它读的是**驱动 patch 进去的全局 preemption buffer**（不是我们的 buffer）

把活体 `do_read_trap_reason`（`base+0x15f2100`）dump 出来、扫描所有 `MOV Rn, imm32`
（`XSCHED_SM120_TRAP_DUMP_READ=1`），恢复出驱动 patch 进去的两个 64 位地址：

| 位置 | 地址 | 含义 |
|---|---|---|
| `do_read+0x90/0xa0`（及 0x340/0x350、0x440/0x450） | `0x7ecc33a00000` = **base + 0x1a00000** | scratchpad（`LD.E.U8.CONSTANT [R4+0x29d]`） |
| `do_read+0x120/0x130` | `0x7ecc30000000` = **base − 0x2000000** | **驱动的 preemption buffer**：`LD.E.CONSTANT [R4+0x4]` 读命令字，`[R4+8]` 读 preempt_idx |
| `do_read+0x150` | `0x630b0` | per-SM 步长（与 vsmid 相关） |
| `do_read+0x380` | `0x290` | 偏移 |

本轮实测的 base = `0x7ffdde000000`，于是驱动 preemption buffer = `0x7ffddc000000`。

**关键结论**
1. handler 的读全部是 **`LD.E.CONSTANT`**（常数/非相干路径），不是普通 `LD.E`；
2. handler 读的**不是** debugger param `c[0x170]` 指向的 XSched buffer，而是**驱动自己 patch 的全局 buffer**；
   这解释了为什么 XSched 的 `Deactivate`（写自己的 buffer）在 sm120 上对 handler 完全无效、
   reason 永远是 `0x1`（SR 位），命令字位 `0x10/0x20` 永远不出现；
3. 我们尝试从 host 写该 buffer（`cuXtraMemcpyHtoD(buf+4, shellcode)`）→ **`cuda error 1 (invalid argument)`**：
   该地址不属于任何可被 host 寻址的 memobj（trap 上下文专用映射）。
4. 之前"命令字=2 无效"的真正原因也清楚了：**写错了 buffer**。

**下一步（唯一缺口）**：找到能写 `base-0x2000000` 的通道——候选：
- 设备侧 kernel store（该 VA 在 trap 上下文可读，若在用户上下文也可写就成）；
- 驱动的 tools memobj / cuXtra 指令内存 API 是否能覆盖该区域；
- RM 是否有"设置 preemption 命令"的控制（`0x83de` 族或 registry 键）。

### 37. 驱动 preemption buffer 的写路径：cuXtra 同步写（仅 Init）可用，运行期/异步不可用

| 方式 | 时机 | 结果 |
|---|---|---|
| `cuXtraMemcpyHtoD(drv_pbuf+4, ...)` | **Init（无内核运行）** | ✅ `w=4`，回读 `0x2` |
| 同上 | 运行期（Trigger，内核在跑） | ❌ `cuda error 1 (invalid argument)`（cuxtra `memcpy.cpp:212`） |
| `cuMemcpyHtoDAsync_v2_ptsz(drv_pbuf+4, ...)` + StreamQuery | 运行期 | ❌ **libcuda 内 SIGSEGV**（该 VA 不是 CUDA 分配，异步路径不做同样校验） |
| `CuMemcpyDtoH_v2(drv_pbuf+4)`（CUDA API） | Init | ❌ `r=1 invalid value` |

⇒ 这个地址虽然"在 Init 时能被 cuxtra 同步写"，但它不属于 CUDA 的分配体系；运行期
写入会被拒绝甚至崩溃。**写它必须走别的通道**：
1. **设备侧 kernel store**（该 VA 在设备/trap 上下文可读，用户上下文是否可写待测）；
2. 驱动 tools-memobj 的 API（cuXtra 指令内存那套）是否覆盖 `base-0x2000000`；
3. RM 是否有设置 preemption 命令字的控制（0x83de / registry）。

**意义**：这是目前离"复刻论文"最近的一步——handler 的输入地址、读法（`LD.E.CONSTANT`）、
命令字语义（+4: 2=save/pause，1=restore；+0: exit flag；+8: preempt_idx）都已确认，
只差一个可用的写通道。

### 38. 驱动 preemption buffer 只在 trap 地址空间：设备侧写也非法

用已有的 `l3write`（PTX，1 block × 1 thread，从 `wr_buf+0x1c0` 拷 4 字节到目标 VA）
在 Trigger 时写 `drv_preempt_buf+4`：

```
CUDA Exception: Warp Illegal Address
The exception was triggered at PC 0x7ffddf6f4180
0x00007ffddf6f41a0 in l3write<<<(1,1,1),(1,1,1)>>> ()
```

⇒ **该地址在普通设备上下文里没有映射**（只有 trap 上下文/驱动自己能访问）。
结合 §37：

| 写方式 | 结果 | 说明 |
|---|---|---|
| cuXtra 同步 memcpy（Init） | "成功"（w=4，cuxtra 回读 0x2） | 只能经 cuxtra 自己的访问路径；不能证明对 trap 有影响 |
| cuXtra 同步 memcpy（运行期） | invalid argument | cuxtra 内部检查失败 |
| CUDA 异步 memcpy（运行期） | libcuda SIGSEGV | 非 CUDA 分配 |
| **设备 kernel store** | **Warp Illegal Address** | 用户上下文未映射 |

**修正与定论**：
1. 这块 buffer 属于**驱动的 trap 专用地址空间**，用户态（host 与 kernel）都无法直接写；
2. 能写它的只有**驱动自己**——而这正好解释了 §29 的观察：**预 attach 后 `0317` 能把内核
   park**（驱动在 debugger-suspend 路径里写了命令字、handler 做了 save/pause）；
   但 resume 需要驱动在 `0318` 时写回 1，而这一步被"必须由调试器 agent 驱动"的
   状态机卡住（§30：`0318` 在任何 client 上都阻塞）。
3. 因此，**"模仿 handler 的信息交流方式"这一层已经被我们完全复刻并打通到了边界**：
   地址、读法（`LD.E.CONSTANT`）、布局（+0/+4/+8）都确认；剩下的写入口属于驱动内部，
   只能通过调试器协议（NvDebugAgent）或驱动自身的 suspend/resume 状态机获得。

### 39. handler 输入的写侧定论：驱动 buffer 只读（trap 上下文读✅ / 写❌）

在 §38 基础上做了两组同一 trap 内的对比（都用 **launch trap 也会命中** 的 cave，因此
任何 fault 都会在第一秒暴露）：

| cave 内容 | 结果 |
|---|---|
| 只读 `drv_pbuf+4`（`LD.E.CONSTANT`，§38 `exp_drvbuf_4.log`） | 正常，多次 trap 无 fault，读回 0 |
| 读 + **写** `drv_pbuf+4`（`ST.E [R6],R2`，`exp_rw_2cycles.log` / `exp_drvbuf_rw*.log`） | **启动后 0.5 s 内 700**（launch trap 一命中就 fault） |

写侧其它通道（§37/§38）：
- host cuXtra 同步写：仅 Init "成功"，运行期 invalid；
- host CUDA 异步写：libcuda SIGSEGV；CUDA DtoH：invalid；
- 设备 kernel store：Warp Illegal Address。

⇒ **结论（定稿）**：
1. trap 上下文对驱动 preemption buffer 是 **只读映射**（`.CONSTANT` 读可用、`ST` 即 fault）；
2. 因此 handler 的输入（exit flag / 命令字 / preempt_idx）**只能由驱动自己写**；
3. 用户态（host 与 kernel）对该地址完全无访问能力；
4. 这也最终解释了 §29–§30：预 attach 后 0317 能 park（驱动写了命令字、handler save/pause），
   而 resume 需要驱动再写 1 并触发 restore —— 这一步只能由调试器侧（NvDebugAgent/gdb 的
   continue）驱动，RM 的 0318 在无调试器时不会走完这条状态机。

**至此，"handler 与外界的信息交流"已被完整逆向并探测到边界**：
- 读侧接口：`base−0x2000000`（只读）+ `base+0x1a00000` scratchpad；
- 写侧接口：只有驱动；
- 用户态能做的：注入 handler 代码、在 handler 里写用户内存/读常量与 SR、以及触发 0317。
  缺少的那一半（写命令字）在任何用户态路径上都不可达——这是平台约束，不是实现遗漏。

### 40. 驱动侧入口补测：`0301 SMDebugModeEnable` 可接受但不改变 park/cancel

| 调用 | 结果 |
|---|---|
| `0x83de0301`，psize=0 | **status 0**（接受） |
| `0x83de0301`，psize=0x48（零填充） | `0x1f NV_ERR_INVALID_ARGUMENT` |
| 0301 成功后再 `0317{1,0}`（默认 instr-mem 配置） | suspend **27.4 ms**（仍是 cancel），随后 700 |

⇒ `0301` 不是把 0317 变成 park 的开关；park 只出现在 **预 attach** 之后（§29），
即"驱动已经通过调试器路径接管上下文"的状态。

**handler 输入侧的最终地图（已闭环）**
```
读：drv_pbuf = base-0x2000000   [只读]   .CONSTANT；+0 exit flag / +4 cmd / +8 preempt_idx
    scratchpad = base+0x1a00000 [可读写] 驱动自用
写：只有驱动（调试器 suspend/resume 状态机）
用户态：host ❌（cuXtra 仅 Init/cuda 异步 SIGSEGV）、device kernel ❌（Illegal Address）、
        trap 上下文 ❌（ST 即 700）
```
下一步若继续，只剩两条：**调试器 agent 协议** 或 **静态逆 driver 的 0317/0318 处理函数**
（找它写 +4 的位置与前置条件，看是否有别的 RM 控制能触发同一路径）。

### 41. 读机制只在"轻上下文"可用；调试态上下文里同一地址会 fault（疑被重定位）

| 上下文 | cave 读 `drv_pbuf+4`（`LD.E.CONSTANT`） |
|---|---|
| 轻上下文（`NO_ICTX_INSTRMEM` + `PLAIN_LAUNCH`，§38 `exp_drvbuf_4.log`） | ✅ 成功，多次 trap 稳定，读回 0 |
| 调试态上下文（默认 instr-mem）+ 预 attach（`exp_drvword_park.log`） | ❌ 启动后 0.1 s 即 700 |

推测：一旦进入 debugger-managed 状态，驱动会为上下文重新分配/重定位 preemption buffer，
并把 `do_read_trap_reason` 里的立即数重新 patch；我们用的 `base−0x2000000` 在那种状态下失效。

**下一步（明确）**：在**会话已建立之后**再 dump 一次活体 `do_read_trap_reason`
（当前 dump 发生在 Init、会话创建之前），读出新的 patched 地址，再用它去读驱动命令字。
若新地址可读，我们就第一次能在"park 发生的现场"看到驱动写的命令字（应为 2）。

### 42. park 的真正入口：**上下文创建时进程处于外部 ptrace 状态**（不需要 cuda-gdb）

同一套受管内核（`NO_PAYLOAD` + `L3AGENT_HOLD` + `KEEP_SESSION`），只改变"谁来 trace"：

| tracer（从 exec 起） | 调试 agent | agent 的 `0317{1,0}` | 计数 | `Synchronize()` | 判定 |
|---|---|---|---|---|---|
| 无 | 无 | `0xffff` | — | 27–30 ms 返回 | cancel（§17/§23） |
| **`strace -f`** | 无 | **阻塞不返回** | **冻结** 21,891,328 / 21,728,768 | 一直不返回 | **park** |
| **plain `gdb`** | 无 | `0xffff` | **冻结** 21,553,920 | 一直不返回 | **park** |
| `cuda-gdb`（launch） | 有 | `0` | 冻结 21,850,624 | 27.3 ms 返回 | cancel |

证据：`evidence/live/exp_strace_park1.log`、`exp_strace_park2.log`、
`exp_plaingdb_park1.log`、`exp_gdb_cancel1.log`。冻结判据不是"没有输出"：`l3qspin` 的
采样线程在 park 后仍每秒打印，计数连续 10–20 s 一模一样；同时队列的 `Synchronize()`
从不返回（进程被 `timeout` 杀掉）。plain gdb 下 `0317` 甚至返回 `0xffff`，内核仍然停住。

**结论（事实层）**
1. 是否 park，与 cuda-gdb 无关；与**外部 tracer 在 CUDA 上下文创建时就已经存在**有关。
   §17 里由 agent 在 Init **之后**才 `PTRACE_ATTACH`，不产生 park（仍 `0xffff`），
   说明这个状态是在 `cuInit`/上下文创建时被驱动记录的。
2. `0317` 的 ioctl 完成态与"停没停住"是两件事：没有调试 agent 时，驱动已经把
   suspend 下发到 GSP（内核停住），但没有任何人完成应答，ioctl 要么阻塞、要么报
   `0xffff`；有 cuda-gdb 的 agent 时，agent 立即处理这次 suspend，结果是 **cancel**
   （stream 判完成、27.3 ms 返回）。
3. 因此 §29 的现象（预 attach 后 park）可以统一解释为：**上下文在 debugger-managed
   状态下创建**；差别只在"谁来应答/恢复"。

**推论（明确标记为推论）**：park 是"debugger-suspend 状态机"的半程；后半程（应答、
写命令字 1、restore）只能由驱动侧的调试 agent 完成。用户态拿到的只是一个
"停住且不可释放"的上下文。

### 43. trap arena / preemption buffer 的 CPU 映射：mprotect 成功，但只是零页

用 §42 的 park 现场（strace + `0317`）解析 `/proc/<pid>/maps`：

| 地址 | 落在哪个 VMA | 属性 |
|---|---|---|
| `drv_pbuf = base-0x2000000` | 64 MB 匿名区间 | `---p`（PROT_NONE） |
| 活体 handler（`base+0x15f0f00`） | 6 MB 匿名区间 | `---p` |
| `scratch = base+0x1a00000` | 6.2 MB | `rw-s /dev/nvidiactl` |

于是加了 `XSCHED_SM120_TRAP_HOSTPOKE_MS`：park 3 s 后在本进程里
`mprotect(drv_pbuf 页, RW)` → 成功（`r=0`），读 `+0/+4/+8` 全 **0**，写 `+4=1`
回读 **1**，然后恢复 `PROT_NONE`。内核保持冻结、`Synchronize()` 仍不返回。

证据：`evidence/live/exp_hostpoke_park1.log`。

**结论**：这个 VMA 只是"驱动在进程 VA 里预留的占位"，没有 CPU 可达的后备页；
CPU 写入是匿名 COW 零页，GPU/trap 看不到。驱动 preemption buffer 在 CPU 侧
**不可读也不可写**，与 §37–§39 的 cuXtra/CUDA API/设备 store 结论一致。

### 44. scratchpad：唯一"host 映射 + trap 可见"的区域，但布局是驱动的 per-warp 记录

从 `evidence/disasm/plugin_120_0.do_read_trap_reason.txt` 与
`plugin_120_0.do_pause.txt` 可以读出 scratchpad 的布局：

```
R0 = warp/SM 相关索引;  R0 *= 0x2a0;  R4 = scratch_base + R0
do_read:  LD.E.U8.CONSTANT R0, [R4+0x29d]
do_pause: ST.E... [R4+0x290] / [R4+0x294] / [R4+0x298] / [R4+0x29e]
```

即 **每条 warp 记录 0x2a0 字节**，驱动的 CILP save/restore 只读写记录里
`+0x290..+0x29e` 附近几个字。

实验（`exp_scratch_read_fault1/2.log`）：host 通过 rw-s 映射写 sentinel 到
`scratch+0x1000`、`+0x300`、`+0x270`，让 cave 用 `LD.E.CONSTANT` 读同一地址；
三种偏移在 **launch trap** 就 700；把 host 写关掉、只读 `+0x270` 仍然 700。

**结论**：scratchpad 的 trap 可见范围只覆盖驱动当前活动的 per-warp 记录（且随
save/restore 生命周期变化），不是一块可以随便读写的通用共享内存；盲写还会破坏
CILP 现场。要用它做通道，必须先完整恢复 per-warp 记录布局与生命周期——目前没有
证据表明其中有"请求/应答"字段（`+0x29d` 是 defer-continuation 状态）。

### 45. 调试器库接口事实（静态）：命令字是 `libcudadebugger` 写的

- `libcuda.so.595.99.02` 导出 `cudbgApiInit/cudbgApiAttach/cudbgApiDetach`、
  `cudbgGetAPI/GetCUDADebuggerAPIVersion`，以及可写全局
  `cudbgEnablePreemptionDebugging`、`cudbgResumeForAttachDetach`、
  `cudbgUseExternalDebugger`、`cudbgEnableLaunchBlocking`。
  `cudbgApiInit(2)` 会遍历已存在的上下文并检查 "late attach" 能力；错误串包括
  "Late attach is not supported with CUDA MPS" / "Late attaching of a debugger that
  does not support CUDA lazy loading is not supported" —— 它不是简单的开关。
- `libcudadebugger.so.1` 字符串：`Failed to write preempt command`、
  `Failed to reset preempt command`、`Failed to get the preemption buffer layout: %d`、
  `Wrong preemption buffer size`，以及 RTTI
  `NV::Debugger::IPreemptionBufferAccessor` / `NullPreemptionBufferAccessor`。
  ⇒ **写命令字的是调试器库的 preemption-buffer accessor**，与 §39"只有驱动侧能写"
  完全吻合；驱动把 buffer layout 交给调试器库，由它在 suspend/resume 时写 2/1。
- `_nv83deCtrlCmdDebugAccessMemory`（0315/0316 的共同实现）参数为
  `{u32 hMemory; u32 size; u64 offset; u64 hostbuf}`，并用
  `offset+size <= memdesc.size` 做边界检查 —— 只能访问 **RM 内存对象**；
  驱动的 preemption buffer 不是 RM memobj，所以 0315/0316 也写不到它。
- `ksmdbgssnConstruct_IMPL`：要求 `hDebuggerClient_deprecated == 0`，解析
  `hAppClient`/`hClass3dObject`（必须是 0xcec0），再
  `kgrctxRegisterKernelSMDebuggerSession` 把会话挂到该 KGO 的 GR context 链表
  （`kgrctx+0x680`）。我们现有的会话创建路径满足这些条件，缺的不是"注册"，
  而是会话背后要有一个会写命令字、会应答的调试 agent。

### 46. 最终通道地图与"打通路径"结论（v4.9）

**已证实的通道地图**

```
trap 读:
  drv_pbuf = base-0x2000000   [设备侧只读]  .CONSTANT；CPU 侧零页、设备 store 非法
  scratch  = base+0x1a00000   [rw-s 映射]   仅驱动活动的 per-warp 记录窗口可读
  c[0x170..], SR              常量/SR        可用（launch 时固定）
trap 写:
  用户全局内存                 ✅            （§35，host 回读一致）
  scratch per-warp 记录        ✅（驱动布局内）
  drv_pbuf                    ❌            ST 即 700（§39）
host 写 drv_pbuf:
  cuXtra 同步（Init）          仅 cuxtra 自称成功，不能证明对 trap 有效
  cuXtra/CUDA 运行期           invalid / SIGSEGV
  设备 kernel store            Illegal Address
  mprotect + CPU store         零页 COW，trap 不可见
RM:
  0317                        debugger 态下：阻塞或 0xffff；内核 park
  0318                        任何 client 都阻塞（§30）
  0315/0316                   仅 RM memobj，有边界检查；trap buffer 不是 memobj
  0301..032b                  无一把 0317 变成"可恢复 park"（§40 + 本轮扫描）
```

**三条"打通"路径的判定**

1. **生产可用路径（已交付）**：GDB backend（`run_l3hal.sh`），suspend→高优先级内核
   0.74 s 完成、`Resume=1`。功能满足 L3 语义，代价是依赖 cuda-gdb/调试 agent。
2. **快速 park-only 路径（本轮新证）**：外部 tracer 在 CUDA init 前存在（`strace`/
   plain `gdb` 均可，**不需要 cuda-gdb**）+ RM `0317` → 真 park（计数冻结、
   `Synchronize` 不返回）。停住本身可以在亚毫秒级完成（§33 的 0.2 ms 是同类状态 +
   预建会话），但**没有任何用户态手段能释放**（`0318` 阻塞、无 agent 写命令字 1）。
   适用场景只有"停住后不再恢复"（驱逐/销毁/整进程回收）。
3. **完整 trap-native park+resume（未打通，但入口唯一）**：必须让**进程内调试 agent**
   （`libcudadebugger`：`DebugAgentMain` / `cudbgApi*` / `IPreemptionBufferAccessor`）
   接管 suspend 的应答与命令字写入。纯 RM 路线已被排除：
   - 输入 buffer 非 RM memobj（0315/0316 到不了）；
   - 0317 的完成/恢复属于调试器状态机；
   - `0318` 在所有 client 上都阻塞。
   下一步唯一有意义的工程是复刻/驱动调试 agent 协议（或在 target 内加载
   `libcudadebugger` 并实现最小 IPC），而不是继续找 RM 命令。

**对论文方法的最终判定**：论文 Lv3 依赖"handler 读请求位 → abort"。在 sm120/595 上，
handler 可读的只有驱动私有、设备侧只读的 preemption buffer；用户态无法写请求位
（§35/§39/§43）。因此 **handler 判据式复现在当前平台不可达**；可行的 L3 只能是
"调试器 agent 的 suspend/resume"（现状）或"debugger 态下的 park-only"（新证）。

### 47. 对照原论文更正：它的"信息交互"不是 handler 读全局 flag

重读 OSDI'25 原文（§6.2–§6.3、Fig. 7/8）后，需要把之前"论文 handler 读 flag"的
说法更正为两个独立机制：

1. **host ↔ 运行中的 kernel（Lv2 guardian，Fig. 7/8）**：
   ```
   guardian(flag_ptr, hwq_id, cmd_id):
       if *flag_ptr == DEACTIVATED: push_aborted_cmd(); exit_thread()
   host deactivate(hwq): *get_deactivation_flag(hwq) = DEACTIVATED
   ```
   这个 flag 在 **GPU global memory**，地址由 kernel 参数/constant memory 传入，
   **由注入到 kernel 头部的 guardian 代码读取**——运行在普通 kernel 上下文，
   不是 trap handler。abort 记录（hwq_id/cmd_id）也由 guardian 写回全局内存，
   host 之后 pop 出来重放。这条通道我们在 sm120 上是通的（§35：普通上下文读写正常）。
2. **host ↔ trap handler（Lv3 queue-based，§6.3）**：
   "we discovered a specific ioctl that triggers GPU interrupts by writing to a GPU
   control register ... it immediately stalls all running kernels and invokes the
   trap handler to save context and **check trap reason**. ... XSched extends the trap
   handler to **detect interrupts triggered by XSched**. When detected, the target
   kernel is aborted ... the rest ones restore the context and continue."
   ⇒ 传给 handler 的"信息"是**中断/trap reason 本身**（由寄存器写编码），
   **没有任何 host→handler 的数据 buffer，也不要求 handler 去读全局内存**。
3. 恢复：论文明确不支持从断点恢复，只对 **idempotent kernel 从头重启**。

**对我们的意义（这是对 §32 的关键澄清）**
- §32 的"handler 没有判据"= 缺的正是**可区分的 trap reason**，而不是"读不到 flag"。
  论文的 flag 是给 guardian（普通上下文）的，我们读不到全局内存这件事并不违背论文。
- sm120 上缺的是：(a) 带内寄存器写被启动期能力位 `pGpu+0x911` 一票否决（v1.1），
  (b) RM 0317 这条 debugger-suspend trap 在 handler 里 reason 恒为 0x1，无法区分
  "XSched 请求"与 launch/debugger trap。
- 因此论文式 Lv3 在 sm120 上不可复现的根因应表述为：**"可区分的带内中断源"缺失**，
  次生结果是 handler 只能沿用驱动自己的 preemption 命令字协议（而写它需要调试 agent）。

### 48. 总汇文档（2026-10-26）

本文（§1–§47）是滚动实验日志。为便于接手/写报告，另整理了一套分册总汇：
`test/sm120_l3_trap_v2/compendium/`

| 文件 | 内容 |
|---|---|
| `README.md` | 一页结论、术语、证据索引 |
| `01-研究过程.md` | v0.1 → v5.9 时间线（目标/做法/结论/证据） |
| `02-路径与方法.md` | R1/R2/R3/R3'/trap/GDB/park-only 六条路径的判定 |
| `03-机制与逆向.md` | handler 地址图、SASS 编码、pbuf/scratchpad、RM/0x83de、调试栈、cuxtra |
| `04-通道矩阵.md` | host↔trap 读写尝试全矩阵与"为什么不行" |
| `05-工具与复现.md` | 构建、探针、环境开关、复现命令、常见坑 |
| `06-结论与下一步.md` | 需求审计、延迟账本、论文对照、Ampere A/B、死路清单 |

后续新增结论请同时更新本文与 `compendium/` 对应分册。

### 49. A/B 实测（2026-10-26）：论文 L3 在 **sm86/570** 上跑通；sm120/595 卡在触发闸门

**被测对象**：上游 `XpuOS/xsched`（main，commit `f49289f`，2026-08-19），
CUDA 12.1 + g++11 + CMake 3.22；**RTX 3090（sm86，570.124.04，GSP on，容器）**。
补充对照：A100-PCIE-40GB（sm80，550.107.02）。

**构建/运行**（3090）
```bash
# 源码用 A100 上 git clone 的 main（3090 到 github 的 git/codeload 不通）
# 补 3rdparty/cuxtra（本地仓库的 include+lib）与 3rdparty/ipc（boost-ipc/tools）；
# CMake 3.22 下把 3rdparty/ipc/CMakeLists.txt 的 if(BUILD_TEST) 临时改 FALSE
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/output \
  -DSHIM_SOFTLINK=ON -DBUILD_TEST=ON -DBUILD_SERVICE=OFF -DPLATFORM_CUDA=ON \
  -DCMAKE_CUDA_STANDARD=17 -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=g++ -DCMAKE_CUDA_FLAGS="-arch=sm_86 -I/usr/local/cuda/include" \
  -DCMAKE_CXX_FLAGS="-I/usr/local/cuda/include" \
  -DCUDA_INCLUDE_DIRS=/usr/local/cuda/include \
  -DCUDA_LIBRARIES=/usr/local/cuda/lib64/libcudart.so
cmake --build build --target install -- -j32
LD_LIBRARY_PATH=$PWD/output/lib ./output/bin/cuda/level 3
```

**结果（`evidence/ab_ampere/logs/`）**

| 机器/级别 | preempt avg | p50 | p95 | p99 | restore avg | 说明 |
|---|---|---|---|---|---|---|
| 3090 / level 1 | 11.7–13.2 ms | 12.5–13.2 ms | 13.7 ms | 13.8 ms | 6.9 µs | 无 L3，等 kernel 完成 |
| **3090 / level 3（run1）** | **230.6 µs** | **227.3 µs** | 242.4 µs | 419.8 µs | **96.7 µs** | dead-loop 也停住 |
| **3090 / level 3（run2）** | **257.2 µs** | **228.9 µs** | 249.7 µs | 2232.6 µs | **90.4 µs** | 20 个 block-time 点全跑完 |

**这次跑的是论文的哪条路**
- `arch.cpp`：sm86 → `CudaQueueLv3Trap`；默认 `GetCudaLv3Implementation()` 返回
  `kCudaLv3ImplementationTrap`（只有显式 `XSCHED_CUDA_LV3_IMPL=TSG` 才走 TSG）。
- `InterruptContext::Interrupt()` → **`cuXtraTriggerTrap(kCtx)`**；
  `InterruptContext::InstrumentTrapHandler()` 先用 `TarpHandlerSM86::Instrument()`
  改写活体 trap handler（guardian/resume/trap-inject SASS），再写回并
  `cuXtraInvalInstrCache`。
- ⇒ 论文的"带内触发"= **插桩后的 trap handler + `cuXtraTriggerTrap`（`0x419e84` bit31，
  经 `0x20800122`）**。**只写寄存器不插桩 handler 是不够的**。

**A100（sm80/550）补充对照**
- `0x20800122` 在 550 上**完全开放**：参数 `{hClientTarget=0, hChannelTarget=0, ...}`
  可读 `0x200510`（PTIMER，实时值）与 `0x419e84`（=0x1c01）；写 0x419e84 bit31 返回 0。
- 但 A100 是 **sm80**，上游 `TarpHandler::Instance` 只支持 sm70/sm86（sm80 返回
  `CudaQueueLv1`），而且我们的探针没有做 `TarpHandlerSM86` 插桩，所以"写寄存器后内核
  照跑"——这不是否定触发路径，只是没有走论文的 handler 插桩。
- cuda-gdb 在 A100 上的中断走 **0x83de0307 → 0317{1,0} → 0315/030c/0316 → 0318**，
  与 sm120 同一条 debugger-suspend 路径，**不是**带内寄存器触发。

**和 sm120/595 的对照结论**
- sm86/570：`0x20800122` 生效（可读写寄存器），cuxtra 触发 + SM86 插桩 handler 工作，
  L3 preempt p50 ≈ 229 µs，restore ≈ 90 µs。
- sm120/595：`0x20800122` 被 `pGpu+0x911` 早退（返回 0、无效果），`cuXtraTriggerTrap`
  无法投递；唯一可用的是 RM 0317（debugger-suspend）→ 27 ms cancel/park；
  GDB backend 116 ms。
- 因此 "Ampere vs Blackwell 架构差异" 不是主因：**同一套上游代码在 sm86/570 上跑通**，
  差别落在 **595 驱动对 sm120 关闭了 regops 能力位**（以及 sm80 不在上游支持列表里）。
- 论文 GV100 的 32 µs 是 sm70 的数字；我们在 sm86 上测得 229 µs，仍是同一个"handler 内
  abort + 重启"机制，量级远好于 sm120 的 27 ms / 116 ms。

**证据**：`evidence/ab_ampere/logs/level1_sm86.log`、`level3_sm86.log`、
`regops_3090.log`、`a100_regops.log`、`a100_agent_ioctl_0317.log`、`smi*.txt`、
`uname*.txt`；`evidence/ab_ampere/README.md`；上游 commit
`f49289f0220931df78de948ed841ecbaf960a919`。

### 50. A/B 完整 battery（2026-10-26）：快路径的三段耦合

在 §49 基础上把两台租用机的测试补全，结论从"触发"细化到"managed launch + handler
插桩 + 触发"三段：

**3090（sm86/570，带 shim）**

| 配置 | 2000 µs block p50 | dead-loop p50 | dead-loop p99 | restore |
|---|---|---|---|---|
| level 1 | 13.38 ms | — | — | 8.2 µs |
| level 2 | **1.257 ms** | — | — | 57.3 µs |
| level 3 ×3 | 231.4 / 230.5 / 227.3 µs | 241.1 / 230.8 / 235.2 µs | 452 / 2826 / 420 µs | 93–127 µs |
| level 3 TSG | 13.35 ms | 180 s 超时 | — | 276.8 µs |

**关键对照**
1. **去掉 shim**（`output/lib/libcuda.so*` 改名）：3090 的 L3 从 229 µs 退化到
   **6–40 ms**（10 µs block → 7.2 ms；500 µs block → 34.0 ms，restore 26.5 ms）。
   ⇒ 快路径依赖 shim 的 managed launch（`XLaunchKernel` → `CudaKernelCommand` →
   debugger params/guardian），不是只靠 handler + 触发。
2. **正确性**：16M int `C=A+B`，400 次 launch 中做 **200 次** `XQueueSuspend/Resume`，
   最后 `bad=0 / 16777216 -> OK`（`evidence/ab_ampere/correct_sm86.log`）。
3. **handler 实物**：sm86 与 sm80 的 handler 都是 **0x1200**，`+0x3e0` 都是
   `IADD3 R1,R1,-0x10,RZ`（words `fffffff001017810 004fe40007ffe0ff`）。上游
   `TarpHandlerSM86::Instrument` 在 **+0x3e0** 替换为 JMP inject_dev，注入
   `trap_inject_instrs` + 原指令 + 跳回。
4. **cuda-gdb 交叉**：3090 attach 0.047 s/detach 0.045 s，A100 attach 0.159 s/detach
   0.267 s；两者目标进程都是 `0307 → 0317 → 0315/030c/0316 → 0318`，与 cuxtra 触发
   是两条独立路径（`sm86_agent_ioctl_0317.log`、`a100_agent_ioctl_0317.log`）。

**A100（sm80/550）**
- `0x20800122` 开放（可读写寄存器），但：
  - 带 shim 的 `level 3` **SIGSEGV in `cuXtraKernelGetFunction`**（libcuda 550）；
    TSG 带 shim 同样崩。
  - 无 shim 的 `level 3` 很慢：300 µs p50 **33.6 ms**、400 µs p50 **46.4 ms**、
    restore 25.8 ms；TSG 无 shim：10–40 µs p50 0.58–1.49 ms，900 µs p50 **55.7 ms**；
    均 180 s 超时。
  - sm80→SM86 的 `arch.cpp` 两行补丁不能解决问题（同一个 cuxtra 崩溃）；上游 cuxtra
    子模块与本地库 md5 相同，不是版本问题，是 **550 驱动兼容性**。
- ⇒ A100 只能跑驱动级慢路径；论文快路径需要可用的 cuxtra/550 或真正的 SM80 端口。

**最终三段图**
```
managed launch（shim/debugger params） + instrumented handler（+0x3e0） + 触发（0x20800122）
  3090: ✅ + ✅ + ✅  -> 229 µs, 正确性 OK
  3090: ❌ + ✅ + ✅  -> 6–40 ms
  A100: ⚠️(cuxtra崩) + ✅ + ✅  -> 不可用（带 shim）/ 30–55 ms（无 shim）
  sm120: ✅ + ✅ + ❌(pGpu+0x911) -> 0317 27 ms / GDB 116 ms
```

**证据**：`evidence/ab_ampere/battery_3090/`、`correct_sm86.log`、`sm86_handler*.bin/log`、
`level3_sm86_noshim.log`、`level3_sm80_noshim.log`、`level3_tsg_a100_noshim.log`、
`level3_sm80patch_gdb.log`、`sm86_agent_ioctl_0317.log`；完整整理见
`compendium/07-A-B-完整实测.md`。

### 51. sm120 debugger param 布局实测：handler 能读 kernel_idx / killable（2026-10-26）

新增 cave 探针（`XSCHED_SM120_TRAP_DBGPARAM_PROBE=1` + `..._DBGPARAM_OFF`）：
在受管 launch 的 trap 里执行 `LDC R10, c[0x0][off]`，`ST.E` 到 `wr_buf+0x100`，
由 `REASON_PROBE` 的 reader 在 +4 s 读出。cave 同时恢复 `MOV R3,RZ`、`IADD R12`、`RET`。

| `c[0x0][off]` | 读回值 | 含义 |
|---|---|---|
| `0x170` | `0x02000000` | XSched preempt buffer 指针低 32 位 |
| `0x178` | `0xb6c00200` | guardian entry 低 32 位 |
| **`0x180`** | **`0x1`** | **kernel_idx = 1**（与 `trap queue: remembered kernel cmd idx=1` 一致） |
| **`0x188`** | **`0x1`** | **killable = 1** |

⇒ `cuXtraSetDebuggerParams` 的 28 字节 args 在 trap 常量库里正是：

```
c[0x170] = preempt_buf   (8 B)
c[0x178] = guardian      (8 B)
c[0x180] = kernel_idx    (8 B)
c[0x188] = killable      (4 B)
```

**结论**
1. **handler 能读自己的 kernel_idx 和 killable**（之前"偏移未确认"的缺口已补上）；
2. cave 是**每个 context 共用**的代码，但每个 warp 在 trap 里读到的是**自己的**
   kernel_idx/killable（常量库按 kernel 设置）；
3. 仍然缺的两件：
   - **target id**（这次 suspend 要杀谁）不是 launch-time debugger param，host 不能在
     运行期改它；自然的通道是 `c[0x170]` 指向的 preempt buffer，但 trap 读该 buffer 的
     **数据**返回 0（指针可读、内容不可读）；
   - **request/reason**（这是抢占不是 launch）仍是 `0x1`，没有区分。
4. 因此现在的 cave 已经可以写：

```
if (reason/request && my_kernel_idx == target_kernel_idx) EXIT;
```

   只差 **target_kernel_idx 和 request** 的 host→trap 通道。

证据：`evidence/live/exp_dbgparam_170.log`、`exp_dbgparam_178.log`、
`exp_dbgparam_180.log`、`exp_dbgparam_188.log`；新探针代码在
`platforms/cuda/hal/src/level3/trap_sm120.cpp`（`XSCHED_SM120_TRAP_DBGPARAM_PROBE`）。

### 52. 驱动 pbuf 的 cmd/preempt_idx 实测：0317 陷阱里 **+0/+4/+8 全 0**（2026-10-26）

§36–§41/§51 之后，最自然的"request + target"通道就是驱动自己 patch 进 `do_read` 的
preemption buffer（`base−0x2000000`，有会话时重定位到 `base−0x8000000`）：0317 时驱动写
`+4=2`、`+8=preempt_idx`，cave 只要读到就能区分"这是抢占"并拿到目标。本轮把这个想法
用可控的方式实测完毕。

**新增探针（`platforms/cuda/hal/src/level3/trap_sm120.cpp`）**
1. `XSCHED_SM120_TRAP_ARM_PBUF_PROBE` / `..._ARM_SENTINEL`：载荷在 Init 时构建，但由
   `Arm()` 在** launch 之后、lazy 0x83de 会话创建之前**装入 cave。这样装入后到 0317 之间
   没有内核启动，cave 看到的就只可能是 0317 那一发 trap。
2. **host 映射记录页**：`cuMemHostAlloc(DEVICEMAP)` 4 KB，cave 直接 `ST.E` 进去，CPU 直接读。
   之前用 `cuXtraMemcpyDtoHAsync` 在 park/调试态里经常读不回来（reader 不打印）。
3. 两种 arm 方式：`HOST_ARM=1`（`cuXtraMemcpyHtoD`，无额外 launch，但会等运行中的内核，
   **实测 ~2.1 s**）；默认设备 writer（`l3write`，**0.2 ms**，但它自己的 launch 会产生
   一发 reason=0x8 的 trap，打到刚装好的载荷上）。

**结果（全部 `run_l3qspin.sh suspend`，即 `0317{1,0}` + `KEEP_SESSION` + cuda-gdb ack）**

| cave 载荷 | arm 方式 | 记录页 `+0x100` |
|---|---|---|
| sentinel：只存 R2 | 设备 writer | **0x8**（writer 内核自己的 launch trap） |
| sentinel：只存 R2 | host cuXtra | **0x1**（0317{1,0} 的 trap） |
| 读 `drv_pbuf+4`（cmd） | host cuXtra | **0** |
| 读 `drv_pbuf+8`（preempt_idx） | host cuXtra | **0** |
| 读 `drv_pbuf+0`（exit flag） | host cuXtra | **0** |
| return-only（预装） | — | resume 后 700（偶发） |

**判读**
1. **0317 路径上驱动不写 pbuf**。cave 在 `handler+0x390`，而 `+0x370` 的 `CALL do_read`
   已经先读过 `pbuf+4`；抓到 `R2=0x1`（而不是 `0x30=0x10|0x20`），说明驱动自己读到的
   命令字也是 0。三个字全 0 与这一点互相印证。
2. **pbuf 在 0317 陷阱里是映射好的**：`LD.E.CONSTANT` 读 `base−0x8000000+{0,4,8}` 返回 0、
   不 fault（§41 里 fault 的是 **launch trap**，reason=0x8）。所以"读不到"不是映射问题，
   而是**驱动没写**。这也给出一个可用的门控判据：reason bit0（0x1）才读 pbuf，
   launch trap（0x8）直接走 benign return。
3. **重定位再次确认**：`0x7ffddc000000 (base−0x2000000) → 0x7ffdd6000000 (base−0x8000000)`，
   与 §41/§51 的会话态观察一致。
4. **0317 是通道级挂起 + 队列重放，不是 pbuf 命令 + CILP 保存**。计数在 hold 期间冻结，
   resume 后从 ~1M 重新涨到 101M（拍点 992256 → 8.3M → …），即原内核被杀、队列重放；
   `trap reactivate: replaying the killed kernel` 与之一致。没有原地恢复。
5. 因此 **驱动 pbuf 不能作为 host→trap 的 request/target 通道**：只有真正的调试器
   suspend 路径（cuda-gdb attach + target 侧注入）才会写 `cmd=2/idx`；agent 自己发的
   `0317{1,0}` 在 sm120/595 上走不到那条状态机（与 §22–§24 的结论同源）。

**附：reason 门控 + EXIT 的直接验证（`REASON_ARM`）**
既然 0317=0x1、launch=0x8，就用现成的 reason-armed 存根（bit0 → 改写返回地址到
EXIT trampoline）做了一次受管 launch：launch 没有被误杀（说明 0x8 确实不带 bit0），
0317 后内核被 EXIT（计数停在 177152）、suspend 27.4 ms；但随后
`trap reactivate: log=0`、重放没有发生，context 最终
`cudaErrorIllegalAddress`、`done=0`。⇒ **判据本身可用，但裸 EXIT 会破坏驱动
trap 状态机**；能重放的仍是"让驱动原生 do_pause + 通道挂起"那条路（27 ms）。
证据：`evidence/live/exp_reason_arm_exit.log`。

**证据**：`evidence/live/exp_arm_sentinel_dev.log`（0x8）、`exp_arm_sentinel_host.log`（0x1）、
`exp_arm_pbuf_cmd4.log`/`exp_arm_pbuf_idx8.log`/`exp_arm_pbuf_flag0.log`（全 0）、
`exp_return_only.log`、`exp_reason_arm_exit.log`；探针 env 见上。

**§52 续：能不能"主动写"驱动 pbuf？（2026-10-26）**

host / 设备 kernel 侧的写早已定论（§37–§39：运行期 cuXtra invalid、CUDA 异步 SIGSEGV、
设备 store Warp Illegal Address、CPU mprotect 后是 COW 零页）。本轮补测**0317 trap 里**
的写，用 arm-at-Trigger + host 映射记录页：

| 载荷 | 结果 |
|---|---|
| `ST.E [pbuf+4], R2`（值 1）+ 记录 R2 到 host 页 | **不 fault**，记录页 `0x1`；内核照常 kill+replay、`cudaSuccess` |
| `ST.E [pbuf+4], R10`（值 **2** = preempt_save） | **不 fault**；行为与写 1 **完全相同**（`resume ack=1`、`log=1` 重放、`cudaSuccess`） |

对比：launch trap 里对同一地址的 `ST` 会 **700**（§39），所以 0317 trap 里这块映射至少
"可发 store"。

**但写 2 没有产生任何可观察影响**。handler 的 dispatch 是 `+0x370 loop → CALL do_read
→ +0x390(cave) → +0x3a0 分派 → R2=1 → goto loop`；如果写进的值能被 `do_read` 看到，
下一轮就会得到 `reason=0x30 (0x10|0x20)` 并走 `do_preempt_save + do_pause`（park），
而实测仍是原来的 kill+replay。⇒ **这个 store 没有进入驱动可见的 pbuf**；
最可能是 trap 上下文看到的是 pbuf 的**私有/快照映射**（可读、可本地写，但不与驱动共享），
这也解释了"读得到 0、写不进去"。

结论：**trap 可以在 0317 时刻向该地址发 store（不 fault），但不能把它当成写通道**；
驱动 pbuf 依旧只能由驱动自己写。

### 52b. 汇报材料与问答（2026-10-26）

为便于汇报/接手，新增独立目录 `report/`：

| 文件 | 内容 |
|---|---|
| `report/README.md` | 目录说明、一页结论、阅读顺序、口径 |
| `report/01-成果汇报.md` | 10–15 分钟汇报稿：一页结论、三个核心观点、需求审计、决策、8 页讲法 |
| `report/06-问答.md` | Q&A：本轮 5 个问题（进度、架构差异、带内入口、触发对照、0317 vs 419）+ 12 条延伸问答 |
| `report/02-中断入口.md` / `report/03-核心结论.md` / `report/04-替代形态.md` / `report/05-下一步论点.md` | 观点 1–3 的完整论证与路线图 |

口径说明：
- 汇报文档以本文 §1–§52 为准；如与早期条目冲突，按最新章节（v6.8）解读。
- 特别更正：`trap 上下文 ST.E 写 pbuf 会 700` 只对 **launch trap** 成立；
  在 **0317 trap** 里 store **不 fault**，但不与驱动共享、行为无效（§52 续）。
- 判据口径：当前配置 `0317=0x1`、`launch=0x8`；生产建议用精确值 `R2==0x1` 门控，
  并在目标配置复验 launch reason（§52 附录）。


### 53. 触发入口的静态定位补测："机制存在但被启动期闸门关掉"（2026-10-26）

针对"sm120 的内部触发是没发现，还是不存在"这个问题，对 595.99.02 的
`nv-kernel.o_binary` 与用户态库做了补扫。结论：**机制存在、接口存在、寄存器地址在
用户态库里；是启动期闸门把它关掉，不是"不存在"**。但"没有第二条入口"仍是经验性穷举。

**1. 触发寄存器 `0x419e84` 不在内核驱动里，在用户态库里**

```bash
grep -abo $'\x84\x9e\x41\x00' nv-kernel.o_binary        # 0 处
grep -abo $'\x84\x9e\x41\x00' libcuda.so.595.99.02      # 多处
grep -abo $'\x84\x9e\x41\x00' libcuxtra_linux_x86_64.a  # 2 处
```

⇒ 寄存器地址由**用户态 reg-op 描述符**传入；内核里的
`subdeviceCtrlCmdGpuExecRegOps_cmn` / `ksmdbgssnCtrlCmdDebugExecRegOps_IMPL`
是通用的"执行这批 reg ops"，它只决定"执不执行"。

**2. 闸门 `pGpu+0x911`：只有两处写，都在初始化**

```bash
objdump -d nv-kernel.o_binary | grep '0x911('
# 0x17376  movb $0x1,0x911(%rdi)   __nvoc_init_dataField_OBJGPU+0x546
# 0x17b4c  movb $0x0,0x911(%rdi)   __nvoc_init_dataField_OBJGPU+0xd1c
# ... 其余全是 cmpb 读点（各调试/reg-op 路径共用），没有运行期 setter
```

**3. gate=0 时两条 reg-op 路径都跳过寄存器操作**

- **subdevice** `subdeviceCtrlCmdGpuExecRegOps_cmn@0x2dcfc0`：
  `0x2dd120 xor eax,eax`；`0x2dd122 cmpb $0x0,0x911(%r12)`；`je 0x2dd02a`
  → **返回 NV_OK（0）并跳过真正的寄存器写**（与 v0.3 的 `return NV_OK` 一致）。
- **会话** `ksmdbgssnCtrlCmdDebugExecRegOps_IMPL@0x21a990`：
  `0x21a9cc cmpb $0x0,0x911(%rbx)`；为 0 时 `mov $0x56,%eax; ret`
  → **返回 0x56，根本不进入执行分支**。
  （v1.1 把这条也写成 "return NV_OK" 不准确，此处更正。）

**4. 动态侧印证**

- `exp_regops2.log`：read 部分 `status=0x3a`、带 channel 的一次 `status=0`；
  PTIMER 等寄存器回读仍为 0（无有效副作用）。
- `exp_baseline_trigger.log`：`cuXtraTriggerTrap` 立即返回，计数继续
  11.1M → 70.6M（36.9 M/s），不冻结。

**5. 地址假设的排除**

`0x419e84` 在 **550/570/595 三个 libcuda** 里都是硬编码立即数（595 反汇编 13 处，
如 `movl $0x419e84,-0x1c(%rbp)`、`mov $0x419e84,%esi`；570 上同地址可用），
不像"sm120 专属地址"；且 gate 检查在写之前，地址对不对都轮不到执行。
⇒ "换个 sm120 地址"不是首选解释；验证地址假设需要未被 gate 的写路径（BAR0/root）
或先开 gate。

**6. 表述边界**

- 能证明：**论文用的那条带内路径在这台 sm120/595 上存在但被启动期关掉**；
- 不能证明：**不存在任何其他用户态可达的触发**（全称否定）；
  目前已枚举 RM controls、cuxtra、debugger/0317、tracer、BAR0（不可用/不安全），
  没有发现第二条；要升级为强结论，需要"同一台机器打开闸门"的反事实实验，
  或全量 RM control/escape 的副作用矩阵。


### 54. 受管 + InstrMemAllocator：cave 可执行，但不改 context cancel（2026-10-27）

此前 §19/v4.x 的表述"受管 + 默认插桩 → 探针不进入"是**特定配置下的简写**，
容易被误读为"managed kernel 与 cave 不能共存"。用 `run_l3qspin`（受管 XQueue、
默认 `InstrMemAllocator`）+ `XSCHED_SM120_TRAP_CAVE=1` + `ARM_SENTINEL=1` +
`HOST_ARM=1` + lazy session + cuda-gdb ack 复测：

```
arm sentinel off=4 tva=0x7ffdd6000004 ... upload=0
trigger sent (ack=1 status=0, not waited)
rec +0x100 = 0x1 ... (t=200 ms)
trap reactivate: log=1 ... replaying the killed kernel
```

- **cave 在 0317 trap 里确实执行**（记录页读到 R2=0x1）；
- 但驱动仍把 context cancel/挂起，resume 时由 XQueue 重放；
- 所以"cave 可执行"与"cave 有决策权"是两件事：
  - managed kernel + live-handler patch + cave **可以共存**；
  - cave 不能阻止驱动对 context 的 cancel/suspend；
  - 观察/记录用 cave；恢复用 XQueue replay/GDB/开闸/调试 agent。

证据：`evidence/live/exp_arm_sentinel_host.log`；`report/04` 的 0317 表、
`report/04` §2.2.1、`report/kb/Methods.md` E-13/P-17。
