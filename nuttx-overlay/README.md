# nuttx-overlay — openvela 内核侧改动

本目录按 **openvela(NuttX) 源码树结构镜像存放**本项目对内核的改动，便于评审定位。

> 依据大赛规则，公共仓（`nuttx` 等）的改动**不在本仓直接 push**，
> 而是 fork 对应公共仓、以 PR 形式提交至 `dev-ai-contest-2026` 分支。
> 本目录仅作**存档与说明**，使适配工作可见、可复现，生产仓保持零改动。

---

## 目录对应关系

| 本目录路径 | 对应 openvela 源码路径 |
|---|---|
| `sched/init/nx_start.c` | `nuttx/sched/init/nx_start.c` |
| `sched/group/group_create.c` | `nuttx/sched/group/group_create.c` |

完整补丁另见 `../patches/esp32p4-openvela-bsp.diff`（53 个文件），
新增的芯片层与板级文件见 `../patches/esp32p4-new-files.tar.gz`（210 项）。

---

## 改动说明

### 1. `sched/init/nx_start.c` — BSS 段显式清零【关键修复】

```c
extern uint8_t _sbss[];
extern uint8_t _ebss[];

void nx_start(void)
{
  /* ESP32-P4: BSS may not be zeroed by bootloader, do it explicitly */

  memset(_sbss, 0, (uintptr_t)_ebss - (uintptr_t)_sbss);
  ...
```

**问题**：ESP32-P4 的 ROM bootloader **不清零 NuttX 的 BSS 段**，
导致所有静态变量上电即携带随机值。

**表现**（排查过程中观察到的多个看似无关的故障，实为同一根因）：
- 启动早期崩溃
- `group` 结构体初始化失败
- 任务列表（tasklist）数据损坏

**影响**：这是整个 ESP32-P4 移植中最关键的一处修复。
在定位到此问题之前，上述故障表现分散，极易被误判为多个独立缺陷。

### 2. `sched/group/group_create.c` — group 结构体初始化

在 `group_initialize()` 中补充 `memset` 清零，与上述 BSS 修复配合，
确保 group 结构体在使用前处于确定状态。

---

## 移植成果

ESP32-P4-Function-EV-Board 属于官方《支持的硬件平台》中的**待适配开发板**，
本项目完成其 openvela 适配，当前状态：

| 项 | 状态 |
|---|---|
| 系统启动 | 正常 |
| UART0 控制台 / NSH 交互 | **正常**（CH340 接 GPIO37/38） |
| 压力测试 | 20/20 通过 |
| 启动耗时 | 约 12 秒 |
| 命令响应 | < 1 秒 |
| 稳定性 | 连续运行无异常 |

已启用：UART0 控制台、NuttShell、调度器、内存管理
未启用：SPI、ADC、I2S、WiFi/网络、procfs

详细的编译、烧录、使用方法见 `../board/contest_board/docs/`。
