# Contributing

This repository is a learning resource, so clear explanations count as much as code.

## Good contributions

- **Improve the guide** (`docs/index.html`): fix a mistake, explain something more simply, or add a diagram.
- **Exercises**: add a solution to one of the ideas in the README in a separate folder or branch, and explain it.
- **Ports**: run `mpu6050.c` on another MCU by writing a new `i2c.c` for it.
- **Bug reports**: describe your board (MPU6050 or MPU6500 family, `WHO_AM_I` value), your wiring, and the output of
  `python3 tools/mpu6050_monitor.py --probe`.

## Code style

- C11, no HAL, every register access through the names in `Inc/stm32f103_regs.h`.
- Every wait loop has a timeout.
- Functions return `0` on success and `-1` on error.
- Comment *why* something is done, and give the reference manual section or application note when you can.
- If you change `telemetry_t`, update `TELEMETRY_FORMAT` in `tools/mpu6050_monitor.py` and the `_Static_assert`s.

## Before opening a pull request

- `./flash.sh` builds without warnings.
- `python3 tools/mpu6050_monitor.py --demo --web` still works.
- If you have the hardware: the monitor shows live data and the self-test passes.
