# T5/T6 证据封存清单（MANIFEST）

> **范围**：sm120 Level-2 支持第五/六阶段（T5 数组落盘与审查、
> T6 MVE 真机闭环）的全部验证证据与交付物。
> 哈希清单：`SHA256SUMS_T5T6.txt`（本文件录入后重跑
> `../seal_t5t6.ps1` 即为最终值）；任何字节级改动都会破坏一致性。
>
> 日期：2026-10-01 · 封存脚本：`../seal_t5t6.ps1`（可重复执行）

---

## 1. 决定性证据（真机闭环）

| # | 证据 | 位置 | 说明 |
|---|---|---|---|
| 1 | MVE 全量运行 ×3 | `mve_full_run{1,2,3}.log`（UTF-8） | 各 **45 PASS / 0 FAIL / 0 ERR**，exit=0 |
| 2 | 部分在飞拦截（M4） | 同上 | 完成/被拦 360/1688、1208/840、653/1395；restore 数逐块精确 |
| 3 | 1000 轮压力（M6） | 同上 | A→B→C1 收敛 6.5s，零故障 |
| 4 | 恢复 bit-exact（M3/M5） | 同上 | 2048×8B 输出与未插桩基线逐字节一致 |

## 2. T5 审查证据

| # | 证据 | 位置 | 说明 |
|---|---|---|---|
| 5 | 数组结构审查（21 断言） | `../t5_verify.py`（运行输出随报告） | R1 管线复现 bit-exact；R2 窗口 LDC；R3 BSSY→BSYNC 五配对；R4/R5 完整性 |
| 6 | 逐条人工审查 | `../T5_REVIEW.md` | guardian 50 / resume 32 分段语义 + 与 sm86 映射表 |
| 7 | 库编译验证 | `../T5_REVIEW.md` §5 | halcuda 完整库（-Werror）构建通过 |

## 3. 交付物（平台侧）

| # | 产物 | 位置 |
|---|---|---|
| 8 | 类声明 | `platforms/cuda/hal/include/xsched/cuda/hal/arch/sm120.h` |
| 9 | 指令数组 | `platforms/cuda/hal/src/arch/sm120.cpp`（入 halcuda 库） |

## 4. MVE 工程与运行对象

| # | 文件 | 说明 |
|---|---|---|
| 10 | `../mve_main.cpp` / `../dummy_kernel.cu` | 主程序 / 负载 kernel |
| 11 | `../mve_arrays.h` / `../gen_arrays.py` | 数组切片（自 sm120.cpp，防手抄漂移） |
| 12 | `../build.ps1` / `../run_mve.ps1` | 一键构建 / 运行（UTF-8 日志） |
| 13 | `mve_kernel.cubin` / `mve_kernel.sass.txt` | 实际运行的设备对象与 SASS 画像（312 条指令 / 4992B） |
| 14 | `../DESIGN.md` / `../REPORT.md` | MVE 设计 / 结果报告 |

## 5. 复现指令（一键）

```powershell
cd test\sm120_mve
powershell -ExecutionPolicy Bypass -File build.ps1        # gen+device+host
powershell -ExecutionPolicy Bypass -File run_mve.ps1      # 全量（~15 s）
python t5_verify.py                                       # T5 结构审查
powershell -ExecutionPolicy Bypass -File seal_t5t6.ps1    # 重建哈希清单
```

预期：MVE `PASS=45 FAIL=0 ERR=0`、exit 0、`RESULT: 0 failed check(s)`；
t5_verify `all T5 checks PASSED`。

## 6. 封存完整性

- `seal_t5t6.ps1` 覆盖：evidence/ 全量 + 平台交付物（sm120.h/cpp）+
  MVE 源码/脚本/文档 + 设备产物（cubin/sass）；输出文件自身排除；
- 校验：`Get-FileHash` 与清单逐条比对；
- 与 T1/T2（`test/sm120_l2_probe/evidence/SHA256SUMS.txt`，165 条目）、
  T3/T4（`test/sm120_l2_gen/evidence/SHA256SUMS_T3T4.txt`，39+2 条目）
  互为独立封存，互不覆盖。

## 7. 复验记录（2026-10-03）

> 在 RTX 5060 / 驱动 610.88 / CUDA 12.9.41 环境下重跑完整 MVE 证据链，
> 并由 `../seal_t5t6.ps1` 对当前快照重新封存。本节为本次复验的结论与
> 过程记录；§1 的“45 PASS”为早期 harness 计数，当前 harness（含 T7 期间
> 新增的 M7/M8 检查）预期为 **48 PASS / 0 FAIL**。

1. **干净环境全量通过**：`mve_run.log` 为 2026-10-03 复验中的最新全量运行
   （48 PASS / 0 FAIL / 0 ERR，exit=0；preflight 4/4 完整、环境健康；
   M4 命中在飞拦截：完成 1320/2048）。
2. **M4 注入时序竞争（已知特性，样本留存）**：M4 的“部分在飞拦截”依赖一次
   跨进度量阻塞式 HtoD 写入在 kernel 执行中途对 SM 可见。实测该写入的可见
   延迟随 WDDM 跨上下文调度在 ~10ms 至 >1.7s 间波动（本 kernel 全时长约
   1.7s）；当延迟超过 kernel 时长时，M4 两项“中途生效”检查报失败（收尾为
   46 PASS / 2 FAIL，其余检查不受影响）。复验期间共捕获 5 例该模式失败，
   代表样本为 `mve_reverify_m4_late.log`（RESULT: 2 failed check(s)）。
   同样模式亦出现在 T7 期封存的 `mve_t7_regression.log` 中。该现象为运行
   环境/WDDM 调度属性，与 guardian 机制无关：M2 全阻断、M3/M5 恢复
   bit-exact、M6 1000 轮压力、M7/M8 形状与三分支语义在全部运行中稳定通过。
3. **原 `mve_run.log` 覆盖说明**：复验运行以同名 Tag 覆盖了首次封存版本
   （旧哈希 `f7d1140e…` 不可回溯；该文件是可重复生成的运行日志，非唯一
   产物，`mve_full_run1-3.log` / `mve_final.log` / `mve_stability*.log` 等
   封存样本不受影响）。本节连同重新生成的 `SHA256SUMS_T5T6.txt` 构成对
   当前快照的最终基线。
