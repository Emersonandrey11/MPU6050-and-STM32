#!/usr/bin/env bash
# Run the firmware's USB code (Src/usb.c, Src/usb_cdc.c, bootloader/) on the
# PC against a model of the STM32F103 USB peripheral and flash, driven by a
# scripted USB host. No hardware needed. Exit code 0 = all checks passed.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=build/usb_sim
mkdir -p "$OUT"
python3 tools/usb_sim/gen_regs.py Inc/stm32f103_regs.h "$OUT/stm32f103_regs.h"

CXX=(g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-overflow -Wno-volatile -I"$OUT" -Itools/usb_sim -IInc)
"${CXX[@]}" -c tools/usb_sim/sim.cpp -o "$OUT/sim.o"
"${CXX[@]}" -x c++ -c Src/usb.c -o "$OUT/usb.o"
"${CXX[@]}" -x c++ -c Src/usb_cdc.c -o "$OUT/usb_cdc.o"
"${CXX[@]}" tools/usb_sim/test_cdc.cpp "$OUT/sim.o" "$OUT/usb.o" "$OUT/usb_cdc.o" -o "$OUT/test_cdc"
"${CXX[@]}" tools/usb_sim/test_dfu.cpp "$OUT/sim.o" "$OUT/usb.o" -o "$OUT/test_dfu"

"$OUT/test_cdc"
"$OUT/test_dfu"
