# ABI：sm120 trap/driver 参考手册

> 版本：2026-10-27　**只对驱动 595.99.02 + RTX 5060（sm_120）做精确断言**；
> 595.71.05 + RTX 5090 的相同项在 §10 标注；换驱动必须重新跑指纹校验。
> 本文是"知识/ABI 手册"，不是结论论证；结论见 `report/03`、`report/04`。
> 证据：`FINDINGS_ZH.md` §1–§54、`compendium/03-机制与逆向.md`、`RM_REVERSE_ZH.md`、
> `evidence/disasm/`、`evidence/live/`。

---

## 0. 使用说明

- **地址都是设备/驱动内部 VA**，不是用户态可直接 `memcpy` 的地址；
  读取要走 `cuXtraMemcpyDtoH`（或设备代码），写入要走 `cuXtraMemcpyHtoD` /
  `cuXtraInstrMemcpyHtoD` / 设备 writer。
- 标注 **✅ 实测** = 有日志/反汇编；**⚠️ 推断** = 由机制推出；**❌ 未验证**。
- 驱动版本敏感：`base` 是每次 context 创建时从 slot20 镜像重算的，
  **不要硬编码绝对地址**；本文的 `base+...` 是相对布局。

---

## 1. 对象与地址地图

### 1.1 三个地址空间

| 空间 | 谁能访问 | 用途 |
|---|---|---|
| 用户进程 VA | host CPU / CUDA API | 应用缓冲、handover 计数、shm |
| 设备 VA | 用户 kernel / trap 上下文 | CUDA 分配、驱动 pbuf（trap 只读） |
| 驱动 arena（tools image + live handler） | 只有 cuxtra / 设备代码 | handler、slot20、scratchpad、reason tables |

### 1.2 从 slot20 镜像算 `base`

```
cuGetExportTable(CU_ETID_ToolsTrapHandler)  -> trap_table
cuGetExportTable(CU_ETID_ToolsMemory)       -> mem_table
trap_table[20](ctx, &ti)                    -> ti.handle
mem_table[24](ctx, ti.handle, &pc)          -> slot20 image pc
mem_table[25](ti.handle, &size)             -> slot20 image size (0x1300)
cuXtraMemcpyDtoH(img, pc, size)
trap_reason_table = MOV pair @ img[0x130]
base              = trap_reason_table - 0xc38300
```

- UUID：`cc529e945c4e9d46837c9625868334e4`（trap handler）、
  `bfdb432dbf3c5a4a945eb34029e81e75`（tools memory）；
- `ti.stub = 0x880`、`ti.hsize = 0x940`（✅ 5060/5090 一致）；
- `pc - base = 0x15f5300`（✅ 5060/5090 一致）。

### 1.3 关键相对地址（✅ 5060/5090 一致）

| 对象 | 相对地址 | 说明 |
|---|---|---|
| live `.text.traphandler` 入口 | **base + 0x15f0f00** | 10 条指纹；patch 点 +0x390 |
| `do_read_trap_reason` | **base + 0x15f2100** | 读 pbuf/reason；+0x120/0x130 = pbuf 立即数 |
| slot20 镜像 pc | base + 0x15f5300 | size 0x1300 |
| scratchpad | base + 0x1a00000 | host rw-s，per-warp |
| pause_reason_table | base + 0x1000000 | 驱动自用 |
| trap_reason_table | base + 0xc38300 | 由它反推 base |
| 驱动 pbuf（pre-session） | base − 0x2000000 | 595.99 早先观测 |
| 驱动 pbuf（session 态） | base − 0x8000000 | 595.99/5090 kill 流程 |
| 驱动 pbuf（5090 trap 流程） | base − 0x6000000 | 595.71 观测 |
| arena span | 0x1a00000 | 扫描边界 |

> **pbuf 地址会随会话/模式重定位**；必须以 live `do_read+0x120/0x130` 的 MOV 立即数为准，
> 不要硬编码。`arena.h`/`find_live_handler` 的解码只是"当前状态"的快照。

