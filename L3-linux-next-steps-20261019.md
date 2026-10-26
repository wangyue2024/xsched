# sm120 Level-3 后续实验规划（Linux 端）

> 版本：2026-10-19
> 适用：RTX 5060（sm_120）+ Linux + 驱动 595.99.02（开源内核模块）
> 分支：`dev/Linuxlevel3`（`origin/dev/Linuxlevel3` @ `933078e`）
> 用途：带着这份文档到 Linux 机器上，按 EP 顺序推进；每一步都给出判据，避免"做了实验但不知道算不算数"。

---

## 0. 这份文档怎么用

### 0.1 三条铁律

1. **每个结论必须能指到一条日志**。指不到，就标成推断。
2. **每条断言标等级**：✅ 已观测 ／ ⚠️ 推断（有证据但依赖假设）／ ❓ 未知。
3. **不写"不可能"**，只写"在什么条件下没观测到"。差别见 §2.3。

### 0.2 建议的工作流

```
写实验 → 跑 → 落日志（带 SHA256）→ 更新本文档 §1 的状态表 → 再写下一个实验
```

不要攒到最后一起写。你的优势就是"证据齐"，别丢。

---

## 1. 现状快照

### 1.1 已确认（观测级，✅）

| # | 事实 | 证据 |
|---|---|---|
| 1 | 导出表可解析：`ToolsTrapHandler` 返回 184 字节，**22 个函数槽（索引 1–22）** | `probe_export_table_*.log`；`GetEtblFunc` 反汇编里的边界检查 `8+slot*8 <= table[0]` |
| 2 | slot 19（`GetInfoPascal`）在 sm120 上，**测过的 5 种参数组合全部返回 101 = `CUDA_ERROR_INVALID_DEVICE`** | `probe_export_table_*.log` |
| 3 | slot 20 返回 40 字节结构：`+0x10` = memobj handle、`+0x18` = 0x880、`+0x20` = 0x940 | `probe_slot20_*.log`（跨调用稳定） |
| 4 | 用 ToolsMemory 槽 24/25 换出：`pc = 0x73e8a75f5300`、`size = 0x1300` | `final_writeback.log` |
| 5 | handler 结构：`[0,0x880)` 变体 A ／ `[0x880,0xA00)` 共享返回桩 ／ `[0xA00,0xA80)` 零填充 ／ `[0xA80,0x1300)` 变体 B | `handler_880_disasm.txt` |
| 6 | 共享返回桩起始是两条 NOP，且**变体 A 所有出口必经** | 同上 |
| 7 | 在 0x880 写入 48 位绝对 JMP 后，DtoH 回读**逐字节一致** | `final_writeback.log` |
| 8 | `cuXtraTriggerTrap` 调用瞬时返回（0.000s），context 健康，**无任何可观测副作用** | `final_trigger_probe.log`、`mini7_off_*.log` |
| 9 | 三处签名载荷（0x0 / 0x60 / 0x880）在触发后**均未出现**（`sig == 0`） | `mini7_off_*.log` |
| 10 | 内核内真实 `brkpt`：不武装 → `cudaErrorIllegalAddress(700)`；载荷不执行 | `mini9_run1.log` |
| 11 | cuda-gdb 会话下重跑，载荷仍不执行 | `mini7b`/`mini9` 会话轮 |
| 12 | **武装 0x83de 三个模式位（`031f/0307/032a` 同开）后，`brkpt` 从 700 崩溃变为 err=0 干净终止** —— 全轮唯一可复现的正向变化。**未做消融，不知是哪一个位起作用** | `arm_modes` + `mini9` |
| 13 | 武装后，注入的代码**仍然不执行** | 同上 |
| 14 | cuda-gdb 对运行中内核发 `SIGINT`，`info cuda threads` 显示 "No CUDA threads"，kernel 仍 Active | `pause_cap.log` |
| 15 | `0x83de` 方法族可从 `nv-kernel.o`（NVIDIA 内核模块，未 strip）的 `__nvoc_exported_method_def_KernelSMDebuggerSession` 解出，共 33 个方法 | `ATTACH_REPLAY_PLAN.md` §0 |
| 16 | LD_PRELOAD 差分（普通 vs cuda-gdb）：只在 attach 侧出现的命令字有 8 个，全部属于 `0x83de` 类 | `ioctl_normal.log` / `ioctl_cudbg.log` |
| 17 | cuxtra 里 `0x419e84` 的写入**看起来是脉冲**（置位 bit31 + 清位）：24 字节 = 两条 12 字节参数，第三字段疑为掩码 —— ⚠️ 字段语义按字节分组推断，cuxtra 未公开该结构 | `cuXtraTriggerTrap` 反汇编（本机复跑） |
| 18 | **GUID 与 `0x419e84` 都存在于 NVIDIA 自己的 `nvcuda64.dll`**（DriverStore），导出表与 22 个槽也在该模块 | `_probe/owner.cpp` 实测 + 字节扫描 |
| 19 | Windows 上 `cuCheckpointProcess*` 六个 API 全部返回 801；`attr90 = 0`（WDDM） | `probe_win_trigger2_*.log` |
| 20 | Linux 上 `attr90 = 1` | `probe_export_table`（Linux 轮） |

