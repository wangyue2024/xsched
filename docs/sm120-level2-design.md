# XSched Level-2（Guardian 插桩）sm120 支持设计文档

> 版本：v1.0
> 范围：仅覆盖 Level-2 抢占（kernel 入口检查点 + Deactivate/Reactivate）在 sm120（Blackwell, CC 12.0）上的完整实现
> 前置文档：`docs/sm120-support-design.md`（总体路线图，本文档是其阶段二的展开）

---

## 1. 机制考古：sm86 的 Guardian 指令到底是怎么来的

在设计 sm120 方案前，必须先精确还原 sm70/86 guardian 的真实生成流程。代码考古结论（证据链见 §1.3）：

### 1.1 生成流水线（sm86 实际做法）

```
inject.cu (CUDA C++ device 函数: check_preempt / restore_exec / check_preempt_trap / exit_if_idempotent)
    │
    ▼  nvcc -cubin --keep-device-functions -Xptxas -astoolspatch -arch=sm_86
    │     (Makefile @ platforms/cuda/hal/inject/Makefile)
inject.cubin
    │
    ▼  cuobjdump -sass
inject_sm86.asm   ← 人工审阅文件（406 行）
    │
    ▼  人工处理（两类手工操作，均为“等长原地替换”，不改变函数长度）
    │   ① 把 brkpt 占位（源码 nop() = "brkpt" → SASS NOP 0x0000000000007918）
    │      1:1 原地替换为 LDC 指令，读取 debugger params 窗口 c[0x0][0x1880~0x1898]
    │      （64 位指针 = 两个坑位 = 两条 LDC。“坑位数 = 指令数”是刻意的
    │        设计约束——占位符机制的本质就是把“插入”转化为安全的“替换”）
    │   ② 改写 LDC 编码内部的 cbuf offset 立即数：-astoolspatch 下参数自然槽位
    │      从 0x1888 起，与信箱固定槽位（buf@0x1880 / 跳转目标@0x1888 /
    │      kernel_idx@0x1890 / killable@0x1898）错位，需字段级改写
    │      （asm 中 "changed" 标记即此类改动；不改指令条数）
    │   ※ 因长度不变，所有相对分支（BRA/BSSY）编码无需修正——sm86.cpp
    │      最终数组与反汇编的分支编码逐字节一致即为证
    │   ※ 真正需要“修地址”的场景不在此静态流水线，而在：
    │      a) 运行时绝对跳转补丁：TarpHandler::SetJumpInstruction 把 40 位
    │         目标地址字节运行时写入 JMP 模板（inject.cu 的 jmp_to_target /
    │         trampoline / relocate_func_call 即为此模板而编译）
    │      b) trap_inject_instrs 的头尾裁剪（sm86.cpp 中被注释的行）：裁剪点
    │         恰好不落在任何“分支→目标”区间内，距离才得以保持——人工核对
    │         性质，自动化工具必须校验
    ▼
arch/sm86.cpp     ← 裸 uint64_t 指令数组（GetGuardianInstructions / GetResumeInstructions）
```

### 1.2 Guardian 运行时挂载方式（与架构无关的部分，sm120 直接复用）

| 环节 | 实现 | sm120 复用性 |
|---|---|---|
| 指令内存分配 | `InstrMemAllocator`（`cuXtraInstrMemBlockAlloc`）+ 块式增长 | API 层复用，需验证 |
| 拼接布局 | `[guardian 指令][原 kernel 指令]`，guardian 入口 = 分配地址 | 布局逻辑复用 |
| 参数注入 | `cuXtraSetDebuggerParams(func, args_buf, 28)`：preempt_buf 指针 / guardian 入口 / kernel idx / killable 四参数写入 `c[0x0][0x1880..0x1898]` 窗口 | **驱动机制，需验证** |
| 入口重定向 | `cuXtraSetEntryPoint(func, ep_guardian / ep_resume)` | **驱动机制，需验证** |
| 寄存器/barrier 适配 | `cuXtraSetLocalRegsPerThread(func, ≥32)`、`cuXtraSetBarrierCnt(func, ≥1)` | API 复用，阈值待 sm120 实测 |
| 抢占触发 | `MemsetD32Async(global_exit_flag, 1)`（1 次 D32 memset） | 完全架构无关，直接复用 |
| 恢复 | 读 `preempt_idx` → 清 header → 命令日志从该 idx 重发射，首个 kernel 走 `entry_point_resume_` | 完全复用 |