### 1.4 slot20 镜像布局（0x1300）

```
0x000 .. 0x9FF   .text.do_pause 的 relocated 副本（160 条，0/160 opcode mismatch）
0x880            upstream 的 payload 返回桩（TarpHandlerSM120::Instrument 改这里）
0xA00 .. 0xA7F   我们的 cave（8 条 SASS = 128 B）
0xA70            EXIT trampoline（cave+0x70）
```

- `live+0x390` 的 `CALL.ABS` 跳到 **pc+0xA00**；
- cave 与 live handler 不是同一段：cave 在 slot20 镜像里，live 在 base+0x15f0f00；
- 上游 payload 路径（`+0x880`）与我们的 cave（`+0xA00`）是两条独立路线。

---

## 2. trap handler

### 2.1 入口与指纹

live 入口前 0xa0 的 10 条 opcode（✅ 5060/5090 一致）：

```
0x0000795c 0x1a007956 0xff007387 0xff007387 0xff007387
0x00047202 0x00057202 0xff067803 0x17077355 0xff007387
```

- `ProbeAndPatch()` 会逐条比对；不匹配就拒绝 patch；
- 入口先 `BPT.DRAIN`/`BMOV MACTIVE`/保存 R0–R13/PR/Rpc/OPT_STACK/B0/THREAD_STATE；
- `+0x210` 读 `SR_GLOBALERRORSTATUS`（bit2 时进入 pbuf/scratchpad 读取路径）；
- `+0x370` 循环：`LEPC R12` → `CALL do_read_trap_reason` → 按 R2 位掩码分派；
- `+0x390` 是 `MOV R3,RZ`（我们的 CALL 覆盖点）；
- `+0x3a0` 是 `STL.64 [R1],R2`（**不能动 R1**）；
- 结束走 epilogue，恢复寄存器/TRAP_RETURN_PC。

### 2.2 reason 位掩码（do_read 的输出）

| bit | 值 | 动作 |
|---|---|---|
| 0 | 0x01 | `do_preempt_restore` + `do_pause`（R2=1，loop） |
| 1 | 0x02 | `do_detect_continuation` |
| 2 | 0x04 | `do_set_defer_continuations` |
| 3 | 0x08 | `do_pause`（R2=1，loop） |
| 4 | 0x10 | `do_preempt_save` + `do_pause`（R2=1，loop） |
| 5 | 0x20 | `do_pause_quiet`（R2=1，loop） |
| 6 | 0x40 | `get_defer_continuation_state_and_clear` |
| 7 | 0x80 | `do_handle_continuations` |

**reason 来源**（`do_read_trap_reason`）：

| 来源 | 结果 |
|---|---|
| 驱动 pbuf `+4` = 2 | `reason |= 0x10 | 0x20`（save + pause） |
| 驱动 pbuf `+4` = 1 | `reason |= 0x01 | 0x20`（restore + pause） |
| pbuf `+0x1c` 的 per-vpcid 位 | `reason |= 0x40` |
| scratchpad `+0x29d` != 0 | `reason |= 0x20` |
| `SR_GLOBALERRORSTATUS` / `SR_WARPERRORSTATUS` | 0x10 / 0x01 / 0x08 / 0x02 / 0x80 |

**实测（当前配置）**：

- `0317{1,0}` → R2 = **0x1**；
- 设备 writer 内核的 launch trap → R2 = **0x8**；
- 若 cmd=2 被读到，至少是 **0x30**（0x10|0x20）；0317 路径 pbuf=0，所以不是 0x30。

### 2.3 `do_read_trap_reason` 的立即数（驱动 patch）

| 偏移 | 含义 | 观测例（595.99） |
|---|---|---|
| +0x90/+0xa0 | scratchpad 低/高 32 位 | base+0x1a00000 |
| +0x120/+0x130 | **驱动 pbuf 低/高 32 位** | base−0x2000000 / −0x8000000 |
| +0x150 | per-SM 步长 | 0x630b0 |
| +0x380 | 偏移 | 0x290 |
| +0x340/0x350、+0x440/0x450 | scratchpad 副本 | 同 +0x90 |

