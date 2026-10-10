# sm120 release/sm120-level2 分支 Diff 审查与优化调研文档

> 版本：v2.0（已按用户决策更新）　日期：2026-10-10
> 范围：仅针对 `release/sm120-level2` 分支的发布 commit `392e7da`
> （`feat(cuda): add sm120 (Blackwell) Level-2 preemption support`）
> 基线：`f49289f`（feat(lg200)）
> 状态：**调研文档，暂不执行**（用户确认后再动手）

---

## 0. 用户已确认的优化策略（v2.0 核心）

> 本节为最终决策，§5–§8 均据此重写。

1. **release 只保留"可构建的完整生产代码"**——即编译进产品（libhalcuda /
   libshimcuda）所必需的源码与构建脚本。
2. **注释增强全部去掉**——3 个公共头文件还原到基线；shim.cpp 只保留真实代码改动。
   注释内容"在 dev 中保留即可，release 不需要"。
3. **与生产无关的测试代码不进 release**——`tests_local/`、`manage_xsched.ps1`
   等移到 dev 分支即可。
4. **release 相对原版本改动越少越好**（最小 diff 原则）。

一句话：**release = 能编译出 sm120 L2 功能的最小生产代码集，其余一律下沉 dev。**

---

## 1. 问题陈述

`release/sm120-level2` 作为**面向上游 PR 的纯生产发布分支**，其单个发布 commit
的 diff 规模为 **25 files, +2542 / -217**。用户反馈：

> "怎么 sm120 的 release level2 的修改这么多？不要在发布版修改这么多注释啊？
> 一看 diff 全是改动，不利于后续审查。"

核心诉求：**发布分支的 diff 应当让审查者一眼看清"功能性改动"，而不是被大量
注释/文档美化淹没。**

---

## 2. 量化诊断（硬数据）

采用两种独立方法交叉验证：
- **方法 A（语义法）**：`strip_comments()` 剥离全部注释后逐行比对代码 token，
  判定"代码是否真正改变"。
- **方法 B（行级法）**：对每一条 `+/-` diff 行分类为"注释/空行" vs "代码行"。

### 2.1 修改型文件（15 个）——注释 vs 代码

| 文件 | 原始 diff | 真实代码改动 | 装饰性(注释) | 判定 |
|------|:---:|:---:|:---:|------|
| `include/xsched/types.h` | +127/-60 | **0（字节级一致）** | 187 | 🔴 纯注释 |
| `include/xsched/xqueue.h` | +100/-71 | **0（字节级一致）** | 171 | 🔴 纯注释 |
| `include/xsched/hint.h` | +64/-35 | **0（字节级一致）** | 99 | 🔴 纯注释 |
| `platforms/cuda/shim/src/shim.cpp` | +284/-12 | **8** | ~288 | 🔴 注释为主 |
| `platforms/cuda/hal/src/arch/arch.cpp` | +41/-18 | 39 | 20 | 🟡 混合 |
| `platforms/cuda/hal/inject/Makefile` | +57/-7 | 54 | 10 | 🟢 代码为主 |
| `platforms/cuda/hal/src/common/cuda_command.cpp` | +28/-0 | 26 | 2 | 🟢 代码 |
| `platforms/cuda/hal/include/.../common/cupti.h` | +7/-1 | 8 | 0 | 🟢 代码 |
| `platforms/cuda/hal/include/.../level2/guardian.h` | +11/-0 | 2 | 9 | 🟡 注释为主 |
| `platforms/cuda/hal/src/level2/instrument.cpp` | +6/-4 | 6 | 4 | 🟢 代码 |
| `platforms/cuda/hal/src/level2/mm.cpp` | +8/-0 | 2 | 6 | 🟡 注释为主 |
| `platforms/cuda/hal/inject/inject.cu` | +6/-0 | 4 | 2 | 🟢 代码 |
| `platforms/cuda/shim/src/intercept_windows.cpp` | +1/-3 | 3 | 1 | 🟢 代码 |
| `README.md` | +8/-3 | 11 | 0 | 🟢 文档 |
| `platforms/cuda/README.md` | +13/-3 | 15 | 1 | 🟢 文档 |

**修改型文件小计：装饰性 ~780 行，真实代码 ~180 行。**

### 2.2 新增文件（10 个，共 +1781 行）

