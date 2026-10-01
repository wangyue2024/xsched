# XSched Guardian 指令生成工具链（tools/instrument）

> **定位**：把 `platforms/cuda/hal/inject/inject.cu` 编译产物中的 `brkpt`
> 占位（SASS `BPT.TRAP`）自动替换为源码注释中声明的目标指令，并生成
> `arch/sm{N}.cpp` 可用的 `uint64_t` 指令数组——即把 sm86 时代的人工
> 编辑流水线**完全自动化**的工程资产（设计文档 T4）。
>
> 状态：**T4 完成**（sm86 金标准回归 9/9 PASS，19/19 编码与官方人工
> 编辑逐位一致；sm120 硬件窗口 0x170 系列已由 T1 实证、本工具自动
> 寻址）。证据：`test/sm120_l2_gen/evidence/`。

---

## 1. 背景：官方（sm86）生成流水线

```
inject.cu  ──nvcc -cubin --keep-device-functions -Xptxas -astoolspatch──▶  inject_<arch>.cubin
    │  每个 nop() (= asm volatile("brkpt;")) 在产物中是 16 字节 BPT.TRAP 占位
    ▼
cuobjdump -sass ──▶ inject_<arch>.asm   （人工审阅文件）
    │  人工把每个占位 1:1 原地替换为注释里写的 SASS 指令
    │  （“坑位数 = 指令数”约束：64 位指针 = 两个占位 = 两条 LDC）
    ▼
arch/sm86.cpp   （uint64_t 数组，运行时由 cuXtraInstrMem* 拷贝进指令内存）
```

本工具自动完成“人工替换”这一步，并附带三重校验（round-trip 自检、
K1 等长替换断言、控制流不变断言）。

## 2. 三个子命令

### 2.1 review —— 编译基线审查（T3）

```powershell
python tools/instrument/extract_sass.py review --asm platforms/cuda/hal/inject/inject_120.asm [--out report.md]
```

输出每函数指令普查表（指令数 / 最大寄存器号 / BSSY / BSYNC / BRA /
BREAK / BAR / LDC / STL / LDL / LD.ST.E / BPT / NOP / EXIT / RET.ABS）。
用于：L2 路径“无局部内存（STL/LDL=0）”契约检查、寄存器上限测算、
占位计数核对。

### 2.2 generate —— 占位替换与数组生成

```powershell
# sm120（本仓库当前目标）：debugger 窗口 0x170..0x188（T1 实证）
python tools/instrument/extract_sass.py generate `
    --cubin platforms/cuda/hal/inject/inject_120.cubin `
    --source platforms/cuda/hal/inject/inject.cu `
    --offset-map sm120 `
    --out-cubin evidence/patched_120.cubin `
    --out-cpp   evidence/preview_sm120.cpp `
    --report    evidence/gen_sm120_report.json

# sm86（对照 / 回归）
python tools/instrument/extract_sass.py generate ` 
    --cubin inject_86.cubin --source inject.cu --offset-map sm86 ...
```

参数：

| 参数 | 说明 |
|---|---|
| `--cubin` | nvcc 产物（含 BPT.TRAP 占位） |
| `--source` | `inject.cu`（从 `nop(); // <spec>` 注释提取插入规格） |
| `--offset-map` | `identity`（原样）/ `sm86`（恒等八槽）/ `sm120`（0x170 系列映射） |
| `--func` | 可重复；默认 `check_preempt restore_exec`（L2 主路径） |
| `--trim` | `auto`（按函数类型：guardian=收口裁剪 / resume=保留出口）/ `guardian` / `resume` / `none` |
| `--out-cubin` | 替换后的 cubin（供 nvdisasm/cuobjdump 独立复检） |
| `--out-cpp` | 数组预览（**PREVIEW**，正式版由 T5 审查后落盘） |
| `--report` | JSON 报告（每个替换点的前后编码、裁剪明细、校验结果） |

### 2.3 golden —— sm86 金标准回归