**读法**：`MOV R4/R5, imm32`（opcode 0x7802）的 `imm` 在 w0 高 32 位；
低半 + 高半 << 32 组成 64 位地址。`XSCHED_SM120_TRAP_DUMP_READ=1` 可打印。

### 2.4 相关例程

| 例程 | 作用 |
|---|---|
| `do_pause` | 保存 warp 状态到 scratchpad，调试器停驻；0xA00 字节 |
| `do_pause_quiet` | 打标记 + BPT.DRAIN + 返回；外层循环反复调用 = 冻结 |
| `do_preempt_save` | 置标志后 JMP 进 `cilp_traphandler_save` |
| `do_preempt_restore` | 置标志后 JMP 进 `cilp_traphandler_restore` |
| `cilp_traphandler_save/restore` | **CILP 全量状态保存/恢复**（8704/8832/7808 B） |
| continuation 系列 | detect/set/handle/clear（256–512 B） |

**CILP**：驱动/硬件语境里的 context/interrupt/launch-pipeline 状态；
GDB 的 80 ms 设备挂起主要花在 save，恢复 8–10 ms。agent-only 0317 没有 save。

---

## 3. SASS 编码与 cave 工具箱

### 3.1 已验证编码（sm_120，16 B/条）

| 指令 | word0 | word1 |
|---|---|---|
| `MOV Rd, imm32` | `(imm<<32) \| (Rd<<16) \| 0x00007802` | `0x003fe20000000f00` |
| `MOV R3,RZ` | `0x000000ff00037202` | `0x003ff00000000f00` |
| `IADD R12,R12,0x10` | `0x000000100c0c7835` | `0x003fe600078e0000` |
| `RET.ABS R12,0x20` | `0x000000000c087950` | `0x003fc00003a00000` |
| `LD.E Rd,[Rb]` | `(Rb<<24) \| (Rd<<16) \| 0x7980` | `0x000ea20000100900` |
| `LD.E.CONSTANT Rd,[Rb]` | 同上 | `0x0032240000108900` |
| `ST.E [Rb],Rs` | `off<<32 \| (Rb<<24) \| 0x7385` | `0x000fe20000100900 \| Rs` |
| `STL.64 [0xffffa8],R0` | `0xffffa800ff007387` | `0x0033e40000100a00` |
| `LOP3 P0,RZ,R2,1` | `0x0000000102ff7812` | `0x003fda000780c0ff` |
| `@!P0 BRA +0x10` | `0x0000000000048947` | `0x003ff20003800000` |
| `@P0 EXIT` | `0x000000000000094d` | `0x000fea0003800000` |
| `EXIT` | `0x000000000000794d` | `0x000fea0003800000` |
| `CALL.ABS (T-0x100)>>8` | `((enc&0xffffffff)<<32) \| (o0&0xffffffff)` | `(o1&~0xff) \| ((enc>>32)&0xff)` |

**坑**：

- `ST.E` 的源寄存器在 **w1 低字节**；写错会当偏移/越界 → 700；
- `LDC` 读 64 位指针必须读低/高两半（`c[0x170]` + `c[0x174]`）；
- sm86 的 `JMP` 编码（`0x345678900000794a` / `0x000fea0003807f12`）在 sm120 上
  **无效**；用 `CALL.ABS` 或已验证的 `EXIT`/`BRA`；
- `RET.ABS` 前必须 `IADD R12,0x10`（否则回到 CALL 自身）。

### 3.2 cave 硬约束

1. **R1 绝对不能用**：handler 在 `+0x3a0` 用 `STL.64 [R1],R2`；
2. **R2 = reason，不要清零**；
3. **R3** 是被覆盖的 `MOV R3,RZ`，返回前最好恢复；
4. **R12** 是 LEPC/返回地址，返回前 `+0x10`；
5. 可自由用 **R4–R11**；
6. cave 只有 **8 条**（128 B），`cave+0x70` 是 EXIT trampoline；
7. 运行期 host 写 cave 会被驱动串行化；用 Init 预装或设备 writer（0.2 ms）。

