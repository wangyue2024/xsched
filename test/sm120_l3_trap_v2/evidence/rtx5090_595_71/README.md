# RTX 5090 / 595.71.05 实测归档（2026-10-26/27）

> GPU：RTX 5090，compute cap 12.0（sm_120），32 GB
> 驱动：595.71.05（GSP 595.71.05，open kernel module）
> 环境：Ubuntu 22.04 容器，CUDA runtime 13.2 / nvcc 12.8，g++ 11.4
> 容器 caps：无 `cap_sys_ptrace` / `cap_sys_module` / `cap_sys_admin`

## 结论

| 项 | 结果 |
|---|---|
| `0x20800122` reg-op 锁（`pGpu+0x911`） | **关闭**：PTIMER/PMC_BOOT_0 读 0，`0x419e84` 写 status 0x3a、无副作用 |
| sm_120 基础 | 正常（2.407 GHz spin） |
| GDB attach/detach | 可用；目标需 `PR_SET_PTRACER_ANY`（容器无 CAP_SYS_PTRACE）；3 s hold 冻结（wall +3.12 s） |
| 活体 handler | 与 595.99 **完全一致**：slot20 0x880/0x940、tools image base+0x15f5300/0x1300、do_pause 0/160 mismatch、live base+0x15f0f00 fingerprint MATCH、`+0x390` patch 回读一致 |
| 0317 签名 | `0x5aa5/0 reason=0x1`（plain context，内核继续） |
| kill 流程（8 s / 20 s victim） | victim 都在 **~3.4 s** 提前结束（8 s→3.395 s；20 s→3.426 s），但签名 0/0（GUARD/EXIT 桩）；agent `devflag arm=0xffffffff`、报 PARKED、release 失败；**5060 的 0.234 s 未复现** |
| pbuf 重定位 | trap 流程报 `base−0x6000000`；kill 流程报 `base−0x8000000`（均 `memobj=1`, size `0x41c54e0`）⇒ 会话/模式相关，需按状态重读 |

## 文件

| 路径 | 内容 |
|---|---|
| `campaign/env.txt` | 驱动/GSP/CUDA/容器 caps/libcuda+libcudadebugger md5 |
| `campaign/regops_read.log` / `regops_write.log` | 锁测试（读 PMT/PTIMER；写 0x419e84） |
| `campaign/spin_gdb.log` / `gdb.log` | GDB 3 s hold（counter 2.837M→2.907M，wall +3.12 s） |
| `campaign/target_trap.log` / `agent_trap.log` | 0317 签名复测（0x5aa5/0x1，victim 12.5 s） |
| `campaign/target_kill20.log` / `agent_kill20.log` | 20 s victim kill 流程（3.426 s 提前结束、devflag/PARKED） |
| `spin.log` / `gdb.log`、`spinA/spin2`、`target/agent`、`target_kill/agent_kill` | 早期同轮日志 |
| `drivers/libcuda.so.595.71.05` | 88 MB，md5 `b0e0d60418a7f4b4f292233ab4bd86bd` |
| `drivers/libcudadebugger.so.595.71.05` | 12 MB，md5 `cab0518793c73f42a4686cc9597ee3ca` |
| `bin/regops`、`spin5090`、`l3x`、`l3agent` | 在 5090 容器（glibc 2.35）上编译的探针 |

> `drivers/` 与 `bin/` 只保存在仓库 evidence 中，**不随 `sm120_L3_report.zip` 附带**（体积原因）。

## 复现

```bash
# 5090 容器内
g++ -O2 -std=c++17 -o regops regops.cpp -I/usr/local/cuda/include -lcuda -ldl -lpthread
./regops            # 读 PMT/PTIMER
./regops --write    # 写 0x419e84 bit31
nvcc -arch=sm_120 -o spin5090 spin5090.cu
nvcc -arch=sm_120 -ccbin g++ -o l3x l3x.cu -I3rdparty/cuxtra/include \
  /root/xs/3rdparty/cuxtra/lib/libcuxtra_linux_x86_64.a -lcuda -ldl -lpthread -lrt
g++ -O2 -std=c++17 -o l3agent l3agent.cpp -Ixsched/platforms/cuda/hal/include -lcuda -ldl -lpthread -lrt
```
