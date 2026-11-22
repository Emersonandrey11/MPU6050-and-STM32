// Application side: enumeration + USB serial (CDC-ACM) through usb.c/usb_cdc.c.
#include "host.hpp"
#include "usb.h"
#include "usb_cdc.h"
#include "usb_ids.h"

static std::vector<uint8_t> g_rx;
void usb_cdc_on_rx(const uint8_t *data, uint32_t len) { g_rx.insert(g_rx.end(), data, data + len); }

struct App : sim::Device {
    void interrupt() override { usb_poll(); }        // what USB_LP_CAN_RX0_IRQHandler does
};

static std::vector<uint8_t> read_bulk(Host &h, unsigned ep, size_t max_packets, bool *saw_zlp)
{
    std::vector<uint8_t> all;
    *saw_zlp = false;
    for (size_t i = 0; i < max_packets; i++) {
        sim::Reply r = sim::in(h.dev, h.addr, ep);
        if (r.kind != sim::ACK) break;
        all.insert(all.end(), r.data.begin(), r.data.end());
        if (r.data.size() < 64) { *saw_zlp = r.data.empty(); break; }
    }
    return all;
}

int main()
{
    sim::reset_model();
    App app;
    Host h{app};
    std::vector<uint8_t> d;

    std::printf("CDC: enumeration\n");
    usb_cdc_init();
    CHECK(!usb_cdc_ready(), "not configured before enumeration");
    std::vector<uint8_t> cfg = h.enumerate(USB_VID, USB_PID_APP);
    CHECK(cfg.size() == 67 && cfg[4] == 2, "CDC config: 67 bytes, 2 interfaces");
    CHECK(usb_cdc_ready(), "configured after SET_CONFIGURATION");

    std::printf("CDC: class requests\n");
    std::vector<uint8_t> lc = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };    // 115200 8N1
    CHECK(h.control_out(0x21, 0x20, 0, 0, lc), "SET_LINE_CODING 115200");
    CHECK(h.control_in(0xA1, 0x21, 0, 0, 7, d) && d == lc, "GET_LINE_CODING returns it");
    CHECK(h.control_out(0x21, 0x22, 0x0003, 0, {}), "SET_CONTROL_LINE_STATE DTR|RTS");
    CHECK(!h.control_out(0x21, 0x7F, 0, 0, {}), "unknown class request STALLs");
    CHECK(!usb_cdc_bootloader_requested(), "no bootloader request yet");

    std::printf("CDC: device -> PC frames\n");
    std::vector<uint8_t> frame(250);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = (uint8_t)(i * 7 + 3);
    CHECK(usb_cdc_send(frame.data(), (uint32_t)frame.size()) == 0, "send a 250-byte frame");
    CHECK(usb_cdc_send(frame.data(), 10) == -1, "second send refused while busy");
    bool zlp;
    std::vector<uint8_t> got = read_bulk(h, 1, 10, &zlp);
    CHECK(got == frame, "PC received the frame intact (%zu bytes)", got.size());
    CHECK(usb_cdc_send(frame.data(), 10) == 0, "free again after the last packet");
    got = read_bulk(h, 1, 10, &zlp);
    CHECK(got.size() == 10, "10-byte frame");

    std::vector<uint8_t> full(256, 0x5A);                            // exactly 4 full packets
    CHECK(usb_cdc_send(full.data(), 256) == 0, "send 256 bytes");
    got = read_bulk(h, 1, 10, &zlp);
    CHECK(got.size() == 256 && zlp, "256 bytes end with a zero-length packet (got %zu, zlp %d)", got.size(), zlp);
    CHECK(usb_cdc_send(full.data(), 257) == -1, "frames larger than the buffer are refused");

    std::printf("CDC: PC -> device\n");
    std::vector<uint8_t> cmd = { 'M', 'C', 1, 0, 0, 0, 2, 3 };
    CHECK(sim::out(app, h.addr, 2, cmd).kind == sim::ACK, "OUT packet accepted");
    CHECK(g_rx == cmd, "on_rx got the command bytes");
    CHECK(sim::out(app, h.addr, 2, cmd).kind == sim::ACK, "endpoint re-armed for the next packet");

    std::printf("CDC: 1200 baud = enter bootloader\n");
    std::vector<uint8_t> lc1200 = { 0xB0, 0x04, 0, 0, 0, 0, 8 };
    CHECK(h.control_out(0x21, 0x20, 0, 0, lc1200), "SET_LINE_CODING 1200");
    CHECK(usb_cdc_bootloader_requested(), "bootloader requested");

    std::printf("CDC: bus reset\n");
    sim::bus_reset(app);
    CHECK(!usb_cdc_ready(), "not configured after a bus reset");
    h.addr = 0;
    CHECK(h.control_in(0x80, 6, 0x0100, 0, 18, d) && d.size() == 18, "answers at address 0 again");

    std::printf(g_failures ? "CDC: %d FAILURE(S)\n" : "CDC: all checks passed\n", g_failures);
    return g_failures != 0;
}
