#!/usr/bin/env python3
"""Read-only health check of the running board over the ST-Link (SWD).

Use it when USB does not work: power the board from micro-USB, connect the
ST-Link with SWDIO, SWCLK and GND only (no 3.3 V), then run

    python3 tools/swd_diag.py

It halts the CPU for a moment to read PC, reads clock/USB/fault registers and
the start of g_telemetry, resumes the CPU and explains what it found.
Nothing is written to the chip.
"""
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP_ELF = os.path.join(ROOT, "build", "Debug", "mpu6050.elf")
BOOT_ELF = os.path.join(ROOT, "build", "Debug", "bootloader.elf")

READS = {               # name: (address, words)
    "RCC_CR": (0x40021000, 1), "RCC_CFGR": (0x40021004, 1),
    "EP0R": (0x40005C00, 1), "USB_CNTR": (0x40005C40, 1), "USB_ISTR": (0x40005C44, 1),
    "USB_FNR": (0x40005C48, 1), "USB_DADDR": (0x40005C4C, 1), "RCC_APB1ENR": (0x4002101C, 1),
    "NVIC_ISER0": (0xE000E100, 1), "VTOR": (0xE000ED08, 1), "CFSR": (0xE000ED28, 1), "HFSR": (0xE000ED2C, 1),
    "GPIOC_ODR": (0x4001100C, 1), "RCC_CSR": (0x40021024, 1), "EP1R": (0x40005C04, 1), "EP2R": (0x40005C08, 1),
}
# firmware variables (looked up in the ELF): name -> words to read
VARS = {"s_tx_busy": "mdb", "s_config": "mdb", "s_ticks_ms": "mdw", "s_tx_pos": "mdw", "s_tx_len": "mdw"}


def symbol(elf, name):
    nm = shutil.which("arm-none-eabi-nm") or os.path.expanduser(
        "~/.local/share/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-nm")
    try:
        out = subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def func_at(pc):
    a2l = shutil.which("arm-none-eabi-addr2line") or os.path.expanduser(
        "~/.local/share/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-addr2line")
    elf = BOOT_ELF if pc < 0x08002000 else APP_ELF
    try:
        out = subprocess.run([a2l, "-f", "-e", elf, hex(pc)], capture_output=True, text=True).stdout.split()
        return "%s (%s)" % (out[0], os.path.basename(elf)) if out else "?"
    except OSError:
        return "?"


