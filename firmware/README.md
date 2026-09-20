# 固件包

四个可直接烧录的固件。**同一块 ESP32-P4 一次只能跑一个**，且烧录地址重叠，
切换必须重烧。

烧录工具统一用这个（其它版本的 esptool 在 P4 上会 Guru Meditation）：

```
C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe -m esptool
```

---

## ESP-IDF 固件（应用侧：屏 / 摄像头 / 采集 / 云）

两个变体，差别在 `CONFIG_ELECTRIC_GUARD_DEMO_MODE`。**这不是取舍上的偷懒，
是板载内部 DMA 装不下**：WiFi/esp-hosted 协议栈要约 53 KB 内部 DMA，
ADS8688 的 SPI DMA + INMP441 的 I2S DMA 又要约 40 KB，总量不够（实测
开机约 87 KB）。二者同时开启时，`[UIS] 内部 DMA 仅剩 16128 B < 24576 B`
→ 代码主动放弃建链（防止整机 DMA 枯竭，该保护是正确的）。

| | `eg-real.bin` | `eg-demo.bin` |
|---|---|---|
| demo mode | 关 | 开 |
| 三相电流/电压 | **真实测量**（ADS8688） | 显示 `--`（不冒充模拟值） |
| 继电器 GPIO21 / 蜂鸣器 GPIO22 | **真实驱动** | 不配置 GPIO |
| 视觉火情/烟雾 | 实时出分 | 实时出分 |
| WiFi / 华为云 / BLE | 不可用（DMA 不足） | **可用** |
| 模拟波形制造的故障事件 | 无 | 有（ARC/OVERLOAD 会触发切断） |

```powershell
# 烧应用分区即可（bootloader/分区表已在板上，除非刚跑过 openvela）
python -m esptool --chip esp32p4 -p COM3 write_flash `
  --flash_mode dio --flash_freq 40m --flash_size 16MB `
  0x20000 eg-real.bin
```

从 openvela 切回来时，bootloader 和分区表被覆盖过，要补烧：

```powershell
python -m esptool --chip esp32p4 -p COM3 write_flash `
  --flash_mode dio --flash_freq 40m --flash_size 16MB `
  0x2000  bootloader.bin `
  0x10000 partition-table.bin `
  0x20000 eg-real.bin
```

### 使用要点

- **WiFi 不会自动连**。启动时刻意跳过 esp-hosted 建链（防 C6 缺席时的 SDIO
  重试风暴），必须在触摸屏「链路」页输入 SSID/密码后点「连接 WiFi」。
- **上电默认分闸**。`guard_control init ... breaker=OFF`，带下拉防自启。
  合闸：板上 **BOOT 键（GPIO35）短按**，或 UI 操作。
- **切断即互锁**。任何 Lv3 事件触发 `CUTOFF` 后进入 `GCS_LOCKED`，
  此后一切合闸请求被拒。解锁要去**「告警」页点「复位」**
  （`guard_control_ack_reset`），不是三相页的合闸。
- **视觉不参与跳闸**。`CONFIG_ELECTRIC_GUARD_VISION_TRIP` 本次构建为关：
  视觉照常采集、判据照常出分、UI 照常显示，但不升级故障等级、不产生
  独立 FIRE/SMOKE 事件。台架验证继电器时避免视觉误报反复互锁。

---

## openvela 固件（平台侧：GPIO 驱动适配）

| | `openvela-nsh-gpio.bin` | `openvela-guardd.bin` |
|---|---|---|
| 内容 | NSH + GPIO 上电自检 | guardd 执行守护进程（作为 init 入口） |
| 状态 | **可用，已实测** | ⚠️ **当前启动失败**，见交接说明 §3.2 |

```powershell
# 注意偏移是 0x2000（不是 0x20000），且不要加 --baud
python -m esptool --chip esp32p4 -p COM3 write_flash --force `
  --flash_size 16MB --flash_mode dio --flash_freq 80m `
  0x2000 openvela-nsh-gpio.bin
```

`openvela-nsh-gpio.bin` 上电输出（实测，证据见
`../app/electric_guard_vision/evidence/openvela-gpio-selftest-20260920.txt`）：

```
GPIO selftest: /dev/gpio0 OK  read=0  ioctl=0     GPIO21 三相断闸
GPIO selftest: /dev/gpio1 OK  read=0  ioctl=0     GPIO22 蜂鸣器
GPIO selftest: /dev/gpio2 OK  read=0  ioctl=0     RTC IO0
NuttShell (NSH)
nsh> uname -a
NuttX 0.0.0 322ad9f1-dirty Sep 20 2026 04:27:31 risc-v esp32p4-function-ev-board
```

NSH 交互要**逐字符输入、每字符间隔 ≥250 ms**（USB-Serial/JTAG 接收侧
已知缺陷）。`uname -a` 这类单次输出正常，`ls` 会挂死。
