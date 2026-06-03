#include "unit_driver.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "UnitCatalog"

std::vector<uint8_t> UnitScanAndRegister(i2c_master_bus_handle_t bus, McpServer& mcp) {
    std::vector<uint8_t> found;
    if (bus == nullptr) {
        ESP_LOGW(TAG, "Port A bus not initialized; skipping Unit scan");
        return found;
    }

    auto& catalog = UnitCatalog::GetInstance();
    int known = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {  // valid 7-bit I2C range
        esp_err_t ret = i2c_master_probe(bus, addr, pdMS_TO_TICKS(50));
        if (ret != ESP_OK) {
            continue;
        }
        found.push_back(addr);

        const UnitCatalogEntry* entry = catalog.Find(addr);
        if (entry == nullptr) {
            ESP_LOGW(TAG, "Port A: no known Unit at 0x%02X (ignored)", addr);
            continue;
        }

        // Instantiate the driver: it runs its init sequence and registers its
        // MCP tools inside the factory. We intentionally leak the pointer — the
        // driver must outlive this scan so its tool callbacks stay valid for the
        // whole boot. AddTool is idempotent by name, so a re-scan is harmless.
        UnitDriver* driver = entry->factory(bus, mcp);
        if (driver == nullptr) {
            ESP_LOGW(TAG, "Port A: %s at 0x%02X failed to init (ignored)", entry->name, addr);
            continue;
        }
        ESP_LOGI(TAG, "Port A scan: found 0x%02X -> registered %s", addr, entry->name);
        known++;
    }

    ESP_LOGI(TAG, "Port A scan complete: %d device(s) ACKed, %d known Unit(s) registered",
             (int)found.size(), known);
    return found;
}
