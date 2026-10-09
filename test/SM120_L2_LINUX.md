# SM120 Level-2 支持：Linux 复验与适配记录（LINUX VERIFICATION）

> **范围**：把本分支（`evidence/sm120-level2`）在 Windows 上封存的 sm120（Blackwell）
> Level-2 机制与全部测试套件（T3–T7），移植到 **Linux + RTX 5060（sm_120）真机**并
> 用 CUDA 12.9 工具链完整复验；记录平台差异、修复项、位级复现矩阵与最终结果。
> 上游 Windows 证据链见 `SM120_L2_EVIDENCE.md`（本文档不替代它）。
> 日期：2026-10-09。

---

## 1. 环境与工具链

| 项 | 值 |
|---|---|
| GPU | NVIDIA GeForce RTX 5060（GB206, CC **12.0 / sm_120**） |
| 驱动 | 595.99.02（真驱动 `/usr/lib/x86_64-linux-gnu/libcuda.so.1`） |
| CUDA（12.9，本机用户态安装） | **12.9.41**（nvcc/ptxas）、cuobjdump 12.9.26、nvdisasm 12.9.19 —— 与 Windows 证据**同版本** |
| CUDA（系统） | 12.4.131（Ubuntu 包，仅作对照；不能编译 sm_120） |
| 宿主编译器 | gcc/g++ 15.2（构建框架）；g++-13（nvcc 12.9 可接受的宿主） |
| 其它 | CMake 4.2.3、GNU Make 4.4.1、Python 3.14.4（脚本纯标准库） |
| 仓库文件系统 | **exFAT**（`/run/media/wy/Share`，不支持符号链接 → 影响 shim 安装与运行方式，见 §4.2） |

### 1.1 CUDA 12.9 免 root 安装（本机做法，可复现）

NVIDIA 官方仓库的 deb 直接解包到用户目录，无需 root、不污染系统：

```bash
BASE=https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64
mkdir -p /tmp/cuda-debs && cd /tmp/cuda-debs
for p in cuda-nvcc-12-9_12.9.41-1 cuda-nvvm-12-9_12.9.41-1 cuda-crt-12-9_12.9.41-1 \
         cuda-cuobjdump-12-9_12.9.26-1 cuda-nvdisasm-12-9_12.9.19-1 \
         cuda-cudart-dev-12-9_12.9.37-1 cuda-cudart-12-9_12.9.37-1 \
         cuda-driver-dev-12-9_12.9.37-1; do
    curl -sfLO "$BASE/${p}_amd64.deb"
done
mkdir -p "$HOME/cuda-12.9-root"
for f in *.deb; do dpkg-deb -x "$f" "$HOME/cuda-12.9-root"; done
ln -sfn "$HOME/cuda-12.9-root/usr/local/cuda-12.9" "$HOME/cuda-12.9"
export PATH="$HOME/cuda-12.9/bin:$PATH"
nvcc --version   # -> release 12.9, V12.9.41
```

包完整性可对照仓库 `Packages` 索引中的 SHA256（本机全部一致）。

### 1.2 glibc ≥ 2.41 兼容补丁（必需）

Ubuntu 25.10 的 glibc 2.41 把 `cospi/sinpi/rsqrt` 等 C23 数学函数**带
`noexcept(true)`** 声明进 `math.h`，与 CUDA ≤ 12.9 `crt/math_functions.h` 中不带
异常规格的声明冲突，nvcc 直接失败：

```
error: exception specification is incompatible with that of previous
       function "cospi" (declared at line 2601 of .../crt/math_functions.h)
```

补丁（`test/cuda129_glibc241_compat.patch`，与 CUDA 13 上游做法一致：
给 CUDA 侧声明补上同样的 noexcept 规格）：

```bash
cd "$HOME/cuda-12.9/targets/x86_64-linux/include"
patch -p1 < <repo>/test/cuda129_glibc241_compat.patch
```

