# NVIDIA RM / CUDA 调试栈逆向参考（595.99.02 + RTX 5060 / sm120）

> 本文是 v2 阶段的**工具书**：把从 `nv-kernel.o_binary`、`libcuda.so.595.99.02`、
> `libcudadebugger.so.595.99.02` 和 ioctl 抓包里恢复出来的 ABI、命令表、状态码、
> 权限模型集中记录下来，附可复现的取证命令与地址，供后续实验直接引用。
> 实验结论/判定见 `FINDINGS_ZH.md`。

约定：所有 `0x2xxxxx` / `0x3xxxxx` / `0xaxxxxx` 地址都是
`/usr/src/nvidia-595.99.02/nvidia/nv-kernel.o_binary` 里的符号地址（该文件未 strip）；
`0x1xxxxx` / `0x2xxxxx` 的 libcuda 地址是 `/usr/lib/x86_64-linux-gnu/libcuda.so.595.99.02`
的 vaddr（= 文件偏移，该 so 的 .text vaddr 与 offset 一致）。

---

## 1. RM escape（ioctl req）全表

来源：`RmIoctlsTable.2 @ 0xaf98a0`（`{escape, paramSize}` 数组，20 项）。
ioctl req 编码 = `dir<<30 | size<<16 | 'F'(0x46)<<8 | nr`。

| nr | size | 含义 | 备注 |
|---|---|---|---|
| 0x27 | 0x38 | RM_ALLOC_MEMORY | |
| 0x28 | 0x14 | RM_ALLOC_OBJECT | |
| 0x29 | 0x10 | RM_FREE | `NVOS00 {hRoot,hParent,hObject,status}` |
| 0x2a | 0x20 | RM_CONTROL | `NVOS54` |
| 0x2b | 0x30 | RM_ALLOC | `NVOS21` |
| 0x34 | 0x1c | RM_DUP_OBJECT | `NVOS55` |
| **0x35** | **0x18** | **RM_SHARE** | `NVOS39`，见 §5 |
| 0x39 | 0x20 | | |
| 0x41 | 0x38 | | |
| 0x4a | 0xb8 | RM_VID_HEAP_CONTROL | |
| 0x4d | 0x48 | RM_ACCESS_REGISTRY | 注册表键读写 |
| 0x4e | 0x38 | RM_MAP_MEMORY | |
| 0x4f | 0x20 | RM_UNMAP_MEMORY | |
| 0x52 | 0x38 | RM_ALLOC_CONTEXT_DMA | |
| 0x57 | 0x40 | RM_MAP_MEMORY_DMA | |
| 0x58 | 0x30 | RM_UNMAP_MEMORY_DMA | |
| 0x59 | 0x10 | | |
| 0x5e | 0x28 | | |
| 0x5f | 0x3c34 | | 巨型批量控制 |
| 0xd1 | 0x0c | | |

`/dev/nvidiactl` 的非 RM escape（`nv-ioctl-numbers.h`，`NV_IOCTL_BASE=200=0xc8`）：

| nr | 名字 | size | 结构 |
|---|---|---|---|
| 0xc8 | NV_ESC_CARD_INFO | 2304 | `nv_ioctl_card_info_t[]` |
| 0xc9 | NV_ESC_REGISTER_FD | 4 | `{int ctl_fd}` —— 把新 fd 绑到已有 ctl fd 的 client 上 |
| 0xca | NV_ESC_ALLOC_OS_EVENT | 16 | |
| 0xd6 | NV_ESC_SYS_PARAMS | 8 | `{u64 memblock_size}`（本机 = 0x08000000） |
| 0xd7 | NV_ESC_WAIT_OPEN_COMPLETE | 560 | |

取证命令：

```bash
K=/usr/src/nvidia-595.99.02/nvidia/nv-kernel.o_binary
nm $K | grep RmIoctlsTable           # 0xaf98a0
# .rodata: vaddr 0x3d5f80 <-> file off 0x3d5fc0（差 0x40）
```

---

## 2. 关键 RM 结构体（实测校准）

