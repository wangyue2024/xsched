# T7 报告：sm120 Level-2 运行时接入、集成测试与环境边界调查

> **文档定位**：记录 sm120 Level-2 抢占的**最终运行时接入**（代码改动、
> 原理、兼容性）、**框架级集成测试**（DLL-proxy 全栈、26 项检查
> 三轮全绿）以及接入过程发现的 **"高外部 GPU 负载下 MVE 工具缺块"**
> 环境边界现象的完整调查与处置。
>
> 日期：2026-10-01 · 状态：**T7 接入完成**（集成测试全绿；MVE 全量
> 验收需在空闲 GPU 上执行——环境前置条件见 §6）

---

## 1. 结论摘要

1. **接入完成**：`GuardianSM120` 注册、Windows 平台 sm120 → `CudaQueueLv2`
   解锁、资源保底参数化（`RequiredRegs/RequiredBarriers`），共 5 个文件
   改动（§2）。
2. **集成测试全绿**：在完整产品路径（cudart → nvcuda.dll 代理 →
   XQueue(L2) → instrument → guardian splice）上 26 项检查 **×3 轮
   全 PASS**——其中一轮正是**外部 GPU 99% 负载**下运行（§4）。
3. **环境边界现象**：测试工具 `mve_main.exe` 在"外部 GPU 99% 独占 +
   2048 块 ×50ms 重负载"时会缺块；**证据链证明与 XSched/L2 机制无关**
   （instrument 之前的纯 launch 也复现）；已内置 **preflight 环境
   检查**并把"独占 GPU"定为测试前置条件（§6）。

## 2. 接入改动清单（全部实际落盘）

| # | 文件 | 改动 | 原理 |
|---|---|---|---|
| 1 | `hal/include/.../level2/guardian.h` | 基类新增 `virtual size_t RequiredRegs(){return 32;}` / `RequiredBarriers(){return 1;}` | 把原先硬编码在 `Instrument()` 里的资源保底值参数化；默认值 = 历史行为（对 sm35/70/86 零变化） |
| 2 | `hal/include/.../arch/sm120.h` | `GuardianSM120` 覆写 `RequiredRegs=32 / RequiredBarriers=1`（T5 实测：guardian R11 / resume R21，各 1 条 BAR.SYNC） | 显式声明 sm120 的硬件实测值 |
| 3 | `hal/src/level2/instrument.cpp` | `if (reg < 32) Set...(32)` → `if (reg < guardian_->RequiredRegs()) Set...(...)`（barrier 同理） | 消除硬编码，未来架构可按需覆盖 |
| 4 | `hal/src/arch/arch.cpp` | ① `Guardian::Instance()` 加 `case 120 → GuardianSM120`；② `CudaQueueCreate()`：**Windows 短路改为"sm120 → Lv2，其余保持 Lv1"**；非 Windows 的 switch 加 `case 120 → CudaQueueLv2`（明确**不落 Lv3Trap**）；③ `DirectLaunch()` 同步处理 case 120 → `CudaQueueLv2::DirectLaunch` | 见 §3 |
| 5 | `platforms/cuda/hal/src/arch/sm120.cpp`（T5 产物） | 已由 GLOB 收编进 halcuda；本次随全库重编译验证 | — |

**构建产物**：`halcuda` 静态库、`nvcuda.dll`（shim）重新构建并 install
到 `output/`（2026-10-01 16:52），集成测试直接链接 `output/lib/nvcuda.lib`。

## 3. Windows 解锁的原理与安全边界

### 3.1 原状
```cpp
std::shared_ptr<HwQueue> CudaQueueCreate(CUstream stream) {
#if defined(_WIN32)
    return std::make_shared<CudaQueueLv1>(stream);   // 一切降级 L1
#endif
    ...
```

### 3.2 为何此前必须短路 / 为何现在可以解锁
- **此前**：L2 依赖 cuxtra 的驱动调试窗口与入口重定向；trap（L3）依赖
  `cuXtraGetTrapHandlerInfo`——该调用在 Windows 标准驱动上直接中止
  （T1-P6，`cuda error 101`）。sm70/86 的 arch 表默认走 Lv3Trap，
  在 Windows 上不可用，因此整体降级。