说明：
- 该补丁**只影响宿主侧声明**，不改变设备代码；打过补丁后本机重新编译
  `inject_120.cubin` / `inject_86.cubin` / `mve_kernel.cubin` 与 Windows 封存产物
  **逐字节一致**（§3），即补丁不引入任何产物差异；
- 也可用 `NVCC_PREPEND_FLAGS='-Xcompiler -U_GNU_SOURCE'` 临时绕过，但对包含
  libstdc++ 头（`<chrono>`/`<thread>`）的 TU 会引发大量错误，故不作为方案；
- 两个 Linux 构建脚本会在缺失补丁时打印告警并指向本节。

## 2. 结果总览

| 项目 | Windows 证据 | Linux 复验（CUDA 12.9） | 证据文件 |
|---|---|---|---|
| T3 编译基线 + T4 金标准回归 | 9/9 PASS，产物可复现 | **9/9 PASS，exit 0；产物位级复现**（§3） | `test/sm120_l2_gen/evidence/run_all_output_linux.txt` |
| T5 数组审查 | 21/21 | **22/22 PASS（含 R1 位级复现）** | `test/sm120_mve/evidence/t5_verify_linux.txt` |
| T6 MVE 真机闭环 | 48 PASS / 0 FAIL，1000 轮 6.4s | **48 PASS / 0 FAIL，1000 轮 6.3s**（本地编译 cubin） | `test/sm120_mve/evidence/mve_linux129.log` |
| T7 全栈集成 | 26 PASS / 0 FAIL，suspend 2.6ms，4–7/24 在飞 | **26 PASS / 0 FAIL，suspend 1.4 ms，4/24 在飞** | `test/sm120_integration/evidence/app_l2_linux129.log` |
| 本地驱动/策略套件 | （Windows 版 `run_tests.ps1`） | **APP / HPF / CFS / GLB 全过**（PyTorch 未安装，跳过） | 脚本输出版本：`tests_local/run_tests.sh` |

关键机制结论与 Windows 一致：sm120 → `CudaQueueLv2` 分派成立；guardian 拼接、
入口退出（Deactivate）、resume 入口恢复（Reactivate）在 Linux 驱动上逐条成立；
exactly-once 语义、混合命令流（kernel/memcpy/memset）、多队列独立挂起均通过。
设备并发块数实测 ≈63（与 Windows 基线 ≈59 同量级——该配额并非 WDDM 独有）。
历史日志保留了修复前的失败样本（`mve_linux_quick*.log`、`app_l2_linux_run1.log`），
与修复后（`*_linux129.log`）形成对照。

## 3. 位级复现矩阵（Linux vs Windows 封存）

工具链同版本（nvcc 12.9.41 / cuobjdump 12.9.26 / nvdisasm 12.9.19）时，
**全部设备二进制产物逐字节一致**（证明生成链与宿主机 OS/编译器无关）：

| 产物 | 封存 SHA256（前 16 位） | Linux 复现 | 结论 |
|---|---|---|---|
| `platforms/cuda/hal/inject/inject_120.cubin` | `56aee7cb…` | 一致 | ✅ 位级 |
| `platforms/cuda/hal/inject/inject_86.cubin` | `6ec85962…` | 一致 | ✅ 位级 |
| `test/sm120_mve/mve_kernel.cubin` | `4220585a…` | 一致 | ✅ 位级 |
| `test/sm120_l2_gen/evidence/patched_120.cubin` | `509c816a…` | 一致 | ✅ 位级 |
| `test/sm120_l2_gen/evidence/patched_86.cubin` | `f3282d3b…` | 一致 | ✅ 位级 |

文本产物（`inject_120.asm`、`inject_120_cc.asm`、`inject_86.asm`、`sass.txt`、
`*.json`、`*.md`）内容一致，但**封存哈希是 Windows 运行时的 CRLF 行尾版本**，
而 git 提交后的工作树是 LF（提交时被规范化）；两种行尾都已在 Linux 侧验证：

