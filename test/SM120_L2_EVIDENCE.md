# SM120 Level-2 支持：证据链总索引（EVIDENCE INDEX）

> **分支定位**：本分支（`evidence/sm120-level2`）封存 sm120（Blackwell, CC 12.0）
> Level-2 抢占支持从机制探针到真机验收的**完整证据链、验证过程与详细文档**。
> 生产代码的干净发布版本见 `release/sm120-level2` 分支（同源内容，规范英文注释）。
>
> **最终结论**：T1–T7 五阶段、**127 项功能检查 + 1000 轮压力循环，全部通过**
> （见 `sm120_integration/evidence/FINAL_ACCEPTANCE.md`）。

---

## 1. 任务链与证据映射

| 阶段 | 目的 | 目录 | 关键证据 |
|---|---|---|---|
| T1 | cuxtra 机制探针（gate） | `sm120_l2_probe/` | `ASSUMPTIONS.md`、`VERIFICATION.md`、`evidence/MANIFEST.md`、`evidence/SHA256SUMS.txt`（**165 条**）、`evidence/recon/cuxtra_lib_full.dis.asm`、`evidence/scan{,_v2}/`；验收记录 23/23 PASS |
| T3 | guardian 编译基线（sm120 反汇编审查） | `sm120_l2_gen/` | `T3_REPORT.md`、`inject_120.asm`（上层目录 `platforms/cuda/hal/inject/`） |
| T4 | `extract_sass.py` 提取工具 + 金标准回归 | `sm120_l2_gen/` | `T4_REPORT.md`、`evidence/golden_report.json`（**9/9 PASS**）、`evidence/run2/`（SHA256 位级可复现）、`tools/instrument/extract_sass.py` |
| T5 | GuardianSM120 数组生成与逐条人工审查 | `sm120_mve/` | `T5_REVIEW.md`、`t5_verify.py`（**21 断言**）；产物 `platforms/cuda/hal/{include,src}/.../sm120.*` |
| T6 | MVE 最小可行实验（真机闭环：插桩→阻断→恢复） | `sm120_mve/` | `DESIGN.md`、`REPORT.md`、`evidence/MANIFEST_T5T6.md`、`evidence/SHA256SUMS_T5T6.txt`（**34 条**）、`mve_*.log`、`mve_kernel.cubin`；**48 PASS / 0 FAIL**、1000 轮压力 6.4s 收敛 |
| T7 | 全栈集成验收（shim+preempt+sched 全链路） | `sm120_integration/` | `T7_REPORT.md`、`evidence/FINAL_ACCEPTANCE.md`、`evidence/SHA256SUMS_T7.txt`（**33 条**）、`app_l2_*.log`；**26 PASS / 0 FAIL**、L2 停转 3.3ms |
| 设计 | 总体路线图 / L2 详设 | `docs/` | `sm120-support-design.md`、`sm120-level2-design.md` |
| 工具 | 新架构适配 SOP | `tools/instrument/` | `README.md`、`extract_sass.py`；生成链 `platforms/cuda/hal/inject/`（`inject.cu`/`Makefile`/`make_msvc.bat`） |

## 2. 封存体系（4 套独立 SHA256 封存，互不覆盖）

| 封存清单 | 条目 | 校验 |
|---|---|---|
| `sm120_mve/evidence/SHA256SUMS_T5T6.txt` | 34 | 2026-10-03 复验后由 `seal_t5t6.ps1` 重新生成，逐条 0 错误 |
| `sm120_integration/evidence/SHA256SUMS_T7.txt` | 33 | 逐条 0 错误 |
| `sm120_l2_gen/evidence/SHA256SUMS_T3T4.txt` | 39 | 逐条 0 错误 |
| `sm120_l2_probe/evidence/SHA256SUMS.txt` | 165 | 逐条 0 错误 |

**271 条封存条目全部校验通过**（校验方法：`Get-FileHash` 与清单逐条比对）。

约定与说明：

- 封存清单由各目录的 `seal_*.ps1` 生成，**可重复执行**（重跑即重封存）；
- `*.log` 在仓库 `.gitignore` 中被全局忽略；**本分支作为证据档案刻意强制纳入**
  运行日志（与 SHA256SUMS 引用一致）；