- **现在**：T1 实证 cuxtra 的 L2 全族 API 在 Windows + 标准驱动上工作；
  T6 在真机完成三步闭环；因此把 **sm120（唯一有 L2 数组且无 trap 需求的
  目标架构）** 放行到 `CudaQueueLv2`。

### 3.3 改动后的分派（Windows）
```
arch == 120  → CudaQueueLv2        （L2 可用；不下落到 Lv3Trap——sm120 无 TarpHandlerSM120）
arch != 120  → CudaQueueLv1        （维持历史行为，零风险）
```
**关键防护**：`case 120` 在 Windows 分支**直接返回 Lv2，不进入 Tsg/Trap
判断**——彻底避免"无 TarpHandlerSM120 时创建 CudaQueueLv3Trap 崩溃"
（设计文档 T7 的显式断言要求）。

### 3.4 `DirectLaunch` 对应处理
未托管流（无 XQueue）上的 kernel 走 `DirectLaunch`。sm120 上改为
`CudaQueueLv2::DirectLaunch`——即 **Original 模式**：先把空的 preempt
buffer 写入调试窗口、再以原入口发射。**意义**：一个已被插桩的
`CUfunction` 被未托管流发射时，guardian 仍会读窗口——Original 模式
保证窗口内容合法（flag=0 → 直接放行），避免读脏数据（I5 用例验证）。

### 3.5 非 Windows 路径与旧架构的兼容性
- 非 Windows：`case 120` 追加在既有 switch 中，**其余 case 与 TSG
  分支逐字未动**；
- sm35/70/86：`RequiredRegs/Barriers` 走基类默认值 = 历史硬编码值，
  行为不变；
- `guardian.h` 新增虚函数：halcuda 为静态库全量重编，无 ABI 残留。

## 4. 集成测试（app_l2.cu）

### 4.1 架构（与真实用户应用一致）
```
app_l2.exe ──链接── output/lib/nvcuda.lib（shim 导入库）
     │                  │
     │ cudart 的 cu* 调用 │ XSched 管理 API（CudaQueueCreate/XQueue*）
     ▼                  ▼
   nvcuda.dll（shim，exe 同目录，DLL 代理）──内部── halcuda(preempt 状态与此同一份)
     │ 经 XSCHED_CUDA_LIB / CUXTRA_CUDA_LIB 转发真驱动
     ▼
   C:\Windows\System32\nvcuda.dll（真驱动，未被替换，系统零改动）
```

### 4.2 用例与结果（26 项检查；run1/run2 常规、run3 于 99% 干扰下）
| 用例 | 验证 | 结果 |
|---|---|---|
| I1 功能 | L2 队列 32 kernel 无抢占 → bit-exact | PASS ×3 |
| I2 停转 | 长流中途 Suspend：部分完成/部分被拦（0<done<N）、冻结期零进展 | PASS ×3 |
| I3 循环 | 20 轮 Suspend/Resume 收敛 | PASS ×3 |
| I4 混合 | kernel+memcpyAsync+memsetAsync 跨停转 → 数据 pattern 完整 | PASS ×3 |
| I5 未托管流 | 无 XQueue 的流并存正常（DirectLaunch/Original 路径） | PASS ×3 |
| I6 多队列 | 两 L2 队列：Suspend q1 时 q2 正常流转；恢复 exactly-once | PASS ×3 |
| — | RESULT / exit | **0 failed ×3 / exit=0** |

日志：`evidence/app_l2_{run1,run2,t7_under_load}.log`（UTF-8）。

### 4.3 过程中修复的问题
1. **MU 编码**：cudart 链路的英中混编头文件在 GBK 代码页下解析错乱
   → 编译加 `-Xcompiler "/utf-8"`；
2. **XSched 日志走 stderr** 触发 PowerShell NativeCommandError
   → run.ps1 临时放宽 EAP 捕获；
