"""program.py on the host: its chip facts against the firmware's headers, the option-byte rule, and
the whole step plan against the simulated G030 (blank, re-flash, saved pages, protection, a bad
option load).

    python3 -I -B -m unittest discover -s stm32/tools/tests
"""
import contextlib
import io
import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import program as p  # noqa: E402

ROOT = os.path.join(HERE, "..", "..", "..")


def read(path):
    with open(path) as f:
        return f.read()


def defines(path):
    out = {}
    for m in re.finditer(r"^#define (\w+) \(?(0x[0-9A-Fa-f]+|\d+)u?\)?(?: << (\d+)\))?", read(path), re.M):
        v = int(m.group(2), 0)
        out[m.group(1)] = v << int(m.group(3)) if m.group(3) else v
    return out


def st_defines(*paths):
    """The numeric #defines of C headers, ST's style included (FOO_Msk (0x1UL << FOO_Pos), sums of
    bases): each name's value, where its expression is integer arithmetic on numbers and names."""
    raw = {}
    for path in paths:
        text = re.sub(r"\\\n", " ", read(path))  # continued lines
        for m in re.finditer(r"^#define (\w+)[ \t]+([^\n]*?)\s*(?:/\*.*)?$", text, re.M):
            raw[m.group(1)] = m.group(2)
    out = {}

    def value(name, depth=0):
        if name in out or depth > 20 or name not in raw:
            return out.get(name)
        e = re.sub(r"\((?:const )?(?:volatile )?u?int\d+_t ?\*?\)", "", raw[name])
        e = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)[uU][lL]?\b", r"\1", e)
        for ref in set(re.findall(r"\b[A-Za-z_]\w*\b", e)):
            v = value(ref, depth + 1)
            if v is None:
                return None
            e = re.sub(r"\b%s\b" % ref, str(v), e)
        if not re.fullmatch(r"[0-9xXa-fA-F ()+\-*<>|&~]*", e):
            return None
        try:
            out[name] = eval(e, {"__builtins__": {}}) & 0xFFFFFFFF
        except (SyntaxError, TypeError, ValueError):
            return None  # not a constant (a memory read, a cast this does not know)
        return out[name]

    for name in raw:
        value(name)
    return out


def image(n_bytes=5000, seed=1):
    body = bytearray((i * 31 + seed) & 0xFF for i in range(n_bytes))
    struct.pack_into("<II", body, 0, 0x20002000, 0x08000101)
    return bytes(body)


def quiet():
    lines = []
    return lines, lines.append


class Facts(unittest.TestCase):
    """The same numbers as the firmware's chip map and register header."""

    def test_against_g030_map(self):
        d = defines(os.path.join(ROOT, "common", "g030", "g030_map.h"))
        self.assertEqual(d["G030_FLASH_BASE"], p.FLASH_BASE)
        self.assertEqual(d["G030_PAGE_SIZE"], p.PAGE_SIZE)
        self.assertEqual(d["G030_PAGES"], p.PAGES)
        self.assertEqual(d["G030_APP_PAGES"], p.APP_PAGES)
        self.assertEqual(d["G030_SAVED_PAGE"], p.SAVED_FIRST)
        self.assertEqual(d["G030_RAM_BASE"], p.RAM_BASE)
        self.assertEqual(d["G030_RAM_END"], p.RAM_END)
        self.assertEqual(d["G030_CHIP_ID"], p.CHIP_ID)
        self.assertEqual(d["G030_OPTION_BASE"], p.OPTION_BASE)
        self.assertEqual(d["G030_OPTR_RDP"], p.OPTR_RDP)
        self.assertEqual(d["G030_RDP_LEVEL_0"], p.RDP_LEVEL_0)
        for name in ("IWDG_SW", "IWDG_STOP", "IWDG_STDBY", "NBOOT_SEL", "NBOOT1", "NBOOT0"):
            self.assertEqual(d["G030_OPTR_" + name], getattr(p, "OPTR_" + name), name)

    def test_against_register_header(self):
        # The driver's registers: ST's device header, and what it leaves out (g030_fields.h).
        d = st_defines(os.path.join(ROOT, "stm32", "hal", "st", "stm32g030xx.h"),
                       os.path.join(ROOT, "stm32", "hal", "g030_fields.h"))
        self.assertEqual(d["FLASH_R_BASE"], p.FLASH_R)
        self.assertEqual(d["IWDG_BASE"], p.IWDG_KR)
        self.assertEqual(d["UID_BASE"], p.UID_BASE)
        self.assertEqual(d["DBG_BASE"], p.DBG_IDCODE)
        self.assertEqual(d["DBG_APB_FZ1_DBG_IWDG_STOP"], p.DBG_IWDG_STOP)
        self.assertEqual(d["FLASH_KEY1"], p.FLASH_KEY1)
        self.assertEqual(d["FLASH_KEY2"], p.FLASH_KEY2)
        for name, ours in (("FLASH_SR_BSY1", p.SR_BSY1), ("FLASH_SR_CFGBSY", p.SR_CFGBSY),
                           ("FLASH_SR_OPTVERR", p.SR_OPTVERR), ("FLASH_CR_PG", p.CR_PG), ("FLASH_CR_PER", p.CR_PER),
                           ("FLASH_CR_STRT", p.CR_STRT), ("FLASH_CR_LOCK", p.CR_LOCK),
                           ("FLASH_CR_PNB_Pos", p.CR_PNB_SHIFT), ("FLASH_CR_OPTSTRT", p.CR_OPTSTRT),
                           ("FLASH_CR_OBL_LAUNCH", p.CR_OBL_LAUNCH), ("FLASH_CR_OPTLOCK", p.CR_OPTLOCK),
                           ("FLASH_OPTR_IWDG_SW", p.OPTR_IWDG_SW), ("FLASH_OPTR_nBOOT_SEL", p.OPTR_NBOOT_SEL),
                           ("IWDG_KR_KEY_UNLOCK", p.IWDG_KEY_UNLOCK), ("IWDG_KR_KEY_RELOAD", p.IWDG_KEY_RELOAD)):
            self.assertEqual(d[name], ours, name)
        self.assertEqual(d["FLASH_SR_ERRORS"], p.SR_ERRORS)

    def test_console_uid_address(self):
        h = read(os.path.join(ROOT, "stm32", "bringup", "console.h"))
        self.assertIn("#define CONSOLE_UID_ADDR 0x%08Xu" % p.UID_BASE, h)


