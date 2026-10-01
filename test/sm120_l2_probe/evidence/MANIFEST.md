# T1/T2 证据封存清单（MANIFEST）

- 封存日期：2026-10-01
- 证据根目录：`test/sm120_l2_probe/evidence/`
- 哈希清单：`SHA256SUMS.txt`（由 `seal_evidence.ps1` 生成，sha256sum 兼容
  `<hash>  <repo-relative-path>` 格式；覆盖本目录全部文件 + 探针源码 +
  T2 inject 产物）
- 验证环境：见 `toolchain_versions.txt`
  - GPU：NVIDIA GeForce RTX 5060（sm_120，compute capability 12.0）
  - 驱动：610.88（`cuDriverGetVersion = 13030`，即 CUDA 13.3 级驱动，
    Microsoft 签名有效）
  - 工具链：nvcc 12.9.41 / cuobjdump 12.9.26 / nvdisasm 12.9.19 /
    g++ 14.2.0 (MinGW) / Python 3.11.9

> 方法论、假设表与证据引用关系见 `../VERIFICATION.md` 与
> `../ASSUMPTIONS.md`。本清单只负责“证据在哪里、是什么”。

---

## 一、决定性证据（T1 核心结论）

**核心结论：sm120 的 debugger-parameters 窗口位于 `c[0x0][0x170..0x18C]`
（28 字节），逐字段对应 sm86 的 `0x1880..0x189C` 布局。**

| 文件 | 内容 | 证明什么 |
|---|---|---|
| `main_std_window0x170.log` | T1 探针标准序列完整输出（第 1 次运行） | P0–P5 全部 `[PASS]`，`RESULT: 0 failed check(s)`；P1 用全新哨兵值（0xA1A2…/0xB1B2…/0xC1C2…/0xE1E2…）命中 0x170/0x178/0x180/0x188；负对照 0x168 无哨兵 |
| `main_std_window0x170_run2.log` | 同上（第 2 次运行，复现） | 25 项检查 0 失败，证明结论可复现 |
| `main_std_v2.log` | 探针早期运行（旧假设窗口 @0x1880） | P1 在 0x1880 处读到全 0 —— 旧假设被否定的过程证据 |
| `scan_v2/run_full_0x0.log` | 全常量 bank 穷举扫描第 0 批（0x0–0x1EF） | 4 个哨兵值出现在 0x170/0x178/0x180/0x188（定址证据本体） |
| `scan_v2/run_selfcheck_0x300.log` | LDC.64 协议自检批（0x300–0x4E8） | `0x380: 0000000b05c00000 <== == out pointer`；unwritten=0 → LDC.64/ST.E.64 编码与 127 槽协议在真实硬件上正确 |
| `p6_run1.log` | P6（dump 驱动 trap handler）尝试 | `cuXtraGetTrapHandlerInfo` 在 `trap.cpp:20` 以 `cuda error 101: invalid device ordinal` 中止 —— P6 路线受阻的完整证据 |

## 二、扫描证据（scan_v2/，v2 协议：127 槽 / LDC.64 / 62 点/批）

- 36 批 × 3 文件（`patch_*.log` + `run_*.log` + `cubin_*.cubin`）。
- 覆盖 `c[0x0][0x0 .. 0x43D0]`，采样粒度 8 字节、批内 gapless
  （批 k 基址 = k × 0x1F0）。
- `run_full_*.log` 内的 `[SCAN] RESULT-SCAN:` 汇总行给出每批
  `nonzero/matches/unwritten` 计数。
- 观察摘要：
  - 仅 3 批出现非零：`0x0`（17 个，含 4 个哨兵）、`0x1f0`（29 个，
    kernel 元数据区）、`selfcheck 0x300`（7 个）。
  - **35 批全零**（0x5d0 起）→ 0x5D0 之后无任何有效 cbank 数据。
  - 全部批次 `unwritten=0` → 每个采样槽都被硬件写入，协议无死角。
  - `matches=4` 只在 `0x0` 批出现：`0x170/0x178/0x180/0x188`。

## 三、扫描证据（scan/，v1 协议：15 槽 / LDC.32 / 6 点/批，历史）

