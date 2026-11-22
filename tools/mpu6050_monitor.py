#!/usr/bin/env python3
"""
MPU6050 live monitor for the STM32F103 firmware in this project.

How the data gets to the PC (no UART adapter needed). Two ways:

  ST-Link (default):
    MPU6050 --I2C--> STM32 RAM (g_telemetry) --SWD--> ST-Link --USB--> OpenOCD
            --TCL socket :6666--> this script --> terminal and/or web dashboard
    The firmware keeps the latest sample in a global struct. OpenOCD reads that
    RAM through the debug port while the MCU keeps running.

  Native USB (--usb): the Blue Pill's own micro-USB port is a USB serial
  device (1209:0001, /dev/ttyACMx or COMx). The firmware sends the same struct
  after every sample as [248 bytes][CRC-16/XMODEM]; commands go back as
  8-byte frames 'M' 'C' seq(u32) id arg.

Usage:
    python3 tools/mpu6050_monitor.py             # live view in the terminal (ST-Link)
    python3 tools/mpu6050_monitor.py --usb --web # native USB + real-time web dashboard
    python3 tools/mpu6050_monitor.py --web       # + real-time web dashboard
    python3 tools/mpu6050_monitor.py --csv       # stream CSV lines
    python3 tools/mpu6050_monitor.py --demo --web  # simulated sensor, no hardware

Only the Python standard library is used. For the ST-Link path OpenOCD must be
installed (sudo apt install openocd); the script starts it if it is not running.
"""

import argparse
import binascii
import collections
import glob
import http.server
import json
import math
import os
import queue
import random
import shutil
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
DEFAULT_ELF = os.path.join(PROJECT, "build", "Debug", "mpu6050.elf")
DASHBOARD_HTML = os.path.join(HERE, "dashboard.html")

# --------------------------------------------------------------------------
# Telemetry layout -- must match Inc/telemetry.h
# --------------------------------------------------------------------------
TELEMETRY_SYMBOL = "g_telemetry"
TELEMETRY_MAGIC = 0x3655504D
TELEMETRY_VERSION = 1
TELEMETRY_FORMAT = "<IHHIIII3hh3hBB3IBBBBBB4sBB3i3iIIII128sIBBHII"
TELEMETRY_FIELDS = (
    "magic version size seq_head uptime_ms sample_count sample_time_ms "
    "ax ay az temp gx gy gz i2c_addr who_am_i gix giy giz "
    "status accel_fs gyro_fs dlpf smplrt_div int_status st_trim ad0_level int_pin_level "
    "sax say saz sgx sgy sgz motion_count int_pin_count i2c_errors selftest_count "
    "regs cmd_seq cmd_id cmd_arg cmd_reserved cmd_ack seq_tail"
).split()
TELEMETRY_SIZE = struct.calcsize(TELEMETRY_FORMAT)
OFFSET_CMD_SEQ = 232
OFFSET_CMD_WORD = 236
assert TELEMETRY_SIZE == 248 and len(TELEMETRY_FIELDS) == 45

Telemetry = collections.namedtuple("Telemetry", TELEMETRY_FIELDS)

STATUS_MPU_OK = 0x01
STATUS_SELFTEST_OK = 0x02
STATUS_VIA_INT_PIN = 0x04

CMD_IDS = {
    "accel_fs": 1,
    "gyro_fs": 2,
    "dlpf": 3,
    "rate_div": 4,
    "self_test": 5,
    "reinit": 6,
    "ad0": 7,
}

USB_VID, USB_PID_APP = 0x1209, 0x0001   # Inc/usb_ids.h (pid.codes test PID)
USB_FRAME_LEN = TELEMETRY_SIZE + 2       # struct + CRC-16/XMODEM
RAM_START = 0x20000000
RAM_SIZE = 20 * 1024

# --------------------------------------------------------------------------
# MPU6050 facts (register map rev 4.2)
# --------------------------------------------------------------------------
ACCEL_RANGE_G = [2, 4, 8, 16]
ACCEL_LSB_PER_G = [16384.0, 8192.0, 4096.0, 2048.0]
GYRO_RANGE_DPS = [250, 500, 1000, 2000]
GYRO_LSB_PER_DPS = [131.0, 65.5, 32.8, 16.4]
# DLPF_CFG -> (accel bandwidth Hz, gyro bandwidth Hz, gyro output rate Hz)
DLPF_TABLE = {
    0: (260, 256, 8000),
    1: (184, 188, 1000),
    2: (94, 98, 1000),
    3: (44, 42, 1000),
    4: (21, 20, 1000),
    5: (10, 10, 1000),
    6: (5, 5, 1000),
}
G = 9.80665

REGISTER_NAMES = {
    0x0D: "SELF_TEST_X", 0x0E: "SELF_TEST_Y", 0x0F: "SELF_TEST_Z", 0x10: "SELF_TEST_A",
    0x19: "SMPLRT_DIV", 0x1A: "CONFIG", 0x1B: "GYRO_CONFIG", 0x1C: "ACCEL_CONFIG",
    0x1F: "MOT_THR", 0x20: "MOT_DUR", 0x23: "FIFO_EN", 0x24: "I2C_MST_CTRL",
    0x25: "I2C_SLV0_ADDR", 0x26: "I2C_SLV0_REG", 0x27: "I2C_SLV0_CTRL",
    0x28: "I2C_SLV1_ADDR", 0x29: "I2C_SLV1_REG", 0x2A: "I2C_SLV1_CTRL",
    0x2B: "I2C_SLV2_ADDR", 0x2C: "I2C_SLV2_REG", 0x2D: "I2C_SLV2_CTRL",
    0x2E: "I2C_SLV3_ADDR", 0x2F: "I2C_SLV3_REG", 0x30: "I2C_SLV3_CTRL",
    0x31: "I2C_SLV4_ADDR", 0x32: "I2C_SLV4_REG", 0x33: "I2C_SLV4_DO",
    0x34: "I2C_SLV4_CTRL", 0x35: "I2C_SLV4_DI", 0x36: "I2C_MST_STATUS",
    0x37: "INT_PIN_CFG", 0x38: "INT_ENABLE", 0x3A: "INT_STATUS",
    0x3B: "ACCEL_XOUT_H", 0x3C: "ACCEL_XOUT_L", 0x3D: "ACCEL_YOUT_H", 0x3E: "ACCEL_YOUT_L",
    0x3F: "ACCEL_ZOUT_H", 0x40: "ACCEL_ZOUT_L", 0x41: "TEMP_OUT_H", 0x42: "TEMP_OUT_L",
    0x43: "GYRO_XOUT_H", 0x44: "GYRO_XOUT_L", 0x45: "GYRO_YOUT_H", 0x46: "GYRO_YOUT_L",
    0x47: "GYRO_ZOUT_H", 0x48: "GYRO_ZOUT_L",
    **{0x49 + i: f"EXT_SENS_DATA_{i:02d}" for i in range(24)},
    0x63: "I2C_SLV0_DO", 0x64: "I2C_SLV1_DO", 0x65: "I2C_SLV2_DO", 0x66: "I2C_SLV3_DO",
    0x67: "I2C_MST_DELAY_CTRL", 0x68: "SIGNAL_PATH_RESET", 0x69: "MOT_DETECT_CTRL",
    0x6A: "USER_CTRL", 0x6B: "PWR_MGMT_1", 0x6C: "PWR_MGMT_2",
    0x72: "FIFO_COUNTH", 0x73: "FIFO_COUNTL", 0x75: "WHO_AM_I",
}


