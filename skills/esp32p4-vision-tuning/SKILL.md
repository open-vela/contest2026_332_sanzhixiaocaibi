---
name: esp32p4-vision-tuning
description: "Build, integrate and calibrate an on-device camera vision pipeline on ESP32-P4 (MIPI-CSI + esp_video V4L2) inside an existing LVGL/BSP application. Use when adding camera capture to a P4 project that already drives the display or audio codec, when a vision task starves other tasks and trips the task watchdog, when the camera brings up but the I2C codec/touch then fails, when LVGL silently changes version after adding a component, or when calibrating detection thresholds against real serial-log measurements instead of guessed constants."
---

# ESP32-P4 端侧视觉集成与判据标定

把摄像头采集 + 端侧判据接进一个**已经在跑**的 ESP32-P4 应用（LVGL 显示、音频、传感器采集并存），
并用真机串口日志标定阈值。

本 Skill 沉淀自「电脉卫士」项目（contest2026_332）在 ESP32-P4-Function-EV-Board 上的实际调试过程，
下列每一条都是实测踩过的坑，不是理论推导。

---

## 一、先验证摄像头，再动主工程

**不要直接往主工程里加摄像头代码。** 先把 `esp_video` 自带的示例拷成独立工程验证出图：

```bash
cp -r managed_components/espressif__esp_video/examples/capture_stream ./cam-test
cd cam-test
echo "CONFIG_EXAMPLE_SELECT_ESP32P4_FUNCTION_EV_BOARD_V1_5=y" >> sdkconfig.defaults
idf.py set-target esp32p4 && idf.py build && idf.py -p COMx flash monitor
```

出图成功的标志（串口）：

```
Capture format: YUV 4:2:2 UYVY, frame size: 1280x720, FPS: 30.0
```

通过之后再把配置搬进主工程。**这一步省不得**——出问题时能立刻分清是摄像头本身还是集成冲突。

---

## 二、三个必踩的坑

### 坑 1：I2C 总线冲突 —— 摄像头起来了，声卡挂了

**症状**：`sc2336: Detected Camera sensor PID=0xcb3a` 成功，随后
`I2C_If: Fail to read from dev 30` 刷屏 → `ES8311: Open fail` → panic 重启。

**根因**：本板 SC2336(SCCB)、GT911(触摸)、ES8311(声卡) **共用同一对物理引脚**
`SCL=GPIO8 / SDA=GPIO7`。BSP 已在 **I2C port 1** 上建好这条总线；
而 `esp_video` 的示例默认在 **port 0** 上"再建一条"——
两个 I2C 控制器同时驱动同一对引脚，总线被打烂。

**注意**：端口号不同 ≠ 没有冲突。冲突发生在**引脚层**，不是端口层。

**解法**：取 BSP 句柄，置 `init_sccb = false`：

```c
extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);

esp_video_init_csi_config_t csi_cfg = {
    .sccb_config = {
        .init_sccb  = false,              /* 复用已有总线，不自建 */
        .i2c_handle = bsp_i2c_get_handle(),
        .freq       = 100000,
    },
    .reset_pin = -1,
    .pwdn_pin  = -1,
};
esp_video_init_config_t vcfg = { .csi = &csi_cfg };
esp_video_init(&vcfg);
```

**不要** `#include "bsp/esp32_p4_function_ev_board.h"` —— 该头会连带引入
`display.h → esp_lcd_types.h`，而 `esp_lcd` 在 BSP 组件里是 `PRIV_REQUIRES`，
外部组件包含即编译失败。直接 `extern` 声明那一个函数原型即可。

### 坑 2：PSRAM 跨行访问 —— 帧处理慢 50 倍，把别的任务饿死

**症状**：`E (xxxxx) task_wdt: Task watchdog got triggered — acq (CPU 1)`，
随后整机自动复位。

**根因**：帧缓冲在 PSRAM。按"隔 N 行取一行"抽样时，每次跨行要跳 `width×2×N` 字节，
**几乎每个采样点都是 cache miss**。按 SRAM 速度估算会严重低估。

实测参照（乐鑫官方 `esp-dl/motion_detect` 在 P4 上的基准）：

| 分辨率 | Stride | 耗时 |
|---|---|---|
| 1280×720 | 1 | 86.8 ms |
| 1280×720 | 2 | 26.8 ms |
| 640×360 | 1 | 21.9 ms |
| 640×360 | 2 | 6.9 ms |

**降分辨率比优化代码有效得多**——1280→640 直接砍掉 4 倍。

**解法（按性价比排序）**：
1. 降采集分辨率（收益最大）
2. 放宽抽样步长（步长翻倍 = 计算量 1/4）
3. 隔帧分析（`frames % 2`）
4. 单遍统计：用**上一帧**的均值判本帧，省掉求均值那一遍
5. 循环末尾显式 `vTaskDelay(pdMS_TO_TICKS(10))` —— **必须加**，
   仅靠 `VIDIOC_DQBUF` 阻塞是不够的：帧持续可用时它几乎不阻塞，任务近似满负荷自旋

### 坑 3：加组件导致 LVGL 被静默降级

**症状**：新增任意组件后，LVGL 突然编译失败：