```c
struct NVOS21 {                      // RM_ALLOC, 48 B —— 不要 packed
    NvU32 hRoot, hParent, hObject, hClass;
    NvP64 pAllocParms, pRightsRequested;   // pRightsRequested 是 RsAccessMask*
    NvU32 paramsSize, flags;
    NvS32 status;
};
struct NVOS54 {                      // RM_CONTROL, 32 B
    NvU32 hClient, hObject, cmd, flags;
    NvP64 params;
    NvU32 paramsSize;
    NvS32 status;
};
struct NVOS00 { NvU32 hRoot, hParent, hObject; NvS32 status; };        // RM_FREE, 16 B
struct NVOS55 {                      // RM_DUP_OBJECT, 28 B
    NvU32 hRoot, hParent, hObject, hClientSrc, hObjSrc, flags;
    NvS32 status;
};
```

`paramsSize == 0` 表示"用该 class 的默认参数尺寸"，**RM 仍然会解引用 `pAllocParms`**。
抓包时必须按 class 默认尺寸 dump，否则看不到内容（v2 第一版拦截器就漏了这一点）。

class 分配参数（抓包实测）：

```c
// class 0x80 NV01_DEVICE_0
struct NV0080_ALLOC_PARAMETERS {
    NvU32 hClientShare;      // 0
    NvU32 hTargetClient;     // 必须是自己的 client handle，留 0 → NV_ERR_INVALID_OBJECT_HANDLE(0x33)
    NvU32 hTargetDevice;     // 0
    NvU32 deviceId;          // 0
    ...                      // 全 0
};
// class 0x2080 NV20_SUBDEVICE_0
struct NV2080_ALLOC_PARAMETERS { NvU32 subDeviceId; };   // 0
// class 0x83de KERNEL_SM_DEBUGGER_SESSION
struct NV83DE_ALLOC_PARAMETERS_DEBUGGER {
    NvU32 hDebuggerClient_deprecated;   // 必须为 0，非 0 直接构造失败
    NvU32 hAppClient;                   // 被调试应用的 client handle
    NvU32 hClass3dObject;               // 被调试的 KernelGraphicsObject（class 0xcec0）
};
```

---

## 3. `KernelSMDebuggerSession`（class 0x83de）控制命令全表

来源：`__nvoc_exported_method_def_KernelSMDebuggerSession @ 0xa870e0`，
表项 40 B：`{pFunc@0, ?@8, ?@12, cmd@16, paramSize@20, classDef@24, paramInfoStr@32}`。
`pFunc == NULL` 的是 nvoc 自动生成的存取器（只改对象内字段，不落到硬件）。