### 1.3 关键证据（来自仓库代码）

1. `inject/Makefile`：
   ```
   nvcc -cubin inject.cu -o inject.cubin --keep-device-functions -Xptxas -astoolspatch -arch=sm_${GPU_GEN_CODE}
   cuobjdump -sass inject.cubin > inject.asm
   ```
   注意 `GPU_GEN_CODE` 取自**本机 GPU** 的 compute capability —— 作者是在真实硬件上就地编译的。
2. `inject_sm86.asm` 中 `LDC R4, c[0x0][0x1880];      changed` 的 `changed` 标记 = 人工改写痕迹；源码对应位置 `nop(); // LDC R4, c[0x0][0x1880];`（`nop()` 即 `asm volatile("brkpt;")`）= **占位标记**，证明 LDC 是人工插入的。
3. `check_preempt`（L2 主 guardian）SASS 全程只用：`S2R / LDC / ISETP / BRA / BSSY / BSYNC / LOP3 / IMAD / LEA / LD.E / ST.E / MEMBAR / WARPSYNC / BAR.SYNC / EXIT`——**不使用 STL/LDL（local memory）**，作者有意规避了栈依赖。`exit_if_idempotent`（仅 trap 路径用）才用了 STL/LDL。→ **L2 路径不依赖 local memory，规避了 Blackwell 栈机制（SETLMEMBASE）变化的最大风险**。
4. `restore_exec`（resume 指令）同样纯寄存器 + global memory + `RET.ABS.NODEC R20`（跳回 `c[0x0][0x1888]` 指定的原入口）。

---

## 2. 可行性论证（逐依赖核验）

| # | 依赖项 | sm120 状态 | 证据 | 结论 |
|---|---|---|---|---|
| D1 | nvcc 支持 `-arch=sm_120` | ✅ CUDA 12.8+ | NVIDIA 论坛/CUTLASS/PyTorch cu128 均确认 | 可行 |
| D2 | `-Xptxas -astoolspatch`（`--compile-as-tools-patch`）在 CUDA 12.8+ 可用且支持 sm_120 | ✅ | NVCC 12.9 官方文档 §4.2.7.13 明确列出该选项；NVBit 生态有 `"--gpu-name=sm_120", "--compile-as-tools-patch"` 的直接用例 | 可行 |
| D3 | `--keep-device-functions` | ✅ | NVCC 文档 §4.2.7.14 | 可行 |
| D4 | nvdisasm 支持 SM120 反汇编 + JSON 输出 | ✅ | cuda-binary-utilities 文档 `-b` 参数值列表明确含 `SM120`/`SM120a`；CUDA 12.8 release notes 确认 JSON 输出 | 可行 |
| D5 | SASS 指令长度 16B/条（128-bit 含 control codes） | ✅ | NVBit sm120 实测："每条 SM120 指令为 16 字节（0x10 偏移步长）" | 可行 |
| D6 | guardian 所用核心指令（S2R/LDC/ISETP/BRA/BSSY/BSYNC/BAR.SYNC/MEMBAR/EXIT/LEA/IMAD/LD.E/ST.E）在 Blackwell 存在 | ✅（预期，需编译确认） | "Volta 之后 SASS 基础指令变化不大，Blackwell 亦是在 Volta 基础上新增"；cuda-binary-utilities 文档含 "Blackwell and Rubin Instruction Set" 一章 | 高度可行，T3 编译时自动确认 |
| D7 | cuxtra `SetDebuggerParams` 的 `c[0x0][0x1880..0x1898]` 窗口在 R570+ Blackwell 驱动上有效 | ❓ **唯一硬性未知** | cuxtra 为闭源预编译库（`3rdparty/cuxtra/lib/*.a`），无公开资料 | **必须 T1 实验先行**，预案见 §9 |
| D8 | cuxtra `SetEntryPoint / GetBinary / InstrMem*` 在 Blackwell 有效 | ❓ 同上 | 同上 | T1 实验先行 |
| D9 | Blackwell ptxas 行为差异（寄存器倾向更多、可能引入 STL/LDL、LDC 带 cache-policy UR） | ⚠️ 有差异但可控 | 论坛实测 sm_100/120 编译"255 registers, more stack"；cuasm 记录 sm8x+ LDG/LDC 隐含 `desc[UR#]` cache-policy | T3/T4 处理：限制 `-maxrregcount`、审查产物、必要时改写源码 |

