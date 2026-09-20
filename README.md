# 电脉卫士 Electric Guard P4

> **2026 首届 openvela AI 硬件开发者大赛** · 队伍 `contest2026_332_sanzhixiaocaibi`（三只小菜鸡）
> 选题方向：**新硬件平台适配 + AI 硬件产品创新**
> 目标硬件：ESP32-P4-Function-EV-Board（大赛官方「待适配开发板」）

> ### 👉 接手开发前请先读 [`docs/交接说明-20260920.md`](docs/交接说明-20260920.md)
>
> 那份文档记录**当前真实状态**：哪些做完了、哪些是坏的、以及几条已被证伪的
> 错误判断。其中「不要重复踩的坑」一节能直接省掉几小时。本 README 描述的是
> 设计与用法，交接说明描述的是**此刻的实际情况**，两者有出入时以交接说明为准。

---

## 一、作品简介

面向工业配电柜与楼宇配电箱的**端侧电气安全终端**。

传统空开与漏电保护器依据单一电气量越限动作，本质是"故障发生后的切断"。而电气火灾的典型路径是——接触不良/绝缘劣化 → 串联电弧 → 局部过热 → 绝缘阴燃 → 明火，其中电弧阶段电流可能**始终不超过额定值**，常规过流保护全程不动作。

电脉卫士的核心是**电流与视觉两条独立证据链的融合决策**：

- **电流侧**：ADS8688 以 3200Hz 同窗口扫描三相电压电流，规则判据识别电弧/过载/漏电/谐波/短路五类故障
- **视觉侧**：板载 SC2336 摄像头做火情感知，YCbCr 色彩判据 + 面积变化技术（AVT），单帧分析 2ms 量级
- **融合决策**：电流侧判出过载/电弧时，若视觉确认现场有明火，**自动将告警从 Lv1/Lv2 升级为 Lv3 并立即切断**

两条判据在物理机理上相互独立（一个测导体内的电学量，一个测空间中的光学量），同时成立时置信度显著高于任一单独判据，从而同时缓解"漏切"与"误切"。

**可视化**：LVGL 九页触屏 UI 中新增**视觉火情页**，实时显示火情与烟雾两组判据分量（得分 / 面积占比 / 闪烁 CV / 增长趋势 / 帧率），得分超阈自动转红，兼作现场标定工具。

**真机实测**：静置 35 秒零误报；明火触发面积占比 1.58%、闪烁 CV 1.26、得分 1.00；交叉验证实测将过载 Lv1（置信度 0.75）升级为 Lv3（置信度 0.94）并触发继电器切断。原始串口日志见 [`app/electric_guard_vision/evidence/`](app/electric_guard_vision/evidence/)。

---

## 二、系统构成（两个固件，各司其职）

| 固件 | 运行平台 | 承载内容 | 状态 |
|---|---|---|---|
| **平台固件** | **openvela (NuttX)** | ESP32-P4 平台移植：启动引导、时钟/PMU、UART0 控制台、NuttShell、调度器、内存管理、GPIO 切断 | 启动与 NSH 已验证，压测 20/20 |
| **应用固件** | ESP-IDF v5.5.4 | 三相采集、五类判别、视觉火情、LVGL UI、四路上云 | 全部已验证 |

### 为什么是两个固件（如实说明）

应用层的图形与多媒体能力目前运行在 ESP-IDF 上，**原因是上游能力空白，而非取舍**：

- openvela/NuttX 的乐鑫驱动层（`arch/risc-v/src/common/espressif/`，共 **79 个文件**）中**不存在任何 MIPI-CSI / ISP / MIPI-DSI / LCD 驱动**
- NuttX 的 V4L2 框架完整，但无乐鑫侧绑定实现
- ESP32-P4 本身无 WiFi 射频，网络依赖板载 ESP32-C6 经 esp-hosted/SDIO，NuttX 侧无 esp-hosted host 驱动，`CONFIG_NET` 亦未启用

在赛程内从零实现 CSI 控制器 + ISP 流水线 + DSI 面板驱动，工作量高于已完成的整个平台移植。该项已列入入围后修正期的工作计划，详见技术报告 3.7。

