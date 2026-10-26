# 上游源码 / 构建资料（本地留存）

> 目的：服务器停租后仍能重建 A/B 环境。所有文件都在本目录。

## 1. 源码

| 文件 | 大小 | md5 | 内容 |
|---|---|---|---|
| `xsched-main-f49289f.tar.gz` | 16,488,899 | `82f48a8c6df311e42edce7df5044ed85` | 上游 `XpuOS/xsched` main（commit `f49289f`，2026-08-19）原样快照 |
| `xsched-ab-a100-sm80.tar.gz` | 20,132,100 | `82a5ac15d16c5664682564c6de8fd754` | 上游 + `3rdparty/cuxtra` + `3rdparty/ipc`（含 boost-ipc/tools）+ ipc CMake 测试关闭 + sm80 arch 补丁，**可直接构建** |
| `arch_sm80.patch` | 1,339 | `edf7832944ffe7598cb9d7676bb9165c` | `arch.cpp` 增加 sm80 → SM86/SM70 分支的实验补丁 |
| `ipc_disable_tests.patch` | 288 | `3609fe9fb7d18153499827aea1fd0ce7` | 把 ipc 的 `if (BUILD_TEST)` 改成 `if (FALSE)`（CMake 3.22 缺 gtest 时用） |

上游：`https://github.com/XpuOS/xsched.git`，commit `f49289f0220931df78de948ed841ecbaf960a919`
（2026-08-19）。
cuxtra 子模块：`https://github.com/XpuOS/cuxtra.git`，`libcuxtra_linux_x86_64.a`
md5 `c91304f30894c5267d0e67737be70a95`（与本仓库 `3rdparty/cuxtra` 相同）。
ipc 子模块：`https://github.com/XpuOS/ipc.git`。

## 2. 参考源码（从上游拷出，便于直接查阅）

`sm86.cpp` / `sm86.h`：`TarpHandlerSM86` 的 guardian/resume/trap-inject SASS 与
`Instrument()`（patch 点 **handler+0x3e0**）。
`arch.cpp`：sm70/sm86 的 arch 选择；`interrupt.cpp`：`InstrumentTrapHandler` +
`cuXtraTriggerTrap`；`cuda_queue.cpp` / `cuda_queue.h`：`CudaQueueLv3Trap` 的
`Interrupt/Restore/OnPreemptLevelChange`；`options.cpp`：默认 L3 实现 = TRAP；
`level.cu`：`platforms/cuda/test` 的 L3 基准；`app_sched.cu`：手动调度示例（Lv2）。

## 3. 构建（CUDA 12.1 + g++11 + CMake 3.22；3090/A100 都验证过）

```bash
# 解压可构建快照
rm -rf /root/xsched-up
tar xzf xsched-ab-a100-sm80.tar.gz -C /root
cd /root/xsched-up

# sm86（3090）；A100 用 -DCMAKE_CUDA_FLAGS="-arch=sm_80 ..."
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/output \
  -DSHIM_SOFTLINK=ON -DBUILD_TEST=ON -DBUILD_SERVICE=OFF -DPLATFORM_CUDA=ON \
  -DCMAKE_CUDA_STANDARD=17 -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=g++ \
  -DCMAKE_CUDA_FLAGS="-arch=sm_86 -I/usr/local/cuda/include" \
  -DCMAKE_CXX_FLAGS="-I/usr/local/cuda/include" \
  -DCUDA_INCLUDE_DIRS=/usr/local/cuda/include \
  -DCUDA_LIBRARIES=/usr/local/cuda/lib64/libcudart.so
cmake --build build --target install -- -j32

LD_LIBRARY_PATH=$PWD/output/lib ./output/bin/cuda/level 3
```

也可直接运行本目录的 `repro_ab.sh sm_86`（或 `sm_80`）。

## 4. 预期结果

- **3090 sm86/570（带 shim）**：`level 3` preempt p50 **230–241 µs**、p99 0.42–2.8 ms、
  restore 93–127 µs；`level 2` 1.257 ms；正确性 200 次抢占后 `bad=0`。
- **3090 去 shim**：`output/lib/libcuda.so*` 改名后，L3 退化到 6–40 ms。
- **A100 sm80/550**：带 shim 在 `cuXtraKernelGetFunction` SIGSEGV（cuxtra 与 550 不兼容）；
  无 shim 只能跑驱动级慢路径 30–55 ms；`arch_sm80.patch` 不能解决 cuxtra 崩溃。
  因此 A100 不是论文快路径的复现平台。
- **sm120/595**：`pGpu+0x911` 关闭 regops，cuxtra 触发无法投递；只能 RM 0317（27 ms）
  或 GDB backend（116 ms）。

## 5. 证据

- 完整 battery：`../battery_3090/`
- 正确性：`../correct_sm86.log`
- handler 实物：`../sm86_handler.bin`、`../a100_handler.bin`
- 去 shim：`../level3_sm86_noshim.log`、`../level3_sm80_noshim.log`
- A100 失败：`../level3_sm80patch.log`、`../level3_sm80patch_gdb.log`、
  `../level3_tsg_a100*.log`
- cuda-gdb 路径：`../sm86_agent_ioctl_0317.log`、`../logs/a100_agent_ioctl_0317.log`
- 环境：`../logs/env_a100.txt`、`../logs/env_3090.txt`
- 总汇：`../../FINDINGS_ZH.md §49–§50`、`../../compendium/07-A-B-完整实测.md`
