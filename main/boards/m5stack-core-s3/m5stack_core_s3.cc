#include "wifi_board.h"
#include "cores3_audio_codec.h"
#include "display/lcd_display.h"
#include "display/badge_watch_display.h"
#include "application.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "axp2101.h"
#include "mcp_server.h"
#include "unit_driver.h"
#include "fpc1020a_fingerprint.h"
#include "device_state_event.h"
#include "memo_store.h"

#include <esp_log.h>
#include <esp_system.h>
#include <driver/i2c_master.h>
#include <driver/uart.h>
#include <wifi_station.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <esp_timer.h>
#include <atomic>
#include <vector>
#include "esp32_camera.h"

// Baked badge assets shared with the C6 board (definitions in the relocated
// display/avatar_assets.c, compiled for C6 || CoreS3 — see main/CMakeLists.txt).
// avatar_a is 172×172 RGB565 = the CoreS3 badge slot; bg_tile_4x4 is the grid
// texture. Only one board is built at a time, so a single definition links.
extern "C" const lv_img_dsc_t avatar_a;
extern "C" const lv_img_dsc_t bg_tile_4x4;

#define TAG "M5StackCoreS3Board"

class Pmic : public Axp2101 {
public:
    // Power Init
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        uint8_t data = ReadReg(0x90);
        data |= 0b10110100;
        WriteReg(0x90, data);
        WriteReg(0x92, 0b00001101);  // ALDO1 / PA PVDD / 1V8
        WriteReg(0x93, 0b00011100);  // ALDO2 / codec / 3V3
        WriteReg(0x94, 0b00011100);  // ALDO3 / codec + mic / 3V3
        WriteReg(0x99, (0b11110 - 5));
        WriteReg(0x97, (0b11110 - 2));
        WriteReg(0x69, 0b00110101);
        WriteReg(0x30, 0b111111);
        WriteReg(0x90, 0xBF);
        WriteReg(0x94, 33 - 5);
        WriteReg(0x95, 33 - 5);
    }

    void SetBrightness(uint8_t brightness) {
        brightness = ((brightness + 641) >> 5);
        WriteReg(0x99, brightness);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(Pmic *pmic) : pmic_(pmic) {}

    void SetBrightnessImpl(uint8_t brightness) override {
        pmic_->SetBrightness(target_brightness_);
        brightness_ = target_brightness_;
    }

private:
    Pmic *pmic_;
};