**结论**：除 D7/D8（cuxtra 黑盒，占比两项）外全部依赖有正面证据。而 D7/D8 恰好可以用一个**半天级的最小实验**提前判定，且存在两条备选路径（§9）。整体可行性评级：**高（8/10）**。

---

## 3. 任务流程总览

```
T1 cuxtra 机制探针 (gate, 2d) ────失败──> §9 预案A/B (延长 +10d)
  │ 通过
T2 源码准备与 Makefile 现代化 (1d)
  │
T3 sm120 guardian 编译 + 反汇编基线 (1d)
  │
T4 指令提取自动化工具 extract_sass.py (5d) ★核心工程资产
  │   └─ T4a 金标准回归：用工具重现 sm86 数组并 diff (1d, 含在 T4)
  │
T5 GuardianSM120 生成与人工审查 (2d)
  │
T6 最小可行实验 MVE：手工挂载 guardian 于 dummy kernel (2d)
  │
T7 运行时集成：arch.cpp 接线 + InstrumentContext 参数化 (2d)
  │
T8 全链路联调：Deactivate/Reactivate 闭环 (3d)
  │
T9 测试与性能验证 (3d)
  │
T10 文档与上游化 (1d)
─────────────────────────────────────────────
合计: 17~22 人日 (gate 失败时 +10d)
```

---

## 4. 任务卡（详细）

### T1. cuxtra 机制探针（Gate，2 人日）

**目标**：判定 D7/D8，给出 Go/No-Go。

**方法**：新建 `test/cuxtra_probe_l2_sm120.cpp`（Windows 可用，参考 `tests_local/test_cuda_driver.cpp` 的驱动 API 直调风格）：

```cpp
// 探针 P1: debugger params 窗口读写
//   1) cuModuleLoad 一个含 1-kernel 的 cubin（用 __global__ kernel）
//   2) kernel 内 inline asm 读 c[0x0][0x1880] 并存入 global buffer
//      （该 LDC 指令从 sm86 guardian 数组拷贝，编码已知）
//   3) launch 前 cuXtraSetDebuggerParams(func, &magic, 8)
//   4) 验证 buffer 中读到 magic  →  证明窗口有效
// 探针 P2: 入口重定向
//   1) cuXtraGetEntryPoint(func) 记录 ep_orig
//   2) 分配指令内存, 写入 [BRA ep_orig+16][原指令] 前缀
//   3) cuXtraSetEntryPoint(func, new_ep); launch; 验证行为 = 前缀被执行
// 探针 P3: 指令内存可执行性
//   cuXtraInstrMemBlockAlloc + cuXtraInstrMemcpyHtoD 写入 dummy kernel
//   二进制副本, 入口跳转执行, 结果比对
// 探针 P4: GetBinary / SetLocalRegsPerThread / SetBarrierCnt 读写回读
```

**判定标准**：
- P1~P3 全过 → Go，按主路径推进；
- P1 失败 → 启动预案 A（参数区尾部追加，§9.1），主路径不变但工具需增加"LDC offset 动态改写"能力（已在 T4 范围内，+2d）；
- P2/P3 失败 → 预案 B（§9.2）。

**产出**：探针二进制 + 结论文档段落（并入本文件 §9）。

---

### T2. 源码准备与 Makefile 现代化（1 人日）

**改动**：
1. `inject/Makefile` 重写，解除"本机 GPU 架构"耦合：
   ```makefile
   ARCH ?= $(shell nvidia-smi --query-gpu=compute_cap --format=csv,noheader | tr -d '.')
   bin:
   	nvcc -cubin inject.cu -o inject_$(ARCH).cubin \
   	     --keep-device-functions -Xptxas -astoolspatch -arch=sm_$(ARCH)
   dump:
   	cuobjdump -sass inject_$(ARCH).cubin > inject_$(ARCH).asm
   	nvdisasm -c -b SM$(ARCH) inject_$(ARCH).cubin > inject_$(ARCH)_cc.asm   # 含 control codes
   ```
