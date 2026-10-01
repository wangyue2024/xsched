# T3 报告：sm120 guardian 编译基线与差异审查

> **文档定位**：记录 sm120 Level-2 支持第三阶段（T3：guardian 编译基线
> + 人工审查清单 + sm86↔sm120 指令差异清单）的方法、结果与结论。
> 设计任务卡见 `docs/sm120-level2-design.md` §4 T3。
>
> 日期：2026-10-01 · 状态：**T3 完成**（全部审查项通过，无阻塞项）

---

## 1. 目标与产出

| 设计要求的审查项 | 结果 |
|---|---|
| 函数体完整保留（`--keep-device-functions`） | ✅ 12/12 函数在场 |
| 是否引入 STL/LDL（L2 路径局部内存依赖） | ✅ check_preempt/restore_exec **0 条** |
| 寄存器用量 → `RequiredRegs()` | ✅ L2 路径 max R11（guardian 裁剪后）/ R21（resume） |
| barrier 数 → `RequiredBarriers()` | ✅ L2 路径各 1（BAR.SYNC.DEFER_BLOCKING） |
| LDC 是否携带 cache-policy UR | ✅ 无（`LDC Rn, c[0x0][off]` 直读形式） |
| brkpt 占位编码形态 | ✅ `BPT.TRAP 0x1` = `0x000000040000795c/0x000fea0000300000`（16B，与 sm86 逐字节一致） |
| sm86↔sm120 指令差异清单 | ✅ §5（13 项） |

**产出文件**：
- 编译基线 `platforms/cuda/hal/inject/inject_120.{cubin,asm,_cc.asm,json}`
  （由 T2 的现代化 Makefile 生成，nvcc 12.9.41 / ptxas 12.9.41）；
- 审查数据 `evidence/review_inject_120.md`（本报告表 1）；
  对照：`review_inject_86_new.md`（CUDA 12.9 重编译的 sm86）、
  `review_inject_sm86_official.md`（当年 CUDA 11 + 人工编辑的官方版）；
- 本报告（含差异清单，§5）。

## 2. 方法与命令

```powershell
cd platforms/cuda/hal/inject
make_msvc.bat ARCH=120 bin dump cc        # inject_120.{cubin,asm,_cc.asm}
python tools/instrument/extract_sass.py review --asm inject_120.asm --out review.md
```

`review` 逐函数统计：指令数 / 寄存器集合与最大号 / BSSY / BSYNC / BRA /
BREAK / BAR（`\bBAR\.`，排除 MEMBAR/ERRBAR）/ LDC / STL / LDL /
LD.ST.E / BPT / NOP / EXIT / RET.ABS。

另以 `nvdisasm -json` 导出结构化反汇编（`inject_120.json`）作为
T4 工具的辅助输入格式验证（工具最终直接解析 cubin ELF，见 T4 报告）。

## 3. 审查结果（表 1：sm120 函数普查）

数据来源 `evidence/review_inject_120.md`（完整表格入档）：

| function | instr | maxreg | BSSY | BSYNC | BRA | BREAK | BAR | LDC | STL | LDL | LD/ST.E | BPT | NOP | EXIT | RET.ABS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| check_preempt | 64 | 20 | 3 | 3 | 6 | 1 | 1 | 2 | **0** | **0** | 11 | 4 | 13 | 1 | 1 |
| restore_exec | 32 | 20 | 1 | 1 | 1 | 0 | 1 | 2 | **0** | **0** | 2 | 4 | 9 | 1 | 1 |
| check_preempt_trap | 56 | 20 | 2 | 2 | 5 | 0 | 0 | 2 | 0 | 0 | 8 | 5 | 12 | 2 | 1 |
| exit_if_idempotent | 40 | 20 | 1 | 1 | 2 | 0 | 0 | 2 | 0 | 0 | 4 | 11 | 8 | 1 | 1 |

要点解读（结合 §4 契约编号，见设计文档 S0）：

- **K4 寄存器契约**：物理最大寄存器号 20（= `RET.ABS.NODEC R20` 的返回
  地址寄存器）。**guardian 数组裁剪掉函数尾声 RET 后，实际寄存器上限
  = R11**（与 sm86 官方数组相同）；resume 保留 RET 出口，替换
  `LDC R20/R21` 后上限 = R21（与 sm86 官方一致）。32 保底值继续成立
  （32 > 21），`RequiredRegs` 维持 32。