| 文件 | 行数 | 性质 | 是否编译进产品 |
|------|:---:|------|:---:|
| `tools/instrument/extract_sass.py` | 973 | 离线生成工具（T4 资产） | ❌ 否 |
| `tools/instrument/README.md` | 205 | 工具文档 | ❌ 否 |
| `manage_xsched.ps1` | 152 | 运维管理脚本 | ❌ 否 |
| `platforms/cuda/hal/src/arch/sm120.cpp` | 128 | **L2 核心：指令数组** | ✅ 是 |
| `tests_local/test_cuda_driver.cpp` | 96 | 测试 | ❌ 否 |
| `tests_local/run_tests.ps1` | 87 | 测试编排 | ❌ 否 |
| `tests_local/test_pytorch.py` | 63 | 测试 | ❌ 否 |
| `platforms/cuda/hal/inject/make_msvc.bat` | 47 | inject 重生成脚本(Win) | ❌ 否(离线) |
| `platforms/cuda/hal/include/.../arch/sm120.h` | 28 | **L2 核心：类声明** | ✅ 是 |
| `make.bat` | 2 | 构建入口 | ✅ 是 |

### 2.3 决定性结论

```
发布 commit 392e7da 的真实构成：
  ├─ 真正的 sm120 L2 功能代码    ≈ 300 行（sm120.cpp/h + arch.cpp + 少量适配）
  ├─ 工具/测试/文档（新增文件）  ≈ 1781 行（其中大部分应下沉 dev）
  └─ 既有文件的注释美化          ≈ 780 行（★ 问题所在，应全部还原 ★）
```

**sm120 L2 的实际功能足迹很小（约 300 行真实代码），却被约 780 行注释改动 +
1781 行工具/测试淹没。** 这正是"一看 diff 全是改动"的根源。

---

## 3. 问题根因分析

### 3.1 三个公共头文件被"文档化重写"

`types.h` / `hint.h` / `xqueue.h` 的改动模式（以 types.h 为例）：

```c
// 改动前
typedef enum {
    kXSchedSuccess           = 0,
    kXSchedErrorHardware     = 1,
    ...
} XResult;

// 改动后（代码完全没变，只是加了注释和分节横幅）
/* ==============================================================================
 * 2. Error Codes & Return Values
 * ============================================================================== */
typedef enum {
    kXSchedSuccess           = 0,   // Operation succeeded.
    kXSchedErrorHardware     = 1,   // Low-level hardware or driver error.
    ...
} XResult;
```

问题性质：
1. **与 sm120 L2 功能零相关**——这些是全局公共 API 头文件，加注释不属于本次特性。
2. **代码字节级未变**（方法 A 证明），却产生 457 行 diff 噪声。
3. **风格突变**——原仓库注释风格被整体替换，审查者难以判断"哪些是本次改动"。

### 3.2 shim.cpp 被插入大段 Doxygen 文档

`shim.cpp` +284 行中约 288 行是 `/** @file ... */` 文件头、`// ===` 分节横幅、
逐函数文档块，**真实代码仅 4 行改动**。审查者打开 diff 看到的是"整屏绿色新增"，
完全无法定位那 4 行关键改动。

### 3.3 违反 Release 分支的"最小必要改动"原则

发布分支（用于上游 PR）应满足：单一职责、最小 diff、可审查性。当前 commit 把
"功能实现 + 全仓库注释美化 + 测试资产 + 离线工具"混在一起，违反全部三条。

---

## 4. 为什么有害（对后续工作的具体影响）

| 影响面 | 具体后果 |
|--------|----------|
| **上游 PR 审查** | 审查者面对 +2542 行，其中 780 行是无关注释，极易漏看真正的 300 行功能代码 |
| **git blame 污染** | types.h/hint.h/xqueue.h 每一行 blame 都指向本 commit，掩盖 API 真实历史 |
| **后续 merge 冲突** | 公共头文件被整体重排，其他分支（如 context-isolation）改同一文件必然大面积冲突 |
| **回滚困难** | 注释改动与功能改动同 commit，无法干净剥离 |
| **跨分支同步** | evidence/sm120-level2、sm120-level3-probe 都基于此，噪声被继承放大 |

---

## 5. 确认方案：release 瘦身为纯生产代码

> 依据 §0 用户决策。核心操作分三类：**KEEP（保留）/ REVERT（还原基线）/ MOVE-TO-DEV（下沉）**。

### 5.1 25 个文件逐一处置表

#### ✅ KEEP —— 编译进产品的生产代码 + 必需构建脚本（13 个）

