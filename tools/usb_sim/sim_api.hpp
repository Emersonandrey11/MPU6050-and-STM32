// Host-side stand-ins for the STM32F103 used by tools/usb_sim.
// Included by the generated stm32f103_regs.h instead of real register access.
#pragma once
#include <cstdint>

namespace sim {

uint32_t reg_read(uint32_t addr);
void     reg_write(uint32_t addr, uint32_t value);

// A register access. Reads and writes go through the hardware model, so
// writes can have the same side effects as on the chip (toggle bits,
// write-0-to-clear flags, flash erase...).
struct Reg {
    uint32_t addr;
    operator uint32_t() const { return reg_read(addr); }
    const Reg &operator=(uint32_t v) const  { reg_write(addr, v); return *this; }
    const Reg &operator|=(uint32_t v) const { reg_write(addr, reg_read(addr) | v); return *this; }
    const Reg &operator&=(uint32_t v) const { reg_write(addr, reg_read(addr) & v); return *this; }
    const Reg &operator^=(uint32_t v) const { reg_write(addr, reg_read(addr) ^ v); return *this; }
};

extern uint8_t pma[1024];              // 512-byte PMA, 16-bit words at a 32-bit stride
extern uint8_t uid[12];
uint16_t flash_size_kb();
void     gpio_config(uint32_t port, uint32_t pin, uint32_t mode);

struct Reset {};                       // thrown by system_reset()
struct Jump { uint32_t sp, pc; };      // thrown by start_image()

}  // namespace sim