```
inject_120.asm     : LF 形式 == 提交版；CRLF 形式 == 封存版 c76e3de8…
inject_120_cc.asm  : LF 形式 == 提交版；CRLF 形式 == 封存版 23211e9e…
inject_86.asm      : LF 形式 == 提交版；CRLF 形式 == 封存版 02f2d081…
mve_kernel.sass.txt: T5T6 封存=LF 版，T7 封存=CRLF 版（同一文件先后两版封存）
```

> 结论：旧 T3T4 清单的文本条目标注的是 CRLF 形式，在**新克隆上无法直接
> `sha256sum -c`**；§6 的 Linux 重封存改用工作树（LF）形式并写明该差异。
> `preview_sm120.cpp` 与 `gen_sm120_report.json` 在两个平台间仅差**内嵌的输入
> 绝对路径**（Windows 原路径 vs 复现机路径），语义一致；已保留原作者提交版本。

## 4. Linux 专有修复

### 4.1 生产代码：`platforms/cuda/hal/src/level2/mm.cpp` 进程退出断言

Windows 驱动在进程退出时让 `cuCtxGetCurrent` 返回 `CUDA_ERROR_DEINITIALIZED`
（代码已早退），Linux 驱动（595.99.02）返回 `CUDA_SUCCESS` + **空当前上下文**，
于是静态析构 `~InstrMemAllocator()` 的 `XASSERT(ctx == ctx_)` 失败：

```
Assertion failed: current context (nil) mismatch InstrMemAllocator context 0x...
```

修复：`~InstrMemAllocator()` / `~ResizableBuffer()` 在 `ctx == nullptr` 时同样
提前返回（上下文已销毁，其指令内存/映射由驱动随上下文回收）。
未列入任何封存清单，不影响既有证据。

### 4.2 Shim 加载方式：`LD_PRELOAD` 不足以拦截 cudart

Linux 上 cudart **在运行时按名字 `dlopen("libcuda.so.1")`** 再解析驱动符号；
`LD_PRELOAD` 只影响可执行文件自身的符号引用，拦不住 cudart。症状极隐蔽：
XQueue 创建成功、`CudaQueueLv2` 分派“通过”，但 **LaunchWorker 从未发射任何命令**
（`cuLaunchKernel` 全走直通），于是 suspend 退化为整条流水线排空
（101.2 ms、24/24 完成，而非 0<done<n）。

正确做法（与上游 `SHIM_SOFTLINK=ON` 设计一致）：让 `libcuda.so.1` **这个文件名**
在搜索路径上解析到 shim，由 shim 内部按 `XSCHED_CUDA_LIB`/`CUXTRA_CUDA_LIB`
加载真驱动。本机仓库在 exFAT 上无法安装软链，因此运行脚本在 `TMPDIR` 下建立
私有链接农场（`ln -s`，失败则 `cp`），并把该目录前置到 `LD_LIBRARY_PATH`：

```
XSCHED_SHIM_DIR=${TMPDIR:-/tmp}/xsched-shim     # libcuda.so.1 -> output/lib/libshimcuda.so
LD_LIBRARY_PATH=$XSCHED_SHIM_DIR:$LD_LIBRARY_PATH
XSCHED_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1   # 必须指向真驱动，否则自递归
CUXTRA_CUDA_LIB=/usr/lib/x86_64-linux-gnu/libcuda.so.1
```

对比证据：`app_l2_linux_run1.log`（LD_PRELOAD，I2 失败：101.2ms / 24-24）
→ `app_l2_linux129.log`（链接农场，I2 通过：1.4ms / 4-24）。

### 4.3 测试装置的同步 API 排序竞态（MVE / 应用观测路径）

