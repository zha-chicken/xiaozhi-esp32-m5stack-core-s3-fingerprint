#ifndef FPC1020A_FINGERPRINT_H
#define FPC1020A_FINGERPRINT_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class McpServer;

class Fpc1020aFingerprint {
public:
    enum class ProbeProtocol {
        Fpc1020a,
        Finger2,
    };

    struct MatchResult {
        bool matched = false;
        uint8_t ack = 0;
        uint16_t user_id = 0;
        uint8_t permission = 0;
        uint16_t score = 0;
        std::string status;
    };

    Fpc1020aFingerprint(uart_port_t uart_num, gpio_num_t rx_pin, gpio_num_t tx_pin,
                         int baud_rate, ProbeProtocol probe_protocol);
    ~Fpc1020aFingerprint();

    bool Begin();
    bool ready() const { return ready_.load(); }

    bool GetUserCount(uint16_t& count);
    MatchResult Verify(uint32_t timeout_ms);
    bool Enroll(uint8_t id, uint8_t permission, std::string* error);
    bool StartEnroll(uint8_t id, uint8_t permission, std::string* error);
    bool DeleteFinger(uint8_t id, std::string* error);
    bool DeleteAllFingers(std::string* error);

    void RegisterTools(McpServer& mcp);
    void StartAutoUnlock();
    void SetAutoUnlockEnabled(bool enabled) { auto_unlock_enabled_.store(enabled); }
    bool auto_unlock_enabled() const { return auto_unlock_enabled_.load(); }
    void UnlockFor(uint32_t duration_ms);
    void Lock();
    bool IsUnlocked() const;
    int64_t unlocked_until_ms() const { return unlocked_until_ms_.load(); }

private:
    uart_port_t uart_num_;
    gpio_num_t rx_pin_;
    gpio_num_t tx_pin_;
    int baud_rate_;
    std::mutex uart_mutex_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> auto_unlock_enabled_{true};
    std::atomic<bool> enrollment_in_progress_{false};
    std::atomic<bool> enroll_requested_{false};
    std::atomic<bool> stop_task_{false};
    TaskHandle_t auto_unlock_task_handle_ = nullptr;
    bool driver_installed_ = false;
    ProbeProtocol probe_protocol_;
    ProbeProtocol active_protocol_ = ProbeProtocol::Fpc1020a;
    uint16_t finger2_capacity_ = 0;

    std::atomic<int> last_match_user_id_{0};
    std::atomic<int> last_match_permission_{0};
    std::atomic<int> last_match_score_{0};
    std::atomic<int64_t> last_match_ms_{0};
    std::atomic<int> last_enroll_id_{-1};
    std::atomic<int> last_enroll_success_{0};
    std::atomic<int64_t> last_enroll_ms_{0};
    std::atomic<int64_t> unlocked_until_ms_{0};
    std::atomic<int> pending_enroll_id_{0};
    std::atomic<int> pending_enroll_permission_{1};

    bool InstallUart();
    void UninstallUart();
    bool SendCommandLocked(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3,
                           uint32_t timeout_ms, uint8_t response[8]);
    bool ReadUserCountLocked(uint16_t& count, uint32_t timeout_ms);
    bool CommandOkLocked(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3,
                         uint32_t timeout_ms, uint8_t* ack, std::string* error);

    bool BeginFpc1020aLocked(uint16_t& count);
    bool BeginFinger2Locked(uint16_t& count);
    bool Finger2TransceiveLocked(std::vector<uint8_t>& response, uint8_t cmd,
                                 const uint8_t* payload = nullptr, uint16_t payload_len = 0,
                                 uint32_t timeout_ms = 1200);
    bool Finger2WriteCommandLocked(uint8_t cmd, const uint8_t* payload, uint16_t payload_len);
    bool Finger2ReadResponseLocked(std::vector<uint8_t>& response, uint32_t timeout_ms);
    bool Finger2ReadValidTemplatesLocked(uint16_t& count);
    MatchResult VerifyFinger2(uint32_t timeout_ms);
    bool EnrollFinger2(uint16_t id, std::string* error);
    bool DeleteFinger2(uint16_t id, std::string* error);
    bool DeleteAllFinger2(std::string* error);

    static const char* AckToString(uint8_t ack);
    static const char* Finger2ConfirmToString(uint8_t confirm);
    static const char* ProtocolToString(ProbeProtocol protocol);
    static void AutoUnlockTaskEntry(void* arg);
    void AutoUnlockTask();
    void RunEnrollJob(uint8_t id, uint8_t permission);
    void HandleMatchedFingerprint(const MatchResult& result);
};

#endif // FPC1020A_FINGERPRINT_H