class OptionBytes(unittest.TestCase):
    def test_factory_value(self):
        self.assertEqual(p.optr_required(p.OPTR_FACTORY), 0xDEFEE1AA)  # BRINGUP step 3
        self.assertFalse(p.wdg1_ok(p.OPTR_FACTORY))
        self.assertTrue(p.wdg1_ok(0xDEFEE1AA))
        self.assertFalse(p.wdg1_ok(0xDEFEE1AA, p.SR_OPTVERR))

    def test_keeps_every_other_bit(self):
        for optr in (0, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF, 0x00F0E1AA):
            want = p.optr_required(optr)
            self.assertTrue(p.wdg1_ok(want))
            self.assertEqual(want & ~p.WDG1_CHECKED & 0xFFFFFFFF, optr & ~p.WDG1_CHECKED & 0xFFFFFFFF)
            self.assertEqual(p.optr_required(want), want)  # idempotent
            self.assertEqual(want & 0xFF, optr & 0xFF)  # RDP untouched

    def test_rdp_only_when_asked(self):
        self.assertEqual(p.rdp_level(p.optr_required(p.OPTR_FACTORY)), 0)
        v = p.optr_required(p.OPTR_FACTORY, rdp_level1=True)
        self.assertEqual(v & 0xFF, p.RDP_LEVEL_1)
        self.assertEqual(p.rdp_level(v), 1)
        self.assertNotEqual(v & 0xFF, p.RDP_LEVEL_2)