---

## 三、目录结构

```text
contest2026_332_sanzhixiaocaibi/
├── app/electric_guard_vision/        视觉火情感知（成员 C）
│   ├── components/vision/            核心组件 vision.c(651行) / vision.h
│   └── evidence/                     真机实测串口日志（量化指标佐证）
├── board/contest_board/              openvela 板级适配
│   └── docs/                         移植文档、改动日志、系统分析、注意事项
├── nuttx-overlay/                    openvela 内核侧改动（按源码树结构镜像）
│   ├── sched/init/nx_start.c         BSS 段清零【关键修复】
│   ├── sched/group/group_create.c    group 结构体初始化
│   └── README.md                     改动说明与移植成果
├── patches/
│   ├── esp32p4-openvela-bsp.diff     完整移植补丁（53 个文件）
│   └── esp32p4-new-files.tar.gz      P4 芯片层与板级新增文件（210 项）
├── skills/esp32p4-vision-tuning/     自定义 Skill（大赛硬性要求）
├── docs/技术报告初稿.md               技术报告（模板 3.1–3.7）
└── logs/Luohongbing83/               AI Coding 日志（858 事件，官方校验 ALL OK）
```

> 应用固件完整工程（`ws-gw`）体积较大（含 `managed_components`），未纳入本仓。
> 视觉组件源码在 `app/electric_guard_vision/components/vision/`，集成方式见下文第五节。

---

## 四、运行方式 A：应用固件（ESP-IDF）

### 4.1 环境要求

| 项 | 要求 | 说明 |
|---|---|---|
| ESP-IDF | **>= 5.5.3**（实际使用 v5.5.4） | `main/idf_component.yml` 中声明 |
| 操作系统 | Windows / Linux / macOS | 本项目在 Windows 11 + PowerShell 下验证 |
| 磁盘 | 约 3 GB | 工程含 managed_components |

### 4.2 ⚠️ 最重要的一条：不要让 IDF 重新下载 managed_components

工程内的 `managed_components/espressif__esp_hosted/` 有一处**必须保留的本地补丁**：

```c
/* host/port/esp/freertos/include/port_esp_hosted_host_os.h */
#define HOSTED_MEM_ALIGNMENT_64     128      /* 原值 64 */
```

**原因**：ESP32-P4 的 PSRAM(L2) cache 行是 **128 字节**，而 SDMMC 的缓冲准入检查要求与之对齐。hosted 按 64 对齐 + 缓冲落在 PSRAM + WiFi STA 发送走 zerocopy 直通 → 约一半的包地址不合格 → `sdmmc_io_write_blocks` 返回 `0x102` → esp-hosted 判定不可恢复 → **整机重启**。

**丢失后的症状**：「WiFi 一连上、开始收发数据就重启」。

因此 `main/idf_component.yml` 里把 `esp_hosted` **精确锁定**到 `2.12.13`（不用通配符）。历史上 `2.12.12` 的同类补丁就是在通配符自动升级时被覆盖丢失的。

**实践要求**：
- 工程整包传递（含 `managed_components/`），不要只传源码让对方重新拉依赖
- 升级该组件时**必须同步重打此补丁**
- 建议先备份该文件：
  ```bash
  cp managed_components/espressif__esp_hosted/host/port/esp/freertos/include/port_esp_hosted_host_os.h ./PATCH-BACKUP.h
  ```

### 4.3 编译与烧录

```bash
# Windows：先激活 IDF 环境（示例路径，按实际安装位置调整）
& 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'

cd <工程目录>/ws-gw
idf.py set-target esp32p4      # 仅首次
idf.py build
```

烧录（`COM3` 换成你的实际端口）：

```bash
# 完整烧录（会覆盖 SPIFFS 中的历史事件日志）
idf.py -p COM3 flash

# 仅烧应用分区（保留 SPIFFS 数据，日常迭代用这个）
idf.py -p COM3 app-flash
```

**⚠️ 分区表匹配**：如果之前烧过别的工程（比如 esp_video 的示例），板上的分区表是那个工程的，此时 `app-flash` 会因偏移不匹配而导致**引导循环**（`invalid segment length` / `Factory app partition is not bootable`）。此时必须完整 `flash` 一次。

