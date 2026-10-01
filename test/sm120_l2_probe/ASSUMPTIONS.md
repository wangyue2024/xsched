# T1/T2 假设表与验证状态

> **文档定位**：本文档完整、准确、严谨地列出 XSched sm120 Level-2 支持
> 第一阶段（T1：cuxtra 机制探针）与第二阶段（T2：inject 工具链现代化）
> 实施与验证所依赖的**全部假设**，逐条给出依据、验证方法、结论与证据索引。
>
> - 配套文档：`VERIFICATION.md`（方法论与结果）、
>   `evidence/MANIFEST.md`（证据清单）、`evidence/SHA256SUMS.txt`（哈希封存）
> - 验证环境：RTX 5060 (sm_120) / Windows / driver 610.88
>   (`cuDriverGetVersion=13030`, CUDA 13.3 级) / nvcc 12.9.41 /
>   g++ 14.2.0 / Python 3.11.9（详见 `evidence/toolchain_versions.txt`）
> - 日期：2026-10-01
>
> **状态图例**：`【已验证】` 有硬件实证；`【已否定】` 被实证推翻
> （保留记录，防止复用）；`【待验证】` 超出本阶段范围，标注承接任务。

---

## 第一部分 T1：cuxtra 机制探针

### A1 · 占位符同构性（探针 ↔ inject 工具链）【已验证】

- **假设**：nvcc 12.9 对 sm_120 **常规编译**产生的 `brkpt;`
  占位符（BPT.TRAP），与 inject 工具链
  （`--keep-device-functions -Xptxas -astoolspatch`）产出的
  BPT.TRAP **逐字节相同**（16 字节：
  `0x000000040000795c / 0x000fea0000300000`），因此允许用常规编译的
  探针研究注占位符替换机制，并将结论外推到 inject 生产管线。
- **依据**：两者共用同一 ptxas 对 `brkpt` 的固定降级编码。
- **验证**：① 探针 cubin 中 127 个 BPT.TRAP 被补丁器完整定位并替换
  （脚本内置数量/位置断言，不匹配即中止不写出）；②
  `inject_120.asm`（astoolspatch 产物）中 26 个 BPT.TRAP 的双字与探针
  模式逐字节比对一致。
- **结果**：成立。两条管线的占位符完全同构。
- **证据**：`evidence/scan_v2/patch_*.log`（"127 BPT.TRAP placeholder(s)
  found (expect 127)"）；`platforms/cuda/hal/inject/inject_120.asm`。

### A2 · 16 字节等长替换不破坏 cubin 结构【已验证】

- **假设**：BPT.TRAP 为 16 字节定长槽位，等长替换为任意合法 SASS
  （LDC / LDC.64 / ST.E.64 / NOP）不破坏 ELF 结构、指令对齐与段布局，
  驱动（`cuModuleLoad`）可直接加载修改后的 cubin。
- **验证**：127 槽全部替换后的 cubin 被驱动加载成功、在 GPU 上执行，
  全部 35 批 × 62 采样点均按预期写入结果（无非法指令/加载错误）。
- **结果**：成立。
- **证据**：`evidence/main_std_window0x170.log`（P0 加载 + P1 执行）、
  `evidence/scan_v2/`（全部 35 批 `run_*.log` 无 CUDA 错误）。

### A3 · LDC.32 编码公式【已验证】

- **假设**：`LDC Rx, c[0x0][off]` 的编码为
  `word0 = 0xff007b82 | (reg<<16) | ((off>>2)<<40)`、
  控制字 `word1 = 0x000fc00000000800`（调度位取保守模板值），
  在 sm120 硬件上语义正确。
- **依据**：sm86.cpp 官方运行时数组的逐位结构
  （0x00062000ff047b82 ↔ LDC R4, c[0x0][0x1880]）+ sm120 ptxas 样本
  （`probe_kernels.sass.txt`）交叉验证。
- **验证（语义级，非仅形状）**：用该编码读 `c[0x0][0x380]`
  （kernel 参数槽），读回值**等于** `cuMemAlloc` 返回的 `out` 指针
  `0x0000000b05c00000`。
- **结果**：成立。
- **证据**：`evidence/scan_v2/run_selfcheck_0x300.log`。

### A4 · LDC.64 编码（word1 bit9）【已验证】

