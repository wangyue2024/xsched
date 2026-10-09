# XSched sm120 Level-3 抢占开发报告（Linux）

> 版本：2026-10-09　分支：`sm120-level3-probe`　硬件：NVIDIA RTX 5060（sm120 / Blackwell，CC 12.0）
> 驱动：595.99.02（CUDA 13.2，开源内核模块）　系统：Linux，GPU 同时驱动显示
> 关联文档：`test/sm120_l3_probe/README.md`（英文，含 Windows 探针与 Linux 两轮实验原始记录）
> 证据目录：`test/sm120_l3_probe/evidence/`（Windows 探针）与 `evidence/linux_2026-10-09/`（Linux 实验）

---

## 0. 摘要

本报告记录了在 Linux + RTX 5060（sm120）上实现 XSched Level-3（L3，运行中内核的微秒级抢占）的完整开发过程：从工具链搭建、驱动接口逆向、handler 重写实现，到端到端验证与根因定位。

**总的状态**：

| 环节 | 结果 |
|---|---|
| 工具链（nvcc 12.9 复现指令数组） | ✅ 与已提交版本逐位一致 |
| sm120 trap handler 获取（GetInfoBlackwell 缺口绕过） | ✅ 读出 4864B 并全量反汇编 |
| handler 补丁写入/读回 | ✅ 字节级一致 |
| RM 触发（`cuXtraTriggerTrap`） | ✅ 空闲/忙碌均成功、上下文健康 |
| **强制 trap → 执行被 patch 的 handler** | ⛔ **不执行**（0x0/0x60/0x880 三处签名载荷、真实 brkpt trap、cuda-gdb 会话下均不执行） |
| TSG 路径（`cuXtraSetTimeslice`） | ⚠️ 写入真实有效，但语义与收益受限于本场景 |

**根因（已定位）**：sm86 时代的"patch 槽 20 trap-handler 内存对象 + 写 RM 寄存器位触发"方案在 595 驱动上已不可行。该内存对象是**非活动模板**；真正的 trap 激活收敛进了 **CUDA 调试器 attach 栈**（专用 RM client + class 0x83de 调试对象 + `0x83de03xx` 方法族 + libcuda `cudbg*` 钩子 + protobuf agent 协议）。本轮已用 RM ioctl 拦截器把该机制测绘为"有完整报文清单的白盒"，为后续攻克留下全部原始材料。

---

## 1. 背景与原理

### 1.1 XSched 与三层抢占模型

XSched（OSDI'25）是面向多厂商 XPU 的抢占式调度框架。四个核心组件：

- **XShim（shim）**：冒充 `nvcuda.dll` / `libcuda.so` 拦截驱动 API，透明接管应用命令；
- **XPreempt（preempt）**：XQueue 可抢占命令队列 + agent（上报队列状态事件）；
- **XAL（hal）**：硬件适配层，实现多级硬件模型（guardian / trap / TSG）；
- **XScheduler（xserver）+ xcli**：全局调度服务与命令行监控。

三层抢占模型（`include/xsched/types.h`）：

| 级别 | 名称 | 机制 | 时延量级 |
|---|---|---|---|
| L1 | Block | 软件缓冲命令，控制物理下发时机 | 提交级 |
| L2 | Deactivate | 撤回"已提交未执行"的命令（CUDA 用 guardian） | 毫秒级 |
| L3 | Interrupt | 中断/换出**正在执行**的内核 | 微秒级 |

### 1.2 CUDA 平台 L3 的两条路径

**Trap 路径**（本报告主线）：

1. 经隐藏接口 `cuGetExportTable(&table, &ETID)` 获得驱动的 TrapHandler 导出表；
2. 用 `EtblTrapHandler` 的 getter（sm86 为 slot 19 `GetInfoPascal`）拿到 trap handler 的设备地址/尺寸；
3. 把 handler 的"即将恢复执行"处 patch 成跳转（sm86 在偏移 `0x3e0`），注入一段"检查抢占标志 → 杀 block 或恢复"的 SASS 代码（`check_preempt_trap`）；
4. 通过 `cuXtraTriggerTrap`（RM 控制写全局寄存器 `0x419e84` bit31 = TRIGGER_TRAP）强制触发硬件 trap，所有在跑 warp 进入 handler → 执行注入代码 → `EXIT` 杀掉 warp。

**TSG 路径**：`XSCHED_CUDA_LV3_IMPL=TSG`，用 `cuXtraGet/SetTimeslice(ctx, us)` 调整通道时间片实现抢占，架构无关。

### 1.3 guardian（L2，已量产验证）简况

