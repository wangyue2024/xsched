# T5 审查报告：sm120.h / sm120.cpp（GuardianSM120）

> **文档定位**：记录 sm120 Level-2 guardian/resume 指令数组的**落盘产
> 物、程序化审查、逐条人工审查与编译验证**（设计任务卡 T5）。
> 上游：T4 生成管线（`test/sm120_l2_gen/T4_REPORT.md`）；
> 下游：T6 MVE（`DESIGN.md` / `REPORT.md`）。
>
> 日期：2026-10-01 · 状态：**T5 完成**（21 项自动断言 + 逐条审查 + 库构建全通过）

---

## 1. 产物与来源追溯

| 产物 | 位置 |
|---|---|
| 类声明 | `platforms/cuda/hal/include/xsched/cuda/hal/arch/sm120.h` |
| 指令数组 | `platforms/cuda/hal/src/arch/sm120.cpp` |
| 程序化审查器 | `test/sm120_mve/t5_verify.py` |

**来源链**（全部可复现、可追溯）：

```
inject.cu ──nvcc 12.9.41 -arch sm_120 -astoolspatch──▶ inject_120.cubin
    │  tools/instrument/extract_sass.py generate --offset-map sm120
    │  （含 round-trip 自检、K1 断言、guardian 尾部裁剪 -14）
    ▼
test/sm120_l2_gen/evidence/preview_sm120.cpp  （50 + 32 条，已入封存）
    │  逐位复制（脚本复核实测 MATCH：100+64 words）
    ▼
platforms/cuda/hal/src/arch/sm120.cpp
```

审查器 `t5_verify.py` 独立重放上游管线并与落盘数组 **bit-exact**
比对（R1），杜绝手抄污染。

## 2. 程序化审查结果（t5_verify.py，21/21 PASS）

```text
[R1] guardian == patch(inject_120.cubin) + guardian trim     PASS (50 instrs)
[R1] resume   == patch(inject_120.cubin), no trim            PASS (32 instrs)
[R2] guardian patched LDC == R4/R5@0x170/174 + R6/R7@0x180/184   PASS
[R2] guardian natural gridDim LDC == R3@0x374/0x378              PASS
[R2] resume patched LDC == R4/R5@0x170/174 + R20/R21@0x178/17C   PASS
[R2] no LDC outside the verified window/gridDim set              PASS
[R3] guardian: 3 BSSY/BSYNC pairs, all resolve                   PASS
[R3] resume:   1 BSSY/BSYNC pair, resolves                       PASS
[R3] guardian caps on BSYNC (K3 contract)                        PASS
[R3] resume contains RET.ABS.NODEC R20 (jump-back exit)          PASS
[R4] lengths 50/32; EXIT x1 each (@P0 / @!P0); BAR.SYNC x1 each  PASS
[R4] guardian MEMBAR.SC.CTA x1; WARPSYNC.ALL x1 each             PASS
[R4] resume self-spin BRA x1 (unreachable pad)                   PASS
[R5] 16-byte alignment; no BPT.TRAP residue                      PASS
```

其中 **R3 的 BSSY→BSYNC 配对**为二进制级证明
（`目标地址 = 本指令地址 + w0[32:62]` 公式，五个完整解出）：
guardian `B0@0→0x310`, `B1@0x120→0x250`, `B2@0x1b0→0x230`；
resume `B0@0→0x150`。

## 3. 逐条人工审查（语义分段）

### 3.1 guardian_instructions（50 条 = 800 字节）

| 段 | 指令 | 语义角色 |
|---|---|---|
| P | `BSSY.RECONVERGENT B0` | 收口屏障：最后一条 `BSYNC.RECONVERGENT B0`（+0x310） |
| ① | `LDC R4/R5 @0x170/0x174` | preempt buffer 指针（T1 实证槽位；patched） |
| ② | `S2R×6 + LDC R3@0x374/378 + IMAD×2 + IADD3 + LOP3` | block_idx 计算（gridDim via 0x374/378）与"非 block 0 线程"谓词 P0 |
| ③ | `IMAD.WIDE.U32 R8 = R4 + 4*(2*block_idx+4)` | `&block_exit_flag[block_idx]` 地址 |
| ④ | `@P0 BRA` / `LD.E R0,[R4]` / `BSSY.RECONVERGENT B1` | 仅 block 0 首线程继续；读 global_exit_flag |
| ⑤ | `@!P0 ST.E [R8],RZ` + `@!P0 BRA` | flag==0 路径：清 exit_flag，直奔收尾 |
| ⑥ | `LDC R6/R7 @0x180/0x184` | kernel_idx（patched；对应 sm86 的 0x1890/0x1894） |
| ⑦ | `HFMA2 R3,-RZ,RZ,0,5.96e-8` + `ST.E [R8],R3` | 置 `*block_exit_flag = 1`（HFMA2 即 sm120 的 MOV-1，D7 差异） |
| ⑧ | `LD.E.64 R10,[R4+8]` + `BSSY.RELIABLE B2` | 读 preempt_idx（64 位） |
| ⑨ | `ISETP.NE.S64 ×2 + @P0 BREAK.RELIABLE + BRA` | 三分支：==0 → 记录 idx+restore；==kernel_idx → 记录 restore；否则不记录（K13） |
| ⑩ | `ST.E.64 [R4+8],R6` / `ST.E [R8+4],R3` / `BSYNC.RELIABLE B2` | 写回 preempt_idx / restore_flag |
| ⑪ | `@!PT LDS RZ,[RZ] ×4` | MEMBAR 前的依赖填充（编译器产物，无副作用） |
| ⑫ | `MEMBAR.SC.CTA` | fence（CTA 域）→ 保证标志可见 |
| ⑬ | `WARPSYNC.ALL` + `NOP` + `BAR.SYNC.DEFER_BLOCKING 0x0` | 块内同步 |
| ⑭ | `LD.E R8,[R8]` + `ISETP` + `@P0 EXIT` | exit_flag!=0 → 退出（`@P0 EXIT` = 0x…094d） |
| 收口 | `BSYNC.RECONVERGENT B0` | **物理 fallthrough 进入原 kernel**（无 RET，K3） |

