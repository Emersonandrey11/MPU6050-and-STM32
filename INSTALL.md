# Installation – Windows

How to build, flash and run the MPU6050 monitor on a Windows 10/11 PC.
A Linux quick reference is at the end.

## What you need

| Item | What it is for | Required? |
|---|---|---|
| Blue Pill (STM32F103C8T6) + MPU6050/MPU6500 board, wired as in `HARDWARE.md` | the hardware | yes |
| ST-Link V2 + USB driver | flashing and reading sensor data | yes |
| Arm GNU toolchain, CMake, Ninja (or VS Code + STM32Cube extension) | compiling the firmware | yes, to build |
| OpenOCD | data path for the monitor, and flashing | yes |
| Python 3.8 or newer | runs `tools/mpu6050_monitor.py` (standard library only, no `pip install`) | yes |
| A web browser | the live dashboard | for `--web` |
| STM32CubeProgrammer | alternative flasher | optional |

> **Work computer:** installing the ST-Link USB driver needs **administrator rights**.
> Everything else can be installed per user. If USB devices or driver installs are
> blocked by company policy, ask IT to install the ST-Link driver
> (or STM32CubeProgrammer, which includes it).

---

## 1. Copy the project

Copy the whole project folder to the Windows PC, for example `C:\dev\mpu6050`.

**Delete the `build` folder** if you copied it from Linux. It contains Linux paths,
and CMake will refuse to configure with it.

Avoid paths with spaces or non-ASCII characters (e.g. not under `OneDrive\Área de Trabalho`).

## 2. ST-Link USB driver

1. Download **STSW-LINK009** (ST-Link USB driver) from st.com (free account required),
   or install STM32CubeProgrammer, which installs the same driver.
2. Run `dpinst_amd64.exe` from the zip (as administrator).
3. Plug in the ST-Link. In Device Manager it should appear under
   *Universal Serial Bus devices* as **STM32 STLink**, without a warning sign.

If the ST-Link is very old, STM32CubeProgrammer can update its firmware
(*Firmware upgrade* button); recent firmware works best with OpenOCD.

## 3. Build tools

Choose **one** of the two options.

### Option A – VS Code + STM32Cube extension (same as the Linux setup)