### 3.3 已验证的 cave 形态

| 形态 | 内容 |
|---|---|
| return stub | `MOV R3,RZ; IADD R12; RET` |
| EXIT trampoline | `STL.64 [0xffffa8],R0`（改返回 PC 到 tramp）；tramp 里 `EXIT` |
| guard | `LDC R4/R5,c[0x170/0x174]; HFMA2 R10,0x5aa5; ST.E [R4],R10; ...` |
| signature | `MOV R8/R9,buf; HFMA2 R10,0x5aa5; ST.E [R8],R10; ST.E [R8+4],R2; ...` |
| reason probe | `LDC c[0x170]; ST.E [ptr],R2; ...` |
| pbuf read/write | `MOV R6/R7,tva; LD.E.CONSTANT/ST.E [R6]; MOV R8/R9,buf; ST.E [R8]; ...` |

---

## 4. debugger params（常量库）

`cuXtraSetDebuggerParams(func, args, 28)` 的 28 B 在 trap 常量库中：

| 常量 | 含义 | 实测值（受管 launch） |
|---|---|---|
| `c[0x0][0x170]` / `0x174` | XSched preempt buffer 指针（64 位） | `0x02000000`（低半） |
| `c[0x0][0x178]` / `0x17c` | guardian entry | `0xb6c00200`（低半） |
| `c[0x0][0x180]` / `0x184` | **kernel_idx**（64 位） | `0x1` |
| `c[0x0][0x188]` | **killable** | `0x1` |

- 这是 **XSched 自己的 buffer**，不是驱动 pbuf；
- handler 能读自己的 kernel_idx/killable；
- 但 host 不能在运行期改这些（launch-time 常量）；
- target/request 不在这些字段里。

---

## 5. 驱动 preemption buffer（pbuf）

### 5.1 布局

| 偏移 | 含义 | 值 |
|---|---|---|
| +0 | exit flag（32 位） | 非 0 = 退出 |
| +4 | command word（32 位） | **2** = preempt_save + do_pause；**1** = preempt_restore |
| +8 | preempt_idx（64 位） | 目标/记账索引 |
| +0x1c | per-vpcid continuation 位 | → reason 0x40 |

上游 `inject.cu` 的布局：`exit_flag | reserved | preempt_idx(64) | block0_exit | block0_restore | block1...`。

### 5.2 读写语义（✅ 实测）

| 操作 | 结果 |
|---|---|
| trap 内 `LD.E.CONSTANT` 读 pbuf | ✅（0317 陷阱可读；launch trap 读重定位地址会 700） |
| trap 内 `ST.E` 写 pbuf | 0317 陷阱 **不 fault**；但写 1/写 2 行为相同 ⇒ **不与驱动共享**（私有/快照） |
| host cuXtra 同步写 | 仅 Init "成功"；运行期 invalid argument |
| host CUDA 异步写 | libcuda SIGSEGV |
| 设备 kernel store | Warp Illegal Address |
| CPU mprotect + 写 | 零页/COW，驱动看不到 |

**0317 陷阱里 `+0/+4/+8` 全 0**（5060/5090）；`R2=0x1`，不是 cmd=2 的 0x30。
⇒ 驱动在 agent 发起的 0317 路径上**不写 pbuf**；cmd=2/idx 只在真调试器 suspend 路径写。

### 5.3 重定位

- 595.99 早先：`base−0x2000000`（无会话）；
- 595.99 会话态：`base−0x8000000`；
- 595.71 5090：trap 流程 `base−0x6000000`，kill 流程 `base−0x8000000`；
- **每次以 `do_read+0x120/0x130` 的 live 立即数为准**；不要用固定偏移。

---

## 6. scratchpad 与 reason tables

### 6.1 scratchpad（base+0x1a00000）