2. `inject.cu` 审查（sm120 兼容性预检）：
   - `get_blockid()` 用 `blockIdx/gridDim` —— 编译器生成 S2R + IMAD 链，架构无关 ✅
   - `__threadfence_block()` → MEMBAR.SC.CTA ✅（Blackwell 存在）
   - `__syncthreads()` → BAR.SYNC 0x0 ✅
   - `asm("exit;")` → EXIT ✅
   - `asm volatile("brkpt;")` → 需确认 sm_120 上 brkpt 仍编译为 NOP 占位（T3 首次编译即见分晓；若 brkpt 在 Blackwell 产生了不同编码，占位符可改为其他可识别模式，如连续两个特定 NOP 变体）
3. 为 L2 所需函数（`check_preempt`、`restore_exec`）保持**不使用 local memory** 的约束写法（现状已满足，固化注释）。

**产出**：现代化 Makefile + 审查记录。

---

### T3. sm120 guardian 编译基线（1 人日）

**步骤**：
1. `make ARCH=120`（CUDA 12.8+ nvcc）→ `inject_sm120.cubin`
2. `cuobjdump -sass` + `nvdisasm -c -b SM120 -json` 双路导出
3. **人工审查清单**（产出审查报告）：
   - [ ] `check_preempt` / `restore_exec` 函数体是否完整保留（`--keep-device-functions` 生效）
   - [ ] 是否引入 STL/LDL（若引入：源码加 `__launch_bounds__` / 重排变量消除，或接受并用 P4 验证 local memory base 在入口重定向下有效）
   - [ ] 寄存器用量（`nvdisasm -plr` 或 `cuobjdump -res-usage`）→ 确定 sm120 的 `RequiredRegs()`
   - [ ] barrier 数 → 确定 `RequiredBarriers()`
   - [ ] LDC 是否携带 cache-policy UR（对照 cuasm 记录的 sm8x+ 隐式 UR 问题；Blackwell 上 nvdisasm 可能显示 `LDC Rn, c[0x0][0x????]` 不变，需对比编码确认）
   - [ ] brkpt 占位的编码形态
4. 记录 sm86↔sm120 指令集差异清单（哪些 guardian 指令在 sm120 有新编码/新变体）。

**产出**：`inject_sm120.asm` 基线 + 差异审查报告。此报告直接决定 T5 人工审查的工作量。

---

### T4. 指令提取自动化工具 `extract_sass.py`（5 人日，核心资产）

**位置**：`tools/instrument/extract_sass.py`（新建目录，与 `inject/` 的关系：inject/ 是源与 Makefile，tools/instrument/ 是自动化提取）。

**输入**：cubin 文件 + 函数名清单 + 目标 LDC 偏移表
**输出**：可直接编译的 `arch/sm{N}.cpp`

**架构**：

```
cubin ──nvdisasm -json -b SM{N}──> 指令流(结构化: {addr, opcode, operands, encoding_hi, encoding_lo})
                                        │
                    ┌───────────────────┤
                    ▼                   ▼
              Pass 1: 函数切分     Pass 2: 指令改写
              (ELF symbol 表)      • brkpt 占位 → LDC 等长 1:1 替换
                                   • LDC cbuf offset 立即数改写
                    │                   │
                    ▼                   ▼
              Pass 3: 分支距离校验（兜底：重定位）
              L2 流水线为等长 1:1 替换，正常情况不产生移位
              • 校验：改写后重新解析，断言每条 BRA/BSSY 的相对距离与
                编译器原值一致，不一致即 fail（防调度差异/手滑/非 1:1 编辑）
              • 兜底：若编辑真的落在“分支→目标”区间（如阶段三 trap
                摘录裁剪），按 JSON 语义目标重算并重写编码中的偏移字段
                    │
                    ▼
              Pass 4: 校验
              • 重汇编视角自检：重新解析改写后的编码，目标地址应正确
              • 指令数/出口/入口结构断言
                    │
                    ▼
              Pass 5: 代码生成（模板渲染 arch/sm{N}.cpp）
```

