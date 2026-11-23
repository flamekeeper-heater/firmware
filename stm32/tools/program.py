#!/usr/bin/env python3
"""Program the safety MCU (STM32G030K8) over SWD: the image, then the option bytes WDG-1 asks for.

    uv run --with pyocd python3 stm32/tools/program.py stm32/build/bringup.bin
    python3 stm32/tools/program.py --dry-run stm32/build/safety.bin     # every step, no probe

Connect an SWD probe (ST-Link, CMSIS-DAP or J-Link) to J_DEBUG: pin 1 GND, 2 V3V3 (reference
only: power the board from its own supply), 3 SWDIO, 4 SWCLK, 5 NRST. Keep the ESP32 off STM_BOOT
(SWCLK is the BOOT0 pin): leave it unflashed, or hold it in its ROM loader. Program on a stable
supply (WDG-11).

The steps, in order (--dry-run prints them, against a simulated factory-fresh chip):
  connect      under reset, the core halted
  identify     DEV_ID 0x466 and a 64 KB flash; the UID read for the log
  protection   read-out protection must be at level 0
  watchdog     if the option bytes already start the IWDG by hardware (a re-flash), it is frozen
               while the core is halted and slowed to about 32 s, and refreshed between pages
  saved pages  pages 30 and 31 (the saved parameters and the plug record) read: blank means a blank
               board, and the whole flash is mass-erased; holding data, the tool stops unless told
               --keep-saved (erase only the image's pages) or --wipe-saved (mass erase anyway)
  erase        mass erase, or page by page (never 30 or 31)
  program      double words; page 0 last, so a cut leaves no vector table
  verify       every byte read back (and the rest of the erased pages blank)
  options      FLASH_OPTR = the WDG-1 set (IWDG_SW 0, IWDG_STOP 1, IWDG_STDBY 1, nBOOT_SEL 0,
               nBOOT1 1, nBOOT0 1), every other bit kept; RDP stays level 0 unless --rdp-level1
  launch       OBL_LAUNCH: the option bytes reload and the chip resets into the new image (a plain
               reset is not enough the first time: the EMPTY flag is refreshed only by an option
               load, WDG-12)
  check        reconnected without a reset: FLASH_OPTR, OPTVERR and the option area (value and
               complement) read back and checked

--rdp-level1 sets read-out protection level 1. Do not use it without a reason: afterwards SWD can no
longer read the flash, the ROM bootloader refuses Read Memory, so the ESP32's updater (which reads
every block back) can no longer update the safety MCU from the app, and going back to level 0
mass-erases the whole flash, the saved pages included. Level 2 is never set (WDG-6).

Standard library, plus pyocd for a real probe (imported only then): `uv run --with pyocd`. pyocd
because it installs from PyPI into uv's cache (nothing system-wide), runs on Linux, macOS and
Windows with the usual probes, and is driven from Python, so the factory test calls this tool in
process and the step plan is host-tested. No CMSIS pack and no flash algorithm: every step is a
register access through the generic Cortex-M target, as RM0454 describes it, so pages 30 and 31 are
erased only when this tool says so. Exit code 0 when the board is programmed and checked.
"""
import argparse
import hashlib
import struct
import sys
import time

# ---------------------------------------------------------------- the chip (RM0454; common/g030/g030_map.h)

FLASH_BASE = 0x08000000
PAGE_SIZE = 2048
PAGES = 32
APP_PAGES = 30  # the application: pages 0 to 29
SAVED_FIRST = 30  # pages 30 and 31: the saved parameters and the plug record
RAM_BASE = 0x20000000
RAM_END = 0x20002000
CHIP_ID = 0x466

OPTION_BASE = 0x1FFF7800  # FLASH_OPTR's stored double word: the value, then its complement
UID_BASE = 0x1FFF7590  # 96-bit unique id (ST's CMSIS header, UID_BASE)
FLASHSIZE_BASE = 0x1FFF75E0  # flash size in KB, 16 bits
DBG_IDCODE = 0x40015800  # DEV_ID in bits 11:0
DBG_APB_FZ1 = 0x40015808
DBG_IWDG_STOP = 1 << 12  # the IWDG frozen while the core is halted (DBG_APB_FZ1, as in ST's stm32g030xx.h)
RCC_APBENR1 = 0x4002103C
RCC_APBENR1_DBGEN = 1 << 27

