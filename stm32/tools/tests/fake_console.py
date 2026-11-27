"""A fake board running the bring-up image, for the factory test's host tests: the console's
answers in the formats of stm32/bringup/console.c (echo, lines, "ok" / "err", "* " events) and the
link's HELLO reply, on a real-time clock. Knobs break it one way at a time.

FakePort plugs it into bringup.Session in process; serve_pty() puts it behind a pseudo-terminal so
factory_test.py runs as a program against it."""
import os
import select
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "bringup"))
import bringup  # noqa: E402
import factory_test as ft  # noqa: E402

UID = (0x00350027, 0x4D4B5009, 0x20393650)

DEFAULTS = {
    "optr": 0xDEFEE1AA,
    "reset": "power_on",
    "powerup": "passed",
    "clock_skew": 1.0,
    "adc": {"glow_v": 0.0, "glow_i": 0.0, "pump_v": 0.4, "fan_i": 0.0, "pump_i": 0, "ntc": 26.0, "board": 24.0,
            "vin": 12.0, "vref": 3300, "co": 30.0},
    "fan_stall": False,
    "glow": {"i": 25.1, "vin": 12.5, "r_mohm": 500, "cold_type": 12},
    "pump_peak": 1200,
    "oc_cuts": True,
    "co_state": "warming_up",
    "wdg_works": True,
    "hello_text": "Flamekeeper bring-up 0.1.0",
}


