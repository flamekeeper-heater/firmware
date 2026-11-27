"""factory_test.py end to end against a fake board (fake_console.py): a good board through every
step and the production flash (simulated by program.py's SimulatedG030), a board failing a step that
does not stop the test, boards failing safety steps that do, the reports, and the program itself on a
pseudo-terminal.

    python3 -I -B -m unittest discover -s stm32/tools/tests
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fake_console as fc  # noqa: E402
import factory_test as ft  # noqa: E402
import program  # noqa: E402
import bringup  # noqa: E402

TOOLS = os.path.join(HERE, "..")


def config(**changes):
    """factory.toml, shortened for the tests' clock."""
    cfg, sha = ft.load_config(os.path.join(TOOLS, "factory.toml"))
    cfg["_dir"] = TOOLS
    cfg["boot"]["wait_s"] = 3.0
    cfg["clock"]["seconds"] = 0.5
    cfg["fan"]["settle_s"] = 0.2
    cfg["interlocks"]["hb_s"] = 1
    for key, value in changes.items():
        section, name = key.split("__")
        cfg[section][name] = value
    return cfg, sha


def image():
    data = bytearray(3000)
    data[0:8] = (0x20002000).to_bytes(4, "little") + (0x08000101).to_bytes(4, "little")
    return bytes(data)


class Run:
    """One factory test against a FakeBoard, in process; the production flash on a SimulatedG030
    that turns the fake board's image into the production one."""

    def __init__(self, board, flash="real", cfg=None, skip=()):
        self.board = board
        self.dir = tempfile.mkdtemp(prefix="fk_factory_")
        self.chip = program.SimulatedG030(optr=0xDEFEE1AA, uid=fc.UID)
        self.chip.flash[:4] = b"\x00\x20\x00\x20"  # the bring-up image is on
        cfg, sha = cfg or config()
        self.image = image()
        cfg["production"]["image"] = os.path.join(self.dir, "safety.bin")
        with open(cfg["production"]["image"], "wb") as f:
            f.write(self.image)
        session = bringup.Session(fc.FakePort(board), echo=False)
        self.lines = []
        self.test = ft.FactoryTest(session, cfg, flasher=self.flasher, flash=flash, log=self.lines.append)
        started = ft.datetime.datetime.now(ft.datetime.timezone.utc)
        self.outcome = self.test.run(skip=set(skip))
        self.base = ft.write_report(self.dir, self.test, self.outcome, "factory.toml", sha, started, started)
        with open(self.base + ".json") as f:
            self.report = json.load(f)
        with open(self.base + ".txt") as f:
            self.text = f.read()

    def flasher(self, img, log, keep):
        r = program.program_board(img, chip=self.chip, log=log, keep_saved=keep, wipe_saved=not keep)
        self.board.image = "safety"
        return r

    def step(self, sid):
        return next(s for s in self.report["steps"] if s["id"] == sid)

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


class Link(unittest.TestCase):
    def test_crc_and_frames(self):
        self.assertEqual(ft.crc16(b"123456789"), 0x4B37)  # CRC-16/MODBUS check value
        seq, f = ft.hello_frame()
        self.assertEqual(f[0], ft.LINK_REQUEST)
        self.assertEqual(f[1] + 2, len(f))
        self.assertEqual(f[3], ft.LINK_OP_HELLO)
        self.assertNotIn(b"\n", f)
        self.assertNotIn(b"\r", f)
        payload = bytes([1, 2, 0, 1, 0, 0, 0, 0, 0, 30, 2, 0]) + b"Flamekeeper bring-up 0.1.0"
        reply = ft.build_frame(ft.LINK_REPLY, seq, ft.LINK_OP_HELLO, payload, 0)
        got, rest = ft.find_reply(b"ok\r\n" + reply[:1] + b"\x00" + reply + b"* x\r\n", seq, ft.LINK_OP_HELLO)
        self.assertEqual(got, reply)
        self.assertEqual(rest, b"ok\r\n" + reply[:1] + b"\x00* x\r\n")
        info = ft.decode_info(got[5:-2])
        self.assertEqual((info["major"], info["kind"], info["reset_reason"], info["text"]),
                         (1, 2, 2, "Flamekeeper bring-up 0.1.0"))
        self.assertEqual(ft.find_reply(reply[:-1], seq, 1)[0], None)  # cut short

    def test_frame_matches_the_c_layout(self):
        # link_build_request: header, length (without the CRC), seq, op, payload, CRC high byte first.
        f = ft.build_frame(ft.LINK_REQUEST, 9, ft.LINK_OP_HELLO, bytes([1]))
        self.assertEqual(f[:5], bytes([0x7A, 5, 9, 1, 1]))
        c = ft.crc16(f[:5])
        self.assertEqual(f[5:], bytes([c >> 8, c & 0xFF]))