FLASH_R = 0x40022000
FLASH_KEYR = FLASH_R + 0x08
FLASH_OPTKEYR = FLASH_R + 0x0C
FLASH_SR = FLASH_R + 0x10
FLASH_CR = FLASH_R + 0x14
FLASH_OPTR = FLASH_R + 0x20
FLASH_KEY1, FLASH_KEY2 = 0x45670123, 0xCDEF89AB
FLASH_OPTKEY1, FLASH_OPTKEY2 = 0x08192A3B, 0x4C5D6E7F

SR_EOP = 1 << 0
SR_ERRORS = 0x83FA  # OPERR PROGERR WRPERR PGAERR SIZERR PGSERR MISSERR FASTERR OPTVERR
SR_OPTVERR = 1 << 15
SR_BSY1 = 1 << 16
SR_CFGBSY = 1 << 18
CR_PG = 1 << 0
CR_PER = 1 << 1
CR_MER1 = 1 << 2
CR_PNB_SHIFT = 3
CR_STRT = 1 << 16
CR_OPTSTRT = 1 << 17
CR_OBL_LAUNCH = 1 << 27
CR_OPTLOCK = 1 << 30
CR_LOCK = 1 << 31

IWDG_KR = 0x40003000
IWDG_PR = 0x40003004
IWDG_RLR = 0x40003008
IWDG_SR = 0x4000300C
IWDG_KEY_UNLOCK, IWDG_KEY_RELOAD = 0x5555, 0xAAAA
IWDG_PR_256, IWDG_RLR_MAX = 6, 0xFFF  # LSI 32 kHz / 256 x 4096: about 32.8 s

# FLASH_OPTR
OPTR_RDP = 0xFF
RDP_LEVEL_0 = 0xAA
RDP_LEVEL_2 = 0xCC
RDP_LEVEL_1 = 0xBB  # any value but 0xAA and 0xCC is level 1
OPTR_IWDG_SW = 1 << 16
OPTR_IWDG_STOP = 1 << 17
OPTR_IWDG_STDBY = 1 << 18
OPTR_NBOOT_SEL = 1 << 24
OPTR_NBOOT1 = 1 << 25
OPTR_NBOOT0 = 1 << 26
WDG1_CLEAR = OPTR_IWDG_SW | OPTR_NBOOT_SEL
WDG1_SET = OPTR_IWDG_STOP | OPTR_IWDG_STDBY | OPTR_NBOOT1 | OPTR_NBOOT0
WDG1_CHECKED = WDG1_CLEAR | WDG1_SET
OPTR_FACTORY = 0xDFFFE1AA  # as shipped (WDG-15)

REGISTER_NAMES = {
    FLASH_KEYR: "FLASH_KEYR", FLASH_OPTKEYR: "FLASH_OPTKEYR", FLASH_SR: "FLASH_SR", FLASH_CR: "FLASH_CR",
    FLASH_OPTR: "FLASH_OPTR", IWDG_KR: "IWDG_KR", IWDG_PR: "IWDG_PR", IWDG_RLR: "IWDG_RLR",
    DBG_APB_FZ1: "DBG_APB_FZ1", RCC_APBENR1: "RCC_APBENR1",
}


def optr_required(optr, rdp_level1=False):
    """FLASH_OPTR with WDG-1's bits cleared and set and every other bit kept (the ESP32 updater's
    g030_optr_required); RDP at level 1 only when asked."""
    v = (optr & ~WDG1_CLEAR & 0xFFFFFFFF) | WDG1_SET
    if rdp_level1:
        v = (v & ~OPTR_RDP) | RDP_LEVEL_1
    return v


def wdg1_ok(optr, flash_sr=0):
    """The firmware's own test (hal_optr_wdg1_ok): WDG-1's bits as required, and loaded cleanly."""
    return (optr & WDG1_CHECKED) == WDG1_SET and not flash_sr & SR_OPTVERR


def rdp_level(optr):
    rdp = optr & OPTR_RDP
    return 0 if rdp == RDP_LEVEL_0 else 2 if rdp == RDP_LEVEL_2 else 1


