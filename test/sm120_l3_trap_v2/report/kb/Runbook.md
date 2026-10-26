# Runbook：sm120 trap 的构建、运行、移植与故障排查

> 最后验证：2026-10-27　主环境：5060 / 595.99.02 / CUDA 12.9。
> 事实/地址/编码 → `ABI.md`；方法与证据 → `Methods.md`；结论/论点 → `../03`、`../04`、`../05`。
> 所有命令默认在 `test/sm120_l3_trap_v2/` 下执行；`HERE` 指该目录。

---

## 1. 环境矩阵

| 平台 | GPU | 驱动 | CUDA | 结果/备注 |
|---|---|---|---|---|
| **5060** | sm_120 | **595.99.02**（open module + GSP） | 12.9（`/home/wy/cuda-12.9`） | 主环境；regops 关；GDB/0317 可用 |
| **5090** | sm_120 | **595.71.05**（open + GSP） | runtime 13.2 / nvcc 12.8 | handler 布局一致；regops 关；GDB 需 `PR_SET_PTRACER`；managed kill 未复现 |
| **3090** | sm86 | 570.124.04 | 12.1 | 三段齐全；L3 p50 229 µs、bad=0 |
| **A100** | sm80 | 550.107.02 | — | regops 开；sm80 handler 缺；带 shim 崩在 `cuXtraKernelGetFunction` |

**驱动库 md5**（对照用）：

| 文件 | md5 |
|---|---|
| 595.99.02 `libcuda.so.595.99.02` | `7a0879a3d48b9a83464f372f0f58eed5` |
| 595.99.02 `libcudadebugger.so.595.99.02` | `d5b428e8a8064b9a3de81f69994c6332` |
| 595.71.05 `libcuda.so.595.71.05` | `b0e0d60418a7f4b4f292233ab4bd86bd` |
| 595.71.05 `libcudadebugger.so.595.71.05` | `cab0518793c73f42a4686cc9597ee3ca` |
| 570.124.04 `libcuda` / `libcudadebugger` | `2ca71a40…` / `a981f842…`（见 `evidence/ab_ampere/drivers/README.md`） |
| 550.107.02 `libcuda` / `libcudadebugger` | `b9efd423…` / `cb644e0d…`（同上） |

**容器能力**（5090 AutoDL）：无 `cap_sys_ptrace`、`cap_sys_module`、`cap_sys_admin`；
CUDA 进程需自己 `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` 才能被 cuda-gdb attach。

---

## 2. 构建

### 2.1 本机 XSched（5060）

```bash
cd /run/media/.../xsched   # 仓库根
cmake -S . -B /tmp/xsbuild -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TEST=ON -DPLATFORM_CUDA=ON -DBUILD_SERVICE=ON \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 -DCMAKE_C_COMPILER=gcc-13 \
  -DCMAKE_CUDA_COMPILER=/home/wy/cuda-12.9/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-13 \
  -DCMAKE_CUDA_FLAGS="-ccbin /usr/bin/g++-13"
cmake --build /tmp/xsbuild -j8
```

- GCC 15 超出 CUDA 12.9 支持范围，必须 `g++-13`；
- 依赖 `3rdparty/cuxtra`、`3rdparty/ipc`（boost-ipc/tools）；
- CMake 3.22 下 `3rdparty/ipc/CMakeLists.txt` 的 `if (BUILD_TEST)` 需临时改 `FALSE`（避免 gtest）。

### 2.2 探针（5060）

```bash
cd test/sm120_l3_trap_v2
./build.sh                 # 全部 probe
./build.sh l3x l3agent     # 指定
```

- 宿主探针用 `g++-13`；CUDA 探针用 `/home/wy/cuda-12.9/bin/nvcc -arch=sm_120 -ccbin g++-13`；
- `l3x`/`l3agent` 需 `-I 3rdparty/cuxtra/include` + `libcuxtra_linux_x86_64.a`。

### 2.3 5090 容器（CUDA 12.8 / g++11）

```bash
g++ -O2 -std=c++17 -o regops regops.cpp -I/usr/local/cuda/include -lcuda -ldl -lpthread
/usr/local/cuda-12.8/bin/nvcc -O2 -std=c++17 -arch=sm_120 -ccbin g++ -o spin5090 spin5090.cu
/usr/local/cuda-12.8/bin/nvcc -O2 -std=c++17 -arch=sm_120 -ccbin g++ -o l3x l3x.cu \
  -I3rdparty/cuxtra/include 3rdparty/cuxtra/lib/libcuxtra_linux_x86_64.a \
  -lcuda -ldl -lpthread -lrt
g++ -O2 -std=c++17 -o l3agent l3agent.cpp \
  -Ixsched/platforms/cuda/hal/include -lcuda -ldl -lpthread -lrt
```