本工程分区表：

```
nvs       0x11000  0x6000
phy_init  0x17000  0x1000
factory   (auto)   9M        ← 应用分区
storage   (auto)   4M        ← SPIFFS
```

### 4.4 串口连接（第一次接板子必看）

ESP32-P4-Function-EV-Board 上有**两个外观相同的 USB-C 口**，功能完全不同：

| 口 | 驱动方 | 固件崩溃时的表现 |
|---|---|---|
| **USB-Serial/JTAG** | 芯片内部硬件 + ROM | 照常枚举为 `VID_303A`，永远能连 |
| USB 2.0 OTG | 应用固件 | 固件未实现 USB 设备类 → **设备管理器报错误 43** |

**插错口的症状**：设备管理器出现「未知 USB 设备（设备描述符请求失败）」，错误代码 43，拿不到 COM 口。**解决办法就是换另一个口**（通常在 BOOT/RST 按键那一侧）。

另外两点实测经验：

- **不要经过 USB 隔离器或扩展坞**。USB 隔离器普遍只支持 USB 1.1 全速且隔离侧供电很弱（一两百毫安），而本板带 7 寸屏，供电不足则芯片起不来、USB 无法枚举。调试阶段直连电脑；等接强电做联调时再加隔离，并给板子接**独立 5V 电源**，隔离器只走数据线。
- 用充电线（无数据线芯）也会表现为「完全认不到」。

**读串口**（PowerShell 原生，无需额外工具）：

```powershell
$port = New-Object System.IO.Ports.SerialPort COM3,115200,None,8,one
$port.Encoding = [System.Text.Encoding]::UTF8      # 中文日志不乱码
$port.DtrEnable = $false; $port.RtsEnable = $false  # 防止误触发复位
$port.Open()
$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline) {
    $d = $port.ReadExisting(); if ($d) { [void]$sb.Append($d) }
    Start-Sleep -Milliseconds 80
}
$port.Close(); $sb.ToString()
```

主动复位：`$port.RtsEnable=$true; Start-Sleep -Milliseconds 150; $port.RtsEnable=$false`

### 4.5 Demo 模式（无外设硬件时使用）

`main/Kconfig.projbuild` 提供 `CONFIG_ELECTRIC_GUARD_DEMO_MODE`：

```
CONFIG_ELECTRIC_GUARD_DEMO_MODE=y   # 零 GPIO/SPI/I2S 硬件接触
```

开启后：
- 电流/电压/THD/温度/电弧**全部模拟生成**
- 三场景循环：**过载冲击 ~25s → 电弧 ~48s → 谐波窗口 ~70s**
- GPIO 切断、SPI、I2S 全部跳过（烧录安全，不会误驱动引脚）
- **视觉功能不受影响，仍是真实检测**

用途：没接 ADS8688/继电器时，仍可完整演示五类故障判别、UI、事件日志，
并且可以验证「模拟过载 + 真实明火 → 交叉验证升 Lv3」这一核心场景。

接上真实硬件后置为 `n`。切换后需重新 `idf.py build`。

> 修改建议：用 `idf.py menuconfig` 勾选，或直接改 `sdkconfig` 后重新编译。
> 切换前先备份：`cp sdkconfig sdkconfig.realhw.bak`

---

## 五、运行方式 B：平台固件（openvela / NuttX）

### 5.1 环境要求

| 项 | 要求 |
|---|---|
| 操作系统 | Ubuntu（官方文档标注仅适配 22.04，**明确不支持 WSL / Docker**；本项目实际在 WSL Ubuntu 下完成，属非官方支持配置） |
| 工具链 | `riscv-none-elf-gcc` **14.2.0**（xPack） |
| 内存 | 建议 8GB 以上，编译用 `make -j2` 或 `-j4` |
| esptool | Python 包，用于生成与烧录镜像 |

### 5.2 源码与依赖

openvela 官方仓库 `boards/risc-v/` 下**只有 esp32c3/c6/h2，没有 ESP32-P4**，本项目的 P4 支持为自行移植。