### 1.2 推断（⚠️ 有证据，但依赖未验证假设）

| # | 推断 | 依赖的假设 |
|---|---|---|
| A1 | slot20 的 memobj 是"非活动模板"（读写合法，但不在硬件执行路径上） | `brkpt` 会进入同一个 handler |
| A2 | 真正的激活机制在 `0x83de` 会话栈 | 武装改变的是"异常处理路径"，且与 trap handler 同源 |
| A3 | slot 20 是 Blackwell 版的 handler-info getter | 结构同形 + 下游接受 + 内容反汇编吻合 + 偏移 0x880 双源一致 |

> A3 的证据强度已经接近结论；A1/A2 仍应带"我倾向于认为"。

### 1.3 未知（❓ 这三个不搞清，汇报口径就会失真）

| # | 问题 | 影响 |
|---|---|---|
| U1 | **`0x419e84` 那次写到底有没有产生一次 trap** | 决定"注入不执行"的两种解释怎么归因 |
| U2 | **`brkpt` 是否走同一个 trap handler** | 决定 A1 是否成立 |
| U3 | Windows 用户态是否可达 D3DKMT / RM 等价路径 | 决定"Windows 能不能做 L3"这个问题是否有意义 |

---

## 2. 需要永久记住的六条纠正

这些是这轮讨论中已经踩过的坑，写下来防止复发。

### 2.1 "Windows 没有这个能力" ❌

**错。** 早期搜 `System32\nvcuda.dll` 找不到常量，是因为那是**转发层**；真正的实现模块是 DriverStore 里的 `nvcuda64.dll`（32 MB）。GUID 和 `0x419e84` 都在里面，导出表与 22 个槽也都在里面。

**正确说法**：机制在 Windows 上也存在；缺的是"用户态怎么走到它"，cuxtra 只实现了 Linux 的入口。

### 2.2 "0x419e84 是触发器，所以写它就该有反应" ❌

**调用成功 ≠ 产生效果**。RM 写寄存器是投递式的，`rc=0` 只代表命令被接受。

**正确说法**：调用被接受；无任何可观测后果；是否产生 trap 未知。

### 2.3 "brkpt 行为变化是触发造成的" ❌

**错。** `arm_modes.c` 只发三条 `0x83de` 命令（`031f/0307/032a`），**没有** `0x419e84`。`mini9.cu` 里也没有触发调用。

**正确说法**：变化来自**武装调试模式**，与触发寄存器无关。

### 2.4 "23 个函数槽"

`table[0] = 184` 是**字节长度**，184/8 = 23 个字，其中第 0 个字是长度，所以**函数槽是 22 个（索引 1–22）**。

### 2.5 "101 表示不支持"