| cmd | 名字 | psize | flags | IMPL |
|---|---|---|---|---|
| 0x83de0301 | SMDebugModeEnable | 0 | 0x48 | （自动生成） |
| 0x83de0302 | SMDebugModeDisable | 0 | 0x48 | （自动生成） |
| 0x83de0307 | DebugSetModeMMUDebug | 4 | 0x10248 | （自动生成/转发） |
| 0x83de0308 | DebugGetModeMMUDebug | 4 | 0x10248 | |
| 0x83de0309 | DebugSetExceptionMask | 4 | 0x10248 | |
| 0x83de030b | DebugReadSingleSmErrorState | 0x38 | 0x10248 | |
| 0x83de030c | DebugReadAllSmErrorStates | 0x12d8 | 0x50048 | `...ReadAllSmErrorStates_IMPL@0x21ad50` |
| 0x83de030f | DebugClearSingleSmErrorState | 8 | 0x10248 | |
| 0x83de0310 | DebugClearAllSmErrorStates | 8 | 0x50048 | `...ClearAllSmErrorStates_IMPL@0x21aed0` |
| 0x83de0313 | DebugSetNextStopTriggerType | 4 | 0x10248 | |
| 0x83de0314 | DebugSetSingleStepInterruptHandling | 4 | 0x48 | |
| **0x83de0315** | **DebugReadMemory** | 0x18 | 0x8 | `...ReadMemory_IMPL@0x21a8e0` |
| **0x83de0316** | **DebugWriteMemory** | 0x18 | 0x8 | `...WriteMemory_IMPL@0x21a940` |
| **0x83de0317** | **DebugSuspendContext** | 8 | 0x10248 | 转发 GSP |
| **0x83de0318** | **DebugResumeContext** | 0 | 0x10248 | 转发 GSP |
| 0x83de031a | ReadSurface | 0x608 | 0x8 | `...ReadSurface_IMPL@0x21a720` |
| 0x83de031b | WriteSurface | 0x608 | 0x8 | `...WriteSurface_IMPL@0x21a730` |
| 0x83de031c | GetMappings | 0x418 | 0x8 | `...GetMappings_IMPL@0x21a740` |
| **0x83de031d** | **DebugExecRegOps** | 0xc88 | 0x10208 | `...ExecRegOps_IMPL@0x21a990` |
| 0x83de031f | DebugSetModeErrbarDebug | 4 | 0x10248 | |
| 0x83de0320 | DebugGetModeErrbarDebug | 4 | 0x48 | |
| 0x83de0321 | DebugSetSingleSmSingleStep | 8 | 0x10248 | |
| 0x83de0322 | DebugSetSingleSmStopTrigger | 8 | 0x48 | |
| 0x83de0323 | DebugSetSingleSmRunTrigger | 8 | 0x48 | |
| 0x83de0324 | DebugSetSingleSmSkipIdleWarpDetect | 8 | 0x48 | |
| 0x83de0325 | DebugGetSingleSmDebuggerStatus | 0xc | 0x48 | |
| 0x83de0326 | DebugReadBatchMemory | 0xe20 | 0x8 | `...ReadBatchMemory_IMPL@0x21aa70` |
| 0x83de0327 | DebugWriteBatchMemory | 0xe20 | 0x8 | `...WriteBatchMemory_IMPL@0x21abe0` |
| 0x83de0328 | DebugReadMMUFaultInfo | 0x48 | 0x8 | `...ReadMMUFaultInfo_IMPL@0x21b050` |
| 0x83de0329 | DebugSetDropDeferredRC | 1 | 0x48 | |
| 0x83de032a | DebugSetModeMMUGccDebug | 4 | 0x248 | |
| 0x83de032b | DebugGetModeMMUGccDebug | 4 | 0x248 | |

其它相关 IMPL：`ksmdbgssnConstruct_IMPL@0x219030`、`ksmdbgssnDestruct_IMPL@0x219950`、
`ksmdbgssnInternalControlForward_IMPL@0x219b70`、
`kgrctxRegisterKernelSMDebuggerSession_IMPL@0x211660`、
`kgrctxDeregisterKernelSMDebuggerSession_IMPL@0x211690`。

`0x20800122 = NV2080_CTRL_CMD_GPU_EXEC_REG_OPS`，psize 0x30，flags 0x10118，
IMPL `subdeviceCtrlCmdGpuExecRegOps_IMPL@0x2dd410` →
`subdeviceCtrlCmdGpuExecRegOps_cmn.constprop.0@0x2dcfc0`（来源：
`__nvoc_exported_method_def_Subdevice @ 0xa9c060`）。

### 3.1 reg-op 编码（两条路径共用）

`gpuValidateRegOps@0x2dd370` 与 `..._cmn@0x2dcfc0` 都以 **32 B 步长**遍历 op 数组，
校验 `*(NvU16*)(op + 0x0a)`（经 `gpuValidateRegOffset_IMPL`），失败时
`*(NvU8*)(op + 0) |= 0x04` 置 `regStatus`；只有当"flags 字节"为 0 时才立刻返回
`NV_ERR_INVALID_ARGUMENT(0x1f)`，否则继续跑完。

```c
struct RegOp32 {                 // 32 B
    NvU8  regOp;                 // +0x00  type|op，失败时 |= 0x04
    NvU8  regStatus;             // +0x01
    NvU16 regQuad;               // +0x02
    NvU32 regAndNackIndex;       // +0x04  寄存器地址
    NvU32 regSubIndex;           // +0x08
    NvU16 (校验用的 16 位偏移)    // +0x0a  ← gpuValidateRegOffset 的入参
    ...                          // +0x0c regValue / regMask / regShift / regGrpMask
};
// 0x83de031d 参数：{NvU8 flagsByte@0; NvU32 regOpCount@4; RegOp32 ops[100]@8} = 0xc88
//                 regOpCount 上限 0x64
// 0x20800122 参数（48 B）：{hClient@0, hChannel@4, hObjectContext@8, ...,
//                 regOpCount@0x14, pRegOps@0x18(NvP64)}；
//                 hChannel 与 hObjectContext 必须同为 0 或同非 0
```