def main():
    tel = symbol(APP_ELF, "g_telemetry")
    cmds = ["init", "halt 200", "reg pc", "reg lr", "reg xPSR", "reg msp"]
    for name, (addr, n) in READS.items():
        cmds.append("echo {%s}; mdw 0x%08x %d" % (name, addr, n))
    if tel:
        cmds.append("echo {TEL}; mdw 0x%08x 6" % tel)
    for v, how in VARS.items():
        a = symbol(APP_ELF, v)
        if a:
            READS[v] = (a, 1)
            cmds.append("echo {%s}; %s 0x%08x 1" % (v, how, a))
    cmds += ["resume", "exit"]
    args = ["openocd", "-f", "interface/stlink.cfg", "-f", "target/stm32f1x.cfg"]
    for c in cmds:
        args += ["-c", c]
    r = subprocess.run(args, capture_output=True, text=True, timeout=30)
    text = r.stdout + r.stderr
    if "open failed" in text or "Error: init mode failed" in text:
        print("Cannot talk to the ST-Link/chip:\n" + "\n".join(l for l in text.splitlines() if "Error" in l))
        print("Check: ST-Link plugged in, SWDIO/SWCLK/GND wired, board powered (micro-USB).")
        return 1

    val = {}
    cur = None
    for line in text.splitlines():
        m = re.match(r"^(\w+)$", line.strip())
        if m and (m.group(1) in READS or m.group(1) == "TEL"):
            cur = m.group(1)
            continue
        m = re.match(r"^0x[0-9a-f]+:\s+((?:[0-9a-f]{2,8}\s*)+)$", line.strip())
        if m and cur:
            val[cur] = [int(w, 16) for w in m.group(1).split()]
            cur = None
        m = re.match(r"^(pc|lr|xPSR|msp) \(/32\): (0x[0-9a-f]+)", line.strip())
        if m:
            val[m.group(1)] = int(m.group(2), 16)

    def g(name, i=0):
        return val.get(name, [None])[i] if name in val else None

    print("Raw: " + ", ".join("%s=0x%08x" % (k, v[0] if isinstance(v, list) else v)
                              for k, v in val.items() if k != "TEL"))
    print()
    pc, xpsr = val.get("pc"), val.get("xPSR")
    if pc is not None:
        exc = xpsr & 0x1FF if xpsr is not None else -1
        where = "thread mode" if exc == 0 else ("HARD FAULT" if exc == 3 else "exception %d" % exc)
        print("CPU      PC=0x%08x in %s, %s" % (pc, func_at(pc), where))
        if val.get("lr") is not None:
            print("         LR=0x%08x in %s, MSP=0x%08x" % (val["lr"], func_at(val["lr"] & ~1), val.get("msp", 0)))
    cfsr, hfsr = g("CFSR"), g("HFSR")
    if cfsr or hfsr:
        print("FAULT    CFSR=0x%08x HFSR=0x%08x  <- the CPU has faulted" % (cfsr or 0, hfsr or 0))
    vtor = g("VTOR")
    if vtor is not None:
        print("VTOR     0x%08x (%s)" % (vtor, {0x08000000: "bootloader", 0x08002000: "application"}.get(vtor, "unexpected")))
    cr, cfgr = g("RCC_CR"), g("RCC_CFGR")
    if cr is not None and cfgr is not None:
        sws = (cfgr >> 2) & 3
        src = "PLL from HSE crystal" if (sws == 2 and cfgr & (1 << 16)) else (
            "PLL from HSI (crystal did NOT start: USB out of tolerance)" if sws == 2 else "HSI 8 MHz (PLL not running)")
        mul = ((cfgr >> 18) & 0xF) + 2
        print("CLOCK    SYSCLK = %s, PLL x%d, USBPRE %s, HSERDY=%d" %
              (src, mul, "/1" if cfgr & (1 << 22) else "/1.5", (cr >> 17) & 1))
    cntr, istr, daddr, fnr, ep0 = g("USB_CNTR"), g("USB_ISTR"), g("USB_DADDR"), g("USB_FNR"), g("EP0R")
    apb1 = g("RCC_APB1ENR")
    if apb1 is not None and not (apb1 >> 23) & 1:
        print("USB      clock NOT enabled: usb_init() never ran")
    elif cntr is not None:
        print("USB      CNTR=0x%04x (%s), DADDR=0x%02x (%s), ISTR=0x%04x, FNR=0x%04x (%s), EP0R=0x%04x" % (
            cntr, "running, CTR+RESET interrupts" if cntr == 0x8400 else ("powered down / in reset" if cntr & 3 else "other"),
            daddr, ("host reset seen, address %d" % (daddr & 0x7F)) if daddr & 0x80 else "no host reset handled yet",
            istr, fnr, "SOFs locked" if fnr & (1 << 13) else "no SOF lock", ep0))
    iser = g("NVIC_ISER0")
    if iser is not None:
        print("NVIC     USB_LP interrupt %s" % ("enabled" if iser & (1 << 20) else "DISABLED"))
    csr = g("RCC_CSR")
    if csr is not None:
        names = [(31, "low-power"), (30, "window watchdog"), (29, "INDEPENDENT WATCHDOG"), (28, "software"),
                 (27, "power-on/brown-out"), (26, "NRST pin")]
        print("RESETS   RCC_CSR=0x%08x since flags were last cleared: %s" % (
            csr, ", ".join(n for b, n in names if csr & (1 << b)) or "none"))
    for v in VARS:
        if v in val:
            print("VAR      %-10s = %d" % (v, val[v][0]))
    if "EP1R" in val:
        e = val["EP1R"][0]
        print("EP1 IN   EP1R=0x%04x STAT_TX=%s" % (e, ["DISABLED", "STALL", "NAK", "VALID"][(e >> 4) & 3]))
    tel = val.get("TEL")
    if tel:
        print("APP      g_telemetry magic %s, sample_count=%d" % (
            "OK" if tel[0] == 0x3655504D else "0x%08x (not set: main() not reached)" % tel[0], tel[4]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