`101 = CUDA_ERROR_INVALID_DEVICE`；`801` 才是 `NOT_SUPPORTED`。101 更像"这个 getter 的设备/架构表里没有这项"。

### 2.6 "Windows 上 ObjGetSize 拿到了大小"

Windows 上返回的是 `0x7ffba2d161b0`——**等于 `ObjGetSize` 函数自己的地址**，是签名不匹配产生的垃圾值。Windows 侧从未成功读回 handler 字节。

---

## 3. 核心问题与实验路线

```
U1（trap 是否发生）
   ├─ 是 → U2/A1（handler 是否活体）→ EP-4 / EP-5
   └─ 否 → 触发路径问题 → EP-3 → EP-6（sm86 对照）
```

**EP-2 是整个规划的分水岭。** 先做它。

---

## 4. 实验清单

> 每个实验给：目的 / 前置 / 步骤 / 观测 / 判据 / 失败说明什么 / 预估。
> **做完一个就更新 §1 的状态表。**

### EP-0 环境固化与基线快照（必做，10 分钟）

**目的**：保证你后面所有结论都能对上一个确定的起点。

```bash
mkdir -p ~/l3/exp && cd ~/l3
{
  uname -a
  nvidia-smi --query-gpu=name,driver_version,compute_cap --format=csv
  nvcc --version
  ls -l /dev/nvidiactl /dev/nvidia0
  cat /proc/driver/nvidia/version
} | tee ~/l3/exp/env.txt

sha256sum ~/l3/exp/env.txt
```

**判据**：`compute_cap = 12.0`，驱动 `595.xx`，`/dev/nvidiactl` 可读写。任一项不符 → 先修环境，别往下做。

---

### EP-1 基线复现：确认"注入不执行"仍成立（必做，30 分钟）

**目的**：如果换了机器/驱动，基线可能已经变了。**先确认你还能复现旧结论**，否则后面所有对比都失效。

**步骤**
```bash
cd ~/xsched && git log --oneline -1          # 应为 933078e 或其后
ls test/sm120_l3_probe/attach_replay/
# 1) 写入验证
./mini7b --offset 0x880 --arm 0
# 2) 真实 brkpt（武装 / 不武装）
./mini9 --arm 0 ; ./mini9 --arm 1
```

**观测**
- 写入后回读是否逐字节一致；
- 三个 offset 的 `sig` 是否都为 0；
- `brkpt` 是否仍是 700 → 武装后 err=0。

**判据**
- 三项都复现 → 基线一致，继续。
- `sig != 0` → **立刻停下**。说明 handler 变活了，旧结论失效，直接跳到 EP-5。
- `brkpt` 在武装后仍是 700 → 武装失效，先查 `arm_modes` 是否真的发出去了。

---

### EP-2 ★ 构造 trap 观测器（最高优先级，0.5–2 天）

**目的**：回答 U1——**在不依赖 handler 执行的前提下，判断 trap 有没有发生**。

这是整份规划里唯一能破局的东西。没有它，你永远在"注入不执行"这一个观测上原地打转。

#### 方法 A：SM 错误状态差分（最省事，先试这个）

`0x83de030c`（`DebugReadAllSmErrorStates`）返回 4824 字节结构化缓冲。

```
读 030c → 触发 → 等 1ms → 再读 030c → diff
对照组：不触发，同样两次读 → diff
```

**判据**：触发组出现可复现的非零差异，对照组没有 → **trap 发生过**。

#### 方法 B：PC / warp 状态采样

用 CUPTI 的 PC sampling，或 `0x83de0315` 读 SM 侧状态，在触发后的极短窗口内采样。

**判据**：采样点落在 handler 地址区间（`pc … 0x1300`）→ 直接证据。
> 注意：trap handler 执行极短（几十条指令），采样命中率低，需要放大（例如循环触发 N 次）。

#### 方法 C：行为侧探针（不需要调试接口）