class Aw9523 : public I2cDevice {
public:
    // Exanpd IO Init
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x02, 0b00000111);  // P0
        WriteReg(0x03, 0b10001111);  // P1
        WriteReg(0x04, 0b00011000);  // CONFIG_P0
        WriteReg(0x05, 0b00001100);  // CONFIG_P1
        WriteReg(0x11, 0b00010000);  // GCR P0 port is Push-Pull mode.
        WriteReg(0x12, 0b11111111);  // LEDMODE_P0
        WriteReg(0x13, 0b11111111);  // LEDMODE_P1
    }

    void ResetAw88298() {
        ESP_LOGI(TAG, "Reset AW88298");
        WriteReg(0x02, 0b00000011);
        vTaskDelay(pdMS_TO_TICKS(10));
        WriteReg(0x02, 0b00000111);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void ResetIli9342() {
        ESP_LOGI(TAG, "Reset IlI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
};

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    
    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Ft6336() {
        delete[] read_buffer_;
    }

    bool UpdateTouchPoint() {
        uint8_t reg = 0x02;
        esp_err_t err = i2c_master_transmit_receive(i2c_device_, &reg, 1, read_buffer_, 6, 100);
        if (err != ESP_OK) {
            tp_.num = 0;
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_i2c_error_log_ms_ > 5000) {
                ESP_LOGW(TAG, "FT6336 touch read skipped: %s", esp_err_to_name(err));
                last_i2c_error_log_ms_ = now_ms;
            }
            return false;
        }
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
        return true;
    }

    inline const TouchPoint_t& GetTouchPoint() {
        return tp_;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
    int64_t last_i2c_error_log_ms_ = 0;
};

class M5StackCoreS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_bus_handle_t port_a_bus_ = nullptr;  // external Grove Port A (ADR 0010)
    std::vector<uint8_t> port_a_boot_addrs_;        // addresses seen at boot scan
    Pmic* pmic_;
    Aw9523* aw9523_;
    Ft6336* ft6336_;
    LcdDisplay* display_;
    Esp32Camera* camera_;
    Fpc1020aFingerprint* fingerprint_ = nullptr;
    bool port_a_reserved_by_fingerprint_ = false;
    std::atomic<int64_t> last_locked_notice_ms_{0};
    esp_timer_handle_t touchpad_timer_;
    esp_timer_handle_t security_lock_ui_timer_ = nullptr;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    // ADR 0010: bring up the external Grove Port A as a SECOND, independent I2C
    // bus on controller 0 (ESP32-S3 has 2). Mirrors InitializeI2c() but on the
    // Port A pins. Never touches the internal bus on controller 1.
    void InitializePortAI2c() {
        if (port_a_bus_ != nullptr) {
            return;  // already up
        }
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = PORT_A_I2C_SDA_PIN,
            .scl_io_num = PORT_A_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &port_a_bus_));
        ESP_LOGI(TAG, "Port A I2C bus up (port 0, SDA=%d SCL=%d)",
                 PORT_A_I2C_SDA_PIN, PORT_A_I2C_SCL_PIN);
    }

    // ADR 0010 Phase 1: scan Port A and register any known Unit's MCP tools.
    // Called at the end of the constructor (after the internal init + display).
    void InitializeTools() {
        RegisterMemoTool();
        InitializeFingerprint();
        if (port_a_reserved_by_fingerprint_) {
            ESP_LOGI(TAG, "Port A I2C scan skipped; pins are reserved by fingerprint UART");
            return;
        }

        InitializePortAI2c();
        auto& mcp_server = McpServer::GetInstance();
        port_a_boot_addrs_ = UnitScanAndRegister(port_a_bus_, mcp_server);
    }

    void RegisterMemoTool() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.memo.manage",
            "Manage local memos. action=list/view/create/update/delete/clear/delete_all. clear/delete_all deletes every memo; update/delete require id.",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("list")),
                Property("id", kPropertyTypeInteger, 0, 0, 1000000),
                Property("title", kPropertyTypeString, std::string("")),
                Property("content", kPropertyTypeString, std::string(""))
            }),
            [](const PropertyList& properties) -> ReturnValue {
                return CoreS3MemoStore::HandleMcpAction(
                    properties["action"].value<std::string>(),
                    properties["id"].value<int>(),
                    properties["title"].value<std::string>(),
                    properties["content"].value<std::string>());
            });
    }

    void InitializeFingerprint() {
        struct FingerprintUartCandidate {
            const char* label;
            gpio_num_t rx_pin;
            gpio_num_t tx_pin;
            int baud_rate;
            Fpc1020aFingerprint::ProbeProtocol protocol;
            bool uses_port_a;
        };

        const FingerprintUartCandidate candidates[] = {
            {"Finger2 Port C UART fallback (RX=G18 TX=G17)", FINGERPRINT_UART_FALLBACK_RX_PIN, FINGERPRINT_UART_FALLBACK_TX_PIN,
             FINGERPRINT2_UART_BAUD, Fpc1020aFingerprint::ProbeProtocol::Finger2, false},
            {"Finger2 Port C UART fallback swapped (RX=G17 TX=G18)", FINGERPRINT_UART_FALLBACK_TX_PIN, FINGERPRINT_UART_FALLBACK_RX_PIN,
             FINGERPRINT2_UART_BAUD, Fpc1020aFingerprint::ProbeProtocol::Finger2, false},
            {"Finger2 Port A UART (RX=G1 TX=G2)", FINGERPRINT_UART_RX_PIN, FINGERPRINT_UART_TX_PIN,
             FINGERPRINT2_UART_BAUD, Fpc1020aFingerprint::ProbeProtocol::Finger2, true},
            {"Finger2 Port A UART swapped (RX=G2 TX=G1)", FINGERPRINT_UART_TX_PIN, FINGERPRINT_UART_RX_PIN,
             FINGERPRINT2_UART_BAUD, Fpc1020aFingerprint::ProbeProtocol::Finger2, true},
            {"FPC1020A Port A UART (RX=G1 TX=G2)", FINGERPRINT_UART_RX_PIN, FINGERPRINT_UART_TX_PIN,
             FINGERPRINT_FPC1020A_UART_BAUD, Fpc1020aFingerprint::ProbeProtocol::Fpc1020a, true},
            {"FPC1020A Port A UART swapped (RX=G2 TX=G1)", FINGERPRINT_UART_TX_PIN, FINGERPRINT_UART_RX_PIN,
             FINGERPRINT_FPC1020A_UART_BAUD, Fpc1020aFingerprint::ProbeProtocol::Fpc1020a, true},
            {"FPC1020A Port C UART fallback (RX=G18 TX=G17)", FINGERPRINT_UART_FALLBACK_RX_PIN, FINGERPRINT_UART_FALLBACK_TX_PIN,
             FINGERPRINT_FPC1020A_UART_BAUD, Fpc1020aFingerprint::ProbeProtocol::Fpc1020a, false},
            {"FPC1020A Port C UART fallback swapped (RX=G17 TX=G18)", FINGERPRINT_UART_FALLBACK_TX_PIN, FINGERPRINT_UART_FALLBACK_RX_PIN,
             FINGERPRINT_FPC1020A_UART_BAUD, Fpc1020aFingerprint::ProbeProtocol::Fpc1020a, false},
        };

        for (const auto& candidate : candidates) {
            ESP_LOGI(TAG, "Probing fingerprint on %s", candidate.label);
            auto* driver = new Fpc1020aFingerprint(
                UART_NUM_1,
                candidate.rx_pin,
                candidate.tx_pin,
                candidate.baud_rate,
                candidate.protocol);
            if (!driver->Begin()) {
                delete driver;
                continue;
            }

            fingerprint_ = driver;
            port_a_reserved_by_fingerprint_ = candidate.uses_port_a;
            auto& mcp_server = McpServer::GetInstance();
            fingerprint_->RegisterTools(mcp_server);
            fingerprint_->StartAutoUnlock();
            ESP_LOGI(TAG, "Fingerprint unlock active on %s", candidate.label);
            return;
        }

        ESP_LOGI(TAG, "Fingerprint unlock not active");
    }

    bool IsConversationState(DeviceState state) const {
        return state == kDeviceStateConnecting ||
               state == kDeviceStateListening ||
               state == kDeviceStateSpeaking;
    }

    void UpdateFingerprintLockUi() {
        auto display = GetDisplay();
        if (display == nullptr) {
            return;
        }

        DeviceState state = Application::GetInstance().GetDeviceState();
        if (IsConversationState(state)) {
            display->SetSecurityLock(false);
            return;
        }
        if (state == kDeviceStateIdle || state == kDeviceStateUnknown) {
            bool unlocked = fingerprint_ != nullptr && fingerprint_->IsUnlocked();
            display->SetSecurityLock(!unlocked);
        }
    }

    void InitializeSecurityLockUi() {
        DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
            [this](DeviceState /*previous*/, DeviceState /*current*/) {
                UpdateFingerprintLockUi();
            });

        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                static_cast<M5StackCoreS3Board*>(arg)->UpdateFingerprintLockUi();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "finger_lock_ui",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &security_lock_ui_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(security_lock_ui_timer_, 1000 * 1000));
    }

    // ADR 0010: no per-tool removal exists in mcp_server. To reflect a Unit being
    // plugged/unplugged we re-probe Port A; if the detected address set changed
    // vs boot, restart so a fresh boot re-scans and the platform re-pulls
    // tools/list on reconnect. Currently callable on demand.
    // TODO(ADR-0010): wire a rescan trigger (e.g. touch long-press).
    void RescanPortA() {
        if (port_a_bus_ == nullptr) {
            return;
        }
        std::vector<uint8_t> now;
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            // 200ms to match UnitScanAndRegister: weak Port A pull-ups make a
            // present device's ACK slow; 50ms can miss it (ADR 0010 bring-up).
            if (i2c_master_probe(port_a_bus_, addr, pdMS_TO_TICKS(200)) == ESP_OK) {
                now.push_back(addr);
            }
        }
        if (now != port_a_boot_addrs_) {
            ESP_LOGW(TAG, "Port A topology changed (%d -> %d devices); restarting to re-scan",
                     (int)port_a_boot_addrs_.size(), (int)now.size());
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeAw9523() {
        ESP_LOGI(TAG, "Init AW9523");
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
        aw9523_->ResetAw88298();
    }

    void ShowMemoList() {
        auto* badge_display = dynamic_cast<BadgeWatchDisplay*>(display_);
        if (badge_display == nullptr) {
            auto display = GetDisplay();
            if (display != nullptr) {
                display->ShowNotification(CoreS3MemoStore::DisplayText().c_str(), 5000);
            }
            return;
        }

        auto text = CoreS3MemoStore::DisplayText();
        badge_display->ShowMemoList(text.c_str());
    }

    bool HideMemoListIfVisible() {
        auto* badge_display = dynamic_cast<BadgeWatchDisplay*>(display_);
        return badge_display != nullptr && badge_display->HideMemoList();
    }

    void PollTouchpad() {
        static bool was_touched = false;
        static bool long_press_handled = false;
        static int64_t touch_start_time = 0;
        const int64_t TOUCH_SHORT_THRESHOLD_MS = 500;
        const int64_t MEMO_VIEW_TOUCH_MS = 3000;
        
        if (!ft6336_->UpdateTouchPoint()) {
            return;
        }
        auto& touch_point = ft6336_->GetTouchPoint();
        
        // 检测触摸开始
        if (touch_point.num > 0 && !was_touched) {
            was_touched = true;
            long_press_handled = false;
            touch_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
        } else if (touch_point.num > 0 && was_touched && !long_press_handled) {
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;
            if (touch_duration >= MEMO_VIEW_TOUCH_MS) {
                long_press_handled = true;
                ShowMemoList();
            }
        }
        // 检测触摸释放
        else if (touch_point.num == 0 && was_touched) {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;
            
            // 只有短触才触发
            if (!long_press_handled && touch_duration < TOUCH_SHORT_THRESHOLD_MS) {
                if (HideMemoListIfVisible()) {
                    return;
                }
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting && 
                    !WifiStation::GetInstance().IsConnected()) {
                    ESP_LOGI(TAG, "Touch ignored while WiFi is still starting");
                    return;
                }
                app.ToggleChatState();
            }
        }
    }

    void InitializeFt6336TouchPad() {
        ESP_LOGI(TAG, "Init FT6336");
        ft6336_ = new Ft6336(i2c_bus_, 0x38);
        
        // 创建定时器，20ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                M5StackCoreS3Board* board = (M5StackCoreS3Board*)arg;
                board->PollTouchpad();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };
        
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touchpad_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(touchpad_timer_, 20 * 1000));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_37;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_36;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeIli9342Display() {
        ESP_LOGI(TAG, "Init IlI9342");

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_3;
        io_config.dc_gpio_num = GPIO_NUM_35;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        aw9523_->ResetIli9342();

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        // Hao Lab badge-watch GUI (shared BadgeWatchDisplay). CoreS3 reuses
        // the C6 badge composition byte-for-byte: same circle/dial/halo
        // geometry, same ticker, same baked assets (avatar_a 172×172 + the
        // 4×4 grid tile, now compiled for both boards — see CMakeLists).
        // Corners/ticker/centering derive from LV_HOR_RES/VER_RES so they
        // auto-adapt to the 320×240 panel; no per-corner work.
        BadgeWatchConfig badge_cfg = {
            .badge_radius      = 86,
            .ring_radius       = 92,
            .halo_radius       = 98,
            .center_y_offset   = -8,
            .tick_len_cardinal = 6,
            .tick_len_major    = 4,
            .tick_len_minor    = 2,
            .hour_mark_w       = 4,
            .hour_mark_h       = 22,
            .show_ticker       = true,
            .show_version_label = false,
            .default_avatar    = &avatar_a,
            .bg_tile           = &bg_tile_4x4,
        };
        display_ = new BadgeWatchDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    badge_cfg);
    }

     void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new Esp32Camera(video_config);
        camera_->SetHMirror(false);
    }

