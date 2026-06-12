// memomate_base_link.h — ESP-NOW receiver for the cordless-phone base link.
//
// The handset C6 (this board) pairs with the base-station S3 that decodes the
// rotary dial + hook switch and ships high-level events (OFF_HOOK / ON_HOOK /
// DIGIT / NUMBER) over ESP-NOW. This module:
//   - answers the base's DISCOVERY broadcast with a PAIR_REPLY carrying the
//     handset's current WiFi channel (no channel scanning on the handset side),
//   - receives base_event_t frames, acks each, and maps them to actions.
//
// Self-contained to the MemoMate Telephone board. Init AFTER WiFi STA connects
// (the ESP-NOW peer rides the STA channel). ESP-NOW coexists with xiaozhi's
// WS-over-STA — it does not touch the TCP/IP stack. Wire protocol is shared
// byte-for-byte with the base station via espnow_link.h.
//
// See ADR: haolab.ai/docs/research/firmware-mods/memomate-espnow-base-link.md

#ifndef MEMOMATE_BASE_LINK_H_
#define MEMOMATE_BASE_LINK_H_

#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdint.h>

class MemomateBaseLink {
public:
    // Idempotent. Safe to call once the STA is connected; a second call is a
    // no-op. Returns false if ESP-NOW could not be brought up.
    bool Start();

private:
    // C-style ESP-NOW callbacks dispatch to the singleton instance.
    static void OnRecvTrampoline(const esp_now_recv_info_t* info,
                                 const uint8_t* data, int len);
    static void DispatchTaskTrampoline(void* arg);

    void HandleQueuedFrame();
    void HandlePairFrame(const uint8_t* src, const uint8_t* data, int len);
    void HandleEventFrame(const uint8_t* src, const uint8_t* data, int len);
    void EnsurePeer(const uint8_t* mac);

    // Level/edge → action helpers (shared by edge events and heartbeat
    // reconciliation).
    void StartChatIfIdle(const char* reason);
    void HangupIfActive(const char* reason);

    bool started_ = false;
    QueueHandle_t rx_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    // Anti-thrash guard for heartbeat-driven auto-start: don't re-issue a start
    // within this window of the last one (covers a stuck off-hook level racing
    // the connect, so we don't hammer OpenAudioChannel every 3s).
    int64_t last_autostart_us_ = 0;
};

#endif  // MEMOMATE_BASE_LINK_H_
