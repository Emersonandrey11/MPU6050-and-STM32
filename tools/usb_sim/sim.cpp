// Hardware model of the parts of the STM32F103 the USB code touches:
// USB registers (with their odd write rules), packet memory, flash + FPEC.
// Everything else is plain memory.
#include "sim.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>

namespace sim {

alignas(4) uint8_t pma[1024];
uint8_t uid[12] = { 0x31, 0xFF, 0xD6, 0x05, 0x48, 0x50, 0x34, 0x39, 0x31, 0x52, 0x14, 0x43 };
std::vector<uint8_t> flash(FLASH_BYTES, 0xFF);

static std::map<uint32_t, uint32_t> s_mem;
static uint16_t s_epr[8];
static uint16_t s_istr;              // flag bits 8..14
static bool     s_unlocked;
static int      s_key_step;

constexpr uint32_t USB = 0x40005C00, ISTR = USB + 0x44, DADDR = USB + 0x4C, BTABLE = USB + 0x50;
constexpr uint32_t FL = 0x40022000, KEYR = FL + 4, SR = FL + 0xC, CR = FL + 0x10, AR = FL + 0x14;

void reset_model()
{
    s_mem.clear();
    for (auto &e : s_epr) e = 0;
    s_istr = 0;
    s_unlocked = false;
    s_key_step = 0;
    s_mem[CR] = 0x80;                // LOCK after reset
    for (auto &b : pma) b = 0xA5;
}

uint16_t flash_size_kb() { return 64; }
void gpio_config(uint32_t, uint32_t, uint32_t) {}

uint32_t reg_read(uint32_t a)
{
    if (a >= USB && a < USB + 0x20) return s_epr[(a - USB) / 4];
    if (a == ISTR) {
        uint32_t r = s_istr;
        for (unsigned ep = 0; ep < 8; ep++) {
            if (s_epr[ep] & 0x8080) {          // CTR_RX | CTR_TX
                r |= 0x8000 | ep | ((s_epr[ep] & 0x8000) ? 0x10 : 0);
                break;
            }
        }
        return r;
    }
    return s_mem[a];
}

static void flash_erase(uint32_t addr)
{
    if (addr < FLASH_BASE || addr >= FLASH_BASE + FLASH_BYTES) { s_mem[SR] |= 0x10; return; }
    uint32_t page = (addr - FLASH_BASE) & ~1023u;
    for (uint32_t i = 0; i < 1024; i++) flash[page + i] = 0xFF;
    s_mem[SR] |= 0x20;                 // EOP
    stats.erases++;
}

void reg_write(uint32_t a, uint32_t w)
{
    if (a >= USB && a < USB + 0x20) {
        // EPnR: EA/EP_TYPE/EP_KIND written as is; CTR_RX/CTR_TX cleared by 0,
        // kept by 1; DTOG/STAT bits toggled by 1; SETUP read-only.
        uint16_t v = s_epr[(a - USB) / 4];
        uint16_t nv = (uint16_t)((v & ~0x070F) | (w & 0x070F));
        if (!(w & 0x8000)) nv &= (uint16_t)~0x8000;
        if (!(w & 0x0080)) nv &= (uint16_t)~0x0080;
        nv ^= (uint16_t)(w & 0x7070);
        s_epr[(a - USB) / 4] = nv;
        return;
    }
    if (a == ISTR) { s_istr &= (uint16_t)(w & 0x7F00); return; }   // rc_w0
    if (a == KEYR) {
        if (s_key_step == 0 && w == 0x45670123u) s_key_step = 1;
        else if (s_key_step == 1 && w == 0xCDEF89ABu) { s_unlocked = true; s_key_step = 0; s_mem[CR] &= ~0x80u; }
        else { s_key_step = 0; }
        return;
    }
    if (a == CR) {
        if (w & 0x80) { s_unlocked = false; s_mem[CR] = 0x80; return; }
        if (!s_unlocked) return;
        if ((w & 0x40) && (w & 0x02)) flash_erase(s_mem[AR]);       // STRT + PER
        s_mem[CR] = w & ~0x40u;
        return;
    }
    if (a == SR) { s_mem[SR] &= ~(w & 0x34u); return; }             // write 1 to clear
    s_mem[a] = w;
}

void flash_write16(uint32_t addr, uint16_t v)
{
    bool ok = s_unlocked && (s_mem[CR] & 1) && (addr % 2 == 0) &&
              addr >= FLASH_BASE && addr + 1 < FLASH_BASE + FLASH_BYTES;
    uint32_t i = addr - FLASH_BASE;
    if (!ok || flash[i] != 0xFF || flash[i + 1] != 0xFF) {          // must be erased first
        s_mem[SR] |= 0x04;                                           // PGERR
        stats.program_errors++;
        return;
    }
    flash[i] = (uint8_t)v;
    flash[i + 1] = (uint8_t)(v >> 8);
    s_mem[SR] |= 0x20;
}

uint8_t flash_read8(uint32_t addr)
{
    if (addr < FLASH_BASE || addr >= FLASH_BASE + FLASH_BYTES) { std::fprintf(stderr, "flash read out of range 0x%08x\n", addr); std::abort(); }
    return flash[addr - FLASH_BASE];
}

uint32_t flash_read32(uint32_t addr)
{
    return flash_read8(addr) | flash_read8(addr + 1) << 8 | flash_read8(addr + 2) << 16 | (uint32_t)flash_read8(addr + 3) << 24;
}

Stats stats;

// ---- host side ------------------------------------------------------------

static uint16_t pma16(uint32_t off) { uint32_t w; std::memcpy(&w, &pma[2 * off], 4); return (uint16_t)w; }
static void pma16_set(uint32_t off, uint16_t v) { uint32_t w = v; std::memcpy(&pma[2 * off], &w, 4); }
static uint32_t bt(unsigned ep, unsigned field) { return s_mem[BTABLE] + 8 * ep + field; }

static bool addressed(uint8_t addr)
{
    uint32_t d = s_mem[DADDR];
    return (d & 0x80) && ((d & 0x7F) == addr);
}

static unsigned rx_capacity(uint16_t count_rx)
{
    unsigned blocks = (count_rx >> 10) & 0x1F;
    return (count_rx & 0x8000) ? 32 * (blocks + 1) : 2 * blocks;
}

void bus_reset(Device &dev)
{
    s_mem[DADDR] = 0;
    s_istr |= 0x0400;
    dev.interrupt();
}

Reply setup(Device &dev, uint8_t addr, const uint8_t pkt[8])
{
    if (!addressed(addr)) return { NOREPLY, {} };
    uint16_t rxaddr = pma16(bt(0, 4));
    for (int i = 0; i < 8; i += 2) pma16_set(rxaddr + i, (uint16_t)(pkt[i] | pkt[i + 1] << 8));
    pma16_set(bt(0, 6), (uint16_t)((pma16(bt(0, 6)) & 0xFC00) | 8));
    uint16_t &e = s_epr[0];
    e = (uint16_t)((e & ~0x3000) | 0x2000);       // STAT_RX -> NAK
    e |= 0x8000 | 0x0800 | 0x4000 | 0x0040;       // CTR_RX, SETUP, next DATA1 both ways
    stats.setups++;
    dev.interrupt();
    return { ACK, {} };
}

Reply in(Device &dev, uint8_t addr, unsigned ep)
{
    if (!addressed(addr)) return { NOREPLY, {} };
    uint16_t &e = s_epr[ep];
    unsigned stat = (e >> 4) & 3;
    if (stat == 0) return { NOREPLY, {} };
    if (stat == 1) return { STALL, {} };
    if (stat == 2) return { NAK, {} };
    uint16_t a = pma16(bt(ep, 0));
    unsigned n = pma16(bt(ep, 2)) & 0x3FF;
    std::vector<uint8_t> data(n);
    for (unsigned i = 0; i < n; i++) data[i] = (uint8_t)(pma16(a + (i & ~1u)) >> (8 * (i & 1)));
    e = (uint16_t)((e & ~0x0030) | 0x0020);       // STAT_TX -> NAK
    e ^= 0x0040;                                  // DTOG_TX
    e |= 0x0080;                                  // CTR_TX
    dev.interrupt();
    return { ACK, data };
}

Reply out(Device &dev, uint8_t addr, unsigned ep, const std::vector<uint8_t> &data)
{
    if (!addressed(addr)) return { NOREPLY, {} };
    uint16_t &e = s_epr[ep];
    unsigned stat = (e >> 12) & 3;
    if (stat == 0) return { NOREPLY, {} };
    if (stat == 1) return { STALL, {} };
    if (stat == 2) return { NAK, {} };
    uint16_t count = pma16(bt(ep, 6));
    if (data.size() > rx_capacity(count)) { std::fprintf(stderr, "EP%u: %zu-byte packet overflows its RX buffer\n", ep, data.size()); std::abort(); }
    uint16_t a = pma16(bt(ep, 4));
    for (size_t i = 0; i < data.size(); i += 2) {
        uint16_t v = data[i] | (i + 1 < data.size() ? data[i + 1] << 8 : 0);
        pma16_set(a + (uint32_t)i, v);
    }
    pma16_set(bt(ep, 6), (uint16_t)((count & 0xFC00) | data.size()));
    e = (uint16_t)((e & ~0x3000) | 0x2000);       // STAT_RX -> NAK
    e &= (uint16_t)~0x0800;                       // not a SETUP
    e ^= 0x4000;                                  // DTOG_RX
    e |= 0x8000;                                  // CTR_RX
    dev.interrupt();
    return { ACK, {} };
}

}  // namespace sim
