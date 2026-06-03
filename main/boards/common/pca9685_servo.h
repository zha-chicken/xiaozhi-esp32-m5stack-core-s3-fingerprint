#ifndef PCA9685_SERVO_H
#define PCA9685_SERVO_H

// ADR 0010 Phase 1 — PCA9685 16-channel I2C servo driver (Unit catalog entry).
//
// Default address 0x40. Fire-and-forget actuator: registers `self.servo.set_angle`
// (position a channel) and `self.servo.wave` (a visible demo sweep) via AddTool.

#include "i2c_device.h"
#include "unit_driver.h"

class McpServer;

// PCA9685 default servo-driver Unit address (M5Stack 4-Relay / Servo HATs ship 0x40).
#define PCA9685_SERVO_DEFAULT_ADDR 0x40

class Pca9685Servo : public I2cDevice, public UnitDriver {
public:
    Pca9685Servo(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

    // Position one channel (0-15) to an angle (0-180 deg).
    void SetAngle(int channel, int angle);

    // Register the servo MCP tools on the given server (idempotent by name).
    void RegisterTools(McpServer& mcp);

private:
    void SetPwm(int channel, uint16_t on_count, uint16_t off_count);
    static uint16_t AngleToOffCount(int angle);
};

#endif // PCA9685_SERVO_H