**关键算法设计**：

1. **LDC offset 立即数改写（两点定标法）**——不依赖对 LDC 完整编码的逆向：
   ```
   已知: LDC R4, c[0x0][0x1880] ⇔ 0x00062000ff047b82
         LDC R6, c[0x0][0x1890] ⇔ 0x00062400ff067b82   (sm86 实测样本)
   差分: offset Δ=0x10 → 编码 Δ=0x400 (位于第 1 字 bit13..19 区段, 逐位线性)
   方法: 对 sm120 编译产物取两个不同 param offset 的 LDC 自然样本做同样差分,
         得到 sm120 的 offset→编码位映射, 再改写任意目标 offset。
   约束: 仅当映射为线性(预期)时成立; 否则退化为多位域查表(仍可自动探测)。
   ```
2. **brkpt 占位 → LDC 插入**：
   - 源码中每个 `nop(); // LDC Rn, c[0x0][0x????];` 注释即插入规格；
   - 工具用正则从 `inject.cu` 提取（寄存器号、目标偏移、参数宽度）；
   - LDC 指令模板 = 该函数内编译器自然生成的 param 读取 LDC（拷贝其编码，仅改 offset 与目标寄存器字段——寄存器字段位同样用差分法定位）。
3. **分支距离校验（兜底：重定位）**：
   - L2 的编辑均为等长 1:1 替换，理论上不产生移位，分支编码应与编译器原值一致；
   - 校验为主：改写后重解析每条 BRA/BSSY，断言相对距离不变，fail-fast；
   - 兜底重写：仅当编辑落在“分支→目标”区间时，按 JSON 语义目标重算距离，
     用差分法定位编码位并重写（Volta+ BRA 为 word 级相对偏移）；
   - **金标准回归**：对 sm_86 重复整个流水线（编译 inject.cu -arch=sm_86 → 工具全自动处理 → 生成 sm86.cpp），与仓库现有 `arch/sm86.cpp` 数组**语义 diff**（逐指令比较 opcode/操作数/分支目标，编码需在 control-code 位上做归一化比较，因为 nvcc 版本差异可能改变 control codes）。语义一致即证明工具正确。这是工具可信度的核心保障。

**产出**：`tools/instrument/extract_sass.py`（约 800~1200 行 Python）+ sm86 金标准回归报告 + 使用文档。

---

### T5. GuardianSM120 生成与人工审查（2 人日）

1. 运行 `extract_sass.py --arch 120` 生成 `platforms/cuda/hal/src/arch/sm120.cpp`（guardian + resume 指令数组）；
2. 生成 `platforms/cuda/hal/include/xsched/cuda/hal/arch/sm120.h`（仿 sm86.h）；
3. **人工逐指令审查**（与 T3 差异报告对照）：每条指令的语义与 sm86 版本一一对应，特别是 BSSY/BSYNC 对齐、MEMBAR 域（CTA 级）、EXIT 位置；
4. 从生成产物统计 `RequiredRegs() / RequiredBarriers()`（写入类的构造或接口）。

**产出**：sm120.h / sm120.cpp（reviewed）。

---

### T6. 最小可行实验 MVE（2 人日）

**目标**：在接入 XSched 框架前，独立验证"guardian 挂载 + Deactivate + Reactivate"三步在 sm120 真机上闭环。

**实验程序** `test/test_guardian_sm120.cpp`（driver API 直调，不依赖 shim/preempt 框架）：

```
1. 编译/加载一个 100ms 级 dummy kernel（如 dummy.cu 扩展版）
2. 手工执行 InstrumentContext::Instrument 的等价逻辑:
   ep_inst = [GuardianSM120 指令][dummy kernel 二进制]
3. Launch 路径: SetDebuggerParams + SetEntryPoint(ep_inst) → launch
   验证 A: 正常执行, 结果与原 kernel 一致
4. Deactivate 等价: memset global_exit_flag=1 (preempt buffer 需先按布局分配)
   验证 B: 后续 launch 的 kernel 进入即退出, 提交端 cuStreamSync 立即返回,
            preempt_idx 被正确写入
5. Reactivate 等价: 清 header, 用 ResumeInstructions 入口重发被退出的 kernel
   验证 C: 结果 bit-exact
6. 循环 1000 次 A→B→C, 无 GPU fault / 无 Xid 错误
```