- **假设**：`LDC.64 Rx, c[0x0][off]` 的 word0 与 LDC.32 完全同构；
  唯一语义差异位在 **word1 的 bit 9**（置 1 标记 64 位形式），
  模板控制字取 `0x000fc00000000a00`；寄存器必须为偶数
  （Rx:Rx+1 成对读取）。
- **依据**：ptxas 对照样本 `LDC R1,c[0x0][0x37c]`（w1=0x000fe20000000800）
  vs `LDC.64 R2,c[0x0][0x380]`（w1=0x000e220000000a00，bit9=1）；
  `trap_handler_sm86.asm` 的 `LDC.64 R6,c[0x0][0x1870]`
  （w1=0x000ea40000000a00）交叉印证。
- **验证**：全扫 2170 个采样点（35 批×62）全部由 LDC.64 完成读取，
  其中 0x380 读回 out 指针、0x170–0x188 读回哨兵——8 字节语义正确；
  `unwritten=0` 证明存储配对正确。
- **结果**：成立。
- **证据**：同 A3；`probe_kernels.sass.txt`；
  `platforms/cuda/hal/inject/trap_handler_sm86.asm`。

### A5 · ST.E.64 编码【已验证】

- **假设**：
  `ST.E.64 [Rb+imm], Rp` 编码为
  `word0 = 0x7385 | (base<<24) | (imm<<32)`、
  `word1 = 0x000fe20000100b00 | pair_reg`，`imm` 为字节偏移。
- **验证**：全部采样点依序准确落入 `out[i]` 的各 8 字节槽
  （相邻槽零串扰；哨兵/指针值逐点吻合）。
- **结果**：成立。
- **证据**：`evidence/scan_v2/run_*.log` 全部。

### A6 · sm120 kernel 参数区偏移 0x380【已验证】

- **假设**：sm120 ABI 下，kernel 第一个参数（此处为 u64 指针
  `out`）位于 `c[0x0][0x380]`（sm86 ABI 为 0x210）。
- **依据**：`ref_use_param` 的自然编译 SASS
  （`LDC.64 R2, c[0x0][0x380]`）。
- **验证**：读 0x380 得到的 8 字节 == `cuMemAlloc` 的
  `out` 指针值（运行时自检）。
- **结果**：成立。
- **证据**：`test/sm120_l2_probe/probe_kernels.sass.txt`；
  `evidence/scan_v2/run_selfcheck_0x300.log`。

### A7 · cuxtra 静态库在 Windows + 标准驱动上可用【已验证】

- **假设**：`libcuxtra_windows_amd64.a` 封装的驱动内置 CUtools 工具
  接口族（经 `cuGetExportTable`/ETID 访问）在 **未打补丁的标准
  Windows 驱动**（610.88 / CUDA 13.3 级）上可用，无需开发驱动。
- **验证**：运行时逐 API 硬件实证（全部 PASS）：
  - `cuXtraGet/SetDebuggerParams`（P1，见 A11）
  - `cuXtraGet/SetEntryPoint`（P2 换体重定向：`kernel_marker_b` 经
    `kernel_marker_a` 入口执行出 0xA…，还原后出 0xB…）
  - `cuXtraInstrMemBlockAlloc / InstrMemcpyHtoD / InvalInstrCache`
    （P3：上传 16 字节 EXIT 桩并重定向执行，`out` 未被触碰）
  - `cuXtraGetBinary / LocalRegs(get/set) / BarrierCnt(get/set) /
    ParamCount / ParamInfo`（P4 全部读回一致）
- **结果**：成立。
- **证据**：`evidence/main_std_window0x170.log`（P2/P3/P4 逐行 PASS）。

### A8 · （旧假设）sm120 窗口沿用 sm86 的 0x1880【已否定】

- **假设**：sm120 的 debugger-parameters 窗口与 sm86 相同，位于
  `c[0x0][0x1880..0x18A0]`。
- **验证**：① 探针 P1 读取 0x1880/0x1888/0x1890/0x1898 → 全 0；
  ② v1 协议对 0x1840–0x19C0 区域专批扫描 → 全 0。
- **结果**：**被否定**。窗口不在 sm86 位置（sm120 ABI 漂移）。
- **证据**：`evidence/main_std_v2.log`（P1 4 项 FAIL）；
  `evidence/scan/`（winA_* 批次全零）。

### A9 · （修正假设）sm120 窗口位于 c[0x0][0x170..0x18C]【已验证】

- **假设**：sm120 驱动的 debugger-parameters 窗口基址为
  `c[0x0][0x170]`，28 字节。
