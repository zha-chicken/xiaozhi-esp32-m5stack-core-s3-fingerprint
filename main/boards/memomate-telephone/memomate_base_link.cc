#include "memomate_base_link.h"

#include <cstring>

#include <esp_log.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "application.h"
#include "espnow_link.h"

#define TAG "memomate_base_link"

// Heartbeat auto-start guard: after a reconcile-triggered start, suppress
// re-starting for this long so a stuck off-hook level can't hammer the connect.
static constexpr int64_t kAutoStartGuardUs = 10'000'000;  // 10s

namespace {

// Singleton pointer so the C ESP-NOW callbacks can reach the instance.
MemomateBaseLink* g_self = nullptr;

// A conversation is "active" in any of these states — i.e. lifting/replacing
// the handset should NOT re-start, and replacing it MUST hang up.
bool InConversation(DeviceState s) {
    return s == kDeviceStateConnecting || s == kDeviceStateListening ||
           s == kDeviceStateSpeaking;
}

// Max ESP-NOW payload is 250 bytes; our largest frame (base_event_t) is well
// under that. Queue a fixed-size record (src MAC + raw bytes + len) so the recv
// callback stays fast and never calls into the Application or esp_now_send.
struct RxRecord {
    uint8_t src[6];
    uint8_t data[64];
    int len;
};

uint8_t CurrentChannel() {
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) {
        return 0;
    }
    return primary;
}

}  // namespace

bool MemomateBaseLink::Start() {
    if (started_) {
        return true;
    }

    rx_queue_ = xQueueCreate(8, sizeof(RxRecord));
    if (rx_queue_ == nullptr) {
        ESP_LOGE(TAG, "rx queue alloc failed");
        return false;
    }

    esp_err_t e = esp_now_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(e));
        vQueueDelete(rx_queue_);
        rx_queue_ = nullptr;
        return false;
    }

    g_self = this;
    esp_now_register_recv_cb(&MemomateBaseLink::OnRecvTrampoline);

    if (xTaskCreate(&MemomateBaseLink::DispatchTaskTrampoline, "base_link", 4096,
                    this, 5, &task_) != pdPASS) {
        ESP_LOGE(TAG, "dispatch task create failed");
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        vQueueDelete(rx_queue_);
        rx_queue_ = nullptr;
        g_self = nullptr;
        return false;
    }

    started_ = true;
    ESP_LOGI(TAG, "ESP-NOW base-link up on channel %u; waiting for DISCOVERY",
             CurrentChannel());
    return true;
}

void MemomateBaseLink::OnRecvTrampoline(const esp_now_recv_info_t* info,
                                        const uint8_t* data, int len) {
    if (g_self == nullptr || g_self->rx_queue_ == nullptr) {
        return;
    }
    if (len < 1 || len > (int)sizeof(RxRecord::data)) {
        return;
    }
    RxRecord rec;
    memcpy(rec.src, info->src_addr, 6);
    memcpy(rec.data, data, len);
    rec.len = len;
    // The recv callback runs in the WiFi task — never block. A 0-timeout send
    // drops the frame if the queue is full (only heartbeats would be lost; the
    // base re-sends on its 3s cadence).
    xQueueSend(g_self->rx_queue_, &rec, 0);
}

void MemomateBaseLink::DispatchTaskTrampoline(void* arg) {
    auto* self = static_cast<MemomateBaseLink*>(arg);
    while (true) {
        self->HandleQueuedFrame();
    }
}

void MemomateBaseLink::HandleQueuedFrame() {
    RxRecord rec;
    if (xQueueReceive(rx_queue_, &rec, portMAX_DELAY) != pdTRUE) {
        return;
    }
    uint8_t tag = rec.data[0];
    if (tag == PAIR_KIND_DISCOVERY && rec.len == (int)sizeof(pair_frame_t)) {
        HandlePairFrame(rec.src, rec.data, rec.len);
    } else if (tag <= BASE_EVT_NUMBER) {
        HandleEventFrame(rec.src, rec.data, rec.len);
    } else {
        ESP_LOGW(TAG, "unclassified frame tag=0x%02x len=%d", tag, rec.len);
    }
}

void MemomateBaseLink::EnsurePeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) {
        return;
    }
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;  // follow STA channel
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t e = esp_now_add_peer(&peer);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "add_peer failed: %s", esp_err_to_name(e));
    }
}