def describe_optr(optr):
    bits = [("iwdg_sw", 16), ("iwdg_stop", 17), ("iwdg_stdby", 18), ("nboot_sel", 24), ("nboot1", 25), ("nboot0", 26)]
    return "0x%08x (rdp level %d, %s)" % (optr, rdp_level(optr), " ".join("%s=%d" % (n, optr >> b & 1) for n, b in bits))


def format_uid(words):
    """The 96-bit UID as the bring-up console's `uid` prints it: three words in address order."""
    return "-".join("%08x" % w for w in words)


def check_image(data):
    """A plausible safety MCU image for this flash map, or a ProgramError."""
    if not data:
        raise ProgramError("the image is empty")
    if len(data) > APP_PAGES * PAGE_SIZE:
        raise ProgramError("the image is %d bytes: more than pages 0-29 (%d)" % (len(data), APP_PAGES * PAGE_SIZE))
    if len(data) < 8:
        raise ProgramError("the image has no vector table")
    sp, reset = struct.unpack_from("<II", data)
    if not RAM_BASE < sp <= RAM_END:
        raise ProgramError("the image's stack pointer 0x%08x is not in the G030's RAM: not a safety MCU image" % sp)
    if not reset & 1 or not FLASH_BASE <= reset & ~1 < FLASH_BASE + len(data):
        raise ProgramError("the image's reset vector 0x%08x is not a Thumb address inside it" % reset)


def image_pages(data):
    return (len(data) + PAGE_SIZE - 1) // PAGE_SIZE


class ProgramError(Exception):
    pass


class Disconnected(Exception):
    """The chip reset under the probe (OBL_LAUNCH)."""


# ---------------------------------------------------------------- the probe


class SwdChip:
    """The chip through pyocd's generic Cortex-M target (no CMSIS pack)."""

    def __init__(self, probe=None, frequency=None):
        self.probe, self.frequency = probe, frequency
        self.session = self.t = None

    def connect(self, mode):
        try:
            from pyocd.core.helpers import ConnectHelper  # type: ignore
        except ImportError:
            raise ProgramError("pyocd is not installed: run with `uv run --with pyocd python3 ...`")
        options = {"target_override": "cortex_m", "connect_mode": mode}
        if self.frequency:
            options["frequency"] = self.frequency
        self.session = ConnectHelper.session_with_chosen_probe(
            unique_id=self.probe, blocking=False, options=options)
        if self.session is None:
            raise ProgramError("no SWD probe found")
        self.session.open()
        self.t = self.session.board.target

    def close(self):
        if self.session is not None:
            try:
                self.session.close()
            except Exception:
                pass
        self.session = self.t = None

    def read32(self, addr):
        return self.t.read32(addr)

    def write32(self, addr, value):
        self.t.write32(addr, value)

    def read_bytes(self, addr, n):
        return bytes(self.t.read_memory_block8(addr, n))

    def write_words(self, addr, words):
        self.t.write_memory_block32(addr, list(words))

    def write32_reset(self, addr, value):
        """A write that resets the chip: the probe loses it, which is expected."""
        try:
            self.t.write32(addr, value)
            self.t.flush()
        except Exception:
            pass
        self.close()


# ---------------------------------------------------------------- a simulated chip (dry run and tests)