- **验证（三重独立证据）**：
  1. **全 bank 穷举扫描**：35 批 gapless 覆盖 0x0–0x43D0，4 个哨兵
     （0x1111…/0x5555…/0x9999…/0xDDDD…）恰好出现在
     0x170/0x178/0x180/0x188，全 bank 其余位置无哨兵；
  2. **变值敏感性**：P1 换用全新哨兵组（0xA1A2…/0xB1B2…/0xC1C2…/
     0xE1E2…）再次全命中同址——窗口内容**跟随
     `SetDebuggerParams` 可控变化**；
  3. **负对照**：窗口前一个 8 字节（0x168）不含任何哨兵。
- **结果**：成立。**T1 核心结论。**
- **证据**：`evidence/scan_v2/run_full_0x0.log`；
  `evidence/main_std_window0x170.log` 与 `_run2.log`（两次运行哈希逐
  字节一致，可复现）。

### A10 · 窗口内部布局与 sm86 逐字段对应【已验证】

- **假设**：28 字节窗口布局为
  `+0x00` preempt_buf(8B)、`+0x08` guardian entry(8B)、
  `+0x10` kernel_idx(8B)、`+0x18` killable(4B)，与
  `instrument.cpp` 的 `args_buf[28]` 及 sm86 窗口语义一一对应。
- **验证**：四个哨兵按上述类型宽度逐字段落位（含 killable 仅低
  4 字节为 0x00000000DDDDEEEE）。
- **结果**：成立。
- **证据**：同 A9。

### A11 · SetDebuggerParams 物理生效（非 API 层缓存）【已验证】

- **假设**：`cuXtraSetDebuggerParams` 写入的数据真实落入 kernel 可见
  的常量 bank（debugger 窗口），且 `GetDebuggerParams` 可原样回读。
- **验证**：① Set→Get 回读逐字节一致；② 哨兵值在 kernel 内通过
  LDC.64 物理读出（见 A9 证据 1/2）。
- **结果**：成立。
- **证据**：`evidence/main_std_window0x170.log`
  （"SetDebuggerParams -> GetDebuggerParams read-back identical"）。

### A12 · （P6 路线）GetTrapHandlerInfo 可在 sm120 上 dump trap handler【已否定】

- **假设**：`cuXtraGetTrapHandlerInfo` 可在本环境取到驱动 trap
  handler 地址并 dump 其机器码，用于定位窗口/注入点。
- **验证**：调用在 cuxtra 内部 `trap.cpp:20` 以
  `cuda error 101: invalid device ordinal` 中止（XASSERT 终止进程）。
- **结果**：**被否定**（Windows + 标准驱动 610.88 环境）。P6 改为
  `--p6` 选择性启用，保留记录；不排除其它 OS/驱动组合可用。
- **证据**：`evidence/p6_run1.log`；
  `evidence/recon/cuxtra_lib_full.dis.asm`（调用链反汇编）。

### A13 · 扫描覆盖完备性（Gapless）【已验证】

- **假设**：每批 62 点 × 8 字节 = 496 字节（0x1F0）连续覆盖，批基址
  步进同为 0x1F0，则 35 批无缝隙、无重叠地覆盖 cbank 0x0–0x43D0；
  任何 ≥8 字节的窗口都不会落入采样间隙。
- **验证**：批基址序列 0x0、0x1F0、0x3E0、…、0x41E0 严格等差；
  全部批 `unwritten=0`（每个采样槽都被硬件写入，无协议死角）。
- **结果**：成立。
- **证据**：`evidence/scan_v2/patch_full_*.log`（span 行）；
  全部 `run_full_*.log`。

### A14 · 哨兵无歧义性【已验证】

- **假设**：四个哨兵魔数不会与 cbank 自然数据混淆（不会误报）。
- **验证**：全 bank 2170 个采样点中，哨兵仅出现于 0x170–0x188；
  其余非零点（driver 元数据）无一与哨兵撞值；`matches` 总计数恰为 4。
- **结果**：成立。
- **证据**：`evidence/scan_v2/`（`RESULT-SCAN` 汇总行：
  `matches=4` 仅在 0x0 批）。

### A15 · 采样写回可判别性（0xCC 哨兵法）【已验证】

- **假设**：kernel 哪怕读到 cbank 的 0 值也会如实写入；通过预填
  0xCC… 哨兵可区分"真实读到 0"与"协议未写入"。
