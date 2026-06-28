#include "fpc1020a_fingerprint.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "Fpc1020a"

namespace {
constexpr uint8_t kAckSuccess = 0x00;
constexpr uint8_t kAckFail = 0x01;
constexpr uint8_t kAckFull = 0x04;
constexpr uint8_t kAckNoUser = 0x05;
constexpr uint8_t kAckUserExist = 0x07;
constexpr uint8_t kAckTimeout = 0x08;
constexpr uint8_t kAckGoOut = 0x0F;

constexpr uint8_t kPermissionGuest = 0x01;
constexpr uint8_t kPermissionNormal = 0x02;
constexpr uint8_t kPermissionMaster = 0x03;

constexpr uint8_t kCmdHead = 0xF5;
constexpr uint8_t kCmdTail = 0xF5;
constexpr uint8_t kCmdAdd1 = 0x01;
constexpr uint8_t kCmdAdd2 = 0x02;
constexpr uint8_t kCmdAdd3 = 0x03;
constexpr uint8_t kCmdDelete = 0x04;
constexpr uint8_t kCmdDeleteAll = 0x05;
constexpr uint8_t kCmdUserCount = 0x09;
constexpr uint8_t kCmdMatch = 0x0C;

constexpr int kPacketSize = 8;
constexpr int kUserMaxCount = 50;
constexpr int kEnrollSamples = 6;
constexpr size_t kUartBufferSize = 256;

constexpr int kAutoUnlockStackSize = 4096;
constexpr int kAutoUnlockPriority = 4;
constexpr uint32_t kAutoUnlockMatchTimeoutMs = 700;
constexpr uint32_t kAutoUnlockCooldownMs = 3000;
constexpr uint32_t kConversationUnlockDurationMs = 60000;

constexpr uint16_t kFinger2PacketHeader = 0xEF01;
constexpr uint32_t kFinger2DefaultAddress = 0xFFFFFFFF;
constexpr uint8_t kFinger2PidCommand = 0x01;
constexpr uint8_t kFinger2PidAck = 0x07;
constexpr uint8_t kFinger2CmdReadSystemParameter = 0x0F;
constexpr uint8_t kFinger2CmdReadValidTemplateNumber = 0x1D;
constexpr uint8_t kFinger2CmdGetEnrollImage = 0x29;
constexpr uint8_t kFinger2CmdGenerateCharacter = 0x02;
constexpr uint8_t kFinger2CmdGenerateTemplate = 0x05;
constexpr uint8_t kFinger2CmdStoreTemplate = 0x06;
constexpr uint8_t kFinger2CmdDeleteTemplate = 0x0C;
constexpr uint8_t kFinger2CmdEmpty = 0x0D;
constexpr uint8_t kFinger2CmdAutoEnroll = 0x31;
constexpr uint8_t kFinger2CmdAutoIdentify = 0x32;
constexpr uint8_t kFinger2CmdCancel = 0x30;
constexpr uint8_t kFinger2CmdActivateModule = 0xD4;
constexpr uint8_t kFinger2CmdGetFirmwareVersion = 0xD7;
constexpr uint8_t kFinger2CmdSetWorkMode = 0xD2;
constexpr uint8_t kFinger2CmdSetSleepTime = 0xD0;
constexpr uint8_t kFinger2ConfirmOk = 0x00;
constexpr uint8_t kFinger2ConfirmNotFound = 0x09;
constexpr uint8_t kFinger2ConfirmUnmatched = 0x08;
constexpr uint8_t kFinger2ConfirmPassiveActivation = 0xFF;
constexpr uint8_t kFinger2AutoIdentifyStageResult = 0x05;
constexpr uint8_t kFinger2AutoEnrollStageStoreTemplate = 0x06;
constexpr uint8_t kFinger2AutoEnrollStored = 0xF2;
constexpr uint16_t kFinger2AutoFlagDontReturnIntermediate = 1U << 2;
constexpr uint16_t kFinger2AutoEnrollFlagAllowOverwrite = 1U << 3;
constexpr uint16_t kFinger2MaxTemplates = 200;
constexpr uint16_t kFinger2MaxPacketSize = 128;

uint8_t PacketChecksum(const uint8_t packet[kPacketSize]) {
    uint8_t checksum = 0;
    for (int i = 1; i < 6; i++) {
        checksum ^= packet[i];
    }
    return checksum;
}

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

uint16_t Sum16(const uint8_t* data, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

} // namespace

Fpc1020aFingerprint::Fpc1020aFingerprint(uart_port_t uart_num, gpio_num_t rx_pin,
                                         gpio_num_t tx_pin, int baud_rate,
                                         ProbeProtocol probe_protocol)
    : uart_num_(uart_num), rx_pin_(rx_pin), tx_pin_(tx_pin), baud_rate_(baud_rate),
      probe_protocol_(probe_protocol) {
}

Fpc1020aFingerprint::~Fpc1020aFingerprint() {
    stop_task_.store(true);
    if (auto_unlock_task_handle_ != nullptr) {
        vTaskDelete(auto_unlock_task_handle_);
        auto_unlock_task_handle_ = nullptr;
    }
    UninstallUart();
}

bool Fpc1020aFingerprint::InstallUart() {
    uart_config_t uart_config = {
        .baud_rate = baud_rate_,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .allow_pd = 0,
        },
    };