public:
    M5StackCoreS3Board() {
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeAxp2101();
        InitializeAw9523();
        I2cDetect();
        InitializeSpi();
        InitializeIli9342Display();
        InitializeCamera();
        InitializeFt6336TouchPad();
        GetBacklight()->RestoreBrightness();
        InitializeTools();  // ADR 0010: scan Port A, register Unit MCP tools
        InitializeSecurityLockUi();
    }

    virtual bool CanStartConversation(const char* source) override {
        if (fingerprint_ != nullptr && fingerprint_->IsUnlocked()) {
            return true;
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t last_notice_ms = last_locked_notice_ms_.load();
        if (now_ms - last_notice_ms > 2000 &&
            last_locked_notice_ms_.compare_exchange_strong(last_notice_ms, now_ms)) {
            bool sensor_ready = fingerprint_ != nullptr && fingerprint_->ready();
            ESP_LOGW(TAG, "Conversation blocked by fingerprint lock source=%s sensor_ready=%d",
                     source != nullptr ? source : "unknown", sensor_ready ? 1 : 0);
            auto display = GetDisplay();
            if (display != nullptr) {
                display->SetSecurityLock(true);
            }
        }
        return false;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CoreS3AudioCodec audio_codec(i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }

    virtual Backlight *GetBacklight() override {
        static CustomBacklight backlight(pmic_);
        return &backlight;
    }
};

DECLARE_BOARD(M5StackCoreS3Board);
