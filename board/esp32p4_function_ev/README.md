# ESP32-P4-Function-EV-Board 板级适配（openvela / NuttX）

本目录是**可直接浏览的板级源码**，对应 openvela 工程树中的
`nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/`。

ESP32-P4-Function-EV-Board 属于大赛《支持的硬件平台》中的**待适配开发板**，
此前 openvela 无任何支持。

> 同样的内容也以补丁形式保留在 `../../patches/`：
> `esp32p4-openvela-bsp.diff`（53 个文件的改动）与
> `esp32p4-new-files.tar.gz`（210 项新增文件，含 `arch/risc-v/{include,src}/esp32p4`）。
> **本目录只覆盖板级部分**，芯片层（`arch/`）因体量与归属仍以 tarball 形式提供。
>
> 按大赛规则，`nuttx` 公共仓的改动不在专属仓直接 push，而是 fork `nuttx`
> 以 PR 形式提交至 `dev-ai-contest-2026` 分支。本目录与 `patches/`
> 是**存档与可复现说明**，使适配工作可见、可审阅。

---

## 目录内容

```
CMakeLists.txt  Kconfig
configs/        35 个 defconfig（nsh / gpio / usbconsole / spi / twai / ... ）
                + electric_guard/  ← 本项目实际使用的完整配置
include/        board.h 等板级头
scripts/        链接脚本
src/            板级初始化与驱动注册
```

## 本项目在此基础上的改动

| 文件 | 改动 | 说明 |
|---|---|---|
| `src/esp32p4_bringup.c` | GPIO 上电自检 | 通过 VFS 打开三路 GPIO 字符设备并执行 `GPIOC_READ`。**只读不写**——`/dev/gpio0` 后接三相断闸继电器，开机不得主动拉高 |
| `src/esp32p4_guardd.c` | **新增** | 电脉卫士执行守护进程，作为 `CONFIG_INIT_ENTRYPOINT` 常驻，从控制台读单字符指令驱动 GPIO |
| `src/esp32p4_gpio.c` | 引脚改为 21/22 | `GPIO_OUT1=21`（光耦→SSR-40DA ×3 三相断闸）、`GPIO_OUT2=22`（蜂鸣器） |
| `src/Make.defs` | 纳入 `esp32p4_guardd.c` | |
| `include/board.h` | `BOARD_NGPIOINT 0` | 板级与驱动的 GPIO 中断 API 在 P4 上不匹配（P4 是 `GPIO_INTR0~3`，驱动按单一 `GPIO_INTR_SOURCE` 写），断闸是输出通路不依赖中断，本期先关 |
| `configs/electric_guard/defconfig` | **新增** | 本项目实测通过的完整配置：`DEV_GPIO=y`、`ESPRESSIF_USBSERIAL=y`（控制台迁至板载 USB-Serial/JTAG）、`INIT_ENTRYPOINT="guardd_main"` |

芯片层与内核侧的改动（含 `esp_start.c` 的启动死锁修复）见
`../../nuttx-overlay/` 与 `../../patches/openvela-p4-gpio-usbconsole.diff`。

## 实测结果

```
GPIO selftest: /dev/gpio0 OK  read=0  ioctl=0     GPIO21 三相断闸 SSR
GPIO selftest: /dev/gpio1 OK  read=0  ioctl=0     GPIO22 蜂鸣器
GPIO selftest: /dev/gpio2 OK  read=0  ioctl=0     RTC IO0

NuttShell (NSH)
nsh> uname -a
NuttX 0.0.0 322ad9f1-dirty Sep 20 2026 04:27:31 risc-v esp32p4-function-ev-board
```

`ioctl=0` 表明驱动注册、VFS 通路与 ioctl 全链路可用。完整串口日志见
`../../app/electric_guard_vision/evidence/openvela-gpio-selftest-20260920.txt`，
定位过程与已知缺陷见同目录 `openvela-gpio-adaptation-20260920.md`。

## 关键修复：`esp_bringup()` 早于 `nx_start()` 导致启动死锁

开启 `CONFIG_DEV_GPIO` 后系统在启动中途死锁，不复位、不报错。逐层插入 ROM 级
打印（`ets_printf`，不依赖 NuttX 控制台）缩小到：

```
BRD:40 gpio-init → G:0 reg-enter i=0 → R:3 register_driver-enter → 挂死
```

根因在 `arch/risc-v/src/common/espressif/esp_start.c`：`esp_bringup()` 被放在
`nx_start()` **之前**调用。彼时调度器尚未启动，`register_driver()` 取得
`g_inode_lock` 后无任何任务可释放它，永久阻塞。此前未暴露，是因为不开
`DEV_GPIO` 时 `esp_bringup()` 内没有任何触及 VFS 的调用。

修复：移除该提前调用，交由板级既有的 `board_late_initialize()` 在调度器就绪后
调用——即 NuttX 的标准时序。**任何在此板上新增字符设备驱动的人都会踩到这一条。**

## 已知缺陷（如实记录）

- **USB-Serial/JTAG 控制台接收侧不稳**：单字符间隔 ≥250 ms 输入正常回显，
  连续快写会堵住主机 OUT 端点；`ls` 这类多次小块输出的命令会挂死，
  `uname -a` 这类单次输出正常。未定位。
- **`guardd` 冷上电后启动失败**：卡在打开 `/dev/console`，与上一条疑似同源。
  `openvela-nsh-gpio.bin`（NSH + GPIO 自检）不受影响，可正常使用。
- `CONFIG_ESPRESSIF_GPIO_IRQ` 关闭（见上表 `board.h` 一行）。