- **验证**：全部 36 批 `unwritten=0`（无任何槽保留 0xCC），即每个
  采样点都确实执行了 LDC.64+ST.E.64。
- **结果**：成立（扫描协议可信度的关键支撑）。
- **证据**：全部 `run_full_*.log` / `run_selfcheck_0x300.log` 的
  `RESULT-SCAN` 行。

### A16 · trap handler 上下文可见同一窗口【待验证 · T3】

- **假设（待定）**：sm120 驱动 trap handler 的常量视野中，窗口同样
  位于 0x170（sm86 上 guardian 与 trap_inject 使用同一窗口地址，
  推测 sm120 亦然）。
- **现状**：本阶段**无法实证**（P6 路线受阻、无 sm120 trap handler
  可加载/触发途径）。已确证的是 **kernel 上下文**窗口 = 0x170。
- **风险**：若两者不同，T3 的 trap 注入代码需区分两套偏移。
- **承接**：T3（trap handler 注入实验）。

### A17 · guardian/resume 注入代码使用 0x170 窗口可工作【待验证 · T3/T4】

- **假设**：将 sm86.cpp 的 guardian/resume 机器码数组的 LDC 偏移
  从 0x1880 系列改为 0x170 系列（相同寄存器与逻辑），在 sm120 上
  即可实现 Level-2 的抢占/恢复路径。
- **现状**：窗口位置与所有基础机制（A2–A7）已验证；真正 Sm120 HAL
  实现属于 T3+ 工作，尚未构建。
- **承接**：T3 起（`platforms/cuda/hal/src/arch/sm120.cpp` 设计）。

---

## 第二部分 T2：inject 工具链现代化

### B1 · 工具链三件套在 CUDA 12.9 / sm_120 上可用【已验证】

- **假设**：`nvcc -cubin --keep-device-functions -Xptxas -astoolspatch`
  在 CUDA 12.9 对 sm_120 可用，且占位符（BPT.TRAP）保持在被注入的
  device function 中不被优化。
- **验证**：`inject_120.cubin` 生成成功；`inject_120.asm` 中 26 个
  BPT.TRAP 完整保留；`inject_120_cc.asm`（nvdisasm 视角）无错误。
- **结果**：成立。
- **证据**：`platforms/cuda/hal/inject/inject_120.*`（哈希已封存）。

### B2 · `-astoolspatch` 与 entry function 互斥【已验证】

- **假设**：`-astoolspatch` 模式下 ptxas 拒绝 entry function
  （"Entry function is not allowed"），故探针改用常规编译、inject 管线
  使用 device function（`--keep-device-functions`）。
- **验证**：探针以常规编译成功产出可用占位内核（127×BPT.TRAP），
  inject 管线以 device function 成功产出 26×BPT.TRAP——两条路线按
  各自约束成立。
- **结果**：成立。
- **证据**：`test/sm120_l2_probe/build.ps1`（注释记录约束）；
  `inject_120.asm`。

### B3 · BPT.TRAP 编码跨架构稳定【已验证】

- **假设**：sm120 的 BPT.TRAP 占位编码与 sm86 相同
  （`0x000000040000795c / 0x000fea0000300000`），inject 机制的上层
  逻辑（定位、替换）可复用。
- **验证**：三方比对一致：`inject_120.asm` ↔ `inject_86.asm` ↔
  探针 cubin（见 A1）。
- **结果**：成立。

### B4 · nvdisasm 对 sm_120 的正确调用方式【已验证】

- **假设**：CUDA 12.9 的 nvdisasm 对 sm_120 cubin 需
  `-b SM120A` 或自动检测；用 `-b SM120` 会误解析（本次记录为 1871 个
  伪 "Unrecognized operation for functional unit 'uC'"）。
- **验证**：以自动检测路径生成的 `inject_120_cc.asm` 干净无错误。
- **结果**：成立（注意事项已写入 Makefile 注释）。
- **证据**：`platforms/cuda/hal/inject/Makefile` 注释；
  `inject_120_cc.asm`。

### B5 · Makefile 可在 Windows/MSVC 环境驱动【已验证】

- **假设**：现代化 Makefile 可经 `make_msvc.bat`（激活 MSVC 宿主环境）
  在 Windows 上完成 bin/dump/cc 全流程；ARCH 缺省自动检测
  （nvidia-smi）。
- **验证**：`inject_120.*`、`inject_86.*` 全部产物按此路径生成成功。
- **结果**：成立。
- **证据**：`platforms/cuda/hal/inject/Makefile`、`make_msvc.bat`。

