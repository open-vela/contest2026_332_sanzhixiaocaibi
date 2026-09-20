# tools —— 大模型断闸决策网关

把电气故障事件 + 视觉火情置信度交给大模型研判，把结论下发给板上的
openvela 执行断闸。

```
  故障事件/视觉置信度
          │
          ▼
   guard_bridge.py  ──── 本地规则引擎（不依赖网络，保留否决权）
     (PC / 主机侧)   ──── 大模型 API（只能升级，不能降级）
          │
          │  USB-Serial/JTAG  单字符指令 c / r / b / s
          ▼
   openvela guardd  ──── /dev/gpio0 (GPIO21) 三相断闸 SSR
   (ESP32-P4 板上)   ──── /dev/gpio1 (GPIO22) 蜂鸣器
```

## 为什么大模型不接在板上

ESP32-P4 没有射频，要借板载 ESP32-C6 的 WiFi（esp-hosted over SDIO）。
openvela 侧当前三个硬门槛：

1. `CONFIG_NET` 在本移植配置里一条都没开，网络栈根本没编进去
2. `ESPRESSIF_WIFI` 的 Kconfig 是 `depends on ESPRESSIF_ESP32C3 || ESPRESSIF_ESP32C6`，**P4 不在列表里**
3. NuttX 树里没有 esp-hosted 驱动

移植 esp-hosted 是以周计的工作量。所以判断端放主机、执行端放 openvela。
ESP-IDF 那个固件（视觉 + UI）是能联网的，后续可以把这条链路收到板上。

## 安全设计

### 规则一：只能升级，不能降级

本地规则引擎先算基线，本地判定要断闸就一定断，不管大模型说什么、也不管
网络通不通。一个安全装置不能把最终否决权交给一条会超时的链路。
`handle()` 里 `rank(llm) > rank(local)` 才采纳大模型。

大模型不可达（超时/报错/返回不合法 JSON）时静默退回本地规则，不阻塞。

### 规则二：升级幅度一次最多一级

这条是实测之后补上的。同一条 TEMP 局部过热事件、同一个模型
（deepseek-chat）、`temperature=0`，两次运行分别给出 **ALARM** 和 **CUT**，
置信度都是 0.82：

```
第一次  大模型 -> ALARM (conf=0.82) ...故先本地声光告警并加强监视，暂不断闸。
第二次  大模型 -> CUT   (conf=0.82) ...已构成电气火灾高风险，应断闸处置。
```

**大模型本身就是不确定的，而 CUT 意味着真停电。** 不封顶的话，一次幻觉
就能让配电柜无故拉闸。封成单级之后，最坏情况只是多响一次蜂鸣器：

```
本地 NONE + 模型 CUT  ->  最终 ALARM（模型建议 CUT，单级封顶）
本地 ALARM + 模型 CUT ->  最终 CUT   （本地已到 ALARM，允许升到 CUT）
```

日志里 `llm_escalated` / `escalation_capped` 两个字段记录了每次的实际情况。

### 大模型的价值在哪

规则引擎只读 `type` / `severity` / `vision_fire` / `vision_smoke` 四个数值，
**它根本不读 `ctx` 那段自由文本**。而现场最关键的信息往往就在那段话里。

演示剧本第 2 条就是为此设计的：A 相端子 118℃、相邻两相 41℃/43℃、温差
75K 且持续扩大，但电流 9.2A 未过载、无电弧、无明火。所有数值阈值全部
不命中，规则引擎判 NONE。大模型读懂了"相间温差持续扩大 + 未过载"这个
组合——这是接触不良/端子松动的典型特征，电气火灾的经典起因——把结论
顶到了告警。

**这是语义理解相对阈值判据的真实增量，不是为了演示凑出来的。**

## 用法

需要 pyserial。乐鑫 IDF 自带的 python 环境里就有，不用另外装：

```
C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe
```

### 1. 配大模型

复制 `guard_bridge.example.ini` 成 `guard_bridge.ini` 填 key，
或者直接用环境变量（不落盘，更稳妥）：

```powershell
$env:GUARD_API_BASE = "https://dashscope.aliyuncs.com/compatible-mode/v1"
$env:GUARD_API_KEY  = "你的key"
$env:GUARD_MODEL    = "qwen-plus"
```