class FakeBoard:
    def __init__(self, boot_delay=0.1, **knobs):
        self.k = {**DEFAULTS, **knobs}
        self.lock = threading.Lock()
        self.out = []  # (due, bytes)
        self.rx = b""
        self.image = "bringup"
        self.commands = []
        self._power(time.monotonic() + boot_delay, self.k["reset"])

    # time and output
    def _power(self, at, reason):
        self.boot_at = at
        self.fan_until = self.hb_until = 0.0
        self.fan_rpm = 0
        self.pump_fault = "none"
        self.latched = False
        self.wdg_stopped = False
        ok = "ok" if (self.k["optr"] & 0x01010000) == 0 else "WRONG"
        self.emit(["* boot flamekeeper-bringup 0.1.0 reset %s csr 0x0c000000 opt 0x%08x %s" % (reason, self.k["optr"], ok)],
                  at=at)
        self.emit(["* powerup %s 0x0000" % self.k["powerup"]], at=at + 0.15)

    def emit(self, lines, at=None):
        at = time.monotonic() if at is None else at
        self.out.append((at, "".join(l + "\r\n" for l in lines).encode("ascii")))

    def take(self):
        """The bytes due now."""
        now = time.monotonic()
        with self.lock:
            due = [o for o in self.out if o[0] <= now]
            self.out = [o for o in self.out if o[0] > now]
        due.sort(key=lambda o: o[0])
        return b"".join(d for _, d in due)

    def uptime_ms(self):
        return int(max(0.0, time.monotonic() - self.boot_at) * 1000 * self.k["clock_skew"])

    def running(self):
        return time.monotonic() >= self.boot_at

    # input
    def feed(self, data):
        with self.lock:
            self.rx += data
            while b"\n" in self.rx:
                line, self.rx = self.rx.split(b"\n", 1)
                line = line.rstrip(b"\r")
                if any(c < 0x20 or c > 0x7E for c in line):
                    self._frames(line)
                elif line and self.running() and self.image == "bringup":
                    self._command(line.decode("ascii"))

    def _frames(self, data):
        i = data.find(bytes([ft.LINK_REQUEST]))
        if i < 0 or i + 2 > len(data):
            return
        n = data[i + 1] + 2
        f = data[i:i + n]
        if len(f) != n or ft.crc16(f[:-2]) != (f[-2] << 8 | f[-1]) or f[3] != ft.LINK_OP_HELLO or not self.running():
            return
        text = self.k["hello_text"] if self.image == "bringup" else "Flamekeeper safety 0.1.0"
        payload = bytes([1, ft.LINK_KIND_STM32, 0, 1, 0]) + (0x7FFF).to_bytes(4, "little") + bytes([30, 1, 0])
        self.out.append((time.monotonic() + 0.005, ft.build_frame(ft.LINK_REPLY, f[2], f[3], payload + text.encode(), 0)))

    def _command(self, cmd):
        self.commands.append(cmd)
        w = cmd.split()
        lines, err, now = ["> " + cmd], None, time.monotonic()
        hb_ms = int((self.hb_until - now) * 1000) if now < self.hb_until else 0
        fan_on = now < self.fan_until
        rpm = self.fan_rpm if fan_on else 0
        if w == ["info"]:
            lines += ["fw flamekeeper-bringup 0.1.0 safety 0.1.0 uptime_ms=%d" % self.uptime_ms(),
                      "reset %s csr 0x0c000000" % self.k["reset"],
                      "opt 0x%08x ok" % self.k["optr"],
                      "checks 0x00",
                      "powerup %s 0x0000 latch_armed=1" % self.k["powerup"],
                      "burn off fault none why none latched=%d fan_stalls=0" % self.latched,
                      "stages fan none pump %s glow none glow_budget_a2s=0 co %s" % (self.pump_fault, self.k["co_state"]),
                      "vdda_mv 3300 vrefint_cal=1655 plug_record=0 plug_setting=12 link_bad_frames=0 out_dropped=0"]
        elif w == ["uid"]:
            lines.append("uid " + " ".join("%08x" % x for x in UID))
        elif w == ["opt"]:
            o = self.k["optr"]
            b = lambda n: (o >> n) & 1  # noqa: E731
            lines += ["optr 0x%08x rdp 0x%02x nrst_stop=1 nrst_stdby=1 iwdg_sw=%d iwdg_stop=%d iwdg_stdby=%d wwdg_sw=1"
                      % (o, o & 0xFF, b(16), b(17), b(18)),
                      "optr nboot_sel=%d nboot1=%d nboot0=%d nrst_mode=3 optverr=0" % (b(24), b(25), b(26)),
                      "want iwdg_sw=0 iwdg_stop=1 iwdg_stdby=1 nboot_sel=0 nboot1=1 nboot0=1 optverr=0: " +
                      ("ok" if (o & 0x07070000) == 0x06060000 else "WRONG")]
        elif w == ["io"]:
            loops = self.uptime_ms() // 10
            lines += ["io latch_armed=1 pump_hs_ok=1 glow_hs_ok=1 hb_ms=%d bursts=0 loops=%d refreshes=%d" % (hb_ms, loops, loops),
                      "io fan_duty=%d pump_stroking=0 pump_strokes=0 glow_on=0 glow_en=0 glow_budget_a2s=0" % (300 if fan_on else 0),
                      "io pump_oc_armed=0 pump_oc_ma=3300 pump_oc_raw=1842 pump_oc_hold_us=50 pump_oc_arms=2 co_heat_en=1 co_heat_wanted=1"]
        elif w == ["adc"]:
            a = dict(self.k["adc"])
            if fan_on:
                a["fan_i"] = 0.5
            fmt = {"glow_v": "%.2f V (x19)", "pump_v": "%.2f V (x19)", "glow_i": "%.2f A (9 mV/A)",
                   "fan_i": "%.3f A average switch current (15 mV/A)", "pump_i": "%d mA (0.45 V/A)",
                   "ntc": "%.1f C heat exchanger", "board": "%.1f C board", "vin": "%.2f V supply (x106.8/6.8)",
                   "vref": "%d mV VDDA cal=1655", "co": "%.1f %% of VDDA"}
            for name in ("glow_v", "glow_i", "pump_v", "fan_i", "pump_i", "ntc", "board", "vin", "vref", "co"):
                v = a[name]
                lines.append("adc %s raw=1000 mv=806 = %s" % (name, "open" if v is None else fmt[name] % v))
            lines.append("hall rpm %d (one pulse a turn assumed)  glow_probe 0.00 A at 0.00 V" % rpm)
        elif w[:1] == ["hb"] and len(w) == 2:
            self.hb_until = now + int(w[1])
            self.emit(["* hb done"], at=self.hb_until)
        elif w == ["precheck"]:
            self.emit(["* precheck done"], at=now + 0.05)
        elif w[:2] == ["fan", "rpm"]:
            self.fan_until = now + int(w[3])
            self.fan_rpm = 0 if self.k["fan_stall"] else int(w[2]) + 12
            if self.k["fan_stall"]:
                self.fan_until = now + 0.1
                self.emit(["* fan_rpm fault fault stall"], at=now + 0.1)
        elif w == ["fan", "off"]:
            if fan_on:
                self.fan_until = now
                self.emit(["* fan_rpm done"], at=now + 0.01)
        elif w == ["glow"]:
            if not fan_on or rpm < 1000:
                err = "no airflow: start a fan action at the glow's minimum speed"
            else:
                g = self.k["glow"]
                self.emit(["* glow done i=%.2f vin=%.2f r_mohm=%d cold_type=%d" % (g["i"], g["vin"], g["r_mohm"], g["cold_type"])],
                          at=now + 0.06)
        elif w == ["glow", "read"]:
            g = self.k["glow"]["i"]
            lines += ["glow periods=500 last_pulse_ms_ago=40",
                      "glow last pulse_raw=0 pulse_A=0.00 gap_raw=0 gap_A=0.00 gap_vin_raw=980 gap_vin=12.50",
                      "glow last_with_pulse pulse_raw=2800 pulse_A=%.2f gap_raw=0 gap_A=0.00 gap_vin_raw=980 gap_vin=12.50" % g]
        elif w[:2] == ["pump", "oc"]:
            if self.latched:
                err = "a fault needs clearing first (clear)"
            elif self.k["oc_cuts"]:
                self.pump_fault, self.latched = "overcurrent", True
                self.emit(["* pump_oc fault fault overcurrent (the cut acted) strokes=0 peak_min_ma=0 peak_max_ma=0"],
                          at=now + 0.1)
            else:
                self.emit(["* pump_oc done (NO CUT) strokes=1 peak_min_ma=1200 peak_max_ma=1200"], at=now + 0.1)
        elif w[:1] == ["pump"] and len(w) in (2, 3):
            if self.latched:
                err = "a fault needs clearing first (clear)"
            else:
                p = self.k["pump_peak"]
                self.emit(["* pump done strokes=%s peak_min_ma=%d peak_max_ma=%d" % (w[1], p, p)], at=now + 0.1)
        elif w == ["clear"]:
            if not self.latched:
                err = "refused (nothing latched, or its cause is still there)"
            self.latched, self.pump_fault = False, "none"
        elif w == ["co"]:
            fitted = int(self.k["co_state"] != "not_fitted")
            lines += ["co %s fitted=%d param_0x52=%d raw=1241 rs_ohm=48000" % (self.k["co_state"], fitted, fitted),
                      "co r0_ohm=0 r0_saved_ohm=0 ratio=unknown ppm=unknown",
                      "co alarm=0 fault=0 heater_checked=0 checking=0 learned_s=0 heat_wanted=%d heat_en=%d" % (fitted, fitted)]
        elif w == ["wdg", "stop"]:
            lines.append("watchdog refresh stopped: expect a reset in about 80 ms")
            if self.k["wdg_works"]:
                self.out = [o for o in self.out if o[0] <= now]
                self._power(now + 0.08, "watchdog")
        elif w == ["stop"]:
            if fan_on:
                self.emit(["* fan_rpm stopped"], at=now + 0.01)
            self.fan_until = self.hb_until = 0.0
        else:
            err = "unknown command (help)"
        lines.append("err " + err if err else "ok")
        self.emit(lines, at=now + 0.002)


class FakePort(bringup.Port):
    """bringup.Port over a FakeBoard, in process."""

    def __init__(self, board):  # noqa: super().__init__ opens a device
        self.board, self.buf, self.ser, self.fd = board, b"", None, None

    def write_bytes(self, data):
        self.board.feed(data)

    def _read(self, timeout):
        end = time.monotonic() + timeout
        while True:
            data = self.board.take()
            if data or time.monotonic() >= end:
                return data
            time.sleep(0.002)


def serve_pty(board):
    """The board behind a pseudo-terminal: (the device path, a stop function)."""
    import tty

    master, slave = os.openpty()
    tty.setraw(slave)
    stop = threading.Event()

    def loop():
        while not stop.is_set():
            r, _, _ = select.select([master], [], [], 0.005)
            if r:
                try:
                    board.feed(os.read(master, 1024))
                except OSError:
                    pass
            data = board.take()
            if data:
                os.write(master, data)

    t = threading.Thread(target=loop, daemon=True)
    t.start()

    def close():
        stop.set()
        t.join(1.0)
        os.close(master)
        os.close(slave)

    return os.ttyname(slave), close