### B6 · 同工具链 sm86 对照基线【已验证】

- **假设**：用同一 CUDA 12.9 工具链重编 sm86（inject_86.*）作为
  对照，可将"sm120 产物形态变化"归因于架构而非工具链版本漂移。
- **验证**：inject_86 全套产物已生成（与 inject_120 同日同工具链）。
- **结果**：成立（对照基线已就位）。
- **证据**：`inject_86.{cubin,asm,_cc.asm}`。

### B7 · T2 产物作为 T3 输入的完备性【待验证 · T3】

- **假设**：`inject_120.cubin/asm` 提供的 sm120 指令池（BPT 占位 +
  自然码 + 可对照的指令编码）足以支撑 T3 构建 sm120 的注入代码。
- **现状**：占位符与基础编码（LDC/LDC.64/ST.E.64/BPT/EXIT/NOP/JMP
  部分）已备；**trap handler 注入所需的 sm120 具体偏移与 JMP 注入点
  尚缺**（依赖 P6 受阻后转向的替代手段，见 A12/A16）。
- **承接**：T3。

---

## 第三部分 背景事实（非假设，已按前述会话验证并引用）

- **C1** 系统 `C:\Windows\System32\nvcuda.dll` 为官方签名驱动
  （610.88，CUDA 13.3 级；`cuDriverGetVersion=13030`）。
- **C2** 仓库 `output\bin\nvcuda.dll`（31 MB、未签名）为 XSched CUDA
  shim 的本地构建产物（`add_shim_lib(cuda ... "nvcuda")`），导出全套
  CUDA API + `cuXtra*`（18 个）+ `cuGetExportTable`；
  `tests_local/run_tests.ps1` 将其拷贝到测试目录以"同目录优先加载"
  方式拦截真驱动调用。
- **C3** 本阶段探针**直接加载 System32 真驱动**（`LoadLibraryA` 定址
  + `CUXTRA_CUDA_LIB` 环境变量管线），与 shim 无耦合，保证测量对象
  唯一。

---

## 附：假设-证据-结论对照速查

| ID | 假设摘要 | 状态 | 关键证据 |
|---|---|---|---|
| A1 | 探针↔inject 占位符同构 | 已验证 | patch 日志 / inject_120.asm |
| A2 | 16B 等长替换保结构 | 已验证 | P0 加载 + 全扫执行 |
| A3 | LDC.32 公式 | 已验证 | selfcheck 0x380=out 指针 |
| A4 | LDC.64=word0 同构+bit9 | 已验证 | 全扫 2170 点、unwritten=0 |
| A5 | ST.E.64 编码 | 已验证 | 全扫槽位落位正确 |
| A6 | 参数区 0x380 | 已验证 | SASS + selfcheck |
| A7 | cuxtra 标准驱动可用 | 已验证 | P2/P3/P4/P5 全 PASS |
| A8 | 窗口=0x1880（旧） | **已否定** | main_std_v2.log / scan winA |
| A9 | 窗口=0x170（修正） | 已验证 | run_full_0x0 + P1 变值×2 |
| A10 | 28B 布局逐字段对应 | 已验证 | 哨兵逐字段命中 |
| A11 | Set 物理生效 | 已验证 | 回读一致 + 物理哨兵 |
| A12 | P6 dump trap handler | **已否定** | p6_run1.log (err 101) |
| A13 | Gapless 覆盖 | 已验证 | 批基址等差 + unwritten=0 |
| A14 | 哨兵无歧义 | 已验证 | matches=4 唯一命中 |
| A15 | 0xCC 判别法 | 已验证 | unwritten=0 全部 |
| A16 | trap handler 视野同 0x170 | 待验证·T3 | — |
| A17 | 0x170 用于 guardian/resume | 待验证·T3/T4 | — |
| B1 | 三件套 sm_120 可用 | 已验证 | inject_120.* |
| B2 | astoolspatch⊥entry | 已验证 | build.ps1 / inject_120 |
| B3 | BPT.TRAP 跨架构稳定 | 已验证 | 三方比对 |
| B4 | nvdisasm SM120A 注意 | 已验证 | Makefile / cc.asm |
| B5 | Makefile Windows 驱动 | 已验证 | 全部产物 |
| B6 | sm86 对照基线 | 已验证 | inject_86.* |
| B7 | T2→T3 输入完备性 | 待验证·T3 | — |