**这是整个 L2-sm120 的 go-live 判据**。MVE 通过后，框架集成只是工程量问题。

---

### T7. 运行时集成（2 人日）

**文件级改动清单**：

| 文件 | 改动 |
|---|---|
| `hal/src/arch/arch.cpp` | `Guardian::Instance()` 加 `case 120: return make_shared<GuardianSM120>()`；`CudaQueueCreate()` 的 trap 分派表加 `case 120:`（阶段三前映射 `CudaQueueLv2`；TSG env 选项优先级在前，保持） |
| `hal/include/.../arch/sm120.h` + `src/arch/sm120.cpp` | T5 产物 |
| `hal/include/.../level2/guardian.h` | `Guardian` 基类增加 `virtual size_t RequiredRegs() { return 32; }` / `virtual size_t RequiredBarriers() { return 1; }`（sm120 覆写实测值） |
| `hal/src/level2/instrument.cpp` | ① `Instrument()` 中硬编码的 `32`/`1` 改为 `guardian_->RequiredRegs()/RequiredBarriers()`；② `InstrumentContext` 构造中 resume 指令加载不变（调 `GuardianSM120::GetResumeInstructions` 自动走对） |
| `hal/include/.../common/options.h/.cpp` | （若总设计中 Auto 策略已实现）确认 120 默认 L3=TSG、L2 始终可用；否则本任务只加 arch 表，不动 options |
| `platforms/cuda/CMakeLists.txt` | `sm120.cpp` 加入编译清单（确认现有按 arch 源文件组织方式） |
| `protocol/def.h` / README | env 说明与支持矩阵更新（L2 ✅） |

**注意**：`CudaQueueLv3Trap` 在 arch 表没有 `case 120` 时会 fallback——必须确保 sm120 不会在 Trap 模式下创建 `CudaQueueLv3Trap`（无 TarpHandlerSM120 会 nullptr 崩溃）。集成时在 `CudaQueueCreate` 显式断言。

---

### T8. 全链路联调（3 人日）

在 XSched 完整框架（shim + preempt + sched + xserver）下联调：

1. `make PLATFORM=cuda`（Windows 下用 `make.bat`）构建；
2. 跑通 `examples/Windows/1_transparent_sched`（透明调度，默认级别）；
3. 用 `xcli` / env（`XSCHED_AUTO_XQUEUE_LEVEL_ENV_NAME=2`）强制 Level-2；
4. 验证点：
   - kernel 提交流（含 memcpy/memset 混合）在 L2 下的 `LaunchWorker` 一致性约束正确触发（对照 `launch_worker.cpp` 的 deactivatable 等待逻辑）；
   - `XQueueSuspend(kQueueSuspendFlagSyncHwQueue)` + Resume 多轮稳定；
   - `XQueueSetPreemptLevel` 在线切换 1↔2 正常（`OnPreemptLevelChange` → `InstrumentManager` 惰性插桩路径）。

---

### T9. 测试与性能验证（3 人日）

| 测试 | 方法 | 通过标准 |
|---|---|---|
| 正确性 | 双进程 HPF（复用 `test/run_threshold_test.ps1` 框架）：低优提交 1000×50ms kernel 流，高优周期性插入，反复 Deactivate/Reactivate | 最终结果与无抢占基线 bit-exact |
| 抢占延迟 | 测量 `Suspend()` 调用 → 队列真正停转（命令完成事件）的时间分布 | ≈ 当前在飞 kernel 剩余时长（L2 语义为 kernel 边界退出），显著小于 L1 的"等整条队列"？——注意 L1 与 L2 在单 kernel 粒度上差异体现为：L2 可让**在飞 kernel 在入口检查点退出**（新 launch 的 kernel），L1 只能等它跑完。构造"Deactivate 后新 kernel 不被执行"用例验证 |
| 恢复开销 | Reactivate → 首 kernel 完成的时间 | 与 sm86 基线同数量级 |
| 插桩开销 | 插桩 kernel vs 原始 kernel 的 launch+执行开销差 | launch 开销增量 < 2×基线（guardian ~119 条指令前缀） |
| 稳定性 | 8 小时循环抢占 + CUDA Graph 工作负载混合（graph 走 L1 回退路径，验证共存） | 无 fault、无泄漏（`InstrMemAllocator` 增长曲线合理） |
| 回归 | sm86 全套既有测试 | 全过 |

