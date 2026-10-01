# T4 报告：extract_sass.py 指令提取自动化工具与金标准回归

> **文档定位**：记录 sm120 Level-2 支持第四阶段（T4：指令提取自动化
> 工具 + sm86 金标准回归 + sm120 生成演练）的实现、验证与结论。
> 设计任务卡见 `docs/sm120-level2-design.md` §4 T4。
>
> 日期：2026-10-01 · 状态：**T4 完成**
> （金标准回归 9/9 PASS；19/19 编码与官方人工编辑逐位一致；
> sm120 演练产物全链路可复现）

---

## 1. 目标与产出

| 设计目标 | 结果 |
|---|---|
| 指令提取自动化工具 | ✅ `tools/instrument/extract_sass.py`（960 行，纯标准库，三子命令） |
| 五 Pass 架构（切分/改写/校验/断言/生成） | ✅ 见 §2 |
| LDC offset 立即数改写（两点定标法思想） | ✅ 升级为**全字段公式化编码** + 自然样本自动交叉验证（§3） |
| 分支距离校验（兜底重定位） | ✅ 等长 1:1 结构性保证 + 控制流逐字节断言（K1） |
| 金标准回归：工具重现 sm86 官方数组 | ✅ 9/9 检查 PASS（§4） |
| 使用文档（新架构适配 SOP） | ✅ `tools/instrument/README.md` |

**产出文件**：
- 工具：`tools/instrument/extract_sass.py` + `README.md`；
- 回归报告：`evidence/golden_report.json`（机器可读）；
- sm120 演练：`evidence/patched_120.cubin`、`evidence/preview_sm120.cpp`、
  `evidence/gen_sm120_report.json`；
- 独立复检反汇编：`evidence/patched_{86,120}_cc.asm`；
- 复现运行（第二次）：`evidence/run2/`（全部逐字节一致，§6）。

## 2. 工具架构

```
inject.cu ─(正则)→ 插入规格表 {func: [spec...]}          # parse_inject_source
inject_<arch>.cubin ─(ELF64)→ .text.<func> 指令流        # text_sections/load_instrs
        │
        ├─ Pass 1 占位定位     BPT.TRAP 双字模式匹配，计数==规格数（否则 abort）
        ├─ Pass 2 原地替换     每条规格 → 编码器（LDC/STL/LDL/IMAD.MOV/@P0 MOV/ISETP）
        ├─ Pass 3 Round-trip  生成双字即刻解码回文本，断言与规格一致
        ├─ Pass 4 K1/结构断言  非占位字逐字节不变；控制流指令集合不变；零占位残留
        ├─ Pass 5 裁剪与生成   guardian: NOP*→BR→RET 剥离 + BSYNC 收口断言
        │                      resume : 保留 RET.ABS.NODEC 出口
        ▼
patched.cubin（供 nvdisasm 独立复检） + preview_<arch>.cpp + JSON 报告
```

## 3. 编码器逆向与验证（工具的“知识”从哪来）

### 3.1 公式（全部经官方数组逐位验证）

| 指令 | word0 | word1 | 验证样本 |
|---|---|---|---|
| LDC Rn, c[0x0][off] | `0xff007b82 \| (n<<16) \| ((off>>2)<<40)` | `0x000fc00000000800` | 官方 5 条 + sm120 自然 12 条 |
| STL [imm], Rn | `0xff007387 \| (n<<16) \| (imm<<40)` | `0x000fc00000100800` | 官方 2 条 |
| LDL Rn, [imm] | `0xff007983 \| (n<<16) \| (imm<<40)` | `0x000fc00000100800` | 官方 2 条 |
| IMAD.MOV.U32 / @P0 MOV / ISETP.NE | 官方模板（寄存器字段可泛化） | — | 官方 3 条 |

### 3.2 一处关键差分修正（LDC 字偏移 vs STL/LDL 字节偏移）

初版实现按 LDC 惯例把 STL/LDL 的立即数也做了 `>>2`，G2b 随即失败。
从官方样本做差分：

```
STL [0xfffe00], R0 官方 = 0xfffe0000ff007387   → 字段(bit40..63)=0xfffe00 = imm 直存
STL [0xfffe04], R0 官方 = 0xfffe0400ff007387   → 0xfffe04 直存
LDC  R4, c[0x0][0x1880] 官方 = 0x00062000ff047b82 → 字段=0x620 = 0x1880>>2（字偏移）
```