```powershell
python tools/instrument/extract_sass.py golden `
    --cubin-86  platforms/cuda/hal/inject/inject_86.cubin `
    --source    platforms/cuda/hal/inject/inject.cu `
    --official-asm platforms/cuda/hal/inject/inject_sm86.asm `
    --official-cpp platforms/cuda/hal/src/arch/sm86.cpp `
    --out-cubin evidence/patched_86.cubin --report evidence/golden_report.json
```

9 项检查（G1–G3b，全部需 PASS）：
1. **G1**：官方数组 == 官方 asm（逐字节）；
2. **G2 / G2b**：工具的每条编码输出（LDC/STL/LDL/IMAD.MOV/@P0 MOV/
   ISETP）都能在官方数组/asm 中按序逐位命中——证明编码器与当年
   人工编辑**逐位等价**；
3. **G3a**：对**新编译**的 cubin 重放替换后，同一编码序列可完全复现；
4. **G3b**：新旧指令流的对齐 diff 摘要（归因“编译器调度差异”）。

## 3. 核心概念

### 3.1 插入规格（insertion spec）

`inject.cu` 中每个占位都带注释规格，工具解析以下六类：

| 源码注释 | 语义 |
|---|---|
| `// LDC Rn, c[0x0][0x1880];` | 读调试窗口（偏移经 `--offset-map` 重映射） |
| `// STL [0xfffe00], Rn;` | 栈存（trap 路径专用） |
| `// LDL Rn, [0xfffe00];` | 栈取（trap 路径专用） |
| `// IMAD.MOV.U32 Rn, RZ, RZ, RZ;` | 寄存器清零 |
| `// @P0 MOV Rn, 0x1;` | 谓词常量装载 |
| `// ISETP.NE.AND P0, PT, Rn, RZ, PT;` | 谓词比较 |

### 3.2 编码器（全部经官方数组 + sm120 硬件交叉验证）

| 指令 | word0 公式 | word1（控制码模板） |
|---|---|---|
| `LDC Rn, c[0x0][off]` | `0xff007b82 \| (n<<16) \| ((off>>2)<<40)` | `0x000fc00000000800` |
| `LDC.64 Rn, c[0x0][off]` | 同上（n 必须为偶） | `0x000fc00000000a00`（bit9=1） |
| `STL [imm], Rn` | `0xff007387 \| (n<<16) \| (imm<<40)` ← **直存字节偏移** | `0x000fc00000100800` |
| `LDL Rn, [imm]` | `0xff007983 \| (n<<16) \| (imm<<40)` | `0x000fc00000100800` |
| `IMAD.MOV.U32 R0, RZ, RZ, RZ` | `0x000000ffff007224` | `0x000fe200078e00ff` |
| `@P0 MOV R0, 0x1` | `0x0000000100000802` | `0x000fe20000000f00` |
| `ISETP.NE.AND P0, PT, Rn, RZ, PT` | `0x000000ff0000720c \| (n<<24)` | `0x004fda0003f05270` |

> 注意 LDC 的偏移字段是**字偏移**（`off>>2`），STL/LDL 是**字节偏移
> 直存**——这是 2026-10 通过官方样本差分确认的（见 T4_REPORT §3）。
> LDC 公式在 sm120 上由 12/12 条 ptxas 自然样本自动交叉验证
> （`evidence/crosscheck_sm120_natural_ldc.txt`）。

### 3.3 尾部裁剪（K3 契约）

两种拼接形态对尾部的要求不同，`--trim auto` 会自动选择：

- **guardian**（前缀型，拼接在 kernel 前面）：从尾部依次剥掉
  `NOP 填充 → 自旋 BRA → 函数尾声 RET.ABS.NODEC R20`，**必须收口
  在 BSYNC**（之后物理 fallthrough 进入原 kernel）。裁剪只改变数组
  长度，不动任何保留指令的编码，因此分支距离天然保持。