class SimulatedG030:
    """The flash controller as RM0454 describes it, as far as this tool uses it: the locks, mass and
    page erase, double-word programming into erased flash only, the option bytes programmed by
    OPTSTRT from FLASH_OPTR and loaded by OBL_LAUNCH (a reset). Tests also use it to break things."""

    def __init__(self, optr=OPTR_FACTORY, flash=None, uid=(0x00350027, 0x4D4B5009, 0x20393650), trace=None,
                 dev_id=CHIP_ID):
        self.flash = bytearray(flash if flash is not None else b"\xff" * (PAGES * PAGE_SIZE))
        self.option_area = [optr, ~optr & 0xFFFFFFFF]
        self.uid = uid
        self.dev_id = dev_id
        self.trace = trace
        self.connected = None
        self.writes = []  # (address, value) of every register write
        self.erased_pages = []
        self.programmed = []  # flash offsets of the double words, in order
        self.mass_erases = 0
        self.resets = 0
        self.iwdg_running = False
        self.fail_after_reset = None  # e.g. lambda chip: ... to corrupt the option load
        self._load()

    # The option load and the reset that follows it.
    def _load(self):
        v, c = self.option_area
        self.sr = 0
        if v != ~c & 0xFFFFFFFF:  # WDG-7: a mismatch loads every user bit as 1, RDP as level 1
            v = 0xFFFFFF00 | RDP_LEVEL_1
            self.sr |= SR_OPTVERR
        self.optr = v
        self.cr = CR_LOCK | CR_OPTLOCK
        self.key_step = self.optkey_step = 0
        self.pending = None
        self.iwdg_running = not v & OPTR_IWDG_SW
        self.iwdg_pr = 0
        if self.fail_after_reset:
            self.fail_after_reset(self)

    def connect(self, mode):
        self.connected = mode
        if self.trace:
            self.trace("    swd: connect (%s)" % mode)

    def close(self):
        self.connected = None

    def _need(self):
        if not self.connected:
            raise Disconnected("not connected")

    def _protected(self):
        return rdp_level(self.optr) != 0

    def read32(self, addr):
        self._need()
        if FLASH_BASE <= addr < FLASH_BASE + len(self.flash):
            return struct.unpack_from("<I", self.read_bytes(addr, 4))[0]
        if addr == FLASH_SR:
            return self.sr
        if addr == FLASH_CR:
            return self.cr
        if addr == FLASH_OPTR:
            return self.optr
        if addr == OPTION_BASE:
            return self.option_area[0]
        if addr == OPTION_BASE + 4:
            return self.option_area[1]
        if addr == DBG_IDCODE:
            return 0x10006000 | self.dev_id
        if addr == FLASHSIZE_BASE:
            return 0xFFFF0000 | (len(self.flash) // 1024)
        if UID_BASE <= addr < UID_BASE + 12 and not (addr - UID_BASE) % 4:
            return self.uid[(addr - UID_BASE) // 4]
        if addr == IWDG_SR:
            return 0
        return 0

    def read_bytes(self, addr, n):
        self._need()
        if FLASH_BASE <= addr and addr + n <= FLASH_BASE + len(self.flash):
            if self._protected():
                raise ProgramError("flash read refused (read-out protection)")
            return bytes(self.flash[addr - FLASH_BASE:addr - FLASH_BASE + n])
        out = b""
        for a in range(addr, addr + n, 4):
            out += struct.pack("<I", self.read32(a))
        return out[:n]

    def write_words(self, addr, words):
        for i, w in enumerate(words):
            self.write32(addr + 4 * i, w)

    def write32_reset(self, addr, value):
        self.write32(addr, value)
        self.connected = None

    def write32(self, addr, value):
        self._need()
        value &= 0xFFFFFFFF
        if FLASH_BASE <= addr < FLASH_BASE + len(self.flash):
            return self._program(addr, value)
        self.writes.append((addr, value))
        if self.trace and addr in REGISTER_NAMES:
            self.trace("    swd: %s <- 0x%08x" % (REGISTER_NAMES[addr], value))
        if addr == FLASH_KEYR:
            if self.key_step == 0 and value == FLASH_KEY1:
                self.key_step = 1
            elif self.key_step == 1 and value == FLASH_KEY2:
                self.cr &= ~CR_LOCK
                self.key_step = 2
            else:  # a wrong key locks FLASH_CR until the next reset
                self.key_step = 3
        elif addr == FLASH_OPTKEYR:
            if self.cr & CR_LOCK:
                return
            if self.optkey_step == 0 and value == FLASH_OPTKEY1:
                self.optkey_step = 1
            elif self.optkey_step == 1 and value == FLASH_OPTKEY2:
                self.cr &= ~CR_OPTLOCK
                self.optkey_step = 2
            else:
                self.optkey_step = 3
        elif addr == FLASH_SR:
            self.sr &= ~(value & (SR_ERRORS | SR_EOP))
        elif addr == FLASH_OPTR:
            if not self.cr & CR_OPTLOCK:
                self.optr = value
        elif addr == FLASH_CR:
            self._cr(value)
        elif addr == IWDG_PR:
            self.iwdg_pr = value

    def _cr(self, value):
        if self.cr & CR_LOCK:
            return
        if value & CR_LOCK:
            self.cr = CR_LOCK | CR_OPTLOCK
            return
        keep = self.cr & (CR_LOCK | CR_OPTLOCK)
        if value & CR_OPTLOCK:
            keep |= CR_OPTLOCK
        if value & CR_OBL_LAUNCH:
            if self.cr & CR_OPTLOCK:
                return
            self.resets += 1
            self._load()
            self.connected = None
            raise Disconnected("OBL_LAUNCH: the chip reset")
        if value & CR_STRT:
            if value & CR_MER1:
                self.flash[:] = b"\xff" * len(self.flash)
                self.mass_erases += 1
            elif value & CR_PER:
                page = (value >> CR_PNB_SHIFT) & 0x3FF
                if page >= PAGES:
                    self.sr |= 1 << 5  # PGAERR
                else:
                    self.flash[page * PAGE_SIZE:(page + 1) * PAGE_SIZE] = b"\xff" * PAGE_SIZE
                    self.erased_pages.append(page)
            value &= ~CR_STRT
        if value & CR_OPTSTRT:
            if keep & CR_OPTLOCK:
                return
            self.option_area = [self.optr, ~self.optr & 0xFFFFFFFF]
            value &= ~CR_OPTSTRT
        self.cr = (value & ~(CR_LOCK | CR_OPTLOCK)) | keep

    def _program(self, addr, value):
        if self.cr & CR_LOCK or not self.cr & CR_PG:
            self.sr |= 1 << 7  # PGSERR
            return
        off = addr - FLASH_BASE
        if self.pending is None:
            if off % 8:
                self.sr |= 1 << 5  # PGAERR
                return
            self.pending = (off, value)
            return
        first_off, first = self.pending
        self.pending = None
        if off != first_off + 4:
            self.sr |= 1 << 5
            return
        if self.flash[first_off:first_off + 8] != b"\xff" * 8:
            self.sr |= 1 << 3  # PROGERR: not erased
            return
        self.flash[first_off:first_off + 8] = struct.pack("<II", first, value)
        self.programmed.append(first_off)


# ---------------------------------------------------------------- the steps


class Programmer:
    def __init__(self, chip, image, keep_saved=False, wipe_saved=False, rdp_level1=False, skip_options=False,
                 log=print, timeout_s=2.0):
        if keep_saved and wipe_saved:
            raise ProgramError("--keep-saved and --wipe-saved contradict each other")
        self.chip, self.image = chip, bytes(image)
        self.keep_saved, self.wipe_saved = keep_saved, wipe_saved
        self.rdp_level1, self.skip_options = rdp_level1, skip_options
        self.log, self.timeout_s = log, timeout_s
        self.iwdg_tamed = False
        self.result = {"image_bytes": len(self.image), "image_sha256": hashlib.sha256(self.image).hexdigest()}

    def steps(self):
        """The plan: (name, what it does, method)."""
        return [
            ("connect", "connect under reset, the core halted", self.connect),
            ("identify", "DEV_ID 0x466, 64 KB of flash, the UID", self.identify),
            ("protection", "read-out protection at level 0", self.protection),
            ("watchdog", "a hardware-started IWDG frozen and slowed (re-flash)", self.watchdog),
            ("saved pages", "pages 30 and 31: blank, or kept / wiped as asked", self.saved_pages),
            ("erase", "mass erase, or the image's pages only", self.erase),
            ("program", "the image in double words, page 0 last", self.program),
            ("verify", "every byte read back", self.verify),
            ("options", "FLASH_OPTR to the WDG-1 set", self.options),
            ("launch", "OBL_LAUNCH: option load and reset", self.launch),
            ("check", "reconnect, read the option bytes back", self.check),
        ]

    def run(self):
        check_image(self.image)
        plan = self.steps()
        try:
            for i, (name, what, fn) in enumerate(plan, 1):
                self.log("[%d/%d] %s: %s" % (i, len(plan), name, what))
                note = fn()
                if note:
                    self.log("      " + note)
        finally:
            self.chip.close()
        self.result["ok"] = True
        return self.result

    # helpers
    def wait_idle(self, what):
        end = time.monotonic() + self.timeout_s
        while True:
            sr = self.chip.read32(FLASH_SR)
            if not sr & (SR_BSY1 | SR_CFGBSY):
                break
            if time.monotonic() > end:
                raise ProgramError("%s: the flash stayed busy (FLASH_SR 0x%08x)" % (what, sr))
        if sr & SR_ERRORS & ~SR_OPTVERR:
            raise ProgramError("%s: FLASH_SR 0x%08x (an error flag)" % (what, sr))
        return sr

    def refresh_iwdg(self):
        if self.iwdg_tamed:
            self.chip.write32(IWDG_KR, IWDG_KEY_RELOAD)

    # steps
    def connect(self):
        self.chip.connect("under-reset")

    def identify(self):
        dev = self.chip.read32(DBG_IDCODE) & 0xFFF
        if dev != CHIP_ID:
            raise ProgramError("DEV_ID 0x%03x: not an STM32G030 (0x466)" % dev)
        kb = self.chip.read32(FLASHSIZE_BASE) & 0xFFFF
        if kb * 1024 != PAGES * PAGE_SIZE:
            raise ProgramError("%d KB of flash: not the G030K8's 64 KB" % kb)
        uid = [self.chip.read32(UID_BASE + 4 * i) for i in range(3)]
        self.result["uid"] = format_uid(uid)
        return "uid %s" % self.result["uid"]

    def protection(self):
        optr = self.chip.read32(FLASH_OPTR)
        self.result["optr_before"] = optr
        if rdp_level(optr) != 0:
            raise ProgramError("read-out protection is at level %d (FLASH_OPTR 0x%08x): this tool does not "
                               "regress it (that mass-erases the whole flash, pages 30 and 31 included)" % (rdp_level(optr), optr))
        return "FLASH_OPTR " + describe_optr(optr)

    def watchdog(self):
        if self.result["optr_before"] & OPTR_IWDG_SW:
            return "IWDG in software mode: not running"
        # UNVERIFIED on the chip: the freeze bit, and that the 0.5 s from the reset are not used up
        # before this step runs (connecting takes a few ms).
        self.chip.write32(RCC_APBENR1, self.chip.read32(RCC_APBENR1) | RCC_APBENR1_DBGEN)
        self.chip.write32(DBG_APB_FZ1, self.chip.read32(DBG_APB_FZ1) | DBG_IWDG_STOP)
        self.chip.write32(IWDG_KR, IWDG_KEY_UNLOCK)
        self.chip.write32(IWDG_PR, IWDG_PR_256)
        self.chip.write32(IWDG_RLR, IWDG_RLR_MAX)
        self.chip.write32(IWDG_KR, IWDG_KEY_RELOAD)
        self.iwdg_tamed = True
        return "IWDG started by hardware: frozen while halted, about 32 s, refreshed between pages"

    def saved_pages(self):
        saved = self.chip.read_bytes(FLASH_BASE + SAVED_FIRST * PAGE_SIZE, (PAGES - SAVED_FIRST) * PAGE_SIZE)
        blank = saved == b"\xff" * len(saved)
        self.result["saved_blank"] = blank
        if self.keep_saved:
            self.mass = False
            return "kept (%s)" % ("blank" if blank else "holding data")
        if blank or self.wipe_saved:
            self.mass = True
            return "blank: a mass erase" if blank else "holding data: WIPED as asked (--wipe-saved)"
        raise ProgramError("pages 30 and 31 hold saved data (parameters, plug record): pass --keep-saved to keep "
                           "them, or --wipe-saved to erase them with the rest")

    def unlock(self):
        if self.chip.read32(FLASH_CR) & CR_LOCK:
            self.chip.write32(FLASH_KEYR, FLASH_KEY1)
            self.chip.write32(FLASH_KEYR, FLASH_KEY2)
            if self.chip.read32(FLASH_CR) & CR_LOCK:
                raise ProgramError("FLASH_CR stayed locked")
        self.chip.write32(FLASH_SR, SR_ERRORS | SR_EOP)

    def erase(self):
        self.unlock()
        self.wait_idle("before the erase")
        if self.mass:
            self.result["erase"] = "mass"
            self.chip.write32(FLASH_CR, CR_MER1)
            self.chip.write32(FLASH_CR, CR_MER1 | CR_STRT)
            self.wait_idle("mass erase")
            self.chip.write32(FLASH_CR, 0)
            return "mass erase: every page, 30 and 31 included"
        pages = list(range(image_pages(self.image)))
        self.result["erase"] = "pages 0-%d" % (pages[-1])
        for p in pages:
            assert p < SAVED_FIRST
            self.refresh_iwdg()
            self.chip.write32(FLASH_CR, CR_PER | p << CR_PNB_SHIFT)
            self.chip.write32(FLASH_CR, CR_PER | p << CR_PNB_SHIFT | CR_STRT)
            self.wait_idle("page %d erase" % p)
        self.chip.write32(FLASH_CR, 0)
        return "pages 0-%d erased, 30 and 31 untouched" % pages[-1]

    def program(self):
        data = self.image + b"\xff" * (-len(self.image) % 8)
        n = image_pages(self.image)
        order = list(range(1, n)) + [0]
        self.chip.write32(FLASH_CR, CR_PG)
        words = 0
        for p in order:
            self.refresh_iwdg()
            for off in range(p * PAGE_SIZE, min((p + 1) * PAGE_SIZE, len(data)), 8):
                w0, w1 = struct.unpack_from("<II", data, off)
                if w0 == w1 == 0xFFFFFFFF:
                    continue  # erased already
                self.chip.write_words(FLASH_BASE + off, (w0, w1))
                self.wait_idle("programming 0x%08x" % (FLASH_BASE + off))
                words += 1
        self.chip.write32(FLASH_CR, 0)
        return "%d bytes in %d pages, %d double words (page 0 last)" % (len(self.image), n, words)

    def verify(self):
        n = image_pages(self.image)
        got = b""
        for p in range(n):
            self.refresh_iwdg()
            got += self.chip.read_bytes(FLASH_BASE + p * PAGE_SIZE, PAGE_SIZE)
        want = self.image + b"\xff" * (n * PAGE_SIZE - len(self.image))
        if got != want:
            bad = next(i for i in range(len(want)) if got[i] != want[i])
            raise ProgramError("verify: 0x%08x reads 0x%02x, the image has 0x%02x" % (FLASH_BASE + bad, got[bad], want[bad]))
        return "%d bytes match" % len(self.image)

    def options(self):
        before = self.chip.read32(FLASH_OPTR)
        want = optr_required(before, self.rdp_level1)
        self.result["optr_wanted"] = want
        if self.skip_options:
            self.result["optr_wanted"] = before
            return "skipped (--skip-options): FLASH_OPTR stays " + describe_optr(before)
        self.chip.write32(FLASH_OPTKEYR, FLASH_OPTKEY1)
        self.chip.write32(FLASH_OPTKEYR, FLASH_OPTKEY2)
        if self.chip.read32(FLASH_CR) & CR_OPTLOCK:
            raise ProgramError("FLASH_CR's OPTLOCK stayed set")
        sr = self.chip.read32(FLASH_SR)
        if want == before and not sr & SR_OPTVERR:
            return "already " + describe_optr(before)
        if self.rdp_level1:
            self.log("      WARNING: read-out protection level 1: SWD and the ROM bootloader can no longer read "
                     "the flash, the app can no longer update this MCU, and level 0 again erases everything")
        self.wait_idle("before the option bytes")
        self.chip.write32(FLASH_OPTR, want)
        self.chip.write32(FLASH_CR, CR_OPTSTRT)
        self.wait_idle("option bytes")
        got = self.chip.read32(FLASH_OPTR)
        if got != want:
            raise ProgramError("FLASH_OPTR reads 0x%08x after the write, not 0x%08x" % (got, want))
        return "%s -> %s" % ("0x%08x" % before, describe_optr(want))

    def launch(self):
        if self.chip.read32(FLASH_CR) & CR_OPTLOCK:  # --skip-options: unlocked here for the load
            self.chip.write32(FLASH_OPTKEYR, FLASH_OPTKEY1)
            self.chip.write32(FLASH_OPTKEYR, FLASH_OPTKEY2)
        try:
            self.chip.write32_reset(FLASH_CR, CR_OBL_LAUNCH)
        except Disconnected:
            pass
        self.chip.close()
        time.sleep(0.2 if isinstance(self.chip, SwdChip) else 0)
        return "the option bytes reload; the chip restarts in the new image"

    def check(self):
        self.chip.connect("attach")  # no reset, no halt: the image runs (and refreshes the watchdog)
        optr = self.chip.read32(FLASH_OPTR)
        sr = self.chip.read32(FLASH_SR)
        area = [self.chip.read32(OPTION_BASE), self.chip.read32(OPTION_BASE + 4)]
        self.result.update(optr=optr, optverr=bool(sr & SR_OPTVERR), option_area=area)
        want = self.result["optr_wanted"]
        if sr & SR_OPTVERR:
            raise ProgramError("OPTVERR: the option bytes did not load (FLASH_OPTR 0x%08x)" % optr)
        if area[1] != ~area[0] & 0xFFFFFFFF:
            raise ProgramError("the option area at 0x%08x reads 0x%08x / 0x%08x: not value and complement" %
                               (OPTION_BASE, area[0], area[1]))
        if optr != want:
            raise ProgramError("FLASH_OPTR loaded as 0x%08x, not 0x%08x" % (optr, want))
        if not self.skip_options and not wdg1_ok(optr, sr):
            raise ProgramError("FLASH_OPTR 0x%08x is not the WDG-1 set" % optr)
        if rdp_level(optr) == 0:
            head = self.chip.read_bytes(FLASH_BASE, 8)
            if head != self.image[:8]:
                raise ProgramError("after the reset the vector table reads %s" % head.hex())
        return "FLASH_OPTR " + describe_optr(optr) + ", loaded cleanly" + (
            ", WDG-1 ok" if wdg1_ok(optr, sr) else ", NOT the WDG-1 set (--skip-options)")


def program_board(image, chip=None, dry_run=False, log=print, **kw):
    """Program and check a board; returns the result dict or raises ProgramError. With dry_run, against
    a simulated factory-fresh chip, printing every register write."""
    if dry_run:
        chip = SimulatedG030(trace=log)
        log("DRY RUN: no probe; a simulated factory-fresh STM32G030K8")
    elif chip is None:
        chip = SwdChip(kw.pop("probe", None), kw.pop("frequency", None))
    kw.pop("probe", None)
    kw.pop("frequency", None)
    return Programmer(chip, image, log=log, **kw).run()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="the image: stm32/build/safety.bin or bringup.bin")
    ap.add_argument("--dry-run", action="store_true", help="print every step against a simulated chip, no probe")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--keep-saved", action="store_true", help="erase only the image's pages; keep pages 30 and 31")
    g.add_argument("--wipe-saved", action="store_true", help="mass erase even when pages 30 and 31 hold data")
    ap.add_argument("--skip-options", action="store_true", help="leave FLASH_OPTR as it is (the chip still reloads it)")
    ap.add_argument("--rdp-level1", action="store_true",
                    help="set read-out protection level 1. NOT for normal use: SWD and the ROM bootloader can then no "
                         "longer read the flash, the ESP32 can no longer update the MCU, and level 0 again erases all")
    ap.add_argument("--probe", help="the probe's unique id (pyocd list), when more than one is connected")
    ap.add_argument("--frequency", type=int, help="SWD clock in Hz (pyocd's default otherwise)")
    args = ap.parse_args(argv)
    with open(args.image, "rb") as f:
        image = f.read()
    try:
        r = program_board(image, dry_run=args.dry_run, keep_saved=args.keep_saved, wipe_saved=args.wipe_saved,
                          rdp_level1=args.rdp_level1, skip_options=args.skip_options, probe=args.probe,
                          frequency=args.frequency)
    except ProgramError as e:
        print("FAILED: %s" % e)
        return 1
    print("done: %s, uid %s, FLASH_OPTR 0x%08x" % (args.image, r.get("uid"), r["optr"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
