# MemoMate Telephone

老式拨盘电话外壳里的无屏 AI 语音设备。基于 Waveshare ESP32-C6-LCD-1.69 开发板，
**1.69" ST7789 屏幕已物理拆除**（碎裂），状态反馈 = WS2812 状态灯（灯语见下表）。

产品仓库（文档/硬件/CAD）：`~/Projects/memomate-telephone`（本固件以 submodule 挂入其 `firmware/xiaozhi-esp32`）。

## 交互模型

IMU 拾取检测方案已废弃（演示不稳定），改为纯按键、确定性优先：

| 操作 | 行为 |
|------|------|
| PWR 长按 | 开机 / 关机（GPIO15 电池 EN 软锁存；USB 供电插电即开机） |
| BOOT 单击 | 功能键：开始 / 暂停对话（ToggleChatState） |
| BOOT 单击（启动中且未配网） | 进入 WiFi 配网模式 |

提示音只保留 xiaozhi 核心自带行为（开机就绪、配网、激活码逐位播报、低电量）。
自定义语音 cue（"请讲/已暂停/正在关机"）已试验并移除——对用户打扰大，LED 灯语取代。

## 灯语（memomate_led.cc）

核心规则：**通电期间灯永不熄灭**（灯灭 = 已关机）。常亮 = 稳定态，闪烁 = 过渡/需操作，
红色只用于故障。

| 状态 | 灯语 | 含义 |
|------|------|------|
| 启动中 | 白·快闪 (100ms) | 正在醒来 |
| 等待配网 | 黄·慢闪 (500ms) | 需要操作：连 WiFi |
| 等待激活 | 黄·快闪 (200ms) | 需要操作：平台输配对码 |
| 连接服务器 | 青·常亮 | 过渡（短暂） |
| **待命** | **暖白·微亮常亮** | 开机底色 |
| 聆听 | 绿·常亮（人声时更亮） | 请讲 |
| AI 说话 | 蓝·常亮 | 对方在说 |
| 固件升级 | 紫·快闪 (100ms) | 勿断电 |
| 故障 | 红·慢闪 (500ms) | 唯一的红 |
| 关机 | 灭 | PWR 长按后灯先灭再断电 |

实现：`MemomateLed : SingleLed`（共享类的绘制原语已设为 protected），板级自带映射，
不影响其他 board。

## 硬件

| 功能 | GPIO | 说明 |
|------|------|------|
| ES8311 I2C | SDA 8 / SCL 7 | 与板载 codec 共享 I2C0 |
| I2S | MCLK 19 / BCLK 20 / WS 22 / DOUT 23 / DIN 21 | 24kHz in/out |
| BOOT 按键 | 9 | 引出至听筒功能键 |
| PWR 按键 | 18 | 引出至开关机键 |
| 电池 | ADC 0 / EN 15 | 5 分钟空闲自动关机（仅电池供电时） |
| WS2812 LED | 17（已接线 2026-06-04） | ESP_RXD 焊点，板上无其他连接；与 3V3/GND pad 相邻三线直焊。供电 3V3（5V pad 电池供电时无电）。不要改用 GPIO16（ROM boot 日志会闪灯） |

预留未来电话硬件（拨号盘脉冲/摘机开关/MAX9814）：拆屏空出的 GPIO 1/2/3/4/5/6 **均无焊点**，
只在 LCD FPC 座上——电话硬件接入时需从 FPC 座引线或改用 I2C 扩展。有焊点的 GPIO 已全部占用
（9/18=按键，7/8=I2C，16=UART TX 保留日志，17=LED，12/13=USB）。

## 构建

```bash
source ~/environments/hardware/esp/v5.5.2/esp-idf/export.sh
idf.py set-target esp32c6
idf.py menuconfig   # Xiaozhi Assistant → Board Type → MemoMate Telephone
idf.py build
```

注意：`config.json` 的 `sdkconfig_append`（电源管理 `CONFIG_PM_ENABLE` 等）只被
`scripts/release.py` 消费。本地 `idf.py` 构建需在 set-target 后手动追加一次：

```bash
cat >> sdkconfig <<'EOF'
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
EOF
idf.py build
```

服务端：继承 fork 全局默认 OTA `https://wexiyi.com/agents/api/ota/`（haolab.ai 平台）。