- **2026-10-03 复验记录**见 `sm120_mve/evidence/MANIFEST_T5T6.md` §7：
  干净环境全量通过（`mve_run.log`，48 PASS）；M4「注入时序竞争」为已知
  WDDM 批次对齐特性（样本 `mve_reverify_m4_late.log`），机制断言不受影响，
  该现象在 `FINAL_ACCEPTANCE.md` §5 有完整量化解剖；
- 原 `mve_run.log` 首次封存版本被复验运行同名覆盖（可重复生成的运行日志，
  非唯一产物）；其余全部历史样本（`mve_full_run1-3.log`、`mve_final.log`、
  `mve_stability*.log` 等）保持原封存状态。

## 3. 复现指南（一键）

环境：Windows、RTX 5060（sm120）、驱动 610.88、CUDA 12.9.41（`nvcc`）、
MinGW g++ 14.2、Python 3.11。

```powershell
# 0) 构建 XSched（根目录）
mingw32-make cuda            # 产物安装至 output/（含 nvcuda.dll 代理）

# 1) T1 探针（可选重跑）
cd test\sm120_l2_probe; powershell -File build.ps1; .\probe_main.exe probe_kernels_patched.cubin

# 2) T3/T4 工具链与金标准回归
cd test\sm120_l2_gen; powershell -File run_all_t3t4.ps1     # 预期 9/9 golden + 可复现

# 3) T5 数组审查 + T6 MVE 真机闭环
cd test\sm120_mve
powershell -File build.ps1                                  # gen + device + host
powershell -File run_mve.ps1                                # 预期 48 PASS / 0 FAIL
python t5_verify.py                                         # 预期 all T5 checks PASSED
powershell -File seal_t5t6.ps1                              # 重新封存（可选）

# 4) T7 全栈集成
cd test\sm120_integration
powershell -File build.ps1; powershell -File run.ps1        # 预期 26 PASS / 0 FAIL
```

MVE 运行提示：`run_mve.ps1` 默认带 `--preflight` 环境传感器；出现
`ENVIRONMENT NOT CLEAN` 警告说明存在外部 GPU 负载，数据可信度下降，建议
关闭重负载应用后重跑。M4 的「部分在飞」读数（0<done<N）随 WDDM 批次对齐
在 288–2048 间波动，功能断言对任意 0<done<N 恒真。

## 4. 已知边界

- **仅 Level-2**：sm120 的 Level-3（trap/interrupt）为阶段三，未实现；
  Windows 上 sm120 的调度路径固定为 `CudaQueueLv2`（见 `arch.cpp` 注释）。
- **验收范围**：功能验收在 Windows/WDDM 完成；性能基线（并发配额 ≈59 块）
  为 WDDM 特性，Linux/TCC 预期可获全量并发。
- CUDA Graph 内部 kernel 不支持 L2 插桩，自动回退 L1（代码内 XWARN）。

## 5. 遗留文件清理记录（2026-10-03）

以下文件在整理上一提交（`6c07b2e`）时被判定无证据价值并剔除
（仍可从 `6c07b2e` 历史检出）：

- 0 字节空文件（6）：`test/run_bench.ps1`、`test/run_threshold_test.ps1`、
  `test/test_comprehensive.cpp`、`test/test_kernel_time.cpp`、
  `test/test_memfree_delay.cpp`、`test/test_ptx.cpp`；
- 旧基准临时产物（6）：`test/bench_result.txt`、`test/result_{deferred,direct,legacy,native,threshold}.txt`；
- 孤儿脚本（1）：`test/run_full_comparison.ps1`（引用不在仓库中的
  `test/native|direct|...` 目录，且其输入产物已移除）。

## 6. 相关分支

| 分支 | 定位 |
|---|---|
| `release/sm120-level2` | 仅生产代码（严格工程规范：英文注释、README 支持矩阵更新、无实验残留） |
| `evidence/sm120-level2` | 本分支：生产代码 + 全部证据链、验证过程、设计文档与记录 |
| `main` / `sm120-level2` | 原始开发历史（`6c07b2e`，未整理状态，保留不动） |