- **K5 barrier 契约**：check_preempt / restore_exec 各含 1 条
  `BAR.SYNC.DEFER_BLOCKING 0x0` → `RequiredBarriers = 1`（与 sm86 相同）。
- **K6 局部内存契约**：L2 路径 STL/LDL = 0 → 无局部内存依赖
  （Blackwell 栈机制变化免疫），与 sm86 的刻意设计一致。
- **1 个新指令形态**：`check_preempt` 的 `@P0 BREAK.RELIABLE B2`
  （sm86 为 `@P0 BREAK`）为 Blackwell 新变体，属正常编译产物，仅
  在数组生成时原样保留（工具不触碰非占位指令）。
- 每条函数均为 16 字节对齐、无 `..........` 截断异常；`.text.*`
  section 尺寸 = 显示指令数 × 16 + NOP 填充（如 check_preempt
  0x400 = 64 条）。

## 4. L2 路径契约核对（k4/k5/k6 速览）

| 契约 | sm86 官方数组 | sm120 本产物 | 结论 |
|---|---|---|---|
| 无 STL/LDL | 0 / 0 | 0 / 0 | ✅ 直接沿用 |
| BAR.SYNC 存在 | 1（`BAR.SYNC 0x0`） | 1（`BAR.SYNC.DEFER_BLOCKING 0x0`，同 0x7b1d 主码） | ✅ 保底 1 |
| 寄存器上限 | R21（resume）/ R11（guardian） | R21（resume）/ R11（guardian 裁剪后） | ✅ 保底 32 |
| 占位编码 | `0x000000040000795c/0x000fea0000300000` | **逐字节相同** | ✅ 替换协议不变 |
| BSSY/BSYNC 配对 | 收口于 BSYNC | 收口于 `BSYNC.RECONVERGENT B0` | ✅ 裁剪规则见 T4 |

### 4.1 尾部形态差异（对数组生成的直接影响）

新工具链下函数尾部多出 **函数尾声**（老编译器/人工链中不存在）：

```
sm120 check_preempt 尾部：                sm86 官方数组尾部（CUDA 11）：
0x300 @P0 EXIT                            0x320 @!P0 BRA 0x340
0x310 BSYNC.RECONVERGENT B0               0x330 EXIT
0x320 RET.ABS.NODEC R20 0x0   ← 尾声      0x340 BSYNC B0   ← 数组就此结束
0x330 BRA 0x330               ← 自旋
0x340..0x3F0 NOP ×12          ← 填充
```

守卫数组必须“BSYNC 收口 → 物理 fallthrough 进入 kernel”，因此
`extract_sass.py` 的 guardian 裁剪规则会剥掉尾部
`NOP* → 自旋 BRA → RET.ABS.NODEC R20`（sm120 实测 -14 条），并断言
新尾部为 BSYNC；resume（跳板型）保留 RET 出口不在裁剪范围内
（两形态判定见 `tools/instrument/README.md` §3.3）。

## 5. sm86 ↔ sm120 指令差异清单（核心产出）

对照三份 asm（官方 CUDA 11 人工版 `inject_sm86.asm`、CUDA 12.9 重编译
`inject_86.asm`、`inject_120.asm`），按影响分级：

### 5.1 语义等价的新编码/新变体（保留即可，不影响替换）

| # | 指令 | sm86（新编译） | sm120 | 备注 |
|---|---|---|---|---|
| D1 | 双字自旋 BRA | `0xfffffff000007947` | `0xfffffffc00fc7947` | 编码不同、语义同（跳自身）；我们只裁剪不解析 |
| D2 | BSSY 控制码 | `0x000fe40003800000` | `0x000fe80003800200` | w0 主码相同（0x…7945） |
| D3 | BSYNC 控制码 | `0x000fea0003800000` | `0x000fea0003800200` | 收口断言只看 w0 低 16 位 0x7941 |
| D4 | BAR.SYNC | `BAR.SYNC.DEFER_BLOCKING` | 同 | 主码 0x7b1d 一致（官方老阵列是 `BAR.SYNC`） |
| D5 | WARPSYNC | `0xffffffff00007948` | `0x0000000000007948`（`.ALL`） | w1 不同 |
| D6 | 退出布局 | `@P0 EXIT`（新）/ `@!P0 BRA+EXIT`（老） | `@P0 EXIT` | 调度差异 |