**结论**：LDC 用字偏移（`>>2`），STL/LDL 用字节偏移直存。修正后
G2b 从 FAIL→PASS（11/11）。此事实已写入工具注释与 README §3.2。

### 3.3 LDC 公式的 sm120 自动交叉验证

对 `inject_120.asm` 中全部**编译器自然生成**的 LDC（读 gridDim 等）
逐条用公式重编码：**12/12 完全一致**（覆盖偏移 0x2f8/0x2fc/0x374/0x378、
寄存器 R3/R5/R6/R7/R9）→ 公式在 sm120 上独立成立，不依赖任何
sm86 外推。证据：`evidence/crosscheck_sm120_natural_ldc.txt`。

## 4. 金标准回归结果（9/9 PASS）

运行：

```powershell
python tools/instrument/extract_sass.py golden `
  --cubin-86  platforms/cuda/hal/inject/inject_86.cubin `
  --source    platforms/cuda/hal/inject/inject.cu `
  --official-asm platforms/cuda/hal/inject/inject_sm86.asm `
  --official-cpp platforms/cuda/hal/src/arch/sm86.cpp `
  --out-cubin evidence/patched_86.cubin `
  --report evidence/golden_report.json
```

| 检查 | 含义 | 结果 |
|---|---|---|
| G1 ×2 | 官方数组（guardian 53 / resume 32）== 官方 asm 逐字节 | PASS |
| G2 ×2 | check_preempt 4 条、restore_exec 4 条编码器输出在官方数组中按序逐位命中（@[2,3,21,22] / @[2,3,16,17]） | PASS |
| G2b | exit_if_idempotent 11 条编码（STL×2/IMAD.MOV/@P0 MOV/LDC×4/LDL×2/ISETP）与官方 asm 逐位一致（@[0,1,2,3,7,8,9,11,22,23,24]） | PASS |
| G3a ×2 | 对 **CUDA 12.9 新编译** cubin 重放替换后，同一编码序列完全复现（official@X == new@Y） | PASS |
| G3b ×2 | 新旧全流对齐 diff 摘要入档（replace 段=调度差异，见报告 JSON） | PASS |

**判定**：**19/19 条替换指令的编码与当年人工编辑逐位一致**；
G3b 中全部非替换差异为编译器版本调度差异（官方 CUDA 11 vs 12.9，
例如占位位移：check_preempt 官方 0x20/0x30/0x150/0x160 → 新版
0x10/0x20/0x130/0x140），可用报告的 `diff_runs` 逐段核对。

### 4.1 独立复检（第三方解码器）

对 `patched_86.cubin`（仅替换 L2 两函数）执行 `nvdisasm -c`：

- 返回码 0；`BPT.TRAP` 残留 18 个（26 总占位 − 8 已替换，精确）；
- check_preempt 显示 `LDC R4, c[0x0][0x1880]` / `R5@0x1884` /
  `R6@0x1890` / `R7@0x1894`；
- restore_exec 显示 `LDC R4/R5@0x1880/84` 与 `LDC R20/R21@0x1888/8c`。

即 **nvdisasm 的独立解码器认可我们写入的每一个二进制位**。

## 5. sm120 生成演练（T5 输入）

```powershell
python tools/instrument/extract_sass.py generate `
  --cubin platforms/cuda/hal/inject/inject_120.cubin `
  --source platforms/cuda/hal/inject/inject.cu --offset-map sm120 `
  --out-cubin evidence/patched_120.cubin `
  --out-cpp evidence/preview_sm120.cpp --report evidence/gen_sm120_report.json
```

| 数组 | 结果 | 内容要点 |
|---|---|---|
| guardian_instructions | **50 条**（64 全函数 − 14 尾部裁剪） | 收口 `BSYNC.RECONVERGENT B0`；`BSSY B0` 目标 0x310 精确指向 BSYNC（裁剪未扰动分支） |
| resume_instructions | **32 条**（不裁剪） | `LDC R20/R21@0x178/0x17c` + `RET.ABS.NODEC R20` 功能出口保留 |