**两条路径都被 `*(NvBool*)(pGpu + 0x911)` 一票否决**：为 false 时直接 `return NV_OK`
而不执行任何 op（`0x2194cc`、`0x2dd122`）。`pGpu + 0x910` 是另一个标志字
（`testl $0xffff00, 0x910(%r12)`）。**这两个字段是后续要攻的目标。**

实测（`evidence/live/exp_regops2.log`）：`0x20800122` 用我们自己的 subdevice +
libcuda 的 `{hChannel=0x5c00001a, hObjectContext=0x5c00001b}` 调用 → **status 0**
（权限已通过），但 `regStatus/regValue` 全 0，即卡在 `pGpu+0x911` 的早退上；
`hChannel/hObjectContext` 同时为 0 → `0x3a NV_ERR_INVALID_PARAM_STRUCT`。

---

## 4. NV 状态码速查（`common/inc/nvstatuscodes.h`）

| code | 名字 | 本项目里出现的位置 |
|---|---|---|
| 0x1b | NV_ERR_INSUFFICIENT_PERMISSIONS | 跨 client 建 0x83de 会话（缺 RS_ACCESS_DEBUG） |
| 0x1f | NV_ERR_INVALID_ARGUMENT | ExecRegOps flags 字节为 0 |
| 0x23 | NV_ERR_INVALID_CLIENT | |
| 0x31 | NV_ERR_INVALID_OBJECT | hClass3dObject 不是 KernelGraphicsObject |
| 0x33 | NV_ERR_INVALID_OBJECT_HANDLE | 自建 client 分配 class 0x80；同进程跨 client DUP |
| 0x36 | NV_ERR_INVALID_OBJECT_PARENT | 0x83de 挂在 subdevice 下 |
| 0x3a | NV_ERR_INVALID_PARAM_STRUCT | ExecRegOps regOpCount = 0 |
| 0x57 | NV_ERR_OBJECT_NOT_FOUND | 扫 parent 时的常见返回 |
| 0x6e | NV_ERR_RESOURCE_LOST | 会话销毁后再发 0x83de031f |

---

## 5. RM 权限模型（为什么"复刻调试器"这么难）

### 5.1 会话构造的检查链

`ksmdbgssnConstruct_IMPL@0x219030`：

```
pParams = *(void**)(allocParams + 0x40);
if (pParams->hDebuggerClient_deprecated != 0) → 失败;         // +0x00
hAppClient     = pParams->hAppClient;                          // +0x04
hClass3dObject = pParams->hClass3dObject;                      // +0x08
if (hAppClient == 0) hAppClient = 调用者自己的 client;
serverGetClientUnderLock(g_resServ, hAppClient, &pAppClient);
clientGetResourceRef_IMPL(pAppClient, hClass3dObject, &pRef);  // 必须是 0xcec0 对象
if (!hypervisorIsVgxHyper())
    → rsAccessCheckRights(pRef, pInvokingClient, mask = 1<<2); // 需要 RS_ACCESS_DEBUG
       失败 → 0x1b
... kgrctxRegisterKernelSMDebuggerSession_IMPL(...)            // 绑定 GR context
```

### 5.2 权利从哪里来

- `rsAccessCheckRights@0x3b4f50` → `rsAccessGetAvailableRights@0x3b4eb0`：
  - 调用者 client == 资源 owner → 直接拷贝 `pResource+0x10c`（owner 的全量权利）；
  - 否则 → `_rsAccessGetSharedRights@0x3b4a70`：遍历 **资源自身策略表**（`res+0xe0`，
    沿 `res+0x18` 向上找第一个 `res+0x108 != 0` 的祖先）+ **全局策略表**，
    策略项 12 B：`{? @0, mask(u32) @4, targetType(u16) @8, action(u8) @10}`，
    `action & 2` 为 DENY，最终 `available = (init | grant) & ~deny`。
