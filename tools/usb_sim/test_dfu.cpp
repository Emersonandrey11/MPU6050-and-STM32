// Bootloader side: the DFU download/upload sequence dfu-util performs, the
// start-up decision, and recovery from an interrupted download.
#include <algorithm>
#include <random>

#include "host.hpp"

static uint32_t g_ms;
uint32_t millis(void) { return g_ms; }
void timebase_init(void) {}
void delay_ms(uint32_t ms) { g_ms += ms; }

// Compile the bootloader itself into this file, against simulated flash.
#define FLASH_MEM8(a)   (sim::flash_read8((uint32_t)(a)))
#define FLASH_MEM16(a)  (sim::FlashHalf{(uint32_t)(a)})
#define FLASH_MEM32(a)  (sim::flash_read32((uint32_t)(a)))
#define main bootloader_main
#include "../../bootloader/dfu_bootloader.c"
#undef main

struct Boot : sim::Device {
    void interrupt() override { usb_poll(); }         // the bootloader polls all the time
    void main_loop() override { g_ms++; usb_poll(); dfu_task(); }
};

enum { DNLOAD = 1, UPLOAD = 2, GETSTATUS = 3, CLRSTATUS = 4, GETSTATE = 5, DETACH = 0 };

static std::vector<uint8_t> get_status(Host &h)
{
    std::vector<uint8_t> st;
    CHECK(h.control_in(0xA1, GETSTATUS, 0, 0, 6, st) && st.size() == 6, "GETSTATUS");
    return st;
}

// dfu-util's download loop (DFU 1.1): DNLOAD block n, GETSTATUS until
// dfuDNLOAD-IDLE, ..., zero-length DNLOAD, GETSTATUS through manifestation.
static bool download(Host &h, Boot &b, const std::vector<uint8_t> &img, size_t stop_after_blocks = SIZE_MAX)
{
    uint16_t n = 0;
    for (size_t off = 0; off < img.size(); off += 1024, n++) {
        if (n == stop_after_blocks) return false;
        std::vector<uint8_t> blk(img.begin() + off, img.begin() + std::min(img.size(), off + 1024));
        if (!h.control_out(0x21, DNLOAD, n, 0, blk)) return false;
        for (int tries = 0;; tries++) {
            std::vector<uint8_t> st = get_status(h);
            if (st.size() != 6 || st[4] == dfuERROR || tries > 50) return false;
            if (st[4] == dfuDNLOAD_IDLE) break;
            for (int i = 0; i < (st[1] | st[2] << 8); i++) b.main_loop();     // honour bwPollTimeout
        }
    }
    if (!h.control_out(0x21, DNLOAD, n, 0, {})) return false;
    for (int tries = 0;; tries++) {
        std::vector<uint8_t> st = get_status(h);
        if (st.size() != 6 || st[4] == dfuERROR || tries > 50) return false;
        if (st[4] == dfuIDLE) return true;
        for (int i = 0; i < (st[1] | st[2] << 8); i++) b.main_loop();
    }
}