窗口寻址（nvdisasm 复检 `patched_120.cubin`，rc=0）：
`LDC R4@0x170 / R5@0x174 / R6@0x180 / R7@0x184`（guardian，= T1 实证的
preempt_buf / kernel_idx 槽）；`LDC R4@0x170 / R5@0x174 / R20@0x178 /
R21@0x17c`（resume，第②槽 = 跳回目标）。BPT 残留 18（26−8）。

> **注意**：这是“可复现的生成演练”，尚不是落盘的 `arch/sm120.cpp`。
> T5 将在此预览上做人工逐指令审查（对照 T3 差异清单），确认后落盘，
> 并补 `RequiredRegs()=32 / RequiredBarriers()=1`。

## 6. 可复现性（两次运行逐字节一致）

| 产物 | 第一次 SHA256 前 16 | 第二次（run2/） | 判定 |
|---|---|---|---|
| patched_120.cubin | `509c816a5b312b5c…` | 同 | MATCH |
| preview_sm120.cpp | `68395ee533c94afd…` | 同 | MATCH |
| gen_sm120_report.json | `5b0d1cbcdbeb93c5…` | 同 | MATCH |
| golden_report.json | `f2bed232de72490a…` | 同 | MATCH |
| patched_86.cubin | `f3282d3bcc0e86a2…` | 同 | MATCH |

一键复现：`test/sm120_l2_gen/run_all_t3t4.ps1`（含全部命令与断言）。

## 7. 过程中发现并修复的问题（供后续维护者参考）

| # | 问题 | 根因 | 修复 |
|---|---|---|---|
| 1 | review 把 `ERRBAR` 误计为 `BAR.SYNC` | 子串匹配 | 改 `\bBAR\.` 词边界正则 |
| 2 | 官方 asm 行尾 `changed` 标记导致 4 条 LDC 漏统 | review 正则未含 changed | 两处正则统一为 `;\s*(?:changed\s*)?` |
| 3 | 多行函数头（参数带注释）未被解析 | 头行正则要求单行闭合 | 改为“标记行 + 花括号深度”状态机 |
| 4 | 子序列比对误纳自然 ISETP（假阳性） | 按 family 过滤 | 改为“规格编码序列 ⊆ 目标流”的子序列断言 |
| 5 | G3 替换未写回输出 cubin | 误传 `bytearray(data)` 副本 | 直接在 data 上替换 |
| 6 | 输出目录不存在即崩溃 | 未建父目录 | `ensure_parent()` |
| 7 | STL/LDL 立即数字段错做 `>>2` | 照搬 LDC 惯例 | 样本差分确认为字节直存（§3.2） |

## 8. 已知边界与 T5 输入

**边界**（同 README §4）：trap 路径（check_preempt_trap /
exit_if_idempotent）的 sm120 编码为 sm86 模板，属阶段三范畴；
预览数组未经人工审查；控制码用模板而非继承。

**T5 任务输入**：
1. `preview_sm120.cpp`（guardian 50 + resume 32，含逐条注释）；
2. T3 差异清单（`T3_REPORT.md` §5）作为人工审查对照表；
3. 落盘目标：`platforms/cuda/hal/src/arch/sm120.cpp` +
   `include/.../arch/sm120.h`（仿 sm86）；`RequiredRegs()=32`、
   `RequiredBarriers()=1`（依据 T3 §3）；
4. 落盘后建议重跑 `generate`（校验预览与落盘一致，哈希比对）。

## 9. 证据索引

| 内容 | 位置 |
|---|---|
| 工具源码/文档 | `tools/instrument/extract_sass.py` / `README.md` |
| 金标准回归报告 | `evidence/golden_report.json` |
| 回归输出 cubin + 反汇编 | `evidence/patched_86.cubin` / `patched_86_cc.asm` |
| sm120 生成报告 | `evidence/gen_sm120_report.json` |
| sm120 预览数组 | `evidence/preview_sm120.cpp` |
| sm120 输出 cubin + 反汇编 | `evidence/patched_120.cubin` / `patched_120_cc.asm` |
| 自然 LDC 交叉验证 | `evidence/crosscheck_sm120_natural_ldc.txt` |
| 复现运行（第二次） | `evidence/run2/`（5 产物哈希 MATCH） |
| 一键复现脚本 | `run_all_t3t4.ps1` |
| 哈希封存 | `evidence/SHA256SUMS_T3T4.txt` |
