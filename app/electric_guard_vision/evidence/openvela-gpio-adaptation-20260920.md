# openvela（NuttX）ESP32-P4 GPIO 驱动适配 — 实测证据

- 日期：2026-09-20
- 硬件：ESP32-P4 Function EV Board，rev v3.2，MAC e8:f6:0a:e3:a9:58
- 固件：openvela/NuttX，`nuttx-gpio-usbconsole.bin`（248 KB，烧录偏移 0x2000）
- 工具链：riscv-none-elf-gcc 14.2.0 (xPack)，esptool 5.4.0 生成镜像 / 4.12.dev3 烧录
- 负责人：成员 C（罗宏滨 / Luohongbing83）

## 1. 做了什么

在 ty 交付的 ESP32-P4 BSP 基础上，把 **GPIO 字符设备驱动**在 openvela 上跑通，
并把 NSH 控制台从 UART0 迁到板载 USB-Serial/JTAG。

对应"电脉卫士"的硬件：

| 设备节点 | 引脚 | 用途 |
|---|---|---|
| `/dev/gpio0` | GPIO21 | 光耦 → SSR-40DA ×3，三相断闸 |
| `/dev/gpio1` | GPIO22 | 蜂鸣器 |
| `/dev/gpio2` | RTC IO0 | 低功耗域 IO |

## 2. 关键问题与定位过程

### 2.1 开 `CONFIG_DEV_GPIO` 后系统在启动中途死锁

现象：串口只打到 `BRD:40 gpio-init` 就没有下文，不复位、不报错、纯挂死。

逐层加 `ets_printf` 埋点缩小范围（ROM 级打印，不依赖 NuttX 控制台）：

```
BRD:40 gpio-init            <- 进入 esp_gpio_init()
G:0   reg-enter i=0         <- 第 0 个输出引脚
R:0   snprintf1-done name=gpio0
R:1   switch-done
R:2   snprintf2-done
R:3   register_driver-enter <- 卡死在这里
```

**根因**：`arch/risc-v/src/common/espressif/esp_start.c` 里，`esp_bringup()`
被放在 `nx_start()` **之前**调用（当时是为绕开 `esp_board_initialize()` 的
ROM 符号冲突）。那个时间点调度器还没起来，`register_driver()` 会去拿
`g_inode_lock`，而没有任何任务能释放它 → 永久阻塞。

之前没暴露，是因为不开 DEV_GPIO 时 `esp_bringup()` 里没有任何触碰 VFS 的调用
（procfs 挂载那段是被跳过的）。

**修复**：删掉 `nx_start()` 之前那次调用。板级本来就有
`board_late_initialize()`（`esp32p4_boot.c`），会在调度器起来之后调用
`esp_bringup()`——即 NuttX 的标准时序。

### 2.2 控制台需要外接 CH340

ty 的配置把 NSH 放在 UART0（GPIO37/38），必须外接 TTL 模块才能交互。
改为开 `CONFIG_ESPRESSIF_USBSERIAL`、关 `CONFIG_ESPRESSIF_UART0` /
`CONFIG_UART0_SERIAL_CONSOLE`、置 `CONFIG_OTHER_SERIAL_CONSOLE=y`，
参照板级自带的 `configs/usbconsole/defconfig`。

结果：一根 USB 线既烧录又进 NSH，不需要额外硬件。

## 3. 实测结果

上电自检通过 VFS 打开三个 GPIO 字符设备并执行 `GPIOC_READ`（**只读**——
`/dev/gpio0` 驱动的是三相断闸继电器，开机不能主动拉高）：

```
ES:14g handing over to nx_start
BRD:0 board_late_init
BRD:10 esp_bringup-enter
BRD:11 procfs-SKIPPED
BRD:12 procfs-done ret=0
GPIO selftest: /dev/gpio0 OK  read=0  ioctl=0
GPIO selftest: /dev/gpio1 OK  read=0  ioctl=0
GPIO selftest: /dev/gpio2 OK  read=0  ioctl=0
BRD:1 esp_bringup-done

NuttShell (NSH)
nsh>
```

`ioctl=0` 表示 `GPIOC_READ` 返回成功，即**驱动注册 + VFS 通路 + ioctl 全链路可用**。