源码树包含：
- 53 个文件的源码修改（见 `patches/esp32p4-openvela-bsp.diff`）
- 210 项新增的芯片层与板级文件（见 `patches/esp32p4-new-files.tar.gz`）
- `esp-hal-3rdparty`（乐鑫 HAL 库，commit 8d0a8989，手动放置非 git submodule）

### 5.3 ⚠️ 三条必须知道的禁忌

**禁忌一：绝对不要执行 `make distclean`**

会删除 `arch/risc-v/src/chip/esp-hal-3rdparty/`，其中的所有本地修改全部丢失：

| 文件 | 修改内容 | 丢失后果 |
|---|---|---|
| `nuttx/esp32p4/include/sdkconfig.h` | `REGION_PROTECTION_ENABLE=0`、`PVT=0`、`RTC_CLK_CAL_CYCLES=0`、`APP_BUILD_TYPE_RAM=1` | 启动卡死（PMA 区域保护 / PVT 温补 / RTC 校准） |
| `nuttx/src/platform/os.h` | `nxsched_usleep` → `nxsig_usleep` | 编译报函数未定义 |
| `nuttx/src/platform/os.c` | 补 `#include <fcntl.h>`、`nxtask_init` 改 posix_spawn 风格 | 同上 |

**务必先备份整个 HAL 目录再做任何清理操作。**

**禁忌二：Kconfig 用 Tab 缩进，不是空格**

openvela 的 Kconfig 文件用 Tab。脚本化修改时必须用 `\t`，否则解析失败。

**禁忌三：不要手改 `.config` 打开带 `select` 的选项**

例如 SPI：

```kconfig
config ESPRESSIF_SPI          # 无提示符，只能被 select
	bool
config ESPRESSIF_SPI_PERIPH   # 同上
	bool
	depends on ESPRESSIF_SPI2
config ESPRESSIF_SPI2
	bool "SPI 2"
	select ESPRESSIF_SPI       # ← 靠 select 带出依赖
	select ESPRESSIF_SPI_PERIPH
```

而 `Make.defs` 里 `esp_spi.c` 的编译条件依赖 `ESPRESSIF_SPI` + `ESPRESSIF_SPI_PERIPH` 同时为 y。
**手动加 `CONFIG_ESPRESSIF_SPI2=y` 不会触发 `select`** → 依赖项仍为 n → `esp_spi.c` 不参与编译 → 链接报一堆函数未定义。

**症状很坑：配置文件看起来完全正确，错误却指向"函数不存在"。**

正确做法：

```bash
kconfig-tweak --enable ESPRESSIF_SPI2
make olddefconfig                    # 让 Kconfig 重新解析依赖
grep -E "^CONFIG_(ESPRESSIF_SPI|ESPRESSIF_SPI_PERIPH|ESPRESSIF_SPI2|SPI)=y" .config   # 回读确认
```

### 5.4 编译与烧录

```bash
export PATH=$HOME/toolchain/riscv-none-elf-gcc/bin:$PATH
cd ~/nuttx
make -j4
```

生成镜像：

```bash
python3 -m esptool --chip esp32p4 elf2image \
    --ram-only-header -fs 16MB -fm dio -ff 80m -o nuttx.bin nuttx
```

烧录：

```bash
python -m esptool --chip esp32p4 -p COM5 write-flash --force 0x2000 nuttx.bin
```

**⚠️ 偏移必须是 `0x2000`，格式必须是 `--ram-only-header`。烧到 `0x0` 会导致循环复位。**

### 5.5 串口连接（两条通道，容易混淆）

openvela 固件的串口布局与应用固件不同：

| 通道 | 物理接口 | 用途 |
|---|---|---|
| USB-Serial/JTAG | 板载 USB-C（COM5） | 烧录；ROM 的调试输出 |
| **UART0（GPIO37=TX / GPIO38=RX）** | **需外接 CH340/CP2102 模块**（COM6） | **NuttShell 控制台** |

**NSH 的输入输出走 UART0，不走 USB-Serial/JTAG。** 只连板载 USB 口会看到启动日志但**无法与 NSH 交互**——这是移植过程中卡了很久的一关。