找一个"trap 一定会改变"的可观测量：
- trap 会做 `CCTL.IVALL` / `MEMBAR.SYS` → 可用**微基准的耗时或缓存命中**做间接探针；
- 若 trap 进入 handler 后会写内存 → 用一个"空 handler 副本"做对照，看写量差异。

**判据**：触发组与不触发组有稳定统计差异（多次重复，p<0.05 级别）。

#### 结果怎么用

| EP-2 结果 | 结论 | 下一步 |
|---|---|---|
| 有 trap 证据 | 触发有效，问题在 handler 位置 | → EP-4、EP-5 |
| 无任何差异 | 触发很可能没生效 | → EP-3 |
| 结果不稳定 | 观测手段不够灵敏 | 换方法 B，或先做 EP-6 对照 |

---

### EP-3 触发路径的独立实现（1–2 天）

**目的**：排除"cuxtra 封装有问题"这个解释，同时为 Windows 侧的 D3DKMT 探索做铺垫。

**步骤**
1. 自己拼 NVOS54（`req = 0xc020462a`），参数结构：
   ```
   +0x00 hClient (4)
   +0x04 hObject (4)
   +0x08 cmd     (4)
   +0x0C flags   (4)
   +0x10 params 指针 (8)
   +0x18 paramsSize (4)
   +0x1C status  (4)
   ```
2. 三种写法各跑一遍：
   - (a) 与 cuxtra 相同：**置位 + 清位脉冲**（两条 12 字节参数：`{0x419e84, 0x80000000, mask}` 和 `{0x419e84, 0, mask}`）
   - (b) 只置位，不清位
   - (c) 置位后延时再清位（拉开时间）
3. 每种写法都接 EP-2 的观测器。

**判据**：若自写能复现 EP-2 的差异 → 触发链路打通，且知道它需要脉冲还是单写。
**失败说明**：要么参数结构不对，要么这条命令需要驱动内部上下文（→ 转 EP-5 的静态 xref）。

---

### EP-4 handler 活性的直接检验（1 天）

**目的**：回答 U2/A1。用"最小、最不会出错"的载荷，避免把"payload 写错"和"没执行"混在一起。

**步骤**
1. **计数器载荷**：在 0x880 写入「读一个全局地址 → 加 1 → 写回 → 恢复原指令 → 返回」。触发后从主机读那个全局地址。
2. **入口魔数**：在 0x0（dispatch 入口）写「写魔数到全局地址」。这条更强——如果 dispatch 都不执行，说明整个 handler 都没进。
3. 三种状态下各跑一遍：不武装 / 武装 / cuda-gdb 会话。

**判据**：计数器或魔数出现 → handler 活体，A1 被推翻（那问题就在别处）。
**若全不出现** → A1 得到支持；但仍需 EP-2 说明 trap 是否发生。

**注意**：写 0x0 会破坏 dispatch，务必先备份原指令并在载荷末尾恢复；否则一次运行就把 handler 弄坏（每个 context 有独立副本吗？先验证这一点）。

---

### EP-5 找到"驱动真正使用的那份 handler"（2–4 天）

**前提**：EP-4 显示 handler 不执行。

**思路**：如果 memobj 只是模板，那么 live copy 必然在别处——要么是驱动自己复制的一份，要么在 attach 时才映射。

#### 路径 A（动态）：dump 前后对比
```
在 cuCtxCreate 之后、attach 之前/之后，分别枚举进程内所有可读的设备 VA 区域，
找出"新出现、且大小为 0x1300、开头两条 NOP"的区域。
```
用 `0x83de0315`（ReadMemory）或 `cuMemcpyDtoH` 配合地址扫描。

#### 路径 B（静态）：找谁读这个 handle
在 `libcuda.so` 里定位 `ObjGetPc` 的调用者，往上追是谁把 memobj 交给 GPU 的 trap 设置逻辑。
> 这就是之前没做的那一步"追 xref"。

#### 路径 C（对照）：在 attach 会话里再看一次
若 live handler 只在 attach 后存在，那么在会话内 dump 应该能看到不同的地址。

