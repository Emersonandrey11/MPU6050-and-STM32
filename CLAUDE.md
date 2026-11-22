# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A bare-metal (no HAL, no CMSIS, no CubeMX code) MPU6050 driver for the STM32F103C8T6 "Blue Pill", plus a Python
PC tool that reads the sensor data out of the MCU's RAM through the ST-Link (no UART) or over the board's native USB port
(bare-metal USB stack, CDC serial). A USB DFU bootloader (also bare metal) lets the app be flashed over USB. The repo is also a learning
resource: `docs/index.html` is a 23-chapter study guide published on GitHub Pages, and explanations matter as much
as code.

## Commands

```bash
./flash.sh                                    # build Debug and flash BOTH images over ST-Link (bootloader + app)
./flash-usb.sh                                # build the app and flash it over native USB (1200-baud touch + dfu-util)
cmake --preset Debug && cmake --build --preset Debug   # build only -> build/Debug/{mpu6050,bootloader}.elf (Release preset also exists)
./tools/usb_sim/run.sh                        # USB core + CDC + DFU bootloader against a host model of the peripheral (exit 0 = pass)
python3 tools/mpu6050_monitor.py --demo --web # dashboard with a simulated sensor, no hardware (http://127.0.0.1:8000)
python3 tools/mpu6050_monitor.py --web        # live data; starts openocd itself on TCL port 6666
python3 tools/mpu6050_monitor.py --usb --web  # live data over native USB (auto-finds 1209:0001; Windows: --usb COM5)
python3 tools/mpu6050_monitor.py --probe      # raw OpenOCD/ELF diagnostics when the live path fails
python3 tools/gen_font.py                     # regenerate Src/font_data.c + Inc/font.h (needs Pillow + X11 misc fonts)
# render the TFT dashboard on the PC (no hardware): same ui.c/gfx.c/fonts, fake panel -> PPM image
gcc -std=c11 -O2 -Wall -Wextra -IInc tools/lcd_preview/lcd_preview.c Src/ui.c Src/gfx.c Src/font_data.c -lm -o build/lcd_preview
./build/lcd_preview build/lcd_preview.ppm      # add --no-sensor for the error screen; prints SPI bytes per ui_task() call
```

- `flash.sh` prepends the toolchain bundles installed by the STM32Cube VS Code extension
  (`~/.local/share/stm32cube/bundles/...`) to `PATH`; a plain `cmake` call needs `arm-none-eabi-gcc`, `cmake` and `ninja`
  on `PATH` itself. Windows equivalent: `flash.ps1` (see `INSTALL.md`).
- Flashing fails while `openocd` (or the monitor) holds the ST-Link; stop it first.
- Automated tests exist only for host-runnable parts: `tools/usb_sim/run.sh` (USB/DFU) and `tools/lcd_preview` (TFT).
  No linters. Hardware-dependent code is verified manually. The PR checklist (`CONTRIBUTING.md`) is: `./flash.sh` builds **without
  warnings** (`-Wall -Wextra -Wpedantic`), `--demo --web` still works, and on hardware the live data and self-test pass.
- clangd reads `build/Debug/compile_commands.json` (`.clangd`), so build once before relying on code navigation.

## Architecture

Firmware layers, each using only the one below (`README.md` "Software layers"):

1. `Src/main.c`: super loop: handle PC command → poll sensor → register dump every second → LED heartbeat →
   `ui_task()` → `usb_stream()`; retries `mpu_start()` every second while the sensor does not answer.
2. `Src/mpu6050.c`: sensor driver. Contains **no STM32 registers**, only `i2c_read()` / `i2c_write()`, so it ports to
   another MCU by rewriting `i2c.c` alone. Detects the MPU6500 family by `WHO_AM_I` (0x70/0x71/0x73/0x74) and adapts.
3. `Src/i2c.c`: polling I2C1 driver following the AN2824 1/2/N-byte read sequences, with ES096 errata workarounds and
   stuck-bus recovery.
4. `Inc/stm32f103_regs.h`, `Src/timebase.c`: hand-written register addresses/bit names and a 1 ms SysTick.
5. `Src/startup_stm32f103xx.S`, `stm32f103x8_flash.ld`: vector table, startup, memory map (ST-supplied; keep ST terms).

Clock: `Src/clock.c` `SystemInit()` runs HSE 8 MHz × 6 = 48 MHz (USB needs exactly 48 MHz), APB1 /2 = 24 MHz, and sets
VTOR. Every timing (SysTick, I2C CCR/TRISE, SPI divider, TIM3 period, loop-count timeouts) derives from the `*_HZ`
constants in `Inc/board.h`; change them together with `clock.c`. I2C runs at 100 kHz; pin map is in `board.h` / `HARDWARE.md`.

### Native USB, flash split and bootloader

- Flash split (`Inc/usb_ids.h`): bootloader 8 KB at 0x08000000 (`bootloader/`, own linker script, always `-Os`), app at
  0x08002000 (`stm32f103x8_flash.ld`, 56 KB). Flashing only the app over SWD leaves no bootloader: use `flash.sh`.
- `Src/usb.c` is the device core (EP0, standard requests) behind a `usb_class_t`; `usb_cdc.c` (app, IRQ 20 driven)
  and `bootloader/dfu_bootloader.c` (polled) are the two classes. Hard-won rules, all exercised by `tools/usb_sim`:
  EPnR STAT/DTOG bits toggle on write-1 and CTR bits clear on write-0 (use the `ep_*` helpers, never `|=`); PMA is 16-bit
  words at a 32-bit stride (`0x40006000 + 2*off`); SET_ADDRESS takes effect after the status stage.