NSH 交互确认：

```
nsh> uname -a
NuttX 0.0.0 322ad9f1-dirty Sep 20 2026 04:27:31 risc-v esp32p4-function-ev-board
```

完整启动日志见同目录 `openvela-gpio-selftest-20260920.txt`。

## 4. 已知限制（如实记录）

- **USB-Serial/JTAG 控制台接收侧不稳**：单字符间隔 ≥250 ms 输入正常回显，
  连续快速写入会把主机侧 OUT 端点堵住；`ls` 这类需要多次小块输出的命令会挂死，
  `uname -a` 这类单次输出正常。怀疑是 `esp_usbserial.c` 的 RX FIFO 排空与
  OUT 包确认时序问题，尚未定位到具体寄存器操作。**不影响 GPIO 驱动本身**——
  上电自检走的是内核 `file_open`/`file_ioctl`，与控制台无关。
- openvela 侧仍**没有** MIPI-CSI / MIPI-DSI / LCD 驱动，视觉与 UI 仍跑在
  ESP-IDF 固件上（双固件方案）。这一点在技术报告中如实说明。
- `CONFIG_ESPRESSIF_GPIO_IRQ` 仍关闭，`BOARD_NGPIOINT=0`：板级与驱动的中断
  API 在 P4 上不匹配（P4 是 `GPIO_INTR0~3`，驱动按单一 `GPIO_INTR_SOURCE` 写的）。
  断闸是输出通路，不依赖 GPIO 中断，故本期先关掉。

## 5. 复现步骤

```bash
# WSL Ubuntu
cd ~/nuttx
export PATH="$HOME/esptool-venv/bin:$HOME/toolchain/riscv-none-elf-gcc/bin:$PATH"
make -j4                      # 产出 nuttx / nuttx.bin
```

```powershell
# Windows，注意偏移是 0x2000 不是 0x20000；不要加 --baud（USB-JTAG 上会崩）
python -m esptool --chip esp32p4 -p COM3 write_flash --force `
  --flash_size 16MB --flash_mode dio --flash_freq 80m 0x2000 nuttx.bin
```

改动补丁：`patches/openvela-p4-gpio-usbconsole.diff`

---

# 追加：openvela 侧执行守护进程 guardd + 大模型断闸链路

## 6. guardd —— 板上执行端

在 openvela 上新增常驻守护进程 `guardd`，从 `/dev/console` 读单字符指令，
通过 `/dev/gpio0`（GPIO21，三相断闸 SSR）与 `/dev/gpio1`（GPIO22，蜂鸣器）
执行动作并回报。源码：`nuttx-overlay/boards/risc-v/esp32p4/
esp32p4-function-ev-board/src/esp32p4_guardd.c`。

实测（连续 3 次复位全部正常启动）：

```
GUARD READY cut=/dev/gpio0(GPIO21) buzzer=/dev/gpio1(GPIO22)
GUARD HELP c=cut r=restore b=beep s=status

