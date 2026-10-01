# T3/T4 证据封存清单（MANIFEST）

> **范围**：sm120 Level-2 支持第三/四阶段（T3 编译基线与差异审查、
> T4 指令提取自动化工具与金标准回归）的全部验证证据。
> 哈希清单：`SHA256SUMS_T3T4.txt`（36+2 条目，本文件录入后重跑
> `seal_t3t4.ps1` 即为最终值）；任何字节级改动都会破坏一致性。
>
> 日期：2026-10-01 · 封存脚本：`../seal_t3t4.ps1`（可重复执行）

---

## 1. 决定性证据（工具正确性）

| # | 证据 | 位置 | 说明 |
|---|---|---|---|
| 1 | 金标准回归 9/9 PASS | `golden_report.json` | G1(字节级 53/32) + G2/G2b(19/19 编码逐位) + G3a/G3b |
| 2 | 回归运行输出 | `run_all_output.txt` | 一键脚本全绿记录（rc=0） |
| 3 | 工具输出 cubin（sm86） | `patched_86.cubin` | 替换后二进制（BPT 26→18） |
| 4 | nvdisasm 独立复检 | `patched_86_cc.asm` | LDC R4/R5/R6/R7@0x1880..、R20/R21@0x1888..（rc=0） |
| 5 | cuobjdump 独立复检 | `patched_86_sass.txt` | 同上的 cuobjdump 视角（BPT=18, LDC×8） |
| 6 | 自然 LDC 公式交叉验证 | `crosscheck_sm120_natural_ldc.txt` | sm120 12/12 ptxas 自然样本与编码公式一致 |

## 2. sm120 生成演练证据

| # | 证据 | 位置 | 说明 |
|---|---|---|---|
| 7 | 生成报告 | `gen_sm120_report.json` | 8 个替换点（guardian×4 + resume×4）的 before/after 双字、裁剪明细 |
| 8 | 预览数组 | `preview_sm120.cpp` | guardian 50 条（BSYNC 收口）+ resume 32 条（RET 出口） |
| 9 | 输出 cubin | `patched_120.cubin` | 替换后二进制（BPT 26→18） |
| 10 | nvdisasm 复检 | `patched_120_cc.asm` | LDC R4-R7@0x170/174/180/184 + R20/R21@0x178/17c |
| 11 | 复现运行（第二次） | `run2/`（5 文件） | 与第一次逐字节一致（SHA256 MATCH ×5） |

## 3. T3 审查证据

| # | 证据 | 位置 | 说明 |
|---|---|---|---|
| 12 | sm120 函数普查表 | `review_inject_120.md` | 12 函数：指令数/寄存器/BSSY/BSYNC/BAR/LDC/STL/LDL/BPT... |
| 13 | sm86 新编译对照表 | `review_inject_86_new.md` | CUDA 12.9 重编译基线 |
| 14 | sm86 官方（CUDA 11）对照表 | `review_inject_sm86_official.md` | 人工编辑版基线（含 changed 行） |
| — | T3/T4 报告正文 | `../T3_REPORT.md` / `../T4_REPORT.md` | 方法、结果、差异清单、边界 |

## 4. 环境（承 T1/T2）

- GPU：NVIDIA GeForce RTX 5060（sm_120 / CC 12.0），驱动 610.88；
- 工具链：nvcc 12.9.41 / cuobjdump 12.9.26 / nvdisasm 12.9.19 /
  Python 3.11.9（`test/sm120_l2_probe/evidence/toolchain_versions.txt`）；
- 编译元数据：`inject_120.json` 头部（SM120 SchemaVersion 12.8、
  tki_toolOptions `-arch sm_120 -m 64 -astoolspatch`）。

## 5. 源码与产物清单（纳入哈希）

- 工具与文档：`tools/instrument/extract_sass.py`（960 行）、
  `tools/instrument/README.md`；
- 阶段文档与脚本：`T3_REPORT.md`、`T4_REPORT.md`、`run_all_t3t4.ps1`、
  `seal_t3t4.ps1`；
- 编译基线：`inject_120.{cubin,asm,_cc.asm,json}`、
  `inject_86.{cubin,asm,_cc.asm,json}`、`inject_sm86.asm`、`inject.cu`、
  `Makefile`、`make_msvc.bat`、`src/arch/sm86.cpp`；
- 本目录 `evidence/` 全部文件。

## 6. 复现指令（一键）

```powershell
# 前置：CUDA 12.9 在 PATH；inject_{86,120} 基线已生成（T2）
cd test\sm120_l2_gen
powershell -ExecutionPolicy Bypass -File run_all_t3t4.ps1   # 期望：ALL PASSED / exit 0
powershell -ExecutionPolicy Bypass -File seal_t3t4.ps1      # 重建哈希清单
```

预期关键断言：golden 9/9 PASS；generate `guardian -14 → 50 条`、
`resume → 32 条`；nvdisasm 复检 `BPT.TRAP == 18` 且六条窗口 LDC
在场；run2 五产物哈希 MATCH。

## 7. 封存完整性

- `seal_t3t4.ps1` 对上述三类（evidence 全量 + 源码/文档 + 产物）
  生成 `SHA256SUMS_T3T4.txt`；输出文件自身被排除；
- 校验方式：`Get-FileHash` 与清单逐条比对即可发现任何改动；
- 与 T1/T2 封存（`test/sm120_l2_probe/evidence/SHA256SUMS.txt`，163+
  条目）互为独立、互不覆盖。