- USB telemetry frame = raw `g_telemetry` + CRC-16/XMODEM (= Python `binascii.crc_hqx`); commands are `'M''C' seq id arg`
  written into the same `cmd_*` fields as the SWD path. 1200 baud (SET_LINE_CODING) or `TM_CMD_ENTER_BOOTLOADER` →
  `BKP_DR1 = 0xB007` + reset → bootloader stays in DFU. BOOT1 (PB2) = 1 also forces DFU.
- DFU writes block 0 (vector table) last, so an interrupted update never looks valid to `app_is_valid()`.
- The D+ pull-up is fixed, so `usb_init()` drives PA12 low 10 ms to force re-enumeration. VID:PID 1209:0001/0002 are
  pid.codes test IDs (not for distribution).
- When USB code changes, run `tools/usb_sim/run.sh`; when it changes how the peripheral is driven, extend the model in
  `tools/usb_sim/sim.cpp` rather than loosening a check.

### Optional TFT dashboard (ST7789V 240x320 on SPI1)

`ui.c` → `gfx.c` → `st7789.c` → `spi.c`. Pins: PA5 SCK, PA7 MOSI, PA6 MISO, PA4 CS, PA3 DC, PA2 RESET, PB0 backlight
(`board.h`, schematic `docs/wiring-tft.svg`). Things that are not obvious from one file:

- `ui.c` reads only `g_telemetry` and contains no STM32 registers; `st7789.h` exposes just four window/pixel calls.
  That is what lets `tools/lcd_preview` compile the real `ui.c`/`gfx.c` on a PC — keep both properties.
- SPI is capped at 4 MHz (8 MHz PCLK2 / 2), so a full screen costs ~0.3 s. `ui_task()` must stay incremental: per call
  it draws one chart column pair or at most `CHARS_PER_CALL` changed glyphs (~1.4 ms worst case) so the 100 Hz sensor
  reads keep their timing. Check the preview's "worst call" figure after UI changes.
- Font glyph codes: ASCII 32–126, then `FONT_DEG` = 0x7F and `FONT_PM` = 0x80 (asserted in `gen_font.py`).
- `atan2f`/`sqrtf` need `m` in `target_link_libraries` (CMakeLists.txt): the `-lm` in the managed `cmake/flags.cmake`
  comes before the object files on the link line and does not resolve them.
- Panel quirks (colour inversion, RGB/BGR, rotation) are `LCD_INVERT` / `LCD_MADCTL` in `board.h`. Backlight is
  TIM3_CH3 PWM on PB0 (20 kHz, `LCD_BRIGHTNESS` %); it is the biggest load on the 3.3 V rail.

### PC ↔ firmware link (the non-obvious part)

`g_telemetry` (`Inc/telemetry.h`) is a 248-byte struct in RAM that acts as a mailbox:

- The monitor finds its address by looking up the symbol in `build/Debug/mpu6050.elf`, then reads it through
  OpenOCD's TCL socket while the MCU runs. It writes commands into the `cmd_*` fields; firmware answers via `cmd_ack`.
- A sequence lock prevents torn reads: firmware bumps `seq_tail` before writing and copies it to `seq_head` after;
  the PC accepts a block only when they match.
- **Layout is a cross-language contract.** Changing `telemetry_t` requires updating `TELEMETRY_FORMAT` in
  `tools/mpu6050_monitor.py` and the `_Static_assert`s in `telemetry.h` together.
- `main()` sets `DBGMCU_CR_DBG_SLEEP` and deliberately never uses `WFI`: RAM reads during sleep returned zeros.
- `dump_registers()` deliberately skips `INT_STATUS` (clears on read), the DMP window 0x6D–0x71 and `FIFO_R_W` 0x74
  (pops the FIFO). Keep those out of any new register reads.

`tools/mpu6050_monitor.py` is Python 3 standard library only (no `pip install`): it converts raw values to units,
runs a complementary filter for angles, and serves `tools/dashboard.html` over Server-Sent Events. The MPU6500 family
uses a different temperature formula (`raw/333.87 + 21`) from the MPU6050 (`raw/340 + 36.53`).

## Code conventions (from CONTRIBUTING.md)

- C11; every register access goes through the names in `Inc/stm32f103_regs.h`.
- Every wait loop has a timeout. Functions return `0` on success, `-1` on error.
- Comments explain *why* and cite the reference manual section or application note (RM0008, PM0056, AN2824, ES096,
  MPU-6000 register map rev 4.2).

## Docs and publishing

- GitHub Pages serves `main:/docs`, so pushing to `main` publishes the guide.
- The guide quotes real addresses/sizes from the Debug ELF (linker chapter, OpenOCD examples, `g_telemetry` address);
  re-sync them when the memory layout changes.
- `docs/index.html` must stay a single self-contained file that works offline: images (logo, project card) are
  embedded as base64 data URIs. `docs/logo.svg`, `docs/project-card.jpg`, `docs/wiring-tft.svg` and
  `docs/lcd-preview.png` are the standalone copies the README uses; re-embed them in the guide when they change.
  When the code changes in a way the guide describes (addresses, register values, struct offsets), update the guide.

## Local-only files

`GITHUB.md` (contains a GitHub token) and `linkedin/` (social-media image sources) are excluded through
`.git/info/exclude`, not `.gitignore`. Never stage them; `git add -f` is the one way they could slip in.