```
R0 = warp/SM 索引
R0 *= 0x2a0
R4 = scratch_base + R0
do_read:  LD.E.U8.CONSTANT R0, [R4 + 0x29d]
do_pause: ST.E ... [R4+0x290] / [R4+0x294] / [R4+0x298] / [R4+0x29e]
```

- per-warp 记录 **0x2a0 B**；驱动只用 `+0x290..+0x29e`；
- host 侧是 `rw-s /dev/nvidiactl` 映射（唯一 CPU 可写的 trap 相关区域）；
- trap 可见窗口只覆盖**活动的 per-warp 记录**；盲读 `+0x270/+0x300/+0x1000`
  在 launch trap 就 700；写会破坏 CILP 现场；
- `+0x29d` 是 defer-continuation 状态，不是请求/应答字段。

### 6.2 reason tables

| 表 | 相对地址 | 特性 |
|---|---|---|
| `trap_reason_table` | base+0xc38300 | host 可读写；**写它会破坏驱动 trap 逻辑**（下次 trap 700） |
| `pause_reason_table` | base+0x1000000 | 驱动自用；不要写 |

---

## 7. trap 上下文内存语义（汇总）

| 操作 | 结果 |
|---|---|
| 读普通全局内存（CUDA 分配 / XSched buffer / host-mapped UVA） | **恒 0** |
| 读代码页（cave/live） | 700 |
| 读带描述符的 `LDG.E` | 700 |
| 读常量库 `c[0x170..0x188]` | ✅ |
| 读 SR（`SR_GLOBALERRORSTATUS` 等） | ✅ |
| 读驱动 pbuf（`.CONSTANT`） | ✅（设备侧只读；0317 路径可读、全 0） |
| 读 scratchpad（活动 per-warp 窗口） | ✅ |
| 写普通全局内存 | ✅（host 回读一致） |
| 写 scratchpad（驱动布局内） | ✅ |
| 写驱动 pbuf | 0317 陷阱不 fault 但不共享；launch trap 700 |
| 写代码页 | 需 `cuXtraInstrMemcpyHtoD` / 设备 writer；运行期 host 写阻塞 |

**注意**：上表是 **debugger-suspend trap** 的语义；论文那种带内 trap 在 sm120 上不可达，
不能外推为"sm120 的 handler 在任何 trap 下都不能读全局"。

---

## 8. RM ABI

### 8.1 escape / 结构

| ioctl | 值 | 结构 |
|---|---|---|
| `NV_ESC_RM_ALLOC` | `0xc030462b` | `NVOS21`（48 B） |
| `NV_ESC_RM_CONTROL` | `0xc020462a` | `NVOS54`（32 B） |
| `NV_ESC_RM_FREE` | `0xc0104609` | `NVOS00`（16 B） |
| `NV_ESC_RM_DUP` | `0xc01c4634` | `NVOS55`（28 B） |
| `NV_ESC_RM_SHARE` | `0xc0184635` | `NVOS39`（24 B） |

```c
struct NVOS54 { NvU32 hClient, hObject, cmd, flags;
                NvP64 params; NvU32 paramsSize; NvS32 status; }; // 32 B
```

### 8.2 handles / classes

| class | 对象 |
|---|---|
| 0x41 | NV01_ROOT |
| 0x80 | NV01_DEVICE_0 |
| 0x2080 | NV20_SUBDEVICE_0 |
| 0xc96f | channel |
| 0xcec0 | KernelGraphicsObject（KGO） |
| 0x83de | KernelSMDebuggerSession |

### 8.3 `0x83de` 常用命令

