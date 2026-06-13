#include "memomate_led.h"
#include "application.h"
#include <esp_log.h>

#define TAG "MemomateLed"

void MemomateLed::ShowRinging() {
    // 橙·快闪：来电振铃 — 醒目"快接电话"。只在 Idle 期间由控制连接触发，与
    // 配网/激活的黄闪不会同时出现。接起后状态切到 Connecting，OnStateChanged
    // 自动重绘；超时/取消止铃则显式调 OnStateChanged 回到 idle 暖白。
    SetColor(20, 8, 0);
    StartContinuousBlink(180);
}

void MemomateLed::OnStateChanged() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    switch (state) {
        case kDeviceStateStarting:
            // 白·快闪：正在启动
            SetColor(8, 8, 8);
            StartContinuousBlink(100);
            break;
        case kDeviceStateWifiConfiguring:
            // 黄·慢闪：等待配网（需要用户操作）
            SetColor(12, 8, 0);
            StartContinuousBlink(500);
            break;
        case kDeviceStateActivating:
            // 黄·快闪：等待平台输入配对码（需要用户操作）
            SetColor(12, 8, 0);
            StartContinuousBlink(200);
            break;
        case kDeviceStateConnecting:
            // 青·常亮：连接服务器（短暂过渡）
            SetColor(0, 8, 8);
            TurnOn();
            break;
        case kDeviceStateIdle:
            // 暖白·微亮常亮：开机待命的底色 — 永不熄灭
            SetColor(6, 3, 1);
            TurnOn();
            break;
        case kDeviceStateListening:
        case kDeviceStateAudioTesting:
            // 绿·常亮：请讲（检测到人声时更亮）
            SetColor(0, app.IsVoiceDetected() ? 32 : 10, 0);
            TurnOn();
            break;
        case kDeviceStateSpeaking:
            // 蓝·常亮：AI 在说话
            SetColor(0, 0, 16);
            TurnOn();
            break;
        case kDeviceStateUpgrading:
            // 紫·快闪：固件升级中，勿断电
            SetColor(10, 0, 12);
            StartContinuousBlink(100);
            break;
        case kDeviceStateFatalError:
        default:
            // 红·慢闪：故障 — 全表唯一的红
            ESP_LOGW(TAG, "LED fault pattern for state %d", state);
            SetColor(16, 0, 0);
            StartContinuousBlink(500);
            break;
    }
}
