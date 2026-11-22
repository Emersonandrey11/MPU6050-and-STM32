// Model of the STM32F103 USB peripheral + flash, and a minimal USB host that
// drives it packet by packet (SETUP / IN / OUT transactions, as on the wire).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_api.hpp"

namespace sim {

constexpr uint32_t FLASH_BASE  = 0x08000000;
constexpr uint32_t FLASH_BYTES = 64 * 1024;
extern std::vector<uint8_t> flash;

struct Stats { unsigned erases = 0, program_errors = 0, setups = 0; };
extern Stats stats;

void     reset_model();
void     flash_write16(uint32_t addr, uint16_t v);
uint8_t  flash_read8(uint32_t addr);
uint32_t flash_read32(uint32_t addr);

// The firmware side: what runs when the USB interrupt fires, and the main
// loop (for work the firmware defers, like flash programming).
struct Device {
    virtual void interrupt() = 0;
    virtual void main_loop() {}
    virtual ~Device() = default;
};

enum Kind { ACK, NAK, STALL, NOREPLY };
struct Reply { Kind kind; std::vector<uint8_t> data; };

void  bus_reset(Device &dev);
Reply setup(Device &dev, uint8_t addr, const uint8_t pkt[8]);
Reply in(Device &dev, uint8_t addr, unsigned ep);
Reply out(Device &dev, uint8_t addr, unsigned ep, const std::vector<uint8_t> &data);

// Flash accessors used by the bootloader through its FLASH_MEMx macros.
struct FlashHalf {
    uint32_t addr;
    void operator=(uint16_t v) const { flash_write16(addr, v); }
};

}  // namespace sim
