#include "pca9685_servo.h"

#include "mcp_server.h"

#include <algorithm>
#include <cmath>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Pca9685Servo"

// PCA9685 register map (subset).
#define PCA9685_REG_MODE1      0x00
#define PCA9685_REG_MODE2      0x01
#define PCA9685_REG_LED0_ON_L  0x06   // base of per-channel block; channel n at base + 4*n
#define PCA9685_REG_PRESCALE   0xFE

// MODE1 bits.
#define PCA9685_MODE1_SLEEP    0x10   // low-power; oscillator off (required before PRESCALE)
#define PCA9685_MODE1_ALLCALL  0x01   // responds to All-Call address (0x70) when set
// MODE2 bits.
#define PCA9685_MODE2_OUTDRV   0x04   // totem-pole outputs

// Servo timing: 50 Hz frame, 500-2500 us pulse spans 0-180 deg.
static constexpr int kServoMinUs = 500;
static constexpr int kServoMaxUs = 2500;
static constexpr int kFrameUs    = 20000;  // 1 / 50 Hz
static constexpr int kPwmCounts  = 4096;   // 12-bit resolution

Pca9685Servo::Pca9685Servo(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : I2cDevice(i2c_bus, addr) {
    // Sleep first so PRESCALE is writable.
    WriteReg(PCA9685_REG_MODE1, PCA9685_MODE1_SLEEP);
    // PRESCALE for ~50 Hz: round(25MHz / (4096 * 50)) - 1 = 121 = 0x79.
    WriteReg(PCA9685_REG_PRESCALE, 0x79);
    // Wake (clears SLEEP). Writing 0x00 also clears ALLCALL (bit0) so the chip
    // stops answering All-Call 0x70 — otherwise it ghosts in the scan and can
    // collide with a PaHUB. This is the deliberate "clear ALLCALL" step.
    WriteReg(PCA9685_REG_MODE1, 0x00);
    vTaskDelay(pdMS_TO_TICKS(1));  // oscillator settle (>= 500 us)
    // Totem-pole outputs (push-pull) — cleaner edges into a servo signal pin.
    WriteReg(PCA9685_REG_MODE2, PCA9685_MODE2_OUTDRV);
    ESP_LOGI(TAG, "PCA9685 @0x%02X init: 50Hz, ALLCALL cleared, OUTDRV", addr);
}

uint16_t Pca9685Servo::AngleToOffCount(int angle) {
    angle = std::clamp(angle, 0, 180);
    // angle 0-180 -> pulse 500-2500 us
    int pulse_us = kServoMinUs + (kServoMaxUs - kServoMinUs) * angle / 180;
    // pulse_us -> 12-bit off-count, rounded.
    long count = std::lround((double)pulse_us * kPwmCounts / kFrameUs);
    if (count < 0) count = 0;
    if (count > kPwmCounts - 1) count = kPwmCounts - 1;  // never exceed 4095
    return (uint16_t)count;
}

void Pca9685Servo::SetPwm(int channel, uint16_t on_count, uint16_t off_count) {
    uint8_t base = PCA9685_REG_LED0_ON_L + 4 * (uint8_t)channel;
    WriteReg(base + 0, on_count & 0xFF);          // LEDn_ON_L
    WriteReg(base + 1, (on_count >> 8) & 0x0F);   // LEDn_ON_H
    WriteReg(base + 2, off_count & 0xFF);         // LEDn_OFF_L
    WriteReg(base + 3, (off_count >> 8) & 0x0F);  // LEDn_OFF_H
}

void Pca9685Servo::SetAngle(int channel, int angle) {
    channel = std::clamp(channel, 0, 15);
    uint16_t off_count = AngleToOffCount(angle);
    SetPwm(channel, 0, off_count);
    ESP_LOGI(TAG, "servo ch%d -> %d deg (off=%u)", channel, angle, off_count);
}

void Pca9685Servo::RegisterTools(McpServer& mcp) {
    mcp.AddTool(
        "self.servo.set_angle",
        "Set a servo on the 16-channel servo driver to an angle. "
        "channel: which servo port (0-15). angle: target angle in degrees (0-180).",
        PropertyList({
            Property("channel", kPropertyTypeInteger, 0, 15),
            Property("angle", kPropertyTypeInteger, 0, 180),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int channel = properties["channel"].value<int>();
            int angle = properties["angle"].value<int>();
            SetAngle(channel, angle);
            return true;
        });

    mcp.AddTool(
        "self.servo.wave",
        "Make a servo wave/sweep back and forth a few times as a greeting gesture. "
        "channel: which servo port (0-15).",
        PropertyList({
            Property("channel", kPropertyTypeInteger, 0, 15),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int channel = properties["channel"].value<int>();
            for (int i = 0; i < 3; i++) {
                SetAngle(channel, 60);
                vTaskDelay(pdMS_TO_TICKS(350));
                SetAngle(channel, 120);
                vTaskDelay(pdMS_TO_TICKS(350));
            }
            SetAngle(channel, 90);  // rest centered
            return true;
        });
}

// Self-register this driver in the Unit catalog. Adding a Unit driver therefore
// touches only this .cc — no central table edit. The factory constructs the
// driver (running its init), registers its tools, and returns it to be kept alive.
static UnitRegistration s_pca9685_registration(
    PCA9685_SERVO_DEFAULT_ADDR,
    "pca9685-servo-16ch",
    [](i2c_master_bus_handle_t bus, McpServer& mcp) -> UnitDriver* {
        auto* driver = new Pca9685Servo(bus, PCA9685_SERVO_DEFAULT_ADDR);
        driver->RegisterTools(mcp);
        return driver;
    });
