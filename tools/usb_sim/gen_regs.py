#!/usr/bin/env python3
"""Make a PC version of Inc/stm32f103_regs.h for tools/usb_sim.

All constants stay exactly as in the firmware header; only the way registers
are accessed changes (sim::Reg instead of a pointer to a fixed address), and
the inline helpers that use ARM instructions are replaced by host versions.
Usage: gen_regs.py <Inc/stm32f103_regs.h> <output header>
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()

def sub(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)

sub("#define REG32(addr)            (*(volatile uint32_t *)(addr))",
    '#include "sim_api.hpp"\n#define REG32(addr)            (sim::Reg{(uint32_t)(addr)})')
sub("#define FLASH_SIZE_KB          (*(volatile const uint16_t *)0x1FFFF7E0UL)",
    "#define FLASH_SIZE_KB          (sim::flash_size_kb())")
sub("#define UNIQUE_ID_BASE         0x1FFFF7E8UL", "#define UNIQUE_ID_BASE         ((uintptr_t)sim::uid)")
sub("#define USB_PMA_BASE           0x40006000UL", "#define USB_PMA_BASE           ((uintptr_t)sim::pma)")

s, n = re.subn(r"static inline [^\n]*\n\{.*?\n\}\n", "", s, flags=re.S)
assert n == 5, n          # gpio_config, start_image, system_reset, irq_save, irq_restore

host = """
static inline void gpio_config(uint32_t port, uint32_t pin, uint32_t mode) { sim::gpio_config(port, pin, mode); }
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t) {}
[[noreturn]] static inline void start_image(uint32_t sp, uint32_t pc) { throw sim::Jump{sp, pc}; }
[[noreturn]] static inline void system_reset(void) { throw sim::Reset{}; }
"""
s = s.replace("#endif /* STM32F103_REGS_H */", host + "\n#endif /* STM32F103_REGS_H */")
open(dst, "w").write(s)