**产出**：测试报告（含 sm86 对照数据）。

---

### T10. 文档与上游化（1 人日）

- `platforms/cuda/README.md`：sm120 支持状态、env 配置；
- `README.md` XPU 矩阵更新；
- `tools/instrument/README.md`：工具链使用与再生成流程（新架构适配 SOP）；
- PR 描述中附 T3 差异报告摘要。

---

## 5. 工具链详细设计补充（T4 的关键决策记录）

**为什么选择"编译 + 反汇编 + 程序化改写"而不是手写 SASS 汇编（CuAssembler 路线）**：
- CuAssembler 不支持 Blackwell（当前 Pascal~Ampere），且引入外部重依赖；
- nvcc + astoolspatch 是 NVBit 验证过的、官方文档化的生成路径，产物天然带正确 control codes；
- 我们需要的“手工操作”只有 2 类（LDC 等长替换 / offset 立即数改写），外加一个分支距离校验 Pass（保险丝），全部可程序化；
- nvdisasm `-json` 提供结构化指令 + 编码双输出，改写后可自校验。

**Control codes 风险**：人工插入 LDC 时，其 control code（第二个 u64，编码记分板/屏障依赖）从模板拷贝——模板来自同一函数的编译器产物，依赖模式与上下文一致，风险低。金标准回归覆盖此点。

**nvcc 版本锁定**：`arch/sm{N}.cpp` 数组是构建期产物（离线生成、仓库固化），运行时不再编译。建议在生成文件头注释记录 nvcc/CUDA 版本，升级 CUDA 后需重新生成并跑金标准回归。

---

## 6. 明确不做（Out of Scope）

- Level-3 trap 注入（`TarpHandlerSM120`）——见总设计阶段三；
- CUDA Graph 内部 kernel 的 L2 插桩（继续走 L1 回退，升级路径单列）；
- Windows 平台的 L2 启用（`arch.cpp` 的 `#if defined(_WIN32)` 短路保持；但注意 T1/T6 探针**可以在 Windows 上跑**，提前积累数据，为后续解锁铺路——MVE 程序用 driver API 直调，不经过 arch.cpp 分派）。

---

## 7. 工作量汇总

| 任务 | 人日 | 累计 | 可并行性 |
|---|---|---|---|
| T1 cuxtra 探针（gate） | 2 | 2 | 阻塞后续 |
| T2 源码/Makefile | 1 | 3 | 与 T1 部分并行 |
| T3 编译基线 | 1 | 4 | 串行 |
| T4 extract_sass.py（含金标准回归） | 5 | 9 | T4a 回归可与 T5 并行 |
| T5 GuardianSM120 生成审查 | 2 | 11 | |
| T6 MVE | 2 | 13 | 依赖 T1+T5 |
| T7 集成 | 2 | 15 | |
| T8 联调 | 3 | 18 | |
| T9 测试 | 3 | 21 | T8/T9 部分重叠可压缩 |
| T10 文档 | 1 | 22 | |
| **合计** | **17~22 人日** | | 单人 4~5 周日历时间 |

若 T1 gate 失败走预案 A：+10 人日（工具增加运行时 LDC 改写 + 参数布局协商），总计 27~32 人日。

---

## 8. 风险矩阵