class Config(unittest.TestCase):
    def test_default_config_has_every_key(self):
        cfg, _ = ft.load_config(os.path.join(TOOLS, "factory.toml"))
        for section in ("boot", "options", "watchdog", "clock", "adc", "interlocks", "fan", "glow", "pump", "pump_oc",
                        "co", "link", "production"):
            self.assertIn(section, cfg)
        self.assertEqual(cfg["options"]["optr"], program.optr_required(program.OPTR_FACTORY))
        self.assertEqual(set(cfg["adc"]), {"vin", "vref", "ntc", "board", "glow_v", "pump_v", "fan_i", "pump_i",
                                           "glow_i", "co", "hall_rpm"})

    def test_json_config(self):
        cfg, _ = ft.load_config(os.path.join(TOOLS, "factory.toml"))
        d = tempfile.mkdtemp()
        try:
            path = os.path.join(d, "c.json")
            with open(path, "w") as f:
                json.dump(cfg, f)
            self.assertEqual(ft.load_config(path)[0], cfg)
        finally:
            shutil.rmtree(d)


class EndToEnd(unittest.TestCase):
    def test_good_board(self):
        r = Run(fc.FakeBoard())
        try:
            self.assertEqual(r.outcome, {"result": "pass", "stopped_at": None}, r.lines)
            self.assertEqual([s["result"] for s in r.report["steps"]], ["pass"] * len(ft.STEPS))
            self.assertEqual(r.report["uid"], "00350027-4d4b5009-20393650")
            self.assertTrue(os.path.basename(r.base).startswith("00350027-4d4b5009-20393650_"))
            self.assertEqual(r.report["programmed"]["erase"], "mass")
            self.assertEqual(r.report["programmed"]["optr"], 0xDEFEE1AA)
            self.assertEqual(bytes(r.chip.flash[:len(r.image)]), r.image)
            self.assertIn("Flamekeeper safety", " ".join(r.step("production")["notes"]))
            self.assertIn("Result     PASS", r.text)
            with open(r.base + ".log") as f:
                log = f.read()
            self.assertIn("send wdg stop", log)
            self.assertIn("HELLO reply 7b", log)
            # The order of risk: nothing with a load before the watchdog and the interlocks passed.
            cmds = r.board.commands
            self.assertLess(cmds.index("wdg stop"), cmds.index("hb 1"))
            self.assertLess(cmds.index("precheck"), cmds.index("fan rpm 1500 30"))
            self.assertLess(cmds.index("fan rpm 1500 30"), cmds.index("glow"))
            self.assertLess(cmds.index("glow"), cmds.index("fan off"))
            self.assertEqual(cmds[-1], "co")  # then the link, and the production image (no console)
            # Every window used is in the report.
            self.assertEqual(r.report["config"]["values"]["adc"]["vin"], [11.7, 12.3])
        finally:
            r.close()

    def test_failing_step_that_does_not_stop(self):
        r = Run(fc.FakeBoard(hello_text="Flamekeeper safety 0.1.0"))  # the wrong image on the board
        try:
            self.assertEqual(r.outcome["result"], "fail")
            self.assertIsNone(r.outcome["stopped_at"])
            self.assertEqual(r.step("link")["result"], "fail")
            self.assertEqual(r.step("co")["result"], "pass")
            self.assertEqual(r.step("production")["result"], "not run")
            self.assertIsNone(r.report["programmed"])
            self.assertEqual(r.chip.resets, 0)  # never touched
            self.assertIn("FAIL text", r.text)
        finally:
            r.close()

    def test_safety_failures_stop(self):
        cases = [
            (dict(oc_cuts=False), "pump_oc"),
            (dict(optr=0xDFFFE1AA), "options"),
            (dict(wdg_works=False), "watchdog"),
            (dict(clock_skew=1.05), "clock"),
            (dict(adc=dict(fc.DEFAULTS["adc"], ntc=None)), "adc"),  # the NTC open
            (dict(fan_stall=True), "fan"),
            (dict(glow=dict(fc.DEFAULTS["glow"], r_mohm=1600, cold_type=24)), "glow"),  # a 24 V plug on the fixture
            (dict(co_state="sensor_fault"), "co"),
            (dict(powerup="failed"), "boot"),
        ]
        for knobs, where in cases:
            with self.subTest(where=where):
                r = Run(fc.FakeBoard(**knobs))
                try:
                    self.assertEqual(r.outcome, {"result": "fail", "stopped_at": where}, r.lines)
                    i = ft.STEPS.index(where)
                    self.assertEqual([s["result"] for s in r.report["steps"][:i]], ["pass"] * i)
                    self.assertEqual({s["result"] for s in r.report["steps"][i + 1:]}, {"not run"})
                    self.assertEqual(r.chip.resets, 0)
                    self.assertEqual(r.board.commands[-1], "stop")
                    self.assertIn("(stopped at %s)" % where, r.text)
                finally:
                    r.close()

    def test_no_flash_and_skips(self):
        r = Run(fc.FakeBoard(), flash="none")
        try:
            self.assertEqual(r.outcome["result"], "pass")
            self.assertEqual(r.step("production")["result"], "skipped")
            self.assertEqual(r.chip.resets, 0)
        finally:
            r.close()
        r = Run(fc.FakeBoard(), skip=("co",))
        try:
            self.assertEqual(r.outcome["result"], "incomplete")
            self.assertEqual(r.step("production")["result"], "not run")
        finally:
            r.close()

    def test_no_boot_line(self):
        cfg = config(boot__wait_s=0.3)
        r = Run(fc.FakeBoard(boot_delay=5.0), cfg=cfg)
        try:
            self.assertEqual(r.outcome["stopped_at"], "boot")
            self.assertTrue(os.path.basename(r.base).startswith("unknown-uid_"))
        finally:
            r.close()


@unittest.skipUnless(hasattr(os, "openpty"), "needs a pseudo-terminal")
class OverPty(unittest.TestCase):
    def test_program_on_a_pty(self):
        board = fc.FakeBoard(boot_delay=0.8)
        path, close = fc.serve_pty(board)
        d = tempfile.mkdtemp(prefix="fk_factory_pty_")
        try:
            cfg, _ = config()
            cfg_path = os.path.join(d, "fixture.json")
            with open(cfg_path, "w") as f:
                json.dump({k: v for k, v in cfg.items() if not k.startswith("_")}, f)
            p = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "factory_test.py"), path, "--config", cfg_path,
                                "--no-flash", "--quiet", "--reports", d], capture_output=True, text=True, timeout=60)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn("00350027-4d4b5009-20393650: PASS", p.stdout)
            reports = [n for n in os.listdir(d) if n.endswith(".json") and n != "fixture.json"]
            self.assertEqual(len(reports), 1)
            with open(os.path.join(d, reports[0])) as f:
                rep = json.load(f)
            self.assertEqual(rep["result"], "pass")
            self.assertEqual(rep["flash"], "none")
        finally:
            close()
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