### 2.4 3090 上游 A/B

- 上游 `XpuOS/xsched`（commit `f49289f`）；
- CUDA 12.1 / g++11 / CMake 3.22；
- 补 `3rdparty/cuxtra`、`3rdparty/ipc`；ipc 的 `if (BUILD_TEST)` 改 FALSE；
- 完整源码/补丁：`evidence/ab_ampere/upstream/`、`repro_ab.sh`、`UPSTREAM_INFO.md`。

---

## 3. 运行

### 3.1 GDB backend（生产可用）

```bash
cd test/sm120_l3_trap_v2
./run_l3hal.sh          # baseline + suspend/resume，Resume=1
```

- HAL：`XSCHED_SM120_L3_BACKEND=GDB`；`GdbInterruptContext` + `l3ctl`；
- CLI：`xcli l3 -x <pid> -s/-q/-r`（suspend/query/resume）；
- 目标需 `PR_SET_PTRACER_ANY`（Yama/容器）；cuda-gdb 版本与驱动匹配；
- 期望：设备静止 **116 ms**（冷）/ **106 ms**（预热），detach 恢复 **8–10 ms**；
- 注意：每次抢占需 **detach + 新 attach**；`-exec-interrupt` 不冻结。

### 3.2 TRAP backend（实验）

```bash
./run_l3qspin.sh suspend   # 受管内核 27–30 ms 冻结（需 cuda-gdb ack）
./run_l3qspin.sh kill      # kill + 队列重放
./run_l3qspin.sh probe     # reason / c[0x170] 现场
./run_l3kill.sh            # standalone trap kill（8 s → 0.234 s）
./run_l3trap.sh            # 活体 handler 签名/注入
```

- 环境：`XSCHED_SM120_L3_BACKEND=TRAP`、`XSCHED_SM120_TRAP_SHM/AGENT`、`CUXTRA_CUDA_LIB`；
- `run_l3qspin` 依赖 cuda-gdb 提供 0317 应答；没有应答 0317 会阻塞；
- 期望：`trap reactivate: replaying the killed kernel`；`resident/转` 见日志。

### 3.3 探针

| 命令 | 用途 |
|---|---|
| `./bin/regops` | 读 PTIMER/PMC；写 `0x419e84`（gate 测试） |
| `./bin/regops --write` | 写 `0x419e84` bit31 |
| `CUXTRA_CUDA_LIB=... ./bin/find_live_handler --dry` | slot20/arena/reloc |
| `./bin/l3x --role target/agent --shm ...` | plain-context patch/0317 |
| `./bin/latency` + `tools/measure_latency.sh` | GDB 延迟分解 |
| `tools/handover_test.sh` | 冻结让出 GPU |

### 3.4 常用 env

`XSCHED_SM120_TRAP_CAVE`、`..._DUMP_READ`、`..._DUMP_RAW`、`..._DBGPARAM_PROBE`、
`..._ARM_PBUF_PROBE`、`..._ARM_SENTINEL`、`..._ARM_PBUF_WRITE`、`..._HOST_ARM`、
`L3AGENT_*`、`L3Q_*`、`CUXTRA_CUDA_LIB`。

---

## 4. 换驱动/换卡 checklist

按顺序跑；任一失败就停，不要继续信任 `ABI.md` 的地址。

1. `nvidia-smi --query-gpu=name,compute_cap,driver_version --format=csv`；
2. `find_live_handler --dry`：确认 slot20 `0x1300`、`stub 0x880`、`base+0x15f5300`、
   reloc `trap_reason_table`；
3. 比对 live 入口 10 条指纹（`ABI.md` §2.1）；
4. `regops`：读 PTIMER/PMC；判断 `0x911`；
5. 0317 四态：普通 / 受管 / `NO_ICTX` / 预 attach；
6. 读 `do_read+0x120/0x130` 的 pbuf 地址（不要用旧偏移）；
7. `run_l3kill` / `run_l3qspin`：kill/replay；
8. GDB attach：目标 `PR_SET_PTRACER_ANY`；测冻结/恢复。