MVE 原用同步 `cuMemsetD8_v2` / `cuMemcpyHtoD_v2` 做“复位/置状态”。
按 CUDA 文档，同步调用**只与 legacy/阻塞流排序**，与 `CU_STREAM_NON_BLOCKING`
无关；本机驱动上该重排表现为：**复位的 memset 迟到数毫秒，在内核运行中把已写入的
完成标记/状态区擦掉**。MVE 的 `M0`（基线不完整）、preflight、`M4`（状态读回全零）
失败均由它引起；Windows 侧记录的“M4 WDDM 批次对齐”现象同源。

独立复现（纯驱动 API、无 XSched/cuxtra，2048 块 × 23ms 内核，8 轮）：
同步 memset + 非阻塞流 **7/8 轮丢 ~1788 个标记**；同一代码改阻塞流 **0/8**；
改流内 `cuMemsetD8Async` **0/8**；同步 memset 后加 50ms 主机睡眠 **0/8**（证明是迟到而非丢失）。

修复：`mve_main.cpp` 中所有“先置状态再发射”的写操作改为**流内 Async + 同步**
（与 HAL `InstrumentManager::Deactivate/Reactivate` 的既有写法一致）；
`M4` 的“在飞标志注入”刻意保留阻塞式 HtoD（其语义即“无序的主机写入”）。
复验：`mve_linux_quick.log`（修复前 46/1 FAIL）→ `mve_linux129.log`（48/0）。

### 4.4 构建与工具链

- `platforms/cuda/hal/inject/Makefile`：`clean` 目标原为 `cmd /c del`（Windows 专有），
  改为按 `OS` 条件选择 `del`/`rm -f`；`ARCH=86/120 bin dump cc` 与 `clean` 已在 Linux 实测。
- T7 / MVE 构建脚本：nvcc 12.x 不接受 g++ ≥ 15，自动改用 `g++-13`（`NVCC_CCBIN` 可覆盖）；
  并在 CUDA 头缺 §1.2 补丁时打印告警。
- T7 构建：XSched 管理 API 在 Linux 上分散于三个库（`CudaQueueCreate→libhalcuda`、
  `XQueue*→libpreempt`、驱动符号→`libshimcuda`），需显式 `-lshimcuda -lhalcuda -lpreempt`；
  框架用系统 g++ 15 构建（依赖 `CXXABI_1.3.15`），nvcc 需 g++-13 ⇒
  **分离编译与链接**：`nvcc -ccbin g++-13 -c` 得到 `.o`，再用系统 g++ 链接。

## 5. 新增/改动的 Linux 文件

| 文件 | 说明 |
|---|---|
| `test/cuda129_glibc241_compat.patch` | glibc ≥ 2.41 × CUDA ≤ 12.9 头文件兼容补丁（§1.2） |
| `tests_local/run_tests.sh` | `run_tests.ps1` 的 Linux 版（driver/APP/HPF/CFS/GLB/L2/pytorch） |
| `test/sm120_mve/build.sh` / `run_mve.sh` | MVE 的 Linux 版（gen/device/host；nvcc 源生支持 sm_120） |
| `test/sm120_integration/build.sh` / `run.sh` | T7 的 Linux 版（链接农场 + LD_LIBRARY_PATH） |
| `test/sm120_l2_gen/run_all_t3t4.sh` | T3/T4 一键复现的 Linux 版（同断言，含可复现性比对） |
| `test/sm120_*/seal_*.sh` | 与 `.ps1` 同清单、同顺序的 Linux 封存脚本（3 套） |
| `mve_main.cpp` | `dlopen/dlsym` 移植（原 `LoadLibraryA`）；流内有序状态写入（§4.3） |
| `app_l2.cu`、`tests_local/test_cuda_driver.cpp` | `windows.h` 改为平台守卫（无 Win32 API 依赖） |
| `t5_verify.py` | `inject_120.cubin` 缺失时 R1 优雅跳过（其余断言照跑） |