- `serverInitGlobalSharePolicies@0x35e3f0` 只装了 **3 条全局策略，mask 全是
  RS_ACCESS_NICE(1)**：GRANT(type=4)、DENY(type=5, action=2)、GRANT(type=7)。
  ⇒ **RS_ACCESS_DEBUG(bit2) 从来不在全局授权里。**
- `rsAccessUpdateRights@0x3b4e10` 循环 `right = 0..3`（⇒ **RS_ACCESS_COUNT == 4**），
  对 `g_rsAccessMetadata[right].flags & 8` 的权利调用回调，回调最终落到
  `os_check_access()`。
- `os_check_access()`（**开源层**，`/usr/src/nvidia-595.99.02/nvidia/os-interface.c:384`）：

```c
NvBool os_check_access(RsAccessRight r) {
    switch (r) {
    case RS_ACCESS_PERFMON: return capable(CAP_PERFMON);   // 或 CAP_SYS_ADMIN
    case RS_ACCESS_NICE:    return capable(CAP_SYS_NICE);
    default:                return NV_FALSE;               // ← RS_ACCESS_DEBUG 永远 FALSE
    }
}
```

⇒ **DEBUG 权利不可能靠 capability 拿到**，只能靠资源上的显式 share policy。
授权入口只有两个：
1. `_clientAllocResourceHelper+0x626` → `rsAccessGrantRights`，来源是
   `NVOS21.pRightsRequested`（抓包里 libcuda / cuda-gdb 全部传 0）；
2. `Nv04ShareKernel@0x339e90` → `_nv04Share@0x339a70` → escape **0x35 (NV_ESC_RM_SHARE, 24 B)**
   → `rmapiShareWithSecInfoTls@0x35e2c0` / `clientShareResource_IMPL@0x3b5960`。
   `_nv04Share` 里：非特权调用者填 `{2, 1}`，特权调用者填 `{osIsAdministrator(), 0}`
   —— 那个 `2` 极可能就是 **RS_ACCESS_DEBUG** 的权利索引，`1` 是 GRANT 动作。
   **抓包中 cuda-gdb 与目标进程都没有调用过 0x35**，所以这条路是否就是答案仍未证实。

### 5.3 实测封锁点

| 尝试 | 结果 |
|---|---|
| 自建 client（class 0x41）→ class 0x80 device | **0x33**，11 种变体全败（新 fd ± CARD_INFO ± SYS_PARAMS ± 0xcb33 ± client 级控制前缀 / libcuda 的 fd / psize 0,4,56 / 不同 handle 风格） |
| libcuda 的 client 下再建一个 class 0x80 device | **0**（唯一可行） |
| 同进程内把 app 的 device `RM_DUP_OBJECT` 到自建 client | **0x33** |
| 跨进程（fork + 先 PTRACE_ATTACH 再 cuInit）建 0x83de，hAppClient=目标 client | **0x1b** |
| 同进程 0x83de（hAppClient=自己）+ 031f/0307/032a=1 + 0317{1,0} | status 0，**设备不冻结** |
| 同进程 0x83de 绑定全部 8 个 compute object 逐个试 | 全部 status 0，**全部不冻结** |
| hClass3dObject = channel / TSG / device / subdev / session | **0x31**（只接受 0xcec0） |

---

## 6. cuda-gdb 真实 suspend 的完整 RM 时序（黄金参考）

抓包：`evidence/live/r3_ioctl.log.20138`（目标进程）、`r3_ioctl.log.20144`（debug agent 进程）、
`r3_watch.log`（外部 /dev/shm 观察者）、`r3_gdb.log`。
复现命令：`./run_r1.sh r3 8`。

**目标进程（client 0xc1d00150，fd 41）对象树**：

```
0xc1d00150  class 0x41    client
└─ 0x5c000002  class 0x80    device      params {hClientShare=0, hTargetClient=0xc1d00150}
   ├─ 0x5c000003  class 0x2080  subdevice
   ├─ 0x5c000013  class 0xa06c  TSG
   │  └─ 0x5c00001a  class 0xc96f  channel
   │     ├─ 0x5c00001b  class 0xcec0  KernelGraphicsObject   ← hClass3dObject
   │     └─ 0x5c00001c  class 0xcab5
   ├─ ...（共 8 个 0xcec0、16 个 0xc96f、3 个 0xa06c）
   └─ 0x5c000073  class 0x83de  session  params {0, 0xc1d00150, 0x5c00001b}
                                   └─ 应用自己也会建一个，只用过一次 0x83de0309
```