---

## 5. 故障树（症状 → 原因 → 检查 → 修复）

| ID | 症状 | 可能原因 | 检查 | 修复 |
|---|---|---|---|---|
| **F-01** | `cuda error 700` | cave 越界/ST.E 编码/R1/pbuf 写 | 回看 `P-01..P-09` | 修正编码/寄存器 |
| **F-02** | `715/719` | launch trap / handler payload | reason、`+0x880`/`+0x390` | 用 benign stub/NO_PAYLOAD |
| **F-03** | `status=0x3a` 且 value 0 | regops gate（0x911=0） | `regops` 读 PTIMER | 不能用户态绕过 |
| **F-04** | `status=0x56` | 会话 regops gate | 同上 | 同上 |
| **F-05** | `0317` 阻塞 | 无应答者 | 是否有 cuda-gdb/agent | 提供应答或接受阻塞 |
| **F-06** | `0318` 阻塞 | 调试器 suspend 状态 | app/agent 两个 client | 无用户态释放；stop-only |
| **F-07** | pbuf 全 0 | 驱动未写/路径不对 | `0317` 陷阱读 `+0/+4/+8` | 不是通道 |
| **F-08** | `devflag arm=0xffffffff` | 5090 pbuf/devflag 未校准 | target/agent 日志 | 重读 pbuf/时序 |
| **F-09** | launch 时 700 | 预置读 pbuf 的 cave | 日志在 launch | 0317 后再读/门控 |
| **F-10** | A100 带 shim SIGSEGV | `cuXtraKernelGetFunction`/550 | gdb backtrace | 不带 shim/换平台 |
| **F-11** | cuda-gdb `ptrace: Operation not permitted` | 容器无 CAP_SYS_PTRACE/Yama | `capsh --print` | 目标 `PR_SET_PTRACER_ANY` |
| **F-12** | 5090 managed kill 3.4 s | pbuf/devflag 校准 | campaign 日志 | 重读地址/时序 |

---

## 6. 资产地图

| ID | 资产 | 路径 | 备注 |
|---|---|---|---|
| **A-01** | 文档/ABI | `report/`、`report/kb/` | 本文所在 |
| **A-02** | 原始日志 | `evidence/live/`（309 个） | 现场 |
| **A-03** | A/B 证据 | `evidence/ab_ampere/`（52 个） | 3090/A100 |
| **A-04** | 5090 归档 | `evidence/rtx5090_595_71/` | campaign/drivers/bin |
| **A-05** | 595.71 驱动 | `evidence/rtx5090_595_71/drivers/` | 88 + 12 MB |
| **A-06** | 550/570 驱动 | `evidence/ab_ampere/drivers/` | 见 README/md5 |
| **A-07** | 上游快照 | `evidence/ab_ampere/upstream/` | tar + 补丁 + repro |
| **A-08** | handler bin | `evidence/ab_ampere/sm86_handler.bin`、`a100_handler.bin` | 实物 |
| **A-09** | 报告包 | `sm120_L3_report.zip` | 文档/日志/代码 |
| **A-10** | 构建树 | `/tmp/xsbuild` | 重建即可 |

---

## 7. 安全与授权边界

- **不要**在共用显示的 5060 上做 BAR0 直写、`rmmod`/重载驱动、未知寄存器写；
- 开闸/驱动 patch 只在**专用机器 + 快照/恢复**上做；
- AutoDL 容器无 `cap_sys_module`，不能重载宿主驱动；
- 不使能内核漏洞/越权；只在驱动配置/签名范围内实验；
- 驱动库/二进制仅研究留存，不分发；
- 任何"停住/恢复"结论必须有三判据（见 `Methods.md` §1.2）。

---

## 8. 机器索引（关键词 → 章节）

| 关键词 | 去 |
|---|---|
| build / cmake / g++-13 / cuxtra/ipc | §2 |
| GDB / run_l3hal / l3ctl / xcli | §3.1 |
| 0317 / run_l3qspin / run_l3kill | §3.2 |
| regops / find_live_handler / l3x | §3.3 |
| 换驱动 / 换卡 / 指纹 / PTIMER | §4 |
| 700 / 715 / 719 / 0x3a / 0x56 / 0xffffffff | §5 |
| 资产 / md5 / 驱动库 / 上游 | §6 |
| BAR0 / rmmod / 共用显示 | §7 |
