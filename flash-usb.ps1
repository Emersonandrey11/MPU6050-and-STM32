# Build the application and flash it over the Blue Pill's own micro-USB port
# (Windows counterpart of flash-usb.sh). The DFU bootloader must have been
# written once with the ST-Link (flash.ps1).
#
#   powershell -ExecutionPolicy Bypass -File .\flash-usb.ps1 -Port COM5
#   powershell -ExecutionPolicy Bypass -File .\flash-usb.ps1 -NoBuild
#
# -Port is the board's USB serial port while the application runs; it is
# opened at 1200 baud to restart the board into the bootloader. Without it,
# put the board in the bootloader yourself: BOOT1 jumper to 1, press RESET.
#
# One-time driver setup: when the board is in the bootloader ("MPU6050 DFU
# bootloader", USB ID 1209:0002), install the WinUSB driver for it with Zadig
# (https://zadig.akeo.ie). dfu-util for Windows: https://dfu-util.sourceforge.net

param([string]$Port = "", [switch]$NoBuild)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

foreach ($root in @("$env:LOCALAPPDATA\stm32cube\bundles",
                    "$env:USERPROFILE\.local\share\stm32cube\bundles",
                    "$env:APPDATA\stm32cube\bundles")) {
    foreach ($tool in @("gnu-tools-for-stm32", "ninja", "cmake")) {
        Get-ChildItem -Path "$root\$tool\*\bin" -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { $env:PATH = "$($_.FullName);$env:PATH" }
    }
}
function Have($name) { [bool](Get-Command $name -ErrorAction SilentlyContinue) }
if (-not (Have "dfu-util")) { throw "dfu-util not found (https://dfu-util.sourceforge.net), add it to PATH." }

if (-not $NoBuild) {
    cmake --preset Debug
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
    cmake --build --preset Debug --target mpu6050
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
}

$elf = "build/Debug/mpu6050.elf"
$dfu = "build/Debug/mpu6050.dfu"
arm-none-eabi-objcopy -O binary $elf $dfu
if (Have "dfu-suffix") { dfu-suffix -v 1209 -p 0002 -a $dfu | Out-Null }

function DfuPresent { (dfu-util -l 2>$null | Select-String "\[1209:0002\]") -ne $null }

if (-not (DfuPresent)) {
    if ($Port -eq "") { throw "Bootloader not found. Pass -Port COMx (the running app), or set BOOT1 = 1 and press RESET." }
    Write-Host "Asking the application on $Port to restart into the bootloader (1200 baud)..."
    $sp = New-Object System.IO.Ports.SerialPort $Port, 1200
    $sp.Open(); Start-Sleep -Milliseconds 100; $sp.Close()
    for ($i = 0; $i -lt 50 -and -not (DfuPresent); $i++) { Start-Sleep -Milliseconds 100 }
    if (-not (DfuPresent)) { throw "The bootloader did not appear on USB (driver installed with Zadig?)." }
}

dfu-util -d 1209:0002 -a 0 -D $dfu -R
if ($LASTEXITCODE -ne 0) { throw "dfu-util failed." }