- **resume**（跳板型）：`RET.ABS.NODEC R20` 是功能出口（R20:R21 由
  `LDC R20/R21 @ c[0x0][窗口第②槽]` 装载，跳回 per-kernel guardian），
  必须保留；其后的 BRA/NOP 亦保留（与官方数组一致，不可达）。

### 3.4 调试窗口偏移映射（--offset-map sm120）

| 源码槽位（sm86 ABI） | 语义 | sm120 ABI（T1 实证） |
|---|---|---|
| `c[0x0][0x1880]/[0x1884]` | preempt_buf 指针 | `c[0x0][0x170]/[0x174]` |
| `c[0x0][0x1888]/[0x188c]` | guardian 入口（跳回目标） | `c[0x0][0x178]/[0x17c]` |
| `c[0x0][0x1890]/[0x1894]` | kernel_idx | `c[0x0][0x180]/[0x184]` |
| `c[0x0][0x1898]` | killable 标志 | `c[0x0][0x188]` |

## 4. 已知边界（务必阅读）

1. **trap 路径的 sm120 编码未在硬件验证**：`check_preempt_trap` /
   `exit_if_idempotent` 用到的 STL/LDL/IMAD.MOV/@P0 MOV/ISETP 目前
   产出的是 **sm86 已验证编码**；sm120 上是否需要换成新变体（如
   `HFMA2` 版 MOV）属于阶段三（Level-3 trap）范畴，届时须按本文
   第 5 节 SOP 重新验证。**L2 主路径（check_preempt/restore_exec）
   只依赖 LDC，已全部实证。**
2. `--out-cpp` 产物是 **PREVIEW**：正式 `arch/sm120.cpp` 需经 T5
   人工逐指令审查后落盘（含 `RequiredRegs/RequiredBarriers` 定值）。
3. 控制码（word1）来自模板而非运行时继承；对 L2 路径沿用官方
   0x000fc00000000800 模板（T1 硬件验证通过）。若未来发现调度
   冒险，可改为从同函数自然样本继承控制码。
4. 工具不重定位分支：等长 1:1 替换下分支编码必然保持（已断言）；
   若未来引入非等长编辑，必须先扩展 Pass 校验。

## 5. 新架构适配 SOP（下一个 sm 世代）

1. `cd platforms/cuda/hal/inject && make_msvc.bat ARCH=<NN> bin dump cc`
   （Windows；Linux 用 `make ARCH=<NN> bin dump cc`）；
2. `python tools/instrument/extract_sass.py review --asm inject_<NN>.asm`
   —— 核对：函数完整、占位计数（26 个）、L2 路径 STL/LDL=0、
   寄存器上限、BAR 计数；
3. **先解决窗口定址**：用 T1 探针方法论（`test/sm120_l2_probe/`）
   定位新架构的 debugger-parameters 窗口，得到新的 `--offset-map`；
4. `generate --offset-map <新映射>` → `patched` cubin →
   `nvdisasm -c` 复检 LDC 文本；
5. 对现有最新架构跑一次 `golden` 确认工具未回归；
6. T5 人工审查 → 落盘 `arch/sm<N>.cpp` → MVE（T6）。

## 6. 文件与证据索引

| 文件 | 说明 |
|---|---|
| `tools/instrument/extract_sass.py` | 本工具（约 960 行，纯标准库） |
| `test/sm120_l2_gen/evidence/golden_report.json` | 金标准回归机器可读报告 |
| `test/sm120_l2_gen/evidence/preview_sm120.cpp` | sm120 数组预览（guardian 50 + resume 32） |
| `test/sm120_l2_gen/evidence/patched_{86,120}.cubin` | 替换后 cubin（可独立反汇编复检） |
| `test/sm120_l2_gen/T3_REPORT.md` / `T4_REPORT.md` | 阶段报告 |
| `test/sm120_l2_gen/run_all_t3t4.ps1` | 一键复现脚本 |
| `test/sm120_l2_gen/seal_t3t4.ps1` | 证据哈希封存脚本 |