```
lv_draw_ppa_private.h: error: #error "PPA buffers need to be aligned to 64-byte boundary!"
lv_draw_ppa_private.h: fatal error: driver/ppa.h: No such file or directory
BUG: component_requirements.py: cannot match original component filename
```

**根因**：上游只把 lvgl 约束成 `">=8,<10"`（见 `dependencies.lock`）。
**任何一次依赖重新解析都可能把它解到别的小版本**，而不同版本的 PPA 绘制单元
与工程的对齐配置不兼容。

**排查方法**：改动前先备份 `sdkconfig`，出问题后 diff：

```bash
cp sdkconfig sdkconfig.bak-before-change
# ...改动、编译失败...
diff sdkconfig.bak-before-change sdkconfig | grep -i lvgl
```

看到 `< CONFIG_LVGL_VERSION_MINOR=5` / `> CONFIG_LVGL_VERSION_MINOR=4` 即确诊。

**解法**：在 `main/idf_component.yml` 里显式锁死：

```yaml
  lvgl/lvgl:
    version: "9.5.*"      # 锁死，不留浮动空间
    public: true
```

---

## 三、判据标定：用实测数据定阈值，不要猜

### 3.1 先加逐帧调试输出

**没有这个，一旦触发失败就完全不知道原因**（面积不够？变化不够？）。

```c
if ((frames % 30) == 0) {
    ESP_LOGI(TAG, "[调试] 面积%.3f%% CV%.2f 分%.2f(阈值%.2f) 连续%d/%d 帧%lu",
             area*100, cv, score, threshold, confirm_run, confirm_n, frames);
}
```

### 3.2 抓串口（Windows PowerShell，无需额外工具）

```powershell
$port = New-Object System.IO.Ports.SerialPort COM3,115200,None,8,one
$port.Encoding = [System.Text.Encoding]::UTF8      # 中文日志不乱码
$port.DtrEnable = $false; $port.RtsEnable = $false  # 防止误触发复位
$port.Open()
$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
    $d = $port.ReadExisting(); if ($d) { [void]$sb.Append($d) }
    Start-Sleep -Milliseconds 80
}
$port.Close(); $sb.ToString()
```

主动复位：`$port.RtsEnable=$true; sleep 0.15; $port.RtsEnable=$false`

### 3.3 标定顺序

1. **静置基线**（30 秒以上）—— 确认无目标时得分为 0，先排除误报
2. **真实目标** —— 记录实测的各项分量
3. 依据实测值调阈值，**不要凭经验设常数**

### 3.4 三个评分设计原则

**原则一：弱证据项必须相乘，不能相加。**

```c
/* 错误：面积很小时，时域项独自把分数顶过阈值 */
score = 0.6f * area_term + 0.4f * temporal_term;

/* 正确：时域只调制面积证据，不能独立成分 */
score = area_term * (0.6f + 0.4f * temporal_term);
```

实测教训：相加式在开机瞬间（AE/AWB 收敛、画面剧烈跳变）
面积仅 0.49% 却因时域项封顶而得分 0.59 > 0.55 → **每次上电就误触发**。

**原则二：确认与解除必须对称（迟滞）。**

只有"连续 N 帧确认"而"1 帧即解除"，会导致状态反复跳变——
实测出现确认→解除→确认→解除的连续抖动，每跳一次就是一次告警和动作。

```c
const float clear_th = score_th * 0.75f;          /* 迟滞带 */
if (score >= score_th)            { conf_run++; clear_run = 0; }
else if (score < clear_th)        { if (++clear_run >= CLEAR_N) conf_run = 0; }
/* 落在迟滞带内：两个计数都不动，维持现状 */
```

**原则三：传感器预热期不参与判定。**

```c
#define WARMUP_FRAMES 45      /* @30fps ≈ 1.5s，等 AE/AWB 收敛 */
if (frames >= WARMUP_FRAMES && area >= AREA_FLOOR) { /* 才计分 */ }
```

同理，方差/变异系数类指标要求最小样本数，否则 4~5 个跳变样本就能算出
CV≈1.0 的假高值。

---

## 四、运动类判据：别只看颜色

判定"低饱和灰色区域"（烟雾、雾气）时，**只用颜色必然失败**——
实测静止房间里"灰色像素"恒占 25%（墙面、桌面、机箱外壳全部命中）。

必须加**帧间差分**：存一份上一帧的抽样点亮度（步长 16 时仅 3.6KB），
要求 `|Y - Y_prev| >= DELTA` 才计入。加上之后静置占比从 25% 降到 **0.000%**。

同时要做**全局变化排除**：

```c
/* 变化点过半 -> 自动曝光调整/开关灯/镜头前有人经过，
 * 此时"运动"维度失去区分力，整帧丢弃 */
if ((float)changed / n > 0.50f) { /* skip this frame */ }
```

---

## 五、集成检查清单

- [ ] 摄像头在独立工程验证出图
- [ ] SCCB 复用 BSP 的 I2C 句柄（`init_sccb=false`）
- [ ] 视觉任务优先级低于安全关键任务，且循环内有显式 `vTaskDelay`
- [ ] `sdkconfig` 改动前已备份，LVGL 版本已锁死
- [ ] 逐帧调试日志已加
- [ ] 静置基线实测为 0，无误报
- [ ] 评分为相乘式，确认/解除对称，有预热期
- [ ] 真机触发实测通过，日志留档作为指标佐证
