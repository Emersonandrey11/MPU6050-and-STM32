#!/usr/bin/env bash
# Build the Debug configuration and flash it to the STM32F103C8T6 via ST-Link V2.
# Writes both images: the USB DFU bootloader (0x08000000) and the application
# (0x08002000). After this once, ./flash-usb.sh can update the app over USB.
# Uses the toolchain bundles installed by the STM32Cube VS Code extension.
#
# Wiring: ST-Link SWDIO, SWCLK, GND to the Blue Pill. If the board is powered
# from its micro-USB port, leave the ST-Link's 3.3 V pin unconnected (two
# supplies on one rail); without USB, connect 3.3 V too so the ST-Link powers it.
set -euo pipefail
cd "$(dirname "$0")"

BUNDLES="$HOME/.local/share/stm32cube/bundles"
export PATH="$BUNDLES/gnu-tools-for-stm32/14.3.1+st.2/bin:$BUNDLES/ninja/1.13.2+st.1/bin:$BUNDLES/cmake/4.4.0+st.1/bin:$PATH"

cmake --preset Debug
cmake --build --preset Debug

ELF=build/Debug/mpu6050.elf
BIN=build/Debug/mpu6050.bin
BOOT_ELF=build/Debug/bootloader.elf
BOOT_BIN=build/Debug/bootloader.bin
arm-none-eabi-objcopy -O binary "$ELF" "$BIN"
arm-none-eabi-objcopy -O binary "$BOOT_ELF" "$BOOT_BIN"
arm-none-eabi-size "$BOOT_ELF" "$ELF"

# Fail early, with a clear message, when no ST-Link is plugged in
# (USB vendor 0483 = STMicroelectronics: 3748 V2, 374b V2-1, 374e/374f/3753/3754 V3).
if command -v lsusb >/dev/null && ! lsusb | grep -q -E 'ID 0483:(3748|374b|374e|374f|3752|3753|3754)'; then
    cat >&2 <<'EOF'

No ST-Link found on USB (lsusb shows no 0483:37xx device).
  - Plug the ST-Link into the PC (try another port or cable).
  - Wire SWDIO, SWCLK, GND to the Blue Pill (+3.3 V only if the board has no USB power).
The build above is done; run ./flash.sh again once the ST-Link is connected.
EOF
    exit 1
fi

if pgrep -x openocd >/dev/null; then
    echo "openocd is running and holds the ST-Link: stop mpu6050_monitor.py / openocd first." >&2
    exit 1
fi

if command -v st-flash >/dev/null; then
    if ! { st-flash write "$BOOT_BIN" 0x08000000 && st-flash --reset write "$BIN" 0x08002000; }; then
        cat >&2 <<'EOF'

Flashing failed. Things to try:
  1. Check the 4 ST-Link wires (SWDIO, SWCLK, GND, 3.3V) and that BOOT0 is on 0.
  2. Hold the board's RESET button, run ./flash.sh, release RESET about 1 s later.
  3. Run "st-info --probe" to check whether the chip is seen at all.
EOF
        exit 1
    fi
elif command -v STM32_Programmer_CLI >/dev/null; then
    STM32_Programmer_CLI -c port=SWD -w "$BOOT_ELF" -v -w "$ELF" -v -rst
elif command -v openocd >/dev/null; then
    openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program $BOOT_ELF verify" -c "program $ELF verify reset exit"
else
    echo "No flasher found. Install one, e.g.: sudo apt install stlink-tools" >&2
    exit 1
fi

cat <<'EOF'

Done: USB bootloader at 0x08000000 and application at 0x08002000.
Next:
  - The board restarts into the application and connects to the PC over its
    micro-USB port (if that cable is plugged in). Check with:  ./tools/usb_check.sh
  - USB-only from now on: unplug the ST-Link, power the board from micro-USB,
    update with ./flash-usb.sh, watch with: python3 tools/mpu6050_monitor.py --usb --web
EOF