3. **DEBG 级日志刷屏**（每调用两行）→ `XLOG_LEVEL=INFO`；
4. **校准失效**：CUDA event 经 XQueue 排队后无干净时间戳 → 改为
   wall-clock 校准；**LCG 忙链被 DCE 消除**（0.28ms/10M 迭代）
   → 结果落盘到独立 sink buffer（MVE 同款手法）。

## 5. 证据链：接入未破坏机制层

| 回归项 | 结果 |
|---|---|
| T5 数组结构审查（t5_verify.py，21 断言） | PASS（数组未变） |
| T6 MVE（空闲 GPU 时段 ×5，45/45） | 全绿（`mve_full_run{1,2,3}`、`stability{3,4}`） |
| 全库构建（halcuda/preempt/nvcuda，`-Werror`） | 通过 |

## 6. 环境边界现象：高外部负载下 MVE 缺块（完整调查记录）

### 6.1 现象
在**外部游戏进程占 GPU 99%（显存 89%）**期间，`mve_main.exe` 的
**任何** 2048 块 ×50ms 重负载 launch 出现"大区间块未完成 + sync
提前返回"（完成数 250~290/2048，缺块集中在中间区间；M0 纯 launch
与 instrument 前 preflight 均中招）。

### 6.2 排除性证据（逐项）
| 实验 | 结论 |
|---|---|
| 纯 CUDA 探针（`pure_cuda_check.cu`）在同负载下 ×6 | 全绿（无 XSched） |
| driver 二分探针（`mve_m0_repro.cu` P0-P4：plain→regs→cache→instrument→guarded，同负载下 30+ 轮） | 全绿 |
| preflight 置于 instrument **之前** | 同样缺块 → **与 cuxtra/L2/instrument 无关** |
| **完整产品路径 `app_l2.exe`（shim+XQueue+guardian）同负载 ×3** | **26/26 全绿** |
| 空闲时段 MVE 全量 ×5 | 全绿 |

### 6.3 结论与处置
- 现象仅与 **"特定测试可执行体 × 极限外部负载"** 的组合相关（两对照
  工具差异在预算内未穷尽，特征符合 WDDM 在显存/算力极端竞争下的
  驱动行为异常）；**由于 preflight（无守护、无 cuxtra 状态）同现**，
  不构成对 L2 机制正确性的任何证据。
- **处置**：`mve_main.exe` 内置 `--preflight` 环境自检（4 轮纯 launch
  完整性探测，默认由 run_mve.ps1 启用）；检出污染时打印
  `ENVIRONMENT NOT CLEAN` 警告；**"独占 GPU"列为 MVE 测试前置条件**。
- 调查全量日志：`evidence/mve_{stability5,diag1..5,preflight1,2}.log`、
  `../sm120_mve/evidence/`（部分在 mve 目录）。

## 7. 复现与验收指南

```powershell
# 1) 重建 XSched（含 sm120 接入）并安装
cd <repo>; .\make.bat cuda          # 或 cmake --build build; cmake --build build --target install
# 2) T7 集成测试（产品路径）
cd test\sm120_integration
powershell -ExecutionPolicy Bypass -File run.ps1          # 期望 PASS=26 FAIL=0 exit=0
# 3) T6 MVE 全量（需独占 GPU；preflight 默认开启）
cd ..\sm120_mve
powershell -ExecutionPolicy Bypass -File run_mve.ps1       # 期望 PASS=45+ FAIL=0
# 4) T5 数组审查（任意时刻可跑）
python t5_verify.py
```

## 8. 对 T8+ 的输入
- **T8 联调**：框架级 Suspend/Resume 已验证到 HAL 层（I2/I6）；下一步
  接 `sched`/`xserver` 层（HPF 双进程场景）——Windows 下 L2 已解锁，
  无需再改 arch.cpp；
- **注意事项**：性能类测试（时延/开销）必须在空闲 GPU 上执行；
  建议 CI/SOP 中先跑 preflight 再分析数据；
- sm120 的 trap（L3）维持"不可用"状态（TarpHandlerSM120 属阶段三，
  且 Windows 标准驱动的 handler dump 受阻）。