接线：

```
CH340 模块 TX  →  ESP32-P4 GPIO38 (UART0 RX)
CH340 模块 RX  →  ESP32-P4 GPIO37 (UART0 TX)
CH340 模块 GND →  GND
```

波特率 115200。接好后 NSH 提示符直接出现，无需改代码。

### 5.6 当前状态与已知限制

| 项 | 状态 |
|---|---|
| 系统启动 | ✅ 正常，约 12 秒 |
| UART0 控制台 / NSH 交互 | ✅ 正常 |
| 压力测试 | ✅ 20/20 通过 |
| 命令响应 | ✅ < 1 秒 |
| 已启用外设 | UART0、RTC |
| 未启用外设 | GPIO（进行中）、SPI、ADC、I2S、网络 |

**为启动而关闭的功能**（记入已知限制，非缺陷隐瞒）：

- PMA 区域保护（`REGION_PROTECTION_ENABLE=0`）
- PVT 温度补偿
- RTC 慢时钟校准（改用固定频率，**会影响时基精度，毫秒级指标需实测校核**）
- bootloader RNG 熵源
- procfs 挂载（`ps` / `free` 命令不可用）
- UART sclk 配置（P4 共享寄存器写入阻塞）

**根因修复记录**：ESP32-P4 的 ROM bootloader **不清零 NuttX 的 BSS 段**，导致静态变量上电即携带随机值，表现为「启动早期崩溃」「group 结构体初始化失败」「任务列表损坏」三个看似无关的故障。修复方式为在 `nx_start()` 入口显式清零：

```c
extern uint8_t _sbss[];
extern uint8_t _ebss[];
/* ESP32-P4: BSS may not be zeroed by bootloader, do it explicitly */
memset(_sbss, 0, (uintptr_t)_ebss - (uintptr_t)_sbss);
```

该问题具有通用性（不限于 ESP32-P4），计划按大赛要求 PR 至 `nuttx` 仓 `dev-ai-contest-2026` 分支。

---

## 六、视觉组件集成说明（给接手的队友）

### 6.1 组件位置与依赖

```
components/vision/
├── vision.c          651 行
├── vision.h           78 行
└── CMakeLists.txt

REQUIRES: app_core esp_video esp_timer driver
```

同时需要把 `esp_video` 自带的 `example_video_common` 组件复制到工程 `components/` 下（提供摄像头初始化），并在 `sdkconfig` 中加入：

```
CONFIG_EXAMPLE_SELECT_ESP32P4_FUNCTION_EV_BOARD_V1_5=y
CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR=y
CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_PORT=0
CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ=100000
CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER=y
CONFIG_CAMERA_SC2336=y
```

### 6.2 三处集成点

| 位置 | 内容 |
|---|---|
| `main/main.cpp` LVGL 初始化之后 | `vision_init()` |
| `main/main.cpp` `on_fault()` 开头 | 视觉交叉验证（升级 ARC/OVERLOAD 至 Lv3） |
| `main/main.cpp` acq 任务判别之后 | 火情/烟雾独立事件上报（边沿触发） |
| `components/app_core/app_core.h` | `FAULT_TYPE_FIRE = 7`、`FAULT_TYPE_SMOKE = 8` |
| `components/lvgl_ui/lvgl_ui.c` | 新增 `PG_VISION` 页（`build_vision()` / `refresh_vision()`，约 135 行），CMakeLists 加 `vision` 依赖 |

### 6.3 集成时踩过的四个坑（务必阅读）

**坑 1 — I2C 引脚层冲突（会导致 panic）**

SC2336(SCCB)、GT911(触摸)、ES8311(声卡) **共用同一对物理引脚 `SCL=GPIO8 / SDA=GPIO7`**。
BSP 已在 **I2C port 1** 建好总线，若让 `esp_video` 按示例默认在 **port 0** 再建一条，
两个 I2C 控制器同时驱动同一对引脚 → 摄像头自身初始化成功（PID 读到 `0xcb3a`），
但随后 ES8311 的 I2C 读写全部失败 → `esp_codec_dev_open` 崩溃 → 整机 panic 重启。