# Many boards sold as "MPU6050 / GY-521" carry an MPU6500-family chip. It is
# register compatible for the data, but temperature scale, self-test, the
# accelerometer filter and motion detection differ.
CHIPS = {0x68: "MPU-6050", 0x70: "MPU-6500", 0x71: "MPU-9250", 0x73: "MPU-9255", 0x74: "MPU-6515"}
MPU6500_FAMILY = {0x70, 0x71, 0x73, 0x74}
ACCEL_DLPF_6500 = {0: 218.1, 1: 218.1, 2: 99.0, 3: 44.8, 4: 21.2, 5: 10.2, 6: 5.05, 7: 420.0}
REGISTER_NAMES_6500 = {a: n for a, n in REGISTER_NAMES.items() if a not in (0x10, 0x20)}
REGISTER_NAMES_6500.update({
    0x00: "SELF_TEST_X_GYRO", 0x01: "SELF_TEST_Y_GYRO", 0x02: "SELF_TEST_Z_GYRO",
    0x0D: "SELF_TEST_X_ACCEL", 0x0E: "SELF_TEST_Y_ACCEL", 0x0F: "SELF_TEST_Z_ACCEL",
    0x13: "XG_OFFSET_H", 0x14: "XG_OFFSET_L", 0x15: "YG_OFFSET_H", 0x16: "YG_OFFSET_L",
    0x17: "ZG_OFFSET_H", 0x18: "ZG_OFFSET_L",
    0x1D: "ACCEL_CONFIG2", 0x1E: "LP_ACCEL_ODR", 0x1F: "WOM_THR", 0x69: "ACCEL_INTEL_CTRL",
})


def chip_name(who):
    return CHIPS.get(who, "unknown chip")


def decode_register(addr, v, is6500=False):
    """Human readable meaning of the most important registers."""
    if is6500:
        if addr == 0x1C:
            st = "".join(c for c, b in zip("XYZ", (7, 6, 5)) if v >> b & 1)
            return "ACCEL_FS_SEL=%d (±%d g)%s" % (v >> 3 & 3, ACCEL_RANGE_G[v >> 3 & 3], ", self-test " + st if st else "")
        if addr == 0x1D:
            if v & 0x08:
                return "accel filter bypassed (1046 Hz)"
            return "A_DLPF_CFG=%d (accel %.1f Hz)" % (v & 7, ACCEL_DLPF_6500[v & 7])
        if addr == 0x1F:
            return "wake-on-motion threshold %d mg" % (v * 4)
        if addr == 0x69:
            return "wake-on-motion %s" % ("ON" if v & 0x80 else "off")
    if addr == 0x19:
        return "sample rate = gyro rate / (1 + %d)" % v
    if addr == 0x1A:
        a, g, _ = DLPF_TABLE.get(v & 7, (0, 0, 0))
        return "DLPF_CFG=%d (accel %d Hz, gyro %d Hz), EXT_SYNC=%d" % (v & 7, a, g, (v >> 3) & 7)
    if addr == 0x1B:
        st = "".join(c for c, b in zip("XYZ", (7, 6, 5)) if v >> b & 1)
        return "FS_SEL=%d (±%d °/s)%s" % (v >> 3 & 3, GYRO_RANGE_DPS[v >> 3 & 3], ", self-test " + st if st else "")
    if addr == 0x1C:
        st = "".join(c for c, b in zip("XYZ", (7, 6, 5)) if v >> b & 1)
        return "AFS_SEL=%d (±%d g), HPF=%d%s" % (v >> 3 & 3, ACCEL_RANGE_G[v >> 3 & 3], v & 7,
                                                 ", self-test " + st if st else "")
    if addr == 0x1F:
        return "motion threshold %d mg" % (v * 2)
    if addr == 0x37:
        parts = ["active " + ("low" if v & 0x80 else "high"),
                 "open-drain" if v & 0x40 else "push-pull",
                 "latched" if v & 0x20 else "50 µs pulse"]
        if v & 0x02:
            parts.append("I2C bypass ON")
        return ", ".join(parts)
    if addr in (0x38, 0x3A):
        names = [n for n, b in (("DATA_RDY", 0x01), ("I2C_MST", 0x08), ("FIFO_OFLOW", 0x10), ("MOTION", 0x40)) if v & b]
        return " | ".join(names) if names else "none"
    if addr == 0x6A:
        names = [n for n, b in (("FIFO_EN", 0x40), ("I2C_MST_EN", 0x20), ("I2C_IF_DIS", 0x10)) if v & b]
        return " | ".join(names) if names else "FIFO off, aux I2C master off"
    if addr == 0x6B:
        clk = {0: "internal 8 MHz", 1: "PLL gyro X", 2: "PLL gyro Y", 3: "PLL gyro Z",
               4: "PLL ext 32 kHz", 5: "PLL ext 19.2 MHz", 7: "stopped"}.get(v & 7, "reserved")
        flags = [n for n, b in (("RESET", 0x80), ("SLEEP", 0x40), ("CYCLE", 0x20), ("TEMP_DIS", 0x08)) if v & b]
        return "clock: %s%s" % (clk, (", " + " | ".join(flags)) if flags else "")
    if addr == 0x6C:
        off = [n for n, b in (("XA", 0x20), ("YA", 0x10), ("ZA", 0x08), ("XG", 0x04), ("YG", 0x02), ("ZG", 0x01)) if v & b]
        return "standby: " + (", ".join(off) if off else "none (all axes on)")
    if addr == 0x75:
        return chip_name(v)
    return ""