### 3.2 resume_instructions（32 条 = 512 字节）

| 段 | 指令 | 语义角色 |
|---|---|---|
| P | `BSSY.RECONVERGENT B0` | 收口于 `BSYNC.RECONVERGENT B0`（+0x150） |
| ① | `LDC R4/R5 @0x170/0x174` | preempt buffer 指针（patched） |
| ② | `S2R×3 + LDC R3/R7@0x374/378 + IMAD×2 + IADD3` | `&restore_flag[block_idx]`（偏移 2*bid+5） |
| ③ | `LD.E R0,[R4]` + `ISETP` + `@!P0 EXIT` | restore_flag==0 → 退出（**这些 block 此前已完成**） |
| ④ | `WARPSYNC.ALL + NOP + BAR.SYNC + ST.E [R4],RZ` | 同步 + 清 restore_flag |
| ⑤ | `LDC R20/R21 @0x178/0x17c` | **跳回目标**：per-kernel guardian 入口（第②槽，patched） |
| 收尾 | `BSYNC.RECONVERGENT B0` + `RET.ABS.NODEC R20 0x0` | 功能出口：`RET.ABS` 跳到 R20:R21（K10 两段式恢复） |
| 填充 | `BRA 自旋` + `NOP×8` | 官方数组同构保留（RET 后不可达） |

### 3.3 与 sm86 官方数组的语义一一映射

| sm86 官方（CUDA 11） | sm120（本产物） | 等价性 |
|---|---|---|
| LDC R4/R5 @0x1880/84 | LDC R4/R5 @0x170/174 | 窗口重映射（T1 实证） |
| LDC R6/R7 @0x1890/94 | LDC R6/R7 @0x180/184 | 同上 |
| LDC R20/R21 @0x1888/8c | LDC R20/R21 @0x178/17c | 同上 |
| IMAD.MOV.U32 R3,…,1 | HFMA2 R3,-RZ,RZ,0,5.96e-8 | 同一语义（置 1，D7） |
| ISETP.NE.U32+EX（2 条） | ISETP.NE.S64（1 条） | 同一语义（D9） |
| BSSY/BSYNC/BREAK | .RECONVERGENT/.RELIABLE 变体 | Blackwell 变体（D2/D3） |
| `@!P0 BRA+EXIT` 布局 | `@P0 EXIT` 布局 | 调度差异（D6） |
| BAR.SYNC | BAR.SYNC.DEFER_BLOCKING | 主码相同（D4） |

## 4. 资源声明（供 T7 接线使用）

- `RequiredRegs = 32`：guardian 实际最大寄存器 R11、resume R21（含
  R20/R21 配对 LDC），保底 32 覆盖两者（与 sm86 同值）；
- `RequiredBarriers = 1`：两数组各有 1 条 BAR.SYNC；
- 数组均 16 字节对齐、无 BPT.TRAP 残留；resume 的
  `RET.ABS.NODEC R20` 出口与 guardian 的 BSYNC 收口互为配套。

## 5. 编译验证

| 检查 | 命令 | 结果 |
|---|---|---|
| 语法 | `g++ -std=c++17 -I platforms/cuda/hal/include -fsyntax-only sm120.cpp` | exit 0 |
| 目标文件 | `g++ -O2 -c sm120.cpp` | 5171 字节 |
| **完整库** | `cmake --build build --target halcuda`（MinGW Makefiles，`-Werror` 全开） | **Built target halcuda** |

注：首次库构建捕获并修复 1 处 `-Werror=comment`（注释行尾续行符 `\`）；
修复后全量通过。`file(GLOB_RECURSE)` 已在 reconfigure 后收录 sm120.cpp，
无需改动 CMakeLists。**arch.cpp 的 `case 120` 注册不在 T5 范围**
（设计文档 T7；Windows 下 L2 仍被 `#if defined(_WIN32)` 短路，注册与否
不影响现状，MVE 走 driver 直调不经过 arch.cpp）。

## 6. 复现指令

```powershell
python test\sm120_mve\t5_verify.py          # 期望: all T5 checks PASSED / exit 0
cmake -S . -B build && mingw32-make -C build halcuda
```
