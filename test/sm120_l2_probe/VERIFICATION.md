# T1/T2 验证方法论与结果

> **文档定位**：记录 XSched sm120 Level-2 支持第一阶段（T1：cuxtra 机制
> 探针）与第二阶段（T2：inject 工具链现代化）的**验证方法论、执行过程、
> 结果与结论**。假设清单见 `ASSUMPTIONS.md`；证据位置见
> `evidence/MANIFEST.md`；哈希封存见 `evidence/SHA256SUMS.txt`。
>
> 日期：2026-10-01 · 状态：**T1/T2 完成**（核心结论全部硬件实证）

---

## 1. 目标与范围

| 任务 | 目标 | 完成标准 |
|---|---|---|
| **T1** | 确证 sm120 上 Level-2 抢占所依赖的 cuxtra 机制在**标准驱动**下可用，并定位 debugger-parameters 窗口的真实常量 bank 偏移 | ① 探针全绿；② 窗口位置定址有不可反驳的物理证据；③ 证据封存 |
| **T2** | 将 inject 工具链现代化到 CUDA 12.9 / sm_120，产出可对照的基线产物 | ① Makefile 参数化（ARCH）；② inject_120（+sm86 对照）全产物；③ 产物形态与 sm86 的差异可归因 |

**范围外**（承接后续任务）：sm120 驱动 trap handler 的注入点定位
（T3）、`sm120.cpp` HAL 实现（T3/T4+）、真实抢占端到端测试。

## 2. 验证环境

- GPU：NVIDIA GeForce RTX 5060，sm_120（CC 12.0）
- 驱动：610.88（`cuDriverGetVersion = 13030`，CUDA 13.3 级，官方签名）
- 工具链：nvcc 12.9.41 / cuobjdump 12.9.26 / nvdisasm 12.9.19 /
  g++ 14.2.0 (MinGW) / Python 3.11.9（`evidence/toolchain_versions.txt`）
- 被测对象唯一性：探针 `LoadLibraryA` 直接加载
  `C:\Windows\System32\nvcuda.dll`（真驱动），cuxtra 经
  `CUXTRA_CUDA_LIB` 环境变量绑定同一驱动；与仓库 shim 无耦合。

## 3. 方法论设计（六项原则）

1. **硬件在环、语义级验证**：每一编码/假设都要求在真实 GPU 上产生
   可观测的语义后果（读到正确值、写到正确位置），而非仅检查编码形状。
2. **静态 roundtrip 自校验**：补丁器对每条生成指令做**解码回验**
   （编码→解码→比对请求操作数），占位符数量/位置/数量不符即中止且
   不写出文件——保证"写进 cubin 的每一位都是意图中的那一位"。
3. **哨兵法（Sentinel）**：`SetDebuggerParams` 写入四个 64/32 位魔数
   （0x1111…/0x5555…/0x9999…/0xDDDD…），内核全 bank 扫描后按**值**
   而非"非零"来识别窗口落点，杜绝把 driver 元数据误判为窗口。
4. **变值复核 + 负对照**：换一组全新哨兵（0xA1A2…/0xB1B2…/0xC1C2…/
   0xE1E2…）复跑命中同址（证明内容可控跟随）；同时检查窗口前一字
   （0x168）不含哨兵（证明无外溢）。
5. **Gapless 全量覆盖**：每批 62 点 × 8 字节 = 0x1F0 与批基址步进相等
   （无缝隙无重叠）；0x0–0x43D0 全域 35 批穷举，杜绝"窗口恰好落在采样
   间隙"的漏检。采样初值 0xCC… 哨兵区分"读到 0"与"未写入"。
6. **双次复现**：决定性实验（标准序列）完整重跑两次，输出逐字节比对
   （SHA256 相同即物理级复现）。

## 4. T1 执行过程与结果

### 4.1 探针架构（最终形态）

- 设备端 `probe_kernels.cu`：`kernel_read_params` 内嵌 **127 个 `brkpt`
  占位槽**（宏展开 `PB64 PB32 PB16 PB8 PB4 PB2` + 1），供补丁器 16B→16B
  替换。
- 补丁器 `patch_cubin.py`：两种模式——
  - **标准协议**：2 参数槽 + 5 采样点（0x170/0x178/0x180/0x188/0x168）
    + 115×NOP；
  - **扫描协议**（`--scan BASE STEP COUNT`）：62 采样点 × (LDC.64+ST.E.64)
    即 496 字节 gapless 采样。