sm120 的 L2 用 `GuardianSM120`（guardian 50 条 + resume 32 条指令数组）拼接到内核入口，通过 debugger 参数窗口 `c[0x0][0x170..0x188]`（sm120 与 sm86 的 `0x1880` 系不同）传递抢占缓冲区指针、跳回点与 killable 标志。本报告工作建立在其已实机验证的基础上。

---

## 2. 开发环境

| 组件 | 说明 |
|---|---|
| GPU / 驱动 | RTX 5060 (sm120)，595.99.02，开源内核模块，`/dev/nvidiactl` 0666 |
| CUDA Toolkit | `/home/wy/cuda-12.9`（12.9.41），需 `-ccbin g++-13`（gcc 15 超支持范围） |
| 兼容补丁 | 本地 `math_functions.h` 对 glibc-C23（≥2.41）新增的 `sinpi/cospi/rsqrt` 系列加 `noexcept` 兼容宏（已备份 `.bak`） |
| 构建 | `cmake -DBUILD_TEST=ON -DPLATFORM_CUDA=ON -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_CUDA_HOST_COMPILER=g++-13 -DCMAKE_CUDA_COMPILER=/home/wy/cuda-12.9/bin/nvcc`，`NVCCFLAGS="-arch=sm_120"` |
| shim 注入 | exFAT 不支持软链，故 `cp output/lib/libshimcuda.so /tmp/xsched_shim/libcuda.so.1`，运行设 `LD_LIBRARY_PATH=/tmp/xsched_shim` |
| 驱动库指引 | `XSCHED_CUDA_LIB=CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1`（必须显式指向真实驱动，避免自加载） |
| 调试器 | 系统自带 `cuda-gdb 12.4`（`/usr/bin/cuda-gdb`），驱动 595 下可正常 attach/运行 |

---

## 3. 前情：Windows 探针（commit ff14d06）结论提要

在 Windows + WDDM 下的逆向探针（`test/sm120_l3_probe/README.md`）已确认：

- TrapHandler 导出表在 sm120 上仍存在（184B / 23 slots）；`cuGetExportTable`（ordinal 204）可调用；
- **slot 19（GetInfoPascal）返回 error 101**（缺 Blackwell 分支）；**slot 20 是事实上的 `GetInfoBlackwell`**：`fn(ctx, out40)` → `+0x10=memObjHandle、+0x18=0x880、+0x20=0x940`；
- Windows 上 trigger 被 cuxtra 的 Linux-only `CudaKernelModule` 阻断（fatal）；TSG 同样被阻断；`cuCheckpointProcess` 返回 801；
- 结论：到 Linux 上验证完整 L3。

---

## 4. Linux 阶段一：实现与逐环验证

### 4.1 工具链复现（T1–T2）

- `make ARCH=120 bin dump cc`（nvcc 12.9.41）生成 `inject_120.cubin/asm`；
- `extract_sass.py generate` 输出的 guardian（50 条）/ resume（32 条）数组与仓库已提交版本**逐位（bit-exact）一致**——工具链在 Linux 上完全可复现；
- trap 路径数组生成：`check_preempt_trap`（56 条，裁后 40 条）、`exit_if_idempotent`（40 条）；所有工具生成编码（LDC/STL/LDL/IMAD.MOV/@P0 MOV/ISETP）经 nvdisasm 12.9 在 sm_120 上全部可正确解码。

### 4.2 GetInfoBlackwell 绕过（已验证可用）

`cuXtraGetTrapHandlerInfo` 在 sm120 报 `error 101 @ trap.cpp:20`（与 Windows 相同）。通过静态逆向 Linux 版 cuxtra 弄清其内部流程后，在 xsched 侧实现了等效路径（`TarpHandlerSM120::GetTrapHandlerInfo`）：

```
1) Driver::GetExportTable(&table, CU_ETID_ToolsTrapHandler)
      GUID = cc529e94-5c4e-9d46-837c-9625868334e4
2) table[20](ctx, &out40)  -> { flags, struct_size=0x28, memObjHandle@+0x10,
                                stub_offset=0x880@+0x18, 0x940@+0x20 }
3) Driver::GetExportTable(&mt, CU_ETID_ToolsMemory)
      GUID = bfdb432d-bf3c-5a4a-945e-b34029e81e75
4) ObjGetPc(ctx, memObjHandle, &pc)    // slot 24
5) ObjGetSize(memObjHandle, &sz)       // slot 25
      -> pc = 0x…5f5300, sz = 0x1300（4864 字节）
```