- 27 文件（9 批）：`0x33c`、`low_912`(=0x390)、`low_1008`(=0x3F0)、
  `low_1104`(=0x450)、`winA_6208`(0x1840)…`winA_6592`(0x19C0)。
- 用途：v2 之前对 sm86 旧窗口邻域（0x1840–0x19C0）与低位零星点位的
  先行排除扫描，全部结果为零。**保留作为方法演进证据**，
  其结论已被 scan_v2 全扫取代。
- 命名注：无 `0x` 前缀的文件名中的数字为十进制文件名（历史遗留），
  如 `low_912` = 0x390。

## 四、侦察证据（recon/）

| 文件 | 内容 |
|---|---|
| `cuxtra_lib_full.dis.asm` | `libcuxtra_windows_amd64.a` 全量反汇编（2.69 MB，objdump），用于确认 cuxtra 底层为驱动内置 CUtools 接口族（EtblTrapHandler 等）并定位 `cuXtraGetTrapHandlerInfo` 调用链 |
| `nvcuda_blobs_search.log` | System32 驱动内嵌 SASS 模式搜索（无命中） |
| `testslocal_blobs_search.log` | `tests_local/nvcuda.dll`（XSched shim，31 MB）内嵌 BPT/EXIT 模式搜索（命中） |
| `test_blobs_search.log` | `test/nvcuda.dll` 同上 |
| `shim_blob_21b500.bin` | 从 shim 提取的 5120 字节 SASS blob（0x21B500–0x21C900 区） |

## 五、环境与工具链

| 文件 | 内容 |
|---|---|
| `toolchain_versions.txt` | nvcc / cuobjdump / nvdisasm / python / g++ 版本 + `nvidia-smi` GPU/驱动信息 |

## 六、产生证据的源码与 T2 产物（位置在仓库其他路径，哈希收录于 SHA256SUMS.txt）

| 路径 | 说明 |
|---|---|
| `test/sm120_l2_probe/probe_kernels.cu` | 127 槽 brkpt 占位内核（T1 探针设备端） |
| `test/sm120_l2_probe/probe_main.cpp` | T1 探针主机端（P0–P6 + `--scan` 模式） |
| `test/sm120_l2_probe/patch_cubin.py` | 静态补丁器（BPT.TRAP→LDC/LDC.64/ST.E 等长替换 + 回环校验） |
| `test/sm120_l2_probe/build.ps1` / `run_scan.ps1` / `run_full_scan.ps1` / `seal_evidence.ps1` | 构建 / 扫描 / 封存驱动脚本 |
| `platforms/cuda/hal/inject/Makefile`、`make_msvc.bat`（T2） | 现代化 inject 工具链（ARCH=120/86，bin/dump/cc 目标） |
| `platforms/cuda/hal/inject/inject_120.{cubin,asm,_cc.asm}`（T2） | sm120 inject 基线产物（26 个 BPT.TRAP 占位，与探针的占位模式逐字节同构：`0x000000040000795c` / `0x000fea0000300000`） |
| `platforms/cuda/hal/inject/inject_86.{cubin,asm,_cc.asm}`（T2） | sm86 同工具链对照产物 |
| `platforms/cuda/hal/inject/trap_handler_sm86.asm` | sm86 驱动 trap handler dump（参考材料，sm86 窗口 0x1880 的直接来源） |

## 七、复现指令

```powershell
cd test\sm120_l2_probe
powershell -ExecutionPolicy Bypass -File build.ps1 -Stage all
$env:CUXTRA_CUDA_LIB = 'C:\Windows\System32\nvcuda.dll'
python patch_cubin.py probe_kernels.cubin probe_kernels_patched.cubin
.\probe_main.exe probe_kernels_patched.cubin          # P0-P5 预期 0 failed
powershell -ExecutionPolicy Bypass -File run_full_scan.ps1   # 35 批全扫
powershell -ExecutionPolicy Bypass -File seal_evidence.ps1   # 重生成哈希
```

## 八、封存完整性

- `SHA256SUMS.txt` 覆盖：本目录全部文件（除自身）+ 上表第六节全部源码/产物。
- 校验方式：`Get-FileHash`（PowerShell）逐文件比对；
  任何文件被改动都会导致哈希不一致。
- 哈希生成时间：见文件内条目与提交历史。
