// memomate_control_link.h — persistent "control connection" for the handset C6.
//
// A second, always-on dial-out WebSocket (alongside the on-demand conversation
// WS) so the platform can reach an idle phone and make it ring. Implements the
// device side of ADR memomate-proactive-notification-ring + ADR 0014:
//   - connects to the OTA-provisioned control endpoint (websocket.control:
//     {url, token, version}) over the same TLS WS stack as the conversation
//     socket, with keepalive pings + exponential-backoff reconnect;
//   - receives {type:"notification", render:"ring", notificationId, ...},
//     stashes the notificationId for the next hello, fires an ESP-NOW RING to
//     the base, lights the 振铃 灯语, and reports lifecycle receipts;
//   - rings for up to 30s: answered (device leaves Idle) -> in_call; nobody
//     answers -> notification.missed.
//
// Self-contained to the MemoMate Telephone board. Start AFTER the STA connects;
// the task waits for OTA to provision the control config, so it's safe to start
// before the first OTA poll completes.
#ifndef MEMOMATE_CONTROL_LINK_H_
#define MEMOMATE_CONTROL_LINK_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WebSocket;

class MemomateControlLink {
public:
    // Sends an ESP-NOW RING(start/stop) to the base. Wired by the board to the
    // base link. Returns true if the frame was sent.
    using RingSender = std::function<bool(bool /*start*/)>;

    // Idempotent. Spawns the connect/reconnect task. Returns false only if the
    // task could not be created.
    bool Start();
    void SetRingSender(RingSender sender) { ring_sender_ = std::move(sender); }

private:
    static void TaskTrampoline(void* arg);
    void Run();                                   // connect/reconnect loop
    void OnMessage(const char* data, size_t len); // control-WS JSON in
    void HandleNotification(const char* notification_id, const char* render);
    void ServiceRing();                           // answered / missed FSM tick
    void StopRing(bool answered, bool send_missed);
    bool SendJson(const std::string& json);       // ws_-guarded send

    RingSender ring_sender_;
    TaskHandle_t task_ = nullptr;
    bool started_ = false;

    std::mutex ws_mutex_;        // guards ws_ pointer for sends vs reconnect
    WebSocket* ws_ = nullptr;    // valid only between connect and disconnect
    std::atomic<bool> connected_{false};

    std::mutex ring_mutex_;      // guards the ring fields below
    bool ringing_ = false;
    int64_t ring_started_us_ = 0;
    int64_t ring_last_resend_us_ = 0;  // periodic RING re-send (self-heal)
    std::string ring_nid_;
};

#endif  // MEMOMATE_CONTROL_LINK_H_