# --------------------------------------------------------------------------
# ELF symbol lookup (to find g_telemetry in RAM)
# --------------------------------------------------------------------------
def elf_symbol(path, name):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("%s is not a 32-bit little-endian ELF" % path)
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<10I", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    wanted = name.encode()
    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        sym_off, sym_size, link = sh[4], sh[5], sh[6]
        str_off = sections[link][4]
        for j in range(sym_size // 16):
            st_name, st_value, st_size = struct.unpack_from("<III", data, sym_off + j * 16)
            end = data.index(b"\0", str_off + st_name)
            if data[str_off + st_name:end] == wanted:
                return st_value, st_size
    return None


def elf_function_at(path, addr):
    """Name of the function containing a code address (for crash reports)."""
    with open(path, "rb") as f:
        data = f.read()
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<10I", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    addr &= ~1
    names = []
    for sh in sections:
        if sh[1] != 2:
            continue
        str_off = sections[sh[6]][4]
        for j in range(sh[5] // 16):
            st_name, st_value, st_size, st_info = struct.unpack_from("<IIIB", data, sh[4] + j * 16)
            if st_info & 0xF == 2 and (st_value & ~1) <= addr < (st_value & ~1) + max(st_size, 2):
                names.append(data[str_off + st_name:data.index(b"\0", str_off + st_name)].decode())
    # all unused interrupt vectors are aliases of Default_Handler
    if "Default_Handler" in names or len(names) > 3:
        return "Default_Handler (unexpected interrupt or fault)"
    return names[0] if names else None


# --------------------------------------------------------------------------
# OpenOCD TCL-RPC client
# --------------------------------------------------------------------------
class OpenOCDError(Exception):
    pass


class OpenOCD:
    def __init__(self, host="127.0.0.1", port=6666, timeout=3.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""
        self.use_read_memory = True

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def cmd(self, command):
        self.sock.sendall(command.encode() + b"\x1a")
        while b"\x1a" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise OpenOCDError("OpenOCD closed the connection")
            self.buf += chunk
        resp, _, self.buf = self.buf.partition(b"\x1a")
        return resp.decode(errors="replace").strip()

    def read_words(self, addr, count):
        if self.use_read_memory:
            resp = self.cmd("read_memory 0x%08x 32 %d" % (addr, count))
            if "invalid command" in resp:
                self.use_read_memory = False  # OpenOCD < 0.12
            else:
                try:
                    words = [int(x, 0) for x in resp.split()]
                except ValueError:
                    raise OpenOCDError(resp.splitlines()[0] if resp else "read failed")
                if len(words) != count:
                    raise OpenOCDError("short read: " + resp[:80])
                return words
        resp = self.cmd("mdw 0x%08x %d" % (addr, count))
        words = []
        for line in resp.splitlines():
            if ":" not in line:
                continue
            try:
                words += [int(x, 16) for x in line.split(":", 1)[1].split()]
            except ValueError:
                raise OpenOCDError(line)
        if len(words) != count:
            raise OpenOCDError("mdw failed: " + resp[:80])
        return words

    def write_word(self, addr, value):
        resp = self.cmd("mww 0x%08x 0x%08x" % (addr, value & 0xFFFFFFFF))
        if resp:
            raise OpenOCDError(resp)


def launch_openocd(port, khz=None):
    exe = shutil.which("openocd")
    if not exe:
        raise OpenOCDError("openocd not found. Install it with: sudo apt install openocd")
    args = [exe,
            "-f", "interface/stlink.cfg",
            "-c", "set CPUTAPID 0",          # also accept clone chips (CKS32, GD32...)
            "-f", "target/stm32f1x.cfg",
            *(["-c", "adapter speed %d" % khz] if khz else []),
            "-c", "gdb_port disabled",
            "-c", "telnet_port disabled",
            "-c", "tcl_port %d" % port]
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = collections.deque(maxlen=40)

    def drain():
        for line in proc.stdout:
            log.append(line.decode(errors="replace").rstrip())
    threading.Thread(target=drain, daemon=True).start()

    deadline = time.time() + 8
    while time.time() < deadline:
        if proc.poll() is not None:
            time.sleep(0.2)
            raise OpenOCDError("openocd exited:\n  " + "\n  ".join(log))
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.3).close()
            return proc, log
        except OSError:
            time.sleep(0.2)
    proc.terminate()
    raise OpenOCDError("openocd did not open port %d:\n  %s" % (port, "\n  ".join(log)))


# --------------------------------------------------------------------------
# Data sources
# --------------------------------------------------------------------------
class SwdDevice:
    """Reads g_telemetry from the running MCU through OpenOCD.

    Normally RAM is read while the CPU runs. If those reads keep returning
    garbage (seen when the CPU sleeps in WFI), it switches to "halt mode":
    each read stops the CPU for about a millisecond, reads, and resumes it.
    """

    LIVE_RETRIES = 10
    BAD_POLLS_BEFORE_HALT_MODE = 20

    def __init__(self, args):
        self.args = args
        self.ocd = None
        self.proc = None
        self.addr = None
        self.log = None
        self.halt_reads = bool(getattr(args, "halt_reads", False))
        self.bad_polls = 0

    @property
    def name(self):
        return "ST-Link (OpenOCD)" + (", halt-mode reads" if self.halt_reads else "")

    def read_block(self, addr, count):
        if not self.halt_reads:
            return self.read_words(addr, count)
        resp = self.ocd.cmd("catch {halt 100}; set r [read_memory 0x%08x 32 %d]; catch {resume}; set r" % (addr, count))
        try:
            words = [int(x, 0) for x in resp.split()]
        except ValueError:
            raise OpenOCDError(resp.splitlines()[0] if resp else "halted read failed")
        if len(words) != count:
            raise OpenOCDError("halted read failed: " + resp[:80])
        return words

    def log_tail(self):
        """Last OpenOCD errors/warnings, to explain a failure."""
        if not self.log:
            return ""
        lines = [l for l in list(self.log) if "Error" in l or "Warn" in l or "Info : " in l][-8:]
        return ("\nOpenOCD log:\n  " + "\n  ".join(lines)) if lines else ""

    def read_words(self, addr, count):
        """Read memory; if it fails, ask OpenOCD to re-examine the target once
        (it gives up after a failed SWD connect at startup) and retry."""
        try:
            return self.ocd.read_words(addr, count)
        except OpenOCDError:
            self.ocd.cmd("catch {[target current] arp_examine}")
            try:
                return self.ocd.read_words(addr, count)
            except OpenOCDError as e:
                state = self.ocd.cmd("catch {[target current] curstate} s; set s")
                raise OpenOCDError("%s (target state: %s)%s" % (e, state or "?", self.log_tail()))

    def connect(self):
        self.close_socket()
        try:
            self.ocd = OpenOCD(self.args.openocd_host, self.args.openocd_port)
        except OSError:
            if self.args.no_launch:
                raise OpenOCDError("cannot connect to OpenOCD at %s:%d" % (self.args.openocd_host, self.args.openocd_port))
            if self.proc is None or self.proc.poll() is not None:
                self.proc, self.log = launch_openocd(self.args.openocd_port, self.args.adapter_khz)
            self.ocd = OpenOCD(self.args.openocd_host, self.args.openocd_port)
        self.addr = self.locate()

    def locate(self):
        candidates = []
        if os.path.exists(self.args.elf):
            sym = elf_symbol(self.args.elf, TELEMETRY_SYMBOL)
            if sym:
                candidates.append(sym[0])
        for addr in candidates:
            for _ in range(self.LIVE_RETRIES):
                if self.read_block(addr, 1)[0] == TELEMETRY_MAGIC:
                    return addr
            if not self.halt_reads:
                self.halt_reads = True          # live reads look broken: try with the CPU halted
                if self.read_block(addr, 1)[0] == TELEMETRY_MAGIC:
                    return addr
                self.halt_reads = bool(getattr(self.args, "halt_reads", False))
        # Fallback: scan RAM for the magic word
        words = self.read_block(RAM_START, RAM_SIZE // 4)
        for i, w in enumerate(words[:-1]):
            if w == TELEMETRY_MAGIC and words[i + 1] == (TELEMETRY_SIZE << 16 | TELEMETRY_VERSION):
                return RAM_START + 4 * i
        diag = self.diagnose()
        raise OpenOCDError("telemetry block not found in RAM - is the MPU6050 firmware flashed and running?"
                           + diag + self.log_tail())

    def diagnose(self):
        """Briefly halt the CPU to see where it is, then let it run again."""
        try:
            state = self.ocd.cmd("catch {[target current] curstate} s; set s")
            self.ocd.cmd("catch {halt 500}")
            icsr = self.read_words(0xE000ED04, 1)[0]
            regs = {}
            for r in ("pc", "xPSR", "msp"):
                out = self.ocd.cmd("catch {reg %s} v; set v" % r)
                regs[r] = out.split(":")[-1].strip() if ":" in out else out
            cfsr, hfsr = self.read_words(0xE000ED28, 2)
            self.ocd.cmd("catch {resume}")
            func = None
            try:
                func = elf_function_at(self.args.elf, int(regs["pc"], 0))
            except (OSError, ValueError):
                pass
            exc = int(regs["xPSR"], 0) & 0x1FF if regs["xPSR"].startswith("0x") else -1
            return ("\nCPU was %s. PC=%s (%s) xPSR=%s exception=%d MSP=%s CFSR=0x%08x HFSR=0x%08x ICSR=0x%08x"
                    "\n(the CPU has been resumed; the monitor retries every second)"
                    % (state, regs["pc"], func or "?", regs["xPSR"], exc, regs["msp"], cfsr, hfsr, icsr))
        except (OpenOCDError, OSError, ValueError, IndexError) as e:
            return "\n(could not read CPU state: %s)" % e

    def read(self):
        bad_magic = 0
        for _ in range(self.LIVE_RETRIES):
            words = self.read_block(self.addr, TELEMETRY_SIZE // 4)
            t = Telemetry(*struct.unpack(TELEMETRY_FORMAT, struct.pack("<%dI" % len(words), *words)))
            if t.magic != TELEMETRY_MAGIC:
                bad_magic += 1
                continue
            if t.seq_head == t.seq_tail:
                self.bad_polls = 0
                return t
        # nothing consistent this time
        self.bad_polls += 1
        if self.bad_polls >= self.BAD_POLLS_BEFORE_HALT_MODE:
            if not self.halt_reads:
                self.halt_reads = True
                self.bad_polls = 0
            elif bad_magic == self.LIVE_RETRIES:
                raise OpenOCDError("telemetry magic lost (MCU reset or reflashed)")
        return None

    def command(self, cmd_id, arg, current_seq):
        if self.halt_reads:
            self.ocd.cmd("catch {halt 100}")
        try:
            self.ocd.write_word(self.addr + OFFSET_CMD_WORD, cmd_id | (arg & 0xFF) << 8)
            self.ocd.write_word(self.addr + OFFSET_CMD_SEQ, (current_seq + 1) & 0xFFFFFFFF)
        finally:
            if self.halt_reads:
                self.ocd.cmd("catch {resume}")

    def close_socket(self):
        if self.ocd:
            self.ocd.close()
            self.ocd = None

    def close(self):
        self.close_socket()
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def find_usb_port():
    """Serial port of the board's native USB (1209:0001), or None."""
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):          # Linux
        usb_dev = os.path.realpath(os.path.join(tty, "device", ".."))
        try:
            with open(os.path.join(usb_dev, "idVendor")) as f:
                vid = int(f.read(), 16)
            with open(os.path.join(usb_dev, "idProduct")) as f:
                pid = int(f.read(), 16)
        except (OSError, ValueError):
            continue
        if (vid, pid) == (USB_VID, USB_PID_APP):
            return "/dev/" + os.path.basename(tty)
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))                      # macOS
    return ports[0] if ports else None


class SerialPort:
    """Minimal serial port with the standard library only. On USB serial the
    baud rate is meaningless, so the port is just put in raw mode."""

    def __init__(self, path):
        self.fd = None
        self.f = None
        if os.name == "nt":
            self.f = open(r"\\.\%s" % path, "r+b", buffering=0)        # e.g. COM5
        else:
            import termios
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
            # Raw mode and 115200 in ONE tcsetattr(): tty.setraw() would first
            # re-send the port's previous speed, and if that is 1200 (left by
            # flash-usb.sh) the firmware takes it as "restart into the bootloader".
            attrs = termios.tcgetattr(self.fd)
            attrs[0] = 0                                    # iflag: no input processing
            attrs[1] = 0                                    # oflag: no output processing
            attrs[2] = (attrs[2] & ~(termios.CSIZE | termios.PARENB)) | termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[3] = 0                                    # lflag: no echo, no line editing
            attrs[4] = attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = 0                      # read() returns after 0.1 s
            attrs[6][termios.VTIME] = 1                     # even without data
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def read(self, n):
        return self.f.read(n) if self.f else os.read(self.fd, n)

    def write(self, data):
        if self.f:
            self.f.write(data)
        else:
            os.write(self.fd, data)

    def close(self):
        if self.f:
            self.f.close()
        elif self.fd is not None:
            os.close(self.fd)
        self.f = self.fd = None


class UsbDevice:
    """Telemetry frames from the firmware over the native USB port."""

    def __init__(self, args):
        self.port_arg = args.usb
        self.path = None
        self.port = None
        self.thread = None
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.latest = None
        self.error = None
        self.frames = 0
        self.bad_crc = 0

    @property
    def name(self):
        return "native USB (%s)" % (self.path or "searching")

    def connect(self):
        self.close()
        self.path = self.port_arg if self.port_arg != "auto" else find_usb_port()
        if not self.path:
            raise OSError("no MPU6050 USB serial port found (USB ID %04x:%04x). Is the board plugged into its "
                          "micro-USB port and the USB firmware flashed? On Windows pass the port: --usb COM5"
                          % (USB_VID, USB_PID_APP))
        self.port = SerialPort(self.path)
        self.stop.clear()
        self.latest = self.error = None
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()
        deadline = time.time() + 2.0
        while time.time() < deadline and self.latest is None and self.error is None:
            time.sleep(0.02)
        if self.latest is None:
            raise OSError("%s is open but no telemetry frames arrive (%s)" % (self.path, self.error or "timeout"))

    def _reader(self):
        sync = struct.pack("<I", TELEMETRY_MAGIC)
        buf = bytearray()
        while not self.stop.is_set():
            try:
                chunk = self.port.read(512)
            except (OSError, AttributeError, ValueError, TypeError) as e:
                self.error = e
                return
            if not chunk:
                continue
            buf += chunk
            while True:
                i = buf.find(sync)
                if i < 0:
                    del buf[:-3]
                    break
                del buf[:i]
                if len(buf) < USB_FRAME_LEN:
                    break
                body = bytes(buf[:TELEMETRY_SIZE])
                crc = struct.unpack_from("<H", buf, TELEMETRY_SIZE)[0]
                if binascii.crc_hqx(body, 0) == crc:
                    t = Telemetry(*struct.unpack(TELEMETRY_FORMAT, body))
                    with self.lock:
                        self.latest = t
                        self.frames += 1
                    del buf[:USB_FRAME_LEN]
                else:
                    self.bad_crc += 1           # the magic bytes appeared inside data: resync
                    del buf[:1]

    def read(self):
        if self.error is not None:
            raise OSError("USB serial %s: %s" % (self.path, self.error))
        with self.lock:
            t, self.latest = self.latest, None
        return t

    def command(self, cmd_id, arg, current_seq):
        self.port.write(b"MC" + struct.pack("<IBB", (current_seq + 1) & 0xFFFFFFFF, cmd_id, arg & 0xFF))

    def close(self):
        self.stop.set()
        if self.port:
            self.port.close()
            self.port = None
        if self.thread:
            self.thread.join(timeout=1)
            self.thread = None


class DemoDevice:
    """Simulated MPU6050 + firmware, for trying the tools without hardware."""

    name = "demo (simulated)"

    def __init__(self, args):
        self.t0 = time.time()
        self.cfg = {"accel_fs": 0, "gyro_fs": 0, "dlpf": 3, "smplrt_div": 9, "ad0": 0}
        self.gint = [0, 0, 0]
        self.count = 0
        self.last_ms = 0
        self.motion = 0
        self.cmd_seq = 0
        self.cmd_ack = 0
        self.st_count = 1
        self.bias = [random.uniform(-2, 2) for _ in range(3)]  # deg/s

    def connect(self):
        pass

    def close(self):
        pass

    def orientation(self, t):
        k = min(1.0, max(0.0, (t - 2.5) / 2.0))  # lie still first: gyro calibration
        roll = k * (35 * math.sin(t * 0.7) + 10 * math.sin(t * 2.3))
        pitch = k * 25 * math.sin(t * 0.5 + 1.0)
        yaw = k * 60 * math.sin(t * 0.21)
        return roll, pitch, yaw

    def read(self):
        now = time.time() - self.t0
        ms = int(now * 1000)
        rate = DLPF_TABLE[self.cfg["dlpf"]][2] / (1 + self.cfg["smplrt_div"])
        new = max(1, int((ms - self.last_ms) * rate / 1000)) if self.count else 1
        dt_ms = ms - self.last_ms if self.count else 0
        self.count += new
        self.last_ms = ms

        r, p, y = self.orientation(now)
        h = 0.01
        r2, p2, y2 = self.orientation(now + h)
        rates = [(r2 - r) / h, (p2 - p) / h, (y2 - y) / h]
        rr, pr = math.radians(r), math.radians(p)
        a = [-math.sin(pr), math.sin(rr) * math.cos(pr), math.cos(rr) * math.cos(pr)]
        shake = 0.02 * math.sin(now * 40)
        a = [v + random.gauss(0, 0.004) + shake for v in a]
        if abs(shake) > 0.0199:
            self.motion += 1
        a_lsb = ACCEL_LSB_PER_G[self.cfg["accel_fs"]]
        g_lsb = GYRO_LSB_PER_DPS[self.cfg["gyro_fs"]]
        lim = lambda v: max(-32768, min(32767, int(round(v))))
        acc = [lim(v * a_lsb) for v in a]
        gyr = [lim((rates[i] + self.bias[i] + random.gauss(0, 0.05)) * g_lsb) for i in range(3)]
        for i in range(3):
            self.gint[i] = (self.gint[i] + gyr[i] * dt_ms) & 0xFFFFFFFF
        temp_c = 27.5 + 1.5 * math.sin(now / 60)
        temp = lim((temp_c - 36.53) * 340)

        regs = bytearray(128)
        regs[0x0D:0x11] = bytes([0x93, 0x52, 0x8F, 0x6A])
        regs[0x19] = self.cfg["smplrt_div"]
        regs[0x1A] = self.cfg["dlpf"]
        regs[0x1B] = self.cfg["gyro_fs"] << 3
        regs[0x1C] = self.cfg["accel_fs"] << 3 | 1
        regs[0x1F], regs[0x20], regs[0x69] = 20, 1, 0x15
        regs[0x37], regs[0x38], regs[0x3A] = 0x20, 0x41, 0x01
        for i, v in enumerate(acc + [temp] + gyr):
            struct.pack_into(">h", regs, 0x3B + 2 * i, v)
        regs[0x6B], regs[0x75] = 0x01, 0x68

        st_trim = bytes(regs[0x0D:0x11])
        sa, sg = demo_selftest_responses(st_trim)
        return Telemetry(
            TELEMETRY_MAGIC, TELEMETRY_VERSION, TELEMETRY_SIZE, 0, ms, self.count, ms,
            acc[0], acc[1], acc[2], temp, gyr[0], gyr[1], gyr[2],
            0x68 | self.cfg["ad0"], 0x68, *self.gint,
            STATUS_MPU_OK | STATUS_SELFTEST_OK | STATUS_VIA_INT_PIN,
            self.cfg["accel_fs"], self.cfg["gyro_fs"], self.cfg["dlpf"], self.cfg["smplrt_div"],
            0x01 | (0x40 if abs(shake) > 0.0199 else 0), st_trim, self.cfg["ad0"], 1,
            *sa, *sg, self.motion, self.count, 0, self.st_count,
            bytes(regs), self.cmd_seq, 0, 0, 0, self.cmd_ack, 0)

    def command(self, cmd_id, arg, current_seq):
        key = {1: "accel_fs", 2: "gyro_fs", 3: "dlpf", 4: "smplrt_div", 7: "ad0"}.get(cmd_id)
        if key:
            self.cfg[key] = arg
        if cmd_id == 5:
            self.st_count += 1
        self.cmd_seq = self.cmd_ack = current_seq + 1


def factory_trim(st_trim):
    """Factory trim values (register map rev 4.2, section 4.1)."""
    x, y, z, a = st_trim
    a_codes = [(x >> 5) << 2 | (a >> 4) & 3, (y >> 5) << 2 | (a >> 2) & 3, (z >> 5) << 2 | a & 3]
    g_codes = [x & 0x1F, y & 0x1F, z & 0x1F]
    ft_a = [4096 * 0.34 * (0.92 / 0.34) ** ((c - 1) / 30.0) if c else 0.0 for c in a_codes]
    ft_g = [(25 * 131 * 1.046 ** (c - 1)) * (-1 if i == 1 else 1) if c else 0.0 for i, c in enumerate(g_codes)]
    return a_codes, g_codes, ft_a, ft_g


def demo_selftest_responses(st_trim):
    _, _, ft_a, ft_g = factory_trim(st_trim)
    return [int(v * 1.04) for v in ft_a], [int(v * 0.97) for v in ft_g]


# --------------------------------------------------------------------------
# Processing: scaling, sensor fusion, self-test evaluation
# --------------------------------------------------------------------------
def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def wrap180(a):
    return (a + 180.0) % 360.0 - 180.0


class Processor:
    TAU = 0.5            # complementary filter time constant (s)
    CALIB_MS = 2000      # gyro bias calibration window

    def __init__(self):
        self.prev = None
        self.angles = None
        self.yaw = 0.0
        self.bias_raw = [0.0, 0.0, 0.0]  # raw counts at the current gyro_fs
        self.calib = {"int": [0, 0, 0], "ms": 0}
        self.calibrating = True
        self.rate_hist = collections.deque(maxlen=50)
        self.poll_hist = collections.deque(maxlen=50)

    def recalibrate(self):
        self.calibrating = True
        self.calib = {"int": [0, 0, 0], "ms": 0}

    def reset_yaw(self):
        self.yaw = 0.0

    def update(self, t):
        now = time.time()
        self.poll_hist.append(now)
        prev = self.prev
        if prev is not None and (t.sample_count < prev.sample_count or t.gyro_fs != prev.gyro_fs
                                 or t.uptime_ms < prev.uptime_ms):
            if t.gyro_fs != prev.gyro_fs and prev.gyro_fs < 4 and t.gyro_fs < 4:
                k = GYRO_LSB_PER_DPS[t.gyro_fs] / GYRO_LSB_PER_DPS[prev.gyro_fs]
                self.bias_raw = [b * k for b in self.bias_raw]
            prev = None
            self.calib = {"int": [0, 0, 0], "ms": 0}
        afs, gfs = t.accel_fs & 3, t.gyro_fs & 3
        a_lsb, g_lsb = ACCEL_LSB_PER_G[afs], GYRO_LSB_PER_DPS[gfs]
        accel = [v / a_lsb for v in (t.ax, t.ay, t.az)]
        gyro_raw = (t.gx, t.gy, t.gz)
        roll_acc = math.degrees(math.atan2(accel[1], accel[2]))
        pitch_acc = math.degrees(math.atan2(-accel[0], math.hypot(accel[1], accel[2])))

        if self.angles is None:
            self.angles = [roll_acc, pitch_acc]
        if prev is not None and t.sample_count != prev.sample_count:
            self.rate_hist.append((now, t.sample_count))
            dt_ms = (t.sample_time_ms - prev.sample_time_ms) & 0xFFFFFFFF
            dint = [s32(a - b) for a, b in zip((t.gix, t.giy, t.giz), (prev.gix, prev.giy, prev.giz))]
            if dt_ms > 0:
                if self.calibrating:
                    self.calib["ms"] += dt_ms
                    self.calib["int"] = [c + d for c, d in zip(self.calib["int"], dint)]
                    if self.calib["ms"] >= self.CALIB_MS:
                        self.bias_raw = [c / self.calib["ms"] for c in self.calib["int"]]
                        self.calibrating = False
                dtheta = [(d - b * dt_ms) / g_lsb / 1000.0 for d, b in zip(dint, self.bias_raw)]
                dt = dt_ms / 1000.0
                alpha = self.TAU / (self.TAU + dt)
                roll, pitch = self.angles
                roll = wrap180(roll_acc + alpha * wrap180(roll + dtheta[0] - roll_acc))
                pitch = pitch_acc + alpha * (pitch + dtheta[1] - pitch_acc)
                self.angles = [roll, pitch]
                if not self.calibrating:
                    self.yaw = wrap180(self.yaw + dtheta[2])
        self.prev = t

        rate = 0.0
        if len(self.rate_hist) >= 2 and self.rate_hist[-1][0] > self.rate_hist[0][0]:
            rate = (self.rate_hist[-1][1] - self.rate_hist[0][1]) / (self.rate_hist[-1][0] - self.rate_hist[0][0])
        poll = 0.0
        if len(self.poll_hist) >= 2 and self.poll_hist[-1] > self.poll_hist[0]:
            poll = (len(self.poll_hist) - 1) / (self.poll_hist[-1] - self.poll_hist[0])

        dlpf = t.dlpf if t.dlpf in DLPF_TABLE else 0
        bw_a, bw_g, base = DLPF_TABLE[dlpf]
        is6500 = t.who_am_i in MPU6500_FAMILY
        if is6500:
            bw_a = 1046 if t.regs[0x1D] & 0x08 else ACCEL_DLPF_6500[t.regs[0x1D] & 7]
            temp_c = t.temp / 333.87 + 21.0
        else:
            temp_c = t.temp / 340.0 + 36.53
        gyro_dps = [(r - b) / g_lsb for r, b in zip(gyro_raw, self.bias_raw)]
        st = self.selftest(t)
        calib_progress = min(1.0, self.calib["ms"] / self.CALIB_MS) if self.calibrating else 1.0

        return {
            "t": now,
            "mpu_ok": bool(t.status & STATUS_MPU_OK),
            "device": {
                "i2c_addr": "0x%02X" % t.i2c_addr,
                "ad0_level": t.ad0_level,
                "ad0_matches": (t.i2c_addr & 1) == t.ad0_level,
                "who_am_i": "0x%02X" % t.who_am_i,
                "chip": chip_name(t.who_am_i),
                "known_chip": t.who_am_i in CHIPS,
                "uptime_s": t.uptime_ms / 1000.0,
                "i2c_errors": t.i2c_errors,
            },
            "config": {
                "accel_fs": afs, "accel_range_g": ACCEL_RANGE_G[afs],
                "gyro_fs": gfs, "gyro_range_dps": GYRO_RANGE_DPS[gfs],
                "dlpf": dlpf, "dlpf_accel_hz": bw_a, "dlpf_gyro_hz": bw_g,
                "smplrt_div": t.smplrt_div, "sample_rate_hz": base / (1 + t.smplrt_div),
            },
            "raw": {"accel": [t.ax, t.ay, t.az], "gyro": list(gyro_raw), "temp": t.temp},
            "accel_g": accel,
            "accel_ms2": [v * G for v in accel],
            "accel_mag_g": math.sqrt(sum(v * v for v in accel)),
            "gyro_dps": gyro_dps,
            "gyro_bias_dps": [b / g_lsb for b in self.bias_raw],
            "temp_c": temp_c,
            "angles": {"roll": self.angles[0], "pitch": self.angles[1], "yaw": self.yaw,
                       "roll_acc": roll_acc, "pitch_acc": pitch_acc},
            "interrupts": {
                "int_status": t.int_status,
                "data_ready": bool(t.int_status & 0x01),
                "motion": bool(t.int_status & 0x40),
                "fifo_overflow": bool(t.int_status & 0x10),
                "via_int_pin": bool(t.status & STATUS_VIA_INT_PIN),
                "int_pin_count": t.int_pin_count,
                "motion_count": t.motion_count,
            },
            "counters": {"sample_count": t.sample_count, "measured_rate_hz": rate, "poll_rate_hz": poll},
            "calibration": {"active": self.calibrating, "progress": calib_progress},
            "selftest": st,
            "registers": [{"addr": a, "name": n, "value": t.regs[a], "decoded": decode_register(a, t.regs[a], is6500)}
                          for a, n in sorted((REGISTER_NAMES_6500 if is6500 else REGISTER_NAMES).items())],
            "cmd_pending": t.cmd_ack != t.cmd_seq,
        }

    @staticmethod
    def selftest(t):
        if not t.status & STATUS_SELFTEST_OK:
            return {"done": False, "count": t.selftest_count, "axes": [], "pass": None}
        if t.who_am_i in MPU6500_FAMILY:
            return Processor.selftest_6500(t)
        a_codes, g_codes, ft_a, ft_g = factory_trim(t.st_trim)
        axes = []
        for i, ax in enumerate("XYZ"):
            for kind, code, ft, resp in (("Accel", a_codes[i], ft_a[i], (t.sax, t.say, t.saz)[i]),
                                         ("Gyro", g_codes[i], ft_g[i], (t.sgx, t.sgy, t.sgz)[i])):
                change = (resp - ft) / ft * 100.0 if ft else None
                axes.append({"name": "%s %s" % (kind, ax), "code": code, "factory_trim": ft,
                             "response": resp, "change_pct": change,
                             "pass": change is not None and abs(change) <= 14.0})
        axes.sort(key=lambda a: a["name"].startswith("Gyro"))
        return {"done": True, "count": t.selftest_count, "axes": axes, "pass": all(a["pass"] for a in axes),
                "criteria": "MPU-6050 rule: within ±14 % of the factory trim value"}

    @staticmethod
    def selftest_6500(t):
        """MPU6500 self-test (AN-MPU-6500A-02): factory value = 2620 * 1.01^(code-1)
        at ±2 g / ±250 °/s. Pass: accel 50-150 %, gyro above 50 % of it."""
        axes = []
        for kind, base, resps, lo, hi in (("Accel", 0x0D, (t.sax, t.say, t.saz), 0.5, 1.5),
                                          ("Gyro", 0x00, (t.sgx, t.sgy, t.sgz), 0.5, None)):
            for i, ax in enumerate("XYZ"):
                code = t.regs[base + i]
                ft = 2620 * 1.01 ** (code - 1) if code else 0.0
                ratio = abs(resps[i]) / ft if ft else None
                ok = ratio is not None and ratio > lo and (hi is None or ratio < hi)
                axes.append({"name": "%s %s" % (kind, ax), "code": code, "factory_trim": ft,
                             "response": resps[i], "change_pct": (ratio - 1) * 100 if ratio is not None else None,
                             "pass": ok})
        return {"done": True, "count": t.selftest_count, "axes": axes, "pass": all(a["pass"] for a in axes),
                "criteria": "MPU-6500 rule: accel 50-150 %, gyro above 50 % of the factory value"}


# --------------------------------------------------------------------------
# Acquisition thread
# --------------------------------------------------------------------------
class Monitor:
    def __init__(self, device, poll_hz):
        self.device = device
        self.period = 1.0 / poll_hz
        self.proc = Processor()
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.state = {"connected": False, "error": "connecting...", "source": device.name}
        self.version = 0
        self.commands = queue.Queue()
        self.last_tm = None
        self.stop = threading.Event()

    def publish(self, state):
        with self.cond:
            self.state = state
            self.version += 1
            self.cond.notify_all()

    def snapshot(self):
        with self.lock:
            return self.version, self.state

    def wait_new(self, version, timeout):
        with self.cond:
            self.cond.wait_for(lambda: self.version != version or self.stop.is_set(), timeout)
            return self.version, self.state

    def send(self, name, value=0):
        self.commands.put((name, int(value)))

    def run(self):
        connected = False
        while not self.stop.is_set():
            try:
                if not connected:
                    self.device.connect()
                    connected = True
                    self.proc = Processor()
                self.process_commands()
                tm = self.device.read()
                if tm is not None:
                    self.last_tm = tm
                    state = self.proc.update(tm)
                    state.update(connected=True, error=None, source=self.device.name)
                    self.publish(state)
                time.sleep(self.period)
            except (OSError, OpenOCDError, ValueError, struct.error) as e:
                connected = False
                if isinstance(self.device, SwdDevice):
                    self.device.close_socket()
                self.publish({"connected": False, "error": str(e), "source": self.device.name})
                self.stop.wait(1.0)
        self.device.close()

    def process_commands(self):
        while True:
            try:
                name, value = self.commands.get_nowait()
            except queue.Empty:
                return
            if name == "calibrate":
                self.proc.recalibrate()
            elif name == "reset_yaw":
                self.proc.reset_yaw()
            elif name in CMD_IDS and self.last_tm is not None:
                self.device.command(CMD_IDS[name], value, self.last_tm.cmd_seq)
                # the next command must see the new cmd_seq
                self.last_tm = self.last_tm._replace(cmd_seq=(self.last_tm.cmd_seq + 1) & 0xFFFFFFFF)


# --------------------------------------------------------------------------
# Terminal output
# --------------------------------------------------------------------------
def fmt3(values, fmt):
    return "  ".join(fmt % v for v in values)


def render_terminal(s, url=None):
    out = []
    w = out.append
    w("MPU6050 monitor  [source: %s]%s" % (s.get("source"), ("   web: " + url) if url else ""))
    w("=" * 78)
    if not s.get("connected"):
        w("Not connected: %s" % s.get("error"))
        w("")
        w("Checklist: ST-Link plugged in, firmware flashed (./flash.sh), openocd installed,")
        w("st-flash/another debugger not using the ST-Link at the same time.")
        return "\n".join(out)
    d, c, i, k = s["device"], s["config"], s["interrupts"], s["counters"]
    if not s["mpu_ok"]:
        w("!! MPU6050 NOT RESPONDING on I2C (check VCC/GND/SCL=PB6/SDA=PB7). Firmware retries every second.")
    w("Device   addr %s (AD0=%d%s)  WHO_AM_I %s = %s  uptime %.1f s  I2C errors %d" % (
        d["i2c_addr"], d["ad0_level"], "" if d["ad0_matches"] else " MISMATCH", d["who_am_i"],
        d["chip"], d["uptime_s"], d["i2c_errors"]))
    w("Config   accel ±%d g   gyro ±%d °/s   DLPF %d (%d/%d Hz)   rate %.1f Hz (div %d)" % (
        c["accel_range_g"], c["gyro_range_dps"], c["dlpf"], c["dlpf_accel_hz"], c["dlpf_gyro_hz"],
        c["sample_rate_hz"], c["smplrt_div"]))
    w("Rates    samples %d   measured %.1f Hz   host polls %.0f Hz" % (
        k["sample_count"], k["measured_rate_hz"], k["poll_rate_hz"]))
    w("-" * 78)
    w("                    X          Y          Z")
    w("Accel raw     %s" % fmt3(s["raw"]["accel"], "%9d"))
    w("Accel g       %s     |a| = %.3f g" % (fmt3(s["accel_g"], "%9.3f"), s["accel_mag_g"]))
    w("Accel m/s²    %s" % fmt3(s["accel_ms2"], "%9.2f"))
    w("Gyro raw      %s" % fmt3(s["raw"]["gyro"], "%9d"))
    w("Gyro °/s      %s     (bias removed)" % fmt3(s["gyro_dps"], "%9.2f"))
    w("Gyro bias °/s %s" % fmt3(s["gyro_bias_dps"], "%9.2f"))
    w("Temperature   %9.2f °C   (raw %d)" % (s["temp_c"], s["raw"]["temp"]))
    w("-" * 78)
    a = s["angles"]
    cal = s["calibration"]
    w("Angles   roll %7.1f°   pitch %7.1f°   yaw %7.1f° (gyro only, drifts)" % (a["roll"], a["pitch"], a["yaw"]))
    w("         accel-only roll %7.1f°  pitch %7.1f°" % (a["roll_acc"], a["pitch_acc"]))
    if cal["active"]:
        w("         >> calibrating gyro bias, keep the sensor still... %d%%" % (cal["progress"] * 100))
    w("Interrupt INT_STATUS 0x%02X [%s%s%s]  PB12 %s  INT-pin samples %d  motion events %d" % (
        i["int_status"], "DATA_RDY " if i["data_ready"] else "", "MOTION " if i["motion"] else "",
        "FIFO_OFLOW" if i["fifo_overflow"] else "",
        "working" if i["int_pin_count"] else "NOT SEEN (polling fallback)", i["int_pin_count"], i["motion_count"]))
    st = s["selftest"]
    w("-" * 78)
    if st["done"]:
        w("Self-test (run %d): %s   [%s]" % (st["count"], "PASS" if st["pass"] else "FAIL", st["criteria"]))
        w("  " + "   ".join("%s %+.1f%%" % (x["name"], x["change_pct"]) if x["change_pct"] is not None
                            else "%s n/a" % x["name"] for x in st["axes"][:3]))
        w("  " + "   ".join("%s %+.1f%%" % (x["name"], x["change_pct"]) if x["change_pct"] is not None
                            else "%s n/a" % x["name"] for x in st["axes"][3:]))
    else:
        w("Self-test: not run yet")
    w("Ctrl+C to quit" + ("" if url else ".  Run with --web for the graphical dashboard."))
    return "\n".join(out)


CSV_HEADER = ("time_s,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,temp_c,roll_deg,pitch_deg,yaw_deg,"
              "int_status,sample_count")


def render_csv(s):
    a = s["angles"]
    return "%.3f,%s,%s,%.2f,%.2f,%.2f,%.2f,%d,%d" % (
        s["t"], ",".join("%.4f" % v for v in s["accel_g"]), ",".join("%.3f" % v for v in s["gyro_dps"]),
        s["temp_c"], a["roll"], a["pitch"], a["yaw"], s["interrupts"]["int_status"], s["counters"]["sample_count"])


# --------------------------------------------------------------------------
# Web dashboard (HTTP + Server-Sent Events)
# --------------------------------------------------------------------------
def make_handler(monitor):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send_json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                with open(DASHBOARD_HTML, "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path == "/api/state":
                self.send_json(monitor.snapshot()[1])
            elif self.path == "/events":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                version = -1
                try:
                    while not monitor.stop.is_set():
                        version, state = monitor.wait_new(version, 1.0)
                        self.wfile.write(b"data: " + json.dumps(state).encode() + b"\n\n")
                        self.wfile.flush()
                        time.sleep(1 / 30)  # cap at ~30 updates/s per browser
                except (BrokenPipeError, ConnectionResetError):
                    pass
            else:
                self.send_error(404)

        def do_POST(self):
            if self.path != "/api/command":
                return self.send_error(404)
            try:
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
                name = body["cmd"]
                if name not in CMD_IDS and name not in ("calibrate", "reset_yaw"):
                    raise KeyError(name)
                monitor.send(name, body.get("value", 0))
                self.send_json({"ok": True})
            except (KeyError, ValueError, TypeError) as e:
                self.send_json({"ok": False, "error": "bad command: %s" % e}, 400)

    return Handler


class WebServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def probe(args):
    """Raw OpenOCD responses: shows whether memory reads return real data."""
    dev = SwdDevice(args)
    try:
        try:
            dev.ocd = OpenOCD(args.openocd_host, args.openocd_port)
        except OSError:
            dev.proc, dev.log = launch_openocd(args.openocd_port, args.adapter_khz)
            dev.ocd = OpenOCD(args.openocd_host, args.openocd_port)
        sym = elf_symbol(args.elf, TELEMETRY_SYMBOL) if os.path.exists(args.elf) else None
        addr = sym[0] if sym else RAM_START
        print("g_telemetry address from ELF: 0x%08x (expect magic 0x%08x)" % (addr, TELEMETRY_MAGIC))
        for c in ("version", "[target current] curstate", "adapter speed",
                  "read_memory 0x08000000 32 2",          # vector table: 0x20005000, Reset_Handler
                  "read_memory 0x%08x 32 4" % addr, "mdw 0x%08x 4" % addr,
                  "halt 500", "read_memory 0x%08x 32 4" % addr, "mdw 0x%08x 4" % addr,
                  "reg pc", "resume", "[target current] curstate"):
            print("\n> %s\n%s" % (c, dev.ocd.cmd("catch {%s} r; set r" % c)))
        if dev.log:
            print("\nOpenOCD log:\n  " + "\n  ".join(dev.log))
    finally:
        dev.close()


def main():
    if os.name == "nt":
        os.system("")  # turns on ANSI escape codes in the Windows console
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")  # °, ±, ² on non-UTF-8 consoles
        except AttributeError:
            pass

    ap = argparse.ArgumentParser(description="Live MPU6050 data from the STM32 (ST-Link or native USB)")
    ap.add_argument("--web", action="store_true", help="serve the real-time dashboard")
    ap.add_argument("--port", type=int, default=8000, help="web dashboard port (default 8000)")
    ap.add_argument("--no-browser", action="store_true", help="do not open the browser automatically")
    ap.add_argument("--csv", action="store_true", help="print CSV lines instead of the live screen")
    ap.add_argument("--demo", action="store_true", help="simulated sensor, no hardware needed")
    ap.add_argument("--usb", nargs="?", const="auto", default=None, metavar="PORT",
                    help="read over the Blue Pill's own USB port (USB serial) instead of the ST-Link; "
                         "PORT is found automatically on Linux/macOS, give it on Windows (e.g. COM5)")
    ap.add_argument("--elf", default=DEFAULT_ELF, help="firmware ELF used to find g_telemetry")
    ap.add_argument("--openocd-host", default="127.0.0.1")
    ap.add_argument("--openocd-port", type=int, default=6666)
    ap.add_argument("--no-launch", action="store_true", help="do not start openocd, connect to a running one")
    ap.add_argument("--adapter-khz", type=int, default=None,
                    help="SWD clock for openocd (e.g. 200 if the connection is unreliable)")
    ap.add_argument("--poll-hz", type=float, default=100.0, help="how often RAM is read (default 100)")
    ap.add_argument("--halt-reads", action="store_true",
                    help="halt the CPU for each read (use if live reads return zeros)")
    ap.add_argument("--probe", action="store_true", help="print raw OpenOCD memory reads for troubleshooting")
    args = ap.parse_args()

    if args.probe:
        return probe(args)

    if args.demo:
        device = DemoDevice(args)
    elif args.usb:
        device = UsbDevice(args)
    else:
        device = SwdDevice(args)
    monitor = Monitor(device, args.poll_hz)
    worker = threading.Thread(target=monitor.run, daemon=True)
    worker.start()

    url = None
    if args.web:
        try:
            server = WebServer(("127.0.0.1", args.port), make_handler(monitor))
        except OSError as e:
            if e.errno != 98 and getattr(e, "winerror", None) != 10048:   # address in use (Linux / Windows)
                raise
            sys.exit("Port %d is already used, probably by another mpu6050_monitor.py.\n"
                     "Stop that one (Ctrl+C in its terminal) or choose another port: --port %d"
                     % (args.port, args.port + 1))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = "http://127.0.0.1:%d/" % args.port
        if not args.no_browser:
            threading.Timer(0.8, lambda: webbrowser.open(url)).start()

    interactive = sys.stdout.isatty() and not args.csv
    try:
        if args.csv:
            print(CSV_HEADER, flush=True)
            last = None
            version = -1
            while True:
                version, s = monitor.wait_new(version, 1.0)
                if not s.get("connected"):
                    print("# " + str(s.get("error")), file=sys.stderr, flush=True)
                    time.sleep(1)
                    continue
                n = s["counters"]["sample_count"]
                if n != last:
                    last = n
                    print(render_csv(s), flush=True)
        else:
            if interactive:
                sys.stdout.write("\x1b[?25l")  # hide cursor
            while True:
                _, s = monitor.snapshot()
                text = render_terminal(s, url)
                if interactive:
                    sys.stdout.write("\x1b[H\x1b[2J" + text + "\n")
                    sys.stdout.flush()
                    time.sleep(0.1)
                else:
                    print(text.splitlines()[2] if s.get("connected") else text, flush=True)
                    time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        if interactive:
            sys.stdout.write("\x1b[?25h\n")
        monitor.stop.set()
        with monitor.cond:
            monitor.cond.notify_all()
        worker.join(timeout=3)


if __name__ == "__main__":
    main()
