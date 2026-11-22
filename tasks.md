# Tasks

Status of the work on this project and what comes next. Newest first.
Nothing below is committed yet: the last commit is `84e9c18` (project card).

## Current state (2026-11-21)

### Done

**Native USB (power, live data, flashing), bare metal**
- [x] Clock moved to 8 MHz crystal × 6 = 48 MHz (`Src/clock.c`, `SystemInit()`), as USB needs an exact 48 MHz.
      All timings (SysTick, I²C CCR/TRISE, SPI, TIM3, loop timeouts) derive from `Inc/board.h`.
- [x] USB device core `Src/usb.c` (EP0, standard requests, packet memory, EPnR helpers).
- [x] USB serial class `Src/usb_cdc.c` (CDC-ACM, `1209:0001`); `main.c` streams `g_telemetry` + CRC-16
      after every sample and accepts `'M' 'C'` command frames; 1200 baud = restart into the bootloader.
- [x] DFU bootloader `bootloader/dfu_bootloader.c` (8 KB at 0x08000000, `1209:0002`); app moved to
      0x08002000 (`stm32f103x8_flash.ld`); block 0 written last so an interrupted update never looks valid.
- [x] Scripts: `flash.sh`/`flash.ps1` write bootloader + app over ST-Link (with an ST-Link pre-check and
      next-step hints); `flash-usb.sh`/`flash-usb.ps1` flash the app over USB with dfu-util;
      `tools/usb_check.sh` (is the board on USB? why not?); `tools/swd_diag.py` (read-only chip state over SWD);
      `tools/udev/50-mpu6050-usb.rules` (installed on this PC).
- [x] Monitor: `python3 tools/mpu6050_monitor.py --usb [PORT]` reads the USB serial frames.
- [x] Host tests: `tools/usb_sim/run.sh` runs the real USB/CDC/DFU code against a model of the peripheral
      (all checks pass; 3 planted bugs were caught).
- [x] Docs: README, INSTALL (step 8), HARDWARE, CLAUDE.md, study guide (clock values, re-synced ELF
      addresses, "Native USB port" section), slide deck "STM32F103 USB, from the wire to DFU" (claude.ai, private).

**2.8" ST7789V TFT dashboard**
- [x] SPI1 + ST7789V drivers, fonts, incremental UI (`ui.c`), PWM backlight on PB0 (`LCD_BRIGHTNESS`),
      wiring schematic `docs/wiring-tft.svg`, PC preview `tools/lcd_preview`.

### Verified on the real board

- [x] `./flash.sh` over ST-Link writes both images; read back over SWD, both match the build byte for byte.
- [x] On USB power only the board enumerates as `1209:0001`, `/dev/ttyACM0` appears, udev adds `/dev/mpu6050`.
- [x] It streams valid frames (1,368 CRC-checked frames in 20 s, status `0x07`: sensor OK, self-test OK, INT pin).
- [x] **Flashing over USB works:** `./flash-usb.sh` at 20:06: app → 1200 baud → bootloader `1209:0002` →
      dfu-util → app back on `/dev/ttyACM0` (kernel log). Also once at 19:11.
- [x] **Web dashboard over USB works:** `python3 tools/mpu6050_monitor.py --usb --web` shows connected,
      source `native USB (/dev/ttyACM0)`, chip MPU-6500 at 0x68, live events; 100 frames/s, no I²C errors.
- [x] **Commands over USB work:** accel range ±2 g → ±8 g → ±2 g from the dashboard API, confirmed by the board.

### Fixed after the first hardware test

- [x] Dashboard started in the wrong mode: `--web` alone means ST-Link. Over USB: `--usb --web`.
- [x] Monitor `SerialPort` now sets raw mode and 115200 in one `tcsetattr()` (no `tty.setraw()` first).
- [x] `flash-usb.sh` puts the port back to 115200 after flashing, so the remembered 1200 baud cannot restart the board.

### Open problems (seen once on hardware, not seen since the fixes above)

1. **Board restarted about every 5 s while a program read the port** (19:13–19:14). Uptime and sample count start over,
   the PC logs a disconnect/re-enumeration (once `usb1-port9: disabled by hub (EMI?)`).
   With the port closed: no restarts in 15 s.
2. **Silent state.** From 19:14:32 the board stayed connected and answers USB control requests (opening the
   port works), but sends no frames at all, so the main loop or its transmit path is stuck.

Both happened while test readers used `tty.setraw()` right after a `flash-usb.sh` run (port left at 1200 baud),
which matches the first lead below. Since the fixes: 8 s raw read with 800 frames and continuous uptime, then
the dashboard and commands, with no restart. Keep watching; if it comes back, go to "Next" step 1.

Leads, none confirmed yet:
- The only code that restarts the app is `enter_bootloader()` (1200 baud or command 8). If the bootloader
  then misses the `BKP_DR1` magic, it jumps straight back to the app, which looks like a plain restart.
  Possible cause: no read-back delay after enabling the PWR/BKP clocks (ST's own F1 HAL adds one).
- Python's `tty.setraw()` re-sends the port's *current* speed before the monitor sets 115200; after a
  `stty … 1200` that speed is 1200. Checked: `ttyACM0` is now at 115200, so this explains at most the first restart.
- Transmit deadlock: `s_tx_busy` stuck at 1 with EP1 not VALID would give exactly the silent state.
- Electrical: display SPI wires next to the USB lines, R10 value (the PC detects the board, so R10 is at least close).

## Next

1. **Look inside the chip while it misbehaves** (needs the ST-Link, SWDIO/SWCLK/GND only, USB stays in):
   `python3 tools/swd_diag.py` in the silent state and right after a restart. It shows PC/LR (where it is stuck),
   `RCC_CSR` reset flags (watchdog / brown-out / software / pin), USB registers, `s_tx_busy`, uptime.
2. **Without the ST-Link:** add USB-visible diagnostics to the firmware: reset cause (`RCC_CSR`, then clear
   with `RMVF`) and a boot counter in a backup register, published in spare `g_telemetry.regs[]` bytes.
   Flash with `./flash-usb.sh` right after a power cycle (the main loop must be running for the 1200-baud
   restart), or BOOT1 = 1 + RESET if it is stuck.
3. **Fixes to make regardless of the diagnosis:**
   - Read back `RCC_APB1ENR` after enabling PWR/BKP (app `enter_bootloader()` and bootloader `dfu_requested()`).
   - `usb_cdc.c` `tx_next()`: check `usb_ep_write()`'s result instead of advancing past a failed write;
     recover if `s_tx_busy` is set while EP1 is not VALID.
   - Only accept the 1200-baud request when DTR drops afterwards (like Arduino), so a stray 1200 setting
     cannot restart the board.
4. Extend `tools/usb_sim` with a test for each fix, run it, rebuild without warnings, flash, re-test on the board:
   `./tools/usb_check.sh`, then `python3 tools/mpu6050_monitor.py --usb --web` for several minutes.
5. When it is stable: commit the TFT and USB work (local-only files `GITHUB.md` and `linkedin/` stay out)
   and push (GitHub Pages republishes the guide).

## Later

- [ ] Own USB product IDs from pid.codes (the current ones are test IDs).
- [ ] Windows: WCID descriptors in the bootloader so DFU needs no Zadig step.
- [ ] TFT pixels via DMA (DMA1 channel 3 = SPI1_TX).
- [ ] Study-guide chapter on the USB stack (the slide deck covers it for now).
- [ ] Revoke the GitHub token stored in `GITHUB.md`.