**判据**：找到一份"被驱动引用、且内容可执行"的 handler → 那就是新的 patch 点。

---

### EP-6 sm86 / V100 正对照（需要硬件，优先级最高但受条件限制）

**目的**：建立"有效"的参照。**这是唯一能给出功能级确定的实验。**

**步骤**
1. 同一份代码、同一套 API（slot 19 而非 20）、同一个 `0x419e84` 触发；
2. 跑 mini7b（签名载荷）+ mini9（brkpt）；
3. 记录：内核是否被中止、payload 是否执行、`attr90` 值。

**判据**
- payload 执行 → 配方在 sm86 上成立，sm120 的差异是平台/驱动层面；
- payload 不执行 → **我们对 sm86 的理解有误**，整个故事要重写（这更重要，要立刻汇报）。

---

### EP-7 slot 20 语义的跨平台/跨架构一致性（半天）

**目的**：把推断 A3 加固成结论。

1. **Linux vs Windows 的 40 字节输出对比**：字段位置、含义是否同形；
2. **slot 19 用 40 字节输出缓冲再扫一遍**：排除"参数尺寸不对"这个解释；
3. （若能拿到 sm86）**同时调 19 和 20**，看是否返回同一个 handle —— 若是，A3 直接闭环。

**判据**：三项中两项成立 → A3 可以写成结论。

---

### EP-8 Windows 侧 D3DKMT 探索（长期，单独排期）

**目的**：回答 U3，为"跨平台扩展"提供依据。

**已有的落点**
- 实现模块：`C:\Windows\System32\DriverStore\...\nvcuda64.dll`
- 文件偏移 `0x21d00` 附近：构造寄存器写命令（`mov [rax+0Ch], 0x419e84` → `call [rax+9208h]`）
- 文件偏移 `0x21c5d` 附近：`mov edx, 0x419e84; call rel32`
- 模块内有 `nvlddmkm`（UTF-16）与 `D3DKMT` 字符串 → 走 WDDM thunk

**步骤**
1. 静态：定位这两个位置所在函数，往上追调用者，看挂在哪个 `cu*` 导出下；
2. 动态：用 API Monitor / ETW 观察普通 CUDA 程序是否执行到这段；
3. 对照：cuda-gdb（Linux）与 Nsight（Windows）下是否只有调试会话才走这条路。

**判据**：若普通程序也走 → 这条路径可被用户态间接使用，值得投入。

---

## 5. 决策树（贴在墙上）

```
EP-2 有 trap 证据？
├─ 是
│   └─ EP-4 payload 执行？
│       ├─ 是 → 问题解决了（说明之前是载荷/位置错），回去做完整 L3
│       └─ 否 → EP-5 找 live handler
└─ 否
    └─ EP-3 自写触发有反应？
        ├─ 是 → 修 cuxtra 调用姿势（脉冲/顺序/参数）
        └─ 否 → EP-6 sm86 对照
                 ├─ sm86 也不行 → 我们的配方理解有误，重读 upstream 实现
                 └─ sm86 行 → 这代平台差异成立，转入 EP-8 / 跨驱动代际
```

---

## 6. 最有价值的方向（按投入产出排序）

| # | 方向 | 为什么值 | 产出形态 |
|---|---|---|---|
| 1 | **trap 观测器** | 它是所有后续判断的前提；且跨架构、跨 XPU 都能用 | 一个可复用的工具 + 方法论 |
| 2 | **触发路径的独立实现** | 去掉对 cuxtra 的依赖，才谈得上"理解机制" | 一份 NVOS54 直调代码 + 参数说明 |
| 3 | **handler 定位方法（模板 vs 活体）** | 这是 L3 在这代平台上的核心差异点 | 一份"如何判断 handler 是否活体"的判据集 |
| 4 | **跨平台/跨代际对照矩阵** | 单机结论永远有歧义；矩阵才能定性 | 一张表（架构 × 驱动 × 触发 × handler 活性） |
| 5 | **把负结论沉淀成组内资产** | 下一个人不用重撞 | 归档文档 + 可复跑脚本 |