### 5.2 影响语义映射的差异（工具须处理）

| # | 主题 | sm86 | sm120 | 处理 |
|---|---|---|---|---|
| D7 | **MOV 常量** | `IMAD.MOV.U32 R3,RZ,RZ,0x1` = `0x00000001ff037424` | `HFMA2 R3,-RZ,RZ,0,5.96e-8` = `0x00000001ff037431` | 仅自然产物；源规格中 `@P0 MOV` 保持官方模板（trap 路径，见 README §4） |
| D8 | **gridDim 读取** | `IMAD R0,R0,c[0x0][0x10],R3` | `LDC R3,c[0x0][0x374/378]` + `IMAD` | 自然产物变化；**说明 0x374/0x378 是 sm120 的 gridDim.y/z 槽**（T1 全扫的旁证） |
| D9 | 64 位谓词比较 | `ISETP.NE.U32.AND` + `ISETP.NE.AND.EX`（2 条） | `ISETP.NE.S64.AND`（1 条） | 指令数减少 1（调度差异） |
| D10 | **调试窗口** | `c[0x0][0x1880..0x1898]` | `c[0x0][0x170..0x188]`（T1 实证） | `--offset-map sm120` 自动重映射 |
| D11 | 谓词化 ST | 无谓词 `ST.E [R8],RZ` | 编译器生成 `@!P0 ST.E [R8],RZ` | 调度差异 |

### 5.3 trap 路径专属差异（记录，T5+ 范围）

| # | 主题 | 说明 |
|---|---|---|
| D12 | `CGAERRBAR` 新指令 | sm120 `check_preempt_trap` 出现 `CGAERRBAR (0x…75ab)`，配合 `MEMBAR.ALL.CTA`（老 sm86 为 `MEMBAR.SC.GPU` + `ERRBAR`）。trap 数组（阶段三）生成时须核对。 |
| D13 | `CCTL.IVALL` 控制码 | 主码 `0x00000000ff00798f` 不变，w1 由 `0x000fca0002000000` 变 `0x000fe20002000000`（调度）。 |

### 5.4 对替换引擎的净影响（结论）

- **L2 主路径需要生成的全部指令 = LDC（窗口读）**，其编码公式在 sm120
  上由 12/12 条 ptxas 自然样本自动交叉验证
  （`evidence/crosscheck_sm120_natural_ldc.txt`）；窗口偏移经
  `--offset-map sm120` 完成 `0x1880→0x170` 系列重映射。
- 其余差异全部是“自然产物”的形态差异（调度/寄存器分配/新变体），
  **不进入** 1:1 替换的编辑点，逐字节原样保留即可。
- trap 路径新增指令（CGAERRBAR 等）与 HFMA2 变体不影响阶段二。

## 6. 验收与结论

- T3 全部审查项通过；**无阻塞项**：L2 路径可依赖的编译产物特征
  （占位编码/函数结构/收口/无局部内存/寄存器上限/barrier）与 sm86
  一一对应，唯一定位参数（窗口偏移）已由 T1 实证并工具化。
- 基线哈希（`evidence/SHA256SUMS_T3T4.txt`）：`inject_120.cubin`
  等全部产物在册。

**对 T4 的输入**：`--offset-map sm120 = {0x1880→0x170, 0x1884→0x174,
0x1888→0x178, 0x188c→0x17c, 0x1890→0x180, 0x1894→0x184, 0x1898→0x188}`；
guardian 裁剪规则（NOP* → 自旋 BRA → RET.ABS.NODEC）；
resume 保留 RET 出口；编译产物元数据（nvcc 12.9.41、SM120 SchemaVersion
12.8、tki_toolOptions `-arch sm_120 -m 64 -astoolspatch`，见
`inject_120.json` 头部）。

## 7. 证据索引

| 内容 | 位置 |
|---|---|
| sm120 审查表 | `evidence/review_inject_120.md` |
| sm86 新编译对照表 | `evidence/review_inject_86_new.md` |
| sm86 官方（CUDA 11）对照表 | `evidence/review_inject_sm86_official.md` |
| sm120 编译基线原件 | `platforms/cuda/hal/inject/inject_120.{cubin,asm,_cc.asm,json}` |
| 自然 LDC 公式交叉验证 | `evidence/crosscheck_sm120_natural_ldc.txt`（12/12） |
| 基线哈希封存 | `evidence/SHA256SUMS_T3T4.txt` |
