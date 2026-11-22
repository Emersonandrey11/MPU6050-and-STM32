#!/usr/bin/env bash
# Build the application and flash it over the Blue Pill's own micro-USB port,
# using the project's DFU bootloader (put there once with ./flash.sh + ST-Link).
#
#   ./flash-usb.sh            build + flash
#   ./flash-usb.sh --no-build flash the last build
#
# If the application is running, it is asked to restart into the bootloader by
# opening its USB serial port at 1200 baud. If that does not work (e.g. the
# app is broken), set the BOOT1 jumper to 1, press RESET, and run this again;
# put the jumper back to 0 afterwards.
#
# Linux needs a udev rule so dfu-util may open the device without sudo:
#   sudo cp tools/udev/50-mpu6050-usb.rules /etc/udev/rules.d/
#   sudo udevadm control --reload-rules && sudo udevadm trigger
set -euo pipefail
cd "$(dirname "$0")"

VID=1209
PID_APP=0001
PID_DFU=0002

if [[ "${1:-}" != "--no-build" ]]; then
    BUNDLES="$HOME/.local/share/stm32cube/bundles"
    export PATH="$BUNDLES/gnu-tools-for-stm32/14.3.1+st.2/bin:$BUNDLES/ninja/1.13.2+st.1/bin:$BUNDLES/cmake/4.4.0+st.1/bin:$PATH"
    cmake --preset Debug
    cmake --build --preset Debug --target mpu6050
fi

ELF=build/Debug/mpu6050.elf
DFU=build/Debug/mpu6050.dfu
arm-none-eabi-objcopy -O binary "$ELF" "$DFU"
arm-none-eabi-size "$ELF"
# A DFU suffix (VID/PID/CRC) lets dfu-util check the file is meant for this device.
if command -v dfu-suffix >/dev/null; then
    dfu-suffix -v "$VID" -p "$PID_DFU" -a "$DFU" >/dev/null
fi

command -v dfu-util >/dev/null || { echo "dfu-util not found: sudo apt install dfu-util" >&2; exit 1; }

dfu_present() { dfu-util -l 2>/dev/null | grep -q "\[$VID:$PID_DFU\]"; }

app_port() {
    for tty in /sys/class/tty/ttyACM*; do
        [[ -e "$tty" ]] || continue
        dev=$(readlink -f "$tty/device/..")
        if [[ "$(cat "$dev/idVendor" 2>/dev/null)" == "$VID" && "$(cat "$dev/idProduct" 2>/dev/null)" == "$PID_APP" ]]; then
            echo "/dev/$(basename "$tty")"
            return
        fi
    done
}

if ! dfu_present; then
    PORT=$(app_port)
    if [[ -z "$PORT" ]]; then
        cat >&2 <<'EOF'
Neither the application (1209:0001) nor the bootloader (1209:0002) is on USB.
  - Is the board connected with a data USB cable (not charge-only)?
  - First time? The chip has no USB code yet, so the USB bootloader must be written
    once by another route (the STM32F103's ROM bootloader cannot use USB):
      ST-Link:            ./flash.sh
      USB-serial adapter: TX->PA10, RX->PA9, GND; BOOT0 = 1, press RESET, then
        stm32flash -w build/Debug/bootloader.bin -v /dev/ttyUSB0
        stm32flash -w build/Debug/mpu6050.bin -v -S 0x08002000 /dev/ttyUSB0
        BOOT0 = 0, press RESET.
    "dmesg" showing "device descriptor read ... error -71" is this case.
  - Bootloader already installed but the app is broken: set the BOOT1 jumper to 1,
    press RESET, run this script again (BOOT1 back to 0 afterwards).
EOF
        exit 1
    fi
    echo "Asking the application on $PORT to restart into the bootloader (1200 baud)..."
    stty -F "$PORT" 1200
    for _ in $(seq 50); do
        dfu_present && break
        sleep 0.1
    done
    dfu_present || { echo "The bootloader did not appear on USB." >&2; exit 1; }
fi

# -R: reset when done, so the bootloader starts the new application.
dfu-util -d "$VID:$PID_DFU" -a 0 -D "$DFU" -R

# Linux remembers the 1200 baud we used above for this port name, and a program
# that later changes only some port settings would re-send 1200 and restart the
# board into the bootloader. Put the port back to 115200 once the app is back.
for _ in $(seq 50); do
    PORT=$(app_port)
    if [[ -n "$PORT" ]]; then
        stty -F "$PORT" 115200 raw -echo 2>/dev/null && echo "Application is back on $PORT."
        break
    fi
    sleep 0.1
done