**关键细节（两次踩坑）**：
- `memObjHandle` 是**指向驱动分配结构的指针**，必须把"值本身当指针"传入；传其拷贝（`&copy`）会得到 0；
- 用 `cuXtraMemcpyDtoH/HtoD` 对 `pc`（设备 VA，非 handle）做读写即可，回环读回字节级一致。

### 4.3 trap handler 逆向（本次最重要的结构性成果）

对 `pc` 处 4864 字节做"cubin 包装 + nvdisasm"反汇编（方法：把原始 SASS 写入一个占位函数的 `.text` 段再解码，见证据目录），得到完整布局：

```
偏移        内容
0x000–0x880  变体 A：handler 主体
             入 口: LOP3 P0 = R2 & 0x20; @P0 BRA.U 0x880（快速返回路径）
             保存序列: S2R/LDC/LDL 采集 VIRTID/SMEMSZ/CTAID/ESR_PC 等 → ST.E [R4+…]
0x880–0xA00  共享返回桩（两条路径必经）：
             NOP, NOP, CCTLL.IVALL, CCTL.IVALL, MEMBAR.SYS, …, RET.ABS R12 0x20
0xA00–0xA80  零填充
0xA80–0x1300  变体 B：CALL.ABS.NOINC 到共享例程的另一处理流程
```

- slot 20 的 `+0x18 = 0x880` 即返回桩入口；**patch site 选 0x880（首个 NOP）**；
- 注入组合（镜像 sm86 已审阅配方）：`[check_preempt_trap 40 条（去外层 BSSY/BSYNC、去 RET 尾）] + [被替换指令] + [跳回 0x890 的绝对 JMP]`；
- 分支代数验证：数组中"非抢占"分支（`@!P0 BRA .L_x_21`）在裁剪后精确落到追加块起点，语义与 sm86 官方数组一致。

### 4.4 实现落点（代码，未提交）

| 文件 | 变更 |
|---|---|
| `platforms/cuda/hal/src/arch/sm120.cpp` | `trap_inject_instrs`（40 条）+ `TarpHandlerSM120`：`GetTrapHandlerInfo`/`Instrument`（patch@0x880）/`SetJumpInstruction`（48 位绝对 JMP，sm86 同款编码）/`GetInjectSize` |
| `platforms/cuda/hal/include/xsched/cuda/hal/arch/sm120.h` | `TarpHandlerSM120` 声明 |
| `platforms/cuda/hal/include/xsched/cuda/hal/level3/trap.h` | 新增虚接口 `TarpHandler::GetTrapHandlerInfo`（默认走 cuxtra Pascal 路径） |
| `platforms/cuda/hal/src/level3/interrupt.cpp` | 走虚接口；关键节点 XINFO 诊断 |
| `platforms/cuda/hal/src/arch/arch.cpp` | `TarpHandler::Instance` 增加 case 120；Linux 下 sm120 → `CudaQueueLv3Trap`（Windows 保持 L2） |
| `platforms/cuda/hal/inject/inject_120.{asm,_cc.asm}` | 由 12.9.41 重新生成（审阅文件） |

> 说明：`sm120.cpp` 中保留了 **env 门控的实验钩子**（`XSCHED_SM120_TRAP_TEST_ADDR` / `XSCHED_SM120_TRAP_TEST_OFFSET`，用于"签名载荷"实验）。不设环境变量时行为与正式实现完全一致。

### 4.5 阶段一验证矩阵

| 验证项 | 方法 | 结果 |
|---|---|---|
| 指令数组可复现 | 与提交版逐位对比 | ✅ 一致 |
| handler 可获取 | DtoH 读回、首指令与反汇编核对 | ✅ |
| 补丁写入 | 补丁后 DtoH 读回比对 | ✅ 字节一致 |
| JMP 编码合法 | nvdisasm 解码 + 与真实 `CALL.ABS` 编码交叉验证 | ✅ |
| 触发可调用 | idle/busy 两阶段（带看门狗） | ✅ 瞬时返回，上下文健康 |
| **trap 执行注入代码** | 签名载荷 + `XSCHED_SM120_LV3` 全链路 | ⛔ 未执行（见第 5 章） |

---

## 5. Linux 阶段二：根因定位（多角度）

### 5.1 决定性实验：签名载荷

用 `env` 门控的最小载荷（`MOV R4/R5=观察地址；MOV R7=0x5AA5；ST.E [R4],R7`）替换正式注入数组，观察点分别用 managed 内存与 `__device__` 全局变量：