**端口号不同 ≠ 没有冲突。冲突发生在引脚层。**

解法：取 BSP 句柄、置 `init_sccb = false`：

```c
extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);

esp_video_init_csi_config_t csi_cfg = {
    .sccb_config = { .init_sccb = false,
                     .i2c_handle = bsp_i2c_get_handle(),
                     .freq = 100000 },
    .reset_pin = -1, .pwdn_pin = -1,
};
```

注意**不要** `#include "bsp/esp32_p4_function_ev_board.h"`——该头会连带引入
`display.h → esp_lcd_types.h`，而 `esp_lcd` 在 BSP 组件里是 `PRIV_REQUIRES`，
外部组件包含即编译失败。直接 `extern` 声明那一个函数原型即可。

**坑 2 — PSRAM 跨行访问代价被低估约 50 倍（会触发看门狗复位）**

帧缓冲在 PSRAM，按"隔 N 行取一行"抽样时每次跨行跳 `width×2×N` 字节，几乎每点都是 cache miss。
初版按 SRAM 速度估算为「每帧 2ms」，**真机实测 100–170ms**。

后果：视觉任务与采集任务同核争抢 → `task_wdt: acq (CPU 1)` → 整机自动复位。

**必须遵守的核间分配**（见队友的 `DUAL_CORE_MAP.md`）：

| 核 | 归属 |
|---|---|
| **Core1** | **acq 采集任务专属**（每 100ms 周期内忙等 40ms 保证 3200Hz 采样率） |
| Core0 | LVGL / 视觉 / 音频 / 网络 / 云端 |

优化手段（合计约 1/16 计算量）：抽样步长 8→16、隔帧分析、单遍统计（用上一帧均值）、
循环内显式 `vTaskDelay(pdMS_TO_TICKS(10))` 让出 CPU。

> 参考基准：乐鑫官方 `esp-dl/motion_detect` 在 P4 上 1280×720 stride=1 需 86.8ms，
> 640×360 stride=2 仅 6.9ms。**降分辨率比优化代码有效得多。**

**坑 3 — 新增组件导致 LVGL 被静默降级**

上游只把 lvgl 约束成 `">=8,<10"`，任何一次依赖重新解析都可能把版本解到别的小版本。
实测新增 vision 组件后 LVGL 从 9.5 掉到 9.4，PPA 绘制单元编译失败：

```
lv_draw_ppa_private.h: error: #error "PPA buffers need to be aligned to 64-byte boundary!"
lv_draw_ppa_private.h: fatal error: driver/ppa.h: No such file or directory
BUG: component_requirements.py: cannot match original component filename
```

**诊断方法**：改动前备份 `sdkconfig`，失败后 `diff sdkconfig.bak sdkconfig | grep -i lvgl`，
看到 `CONFIG_LVGL_VERSION_MINOR` 从 5 变 4 即确诊。

**解法**：在 `main/idf_component.yml` 显式锁死

```yaml
  lvgl/lvgl:
    version: "9.5.*"
    public: true
```

**坑 4 — 判据设计的三条原则（否则会误报/跳闸）**

1. **弱证据项必须相乘，不能相加。** 原实现 `0.6×面积 + 0.4×时域`，
   开机时 AE/AWB 收敛期面积仅 0.49% 但 CV 冲到 0.99，得分 0.59 > 阈值 0.55
   → **每次上电就误报并跳闸互锁**。改为 `面积 × (0.6 + 0.4×时域)` 后同样数据得 0.33，正确判为无火。
2. **确认与解除必须对称（迟滞）。** 原实现确认需 5 帧、解除仅需 1 帧，
   火焰自然闪烁导致得分反复穿越阈值 → 实测出现确认→解除→确认的连续跳变，
   每跳一次就是一次切断 + 事件入库 + 四路上云。现解除需连续 15 帧低于 `阈值×0.75`。
3. **传感器预热期不参与判定。** 前 45 帧（@30fps 约 1.5s）不出分；
   方差类指标要求至少 16 个样本，否则 4~5 个跳变样本就能算出 CV≈1.0 的假高值。