    if (!uart_is_driver_installed(uart_num_)) {
        esp_err_t err = uart_driver_install(uart_num_, kUartBufferSize, 0, 0, nullptr, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UART%d driver install failed: %s", uart_num_, esp_err_to_name(err));
            return false;
        }
        driver_installed_ = true;
    }

    esp_err_t err = uart_param_config(uart_num_, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART%d config failed: %s", uart_num_, esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(uart_num_, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART%d pin setup failed: %s", uart_num_, esp_err_to_name(err));
        return false;
    }
    uart_flush_input(uart_num_);
    return true;
}

void Fpc1020aFingerprint::UninstallUart() {
    if (driver_installed_ && uart_is_driver_installed(uart_num_)) {
        uart_driver_delete(uart_num_);
    }
    driver_installed_ = false;
}

bool Fpc1020aFingerprint::Begin() {
    if (!InstallUart()) {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    std::lock_guard<std::mutex> lock(uart_mutex_);
    uint16_t count = 0;
    bool detected = false;
    if (probe_protocol_ == ProbeProtocol::Fpc1020a) {
        detected = BeginFpc1020aLocked(count);
    } else {
        detected = BeginFinger2Locked(count);
    }

    if (!detected) {
        ESP_LOGI(TAG, "%s not detected on UART%d RX=%d TX=%d baud=%d",
                 ProtocolToString(probe_protocol_), uart_num_, rx_pin_, tx_pin_, baud_rate_);
        UninstallUart();
        return false;
    }

    ready_.store(true);
    ESP_LOGI(TAG, "%s ready on UART%d RX=%d TX=%d baud=%d users=%u",
             ProtocolToString(active_protocol_), uart_num_, rx_pin_, tx_pin_, baud_rate_, count);
    return true;
}

bool Fpc1020aFingerprint::BeginFpc1020aLocked(uint16_t& count) {
    uint16_t first_count = 0;
    uint16_t second_count = 0;
    if (!ReadUserCountLocked(first_count, 700)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!ReadUserCountLocked(second_count, 700) || first_count != second_count || second_count > kUserMaxCount) {
        return false;
    }
    count = second_count;
    active_protocol_ = ProbeProtocol::Fpc1020a;
    return true;
}

bool Fpc1020aFingerprint::SendCommandLocked(uint8_t cmd, uint8_t p1, uint8_t p2,
                                            uint8_t p3, uint32_t timeout_ms,
                                            uint8_t response[kPacketSize]) {
    if (!uart_is_driver_installed(uart_num_)) {
        return false;
    }

    uint8_t packet[kPacketSize] = {kCmdHead, cmd, p1, p2, p3, 0x00, 0x00, kCmdTail};
    packet[6] = PacketChecksum(packet);

    std::memset(response, 0, kPacketSize);
    uart_flush_input(uart_num_);

    int written = uart_write_bytes(uart_num_, packet, kPacketSize);
    if (written != kPacketSize) {
        ESP_LOGW(TAG, "UART%d write failed: %d/%d", uart_num_, written, kPacketSize);
        return false;
    }
    uart_wait_tx_done(uart_num_, pdMS_TO_TICKS(100));

    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    int index = 0;
    while (esp_timer_get_time() < deadline) {
        int64_t remaining_ms = (deadline - esp_timer_get_time()) / 1000;
        TickType_t wait_ticks = pdMS_TO_TICKS(std::max<int64_t>(1, std::min<int64_t>(20, remaining_ms)));

        uint8_t ch = 0;
        int read = uart_read_bytes(uart_num_, &ch, 1, wait_ticks);
        if (read <= 0) {
            continue;
        }

        if (index == 0 && ch != kCmdHead) {
            continue;
        }
        response[index++] = ch;
        if (index == kPacketSize) {
            break;
        }
    }

    if (index != kPacketSize) {
        return false;
    }
    if (response[0] != kCmdHead || response[7] != kCmdTail) {
        return false;
    }
    if (response[1] != cmd) {
        return false;
    }
    if (PacketChecksum(response) != response[6]) {
        return false;
    }
    return true;
}

bool Fpc1020aFingerprint::ReadUserCountLocked(uint16_t& count, uint32_t timeout_ms) {
    uint8_t response[kPacketSize] = {};
    if (!SendCommandLocked(kCmdUserCount, 0, 0, 0, timeout_ms, response)) {
        return false;
    }
    if (response[4] != kAckSuccess) {
        return false;
    }
    count = response[3];
    return true;
}

bool Fpc1020aFingerprint::Finger2WriteCommandLocked(uint8_t cmd, const uint8_t* payload,
                                                    uint16_t payload_len) {
    if (!uart_is_driver_installed(uart_num_)) {
        return false;
    }
    if (payload_len + 3 > UINT16_MAX) {
        return false;
    }

    std::vector<uint8_t> packet(12 + payload_len);
    packet[0] = static_cast<uint8_t>(kFinger2PacketHeader >> 8);
    packet[1] = static_cast<uint8_t>(kFinger2PacketHeader & 0xFF);
    packet[2] = static_cast<uint8_t>(kFinger2DefaultAddress >> 24);
    packet[3] = static_cast<uint8_t>(kFinger2DefaultAddress >> 16);
    packet[4] = static_cast<uint8_t>(kFinger2DefaultAddress >> 8);
    packet[5] = static_cast<uint8_t>(kFinger2DefaultAddress & 0xFF);
    packet[6] = kFinger2PidCommand;
    uint16_t length = 3 + payload_len;
    packet[7] = static_cast<uint8_t>(length >> 8);
    packet[8] = static_cast<uint8_t>(length & 0xFF);
    packet[9] = cmd;
    if (payload != nullptr && payload_len > 0) {
        std::memcpy(packet.data() + 10, payload, payload_len);
    }
    uint16_t sum = Sum16(packet.data() + 6, 4 + payload_len);
    packet[10 + payload_len] = static_cast<uint8_t>(sum >> 8);
    packet[11 + payload_len] = static_cast<uint8_t>(sum & 0xFF);

    uart_flush_input(uart_num_);
    int written = uart_write_bytes(uart_num_, packet.data(), packet.size());
    if (written != static_cast<int>(packet.size())) {
        return false;
    }
    uart_wait_tx_done(uart_num_, pdMS_TO_TICKS(100));
    return true;
}

bool Fpc1020aFingerprint::Finger2ReadResponseLocked(std::vector<uint8_t>& response,
                                                    uint32_t timeout_ms) {
    response.clear();
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    uint8_t header[9] = {};
    int index = 0;

    while (esp_timer_get_time() < deadline && index < 9) {
        int64_t remaining_ms = (deadline - esp_timer_get_time()) / 1000;
        TickType_t wait_ticks = pdMS_TO_TICKS(std::max<int64_t>(1, std::min<int64_t>(20, remaining_ms)));
        uint8_t ch = 0;
        int read = uart_read_bytes(uart_num_, &ch, 1, wait_ticks);
        if (read <= 0) {
            continue;
        }

        if (index == 0 && ch != static_cast<uint8_t>(kFinger2PacketHeader >> 8)) {
            continue;
        }
        if (index == 1 && ch != static_cast<uint8_t>(kFinger2PacketHeader & 0xFF)) {
            index = (ch == static_cast<uint8_t>(kFinger2PacketHeader >> 8)) ? 1 : 0;
            header[0] = ch;
            continue;
        }
        header[index++] = ch;
    }

    if (index != 9 || header[6] != kFinger2PidAck) {
        return false;
    }

    uint16_t length = (static_cast<uint16_t>(header[7]) << 8) | header[8];
    if (length < 3 || length > kFinger2MaxPacketSize) {
        return false;
    }

    response.resize(9 + length);
    std::memcpy(response.data(), header, sizeof(header));

    size_t offset = 9;
    while (offset < response.size() && esp_timer_get_time() < deadline) {
        int64_t remaining_ms = (deadline - esp_timer_get_time()) / 1000;
        TickType_t wait_ticks = pdMS_TO_TICKS(std::max<int64_t>(1, std::min<int64_t>(20, remaining_ms)));
        int read = uart_read_bytes(uart_num_, response.data() + offset, response.size() - offset, wait_ticks);
        if (read > 0) {
            offset += read;
        }
    }
    if (offset != response.size()) {
        return false;
    }

    uint16_t sum = Sum16(response.data() + 6, response.size() - 8);
    uint16_t received_sum = (static_cast<uint16_t>(response[response.size() - 2]) << 8) |
                            response[response.size() - 1];
    return sum == received_sum;
}

bool Fpc1020aFingerprint::Finger2TransceiveLocked(std::vector<uint8_t>& response, uint8_t cmd,
                                                  const uint8_t* payload, uint16_t payload_len,
                                                  uint32_t timeout_ms) {
    if (!Finger2WriteCommandLocked(cmd, payload, payload_len)) {
        return false;
    }

    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (esp_timer_get_time() < deadline) {
        uint32_t remaining_ms = static_cast<uint32_t>(std::max<int64_t>(1, (deadline - esp_timer_get_time()) / 1000));
        if (!Finger2ReadResponseLocked(response, std::min<uint32_t>(remaining_ms, 700)) || response.size() < 10) {
            continue;
        }

        uint8_t confirm = response[9];
        if (confirm == kFinger2ConfirmPassiveActivation && response.size() == 12) {
            ESP_LOGD(TAG, "Finger2 wakeup packet skipped");
            continue;
        }
        return confirm == kFinger2ConfirmOk;
    }

    return false;
}

bool Fpc1020aFingerprint::BeginFinger2Locked(uint16_t& count) {
    std::vector<uint8_t> response;
    uint8_t firmware_version = 0;
    for (int retry = 0; retry < 3 && firmware_version == 0; retry++) {
        bool active = Finger2TransceiveLocked(response, kFinger2CmdActivateModule, nullptr, 0, 900);
        if (active) {
            Finger2TransceiveLocked(response, kFinger2CmdCancel, nullptr, 0, 500);
        }
        if (active &&
            Finger2TransceiveLocked(response, kFinger2CmdGetFirmwareVersion, nullptr, 0, 900) &&
            response.size() >= 13) {
            firmware_version = response[10];
        }
        if (firmware_version == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (firmware_version == 0) {
        return false;
    }

    // Finger2 official library documents 1 as always-on mode. Local enrollment
    // can last up to a minute; timed sleep mode kept producing passive wakeup
    // packets and no stored template.
    uint8_t work_mode = 1;
    if (!Finger2TransceiveLocked(response, kFinger2CmdSetWorkMode, &work_mode, 1, 1200)) {
        return false;
    }
    uint8_t sleep_time = 10;
    if (!Finger2TransceiveLocked(response, kFinger2CmdSetSleepTime, &sleep_time, 1, 1200)) {
        return false;
    }
    if (!Finger2TransceiveLocked(response, kFinger2CmdReadSystemParameter, nullptr, 0, 1200) ||
        response.size() != 28) {
        return false;
    }

    finger2_capacity_ = (static_cast<uint16_t>(response[14]) << 8) | response[15];
    if (finger2_capacity_ == 0 || finger2_capacity_ > kFinger2MaxTemplates) {
        finger2_capacity_ = kFinger2MaxTemplates;
    }
    if (!Finger2ReadValidTemplatesLocked(count)) {
        count = 0;
    }
    active_protocol_ = ProbeProtocol::Finger2;
    ESP_LOGI(TAG, "Finger2 firmware=0x%02X capacity=%u", firmware_version, finger2_capacity_);
    return true;
}

bool Fpc1020aFingerprint::Finger2ReadValidTemplatesLocked(uint16_t& count) {
    std::vector<uint8_t> response;
    if (!Finger2TransceiveLocked(response, kFinger2CmdActivateModule, nullptr, 0, 1200) ||
        !Finger2TransceiveLocked(response, kFinger2CmdReadValidTemplateNumber, nullptr, 0, 1200) ||
        response.size() != 14) {
        return false;
    }
    count = (static_cast<uint16_t>(response[10]) << 8) | response[11];
    return true;
}

bool Fpc1020aFingerprint::GetUserCount(uint16_t& count) {
    if (!ready()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(uart_mutex_);
    if (active_protocol_ == ProbeProtocol::Finger2) {
        return Finger2ReadValidTemplatesLocked(count);
    }
    return ReadUserCountLocked(count, 1200);
}

void Fpc1020aFingerprint::UnlockFor(uint32_t duration_ms) {
    unlocked_until_ms_.store(NowMs() + duration_ms);
}

void Fpc1020aFingerprint::Lock() {
    unlocked_until_ms_.store(0);
}

bool Fpc1020aFingerprint::IsUnlocked() const {
    return ready() && NowMs() < unlocked_until_ms_.load();
}

bool Fpc1020aFingerprint::CommandOkLocked(uint8_t cmd, uint8_t p1, uint8_t p2,
                                          uint8_t p3, uint32_t timeout_ms,
                                          uint8_t* ack, std::string* error) {
    uint8_t response[kPacketSize] = {};
    if (!SendCommandLocked(cmd, p1, p2, p3, timeout_ms, response)) {
        if (error != nullptr) {
            *error = "fingerprint sensor did not respond";
        }
        return false;
    }

    if (ack != nullptr) {
        *ack = response[4];
    }
    if (response[4] != kAckSuccess) {
        if (error != nullptr) {
            *error = AckToString(response[4]);
        }
        return false;
    }
    return true;
}

Fpc1020aFingerprint::MatchResult Fpc1020aFingerprint::Verify(uint32_t timeout_ms) {
    if (active_protocol_ == ProbeProtocol::Finger2) {
        return VerifyFinger2(timeout_ms);
    }

    MatchResult result;
    if (!ready()) {
        result.status = "not_ready";
        return result;
    }

    std::lock_guard<std::mutex> lock(uart_mutex_);
    uint8_t response[kPacketSize] = {};
    if (!SendCommandLocked(kCmdMatch, 0, 0, 0, timeout_ms, response)) {
        result.ack = kAckTimeout;
        result.status = "timeout";
        return result;
    }

    result.ack = response[4];
    if (response[4] == kAckNoUser) {
        result.status = "no_user";
        return result;
    }
    if (response[4] == kAckTimeout) {
        result.status = "timeout";
        return result;
    }

    uint16_t id = (static_cast<uint16_t>(response[2]) << 8) | response[3];
    uint8_t permission = response[4];
    if (id != 0 && permission >= kPermissionGuest && permission <= kPermissionMaster) {
        result.matched = true;
        result.user_id = id;
        result.permission = permission;
        result.status = "matched";
        return result;
    }

    result.status = AckToString(response[4]);
    return result;
}

bool Fpc1020aFingerprint::Enroll(uint8_t id, uint8_t permission, std::string* error) {
    if (active_protocol_ == ProbeProtocol::Finger2) {
        return EnrollFinger2(id, error);
    }

    if (!ready()) {
        if (error != nullptr) {
            *error = "fingerprint sensor is not ready";
        }
        return false;
    }
    if (id == 0 || id > kUserMaxCount) {
        if (error != nullptr) {
            *error = "fingerprint id must be 1-50";
        }
        return false;
    }
    if (permission < kPermissionGuest || permission > kPermissionMaster) {
        if (error != nullptr) {
            *error = "permission must be 1-3";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(uart_mutex_);
    for (int sample = 0; sample < kEnrollSamples; sample++) {
        uint8_t cmd = sample == 0 ? kCmdAdd1 : (sample < kEnrollSamples - 1 ? kCmdAdd2 : kCmdAdd3);
        bool step_ok = false;
        for (int attempt = 0; attempt < 8 && !step_ok; attempt++) {
            uint8_t response[kPacketSize] = {};
            if (SendCommandLocked(cmd, 0, id, permission, 2000, response)) {
                step_ok = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(400));
        }

        if (!step_ok) {
            if (error != nullptr) {
                *error = "fingerprint enrollment timed out at sample " + std::to_string(sample + 1);
            }
            return false;
        }
        ESP_LOGI(TAG, "Enroll finger id=%u sample=%d/%d", id, sample + 1, kEnrollSamples);
    }
    return true;
}

bool Fpc1020aFingerprint::StartEnroll(uint8_t id, uint8_t permission, std::string* error) {
    if (!ready()) {
        if (error != nullptr) {
            *error = "fingerprint sensor is not ready";
        }
        return false;
    }

    if (active_protocol_ == ProbeProtocol::Finger2) {
        uint16_t capacity = finger2_capacity_ ? finger2_capacity_ : kFinger2MaxTemplates;
        if (id >= capacity) {
            if (error != nullptr) {
                *error = "fingerprint id must be within Finger2 capacity";
            }
            return false;
        }
    } else {
        if (id == 0 || id > kUserMaxCount) {
            if (error != nullptr) {
                *error = "fingerprint id must be 1-50";
            }
            return false;
        }
        if (permission < kPermissionGuest || permission > kPermissionMaster) {
            if (error != nullptr) {
                *error = "permission must be 1-3";
            }
            return false;
        }
    }

    if (enrollment_in_progress_.exchange(true)) {
        if (error != nullptr) {
            *error = "fingerprint enrollment is already running";
        }
        return false;
    }

    if (auto_unlock_task_handle_ == nullptr) {
        enrollment_in_progress_.store(false);
        if (error != nullptr) {
            *error = "fingerprint service is not running";
        }
        return false;
    }

    pending_enroll_id_.store(id);
    pending_enroll_permission_.store(permission);
    enroll_requested_.store(true);
    return true;
}

bool Fpc1020aFingerprint::DeleteFinger(uint8_t id, std::string* error) {
    if (active_protocol_ == ProbeProtocol::Finger2) {
        return DeleteFinger2(id, error);
    }

    if (!ready()) {
        if (error != nullptr) {
            *error = "fingerprint sensor is not ready";
        }
        return false;
    }
    if (id == 0 || id > kUserMaxCount) {
        if (error != nullptr) {
            *error = "fingerprint id must be 1-50";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(uart_mutex_);
    uint8_t ack = 0;
    return CommandOkLocked(kCmdDelete, 0, id, 0, 1200, &ack, error);
}

bool Fpc1020aFingerprint::DeleteAllFingers(std::string* error) {
    if (active_protocol_ == ProbeProtocol::Finger2) {
        return DeleteAllFinger2(error);
    }

    if (!ready()) {
        if (error != nullptr) {
            *error = "fingerprint sensor is not ready";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(uart_mutex_);
    uint8_t ack = 0;
    return CommandOkLocked(kCmdDeleteAll, 0, 0, 0, 1200, &ack, error);
}

Fpc1020aFingerprint::MatchResult Fpc1020aFingerprint::VerifyFinger2(uint32_t timeout_ms) {
    MatchResult result;
    if (!ready()) {
        result.status = "not_ready";
        return result;
    }

    std::lock_guard<std::mutex> lock(uart_mutex_);
    std::vector<uint8_t> response;
    if (!Finger2TransceiveLocked(response, kFinger2CmdActivateModule, nullptr, 0, 1200)) {
        result.status = "not_active";
        return result;
    }

    uint16_t flags = kFinger2AutoFlagDontReturnIntermediate;
    uint8_t payload[5] = {
        0x00,  // security level 0
        0xFF,
        0xFF,  // 1:N identify against all pages
        static_cast<uint8_t>(flags >> 8),
        static_cast<uint8_t>(flags & 0xFF),
    };

    if (!Finger2WriteCommandLocked(kFinger2CmdAutoIdentify, payload, sizeof(payload))) {
        result.status = "write_failed";
        return result;
    }

    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (esp_timer_get_time() < deadline) {
        uint32_t remaining_ms = static_cast<uint32_t>(std::max<int64_t>(1, (deadline - esp_timer_get_time()) / 1000));
        if (!Finger2ReadResponseLocked(response, std::min<uint32_t>(remaining_ms, 1200))) {
            result.ack = 0xF9;
            result.status = "timeout";
            return result;
        }

        uint8_t confirm = response[9];
        result.ack = confirm;
        if (response.size() == 17 && response[10] == kFinger2AutoIdentifyStageResult) {
            result.matched = confirm == kFinger2ConfirmOk;
            result.user_id = (static_cast<uint16_t>(response[11]) << 8) | response[12];
            result.score = (static_cast<uint16_t>(response[13]) << 8) | response[14];
            if (result.matched) {
                result.status = "matched";
            } else if (confirm == kFinger2ConfirmNotFound) {
                result.status = "not_found";
            } else if (confirm == kFinger2ConfirmUnmatched) {
                result.status = "unmatched";
            } else {
                result.status = Finger2ConfirmToString(confirm);
            }
            return result;
        }

        if (confirm != kFinger2ConfirmOk && confirm != kFinger2ConfirmPassiveActivation) {
            result.status = Finger2ConfirmToString(confirm);
            return result;
        }
    }

    result.ack = 0xF9;
    result.status = "timeout";
    return result;
}

bool Fpc1020aFingerprint::EnrollFinger2(uint16_t id, std::string* error) {
    if (!ready()) {
        if (error != nullptr) {
            *error = "fingerprint sensor is not ready";
        }
        return false;
    }
    uint16_t capacity = finger2_capacity_ ? finger2_capacity_ : kFinger2MaxTemplates;
    if (id >= capacity) {
        if (error != nullptr) {
            *error = "fingerprint id must be within Finger2 capacity";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(uart_mutex_);
    std::vector<uint8_t> response;
    if (!Finger2TransceiveLocked(response, kFinger2CmdActivateModule, nullptr, 0, 1200)) {
        if (error != nullptr) {
            *error = "fingerprint sensor is not active";
        }
        return false;
    }
    uint8_t work_mode = 1;
    Finger2TransceiveLocked(response, kFinger2CmdSetWorkMode, &work_mode, 1, 1200);

    auto confirm_code = [&response]() -> uint8_t {
        return response.size() > 9 ? response[9] : 0xF9;
    };

    auto wait_for_image = [&](uint8_t sample, uint32_t timeout_ms) -> bool {
        ESP_LOGI(TAG, "Finger2 manual enroll sample=%u waiting for finger", sample);
        uint8_t last_confirm = 0xF9;
        const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
        while (esp_timer_get_time() < deadline) {
            bool ok = Finger2TransceiveLocked(response, kFinger2CmdGetEnrollImage, nullptr, 0, 1200);
            uint8_t confirm = confirm_code();
            if (confirm != last_confirm) {
                ESP_LOGI(TAG, "Finger2 manual enroll sample=%u get_image confirm=0x%02X (%s)",
                         sample, confirm, Finger2ConfirmToString(confirm));
                last_confirm = confirm;
            }
            if (ok) {
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (error != nullptr) {
            *error = "manual enroll sample " + std::to_string(sample) +
                     " image failed after " + Finger2ConfirmToString(last_confirm);
        }
        return false;
    };

    auto wait_for_lift = [&](uint32_t timeout_ms) -> bool {
        ESP_LOGI(TAG, "Finger2 manual enroll waiting for finger lift");
        const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
        while (esp_timer_get_time() < deadline) {
            Finger2TransceiveLocked(response, kFinger2CmdGetEnrollImage, nullptr, 0, 800);
            uint8_t confirm = confirm_code();
            if (confirm == 0x02) {
                ESP_LOGI(TAG, "Finger2 manual enroll lift detected");
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (error != nullptr) {
            *error = "manual enroll timed out waiting for finger lift";
        }
        return false;
    };

    auto gen_char = [&](uint8_t buffer_id) -> bool {
        uint8_t payload[1] = {buffer_id};
        bool ok = Finger2TransceiveLocked(response, kFinger2CmdGenerateCharacter, payload, sizeof(payload), 3000);
        uint8_t confirm = confirm_code();
        ESP_LOGI(TAG, "Finger2 manual enroll gen_char buffer=%u confirm=0x%02X (%s)",
                 buffer_id, confirm, Finger2ConfirmToString(confirm));
        if (!ok && error != nullptr) {
            *error = "manual enroll gen char buffer " + std::to_string(buffer_id) +
                     " failed: " + Finger2ConfirmToString(confirm);
        }
        return ok;
    };

    if (!wait_for_image(1, 20000) || !gen_char(1) || !wait_for_lift(15000) ||
        !wait_for_image(2, 20000) || !gen_char(2)) {
        Finger2TransceiveLocked(response, kFinger2CmdCancel, nullptr, 0, 1200);
        return false;
    }

    bool ok = Finger2TransceiveLocked(response, kFinger2CmdGenerateTemplate, nullptr, 0, 3000);
    uint8_t confirm = confirm_code();
    ESP_LOGI(TAG, "Finger2 manual enroll reg_model confirm=0x%02X (%s)",
             confirm, Finger2ConfirmToString(confirm));
    if (!ok) {
        if (error != nullptr) {
            *error = "manual enroll generate template failed: " + std::string(Finger2ConfirmToString(confirm));
        }
        return false;
    }

    uint8_t store_payload[3] = {
        0x01,
        static_cast<uint8_t>(id >> 8),
        static_cast<uint8_t>(id & 0xFF),
    };
    ok = Finger2TransceiveLocked(response, kFinger2CmdStoreTemplate, store_payload, sizeof(store_payload), 3000);
    confirm = confirm_code();
    ESP_LOGI(TAG, "Finger2 manual enroll store id=%u confirm=0x%02X (%s)",
             id, confirm, Finger2ConfirmToString(confirm));
    if (!ok && error != nullptr) {
        *error = "manual enroll store failed: " + std::string(Finger2ConfirmToString(confirm));
    }
    return ok;
}

bool Fpc1020aFingerprint::DeleteFinger2(uint16_t id, std::string* error) {
    uint16_t capacity = finger2_capacity_ ? finger2_capacity_ : kFinger2MaxTemplates;
    if (id >= capacity) {
        if (error != nullptr) {
            *error = "fingerprint id must be within Finger2 capacity";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(uart_mutex_);
    std::vector<uint8_t> response;
    uint8_t payload[4] = {
        static_cast<uint8_t>(id >> 8),
        static_cast<uint8_t>(id & 0xFF),
        0x00,
        0x01,
    };
    if (!Finger2TransceiveLocked(response, kFinger2CmdActivateModule, nullptr, 0, 1200) ||
        !Finger2TransceiveLocked(response, kFinger2CmdDeleteTemplate, payload, sizeof(payload), 1200)) {
        if (error != nullptr) {
            *error = response.size() > 9 ? Finger2ConfirmToString(response[9]) : "fingerprint delete failed";
        }
        return false;
    }
    return true;
}

bool Fpc1020aFingerprint::DeleteAllFinger2(std::string* error) {
    std::lock_guard<std::mutex> lock(uart_mutex_);
    std::vector<uint8_t> response;
    if (!Finger2TransceiveLocked(response, kFinger2CmdActivateModule, nullptr, 0, 1200) ||
        !Finger2TransceiveLocked(response, kFinger2CmdEmpty, nullptr, 0, 3000)) {
        if (error != nullptr) {
            *error = response.size() > 9 ? Finger2ConfirmToString(response[9]) : "fingerprint clear failed";
        }
        return false;
    }
    return true;
}

void Fpc1020aFingerprint::RegisterTools(McpServer& mcp) {
    mcp.AddTool(
        "self.fingerprint.get_status",
        "Get the local fingerprint unlock sensor status, enrolled user count, auto-unlock state, and last matched user.",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            uint16_t count = 0;
            bool count_ok = GetUserCount(count);

            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "present", ready());
            cJSON_AddStringToObject(json, "protocol", ProtocolToString(active_protocol_));
            cJSON_AddBoolToObject(json, "auto_unlock", auto_unlock_enabled());
            if (active_protocol_ == ProbeProtocol::Finger2) {
                cJSON_AddNumberToObject(json, "capacity", finger2_capacity_);
            }
            if (count_ok) {
                cJSON_AddNumberToObject(json, "user_count", count);
            }
            cJSON_AddNumberToObject(json, "last_match_user_id", last_match_user_id_.load());
            cJSON_AddNumberToObject(json, "last_match_permission", last_match_permission_.load());
            cJSON_AddNumberToObject(json, "last_match_score", last_match_score_.load());
            cJSON_AddNumberToObject(json, "last_match_ms", static_cast<double>(last_match_ms_.load()));
            cJSON_AddBoolToObject(json, "enrollment_in_progress", enrollment_in_progress_.load());
            cJSON_AddNumberToObject(json, "last_enroll_id", last_enroll_id_.load());
            cJSON_AddBoolToObject(json, "last_enroll_success", last_enroll_success_.load() == 1);
            cJSON_AddNumberToObject(json, "last_enroll_ms", static_cast<double>(last_enroll_ms_.load()));
            cJSON_AddBoolToObject(json, "conversation_unlocked", IsUnlocked());
            cJSON_AddNumberToObject(json, "unlocked_until_ms", static_cast<double>(unlocked_until_ms_.load()));
            return json;
        });

    mcp.AddTool(
        "self.fingerprint.verify_once",
        "Wait briefly for a fingerprint and return whether it matches an enrolled local user.",
        PropertyList({
            Property("timeout_ms", kPropertyTypeInteger, 800, 100, 5000),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int timeout_ms = properties["timeout_ms"].value<int>();
            MatchResult match = Verify(static_cast<uint32_t>(timeout_ms));

            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "matched", match.matched);
            cJSON_AddStringToObject(json, "status", match.status.c_str());
            cJSON_AddNumberToObject(json, "user_id", match.user_id);
            cJSON_AddNumberToObject(json, "permission", match.permission);
            cJSON_AddNumberToObject(json, "score", match.score);
            cJSON_AddNumberToObject(json, "ack", match.ack);
            return json;
        });

    mcp.AddTool(
        "self.fingerprint.enroll",
        "Start local fingerprint enrollment for unlock in the background. Keep touching/releasing the same finger as the sensor prompts. "
        "For FPC1020A id is 1-50 and permission is 1 guest, 2 normal, or 3 master. For Finger2 id is 0-199 and permission is ignored.",
        PropertyList({
            Property("id", kPropertyTypeInteger, 1, 0, kFinger2MaxTemplates - 1),
            Property("permission", kPropertyTypeInteger, static_cast<int>(kPermissionGuest), 1, 3),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int id = properties["id"].value<int>();
            int permission = properties["permission"].value<int>();

            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->ShowNotification("Keep finger on sensor", 3000);
            }

            std::string error;
            if (!StartEnroll(static_cast<uint8_t>(id), static_cast<uint8_t>(permission), &error)) {
                throw std::runtime_error(error);
            }
            return std::string("fingerprint enrollment started");
        });

    mcp.AddTool(
        "self.fingerprint.delete_user",
        "Delete one enrolled local fingerprint user by id.",
        PropertyList({
            Property("id", kPropertyTypeInteger, 1, 0, kFinger2MaxTemplates - 1),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int id = properties["id"].value<int>();
            std::string error;
            if (!DeleteFinger(static_cast<uint8_t>(id), &error)) {
                throw std::runtime_error(error);
            }
            return true;
        });

    mcp.AddTool(
        "self.fingerprint.delete_all",
        "Delete all local fingerprint users from the sensor.",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string error;
            if (!DeleteAllFingers(&error)) {
                throw std::runtime_error(error);
            }
            return true;
        });

    mcp.AddTool(
        "self.fingerprint.set_auto_unlock",
        "Enable or disable automatic local fingerprint unlock. When enabled, a successful match unlocks the idle device so the user can tap the avatar or use wake word to start talking.",
        PropertyList({
            Property("enabled", kPropertyTypeBoolean),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            bool enabled = properties["enabled"].value<bool>();
            SetAutoUnlockEnabled(enabled);
            return true;
        });
}

void Fpc1020aFingerprint::StartAutoUnlock() {
    if (!ready() || auto_unlock_task_handle_ != nullptr) {
        return;
    }

    stop_task_.store(false);
    BaseType_t ok = xTaskCreate(
        AutoUnlockTaskEntry,
        "finger_unlock",
        kAutoUnlockStackSize,
        this,
        kAutoUnlockPriority,
        &auto_unlock_task_handle_);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to start auto-unlock task");
        auto_unlock_task_handle_ = nullptr;
    }
}

void Fpc1020aFingerprint::RunEnrollJob(uint8_t id, uint8_t permission) {
    ESP_LOGI(TAG, "Fingerprint enrollment started id=%u", id);
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification("Keep finger on sensor", 3000);
    }

    std::string error;
    bool ok = Enroll(id, permission, &error);
    last_enroll_id_.store(id);
    last_enroll_success_.store(ok ? 1 : 0);
    last_enroll_ms_.store(NowMs());
    enrollment_in_progress_.store(false);

    uint16_t user_count = 0;
    bool count_ok = GetUserCount(user_count);

    if (ok) {
        ESP_LOGI(TAG, "Fingerprint enrollment finished id=%u user_count_ok=%d user_count=%u",
                 id, count_ok ? 1 : 0, user_count);
        if (display != nullptr) {
            display->ShowNotification("Fingerprint enrolled", 3000);
        }
    } else {
        ESP_LOGW(TAG, "Fingerprint enrollment failed id=%u user_count_ok=%d user_count=%u: %s",
                 id, count_ok ? 1 : 0, user_count, error.c_str());
        if (display != nullptr) {
            display->ShowNotification("Fingerprint enroll failed", 3000);
        }
    }
}

void Fpc1020aFingerprint::AutoUnlockTaskEntry(void* arg) {
    auto* self = static_cast<Fpc1020aFingerprint*>(arg);
    self->AutoUnlockTask();
}

void Fpc1020aFingerprint::AutoUnlockTask() {
    while (!stop_task_.load()) {
        if (enroll_requested_.exchange(false)) {
            RunEnrollJob(
                static_cast<uint8_t>(pending_enroll_id_.load()),
                static_cast<uint8_t>(pending_enroll_permission_.load()));
            continue;
        }

        if (!auto_unlock_enabled_.load() || enrollment_in_progress_.load()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        MatchResult result = Verify(kAutoUnlockMatchTimeoutMs);
        if (result.matched) {
            HandleMatchedFingerprint(result);
            vTaskDelay(pdMS_TO_TICKS(kAutoUnlockCooldownMs));
        } else {
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    auto_unlock_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void Fpc1020aFingerprint::HandleMatchedFingerprint(const MatchResult& result) {
    last_match_user_id_.store(result.user_id);
    last_match_permission_.store(result.permission);
    last_match_score_.store(result.score);
    last_match_ms_.store(NowMs());
    UnlockFor(kConversationUnlockDurationMs);
    ESP_LOGI(TAG, "Fingerprint unlock matched user=%u permission=%u score=%u unlocked_for_ms=%u",
             result.user_id, result.permission, result.score, kConversationUnlockDurationMs);

    auto& app = Application::GetInstance();
    app.Schedule([user_id = result.user_id]() {
        auto& app = Application::GetInstance();
        DeviceState state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetSecurityLock(false);
                display->ShowNotification("Fingerprint unlocked", 1500);
            }
        } else if (state == kDeviceStateActivating) {
            app.SetDeviceState(kDeviceStateIdle);
        } else {
            ESP_LOGD(TAG, "Fingerprint matched user=%u while state=%d; no unlock action",
                     user_id, state);
        }
    });
}

const char* Fpc1020aFingerprint::AckToString(uint8_t ack) {
    switch (ack) {
        case kAckSuccess:
            return "success";
        case kAckFail:
            return "failed";
        case kAckFull:
            return "fingerprint storage is full";
        case kAckNoUser:
            return "no matching enrolled user";
        case kAckUserExist:
            return "user already exists";
        case kAckTimeout:
            return "timeout";
        case kAckGoOut:
            return "finger moved away";
        default:
            return "unknown fingerprint error";
    }
}

const char* Fpc1020aFingerprint::Finger2ConfirmToString(uint8_t confirm) {
    switch (confirm) {
        case kFinger2ConfirmOk:
            return "success";
        case 0x02:
            return "no finger";
        case kFinger2ConfirmUnmatched:
            return "unmatched";
        case kFinger2ConfirmNotFound:
            return "not found";
        case 0x1F:
            return "fingerprint database is full";
        case 0x22:
            return "template is not empty";
        case 0x23:
            return "template is empty";
        case 0x24:
            return "fingerprint database is empty";
        case 0x26:
            return "timeout";
        case 0x27:
            return "fingerprint already exists";
        case 0xF9:
            return "packet timeout";
        case 0xFC:
            return "operation blocked";
        case 0xFD:
            return "parameter error";
        case 0xFE:
            return "not active";
        case kFinger2ConfirmPassiveActivation:
            return "passive activation";
        default:
            return "unknown Finger2 error";
    }
}

const char* Fpc1020aFingerprint::ProtocolToString(ProbeProtocol protocol) {
    switch (protocol) {
        case ProbeProtocol::Fpc1020a:
            return "fpc1020a";
        case ProbeProtocol::Finger2:
            return "finger2";
        default:
            return "unknown";
    }
}
