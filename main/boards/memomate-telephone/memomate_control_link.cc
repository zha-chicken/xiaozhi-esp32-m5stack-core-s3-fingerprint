#include "memomate_control_link.h"

#include <cstring>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "board.h"
#include "memomate_led.h"
#include "network_interface.h"
#include "settings.h"
#include "system_info.h"
#include "web_socket.h"

#define TAG "memomate_control"

namespace {
constexpr int kConnectId = 0;          // conversation WS uses 1; control = 0
constexpr int kTickMs = 200;           // ring-FSM service cadence
constexpr int kKeepaliveMs = 25000;    // client ping interval
constexpr int kProvisionWaitMs = 3000; // poll for OTA-provisioned config
constexpr int kBackoffMinMs = 1000;
constexpr int kBackoffMaxMs = 30000;
constexpr int64_t kRingTimeoutMs = 15000;  // ring this long unanswered, then give up
                                           // (15s — long enough to reach the phone,
                                           // short enough to not nag; tune here)
constexpr int64_t kRingResendMs = 1000;    // re-assert RING while ringing (self-heal)
}  // namespace

bool MemomateControlLink::Start() {
    if (started_) {
        return true;
    }
    if (xTaskCreate(&MemomateControlLink::TaskTrampoline, "control_link", 8192,
                    this, 4, &task_) != pdPASS) {
        ESP_LOGE(TAG, "control task create failed");
        return false;
    }
    started_ = true;
    return true;
}

void MemomateControlLink::TaskTrampoline(void* arg) {
    static_cast<MemomateControlLink*>(arg)->Run();
}

bool MemomateControlLink::SendJson(const std::string& json) {
    std::lock_guard<std::mutex> lock(ws_mutex_);
    if (ws_ == nullptr || !connected_) {
        return false;
    }
    return ws_->Send(json);
}

void MemomateControlLink::Run() {
    int backoff = kBackoffMinMs;
    while (true) {
        // Read OTA-provisioned config each attempt; wait until it appears so we
        // can start before the first OTA poll completes.
        std::string url, token;
        int version;
        {
            Settings settings("control_ws", false);
            url = settings.GetString("url");
            token = settings.GetString("token");
            version = settings.GetInt("version", 1);
        }
        if (url.empty()) {
            vTaskDelay(pdMS_TO_TICKS(kProvisionWaitMs));
            continue;
        }

        auto ws = Board::GetInstance().GetNetwork()->CreateWebSocket(kConnectId);
        if (ws == nullptr) {
            ESP_LOGE(TAG, "CreateWebSocket failed");
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff = backoff >= kBackoffMaxMs ? kBackoffMaxMs : backoff * 2;
            continue;
        }

        if (!token.empty()) {
            std::string auth = token;
            if (auth.find(' ') == std::string::npos) {
                auth = "Bearer " + auth;
            }
            ws->SetHeader("Authorization", auth.c_str());
        }
        ws->SetHeader("Protocol-Version", std::to_string(version).c_str());
        ws->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        ws->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

        connected_ = false;
        ws->OnConnected([this]() { connected_ = true; });
        ws->OnDisconnected([this]() { connected_ = false; });
        ws->OnData([this](const char* data, size_t len, bool binary) {
            if (!binary) {
                OnMessage(data, len);
            }
        });

        // Publish ws_ BEFORE Connect — the server sends its control hello right
        // after the handshake, so OnData/SendJson may run during Connect().
        {
            std::lock_guard<std::mutex> lock(ws_mutex_);
            ws_ = ws.get();
        }

        ESP_LOGI(TAG, "control WS connecting: %s (v%d)", url.c_str(), version);
        if (ws->Connect(url.c_str())) {
            connected_ = true;
            backoff = kBackoffMinMs;
            ESP_LOGI(TAG, "control WS connected");
            last_status_.clear();  // re-report occupancy on a fresh connection
            int since_ping = 0;
            while (connected_) {
                vTaskDelay(pdMS_TO_TICKS(kTickMs));
                ServiceRing();
                ReportStatus();
                since_ping += kTickMs;
                if (since_ping >= kKeepaliveMs) {
                    since_ping = 0;
                    ws->Ping();
                }
            }
            ESP_LOGW(TAG, "control WS disconnected");
        } else {
            ESP_LOGW(TAG, "control WS connect failed");
        }

        {
            std::lock_guard<std::mutex> lock(ws_mutex_);
            ws_ = nullptr;
        }
        connected_ = false;
        // If the link dropped mid-ring, stop locally (can't report missed).
        StopRing(/*answered=*/false, /*send_missed=*/false);
        ws.reset();  // close + free

        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = backoff >= kBackoffMaxMs ? kBackoffMaxMs : backoff * 2;
    }
}

void MemomateControlLink::OnMessage(const char* data, size_t len) {
    (void)len;
    cJSON* root = cJSON_Parse(data);
    if (root == nullptr) {
        ESP_LOGW(TAG, "control: bad json");
        return;
    }
    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        const char* t = type->valuestring;
        if (strcmp(t, "notification") == 0) {
            cJSON* idj = cJSON_GetObjectItem(root, "notificationId");
            cJSON* rj = cJSON_GetObjectItem(root, "render");
            const char* nid = cJSON_IsString(idj) ? idj->valuestring : nullptr;
            const char* render = cJSON_IsString(rj) ? rj->valuestring : nullptr;
            // Receipt: we received it (regardless of render).
            if (nid != nullptr) {
                cJSON* ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "type", "notification.delivered");
                cJSON_AddStringToObject(ack, "notificationId", nid);
                char* s = cJSON_PrintUnformatted(ack);
                SendJson(s);
                cJSON_free(s);
                cJSON_Delete(ack);
            }
            HandleNotification(nid, render);
        } else if (strcmp(t, "hello") == 0) {
            ESP_LOGI(TAG, "control WS server hello");
        } else if (strcmp(t, "ack") == 0) {
            ESP_LOGD(TAG, "control ack");
        } else if (strcmp(t, "error") == 0) {
            cJSON* code = cJSON_GetObjectItem(root, "code");
            ESP_LOGW(TAG, "control error: %s",
                     cJSON_IsString(code) ? code->valuestring : "?");
        } else {
            ESP_LOGD(TAG, "control msg type=%s", t);
        }
    }
    cJSON_Delete(root);
}

