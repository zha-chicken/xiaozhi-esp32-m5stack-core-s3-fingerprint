#ifndef UNIT_DRIVER_H
#define UNIT_DRIVER_H

// ADR 0010 Phase 1 — modular I2C Unit auto-discovery.
//
// A vendor-agnostic catalog that maps a probed 7-bit I2C address on an external
// (Grove) bus to a Unit driver. On boot the board scans the bus and, for each
// detected address that the catalog knows, instantiates the driver and lets it
// register its MCP tools. Plug a Unit -> its capability appears in tools/list;
// unplug -> rescan -> esp_restart() re-scans on the next boot (no per-tool removal).
//
// This is intentionally NOT a plugin system: it is a small std::vector of
// {addr, name, factory} entries. A driver self-registers one entry via a static
// UnitRegistration object in its own .cc, so adding a Unit driver touches only
// that driver's files.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <driver/i2c_master.h>

class McpServer;
class UnitDriver;

// Factory: given the external bus handle, construct the concrete driver, run its
// init sequence, register its MCP tools, and return ownership. Returning nullptr
// means "this address was a false hit / init failed" and the scanner ignores it.
using UnitFactory = std::function<UnitDriver*(i2c_master_bus_handle_t bus, McpServer& mcp)>;

struct UnitCatalogEntry {
    uint8_t addr;            // 7-bit I2C address this Unit answers
    const char* name;        // human/log name, e.g. "pca9685-servo-16ch"
    UnitFactory factory;
};

// Base class for every Unit driver. Concrete drivers also subclass I2cDevice for
// register access; UnitDriver only exists so the catalog can own a heterogeneous
// list of live drivers (they must outlive the scan so their tool callbacks stay valid).
class UnitDriver {
public:
    virtual ~UnitDriver() = default;
};

// The catalog is a process-wide singleton populated by static registrations.
class UnitCatalog {
public:
    static UnitCatalog& GetInstance() {
        static UnitCatalog instance;
        return instance;
    }

    void Register(const UnitCatalogEntry& entry) { entries_.push_back(entry); }

    // Look up a probed address; returns nullptr if no known Unit lives there.
    const UnitCatalogEntry* Find(uint8_t addr) const {
        for (const auto& e : entries_) {
            if (e.addr == addr) {
                return &e;
            }
        }
        return nullptr;
    }

    const std::vector<UnitCatalogEntry>& entries() const { return entries_; }

private:
    UnitCatalog() = default;
    std::vector<UnitCatalogEntry> entries_;
};

// Static-init helper so a driver self-registers from its own translation unit.
struct UnitRegistration {
    UnitRegistration(uint8_t addr, const char* name, UnitFactory factory) {
        UnitCatalog::GetInstance().Register({addr, name, std::move(factory)});
    }
};

// Probe the whole 7-bit address space on `bus`; for each ACKing address look it
// up in the catalog, instantiate + register its tools, and keep the driver alive.
// Returns the set of addresses that ACKed (whether known or not) so a caller can
// detect plug/unplug changes for a rescan-restart. Drivers are leaked on purpose:
// they must live for the whole boot so their tool callbacks remain valid.
std::vector<uint8_t> UnitScanAndRegister(i2c_master_bus_handle_t bus, McpServer& mcp);

#endif // UNIT_DRIVER_H
