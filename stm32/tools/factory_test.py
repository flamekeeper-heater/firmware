#!/usr/bin/env python3
"""The safety MCU's factory test: a board running the bring-up image, on the test fixture, through
the bring-up checklist's steps that need no hand measurement; then the production image.

    uv run --with pyocd python3 stm32/tools/factory_test.py /dev/cu.usbserial-XXXX
    python3 stm32/tools/factory_test.py PORT --no-flash               # the test alone
    python3 stm32/tools/factory_test.py PORT --flash-dry-run          # the flash simulated

Program a blank board with the bring-up image first (program.py stm32/build/bringup.bin, which also
sets the option bytes), fit it to the fixture factory.toml describes, connect a 3.3 V USB-UART to
J_DEBUG (pin 1 GND, pin 6 to the adapter's RX, pin 7 to its TX) and the SWD probe, start this tool,
then switch the board's supply on: the first step waits for the boot line.

The steps; "stop" ones end the test at their first failure (nothing with a load runs after a
failed check of what protects it), the others are reported and the test goes on:
  boot        stop  boot line, firmware, reset reason; the power-up self-test passed
  uid         -     the 96-bit UID (`uid`): names the report
  info        stop  no check failing (option bytes, self-test, NTC), the burn off, no stage fault
  options     stop  `opt`: FLASH_OPTR exactly as configured, WDG-1 ok, no OPTVERR
  watchdog    stop  `wdg stop`: a watchdog reset within the window, then the power-up test again
  clock       stop  the MCU's uptime against the host's clock, loop passes a second
  adc         stop  every ADC channel within its window, the Hall speed 0
  interlocks  stop  `io` at rest (latch armed, high sides ok, outputs off, the cut's setting); a
                    heartbeat alone; the pre-start self-test
  fan         stop  the speed loop to its target, the Hall speed and fan current; no fault
  glow        stop  one probe into the dummy plug (inside the fan action): current, resistance,
                    the cold type; `glow read`
  pump        stop  strokes into the dummy coil: done, peak current
  pump_oc     stop  one stroke with the cut lowered: the cut acts; `clear`; the setting back
  co          stop  the CO alarm's state and fitting as configured
  link        -     a binary HELLO frame on the same UART: the reply, the bring-up text, checks 0
  production  -     only if every step passed: program.py with the production image, then HELLO
                    with the production text (--no-flash skips it, --flash-dry-run simulates it)

The report goes to --reports DIR as <uid>_<UTC time>.json, .txt and .log (the console transcript):
the result, each step's checks (value, window, pass or fail), the configuration used and its
SHA-256, the production image's SHA-256 and what program.py read back. Exit code 0 when every step
passed (and the production image is on, unless skipped).

Standard library only (Python 3.11 or later for TOML; a .json configuration works the same), plus
pyocd for the production flash. Every action is a bring-up console command: single, time-limited,
through the firmware's own drivers and interlocks. Ctrl-C sends "stop".
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import struct
import sys
import time

sys.dont_write_bytecode = True  # no __pycache__ beside the tools
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "bringup"))
import program  # noqa: E402

DEFAULT_CONFIG = os.path.join(HERE, "factory.toml")
STEPS = ["boot", "uid", "info", "options", "watchdog", "clock", "adc", "interlocks", "fan", "glow", "pump",
         "pump_oc", "co", "link", "production"]
SAFETY_STEPS = {"boot", "info", "options", "watchdog", "clock", "adc", "interlocks", "fan", "glow", "pump",
                "pump_oc", "co"}

# ---------------------------------------------------------------- the link (common/link/link.h)

LINK_REQUEST, LINK_REPLY = 0x7A, 0x7B
LINK_OP_HELLO = 0x01
LINK_MAJOR = 1
LINK_KIND_STM32 = 2
LINK_FRAME_MAX = 64


def crc16(data):
    """CRC16-Modbus (link_crc16)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_frame(header, seq, op, payload, result=None):
    body = bytes([header, 0, seq, op]) + (bytes([result]) if result is not None else b"") + bytes(payload)
    body = bytes([body[0], len(body)]) + body[2:]
    c = crc16(body)
    return body + bytes([c >> 8, c & 0xFF])