void MemomateBaseLink::HandlePairFrame(const uint8_t* src, const uint8_t* data,
                                       int len) {
    pair_frame_t disc;
    memcpy(&disc, data, sizeof(disc));
    EnsurePeer(src);
    pair_frame_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.kind = PAIR_KIND_REPLY;
    reply.channel = CurrentChannel();
    reply.nonce = disc.nonce;  // echo so the base rejects stale replies
    esp_err_t e = esp_now_send(src, (const uint8_t*)&reply, sizeof(reply));
    ESP_LOGI(TAG,
             "DISCOVERY from %02x:%02x:%02x:%02x:%02x:%02x (nonce=%lu) -> "
             "PAIR_REPLY ch=%u (%s)",
             src[0], src[1], src[2], src[3], src[4], src[5],
             (unsigned long)disc.nonce, reply.channel,
             e == ESP_OK ? "sent" : esp_err_to_name(e));
}

void MemomateBaseLink::StartChatIfIdle(const char* reason) {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    last_autostart_us_ = esp_timer_get_time();
    ESP_LOGI(TAG, "start chat (%s)", reason);
    app.ToggleChatState();  // Idle -> open channel + listen
}

void MemomateBaseLink::HangupIfActive(const char* reason) {
    auto& app = Application::GetInstance();
    if (!InConversation(app.GetDeviceState())) {
        return;
    }
    ESP_LOGI(TAG, "hang up (%s)", reason);
    app.EndChat();  // state-agnostic: Connecting/Listening/Speaking -> Idle
}

void MemomateBaseLink::HandleEventFrame(const uint8_t* src, const uint8_t* data,
                                        int len) {
    if (len != (int)sizeof(base_event_t)) {
        ESP_LOGW(TAG, "event tag but %d bytes (expected %d)", len,
                 (int)sizeof(base_event_t));
        return;
    }
    base_event_t ev;
    memcpy(&ev, data, sizeof(ev));
    ev.number[sizeof(ev.number) - 1] = '\0';  // defensive NUL

    // Ack first (the base judges liveness by acks), then act.
    EnsurePeer(src);
    base_ack_t ack = {.magic = BASE_ACK_MAGIC, .ack_seq = ev.seq};
    esp_now_send(src, (const uint8_t*)&ack, sizeof(ack));

    DeviceState state = Application::GetInstance().GetDeviceState();

    switch (ev.type) {
        case BASE_EVT_HEARTBEAT: {
            // digit carries the live hook level (1 = off-hook, 0 = on-hook).
            // Reconcile our conversation state against the cradle so a dropped
            // OFF_HOOK/ON_HOOK edge still converges — the safety net.
            bool off_hook = (ev.digit != 0);
            ESP_LOGD(TAG, "RX HEARTBEAT seq=%lu hook=%s", (unsigned long)ev.seq,
                     off_hook ? "off" : "on");
            if (!off_hook && InConversation(state)) {
                // On-hook but still in a call → hang up (lost an ON_HOOK edge).
                // Always-safe direction: do it immediately.
                HangupIfActive("heartbeat: on-hook while active");
            } else if (off_hook && state == kDeviceStateIdle) {
                // Off-hook but idle → start (lost an OFF_HOOK edge). Guard
                // against re-issuing every 3s if a connect is stuck / the level
                // stays asserted.
                int64_t now = esp_timer_get_time();
                if (now - last_autostart_us_ > kAutoStartGuardUs) {
                    StartChatIfIdle("heartbeat: off-hook while idle");
                }
            }
            break;
        }

        case BASE_EVT_OFF_HOOK:
            ESP_LOGI(TAG, "OFF_HOOK edge (seq=%lu, state=%d)",
                     (unsigned long)ev.seq, (int)state);
            // Lift handset: start only if idle; never tear down an active call.
            StartChatIfIdle("OFF_HOOK edge");
            break;

        case BASE_EVT_ON_HOOK:
            ESP_LOGI(TAG, "ON_HOOK edge (seq=%lu, state=%d)",
                     (unsigned long)ev.seq, (int)state);
            // Replace handset: clean hang-up from ANY conversation state →
            // Idle. EndChat (not ToggleChatState) so Speaking doesn't fall back
            // to Listening — this is the bug fix.
            HangupIfActive("ON_HOOK edge");
            break;

        case BASE_EVT_DIGIT:
            // Persona/number->agent selection needs the platform; log only.
            ESP_LOGI(TAG, "DIGIT %u (seq=%lu)", ev.digit, (unsigned long)ev.seq);
            break;

        case BASE_EVT_NUMBER:
            ESP_LOGI(TAG, "NUMBER '%s' (seq=%lu)", ev.number,
                     (unsigned long)ev.seq);
            break;

        default:
            ESP_LOGW(TAG, "unknown event type=%u seq=%lu", ev.type,
                     (unsigned long)ev.seq);
            break;
    }
}