class Plan(unittest.TestCase):
    def run_on(self, chip, img, **kw):
        lines, log = quiet()
        r = p.Programmer(chip, img, log=log, **kw).run()
        return r, lines

    def test_order(self):
        names = [s[0] for s in p.Programmer(p.SimulatedG030(), image()).steps()]
        self.assertEqual(names, ["connect", "identify", "protection", "watchdog", "saved pages", "erase", "program",
                                 "verify", "options", "launch", "check"])

    def test_blank_board(self):
        chip, img = p.SimulatedG030(), image(5000)
        r, lines = self.run_on(chip, img)
        self.assertEqual(chip.mass_erases, 1)
        self.assertEqual(bytes(chip.flash[:len(img)]), img)
        self.assertEqual(bytes(chip.flash[len(img):]), b"\xff" * (len(chip.flash) - len(img)))
        self.assertEqual(chip.programmed[-1] // p.PAGE_SIZE, 0)  # page 0 last
        self.assertGreater(chip.programmed[0], p.PAGE_SIZE - 1)
        self.assertEqual(chip.optr, 0xDEFEE1AA)
        self.assertEqual(chip.option_area, [0xDEFEE1AA, 0x21011E55])
        self.assertEqual(chip.resets, 1)  # OBL_LAUNCH
        self.assertEqual(r["uid"], "00350027-4d4b5009-20393650")
        self.assertEqual(r["erase"], "mass")
        self.assertTrue(r["ok"])
        self.assertFalse(any(a == p.IWDG_KR for a, _ in chip.writes))  # software IWDG: left alone
        self.assertTrue(any("[11/11] check" in l for l in lines))

    def test_reflash_keeps_saved_pages(self):
        chip = p.SimulatedG030()
        self.run_on(chip, image(9000, 1))
        chip.flash[30 * p.PAGE_SIZE:30 * p.PAGE_SIZE + 4] = b"\x07\x0e\x15\x1c"  # saved parameters
        chip.flash[31 * p.PAGE_SIZE + 256:31 * p.PAGE_SIZE + 258] = b"R0"
        saved = bytes(chip.flash[30 * p.PAGE_SIZE:])
        with self.assertRaisesRegex(p.ProgramError, "--keep-saved"):
            self.run_on(chip, image(3000, 2))
        img = image(3000, 2)
        before = len(chip.writes)
        r, _ = self.run_on(chip, img, keep_saved=True)
        self.assertEqual(chip.erased_pages, [0, 1])
        self.assertEqual(chip.mass_erases, 1)  # only the first
        self.assertEqual(bytes(chip.flash[30 * p.PAGE_SIZE:]), saved)
        self.assertEqual(bytes(chip.flash[:len(img)]), img)
        self.assertEqual(bytes(chip.flash[len(img):2 * p.PAGE_SIZE]), b"\xff" * (2 * p.PAGE_SIZE - len(img)))
        # The option bytes were right: not written again, but still reloaded (OBL_LAUNCH).
        self.assertFalse(any(a == p.FLASH_OPTR for a, _ in chip.writes[before:]))
        self.assertEqual(chip.resets, 2)
        # The IWDG ran by hardware: frozen, slowed, refreshed.
        kr = [v for a, v in chip.writes if a == p.IWDG_KR]
        self.assertIn(p.IWDG_KEY_UNLOCK, kr)
        self.assertGreaterEqual(kr.count(p.IWDG_KEY_RELOAD), 3)
        self.assertIn((p.IWDG_PR, p.IWDG_PR_256), chip.writes)
        self.assertIn((p.IWDG_RLR, p.IWDG_RLR_MAX), chip.writes)
        self.assertTrue(any(a == p.DBG_APB_FZ1 and v & p.DBG_IWDG_STOP for a, v in chip.writes))
        # Wiped when asked.
        self.run_on(chip, img, wipe_saved=True)
        self.assertEqual(chip.mass_erases, 2)
        self.assertEqual(bytes(chip.flash[30 * p.PAGE_SIZE:]), b"\xff" * (2 * p.PAGE_SIZE))
        with self.assertRaises(p.ProgramError):
            p.Programmer(chip, img, keep_saved=True, wipe_saved=True)

    def test_refusals(self):
        with self.assertRaisesRegex(p.ProgramError, "level 1"):
            self.run_on(p.SimulatedG030(optr=0xDEFEE1BB), image())
        with self.assertRaisesRegex(p.ProgramError, "DEV_ID"):
            self.run_on(p.SimulatedG030(dev_id=0x460), image())
        with self.assertRaisesRegex(p.ProgramError, "pages 0-29"):
            self.run_on(p.SimulatedG030(), image(30 * p.PAGE_SIZE + 1))
        esp = bytearray(image())
        struct.pack_into("<I", esp, 0, 0x403C0000)
        with self.assertRaisesRegex(p.ProgramError, "stack pointer"):
            self.run_on(p.SimulatedG030(), bytes(esp))
        with self.assertRaisesRegex(p.ProgramError, "empty"):
            self.run_on(p.SimulatedG030(), b"")

    def test_rdp_level1_on_request(self):
        chip = p.SimulatedG030()
        r, lines = self.run_on(chip, image(), rdp_level1=True)
        self.assertEqual(p.rdp_level(chip.optr), 1)
        self.assertTrue(p.wdg1_ok(chip.optr))
        self.assertTrue(any("WARNING" in l for l in lines))
        with self.assertRaises(p.ProgramError):  # the flash is no longer readable
            chip.connect("attach")
            chip.read_bytes(p.FLASH_BASE, 4)

    def test_skip_options(self):
        chip = p.SimulatedG030()
        r, _ = self.run_on(chip, image(), skip_options=True)
        self.assertEqual(chip.optr, p.OPTR_FACTORY)
        self.assertEqual(chip.resets, 1)  # reloaded all the same (the EMPTY flag)

    def test_bad_option_load(self):
        chip = p.SimulatedG030()

        def corrupt(c):  # a word / complement mismatch at the load, as after a cut write (WDG-7)
            if c.resets:
                c.option_area[1] ^= 1
                c.optr = 0xFFFFFF00 | p.RDP_LEVEL_1
                c.sr |= p.SR_OPTVERR

        chip.fail_after_reset = corrupt
        with self.assertRaisesRegex(p.ProgramError, "OPTVERR"):
            self.run_on(chip, image())

    def test_dry_run_cli(self):
        path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "fk_program_test_%d.bin" % os.getpid())
        with open(path, "wb") as f:
            f.write(image(7000))
        try:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = p.main(["--dry-run", path])
            text = out.getvalue()
        finally:
            os.remove(path)
        self.assertEqual(rc, 0)
        for name in ("connect", "identify", "protection", "watchdog", "saved pages", "erase", "program", "verify",
                     "options", "launch", "check"):
            self.assertIn("] %s:" % name, text)
        self.assertIn("FLASH_OPTR <- 0xdefee1aa", text)
        self.assertIn("FLASH_CR <- 0x08000000", text)  # OBL_LAUNCH
        self.assertIn("mass erase", text)


if __name__ == "__main__":
    unittest.main()