| cmd | 名称 | psize | 备注 |
|---|---|---|---|
| 0301 | SMDebugModeEnable | 0 | 接受；不改变 park/cancel |
| 0302 | SMDebugModeDisable | 0 | 未测 |
| 0307 | SetModeMMUDebug | 4 | 接受 |
| 0309 | SetExceptionMask | 4 | 接受 |
| 030c | ReadAllSmErrorStates | 0x12d8 | 只读 |
| **0315/0316** | Read/WriteMemory | 0x18 | 仅 RM memobj，边界检查 |
| **0317** | DebugSuspendContext | 8 | 阻塞 / 0xffff / 0 |
| **0318** | DebugResumeContext | 0 | 所有 client 阻塞 |
| **031d** | DebugExecRegOps | 0xc88 | 被 `0x911` 挡 |
| 031f/032a | Errbar/MMU GCC 调试 | 4 | 接受 |
| 0321–0324 | 单 SM 单步/触发/运行/跳过空闲 | 8 | 未改语义 |
| 0329 | DropDeferredRC | 1 | 未测 |

### 8.4 会话构造与权限

`NV83DE_ALLOC_PARAMETERS_DEBUGGER`：
```c
{ hDebuggerClient_deprecated=0, hAppClient, hClass3dObject(KGO) }
```
- 跨 client 建会话需 `RS_ACCESS_DEBUG`；
- `os_check_access()` 对 DEBUG 永远 FALSE，只能靠 target 发 **`0xd04`** share policy；
- 构造后 `kgrctxRegisterKernelSMDebuggerSession` 挂到 `kgrctx+0x680`；
- **target 自己的 session 发 0317 是 no-op**；必须外部 agent（独立 client + 完整 libcuda）。

### 8.5 reg-op（两条路径）

| 路径 | cmd | 参数 |
|---|---|---|
| subdevice | `0x20800122` | 48 B：`hClient/hChannel/hObjectContext/field0c/field10/regOpCount/pRegOps` |
| session | `0x83de031d` | `{flags@0, regOpCount@4, RegOp32 ops[100]@8}` = 0xc88 |

`RegOp32`（32 B）：`regOp@0 | regStatus@1 | regQuad@2 | regAndNackIndex@4 |
regSubIndex@8 | 校验偏移@0x0a | regValue@0x10 | regMask@0x14 | regShift@0x18 | regGrpMask@0x1c`。

**gate**：两条路径都在执行前检查 `*(NvBool*)(pGpu + 0x911)`：

- subdevice：`0x2dd122 cmpb $0,0x911(%r12); je 0x2dd02a` → **NV_OK，跳过**；
- session：`0x21a9cc cmpb $0,0x911(%rbx)` → **0x56，不进执行**；
- `0x911` 只有两处写：`0x17376 movb $1`、`0x17b4c movb $0`，都在
  `__nvoc_init_dataField_OBJGPU`；
- `0x419e84` 不在 nv-kernel，而在 libcuda（595 反汇编 13 处）/libcuxtra.a（2 处）。

### 8.6 状态码

| code | 名字 |
|---|---|
| 0x1b | INSUFFICIENT_PERMISSIONS |
| 0x1f | INVALID_ARGUMENT |
| 0x23 | INVALID_CLIENT |
| 0x31 | INVALID_OBJECT |
| 0x33 | INVALID_OBJECT_HANDLE |
| 0x36 | INVALID_OBJECT_PARENT |
| 0x3a | INVALID_PARAM_STRUCT |
| 0x57 | OBJECT_NOT_FOUND |
| 0x6e | RESOURCE_LOST |

### 8.7 GSP RPC 符号（说明关键路径在固件）

`rpcCtrlPreempt`、`rpcCtrlGrCtxswPreemptionBind`、`rpcCtrlGrSetCtxswPreemptionMode`、
`rpcCtrlDbgSuspendContext`、`rpcCtrlDbgResumeContext`、
`kgrctxUnmap/FreeCtxPreemptionBuffers`。

---

## 9. 调试器栈

### 9.1 cuda-gdb

- 我们的 GDB backend 用 MI2：`-target-attach <pid>`、`-target-detach`、`-exec-continue`；
- 目标需 `PR_SET_PTRACER_ANY`（Yama/容器）；容器无 CAP_SYS_PTRACE 时这是必要条件；
- cuda-gdb 13.2 无 Python，MI 状态跟踪脆弱，命令要限速/显式匹配；
- 序列：`0307 → 0317 → 0315/030c/0316 → 0318`（与 cuxtra 触发是两条路径）。