**debug agent 进程（client 0xc1d0014c，fd 35；自己跑了一遍完整 libcuda 初始化）**：

```
t=0.208207  RM_CONTROL 0x20803601 (subdev, psize 72, 含 "595.99.02")
t=0.211742  RM_ALLOC   0x83de  obj 0x5c00000c  parent=自己的 device 0x5c000002
            params = {hDebuggerClient=0, hAppClient=0xc1d00150, hClass3dObject=0x5c00001b}
t=0.212184  RM_CONTROL 0x83de031f = 1                (SetModeErrbarDebug)
t=0.216462  fd60: NV_ESC_REGISTER_FD {ctl_fd=35}
t=0.216473  fd60: NV_ESC_ALLOC_OS_EVENT
t=0.216947  fd60: RM_ALLOC class 0x79  obj 0x5c00000d  parent=会话 0x5c00000c
t=0.217734  RM_CONTROL 0x83de0307 = 1                (SetModeMMUDebug)
t=0.218311  RM_CONTROL 0x83de032a = 1                (SetModeMMUGccDebug)
t=0.219000  RM_CONTROL 0x83de0317 {0,0}              ← 轮询式 suspend
t=0.219034  RM_DUP_OBJECT  0x5c00000e ← app 0x5c000074
t=0.222618  RM_CONTROL 0x83de0315 {0x5c00000e, 0x005eb010, ...}   (ReadMemory)
t=0.222656  RM_DUP_OBJECT  0x5c00000f ← app 0x5c000069
t=0.222686  RM_CONTROL 0x83de0315 {0x5c00000f, 0x00001680, ...}
t=0.223018  RM_CONTROL 0x83de030c (psize 4824)       (ReadAllSmErrorStates)
t=0.224327  RM_DUP_OBJECT  0x5c000010 ← app 0x5c000080
t=0.225228  RM_CONTROL 0x83de0316 {0x5c000010, 0x4000, ...}        (WriteMemory ← 下载 plugin)
t=0.225994  RM_CONTROL 0x83de0316 {0x5c00000e, 0x005eb010, ...}
t=0.227055  RM_CONTROL 0x83de0318                    (ResumeContext)
            …… 上述 0317{0,0}/0315/030c/0316/0318 循环 11 轮 ……
t=3.351919  RM_CONTROL 0x83de0317 {1,0}              ← ★真正的冻结，之后没有 0318
t=13.367395 RM_CONTROL 0x83de0318                    ← 10.02 s 后恢复（= gdb 的 shell sleep 10）
t=17.221703 RM_CONTROL 0x83de031f = 0  → status 0x6e (RESOURCE_LOST)
```

外部观察者（`r3_watch.log`）在冻结窗口里计数值钉死在 `110052096`，
恢复前 36.7 M/s、恢复后立刻回到 36.7 M/s ⇒ **确实是运行中内核被整机暂停**。

`0x83de0317` 的 8 字节参数：`{0,0}` = 轮询（~7 ms 后就 resume），
`{1,0}` = 挂起并保持。**两次调用的字节完全相同的部分我们已经能发出，
差异只在会话的归属。**

**目标进程的 RM 流在"被调试"与"不被调试"两种情况下逐条一致**
（`plain_ioctl.log.20403` vs `r3_ioctl.log.20138`：class 计数、命令集合、
唯一一次 `0x83de0309` 全部相同）⇒ 目标侧没有任何"授权调试器"的 RM 动作，
授权判定完全发生在 RM 内部（进程身份/ptrace/share policy），这是当前唯一的未知项。

---

## 7. libcuda / libcudadebugger 的调试入口

libcuda 导出的 `T` 符号（`nm -D libcuda.so.595.99.02`）：

