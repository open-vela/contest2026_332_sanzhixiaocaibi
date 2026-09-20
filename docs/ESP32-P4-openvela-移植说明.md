# ESP32-P4 openvela 移植说明

> 作者：ty（成员 A）| 工具：Claude Desktop / Codex CLI
> 日期：2026-08 ~ 2026-09

## 一、任务概述

将 openvela（小米基于 Apache NuttX 的 RTOS）从零移植到 ESP32-P4 Function EV-Board（Rev1.3，双核 RISC-V HP+LP 400MHz）。

openvela 官方仅支持 ESP32-S3/C3/C6 等旧芯片，ESP32-P4 为全新芯片，需要从上游 apache/nuttx 搬运 BSP 并做大量适配。

## 二、完成的工作

### 2.1 编译适配（解决 20+ 编译错误）

- 升级 esp-hal-3rdparty（乐鑫 HAL 库）至支持 ESP32-P4 的版本
- 修复 HAL API 变更：中断函数改名、GPIO 函数改名、UART 时钟使能方式变更、串口信号定义变更
- 补齐 openvela 缺失的 Kconfig 配置项（芯片型号、CPU 频率、中断数量、日志等级等）
- 补齐缺失的头文件和板级文件（从上游 apache/nuttx 搬运）
- 解决 openvela 旧内核与新 HAL 的适配问题（中断架构从矩阵式到 handle 式的桥接）

### 2.2 启动调试（解决 10+ 运行时卡死）

| 问题 | 解法 |
|------|------|
| PMA region protection 卡死 | 关闭保护配置 |
| Flash 初始化卡死 | 标记为 RAM-only 镜像 |
| RNG 熵源卡死 | 跳过 |
| PVT 温度补偿卡死 | 关闭 |
| RTC 慢时钟校准卡死 | 跳过校准用固定频率 |
| BSS 未清零导致全局变量脏数据 | 修复 |
| UART 时钟源配置卡死（P4 共享寄存器） | 跳过 |
| procfs 文件系统挂载卡死 | 跳过 |
| board_late_initialize 不被调用 | 绕过直接调用 esp_bringup |
| esp_board_initialize ROM 符号冲突 | 条件编译绕过 |

### 2.3 最终效果

固件成功烧录，启动链路全部通过：

```
ROM bootloader → NuttX 内核 → 任务调度 → 内存管理 → 设备驱动初始化 → nsh 任务创建
```

串口日志验证：BI:0~BI:18 → ES:10a~ES:14g → NX:9 → BRD:10 全链路通过。

## 三、修改的文件清单

### 3.1 openvela 源码修改（约 20 个文件）

| 文件 | 修改内容 |
|------|----------|
| `arch/risc-v/Kconfig` | 补 ARCH_CHIP_ESP32P4 choice |
| `common/espressif/Kconfig` | 补 ESP32P4 配置项 |
| `common/espressif/esp_start.c` | 删旧芯片宏、补 esp_rtc_init、直接调 esp_bringup |
| `common/espressif/esp_lowputc.c/h` | txsig→UART_PERIPH_SIGNAL |
| `common/espressif/esp_serial.c` | PERIPH_RCC_ATOMIC、up_putc 改用 ets_printf |
| `common/espressif/esp_timerisr.c` | CHIP_SYSTIMER_SOURCE 条件宏 |
| `common/espressif/esp_irq.c/h` | 补 handle 架构 + up_irq_to_ndx |
| `common/espressif/esp_gpio.c` | GPIO 中断适配 |
| `include/nuttx/debug.h` | 从上游搬运 |
| `arch/risc-v/include/esp32p4/chip.h` | 从上游搬运 |
| `tools/espressif/Config.mk` | 恢复 --ram-only-header |

### 3.2 HAL 内部修改

| 文件 | 修改内容 |
|------|----------|
| `sdkconfig.h` | REGION_PROTECTION_ENABLE=0、PVT=0、RTC_CLK_CAL_CYCLES=0 |
| `esp_lowputc.c` | uart_ll_sclk_enable 适配 |
| `uart_ll.h` | uart_ll_set_sclk P4 共享寄存器处理 |

### 3.3 板级新增文件

```
arch/risc-v/src/esp32p4/          — P4 芯片层源码
arch/risc-v/include/esp32p4/      — P4 头文件
boards/risc-v/esp32p4/            — 板级 BSP
boards/risc-v/esp32p4/esp32p4-function-ev-board/  — EV-Board 适配
```

## 四、串口调试标记系统

为精确定位启动卡点，所有关键路径都加了 `ets_printf` 标记：

| 标记前缀 | 来源文件 | 含义 |
|----------|----------|------|
| BI:xxx | bootloader_esp32p4.c | ROM bootloader 阶段 |
| ES:xxx | esp_start.c / esp_serial.c | ESP 驱动初始化 |
| CLK/SLOW/RTC/PMU | 时钟初始化 | 各时钟域状态 |
| LP:xxx | esp_lowputc.c | 低级串口 |
| CP:xxx | GPIO 配置 | 引脚配置 |
| NX:xxx | nx_start / nx_bringup | 内核启动 |
| BRD:xxx | esp_bringup | 板级初始化 |
| LL:xxx | uart_ll.h | HAL 底层 |

## 五、编译与烧录

### 环境

- 工具链：riscv-none-elf-gcc 14.2.0（xPack）
- 系统：WSL Ubuntu 26.04
- 内存：~6.7-11GB，用 `-j2` 编译

### 编译

```bash
cd ~/nuttx
make distclean
./tools/configure.sh esp32p4-function-ev-board:nsh
make -j2
```

### 烧录

```bash
# WSL 生成 bin
esptool.py -c esp32p4 elf2image --ram-only-header -fs 16MB -fm dio -ff 80m -o nuttx.bin nuttx

# PowerShell 烧录（COM10 为 USB-Serial/JTAG）
python -m esptool --chip esp32p4 -p COM10 --baud 921600 write-flash 0x2000 nuttx.bin
```

### 串口监控

```powershell
Start-Sleep -Seconds 3
$port = New-Object System.IO.Ports.SerialPort COM10,115200,None,8,one
$port.ReadTimeout = 500
$port.Open()
Write-Host "按复位键..."
$deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline) {
    try { $data = $port.ReadExisting(); if ($data) { Write-Host -NoNewline $data } } catch {}
    Start-Sleep -Milliseconds 100
}
$port.Close()
```

## 六、已知限制与后续路线

### 当前卡点

- NuttX 控制台走 UART0 (GPIO37/38)，调试口走 USB-Serial/JTAG (COM10)，两条不同物理通道
- 需要 CH340/CP2102 USB-UART 转接模块接到 GPIO37/38 才能交互 nsh

### 后续路线

1. **串口通** → nsh 可交互
2. **恢复 procfs** → `ps` 等命令可用
3. **应用层** → SPI/ADC（电流采集）、GPIO（继电器控制）、I2S（音频）、WiFi/以太网、AI 推理（NCNN）、LVGL（触屏 UI）
4. **恢复跳过功能** → PMA、PVT、RTC 校准按需恢复

## 七、AI 辅助开发说明

整个移植过程使用 Claude Desktop 和 Codex CLI 辅助：

- **编译错误排查**：AI 分析编译日志，定位 API 变更并生成修复代码
- **启动卡死调试**：AI 分析串口日志，设计 ets_printf 标记方案，逐步缩小卡点范围
- **HAL 适配**：AI 对比新旧 HAL 头文件差异，生成桥接代码
- **文档生成**：交接文档、技术报告由 AI 辅助编写

AI Coding 日志见 `logs/sun123D/` 目录。