| 实验 | 场景 | 结果 |
|---|---|---|
| mini7 | RM 触发 + 64 个永久自旋 block；载荷打 **0x0 / 0x60 / 0x880** 三处 | 签名均未写入（`mini7_off_*.log`） |
| mini9 | **真实 trap**（内核内 `brkpt`）；同样三处载荷 | 内核报 `cudaErrorIllegalAddress(700)`，签名仍未写入 |
| cuda-gdb | 上述流程整体跑在真实 `cuda-gdb` 会话下 | 仍不执行 |

**结论：槽 20 的内存对象在 595 驱动上是非活动模板**——无论 RM 强制触发还是真实陷阱，执行流都不经过它（真实陷阱被驱动内部处理，直接以 700 报错回收内核）。

### 5.2 触发寄存器验证（排除常量问题）

- cuxtra（Linux 版）`cuXtraTriggerTrap` 写 `{reg=0x419e84, val=0x80000000}`；
- 在 `libcuda.so.595` 中定位到驱动内部同款助手（文件偏移 `0x487560` 附近）：机器码同样构造 `{0x419e84, 0x80000000}` 并批量写出；
- ⇒ 触发地址/位值在 sm120 + 595 上**并未变化**，问题不在触发本身。

### 5.3 真正的激活机制：CUDA 调试器 attach 栈（本轮测绘成果）

方法：自研 **LD_PRELOAD RM-ioctl 拦截器**（源码 `evidence/linux_2026-10-09/ioctl_log2.c`），对同一二进制分别做"普通运行"与"cuda-gdb 会话运行"，记录 `/dev/nvidia*` 上全部 ioctl 的请求号与载荷（含 RM_CONTROL 指向的参数缓冲区），做差分。

**调试器专属的 RM 方法族**（普通运行不存在）：

| RM 控制方法 | 载荷大小 | 语义（推断） |
|---|---|---|
| `0x83de0307` | 4B | 标志（值 1） |
| `0x83de030c` | **4824B** | 大型结构缓冲（模块/调试区相关） |
| `0x83de0315` / `0316` | 24B | `{新 RM 句柄, 区域尺寸, 宿主指针}`——模块调试区注册（11~22 次/会话） |
| `0x83de0317` | 8B | 0/1 标志 |
| `0x83de0318` | 0B | 无参命令 |
| `0x83de031f` / `032a` | 4B | 标志（1/0） |

**完整链路**：

```
cuda-gdb 会话
 ├─ 专用 RM client（独立于应用 client；实测每次会话 client 号不同）
 ├─ RM_ALLOC: class=0x83de 对象（父对象 = 设备 0x5c000002；对象句柄会话内分配）
 ├─ 上述 0x83de03xx 方法族（全部指向该调试对象）
 ├─ libcuda 侧钩子（libcuda 导出符号）：
 │     cudbgDebuggerCapabilities / cudbgDebuggerInitialized
 │     cudbgEnablePreemptionDebugging   ← 与抢占直接相关
 │     cudbgUseExternalDebugger / cudbgInitiateDebuggerAttachProcedureFd
 └─ libcudadebugger：protobuf agent 协议（"DebuggeeAttach"、"DebuggeeAttachFdAvailable" 等消息）
```

⇒ sm120 上 trap 的"武装/路由"不再是写一个寄存器位即可，而是深度依赖调试器 attach 会话（RM 对象 + 方法族 + agent 握手）。

### 5.4 TSG 路径评估

| 验证项 | 结果 |
|---|---|
| `cuXtraGetTimeslice` | ✅ 2048 µs |
| `cuXtraSetTimeslice(ctx, 0)` | ✅ 写入真实有效（立即读回 0） |
| 语义限制 | TSG 只"暂停/换出"，**不杀死**内核 ⇒ 对永久自旋内核做 `kQueueSuspendFlagSyncHwQueue` 的 suspend 必然死锁（`mini5`/`mini12`）；TSG 场景应使用不带同步标志的 suspend |
| 抢占收益测量 | `mini13`（双上下文/双 TSG，2048×256 饱和受害负载）：竞争核仍全速运行，`ts` 调整无可测差异——本机硬件/驱动的**默认跨上下文调度已高度均衡**；TSG 的价值需多进程/多设备场景方可评估 |

---

## 6. 下一步路线（复现 attach 子集）

按优先级排列，全部所需原始报文已在本仓库证据目录：

