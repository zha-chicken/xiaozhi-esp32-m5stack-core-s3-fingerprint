# MemoMate Telephone

老式拨盘电话外壳里的无屏 AI 语音设备。基于 Waveshare ESP32-C6-LCD-1.69 开发板，
**1.69" ST7789 屏幕已物理拆除**（碎裂），状态反馈 = 语音提示音 + WS2812 状态灯。

产品仓库（文档/硬件/CAD）：`~/Projects/memomate-telephone`（本固件以 submodule 挂入其 `firmware/xiaozhi-esp32`）。

## 交互模型

IMU 拾取检测方案已废弃（演示不稳定），改为纯按键、确定性优先：

| 操作 | 行为 |
|------|------|
| PWR 长按 | 开机 / 关机（GPIO15 电池 EN 软锁存；USB 供电插电即开机） |
| BOOT 单击 | 功能键：开始 / 暂停对话（ToggleChatState） |
| BOOT 单击（启动中且未配网） | 进入 WiFi 配网模式 |

状态提示音：开机就绪=success（核心自带）、开始对话=popup、对话结束=vibration、
配网/激活码/低电量播报为核心自动行为。自定义中文播报（"请讲"/"正在关机"等）待
用 Doubao TTS 生成 OGG 放入 `main/assets/locales/zh-CN/` 替换。

## 硬件

| 功能 | GPIO | 说明 |
|------|------|------|
| ES8311 I2C | SDA 8 / SCL 7 | 与板载 codec 共享 I2C0 |
| I2S | MCLK 19 / BCLK 20 / WS 22 / DOUT 23 / DIN 21 | 24kHz in/out |
| BOOT 按键 | 9 | 引出至听筒功能键 |
| PWR 按键 | 18 | 引出至开关机键 |
| 电池 | ADC 0 / EN 15 | 5 分钟空闲自动关机（仅电池供电时） |
| WS2812 LED | 4（默认，**未接线**） | 拆屏空出的 LCD_RST 脚；实际安装时按 pad 调整 config.h |

预留未来电话硬件（拨号盘脉冲/摘机开关/MAX9814）：可用拆屏空出的 GPIO 1/2/3/5/6。

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
