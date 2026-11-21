# Build the Debug configuration and flash it to the STM32F103C8T6 via ST-Link V2.
# Windows counterpart of flash.sh.
#
#   powershell -ExecutionPolicy Bypass -File .\flash.ps1            # build + flash
#   powershell -ExecutionPolicy Bypass -File .\flash.ps1 -NoBuild   # flash only (built in VS Code)

param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# Tools installed by the STM32Cube VS Code extension, if present.
foreach ($root in @("$env:LOCALAPPDATA\stm32cube\bundles",
                    "$env:USERPROFILE\.local\share\stm32cube\bundles",
                    "$env:APPDATA\stm32cube\bundles")) {
    foreach ($tool in @("gnu-tools-for-stm32", "ninja", "cmake")) {
        Get-ChildItem -Path "$root\$tool\*\bin" -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { $env:PATH = "$($_.FullName);$env:PATH" }
    }
}

function Have($name) { [bool](Get-Command $name -ErrorAction SilentlyContinue) }

if (-not $NoBuild) {
    foreach ($t in @("cmake", "ninja", "arm-none-eabi-gcc")) {
        if (-not (Have $t)) { throw "$t not found. See INSTALL.md, step 3." }
    }
    cmake --preset Debug
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (if you copied the project from Linux, delete the build folder)." }
    cmake --build --preset Debug
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
}

$elf = "build/Debug/mpu6050.elf"
if (-not (Test-Path $elf)) { throw "$elf not found. Build first." }

if (Get-Process openocd -ErrorAction SilentlyContinue) {
    throw "openocd is running and holds the ST-Link: stop mpu6050_monitor.py / openocd first."
}

if (Have "STM32_Programmer_CLI") {
    STM32_Programmer_CLI -c port=SWD -w $elf -v -rst
} elseif (Have "openocd") {
    openocd -f interface/stlink.cfg -c "set CPUTAPID 0" -f target/stm32f1x.cfg -c "program $elf verify reset exit"
} elseif (Have "st-flash") {
    arm-none-eabi-objcopy -O binary $elf build/Debug/mpu6050.bin
    st-flash --reset write build/Debug/mpu6050.bin 0x08000000
} else {
    throw "No flasher found. Install OpenOCD (INSTALL.md, step 4)."
}

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Flashing failed. Things to try:" -ForegroundColor Yellow
    Write-Host "  1. Check the 4 ST-Link wires (SWDIO, SWCLK, GND, 3.3V) and that BOOT0 is on 0."
    Write-Host "  2. Unplug the ST-Link, plug it back in, try again."
    Write-Host "  3. Hold the board's RESET button, start the script, release RESET about 1 s later."
    exit 1
}