| 文件 | 处置细节 |
|------|----------|
| `platforms/cuda/hal/src/arch/sm120.cpp` | 保留全部（新增，指令数组=生产代码） |
| `platforms/cuda/hal/include/.../arch/sm120.h` | 保留全部（新增，类声明） |
| `platforms/cuda/hal/src/arch/arch.cpp` | 保留 39 行代码，**剥离 20 行注释** |
| `platforms/cuda/hal/src/common/cuda_command.cpp` | 保留（26 行代码） |
| `platforms/cuda/hal/src/level2/instrument.cpp` | 保留（guardian 资源参数化，6 行代码） |
| `platforms/cuda/hal/src/level2/mm.cpp` | 保留 2 行代码，**剥离 6 行注释** |
| `platforms/cuda/shim/src/shim.cpp` | **仅保留 4 行真实改动，剥离 ~288 行 Doxygen 块** |
| `platforms/cuda/shim/src/intercept_windows.cpp` | 保留（3 行代码） |
| `platforms/cuda/hal/include/.../common/cupti.h` | 保留（8 行代码） |
| `platforms/cuda/hal/include/.../level2/guardian.h` | 保留 2 行代码，**剥离 9 行注释** |
| `platforms/cuda/hal/inject/Makefile` | 保留（ARCH=120 构建支持） |
| `platforms/cuda/hal/inject/inject.cu` | 保留（+6 行 trap 源，既有生产文件） |
| `make.bat` | 保留（+2 行构建入口） |

#### 🔴 REVERT —— 纯注释改动，还原到基线 f49289f（3 个）

| 文件 | 处置细节 |
|------|----------|
| `include/xsched/types.h` | `git checkout f49289f --` 完全还原（代码本就字节级一致） |
| `include/xsched/hint.h` | 同上，完全还原 |
| `include/xsched/xqueue.h` | 同上，完全还原 |

#### 📦 MOVE-TO-DEV —— 与生产无关，下沉 dev/evidence 分支（6 个）

| 文件 | 去向 | 理由 |
|------|------|------|
| `tests_local/run_tests.ps1` | dev/evidence | 测试编排，非生产 |
| `tests_local/test_cuda_driver.cpp` | dev/evidence | 测试代码 |
| `tests_local/test_pytorch.py` | dev/evidence | 测试代码 |
| `manage_xsched.ps1` | dev/evidence | 运维脚本，非生产 |
| `tools/instrument/extract_sass.py` | dev/evidence | 离线生成器（973 行），不编译进产品 |
| `tools/instrument/README.md` | dev/evidence | 工具文档 |

> 说明：这些文件在 `evidence/sm120-level2` / `sm120-level2` 分支中**大多已存在**，
> "下沉"在操作上等于"从 release 移除"（dev 侧已保留），不会丢失。

#### ⚖️ 需你最终拍板的 3 个判断项

| 文件 | 我的建议 | 权衡 |
|------|----------|------|
| `platforms/cuda/hal/inject/make_msvc.bat`(47) | **MOVE-TO-DEV** | 它是 inject_120.cubin 的 Windows 重生成脚本，与 extract_sass.py 同属"离线生成链"，不编译进产品。若你认为 release 应能就地重生成指令数组，则 KEEP。 |
| `README.md`(+8/-3) | **KEEP** | 仅是把 sm120 加入支持矩阵的小改动，属于"发布该功能的正当文档"。若追求极致最小 diff 可 REVERT。 |
| `platforms/cuda/README.md`(+13/-3) | **KEEP** | 同上，平台支持说明。 |

### 5.2 净化效果预估

```
重构前 release diff：  25 files, +2542 / -217
重构后 release diff：  ~13 files, 约 +600 / -40
  ├─ 3 个公共头文件：      457 行噪声 → 0
  ├─ shim.cpp：            296 行 → ~8 行
  ├─ 其余 KEEP 文件注释：   ~37 行 → 0
  └─ 6 个 MOVE-TO-DEV：    1626 行 → 0（下沉 dev）
```

审查者打开重构后的 release diff，看到的就是**纯粹的 sm120 L2 功能代码**。

---

## 6. 执行计划（确认策略后执行，当前暂不动手）

> 原则：先在临时分支重构 + 等价性验证，确认无误再由你决定是否覆盖 release。

### 步骤 1：建立安全工作分支
```bash
git switch -c refactor/sm120-release-cleanup origin/release/sm120-level2
```

### 步骤 2：REVERT 三个公共头文件到基线
```bash
git checkout f49289f -- include/xsched/types.h \
                        include/xsched/hint.h \
                        include/xsched/xqueue.h
```