> 第 4 项是实验室最能提供支持的——你缺的不是想法，是第二台机器。

---

## 7. 汇报口径（三句话不能混）

| 场景 | 说 | 不要说 |
|---|---|---|
| 触发 | "调用被接受，无任何可观测后果；**是否产生 trap 未知**" | "触发生效了" / "触发无效" |
| handler | "拿到的是驱动暴露的 handler 内存；读写在 Linux 上验证通过" | "拿到了 trap 入口" |
| 结论 | "**这条配方**在这台机器上没走通，失败点不在写入环节" | "L3 不可行" / "平台能力边界" |
| 变化 | "武装调试模式后 `brkpt` 行为变化" | "触发后 `brkpt` 行为变化" |

---

## 8. 附录

### 8.1 关键常量

| 名称 | 值 |
|---|---|
| ETID ToolsTrapHandler | `cc529e94-5c4e-9d46-837c-9625868334e4` |
| ETID ToolsMemory | `bfdb432d-bf3c-5a4a-945e-b34029e81e75` |
| ETID ToolsModule | `6e163fbe-b958-444d-835c-e182aff1991e` |
| ETID ToolsContext | `21318c60-9714-3248-8ca6-41ff7324c8f2` |
| ETID ToolsPushBufferHal | `a6b1ff99-ecc4-c94f-92f9-1928663d5585` |
| slot 19 | `EtblTrapHandler::GetInfoPascal` |
| slot 20 | 事实上的 Blackwell getter（cuxtra 无符号） |
| slot 24 / 25 | `EtblMemory::ObjGetPc` / `ObjGetSize` |
| handler 大小 | `0x1300` = 4864 字节 |
| patch 点 | `0x880`（共享返回桩，起始两条 NOP） |
| 触发寄存器 | `0x419e84` bit31（⚠️ 推断为"置位 + 清位"脉冲，字段语义未公开） |
| NVOS54 请求码 | `0xc020462a`（size 0x20，type 0x46='F'，nr 0x2a） |
| 调试会话类 | `0x83de`（KernelSMDebuggerSession） |
| 会话方法 | `0315/0316` 读写内存；`0317/0318` suspend/resume；`030c` SM 错误状态；`031f/0307/032a` 模式位 |

### 8.2 文件索引（`dev/Linuxlevel3`）

```
test/sm120_l3_probe/
├── ARCHIVE_ZH.md                  证据索引
├── ATTACH_REPLAY_PLAN.md          0x83de 方法表 + R2 实验全表
├── L3_FINAL_SUMMARY_ZH.md         总报告（已做措辞修订）
├── L3_LINUX_REPORT_ZH.md
├── README.md                      Windows 探针原始记录（英文）
├── src/probe_*.cpp                探针源码
├── attach_replay/
│   ├── arm_modes.c                武装工具（只发 031f/0307/032a）
│   ├── ioctl_log2.c               RM ioctl 差分记录器
│   ├── l3ctl.cpp                  会话复刻工具
│   ├── mini7b.cu                  触发 + 签名载荷
│   ├── mini9.cu                   brkpt 对照
│   └── mini13.cu                  TSG 双上下文
└── evidence/
    ├── probe_export_table_*.log
    ├── probe_slot20_*.log
    ├── linux_2026-10-09/{final_writeback,final_trigger_probe,mini7_off_*,mini9_run1,ioctl_*}.log
    └── nvcuda_analysis.txt
```

### 8.3 证据 → 结论对照

| 结论 | 关键证据文件 |
|---|---|
| slot 19 在 sm120 上 101 | `probe_export_table_*.log` |
| slot 20 是 getter | `probe_slot20_*.log` + `handler_880_disasm.txt` |
| 写入正确 | `final_writeback.log` |
| 注入不执行 | `mini7_off_*.log`、`mini9_run1.log` |
| 武装有效 | `arm_modes` 输出 + `mini9` 两次对照 |
| 激活在 0x83de | `ioctl_normal.log` vs `ioctl_cudbg.log` |
| Windows 侧机制存在 | `_probe/owner.cpp` 实测 + `nvcuda64.dll` 字节扫描 |

