# kb：sm120 trap/driver 检索型知识库

> 最后验证：2026-10-27　主驱动：595.99.02（5060）；595.71.05（5090）见文内标注。
> 这里放**事实、方法、操作、故障**；**结论/论点**在 `report/03`、`04`、`05`。
> 原始日志在 `test/sm120_l3_trap_v2/evidence/`；本目录只做索引和提炼。
> 全局总索引/结论全表：`../00-总索引与结论全表.md`。

---

## 1. 这个目录是什么

| 文件 | 回答的问题 | 组织方式 |
|---|---|---|
| `ABI.md` | 地址/编码/命令/偏移是什么？ | 按对象分章（地址图、handler、SASS、pbuf、RM…） |
| `Methods.md` | 我们怎么知道？哪些结论可信？踩过哪些坑？ | 证据标准、方法目录、证据账本、自更正、坑清单 |
| `Runbook.md` | 怎么 build/run？报错怎么修？换驱动怎么验？ | 环境矩阵、构建、运行、移植、故障树、资产 |

**不要在这里找"论文为什么不可行""下一步做什么"**——去 `report/03`、`04`、`05`。

---

## 2. 先查哪里（问题 → 章节）

| 你可能在找 | 去哪 |
|---|---|
| `base` 怎么算？`live`/`do_read`/slot20 在哪？ | `ABI.md` §1 |
| reason 0x1/0x8/0x30 是什么？ | `ABI.md` §2.2 |
| cave 怎么写？SASS 编码？R1 为什么不能用？ | `ABI.md` §3 |
| `c[0x170..0x188]` 是什么？ | `ABI.md` §4 |
| 驱动 pbuf 布局/重定位/读写？ | `ABI.md` §5 |
| scratchpad 布局？为什么盲读 700？ | `ABI.md` §6 |
| trap 里能读/写什么？ | `ABI.md` §7 |
| 0x83de 命令表？0xd04？0x20800122？0x911？ | `ABI.md` §8 |
| cuda-gdb / NvDebugAgent / libcudadebugger？ | `ABI.md` §9 |
| 5060/5090/3090/A100 差异？ | `ABI.md` §10 |
| 700/715/719、0x3a、0x56、0xffffffff？ | `Runbook.md` §5 |
| 怎么 build？怎么跑 GDB/TRAP？ | `Runbook.md` §2–§3 |
| 换驱动/换卡先跑什么？ | `Runbook.md` §4 |
| 某个"结论"当时怎么测的？ | `Methods.md` §1–§3 |
| 旧结论被推翻过哪些？ | `Methods.md` §4 |
| 遇到奇怪现象先排什么？ | `Methods.md` §5、`Runbook.md` §5 |

---

## 3. 检索约定

### 3.1 ID 规则

| 前缀 | 含义 | 文件 |
|---|---|---|
| `ABI-*` | 地址/编码/命令事实 | `ABI.md` |
| `M-*` | 方法 | `Methods.md` |
| `E-*` | 证据账本条目 | `Methods.md` |
| `C-*` | 自更正 | `Methods.md` |
| `P-*` | 坑 | `Methods.md` |
| `R-*` | runbook 任务 | `Runbook.md` |
| `F-*` | 故障条目 | `Runbook.md` |
| `A-*` | 资产 | `Runbook.md` |

### 3.2 版本/日期

- 每条关键事实应带 `applies_to`（驱动/GPU）和 `last_verified`；
- 换驱动后先跑 `Runbook.md` §4 的 checklist，再信任本目录的地址/偏移。

### 3.3 命令行检索

```bash
cd test/sm120_l3_trap_v2/report/kb
grep -n "pbuf" *.md
grep -n "0317" *.md
grep -n "0x15f0f00" *.md
grep -n "700" Runbook.md
```

---

## 4. 文档路由

```
结论/论点        → report/03、04、05
事实/编码/ABI    → kb/ABI.md
方法/证据/自更正 → kb/Methods.md
构建/运行/故障   → kb/Runbook.md
原始日志         → test/sm120_l3_trap_v2/evidence/
```