1. **复刻 attach 最小子集**：在本进程内创建专用 RM client（`/dev/nvidiactl` 的 `NV_ESC_RM_ALLOC`，或经 cuxtra 的 `cuXtraGetRmControlFd()` 复用通道）→ 分配 class 0x83de 对象（父=设备）→ 按捕获的载荷模板调用 `0x83de03xx` 序列；
2. **对照实验**：逐一启用/停用每个方法，配合 mini7 签名载荷，找出"最小可 trap 子集"；
3. **验证信号**：mini9（brkpt）从"700 报错"变为"执行到我们的注入代码"即成功；
4. **产品化**：成功后把该序列封装为 xsched hal 的 sm120 初始化步骤（L3 enable），再接回现有 `TarpHandlerSM120` 流程；
5. **TSG 线**：在双进程/多设备场景复测 TSG 抢占收益，明确其适用边界。

---

## 7. 复现指南（关键命令）

```bash
X=/path/to/xsched
# 构建（含测试）
export PATH=/home/wy/cuda-12.9/bin:$PATH NVCCFLAGS="-arch=sm_120"
cmake -B$X/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$X/output \
      -DSHIM_SOFTLINK=ON -DBUILD_TEST=ON -DPLATFORM_CUDA=ON \
      -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
      -DCMAKE_CUDA_HOST_COMPILER=g++-13 -DCMAKE_CUDA_COMPILER=/home/wy/cuda-12.9/bin/nvcc
cmake --build $X/build --target install -j$(nproc)

# 运行期环境（shim 注入 + 真实驱动指引）
mkdir -p /tmp/xsched_shim && cp $X/output/lib/libshimcuda.so /tmp/xsched_shim/libcuda.so.1
export LD_LIBRARY_PATH=/tmp/xsched_shim
export XSCHED_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1
export CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1

# 指令生成/回归
cd $X/platforms/cuda/hal/inject && make ARCH=120 NVCC="nvcc -ccbin g++-13" bin dump cc
python3 $X/tools/instrument/extract_sass.py generate --cubin inject_120.cubin \
        --source inject.cu --offset-map sm120 --out-cpp <preview>.cpp --report <report>.json

# 签名载荷实验（证据目录内有测试源码 mini7/mini9）
XSCHED_SM120_TRAP_TEST_ADDR=0x<观察地址> XSCHED_SM120_TRAP_TEST_OFFSET=0x880 ./mini7 64
# cuda-gdb 会话 + 预加载拦截器（捕获 RM ioctl）
cuda-gdb -batch -ex "set exec-wrapper env LD_PRELOAD=.../ioctl_log2.so IOCTL_LOG=.../out.log" -ex run ./app
```

---

## 8. 证据与文档索引（`test/sm120_l3_probe/evidence/linux_2026-10-09/`）

| 文件 | 内容 |
|---|---|
| `handler_sm120.bin` | 4864B trap handler 原始镜像 |
| `handler_880_disasm.txt` | 变体 A [0,0x880) 全量反汇编 |
| `review_sm120.md` / `gen_sm120_report.json` / `preview_sm120.cpp` | guardian/resume 生成与 census（复现性验证） |
| `gen_sm120_trap_report.json` / `preview_sm120_trap.cpp` | trap 路径数组生成报告（含每次替换的编码） |
| `final_writeback.log` | 补丁写入/读回字节级验证记录 |
| `final_trigger_probe.log` | RM 触发 idle/busy 实测记录 |
| `mini5_trap_run4.log`、`mini7_off_{0x0,0x60,0x880}.log` | 注入未执行的逐 offset 证据 |
| `mini9_run1.log`、`mini9.cu` | 真实 brkpt trap 判别实验 |
| `ioctl_normal.log` / `ioctl_cudbg.log` / `ioctl2_cudbg.log` | 普通 vs cuda-gdb 的 RM ioctl 全量差分（含 0x83de 方法载荷） |
| `ioctl_log2.c` | RM ioctl 拦截器源码（可复现捕获） |
| `mini12_*.log`、`mini13_run6.log`、`mini13.cu` | TSG 写入验证与双上下文竞争实验 |

对应英文原始记录见 `test/sm120_l3_probe/README.md` 的 "Linux follow-up session" 与 "Round 2" 两节。

---

## 9. 遗留问题

1. `platforms/cuda/test/main/level.cu` 在本 Linux 环境 level 1 即挂起（LoopRunner 的 64 发射+同步风暴经 shim 触发；最小复现均通过）——与 L3 无关，未解决；
2. Linux 上 `ObjGetPc` 的 `pc` 字段返回 0（Windows 13030 返回 0x880），语义差异待查（不影响当前工作路径）；
3. `0x83de030c` 的 4824 字节结构尚未逐字段解析；
4. TSG 的"按上下文 vs 按通道"时间片粒度需在双进程场景确认。