### 8.4 复现 cuxtra 分析的步骤（如果你要重做）

```bash
git clone --depth 1 https://github.com/XpuOS/cuxtra.git
cd cuxtra && sha256sum lib/*.a
mkdir work && cd work && ar x ../lib/libcuxtra_linux_x86_64.a trap.cpp.o
nm trap.cpp.o | grep ETID                     # .rodata 偏移 + 符号名
objdump -s -j .rodata trap.cpp.o              # 16 字节 GUID
objdump -d -C -r --no-show-raw-insn trap.cpp.o \
  --disassemble=_ZN6cuxtra4etbl11GetEtblFuncEPK9CUuuid_stm
# 找 lea(GUID) + mov $slot,%esi + call GetEtblFunc 三件套
```

### 8.5 现有文档需要修的六处

1. `L3_FINAL_SUMMARY_ZH.md:107`：Windows 的 `ObjGetPc ✅ pc=0x880` 应注明"疑为偏移而非 VA，且未读回验证"。
2. `sm120-level3-probe/README.md`：slot 20 的 `+0x18 = 2176 bytes = handler size?` 与后文 `stub offset 0x880` 矛盾，后者正确。
3. 同上：`Trigger on Linux (works)` 应改为"调用被接受，无观测效应"。
4. 同上：结论第 1 条 `L3 is NOT fundamentally impossible` 应降级为"信息侧存在，抢占侧未验证"。
5. 平台通道表（`L3_FINAL_SUMMARY_ZH.md:304`）："Windows/WDDM 上一切 RM 直通被 kmod 阻断"应补充"驱动内存在等价常量（`nvcuda64.dll`），只是无用户态入口"。
6. 槽位数：`184B（23 槽）` → `184B，22 个函数槽`。

---

## 9. 下一步（今天就能开始的三件事）

1. **EP-0 + EP-1**：固化环境，确认基线还能复现。
2. **EP-2 方法 A**：用 `030c` 做 SM 状态差分——这是最可能当天出结果的一步。
3. **给学长的那条消息**（只问方法，不问结论）：

> 我在 sm120 上确认了 slot 19 对所有参数返回 101、slot 20 能换出 handler（`pc=0x…5f5300, size=0x1300`，反汇编吻合 0x880）。但 patch 写进去、回读一致、触发调用也返回成功，注入的代码却从未执行。想请问：**你们在 sm86 上验证这条链"生效"时的判据是什么？** 我现在缺的正是"trap 是否真的发生"这个观测量。

---

*文档结束。做完 EP-0/EP-1 回来更新 §1 的状态表，再做 EP-2。*

---

## 10. 状态更新（2026-10-25）— 本文件已被 v2 研究超越

root 文档里的 v1 结论（slot20 是活体 handler、触发无效即判死）**已过时**。
最新状态请看：

- `test/sm120_l3_trap_v2/L3_TRAP_SM120_REPORT_ZH.md`（阶段总报告）
- `test/sm120_l3_trap_v2/FINDINGS_ZH.md` v0.1–v2.1（滚动证据）

要点：

- `0xd04 ClientSetInheritedSharePolicy` 打通了 RS_ACCESS_DEBUG（跨进程 0x83de 会话 status 0）；
- 活体 handler 在 `arena base + 0x15f0f00`，入口 patch 后可执行注入 SASS，context 存活；
- trap 内"重定向 TRAP_RETURN_PC → EXIT"可干净杀 block（独立原型 8 s → 0.24 s，可复现）；
- XSched HAL 的 target↔agent 握手/授权/patch 已实现并跑通前半段；端到端还卡在
  "运行中改写 handler 被驱动同步阻塞 / 预置改写影响 launch trap"（715/719）。
- 生产可用路径仍是 `XSCHED_SM120_L3_BACKEND=GDB`（已复测冻结/恢复）。