s -> GUARD STATUS cut=0 buzzer=0
b -> GUARD BEEP
c -> GUARD CUT ret=0
s -> GUARD STATUS cut=1 buzzer=0     <- 断闸输出已置位，读回确认
r -> GUARD RESTORE ret=0
s -> GUARD STATUS cut=0 buzzer=0
```

## 7. 踩到的坑：新增 builtin 应用会让 NSH 启动挂死

最初把 guardd 做成 `apps/examples/guardd` 的 builtin 应用。编译、注册都正常
（`builtin_list.h` 里条目正确，`nm` 能看到 `guardd_main`），但**系统启动到
NSH 就停住**，无任何 panic。

逐层埋点定位到：

```
BRD:1 esp_bringup-done
NX:spawn ret=1          <- task_spawn 成功，pid=1
NSH:main-enter          <- nsh_main 进来了
NSH:before-init
NSH:init-done
NSH:before-consolemain  <- 挂在 nsh_consolemain() 里
```

把 guardd 从 builtin 注册表里**彻底**摘掉（删 `builtin/registry/guardd.*`、
重生成 `builtin_list.h`、从 `libapps.a` 里 `ar d` 掉目标文件）后 NSH 恢复正常。
注意：只改 `.config` 关掉 `CONFIG_EXAMPLES_GUARDD` **不够**——那张表不会
重新生成，会得到一个假的"对照组"，我们一开始就被这个误导过。

改成放在板级代码、直接作为 init 入口（`CONFIG_INIT_ENTRYPOINT=guardd_main`）
绕开了 apps/builtin 这条链路，但**同样挂死**在打开 `/dev/console` 的地方。

## 8. 一个尚未定位的时序问题（如实记录）

为了查是不是堆越界，在 `up_allocate_heap()` 里加了一句早期 ROM 打印：

```
HEAP: start=0x4ff7561c romres=0x4ffbfbb0 size=304532
```

堆有 297 KB 可用，**根本不紧张，排除了内存假设**。但加上这句打印之后，
挂死现象消失了，guardd 正常启动，连续 3 次复位稳定。

也就是说：在 `up_allocate_heap()` 中增加一句 `ets_printf` 是目前让系统正常
启动的**必要条件**，去掉它（其余代码完全相同）100% 复现挂死。这说明问题
是启动早期的时序/竞态，不是内存容量。**根因尚未定位**，当前固件里保留了
这句打印。这是一个已知的脆弱点，不应视为已修复。

## 9. 大模型断闸决策链路

判断端在 PC（`tools/guard_bridge.py`），执行端在 openvela。
openvela 侧联不了网的三个硬门槛见 `tools/README.md`。

安全设计：本地规则引擎先算基线，大模型只能升级不能降级；本地判定要断闸
就一定断，不管大模型说什么、也不管网络通不通。

实测（`--no-llm`，只跑本地规则，串口真实下发）：

| 事件 | 决策 | 板子回报 |
|---|---|---|
| OVERVOLT Lv1，视觉无异常 | NONE | — |
| OVERLOAD Lv2，烟雾 0.31 | ALARM | `GUARD BEEP` |
| ARC Lv2 + 视觉火情 0.71 | **CUT** | `GUARD CUT ret=0` |
| TEMP Lv1，断闸后回落 | NONE | — |

第三条是整套设计的核心：单看电气特征只是 Lv2（不断闸），单看视觉也可能
误报，**两路独立证据同时成立才升级到断闸**。

逐条决策记录见同目录 `llm-cutoff-bridge-20260920.jsonl`。

## 10. 大模型的不确定性与升级封顶（实测记录）

演示剧本第 2 条（TEMP，A 相端子 118℃、相邻两相 41/43℃、温差 75K 持续扩大、
电流 9.2A 未过载）设计用来检验语义理解相对阈值判据的增量：所有数值阈值
全部不命中，本地规则判 NONE。

**同一条事件、同一模型（deepseek-chat）、`temperature=0`，两次运行结论不同：**

```
第一次  大模型 -> ALARM (conf=0.82)
        "A相端子118℃且相间温差达75K并持续扩大，属严重局部过热隐患，
         但无电弧、无过载、无视觉火情证据，故先本地声光告警并加强监视，暂不断闸。"

第二次  大模型 -> CUT   (conf=0.82)
        "A相端子118℃且相间温差达75K并持续扩大，属严重局部过热，
         虽无电弧与明火，但已构成电气火灾高风险，应断闸处置。"
```

两次推理过程都正确，但结论落在不同的处置等级上，置信度完全相同。

**这直接暴露了原设计的缺陷**：最初只约束了"大模型不能降级"，没有约束
升级幅度。按那个设计，第二次运行会直接导致一次无故断闸——配电柜停电。

**修正**：升级幅度一次最多一级（`guard_bridge.py` 的 `handle()`）。

| 本地规则 | 模型建议 | 最终 | 说明 |
|---|---|---|---|
| NONE | CUT | ALARM | 封顶，模型建议记录在案但不执行 |
| ALARM | CUT | CUT | 本地已到告警，允许升到断闸 |

日志字段 `llm_escalated` / `escalation_capped` 记录每次的实际情况，
见同目录 `llm-cutoff-bridge-20260920.jsonl`。

**结论**：把大模型放进安全回路是可行的，但必须限幅。它的价值在于读懂
规则引擎不看的自由文本现场描述（本例中"不过载却局部高温"这个组合是
接触不良的典型特征），而不在于替代确定性的判据。