- 主机端 `probe_main.cpp`：P0–P5 全套检查 + `--scan` 模式 + `--p6`
  选择性启用；退出码 = 失败检查数。

### 4.2 标准序列结果（决定性证据）

`evidence/main_std_window0x170.log`（`_run2` 为复现）：

- **23 项运行时检查全部 PASS，`RESULT: 0 failed check(s)`，exit=0**
  （P0×4、P1×6、P2×3、P3×4、P4×5、P5×1）
- 两次运行日志 SHA256 完全一致
  （`79241c7b…`）——结果可复现。
- P1 明细：全新哨兵组在 0x170/0x178/0x180/0x188 **全部命中**；
  0x168 负对照无哨兵；Set→Get 回读一致。
- P2/P3/P4/P5：entry point 重定向（0xA…/0xB… 双向）、指令内存上传
  与 EXIT 桩执行、GetBinary/LocalRegs/BarrierCnt/ParamCount/ParamInfo
  全部读回一致——**cuxtra 工具接口族在标准驱动上全部物理可用**。

### 4.3 窗口定位实验（全 bank 穷举）

- 先验：sm86 窗口 0x1880（来自 `sm86.cpp` 官方数组）→ 在 sm120 上
  专批扫描 0x1840–0x19C0（v1 协议）与探针 P1 → **全 0（假设被否定）**。
- 主力实验：**35 批 × 62 点 gapless 穷举 0x0–0x43D0**（v2 协议，
  LDC.64 8 字节/点）。每批 patch 日志、运行日志、补丁后 cubin 三件套
  入档（`evidence/scan_v2/`）。
- 结果分布（全部批次汇总）：
  - `matches=4` **仅出现在第 0 批**：0x170/0x178/0x180/0x188
    （哨兵值精确命中）；
  - 其余非零点集中在 0x0 批（driver 元数据，17 个）与 0x1F0 批
    （kernel 元数据区，29 个）；0x5D0 之后**全零**；
  - **全部 36 批 `unwritten=0`**（协议无死角）；
  - 2170+62 个采样点中哨兵零误报。

### 4.4 关键发现（T1 结论）

> **sm120 的 debugger-parameters 窗口 = `c[0x0][0x170 .. 0x18C]`（28
> 字节）**，布局与 sm86 的 `0x1880..0x189C` 逐字段对应：
> `0x170` preempt_buf(8B) / `0x178` guardian(8B) / `0x180` kernel_idx(8B)
> / `0x188` killable(4B)。

证据链（相互独立、逐级加强）：
1. 全 bank 穷举：哨兵唯一定位于 0x170–0x188（`run_full_0x0.log`）；
2. 变值复核：P1 新哨兵组命中同址（`main_std_window0x170.log`）；
3. 负对照：0x168 无哨兵（同上）；
4. 复现：两次标准序列逐字节一致（SHA256 同值）；
5. 机制自检：0x380 == out 指针（LDC.64 语义在真实参数上闭环,
   `run_selfcheck_0x300.log`）。

### 4.5 P6 路线（trap handler dump）记录【受阻】

`cuXtraGetTrapHandlerInfo` 在 cuxtra `trap.cpp:20` 以
`cuda error 101: invalid device ordinal` 中止进程
（`evidence/p6_run1.log`；调用链反汇编见
`evidence/recon/cuxtra_lib_full.dis.asm`）。因此 P6 默认禁用、保留
`--p6` 入口与失败证据；T1 目标不依赖该路径（已由全扫路线达成）。

### 4.6 副产物（对 T3 有用）

- **kernel 元数据区画像**：0x340–0x390 出现稳定的指针/维度常量
  （0x348/0x350 双指针 0x…0380/0x…0388、0x360–0x378 常量域），
  0x340 前含 0x0C0 随机种子等——为 T3 识别注入环境提供参照。
- **cbank 有效域边界**：0x5D0 之后全零 → sm120 该 kernel 的
  `c[0x0]` 实际使用范围 ≈ 0x0–0x5D0。
- **LDC.64/ST.E.64 的可复用编码器**（已附 roundtrip）。

## 5. T2 执行过程与结果