### 步骤 3：剥离 KEEP 文件中的注释增量（保留代码）
- `shim.cpp`：移除新增 `/** @file */`、`// ===` 横幅、逐函数 Doxygen 块，
  **仅保留那 4 行功能改动**（用 strip_comments 比对确认代码零丢失）。
- `arch.cpp` / `mm.cpp` / `guardian.h`：移除新增注释行，保留代码行。

### 步骤 4：MOVE-TO-DEV —— 从 release 移除测试/工具/脚本
```bash
git rm -r tests_local/ manage_xsched.ps1 tools/instrument/
# make_msvc.bat 视 §5.1 判断项决定
```
（确认这些内容已在 evidence/dev 分支存在，不会丢失）

### 步骤 5：squash 为单个干净功能 commit
```bash
git commit --amend -m "feat(cuda): add sm120 (Blackwell) Level-2 preemption support"
git diff --stat f49289f HEAD   # 期望：~13 files, +600/-40
```

### 步骤 6：等价性验证（关键，不可省）
```bash
# 确保重构后"代码"与原始 release 完全等价（只少了注释/测试，功能零回退）
python test/analyze_release_diff.py --verify-equivalence
# 期望：所有 KEEP 文件 CODE IDENTICAL
```

### 步骤 7：构建 + 测试验证
```bash
mingw32-make cuda          # 确认 release 瘦身后仍能完整构建
# 在 dev/evidence 分支重跑 MVE(48) + 集成(26) 确认功能未回退
```

---

## 7. 验证标准（重构成功的判据）

| 判据 | 目标值 | 验证方法 |
|------|--------|----------|
| 公共头文件 diff | **0 行**（types/hint/xqueue.h 回到基线） | `git diff f49289f HEAD -- include/` |
| shim.cpp diff | ≤ 12 行（仅 4 行代码 + 上下文） | `git diff --numstat` |
| tests_local/、manage_xsched.ps1、tools/instrument/ | **不在 release** | `git ls-files` 检查 |
| 功能代码等价性 | **字节级一致** | strip_comments 语义比对 |
| 发布 commit 总 diff | 从 +2542 降至 ~+600 | `git diff --stat f49289f HEAD` |
| 构建 | `mingw32-make cuda` 成功 | 全量编译零错误 |
| 功能不回退 | 26/26 集成 + 48/48 MVE（dev 分支跑） | 重跑 evidence 套件 |

---

## 8. 风险与注意事项

1. **强推风险**：重构 release 需 `git push --force`。建议先在
   `refactor/sm120-release-cleanup` 完成并验证，再由你决定是否覆盖 release。

2. **下游分支联动**：`sm120-level3-probe`、`evidence/sm120-level2` 基于 release。
   重构后需评估 rebase——L3 分支主要新增 `test/` 与 `hal/src/arch`，与头文件
   注释无冲突，rebase 成本低。

3. **等价性验证不可省**：任何"移除注释"操作都必须用 strip_comments 语义比对
   确认代码零改动，避免误删功能代码（尤其 shim.cpp 那 4 行、arch.cpp 的
   sm120 dispatch——历史上 arch.cpp 曾有 Windows 短路缺陷，务必确认 dispatch
   逻辑完整保留）。

4. **extract_sass.py 可复现性**：移到 dev 后，release 单独看无法重生成
   sm120.cpp。**这是用户明确接受的权衡**（release 只要"可构建的完整代码"，
   sm120.cpp 已作为生成产物提交）。务必确保 dev/evidence 分支保留该工具，
   以便未来 sm130 适配复用。

5. **测试下沉不丢失**：MOVE-TO-DEV 前必须确认 tests_local/ 等已存在于
   dev/evidence 分支（`git ls-files origin/evidence/sm120-level2` 核对），
   避免"移除即丢失"。

---

## 附录：分析工具

本次诊断使用的脚本：`test/analyze_release_diff.py`
- 方法 A：strip_comments 语义比对（判定代码是否真变）
- 方法 B：行级注释/代码分类（量化 churn 比例）
- 可复现：`python test/analyze_release_diff.py`
- 待扩展：`--verify-equivalence` 模式（步骤 6 用，重构后校验 KEEP 文件代码等价）

数据来源：
- `git diff --numstat f49289f origin/release/sm120-level2`
- `git show f49289f:<file>` vs `git show origin/release/sm120-level2:<file>`