## 6. 封存与审计

Linux 复验后，**T3T4 / T5T6 / T7 三套封存清单全部用 Linux 对等脚本重新生成**
（`seal_t3t4.sh` / `seal_t5t6.sh` / `seal_t7.sh`，与 `.ps1` 同清单、同顺序），
新增条目覆盖 Linux 脚本与 Linux 运行日志；`sha256sum -c` 全部通过。
核对方法：`cd <repo> && sha256sum -c test/sm120_*/evidence/SHA256SUMS_*.txt`。

注意：T3T4 清单中的 `platforms/cuda/hal/inject/inject_{86,120}.cubin` 是**本地构建
产物**（被 `inject/.gitignore` 有意排除），全新克隆需先
`cd platforms/cuda/hal/inject && make ARCH=86 bin && make ARCH=120 bin` 再校验
（本机复现的两个 cubin 与旧封存哈希逐字节一致，见 §3）。

两个被封印文件在本次适配中被修改（原因见 §4.3/§4.4），改前哈希如下
（可从提交 `0de4785` 检出）：

| 文件 | 改前 SHA256（Windows 封存版） |
|---|---|
| `test/sm120_mve/mve_main.cpp` | `7ef3101ecc5b290fe43ab5f791df1b61d6d13f9ec5b72fadfca5bdbbf06a51e8` |
| `test/sm120_integration/app_l2.cu` | `8f3dc7b411a313f3cd22570abd98f5a3328924425c3e3c231d69840205f49ec2` |

生产代码改动（`platforms/cuda/hal/src/level2/mm.cpp` §4.1、`inject/Makefile` §4.4）
不在任何封存清单内，不影响既有封存。

提示：`.gitignore` 全局忽略 `*.log`，本分支的证据日志需 `git add -f` 才能纳入提交；
Linux 测试二进制已在 `.gitignore` 中显式忽略。

## 7. 复现命令（Linux，CUDA 12.9）

```bash
export PATH="$HOME/cuda-12.9/bin:$PATH"        # §1.1 安装 + §1.2 补丁

# 0) 构建 XSched（exFAT 上软链安装会失败，属预期；运行脚本自行建链接农场）
make cuda

# 1) T3 编译基线 + T4 金标准回归（预期 9/9 PASS + 产物可复现，exit 0）
cd platforms/cuda/hal/inject && make ARCH=120 bin && make ARCH=86 bin && cd -
test/sm120_l2_gen/run_all_t3t4.sh

# 2) T5 数组审查（预期 22/22 PASS；R1 依赖 inject_120.cubin）
test/sm120_mve/build.sh gen && python3 test/sm120_mve/t5_verify.py

# 3) T6 MVE 真机闭环（预期 48 PASS / 0 FAIL，含 1000 轮压力）
cd test/sm120_mve && ./build.sh all && ./run_mve.sh --tag linux129

# 4) T7 全栈集成（预期 26 PASS / 0 FAIL）
cd ../sm120_integration && ./build.sh && ./run.sh --tag linux129

# 5) 本地驱动/策略套件
cd ../../tests_local && ./run_tests.sh all
```

## 8. 环境注意事项（影响读数可信度）

- **外部 GPU 负载**：本机为桌面机（gnome-shell/Xwayland/浏览器常驻）；同机可能同时有
  其它会话在跑 GPU 测试。MVE 的 preflight 传感器即为识别此情形而设，重负载告警时
  应先确认空闲再判读。注意这与 §4.3 的复位竞态是**两个不同现象**：前者随负载波动、
  后者可确定性复现且已修复。
- **CUDA Graph**：L2 不插桩 graph 内部 kernel，代码内 `XWARN` 并回退 level-1（与 Windows 一致）。
- **性能口径**：本机为消费级 GB206 + 桌面驱动，并发块数与 Windows 基线同量级（≈60），
  不等于 TCC/数据中心卡的调度口径。