| 符号 | vaddr | 说明 |
|---|---|---|
| `cudbgGetAPI` | 0x221280 | 4 个 int/指针入参，经 `*(0x578fba0)` 间接跳转；该指针为 NULL 时返回 0xa |
| `cudbgGetAPIVersion` | 0x221300 | |
| `cudbgApiInit` | 0x223330 | `cudbgApiInit(1)`→起线程跑 `sub_222f50(1,0)`；`(2)`→`sub_222f50(2,1)`；其它值报错 |
| `cudbgApiAttach` | 0x223410 | 无参，同样起线程跑 `sub_222f50(1,0)`，`pthread_join` 等待 |
| `cudbgApiDetach` | 0x2234c0 | 检查 `cudbgUseExternalDebugger(0x574f1b0)`，经 `*(0x5746f90)` 间接调用 (0x14, 5, 0) |
| `cudbgMain` / `cudbgPreInit` | | |

BSS 标志：`cudbgUseExternalDebugger@0x574f1b0`、`cudbgDebuggerInitialized@0x578fb80`、
`cudbgEnablePreemptionDebugging@0x57a4d14`、`cudbgInjectionPath@0x578fbc0`、
`cudbgDebuggerCapabilities@0x578fb60`、`cudbgReportedDriverInternalErrorCode@0x578fb88`。
相关字符串：`CUDBG_INJECTION_PATH`、`CUDA_INJECTION64_PATH`、`/cuda_injection_path_shm`、
`InitializeInjection`。

`sub_222f50@0x222f50`（attach 过程主体）：`edi==0` 分支处理 injection path；
`edi!=0` 分支遍历 `*(0x5747420 + 8*i)`（i < `*(0x5747620)`）里的 context 对象，
调用 `ctx->vtbl[0x8ee8/8]()` 并检查 `ctx+0x9410`，即"把已存在的 context 逐个切进调试态"。

libcudadebugger.so.595.99.02（11.7 MB）：

| 符号 | vaddr | 说明 |
|---|---|---|
| `InitializeInjection` | 0x1246d0 | **无参**（忽略 rdi）；失败时 `raise(SIGTRAP)` |
| `GetCUDADebuggerAPI` | 0x149ac0 → 0x16e4b0 | 返回客户端 API 表（cuda-gdb 用的那张） |
| `GetCUDADebuggerAPIVersion` | 0x14b9d0 | |
| `DebugAgentMain` | 0x1529c0 | 目标内 debug agent 主循环 |
| `LaunchDebugAgentExecutable` | 0x14d000 | 由调试器侧拉起 agent 进程 |

该库 **不 import 任何 `cudbg*` 符号、也不含 `cudbg*` 字符串**，说明它是通过
dlopen/dlsym 动态取 libcuda 的入口；内部类名 `NV8Debugger::CudaInjectionDebuggerPlugin`
以及 `DebugAgentProcess.c` 的路径字符串表明它就是 cuda-gdb 的"注入式调试插件"。
错误/事件枚举字符串齐全（`CUDBG_ERROR_*` 50 个、`CUDBG_EVENT_*` 20 个、
`CUDBG_EXCEPTION_*`），可用来反推 API 表槽位。

---

## 8. 复现命令清单

```bash
cd xsched/test/sm120_l3_trap_v2
./build.sh                                   # 构建 probe/ 下全部探针
./run_r1.sh r3 8                             # cuda-gdb 真实 suspend 的 ioctl 抓包 + 外部观察者
./run_l3x.sh x4                              # fork+ptrace 的跨进程 0x83de 扫描
LD_PRELOAD=$PWD/bin/ioctl_log6.so IOCTL_LOG=/tmp/plain ./bin/shm_spin /s 3   # 无调试器基线

# 从 nv-kernel.o_binary 里再取一次 0x83de 命令表
K=/usr/src/nvidia-595.99.02/nvidia/nv-kernel.o_binary
nm $K | grep exported_method_def_KernelSMDebuggerSession     # 0xa870e0
objdump -dr --start-address=0x219030 --stop-address=0x219950 $K   # 会话构造 + 权限检查
objdump -dr --start-address=0x3b4a70 --stop-address=0x3b4f50 $K   # rsAccess 权利计算
sed -n '384,405p' /usr/src/nvidia-595.99.02/nvidia/os-interface.c # os_check_access
```