### 6.4 视觉 UI 页

`components/lvgl_ui/lvgl_ui.c` 中新增 `PG_VISION` 页，数据源为 `vision_get_stats()`。

```
视觉火情感知 · SC2336 端侧判据（YCbCr + 面积变化 AVT）
┌────────────────────────────────────────────────────┐
│ 感知状态   监视中·正常            帧 1234   30.0 fps │
└────────────────────────────────────────────────────┘
┌─ 火情 · 确认后升 Lv3 立即切断 ───────────────────────┐
│   综合得分      火焰面积占比 %      闪烁变异系数      │
└────────────────────────────────────────────────────┘
┌─ 烟雾 · 确认后报 Lv2 预警（不切断）──────────────────┐
│   综合得分      烟雾占比 %          增长趋势          │
└────────────────────────────────────────────────────┘
预警级辅助判据，与电流侧交叉验证；不替代 GB 4717 火灾自动报警系统
```

设计说明见技术报告 3.4.4。要点：火情与烟雾分列（告警语义不同）；
显示分量而非仅结论（便于现场读屏标定）；未就绪显示 `--` 而非 0.00。

添加新页的方法（供后续扩展参考）：
1. `enum { ... PG_VISION, PG_MAX }` 加枚举
2. 前向声明 `build_xxx()` / `refresh_xxx()`
3. `ensure_page_built()` 的 switch 加分支
4. `page_show()` 与 UI timer 两处 switch 加刷新调用
5. `CMakeLists.txt` 的 `REQUIRES` 加数据源组件

底部页码圆点用 `PG_MAX` 自动扩容，无需改动。

### 6.5 可调阈值

```c
vision_set_thresholds(score_th, area_ref, confirm_n);
/* 默认：score_th=0.55, area_ref=0.015(1.5%画面), confirm_n=5 */
```

现场标定建议：先加逐帧调试日志（每 30 帧输出面积/CV/得分），
先测静置基线确认零误报，再测真实目标，**依据实测值调阈值，不要凭经验设常数**。

---

## 七、AI Coding 使用说明

本作品全程使用 **Claude Code**（模型 `claude-opus-5`）协作开发。

| 指标 | 数据 |
|---|---|
| 会话跨度 | 2026-09-01 → 09-19（18 天） |
| 事件数 | 858（官方 `validate-log.py` 校验 `ALL OK`） |
| Token 总量 | 约 **1.99 亿**（含缓存读取；不含缓存读取为 492 万） |
| 新增 Skill | 1 个：[`skills/esp32p4-vision-tuning`](skills/esp32p4-vision-tuning/) |

完整对话日志见 [`logs/Luohongbing83/`](logs/Luohongbing83/)。

**AI 的实际价值主要在定位跨层缺陷**，而非写代码：I2C 引脚层冲突（摄像头正常但声卡崩溃，表象无关）、
LVGL 静默降级（报错指向 PPA 对齐，与改动无表面关联）、
以及在**没有编译环境**的条件下通过静态比对上游源码，预判出 openvela GPIO 驱动的链接错误。

**AI 也翻过车，一并记录**：性能估算偏差近 50 倍（估 2ms、实测 170ms）；
生成的代码违反了队友文档中「Core1 为采集任务专属」的既有约定，导致看门狗触发与整机复位。
**结论：所有性能数字以串口日志为准，不采信估算；集成前先读团队已有的设计文档。**

---

## 八、证据索引

| 主张 | 文件 |
|---|---|
| 摄像头出图、火焰检出、交叉验证、稳定性对比 | `app/electric_guard_vision/evidence/fire-detection-serial-log-20260919.md` |
| 已验证固件与烧录方式 | `app/electric_guard_vision/evidence/firmware-verified-0919.txt` |
| openvela 移植改动说明 | `nuttx-overlay/README.md`、`board/contest_board/docs/` |
| 完整移植补丁（53 文件） | `patches/esp32p4-openvela-bsp.diff` |
| 技术报告 | `docs/技术报告初稿.md` |

---

## 九、许可

Apache-2.0
