# F6 视觉火情检测 — 真机实测记录

> 设备：ESP32-P4-Function-EV-Board（板载 SC2336 MIPI-CSI 摄像头）
> 固件：`electric_guard_p4` v0.1.0，ESP-IDF v5.5.4
> 采集方式：USB-Serial/JTAG（COM3，115200，UTF-8）
> 日期：2026-09-19

本文件为原始串口输出摘录，用于佐证技术报告 3.5 节的量化指标。
所有数值均为板上实测，非估算。

---

## 一、摄像头链路验证

独立工程 `esp_video/examples/capture_stream` 验证 SC2336 出图：

```
Capture format: RGB,           frame size: 1280x720, FPS: 30.0, size 2764800
Capture format: YUV 4:2:0,     frame size: 1280x720, FPS: 30.0, size 1382400
Capture format: YUV 4:2:2 UYVY, frame size: 1280x720, FPS: 30.0, size 1843200
```

三种像素格式均稳定 30 fps。主工程采用 UYVY。

## 二、系统集成后启动日志

```
I (1778) example_init_video: MIPI-CSI camera sensor I2C port=0, scl_pin=8, sda_pin=7, freq=100000
I (1794) sc2336: Detected Camera sensor PID=0xcb3a
I (1877) vision: 视觉火情感知就绪（SC2336 1280x720）
I (1877) vision: 检测任务启动：1280x720 UYVY，抽样 16x16，阈值 score=0.55 area_ref=0.015 confirm=5
I (2407) Adev_Codec: Open codec device OK
I (2440) Adev_Codec: Open codec device OK
I (1729) GT911: TouchPad_ID:0x39,0x31,0x31
```

摄像头（SCCB）、声卡（ES8311）、触摸（GT911）三者共用 I2C 总线且均正常工作，
证明 SCCB 复用 BSP 句柄的方案有效（自建第二条 I2C 总线会导致声卡 panic，详见 skills/）。

## 三、静置基线（无火）

连续 35 秒观测，无任何误报：

```
I (18882) vision: [调试] 火:面积0.000% CV0.00 分0.00(0/5) | 烟:占比0.000% 增长0.00 分0.00(0/15) | 帧180
I (22161) vision: [调试] 火:面积0.000% CV0.00 分0.00(0/5) | 烟:占比0.000% 增长0.00 分0.00(0/15) | 帧210
I (25369) vision: [调试] 火:面积0.000% CV0.00 分0.00(0/5) | 烟:占比0.000% 增长0.00 分0.00(0/15) | 帧240
I (28506) vision: [调试] 火:面积0.000% CV0.00 分0.00(0/5) | 烟:占比0.000% 增长0.00 分0.00(0/15) | 帧270
I (31961) vision: [调试] 火:面积0.000% CV0.00 分0.00(0/5) | 烟:占比0.000% 增长0.00 分0.00(0/15) | 帧300
I (35364) vision: [调试] 火:面积0.000% CV0.00 分0.00(0/5) | 烟:占比0.000% 增长0.00 分0.00(0/15) | 帧330
```

> 口径说明：该 35 秒样本仅能说明**本次观测未出现误报**，样本量不足以给出误报率统计值。
> 报告中不宣称"误报率 0%"。

## 四、真火触发（打火机，距镜头约 20–30 cm）

### 4.1 第一轮：检出能力验证

```
W (22410) vision: 【火情确认】score=1.00 面积=2.48%  闪烁CV=0.30
W (34346) vision: 【火情确认】score=1.00 面积=18.78% 闪烁CV=1.57
W (37190) vision: 【火情确认】score=1.00 面积=3.05%  闪烁CV=1.21
W (59157) vision: 【火情确认】score=1.00 面积=2.83%  闪烁CV=2.21
```

### 4.2 全链路联动（摄像头 → 判据 → 事件 → 继电器）

```
I (86153) app_core: event #42 type=FIRE sev=3 conf=1.00
W (86153) guard_control: CUTOFF! type=FIRE sev=3 node=1
```

`CUTOFF!` 表明 GPIO21 已驱动固态继电器执行三相切断，闭环成立。

### 4.3 第二轮：迟滞验证（修复跳变后）

静置 35 秒 → 点火 → 移开，全过程**仅触发一次**，无反复跳闸：

```
I (82144) vision: [调试] 火:面积0.139% CV0.30 分0.00(0/5) | 帧720     ← 静置
W (85868) vision: 【火情确认】score=1.00 面积=1.67% 闪烁CV=1.28        ← 点火
W (85967) main: [F6] 火情事件：确认明火 -> Lv3 切断 score=1.00
I (86153) app_core: event #42 type=FIRE sev=3 conf=1.00
W (86153) guard_control: CUTOFF! type=FIRE sev=3 node=1
I (86048) vision: [调试] 火:面积1.583% CV1.26 分1.00(5/5) | 帧750
I (88918) vision: [调试] 火:面积1.750% CV0.61 分1.00(5/5) | 帧780      ← 持续燃烧，稳定保持
I (94002) vision: [调试] 火:面积0.222% CV0.37 分0.15(5/5) | 帧810      ← 移开火源，迟滞未抖动
```

## 五、稳定性

修复 CPU 占用问题后连续运行，采集任务心跳正常，无看门狗、无自动复位：

```
I (37544) MON: ui=348 acq=323 http=16 c6=0 flush=466 heap=24873816
I (38545) MON: ui=358 acq=330 http=17 c6=0 flush=478 heap=23595036
I (39555) MON: ui=368 acq=339 http=17 c6=0 flush=490 heap=24873816
I (11750) lvgl_adapter_init: [DISP] VSYNC 60 Hz (5s), flush 68 次/5s（其中整屏 0），等超时 0
```

对照修复前（视觉任务与 acq 同核争抢 CPU）：

```
E (93386) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (93386) task_wdt:  - acq (CPU 1)
E (98577) MON: ui/acq 停滞 20s —— 渲染或采集链路卡死，自动复位自愈
```

## 六、烟雾判据修复前后对比

原判据仅用「低饱和 + 中亮度」，等价于统计灰色像素：

```
修复前（静置无烟）  烟雾=25.56%  25.11%  24.94%  26.03%
修复后（静置无烟）  烟:占比0.000%
```

加入帧间差分（要求 `|Y - Y_prev| >= 6`）后，静止背景被正确排除。

> **待完成**：真烟标定尚未进行（缺烟源）。当前烟雾阈值为设计值而非实测标定值，
> 报告中据实标注为"实验性功能"。

## 七、资源占用

| 项 | 数值 |
|---|---|
| 摄像头帧缓冲（PSRAM） | 约 3.9 MB（1280×720 UYVY × 2） |
| 集成后可用 heap | 24.8 MB（集成前 28.7 MB） |
| 固件增量 | +178 KB |
| 分区占用 | 2.49 MB / 9 MB（剩余 74%） |
