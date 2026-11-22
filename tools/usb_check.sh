#!/usr/bin/env bash
# Is the Blue Pill visible on its native USB port? Run after plugging it in.
#   ./tools/usb_check.sh          wait up to 10 s and report
# Exit code 0 = application (or bootloader) found, 1 = not found.
set -uo pipefail

VID=1209
PID_APP=0001
PID_DFU=0002

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

echo "Looking for the board on USB (up to 10 s)..."
for _ in $(seq 20); do
    if lsusb | grep -q "ID $VID:$PID_APP"; then
        PORT=$(app_port)
        echo "OK: application running, USB serial ${PORT:-(port not created yet)}"
        lsusb | grep "ID $VID:$PID_APP"
        echo "Live data:      python3 tools/mpu6050_monitor.py --usb --web"
        echo "Update the app: ./flash-usb.sh"
        exit 0
    fi
    if lsusb | grep -q "ID $VID:$PID_DFU"; then
        echo "OK: DFU bootloader running (LED double-blinks). That happens when"
        echo "  - there is no valid application yet -> run ./flash-usb.sh, or"
        echo "  - the BOOT1 jumper is at 1 -> set it to 0 and press RESET."
        exit 0
    fi
    sleep 0.5
done

echo "NOT FOUND: neither 1209:0001 (application) nor 1209:0002 (bootloader)."
LOG=$(journalctl -k --since "-3 min" --no-pager 2>/dev/null || dmesg 2>/dev/null)
EVENTS=$(echo "$LOG" | grep -E 'usb [0-9]+-[0-9.]+:' | tail -6)
if [[ -z "$EVENTS" ]]; then
    cat <<'EOF'
The PC saw no USB device at all in the last 3 minutes:
  - Is the cable a data cable? Many micro-USB cables only charge.
  - Is the board powered (red power LED on)? Try another USB port.
EOF
elif echo "$EVENTS" | grep -q -E 'error -71|unable to enumerate|not accepting address|descriptor read'; then
    echo "The PC sees the board (D+ pull-up) but it does not answer USB requests:"
    echo "$EVENTS" | sed 's/^/    /'
    cat <<'EOF'
  - New firmware not installed yet? Flash once with the ST-Link: ./flash.sh
  - R10 (D+ to 3.3 V) must be 1.5 kOhm; 10k/4.7k on many Blue Pills -> replace it.
  - The 8 MHz crystal must run (USB needs it); a board without it cannot do USB.
  - The LED on PC13 should blink slowly (sensor OK) or fast (no sensor): if it does
    not blink at all, the application is not running.
EOF
else
    echo "Last kernel USB messages:"
    echo "$EVENTS" | sed 's/^/    /'
fi
exit 1