### 9.2 libcuda 调试 API

导出：`cudbgApiInit/Attach/Detach`、`cudbgGetAPI/GetCUDADebuggerAPIVersion`、
`cudbgMain/cudbgPreInit/cudbgReport*`；
BSS 开关：`cudbgUseExternalDebugger`、`cudbgEnablePreemptionDebugging`、
`cudbgResumeForAttachDetach`、`cudbgEnableLaunchBlocking`、`cudbgIpcFlag`。

### 9.3 libcudadebugger / NvDebugAgent

- `libcudadebugger.so.1`（11.7 MB，stripped）导出 `DebugAgentMain`、
  `InitializeInjection`、`GetCUDADebuggerAPI`、`LaunchDebugAgentExecutable`；
- 字符串：`Failed to write preempt command`、`Failed to reset preempt command`、
  `Failed to get the preemption buffer layout`、`Wrong preemption buffer size`；
- RTTI：`NV::Debugger::IPreemptionBufferAccessor` / `NullPreemptionBufferAccessor`；
- **⇒ pbuf+4 的命令字由调试 agent 写**，驱动只提供 layout；
- `NvDebugAgent`（25 KB，带 DWARF，薄启动器）：
  `DebugAgentMain(debugger_pid, 142, 1, 1, 63, 72, -1)`；
- gdb↔agent 帧：`u64 len | u32 type(0xc) | u32 cmd | args…`，应答
  `u64 len | payload`（样本 `evidence/live/gdb_agent_protocol.log`）。

---

## 10. 版本/平台矩阵

| 项 | 5060 / 595.99.02 | 5090 / 595.71.05 | 3090 / 570 | A100 / 550 |
|---|---|---|---|---|
| sm | 120 | 120 | 86 | 80 |
| handler 入口 | base+0x15f0f00 | **同**（fingerprint MATCH） | 0x1200 对象 | 0x1200 对象 |
| slot20 stub/size | 0x880 / 0x1300 | 同 | — | — |
| do_pause 0xA00 | 0/160 mismatch | 同 | — | — |
| pbuf 观测 | −0x2000000 / −0x8000000 | −0x6000000 / −0x8000000 | 全局 preempt buffer | — |
| `0x911` | 0（关） | 0（关） | 开 | 开 |
| `0x20800122` | 被挡 | 被挡 | 可用 | 可用 |
| handler 插桩 | 自研 +0x390 / 上游 +0x880 | 自研可移植 | `+0x3e0 IADD3` | sm80 不在上游列表 |
| L3 结果 | GDB/0317 | GDB/0317（managed kill 未复现 0.234 s） | **229 µs、bad=0** | cuxtra 崩 / 慢路径 |

**换驱动/换卡检查清单**：

1. 跑 `find_live_handler --dry`，确认 slot20/arena/reloc；
2. 比对 live 入口 10 条指纹；
3. 用 `regops` 读 PTIMER/PMC（判断 gate）；
4. 检查 0317 四态（普通/受管/NO_ICTX/预 attach）；
5. 重新读 `do_read+0x120/0x130` 的 pbuf 地址；
6. 重跑 `run_l3kill` / `run_l3qspin`，确认 kill/replay。

---

## 11. 失败模式与坑

