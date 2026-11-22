// A tiny USB host for the tests: control transfers built from single
// SETUP/IN/OUT transactions, with NAK retries like a real host controller.
#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "sim.hpp"

static int g_failures;
static const int g_unbuffered = std::setvbuf(stdout, nullptr, _IONBF, 0);
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                              std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Host {
    sim::Device &dev;
    uint8_t addr = 0;

    sim::Reply in_retry(unsigned ep)
    {
        for (int i = 0; i < 2000; i++) {
            sim::Reply r = sim::in(dev, addr, ep);
            if (r.kind != sim::NAK) return r;
            dev.main_loop();
        }
        return { sim::NAK, {} };
    }

    sim::Reply out_retry(unsigned ep, const std::vector<uint8_t> &d)
    {
        for (int i = 0; i < 2000; i++) {
            sim::Reply r = sim::out(dev, addr, ep, d);
            if (r.kind != sim::NAK) return r;
            dev.main_loop();
        }
        return { sim::NAK, {} };
    }

    static void pack(uint8_t p[8], uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
    {
        p[0] = bm; p[1] = req; p[2] = (uint8_t)val; p[3] = (uint8_t)(val >> 8);
        p[4] = (uint8_t)idx; p[5] = (uint8_t)(idx >> 8); p[6] = (uint8_t)len; p[7] = (uint8_t)(len >> 8);
    }

    // Returns false on STALL. Data stage reads until a short packet or wLength.
    bool control_in(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, std::vector<uint8_t> &data)
    {
        uint8_t p[8];
        pack(p, bm, req, val, idx, len);
        data.clear();
        if (sim::setup(dev, addr, p).kind != sim::ACK) return false;
        for (;;) {
            sim::Reply r = in_retry(0);
            if (r.kind == sim::STALL) return false;
            if (r.kind != sim::ACK) { CHECK(false, "IN data stage: no reply (req %u)", req); return false; }
            data.insert(data.end(), r.data.begin(), r.data.end());
            if (r.data.size() < 64 || data.size() >= len) break;
        }
        sim::Reply s = out_retry(0, {});                    // status stage: zero-length OUT
        CHECK(s.kind == sim::ACK, "status OUT not acknowledged (req %u, kind %d)", req, s.kind);
        return s.kind == sim::ACK;
    }

    bool control_out(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, const std::vector<uint8_t> &data)
    {
        uint8_t p[8];
        pack(p, bm, req, val, idx, (uint16_t)data.size());
        if (sim::setup(dev, addr, p).kind != sim::ACK) return false;
        for (size_t off = 0; off < data.size(); off += 64) {
            std::vector<uint8_t> chunk(data.begin() + off, data.begin() + std::min(data.size(), off + 64));
            sim::Reply r = out_retry(0, chunk);
            if (r.kind == sim::STALL) return false;
            CHECK(r.kind == sim::ACK, "OUT data stage not acknowledged (req %u)", req);
        }
        sim::Reply s = in_retry(0);                         // status stage: zero-length IN
        if (s.kind == sim::STALL) return false;
        CHECK(s.kind == sim::ACK && s.data.empty(), "status IN wrong (req %u, kind %d, %zu bytes)", req, s.kind, s.data.size());
        return s.kind == sim::ACK;
    }

    // Standard enumeration up to SET_CONFIGURATION; returns the config descriptor.
    std::vector<uint8_t> enumerate(uint16_t vid, uint16_t pid)
    {
        std::vector<uint8_t> d;
        sim::bus_reset(dev);
        addr = 0;
        CHECK(control_in(0x80, 6, 0x0100, 0, 64, d) && d.size() == 18, "GET_DESCRIPTOR(device) at address 0");
        CHECK(d.size() == 18 && d[7] == 64 && (d[8] | d[9] << 8) == vid && (d[10] | d[11] << 8) == pid,
              "device descriptor: EP0 size / VID / PID");
        sim::bus_reset(dev);                                 // Windows-style second reset
        CHECK(control_out(0x00, 5, 7, 0, {}), "SET_ADDRESS(7)");
        addr = 7;
        CHECK(sim::in(dev, 0, 0).kind == sim::NOREPLY, "device still answers at address 0 after SET_ADDRESS");
        CHECK(control_in(0x80, 6, 0x0100, 0, 18, d) && d.size() == 18, "GET_DESCRIPTOR(device) at the new address");
        CHECK(control_in(0x80, 6, 0x0200, 0, 9, d) && d.size() == 9, "GET_DESCRIPTOR(config, 9 bytes)");
        if (d.size() < 4) return {};
        uint16_t total = (uint16_t)(d[2] | d[3] << 8);
        std::vector<uint8_t> cfg;
        CHECK(control_in(0x80, 6, 0x0200, 0, total, cfg) && cfg.size() == total, "GET_DESCRIPTOR(config, %u bytes)", total);
        size_t pos = 0;                                      // descriptors must chain to exactly wTotalLength
        while (pos < cfg.size() && cfg[pos] != 0) pos += cfg[pos];
        CHECK(pos == total, "descriptor lengths add up to %zu, wTotalLength says %u", pos, total);
        CHECK(control_in(0x80, 6, 0x0300, 0, 255, d) && d.size() == 4 && d[2] == 0x09 && d[3] == 0x04, "string 0 (languages)");
        for (uint8_t i = 1; i <= 3; i++) {
            CHECK(control_in(0x80, 6, (uint16_t)(0x0300 | i), 0x0409, 255, d) && d.size() >= 4 && d[0] == d.size() && d[1] == 3,
                  "string descriptor %u", i);
        }
        CHECK(control_in(0x80, 6, 0x0300 | 3, 0x0409, 255, d) && d.size() == 2 + 2 * 24, "serial number: 24 hex characters");
        CHECK(!control_in(0x80, 6, 0x0600, 0, 10, d), "device qualifier must STALL (full-speed only device)");
        CHECK(control_in(0x80, 6, 0x0100, 0, 64, d), "EP0 still works after a STALL");
        CHECK(control_out(0x00, 9, 1, 0, {}), "SET_CONFIGURATION(1)");
        CHECK(control_in(0x80, 8, 0, 0, 1, d) && d.size() == 1 && d[0] == 1, "GET_CONFIGURATION = 1");
        return cfg;
    }
};