1. Install [VS Code](https://code.visualstudio.com/) (`winget install Microsoft.VisualStudioCode`).
2. In VS Code, install the extension **STM32Cube for Visual Studio Code** (publisher: STMicroelectronics).
3. *File → Open Folder* → the project folder. The extension downloads its own CMake,
   Ninja and GNU Tools for STM32 (listed in `.settings/bundles.store.json`).
4. Select the **Debug** preset and click **Build** in the status bar.

`flash.ps1` looks for these downloaded tools automatically. If it still cannot find them,
build in VS Code and flash with `.\flash.ps1 -NoBuild`.

### Option B – standalone tools

In PowerShell:

```powershell
winget install Kitware.CMake
winget install Ninja-build.Ninja
```

Then install the **Arm GNU Toolchain** (target `arm-none-eabi`, Windows installer `.exe`) from
<https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads>.
On the last page of the installer tick **"Add path to environment variable"**.

Open a **new** PowerShell window and check:

```powershell
cmake --version
ninja --version
arm-none-eabi-gcc --version
```

## 4. OpenOCD

1. Download the Windows zip `xpack-openocd-<version>-win32-x64.zip` from
   <https://github.com/xpack-dev-tools/openocd-xpack/releases> (0.12 or newer).
2. Unzip it, e.g. to `C:\tools\xpack-openocd`.
3. Add its `bin` folder to your **user** PATH (no admin needed):
   *Start → "Edit environment variables for your account" → Path → New →*
   `C:\tools\xpack-openocd\bin`
4. New PowerShell window, then check:

```powershell
openocd --version
```

Optional: if you install **STM32CubeProgrammer** and add its `bin` folder to PATH,
`flash.ps1` uses `STM32_Programmer_CLI` for flashing. OpenOCD is still needed for the monitor.

## 5. Python

```powershell
winget install Python.Python.3.12
```

(or the installer from python.org with **"Add python.exe to PATH"** ticked).
No extra packages are needed. Check with `py --version` or `python --version`.

## 6. Build and flash

Plug in the ST-Link, open PowerShell in the project folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\flash.ps1
```

`-ExecutionPolicy Bypass` is needed because Windows blocks unsigned scripts by default;
it applies only to this one command.

The script builds the Debug configuration and flashes `build\Debug\mpu6050.elf` with
STM32CubeProgrammer, OpenOCD or st-flash (whichever it finds first).

After flashing, the LED on PC13 shows the status:
**slow blink (1.5 s)** = sensor found, **fast blink** = sensor not answering on I2C.

## 7. Run the monitor

```powershell
py tools\mpu6050_monitor.py --web
```

- Starts OpenOCD by itself, shows the live values in the terminal and opens the dashboard
  at <http://127.0.0.1:8000/>.
- Keep the sensor still for the first 2 seconds (gyro calibration).
- Other modes: `--csv` (one CSV line per sample, e.g. `> data.csv`), `--demo --web`
  (simulated sensor, no hardware), `--probe` (raw OpenOCD diagnostics).
- Stop it with **Ctrl+C** before flashing again; only one program can use the ST-Link.

Use **Windows Terminal** (default on Windows 11) for the best terminal view.
The server listens on `127.0.0.1` only, so the firewall normally does not ask anything;
if it does, *Private networks* is enough.

---

## 8. Flashing over USB (optional, after step 6)

Step 6 also wrote the project's **USB DFU bootloader**. From now on the application can be updated
through the Blue Pill's own micro-USB port, without the ST-Link:

1. Install **dfu-util** for Windows (https://dfu-util.sourceforge.net) and add its folder to `PATH`.
2. Plug the Blue Pill's micro-USB port into the PC. It appears as a USB serial port (`COMx`,
   USB ID 1209:0001); Windows 10/11 loads the driver by itself. Note the COM number in Device Manager.
3. Put the board in the bootloader once (BOOT1 jumper to 1, press RESET; the LED double-blinks).
   It appears as **MPU6050 DFU bootloader** (1209:0002). Open **Zadig** (https://zadig.akeo.ie),
   choose that device and install **WinUSB**. Put BOOT1 back to 0 and press RESET.
4. Flash over USB:

```powershell
powershell -ExecutionPolicy Bypass -File .\flash-usb.ps1 -Port COM5
```

The script asks the running application to restart into the bootloader (it opens the port at 1200 baud)
and writes the new application with `dfu-util`. Live data over USB:

```powershell
python tools\mpu6050_monitor.py --usb COM5 --web
```

If the PC does not detect the board at all, check the D+ pull-up resistor R10 on the Blue Pill:
it must be 1.5 kΩ (many boards have 10 kΩ or 4.7 kΩ). Debugging with breakpoints still uses the ST-Link.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `openocd not found` | Step 4: `bin` folder not in PATH, or PowerShell opened before PATH was changed. |
| `LIBUSB_ERROR_NOT_SUPPORTED` / `open failed` in OpenOCD | ST-Link driver missing (step 2), or another program (STM32CubeProgrammer, CubeIDE, st-util) has the ST-Link open. |
| `Failed to enter SWD mode` / `unable to connect to the target` | Check SWDIO/SWCLK/GND/3.3V wires, BOOT0 = 0. Unplug and replug the ST-Link. Hold RESET while starting, release after ~1 s. |
| CMake error about a different source directory / `CMakeCache.txt` | Delete the `build` folder (step 1). |
| `arm-none-eabi-gcc not found` when building | Step 3; open a new PowerShell window after installing. |
| Monitor: `telemetry block not found` | Firmware not flashed, or an old build: run `flash.ps1` again. `--probe` shows the raw reads. |
| Monitor: source shows `halt-mode reads` | Live memory reads failed, the monitor pauses the CPU ~1 ms per read instead. Data is still correct. |
| Monitor: `I2C errors` keeps increasing | Loose SDA/SCL wire or missing pull-ups on the sensor board. |
| Escape codes like `←[H` in the terminal | Use Windows Terminal or PowerShell 7 instead of the old `cmd.exe` window. |
| USB: board not detected at all | Data cable (not charge-only)? R10 = 1.5 kΩ? 8 MHz crystal present (USB needs it)? |
| USB: `flash-usb.ps1` says bootloader not found | WinUSB driver installed with Zadig (step 8)? Or set BOOT1 = 1, press RESET, run it again. |

---

## Linux quick reference

```bash
sudo apt install openocd stlink-tools python3 dfu-util
./flash.sh                                  # build + flash bootloader + app (uses the STM32Cube VS Code bundles)
python3 tools/mpu6050_monitor.py --web

# Native USB (after ./flash.sh once):
sudo cp tools/udev/50-mpu6050-usb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
./flash-usb.sh                              # build + flash the app over USB
python3 tools/mpu6050_monitor.py --usb --web
```