1. **R1**：cave 里用 R1 → `STL.64 [R1],R2` 越界 → 700；
2. **R2**：清零 reason → handler 重执行 fault 指令；
3. **R3**：不恢复 `MOV R3,RZ` 可能破坏后续分派；
4. **R12**：忘记 `IADD R12,0x10` → 回到 CALL 自身/死循环；
5. **ST.E 编码**：源寄存器在 w1 低字节，偏移在 w0 高 32 位；
6. **LDC 64 位**：只读低半会得到错误指针；
7. **sm86 JMP 编码**：在 sm120 上无效（700）；用 CALL.ABS/已验证 BRA；
8. **launch trap**：受管 launch 会有 reason=0x8；预置读 pbuf 的 cave 会在 launch 700；
9. **pbuf 在 launch trap 未映射**：0317 陷阱才可读；地址还会重定位；
10. **scratchpad 盲访**：只有活动 per-warp 窗口可读；写会破坏 CILP；
11. **trap_reason_table 写**：会破坏驱动 trap 逻辑，下一次 trap 700；
12. **会话存在时写代码/launch 被串行化**；运行期 arm 用设备 writer 或预装；
13. **0317 无应答会阻塞**；0318 在所有 client 阻塞；
14. **裸 EXIT**：从 cave/tramp 直接 EXIT 会破坏驱动的 trap 状态机 → 重放失败/700；
15. **A100/550**：cuxtra `cuXtraKernelGetFunction` SIGSEGV；sm80 handler 未支持；
16. **5090 managed kill**：`devflag arm=0xffffffff`、PARKED、release 失败；
    victim 提前结束但 0.234 s 未复现，需校准 pbuf/devflag。

---

## 12. 工具与复现

| 工具 | 用途 |
|---|---|
| `probe/find_live_handler` | 定位 slot20/live/handler、解码 reloc、指纹扫描 |
| `probe/regops` | 读 PTIMER/PMC、写 `0x419e84`（gate 测试） |
| `probe/l3x` + `probe/l3agent` | plain-context 活体 patch、0317、policy |
| `probe/l3qspin` | 受管 XQueue 内核、suspend/kill/replay、时间线 |
| `probe/latency` + `tools/measure_latency.sh` | GDB 延迟分解 |
| `tools/handover_test.sh` | 冻结让出 GPU |
| `run_l3kill.sh` / `run_l3qspin.sh` / `run_l3hal.sh` | 复现 kill / suspend / GDB backend |

常用环境变量：`XSCHED_SM120_L3_BACKEND=TRAP|GDB`、`XSCHED_SM120_TRAP_SHM/AGENT`、
`XSCHED_SM120_TRAP_CAVE`、`..._DUMP_READ`、`..._DUMP_RAW`、`..._DBGPARAM_PROBE`、
`..._ARM_PBUF_PROBE`、`..._ARM_SENTINEL`、`..._ARM_PBUF_WRITE`、`L3AGENT_*`、`L3Q_*`。

---

## 13. 术语

| 术语 | 含义 |
|---|---|
| **CILP** | context/interrupt/launch-pipeline 状态（调试器保存/恢复的核心） |
| **pbuf** | 驱动 preemption buffer；`+0/+4/+8` |
| **KGO** | KernelGraphicsObject（class 0xcec0） |
| **slot20** | tools export table 的第 20 项，返回 0x1300 镜像 |
| **cave** | 我们注入的 8 条 SASS（slot20 pc+0xA00） |
| **tramp** | cave+0x70 的 EXIT trampoline |
| **GSP** | GPU System Processor，RM 命令的固件侧执行者 |
| **RS_ACCESS_DEBUG** | 调试对象共享权限；靠 0xd04 授予 |
| **VSM/VPCID** | 虚拟 SM / warp 上下文 ID（scratchpad/reason 索引） |
| **TSG** | Timeslice Group（进程/上下文调度组） |

---

## 14. 证据索引

- `FINDINGS_ZH.md`：§1–§2（地址/分派）、§26–§31（InstrMemAllocator/park）、
  §35–§41（通道/scratchpad）、§42–§46（park 入口/调试栈）、§47（论文更正）、
  §49–§50（A/B）、§51（debugger params）、§52（pbuf）、§53（gate 静态）。
- `compendium/03-机制与逆向.md`、`RM_REVERSE_ZH.md`。
- `evidence/disasm/`（nvdisasm 反汇编）、`evidence/sections/`、`evidence/fp/`。
- `evidence/live/`（309 个现场日志）、`evidence/ab_ampere/`（52 个 A/B）、
  `evidence/rtx5090_595_71/`（campaign/drivers/bin）。