def hello_frame():
    """A HELLO request with no CR or LF in it: the console drops a line holding binary bytes, and a
    newline inside the frame could start a new (garbage) line."""
    for seq in range(1, 256):
        f = build_frame(LINK_REQUEST, seq, LINK_OP_HELLO, bytes([LINK_MAJOR]))
        if b"\n" not in f and b"\r" not in f:
            return seq, f
    raise AssertionError("no clean HELLO frame")


def find_reply(data, seq, op):
    """(frame, the bytes after it) for the first valid reply to seq / op in data, or (None, data)."""
    i = 0
    while True:
        i = data.find(bytes([LINK_REPLY]), i)
        if i < 0 or i + 2 > len(data):
            return None, data
        n = data[i + 1] + 2
        if 7 <= n <= LINK_FRAME_MAX and i + n <= len(data):
            f = data[i:i + n]
            if crc16(f[:-2]) == (f[-2] << 8 | f[-1]) and f[2] == seq and f[3] == op:
                return f, data[:i] + data[i + n:]
        i += 1


def decode_info(payload):
    if len(payload) < 12:
        return None
    caps = struct.unpack_from("<I", payload, 5)[0]
    return {"major": payload[0], "kind": payload[1], "version": "%d.%d.%d" % tuple(payload[2:5]),
            "capabilities": caps, "param_count": payload[9], "reset_reason": payload[10], "checks": payload[11],
            "text": payload[12:].decode("ascii", "replace")}


# ---------------------------------------------------------------- configuration and checks


def load_config(path):
    with open(path, "rb") as f:
        raw = f.read()
    if path.endswith(".json"):
        cfg = json.loads(raw)
    else:
        import tomllib

        cfg = tomllib.loads(raw.decode("utf-8"))
    return cfg, hashlib.sha256(raw).hexdigest()


def kv(line):
    """The key=value words of a console line, as text."""
    return dict(m.groups() for m in re.finditer(r"(\w+)=(\S+)", line))


