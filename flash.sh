#!/usr/bin/env bash
# Build the Debug configuration and flash it to the STM32F103C8T6 via ST-Link V2.
# Uses the toolchain bundles installed by the STM32Cube VS Code extension.
set -euo pipefail
cd "$(dirname "$0")"

BUNDLES="$HOME/.local/share/stm32cube/bundles"
export PATH="$BUNDLES/gnu-tools-for-stm32/14.3.1+st.2/bin:$BUNDLES/ninja/1.13.2+st.1/bin:$BUNDLES/cmake/4.4.0+st.1/bin:$PATH"

cmake --preset Debug
cmake --build --preset Debug

ELF=build/Debug/mpu6050.elf
BIN=build/Debug/mpu6050.bin
arm-none-eabi-objcopy -O binary "$ELF" "$BIN"
arm-none-eabi-size "$ELF"

if pgrep -x openocd >/dev/null; then
    echo "openocd is running and holds the ST-Link: stop mpu6050_monitor.py / openocd first." >&2
    exit 1
fi

if command -v st-flash >/dev/null; then
    if ! st-flash --reset write "$BIN" 0x08000000; then
        cat >&2 <<'EOF'

Flashing failed. Things to try:
  1. Check the 4 ST-Link wires (SWDIO, SWCLK, GND, 3.3V) and that BOOT0 is on 0.
  2. Hold the board's RESET button, run ./flash.sh, release RESET about 1 s later.
  3. Run "st-info --probe" to check whether the chip is seen at all.
EOF
        exit 1
    fi
elif command -v STM32_Programmer_CLI >/dev/null; then
    STM32_Programmer_CLI -c port=SWD -w "$ELF" -v -rst
elif command -v openocd >/dev/null; then
    openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program $ELF verify reset exit"
else
    echo "No flasher found. Install one, e.g.: sudo apt install stlink-tools" >&2
    exit 1
fi