void MemomateControlLink::HandleNotification(const char* notification_id,
                                             const char* render) {
    if (notification_id == nullptr || *notification_id == '\0') {
        ESP_LOGW(TAG, "notification without notificationId — ignored");
        return;
    }
    if (render == nullptr || strcmp(render, "ring") != 0) {
        // Phase 1 only renders "ring". Others are acknowledged (delivered) but
        // not acted on — "通知 ≠ 强制交互".
        ESP_LOGI(TAG, "notification render=%s (not ring) — noted only",
                 render ? render : "?");
        return;
    }

    // Stash for the answer hello (consumed by GetHelloMessage on off-hook).
    Application::GetInstance().SetPendingNotificationId(notification_id);
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        ringing_ = true;
        ring_started_us_ = esp_timer_get_time();
        ring_last_resend_us_ = ring_started_us_;
        ring_nid_ = notification_id;
    }
    if (ring_sender_) {
        ring_sender_(true);  // ESP-NOW RING START -> base body ringer
    }
    static_cast<MemomateLed*>(Board::GetInstance().GetLed())->ShowRinging();
    // status:ringing is reported by ReportStatus() on the next tick (single
    // source of truth derived from ring + device state).
    ESP_LOGI(TAG, "RING notification nid=%s -> ESP-NOW RING + 振铃灯语",
             notification_id);
}

void MemomateControlLink::ServiceRing() {
    bool answered = false, missed = false, resend = false;
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        if (!ringing_) {
            return;
        }
        int64_t now = esp_timer_get_time();
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            answered = true;  // off-hook opened a conversation
            ringing_ = false;
        } else if ((now - ring_started_us_) / 1000 >= kRingTimeoutMs) {
            missed = true;
            ringing_ = false;
        } else if ((now - ring_last_resend_us_) / 1000 >= kRingResendMs) {
            // Self-heal: re-assert RING START so a transient base reboot/unpair
            // (it re-pairs within seconds) still catches the ring. Idempotent on
            // the base (ring_audio_start no-ops if already ringing). Mirrors the
            // hook-level reconciliation the heartbeat already does.
            resend = true;
            ring_last_resend_us_ = now;
        }
    }
    if (answered) {
        StopRing(/*answered=*/true, /*send_missed=*/false);
    } else if (missed) {
        StopRing(/*answered=*/false, /*send_missed=*/true);
    } else if (resend && ring_sender_) {
        ring_sender_(true);
    }
}

void MemomateControlLink::StopRing(bool answered, bool send_missed) {
    std::string nid;
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        // Allow StopRing to run from the disconnect path even if ServiceRing
        // already cleared ringing_ (idempotent): only act if there's a nid.
        if (ring_nid_.empty() && !ringing_) {
            return;
        }
        ringing_ = false;
        nid = ring_nid_;
        ring_nid_.clear();
    }
    if (ring_sender_) {
        ring_sender_(false);  // ESP-NOW RING STOP (base also stops on off-hook)
    }
    // status (in_call / idle) is reported by ReportStatus() from device state.
    if (answered) {
        ESP_LOGI(TAG, "ring answered -> conversation (nid=%s)", nid.c_str());
        return;
    }
    // Not answered: clear the stale pending id, restore idle 灯语.
    Application::GetInstance().SetPendingNotificationId("");
    static_cast<MemomateLed*>(Board::GetInstance().GetLed())->OnStateChanged();
    if (send_missed && !nid.empty()) {
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "type", "notification.missed");
        cJSON_AddStringToObject(m, "notificationId", nid.c_str());
        char* s = cJSON_PrintUnformatted(m);
        SendJson(s);
        cJSON_free(s);
        cJSON_Delete(m);
        ESP_LOGW(TAG, "ring timeout -> missed (nid=%s)", nid.c_str());
    } else {
        ESP_LOGW(TAG, "ring stopped (link/local), no receipt");
    }
}

// Occupancy reporting (ADR §6): one source of truth for the control-connection
// status, derived from ring + device state. Reported on change so the platform's
// busy policy gates competing calls — covers BOTH ring-answered and manual
// (dial / button) conversations, not just answered notifications.
void MemomateControlLink::ReportStatus() {
    bool ringing;
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        ringing = ringing_;
    }
    DeviceState ds = Application::GetInstance().GetDeviceState();
    bool in_conversation = (ds == kDeviceStateConnecting ||
                            ds == kDeviceStateListening ||
                            ds == kDeviceStateSpeaking);
    const char* status = ringing ? "ringing" : (in_conversation ? "in_call" : "idle");
    if (last_status_ == status) {
        return;
    }
    last_status_ = status;
    std::string json = std::string("{\"type\":\"status\",\"state\":\"") + status + "\"}";
    SendJson(json);
    ESP_LOGI(TAG, "status -> %s", status);
}