- **Makefile 现代化**（`platforms/cuda/hal/inject/Makefile`）：
  ARCH 参数化（`make ARCH=120 bin dump cc`；缺省 nvidia-smi 自动检测）、
  三目标（bin=cubin / dump=cuobjdump / cc=nvdisasm -c）、
  Windows 包装器 `make_msvc.bat`（MSVC 宿主环境）；
  nvdisasm sm_120 注意事项写入注释（`-b SM120` 误解析，用自动检测/
  `SM120A`）。
- **产物**：`inject_120.{cubin,asm,_cc.asm}`（26×BPT.TRAP）与
  `inject_86.{cubin,asm,_cc.asm}` 同工具链生成——sm86 作为对照基线。
- **产物同构核验**：inject_120 的 BPT.TRAP 双字
  （0x000000040000795c / 0x000fea0000300000）与探针占位逐字节一致；
  与 inject_86 亦一致 → 占位机制跨架构稳定，sm120 的差异可归因架构。

## 6. 结论与后续任务输入

**T1/T2 结论**：sm120 Level-2 的关键机制底座已硬件实证可用——
cuxtra 工具链（entry point 重定向、指令内存、资源读写）在标准 Windows
驱动上全部工作；debugger 窗口定址 **0x170**（不再沿用 0x1880）；
inject 产线与占位替换机制跨架构同构。

**对 T3+ 的直接输入**：
1. `sm120.cpp` 的 guardian/resume 机器码：LDC 偏移从 0x1880 系列改为
   0x170 系列（寄存器与逻辑骨架可沿用 sm86）；
2. trap handler 注入：P6 路线受阻，需替代手段（如从 inject_120
   产物提取指令模板 + 运行时探测），且 **A16 假设（trap 视野同窗
   0x170）必须在 T3 实证**；
3. `ParamInfo(0) offset=0x0 size=8`、`LocalRegs 16`、`BarrierCnt 0`
   等实测值可作 T3 基准。

**未决问题**：
- trap handler 上下文窗口偏移（A16，T3）；
- P6 受阻的环境边界（Windows/标准驱动；Linux/开发驱动的适用性未知）；
- 0x340–0x390 元数据区各字段的精确语义（不阻塞 T3）。

## 7. 证据索引与封存

| 内容 | 位置 | 哈希 |
|---|---|---|
| 封存清单 | `evidence/MANIFEST.md` | 见 `SHA256SUMS.txt` |
| 全量哈希（163 条目） | `evidence/SHA256SUMS.txt` | — |
| T1 决定性运行 ×2 | `evidence/main_std_window0x170{,_run2}.log` | `79241c7b…`（两次相同） |
| 窗口定址本体 | `evidence/scan_v2/run_full_0x0.log` | 见清单 |
| LDC.64 语义自检 | `evidence/scan_v2/run_selfcheck_0x300.log` | 见清单 |
| 全扫 35 批 | `evidence/scan_v2/`（108 文件） | 见清单 |
| v1 历史扫描 | `evidence/scan/`（27 文件） | 见清单 |
| P6 受阻 | `evidence/p6_run1.log` | 见清单 |
| 侦察材料 | `evidence/recon/`（5 文件） | 见清单 |
| T2 产物 | `platforms/cuda/hal/inject/`（收录于清单第六节） | 见清单 |

封存方式：`seal_evidence.ps1` 对证据目录全部文件 + 探针源码 + T2 产物
生成 SHA256 清单；任何字节级改动都会破坏哈希一致性。

## 8. 复现指南

```powershell
cd test\sm120_l2_probe
powershell -ExecutionPolicy Bypass -File build.ps1 -Stage all   # 设备+宿主构建
$env:CUXTRA_CUDA_LIB = 'C:\Windows\System32\nvcuda.dll'
python patch_cubin.py probe_kernels.cubin probe_kernels_patched.cubin
.\probe_main.exe probe_kernels_patched.cubin        # 预期: 23 PASS / RESULT 0
powershell -ExecutionPolicy Bypass -File run_full_scan.ps1      # 35 批全扫
powershell -ExecutionPolicy Bypass -File seal_evidence.ps1      # 重生成哈希
```

预期：标准序列 exit=0；全扫在 0x0 批出现 4 条 `MATCH` 行与
`nonzero=17`，其余批次 `nonzero=0`；P1 使用任意哨兵组均命中
0x170/0x178/0x180/0x188。