int main()
{
    std::mt19937 rng(42);
    Boot boot;
    Host h{boot};
    std::vector<uint8_t> d;

    // An application image like ours: SP = top of RAM, reset vector in the app.
    std::vector<uint8_t> img(3 * 1024 + 517);                    // odd length on purpose
    for (auto &x : img) x = (uint8_t)rng();
    const uint32_t sp = 0x20005000, pc = APP_BASE + 0x1339;
    std::memcpy(&img[0], &sp, 4);
    std::memcpy(&img[4], &pc, 4);

    std::printf("DFU: start-up decision\n");
    sim::reset_model();
    CHECK(!app_is_valid(), "erased flash is not a valid application (-> stays in DFU)");

    std::printf("DFU: enumeration\n");
    sim::reset_model();
    usb_init(&s_dfu);
    std::vector<uint8_t> cfg = h.enumerate(USB_VID, USB_PID_DFU);
    CHECK(cfg.size() == 27 && cfg[9 + 5] == 0xFE && cfg[9 + 6] == 0x01 && cfg[9 + 7] == 0x02, "DFU interface (FE/01/02)");
    CHECK(cfg.size() == 27 && cfg[18 + 1] == 0x21 && cfg[18 + 2] == 0x07 && (cfg[18 + 5] | cfg[18 + 6] << 8) == 1024, "DFU functional descriptor");
    CHECK(h.control_in(0xA1, GETSTATE, 0, 0, 1, d) && d.size() == 1 && d[0] == dfuIDLE, "GETSTATE = dfuIDLE");

    std::printf("DFU: download %zu bytes like dfu-util -D\n", img.size());
    for (uint32_t i = 0; i < 2048; i++) sim::flash[BOOTLOADER_SIZE + i] = 0x00;   // an old app (not erased)
    unsigned erases0 = sim::stats.erases;
    CHECK(download(h, boot, img), "download completes and returns to dfuIDLE");
    CHECK(std::memcmp(&sim::flash[BOOTLOADER_SIZE], img.data(), img.size()) == 0, "flash holds the image");
    CHECK(sim::flash[BOOTLOADER_SIZE + img.size()] == 0xFF, "odd length padded with 0xFF");
    CHECK(sim::stats.erases - erases0 == 4, "4 pages erased (%u)", sim::stats.erases - erases0);
    CHECK(sim::stats.program_errors == 0, "no programming errors");
    CHECK(std::all_of(sim::flash.begin(), sim::flash.begin() + BOOTLOADER_SIZE, [](uint8_t x) { return x == 0xFF; }),
          "bootloader area untouched");
    CHECK(app_is_valid(), "image is a valid application");

    std::printf("DFU: upload (dfu-util -U)\n");
    std::vector<uint8_t> back;
    for (uint16_t n = 0;; n++) {
        CHECK(h.control_in(0xA1, UPLOAD, n, 0, 1024, d), "UPLOAD block %u", n);
        back.insert(back.end(), d.begin(), d.end());
        if (d.size() < 1024 || n > 100) break;
    }
    CHECK(back.size() == 56 * 1024, "upload returns the whole 56 KB app area (%zu)", back.size());
    CHECK(std::equal(img.begin(), img.end(), back.begin()), "upload matches the image");

    std::printf("DFU: errors\n");
    std::vector<uint8_t> blk(1024, 0x11);
    CHECK(h.control_out(0x21, DNLOAD, 60, 0, blk), "DNLOAD beyond the end of flash (accepted, checked later)");
    std::vector<uint8_t> st = get_status(h);
    for (int i = 0; i < 5; i++) boot.main_loop();
    st = get_status(h);
    CHECK(st[0] == errADDRESS && st[4] == dfuERROR, "reports errADDRESS / dfuERROR");
    CHECK(!h.control_out(0x21, DNLOAD, 0, 0, blk), "DNLOAD refused in dfuERROR");
    CHECK(h.control_out(0x21, CLRSTATUS, 0, 0, {}), "CLRSTATUS");
    st = get_status(h);
    CHECK(st[0] == OK && st[4] == dfuIDLE, "back to dfuIDLE");

    std::printf("DFU: interrupted download leaves no valid app\n");
    CHECK(!download(h, boot, img, 2), "download stopped after 2 blocks");
    CHECK(!app_is_valid(), "half-written image is not valid -> bootloader stays in DFU");
    CHECK(h.control_out(0x21, 6 /* ABORT */, 0, 0, {}), "ABORT");
    CHECK(download(h, boot, img), "second full download");
    CHECK(app_is_valid(), "valid again");

    std::printf("DFU: restart after dfu-util -R\n");
    CHECK(h.control_out(0x21, DETACH, 1000, 0, {}), "DETACH");
    bool reset = false;
    try { for (int i = 0; i < 200; i++) boot.main_loop(); } catch (sim::Reset &) { reset = true; }
    CHECK(reset, "system reset requested after DETACH");

    std::printf("DFU: start-up after the update\n");
    sim::reg_write(0x40006C04, 0);                                 // BKP_DR1 empty, BOOT1 low
    bool jumped = false;
    try { bootloader_main(); } catch (sim::Jump &j) { jumped = (j.sp == sp && j.pc == pc); }
    CHECK(jumped, "jumps to the application with its SP and reset vector");
    sim::reg_write(0x40006C04, BOOT_REQUEST_MAGIC);
    CHECK(dfu_requested(), "BKP_DR1 magic -> stay in DFU");
    CHECK((sim::reg_read(0x40006C04) & 0xFFFF) == 0, "magic cleared (one shot)");
    sim::reg_write(0x40010C08, 1u << 2);                           // GPIOB IDR: BOOT1 jumper high
    CHECK(dfu_requested(), "BOOT1 = 1 -> stay in DFU");

    std::printf(g_failures ? "DFU: %d FAILURE(S)\n" : "DFU: all checks passed\n", g_failures);
    return g_failures != 0;
}