| 风险 | 概率 | 影响 | 缓解 | 触发任务 |
|---|---|---|---|---|
| `c[0x0][0x1880]` debugger params 窗口在 Blackwell 失效 | 中 | 高（L2 主路径变） | 预案 A：参数区尾部追加 + 运行时 LDC 改写（工具能力已在 T4 建成） | T1-P1 |
| `SetEntryPoint` 在 Blackwell 失效 | 低 | 高 | 预案 B：入口 patch（在原 kernel 首指令位置写跳转，需指令内存可写原 kernel 区域——`InstrMemAllocator` 已支持 FtoH/HtoF 拷贝） | T1-P2 |
| Blackwell guardian 产物带 STL/LDL | 中低 | 中 | 源码改写消除（`__launch_bounds__`/寄存器化）；最坏接受并在 MVE 验证 local memory base | T3 |
| brkpt 占位在 sm120 编码不同 | 低 | 低 | 换占位模式（特定 NOP 序列） | T3 |
| 分支重定位算法位域在 Blackwell 非线性 | 低 | 中 | 多点探测查表（自动）；金标准回归兜底 | T4 |
| ptxas 对小函数激进内联导致函数体消失 | 低 | 低 | `--keep-device-functions` + `__noinline__` 已有；T3 审查确认 | T3 |
| 驱动版本更新导致窗口/行为漂移 | 低（长期） | 中 | 启动时探针自检（P1 快速版进 `InstrumentContext` 构造，失败则 XWARN + 降级 L1） | T7 |

---

## 9. 预案

### 9.1 预案 A：参数区尾部追加（替代 debugger params 窗口）

若 P1 失败：利用 `cuXtraGetParamCount/GetParamInfo` 获取原 kernel 参数布局，将 XSched 4 参数（28B）**追加到原 kernel 参数 buffer 尾部**（发射时 `LaunchWrapper` 的 params 指向扩充后的 buffer），guardian 中的 LDC offset 改为"原参数末尾偏移"（因 kernel 而异 → **运行时改写 LDC 立即数**，插桩时对指令内存中的 guardian 副本做一次二进制 patch——T4 的 offset 改写算法直接复用于运行时）。
代价：+10 人日；收益：**摆脱对驱动调试窗口的依赖，长期更稳健**。若 T1 结论为"窗口可用但脆弱"，可与主路径并行实施为可选模式。

### 9.2 预案 B：入口原地 patch（替代 SetEntryPoint）

若 P2 失败：不重定向入口，而是将原 kernel 二进制首部若干条指令**复制到 stub**，并在原位置写入 `BRA guardian`（guardian 尾部跳回 stub 执行被覆盖指令后 `BRA` 回原 kernel+patch 长度处）。需要指令内存中可执行"被覆盖指令副本"，`cuXtraInstrMemcpyFtoH/HtoF` 已提供能力。
代价：+8 人日；风险：被覆盖首指令含分支/入口序言时需特判。

---

## 10. 验收标准（Level-2 专项）

- [ ] T1 探针结论归档（Go / 预案A / 预案B）；
- [ ] `tools/instrument/extract_sass.py` 对 sm86 生成结果通过金标准语义 diff；
- [ ] MVE（T6）1000 轮 Deactivate/Reactivate bit-exact 无 fault；
- [ ] sm120 完整框架下 Level-2 透明调度可用（`xcli` 可见队列级别为 2）；
- [ ] 混合命令流（kernel/memcpy/memset）正确性测试通过；
- [ ] 插桩与抢占开销报告（对照 sm86）产出；
- [ ] sm86/sm70 既有测试全量回归通过；
- [ ] 新架构适配 SOP 文档（`tools/instrument/README.md`）就绪——下一个架构（sm130 等）预期 1 周内可完成适配。

---

## 附录：与总设计文档的衔接

本文档对应 `docs/sm120-support-design.md` 的"阶段二"，差异与增强：
1. 修正了对 guardian 生成流程的认知——原以为可纯 nvcc 产出，考古发现存在 **brkpt 占位 + 人工 LDC 等长替换 + offset 立即数改写** 两类手工操作（均为等长原地替换，分支编码从未被人工修正，数组与反汇编逐字节一致即为证）；工具链（T4）据此设计，Pass 3 定位为校验为主、重写为兜底；
2. 新增**金标准回归**机制（用 sm86 现有数组验证工具），大幅提升自动化可信度；
3. 新增 **MVE（最小可行实验）** 作为独立 gate，把"框架集成"与"机制验证"解耦，风险前置；
4. 任务粒度从"阶段"细化到 10 张任务卡，总量评估 17~22 人日（原估 15~25，一致）。