def num(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


class StepFailed(Exception):
    pass


class Step:
    def __init__(self, sid):
        self.id, self.safety = sid, sid in SAFETY_STEPS
        self.result, self.checks, self.notes, self.seconds = "not run", [], [], 0.0

    def check(self, name, value, window=None, want=None, prefix=None):
        """A value within [lo, hi], equal to want (or in it, for a list), or starting with prefix.
        Returns ok."""
        if prefix is not None:
            ok, spec = isinstance(value, str) and value.startswith(prefix), "starts with " + prefix
        elif window is not None:
            lo, hi = window
            ok = value is not None and lo <= value <= hi
            spec = "%s..%s" % (lo, hi)
        elif isinstance(want, (list, tuple)):
            ok, spec = value in want, "one of " + ", ".join(map(str, want))
        else:
            ok, spec = value == want, "%s" % (want,)
        self.checks.append({"name": name, "value": value, "want": spec, "ok": bool(ok)})
        return ok

    def must(self, name, value, window=None, want=None):
        if not self.check(name, value, window, want):
            raise StepFailed("%s = %s, want %s" % (name, value, self.checks[-1]["want"]))

    def failed(self):
        return any(not c["ok"] for c in self.checks)

    def as_dict(self):
        return {"id": self.id, "safety": self.safety, "result": self.result, "seconds": round(self.seconds, 2),
                "checks": self.checks, "notes": self.notes}


# ---------------------------------------------------------------- the test


class FactoryTest:
    def __init__(self, session, cfg, flasher=None, flash="real", log=print):
        """flash: "real", "dry" (simulated) or "none". flasher(image_bytes, log) programs the board
        (program.program_board by default)."""
        self.s, self.cfg, self.flash, self.log = session, cfg, flash, log
        self.flasher = flasher
        self.uid = None
        self.steps = [Step(sid) for sid in STEPS]
        self.info = {}
        self.programmed = None

    # console helpers
    def cmd(self, line, timeout=10.0):
        ok, lines = self.s.command(line, timeout=timeout, quiet=True)
        if not ok:
            raise StepFailed("`%s` refused or unanswered" % line)
        return lines

    def lines_kv(self, line, prefix):
        out = {}
        for l in self.cmd(line):
            if l.startswith(prefix):
                out.update(kv(l))
        return out

    def event(self, prefix, timeout):
        line = self.s.wait_event(lambda l: l.startswith(prefix), timeout)
        if line is None:
            raise StepFailed("no `%s` within %.1f s" % (prefix, timeout))
        return line

    def read_info(self):
        info = {}
        for l in self.cmd("info"):
            w = l.split()
            if not w:
                continue
            if w[0] == "fw":
                info["fw"] = " ".join(x for x in w[1:] if "=" not in x)
                info.update(kv(l))
            elif w[0] == "reset":
                info["reset"] = w[1]
            elif w[0] == "opt":
                info["opt"], info["opt_ok"] = int(w[1], 16), w[2] == "ok"
            elif w[0] == "checks":
                info["checks"] = int(w[1], 16)
            elif w[0] == "powerup":
                info["powerup"] = w[1]
                info.update(kv(l))
            elif w[0] == "burn":
                info.update(burn=w[1], fault=w[3], why=w[5])
            elif w[0] == "stages":
                info.update(fan_fault=w[2], pump_fault=w[4], glow_fault=w[6], co_state=w[-1])
            elif w[0] == "vdda_mv":
                info["vdda_mv"] = int(w[1])
                info.update(kv(l))
        return info

    def read_adc(self):
        values = {}
        for l in self.cmd("adc"):
            m = re.match(r"adc (\w+) raw=(\d+) mv=(-?\d+) = (\S+)", l)
            if m:
                values[m.group(1)] = num(m.group(4))
            m = re.match(r"hall rpm (\d+)", l)
            if m:
                values["hall_rpm"] = int(m.group(1))
        return values

    def hello(self, timeout=1.0):
        self.s.drain(0.2)
        seq, frame = hello_frame()
        self.s.transcript.append("%8.3f send HELLO frame %s" % (time.monotonic() - self.s.t0, frame.hex()))
        self.s.port.write_bytes(frame + b"\n")  # the newline ends the console's (dropped) line
        data, end = b"", time.monotonic() + timeout
        while time.monotonic() < end:
            data += self.s.port.read_bytes(min(0.05, max(0.0, end - time.monotonic())))
            reply, rest = find_reply(data, seq, LINK_OP_HELLO)
            if reply:
                self.s.port.unread(rest)
                self.s.transcript.append("%8.3f HELLO reply %s" % (time.monotonic() - self.s.t0, reply.hex()))
                if reply[4] != 0:
                    raise StepFailed("HELLO answered with result 0x%02x" % reply[4])
                return decode_info(reply[5:-2])
        self.s.port.unread(data)
        raise StepFailed("no HELLO reply within %.1f s" % timeout)

    # the steps
    def step_boot(self, st):
        c = self.cfg["boot"]
        self.log("  waiting %.0f s for the boot line: switch the board on" % c["wait_s"])
        line = self.event("* boot", c["wait_s"])
        w = line.split()
        st.must("firmware", w[2] if len(w) > 2 else None, want="flamekeeper-bringup")
        reason = w[w.index("reset") + 1] if "reset" in w else None
        st.must("reset reason", reason, want=c["reasons"])
        pu = self.event("* powerup", c["powerup_wait_s"])
        st.must("power-up self-test", pu.split()[2], want="passed")

    def step_uid(self, st):
        for l in self.cmd("uid"):
            m = re.match(r"uid ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})$", l)
            if m:
                self.uid = "-".join(m.groups())
        st.must("uid read", self.uid is not None, want=True)
        st.notes.append(self.uid)

    def step_info(self, st):
        i = self.info = self.read_info()
        st.notes.append(i.get("fw", ""))
        st.check("checks", i.get("checks"), want=0)
        st.check("power-up", i.get("powerup"), want="passed")
        st.check("latch armed", i.get("latch_armed"), want="1")
        st.check("burn", i.get("burn"), want="off")
        st.check("fault", i.get("fault"), want="none")
        for s in ("fan_fault", "pump_fault", "glow_fault"):
            st.check(s, i.get(s), want="none")
        st.check("link bad frames", i.get("link_bad_frames"), want="0")

    def step_options(self, st):
        lines = self.cmd("opt")
        values = {}
        for l in lines:
            if l.startswith("optr"):  # not the last line ("want ...: ok")
                values.update(kv(l))
            m = re.match(r"optr (0x[0-9a-f]{8})", l)
            if m:
                values["optr"] = int(m.group(1), 16)
        want = self.cfg["options"]["optr"]
        st.check("FLASH_OPTR", "0x%08x" % values.get("optr", 0), want="0x%08x" % want)
        st.check("OPTVERR", values.get("optverr"), want="0")
        st.check("WDG-1", lines[-1].rsplit(":", 1)[-1].strip() if lines else None, want="ok")
        st.check("RDP level 0", "0x%02x" % (values.get("optr", 0) & 0xFF), want="0xaa")

    def step_watchdog(self, st):
        c = self.cfg["watchdog"]
        self.s.drain(0.2)
        t0 = time.monotonic()
        self.s.send("wdg stop")
        line = self.event("* boot", 3.0)
        ms = (time.monotonic() - t0) * 1000.0
        st.check("reset after (ms)", round(ms), window=c["boot_ms"])
        w = line.split()
        st.check("reset reason", w[w.index("reset") + 1] if "reset" in w else None, want="watchdog")
        st.check("option bytes after", w[-1], want="ok")
        pu = self.event("* powerup", self.cfg["boot"]["powerup_wait_s"])
        st.check("power-up self-test", pu.split()[2], want="passed")

    def step_clock(self, st):
        c = self.cfg["clock"]
        a, ta = self.read_info(), time.monotonic()
        io_a = self.lines_kv("io", "io")
        time.sleep(c["seconds"])
        b, tb = self.read_info(), time.monotonic()
        io_b = self.lines_kv("io", "io")
        ratio = (int(b["uptime_ms"]) - int(a["uptime_ms"])) / ((tb - ta) * 1000.0)
        st.check("ms ratio", round(ratio, 4), window=c["ratio"])
        loops = (int(io_b["loops"]) - int(io_a["loops"])) / (tb - ta)
        st.check("loops per s", round(loops, 1), window=c["loops_per_s"])
        st.check("refreshes per s", round((int(io_b["refreshes"]) - int(io_a["refreshes"])) / (tb - ta), 1),
                 window=c["loops_per_s"])

    def step_adc(self, st):
        values = self.read_adc()
        for name, window in self.cfg["adc"].items():
            st.check(name, values.get(name), window=window)

    def step_interlocks(self, st):
        c = self.cfg["interlocks"]
        io = self.lines_kv("io", "io")
        for k, v in (("latch_armed", "1"), ("pump_hs_ok", "1"), ("glow_hs_ok", "1"), ("hb_ms", "0"),
                     ("fan_duty", "0"), ("pump_stroking", "0"), ("glow_on", "0"), ("glow_en", "0"),
                     ("pump_oc_armed", "0")):
            st.check(k, io.get(k), want=v)
        st.check("pump_oc_ma", io.get("pump_oc_ma"), want=str(c["pump_oc_ma"]))
        if st.failed():
            raise StepFailed("outputs or interlocks not at rest")
        self.s.drain(0.1)
        self.cmd("hb %d" % c["hb_s"])
        time.sleep(min(0.5, c["hb_s"] / 2))
        io = self.lines_kv("io", "io")
        st.check("heartbeat running (hb_ms)", int(io.get("hb_ms", 0)) > 0, want=True)
        end = self.event("* hb ", c["hb_s"] + 3.0)
        st.must("heartbeat action", end.split()[2], want="done")
        io = self.lines_kv("io", "io")
        st.check("heartbeat stopped (hb_ms)", io.get("hb_ms"), want="0")
        self.cmd("precheck")
        end = self.event("* precheck ", 5.0)
        st.must("pre-start self-test", end.split()[2], want="done")

    def step_fan(self, st):
        c = self.cfg["fan"]
        self.s.drain(0.1)
        self.cmd("fan rpm %d %d" % (c["rpm"], c["seconds"]))
        self.s.events_for(c["settle_s"])
        fault = [e for e in self.s.events if e.startswith("* fan_")]
        st.must("fan action", fault[0] if fault else "running", want="running")
        values = self.read_adc()
        st.must("hall rpm", values.get("hall_rpm"), window=c["rpm_window"])
        st.check("fan_i", values.get("fan_i"), window=c["fan_i"])
        self.fan_on = True

    def step_glow(self, st):
        c = self.cfg["glow"]
        self.s.drain(0.1)
        self.cmd("glow")
        line = self.event("* glow ", 3.0)
        w = line.split()
        st.must("glow probe", w[2], want="done")
        v = kv(line)
        st.check("probe current (A)", num(v.get("i")), window=c["i"])
        st.check("resistance (mOhm)", num(v.get("r_mohm")), window=c["r_mohm"])
        st.check("cold type", num(v.get("cold_type")), want=c["cold_type"])
        g = {}
        for l in self.cmd("glow read"):
            if l.startswith("glow last_with_pulse"):
                g = kv(l)
        st.check("in-pulse reading (A)", num(g.get("pulse_A")), window=c["pulse_a"])
        st.check("gap reading (A)", num(g.get("gap_A")), window=c["gap_a"])

    def fan_off(self):
        if getattr(self, "fan_on", False):
            self.fan_on = False
            try:
                self.cmd("fan off")
            except StepFailed:
                self.s.send("stop")

    def step_pump(self, st):
        self.fan_off()
        c = self.cfg["pump"]
        self.s.drain(0.1)
        self.cmd("pump %d" % c["strokes"])
        line = self.event("* pump ", 5.0 + c["strokes"])
        st.must("pump action", line.split()[2], want="done")
        v = kv(line)
        st.check("strokes", num(v.get("strokes")), want=c["strokes"])
        st.check("lowest peak (mA)", num(v.get("peak_min_ma")), window=c["peak_ma"])
        st.check("highest peak (mA)", num(v.get("peak_max_ma")), window=c["peak_ma"])

    def step_pump_oc(self, st):
        c = self.cfg["pump_oc"]
        setting = self.cfg["interlocks"]["pump_oc_ma"]
        self.s.drain(0.1)
        self.cmd("pump oc %d" % c["ma"])
        line = self.event("* pump_oc ", 6.0)
        st.must("cut", "acted" if "(the cut acted)" in line else line[2:], want="acted")
        self.cmd("clear")
        io = self.lines_kv("io", "io")
        st.check("setting back (mA)", io.get("pump_oc_ma"), want=str(setting))
        st.check("cut disarmed", io.get("pump_oc_armed"), want="0")
        st.check("heartbeat stopped", io.get("hb_ms"), want="0")
        i = self.read_info()
        st.check("pump fault cleared", i.get("pump_fault"), want="none")

    def step_co(self, st):
        c = self.cfg["co"]
        lines = self.cmd("co")
        first = lines[0].split() if lines else ["co", "?"]
        v = {}
        for l in lines:
            v.update(kv(l))
        st.check("state", first[1], want=c["states"])
        st.check("fitted", v.get("fitted"), want="1" if c["fitted"] else "0")
        st.check("alarm", v.get("alarm"), want="0")
        if c["fitted"]:
            st.check("heater on", v.get("heat_en"), want="1")

    def step_link(self, st):
        info = self.hello()
        st.notes.append(info["text"])
        st.check("major", info["major"], want=LINK_MAJOR)
        st.check("kind", info["kind"], want=LINK_KIND_STM32)
        st.check("text", info["text"], prefix=self.cfg["link"]["text"])
        st.check("checks", info["checks"], want=0)

    def step_production(self, st):
        c = self.cfg["production"]
        path = c["image"] if os.path.isabs(c["image"]) else os.path.join(self.cfg["_dir"], c["image"])
        with open(path, "rb") as f:
            image = f.read()
        st.notes.append("%s, %d bytes, sha256 %s" % (os.path.basename(path), len(image), hashlib.sha256(image).hexdigest()))
        keep = c.get("saved_pages", "wipe") == "keep"
        log = lambda line: self.log("    " + line)  # noqa: E731
        try:
            if self.flasher:
                r = self.flasher(image, log, keep)
            else:
                r = program.program_board(image, dry_run=self.flash == "dry", log=log,
                                          keep_saved=keep, wipe_saved=not keep)
        except program.ProgramError as e:
            raise StepFailed("program.py: %s" % e)
        self.programmed = {k: r.get(k) for k in ("uid", "optr", "erase", "image_sha256", "image_bytes")}
        st.check("programmed", r.get("ok"), want=True)
        st.check("FLASH_OPTR", "0x%08x" % r["optr"], want="0x%08x" % self.cfg["options"]["optr"])
        if self.flash == "dry":
            st.notes.append("DRY RUN: nothing was programmed")
            return
        st.check("UID over SWD", r.get("uid"), want=self.uid)
        end = time.monotonic() + c["hello_wait_s"]
        info = None
        while time.monotonic() < end:
            try:
                info = self.hello()
                if info["checks"] == 0:
                    break
            except StepFailed:
                pass
            time.sleep(0.3)
        st.must("HELLO", info is not None, want=True)
        st.notes.append(info["text"])
        st.check("text", info["text"], prefix=c["text"])
        st.check("checks", info["checks"], want=0)

    def run(self, skip=()):
        stopped = None
        try:
            for st in self.steps:
                if stopped:
                    continue
                if st.id in skip:
                    st.result = "skipped"
                    continue
                if st.id == "production":
                    if any(s.result not in ("pass",) for s in self.steps if s is not st):
                        st.result = "not run"
                        st.notes.append("not every step passed: the bring-up image stays on")
                        continue
                    if self.flash == "none":
                        st.result = "skipped"
                        continue
                self.log("%-11s ..." % st.id)
                t0 = time.monotonic()
                try:
                    getattr(self, "step_" + st.id)(st)
                    st.result = "fail" if st.failed() else "pass"
                except StepFailed as e:
                    st.result = "fail"
                    st.notes.append(str(e))
                except (ValueError, KeyError, IndexError, TypeError, OSError) as e:  # an answer not understood
                    st.result = "fail"
                    st.notes.append("error: %r" % (e,))
                st.seconds = time.monotonic() - t0
                bad = [c for c in st.checks if not c["ok"]]
                self.log("%-11s %s%s" % (st.id, st.result.upper(), "".join(
                    "\n    %s = %s, want %s" % (c["name"], c["value"], c["want"]) for c in bad) +
                    "".join("\n    " + n for n in st.notes if st.result == "fail")))
                if st.result == "fail" and st.safety:
                    stopped = st.id
        finally:
            try:
                self.s.send("stop")
            except Exception:
                pass
        results = [s.result for s in self.steps]
        if "fail" in results:
            overall = "fail"
        elif all(r == "pass" for r in results) or (self.flash == "none" and results[-1] == "skipped" and
                                                   all(r == "pass" for r in results[:-1])):
            overall = "pass"
        else:
            overall = "incomplete"
        return {"result": overall, "stopped_at": stopped}


# ---------------------------------------------------------------- the report


def write_report(directory, test, outcome, cfg_path, cfg_sha, started, finished):
    os.makedirs(directory, exist_ok=True)
    stamp = started.strftime("%Y%m%dT%H%M%SZ")
    base = os.path.join(directory, "%s_%s" % (test.uid or "unknown-uid", stamp))
    cfg = {k: v for k, v in test.cfg.items() if not k.startswith("_")}
    report = {
        "tool": "flamekeeper factory_test", "report_version": 1,
        "uid": test.uid, "result": outcome["result"], "stopped_at": outcome["stopped_at"],
        "started": started.isoformat(), "finished": finished.isoformat(),
        "firmware": test.info.get("fw"), "flash": test.flash, "programmed": test.programmed,
        "config": {"file": os.path.basename(cfg_path), "sha256": cfg_sha, "values": cfg},
        "steps": [s.as_dict() for s in test.steps],
        "transcript": os.path.basename(base) + ".log",
    }
    with open(base + ".json", "w") as f:
        json.dump(report, f, indent=2)
        f.write("\n")
    with open(base + ".log", "w") as f:
        f.write("\n".join(test.s.transcript) + "\n")
    out = ["Flamekeeper safety MCU factory test",
           "UID        %s" % (test.uid or "unknown"),
           "Result     %s%s" % (outcome["result"].upper(),
                                " (stopped at %s)" % outcome["stopped_at"] if outcome["stopped_at"] else ""),
           "Started    %s" % report["started"], "Firmware   %s" % (report["firmware"] or "?"),
           "Config     %s (sha256 %s)" % (report["config"]["file"], cfg_sha[:16]), ""]
    for s in test.steps:
        out.append("%-11s %-8s %s%6.1f s" % (s.id, s.result.upper(), "stop  " if s.safety else "      ", s.seconds))
        for c in s.checks:
            out.append("    %-4s %-28s %-24s want %s" % ("ok" if c["ok"] else "FAIL", c["name"], c["value"], c["want"]))
        for n in s.notes:
            out.append("    note %s" % n)
    if test.programmed:
        out += ["", "Production image: %s bytes, sha256 %s, erase %s, FLASH_OPTR 0x%08x" % (
            test.programmed["image_bytes"], test.programmed["image_sha256"], test.programmed["erase"],
            test.programmed["optr"])]
    with open(base + ".txt", "w") as f:
        f.write("\n".join(out) + "\n")
    return base


def main(argv=None, port=None, flasher=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="the USB-UART on J_DEBUG pins 6 / 7, e.g. /dev/cu.usbserial-XXXX")
    ap.add_argument("--config", default=DEFAULT_CONFIG, help="windows (TOML or JSON; default factory.toml)")
    ap.add_argument("--reports", default="factory-reports", help="report directory (default ./factory-reports)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--no-flash", action="store_true", help="leave the bring-up image on")
    g.add_argument("--flash-dry-run", action="store_true", help="simulate the production flash (no probe)")
    ap.add_argument("--skip", action="append", default=[], choices=STEPS[:-1],
                    help="leave a step out (the result is then incomplete, and no production flash)")
    ap.add_argument("--quiet", action="store_true", help="no console lines on the screen")
    args = ap.parse_args(argv)

    cfg, cfg_sha = load_config(args.config)
    cfg["_dir"] = os.path.dirname(os.path.abspath(args.config))
    flash = "none" if args.no_flash else "dry" if args.flash_dry_run else "real"
    if flash == "real" and flasher is None:
        try:
            import pyocd  # type: ignore  # noqa: F401
        except ImportError:
            print("pyocd is needed for the production flash: `uv run --with pyocd python3 ...` (or --no-flash)")
            return 2
    import bringup

    session = bringup.Session(port or bringup.Port(args.port), echo=not args.quiet)
    test = FactoryTest(session, cfg, flasher=flasher, flash=flash)
    started = datetime.datetime.now(datetime.timezone.utc)
    try:
        outcome = test.run(skip=set(args.skip))
    except KeyboardInterrupt:
        session.send("stop")
        outcome = {"result": "incomplete", "stopped_at": "interrupted"}
    finished = datetime.datetime.now(datetime.timezone.utc)
    base = write_report(args.reports, test, outcome, args.config, cfg_sha, started, finished)
    print("%s: %s (report %s.txt)" % (test.uid or "unknown uid", outcome["result"].upper(), base))
    return 0 if outcome["result"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