任何 OpenAI 兼容接口都行（通义 / DeepSeek / 智谱 ...）。

### 2. 板子烧 guardd 固件

```powershell
python -m esptool --chip esp32p4 -p COM3 write_flash --force `
  --flash_size 16MB --flash_mode dio --flash_freq 80m 0x2000 nuttx-guardd-demo.bin
```

偏移是 **0x2000**（不是 0x20000），**不要加 `--baud`**——USB-Serial/JTAG
上改波特率会让 esptool 崩。

### 3. 跑演示

```powershell
python guard_bridge.py --port COM3 --scenario scenarios\demo.json
```

启动后终端会打印：

```
监控台: http://127.0.0.1:8765   （录像时全屏这个页面）
```

浏览器打开这个地址就是实时监控面板（`dashboard.html`）。

不带 key 想先看链路，加 `--no-llm`（只跑本地规则）；不接板子加 `--dry-run`；
不要面板加 `--serve 0`；交互式逐条投事件用 `--interactive`。
`--pace N` 控制事件之间的停顿秒数，录像时建议 5 以上。

### 4. 监控面板

MIPI 屏在 openvela 下点不亮（NuttX 对乐鑫没有 MIPI-DSI 驱动），所以
openvela 这一段的可视化放在主机侧。**面板上的数据全部是真的**：

- 两盏执行器指示灯的状态来自板子自己回报的 `GUARD STATUS cut=N buzzer=N`，
  每次动作后网关都会发 `s` 重新读一次，不是本地猜的
- 事件、双路证据条、本地规则结论、大模型结论与理由，都是当次运行的实际值
- 断闸时整页转红并加脉冲边框，与板上继电器动作同步

面板只读 `/state`，决策逻辑完全不依赖它——**面板挂了不影响断闸**。

录像建议：1600×900 以上全屏（窄于 1180px 会自动改成纵向排布），
左边面板右边板子同框，继电器一动画面同步变红。

每次决策都会追加到 `guard_bridge.log`（JSON 行），含本地结论、大模型
结论、最终动作和板子回报，可直接作为证据。

## 演示剧本 `scenarios/demo.json`

五条事件：

| # | 事件 | 本地规则 | 大模型 | 最终 | 意图 |
|---|---|---|---|---|---|
| 1 | OVERVOLT Lv1，视觉无异常 | NONE | NONE | NONE | 电压瞬时波动不该断闸 |
| 2 | **TEMP Lv1，端子 118℃ / 温差 75K** | NONE | CUT | **ALARM** | **大模型读懂自由文本，把规则漏掉的隐患顶出来；单级封顶** |
| 3 | OVERLOAD Lv2，烟雾 0.31 | ALARM | ALARM | ALARM | 烟先于火，先告警 |
| 4 | ARC Lv2 + 视觉火情 0.71 | **CUT** | CUT | **CUT** | 电弧叠加视觉证据，交叉验证断闸 |
| 5 | TEMP Lv1，断闸后回落 | NONE | NONE | NONE | 处置生效 |

两条主线：

- **第 2 条讲大模型的价值**：所有阈值都不命中，规则引擎判 NONE，是大模型
  从"温差 75K 持续扩大但电流未过载"里读出了接触不良隐患。同时它建议的
  CUT 被单级封顶拦成 ALARM——展示我们对模型不确定性的防护。
- **第 4 条讲交叉验证**：单看电气特征只是 Lv2（不断闸），单看视觉也可能
  误报；两路独立证据同时成立才升级到断闸。

## 板上协议

`guardd` 常驻在 openvela 上，从 `/dev/console` 读单字符：

| 指令 | 动作 | 回报 |
|---|---|---|
| `c` | GPIO21 拉高断闸 + 长鸣 | `GUARD CUT ret=0` |
| `r` | GPIO21 拉低复位 | `GUARD RESTORE ret=0` |
| `b` | 仅鸣笛，不断闸 | `GUARD BEEP` |
| `s` | 读回引脚状态 | `GUARD STATUS cut=0 buzzer=0` |

指令必须**逐字符发、每个间隔 ≥250 ms**。USB-Serial/JTAG 接收侧连续快写
会把主机的 OUT 端点堵死（已知缺陷，见
`../app/electric_guard_vision/evidence/`）。`guard_bridge.py` 已经按这个
节奏发，手工用串口助手测的时候也要注意。
