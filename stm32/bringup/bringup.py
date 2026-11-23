#!/usr/bin/env python3
"""Drive the safety MCU's bring-up console from a USB-UART (J_DEBUG pins 6 / 7, 115200 8N1).

Interactive (type commands, Ctrl-C sends "stop" and quits):
    bringup.py /dev/ttyUSB0
Run commands in order, then keep reading events for a while:
    bringup.py /dev/ttyUSB0 -c info -c adc
    bringup.py /dev/ttyUSB0 -c "fan duty 300 10" --events 12
Save a flash page as a file (the parameter page is 30, the plug record 31):
    bringup.py /dev/ttyUSB0 --dump 30 params.bin
Time a watchdog reset (WDG-14), from the command to the boot line:
    bringup.py /dev/ttyUSB0 --wdg hang
Every line can go to a log with --log FILE (appended, with times).

Python 3 standard library only on Linux and macOS; pyserial is used if it is installed (and is
needed on Windows). Keep the ESP32 off the link meanwhile: it must not drive the MCU's RX line.
"""
import argparse
import os
import select
import sys
import threading
import time

BAUD = 115200


class Port:
    """A serial port at 115200 8N1, raw: pyserial if present, else POSIX termios."""

    def __init__(self, path):
        self.buf = b""
        try:
            import serial  # type: ignore

            self.ser = serial.Serial(path, BAUD, timeout=0)
            self.fd = None
        except ImportError:
            import termios

            self.ser = None
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            a = termios.tcgetattr(self.fd)
            a[0] = 0  # iflag
            a[1] = 0  # oflag
            a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            a[3] = 0  # lflag: raw
            a[4] = a[5] = termios.B115200
            a[6][termios.VMIN] = 0
            a[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
            termios.tcflush(self.fd, termios.TCIOFLUSH)

    def write_line(self, line):
        self.write_bytes(line.encode("ascii") + b"\n")

    def write_bytes(self, data):
        if self.ser:
            self.ser.write(data)
        else:
            os.write(self.fd, data)

    def read_bytes(self, timeout):
        """Whatever is buffered and arrives within timeout seconds, raw (a link frame is read so)."""
        data, self.buf = self.buf, b""
        return data + self._read(timeout)

    def unread(self, data):
        self.buf = data + self.buf

    def _read(self, timeout):
        if self.ser:
            self.ser.timeout = timeout
            return self.ser.read(256)
        r, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 256) if r else b""

    def readline(self, timeout):
        """One text line, or None after timeout seconds. Binary bytes (link frames) are skipped."""
        end = time.monotonic() + timeout
        while b"\n" not in self.buf:
            left = end - time.monotonic()
            if left <= 0:
                return None
            self.buf += self._read(min(left, 0.05))
        raw, self.buf = self.buf.split(b"\n", 1)
        raw = raw.rstrip(b"\r")
        if any(c < 0x20 or c > 0x7E for c in raw):
            return ""  # a link frame or noise: not the console's
        return raw.decode("ascii")


class Session:
    """echo=False keeps the lines off the screen; every line is kept in .transcript, and unasked
    lines ("* ...") in .events until wait_event takes them."""

    def __init__(self, port, log=None, echo=True):
        self.port = port
        self.log = open(log, "a") if log else None
        self.echo = echo
        self.t0 = time.monotonic()
        self.transcript = []
        self.events = []

    def show(self, line):
        stamp = "%8.3f" % (time.monotonic() - self.t0)
        self.transcript.append("%s %s" % (stamp, line))
        if self.echo:
            print(stamp, line, flush=True)
        if self.log:
            self.log.write("%s %s %s\n" % (time.strftime("%H:%M:%S"), stamp, line))
            self.log.flush()

    def send(self, cmd):
        self.transcript.append("%8.3f send %s" % (time.monotonic() - self.t0, cmd))
        if self.log:
            self.log.write("%s send %s\n" % (time.strftime("%H:%M:%S"), cmd))
        self.port.write_line(cmd)

    def take(self, line, quiet=False):
        """A line read: shown (unless quiet and ordinary), kept if an event."""
        if line.startswith("* "):
            self.events.append(line)
        if not quiet or line.startswith(("err", "*")):
            self.show(line)

    def wait_event(self, match, timeout):
        """The first event (kept or arriving within timeout s) for which match(line) is true, or None.
        Events before it are dropped."""
        end = time.monotonic() + timeout
        while True:
            while self.events:
                line = self.events.pop(0)
                if match(line):
                    return line
            left = end - time.monotonic()
            if left <= 0:
                return None
            line = self.port.readline(left)
            if line:
                self.take(line)

    def drain(self, seconds=0.2):
        """Read (and keep) whatever comes within seconds, then forget the events."""
        self.events_for(seconds)
        self.events.clear()

    def events_for(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            line = self.port.readline(end - time.monotonic())
            if line:
                self.take(line)

    def command(self, cmd, timeout=10.0, quiet=False):
        """Send one command; return (ok, lines) once its "ok" or "err" line arrives."""
        self.send(cmd)
        lines, echoed = [], False
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = self.port.readline(end - time.monotonic())
            if line is None:
                break
            if not line:
                continue
            self.take(line, quiet)
            if line == "> " + cmd:
                echoed = True
                continue
            if not echoed:
                continue
            if line == "ok":
                return True, lines
            if line.startswith("err"):
                return False, lines
            if not line.startswith("* "):
                lines.append(line)
        self.show("-- no answer to: " + cmd)
        return False, lines



def dump(session, page, path):
    ok, lines = session.command("flash %d" % page, timeout=5.0, quiet=True)
    if not ok:
        return False
    data = bytearray()
    for line in lines:
        words = line.split()
        if len(words) < 2 or words[0] != "flash":
            continue
        data += bytes(int(w, 16) for w in words[2:])
    with open(path, "wb") as f:
        f.write(data)
    session.show("-- %d bytes of page %d in %s" % (len(data), page, path))
    return len(data) == 2048


def wdg(session, how):
    session.send("wdg " + how)
    start = time.monotonic()
    end = start + 3.0
    while time.monotonic() < end:
        line = session.port.readline(end - time.monotonic())
        if not line:
            continue
        session.show(line)
        if line.startswith("* boot"):
            ms = (time.monotonic() - start) * 1000.0
            session.show("-- boot line %.0f ms after the command (USB-UART latency included)" % ms)
            return "reset watchdog" in line
    session.show("-- no reset within 3 s: the watchdog is NOT working")
    return False


def interactive(session):
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            line = session.port.readline(0.2)
            if line:
                session.show(line)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    try:
        for line in sys.stdin:
            line = line.strip()
            if line:
                session.send(line)
    except KeyboardInterrupt:
        pass
    session.send("stop")
    time.sleep(0.3)
    stop.set()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial device, e.g. /dev/ttyUSB0 or /dev/cu.usbserial-XXXX")
    ap.add_argument("-c", "--command", action="append", default=[], help="a console command (repeatable)")
    ap.add_argument("--events", type=float, default=0, help="keep printing events this many seconds after")
    ap.add_argument("--dump", nargs=2, metavar=("PAGE", "FILE"), help="save flash page PAGE (0-31) to FILE")
    ap.add_argument("--wdg", choices=["stop", "hang"], help="time a watchdog reset")
    ap.add_argument("--log", help="append every line to this file")
    args = ap.parse_args()

    session = Session(Port(args.port), args.log)
    ok = True
    if args.dump:
        ok = dump(session, int(args.dump[0]), args.dump[1])
    elif args.wdg:
        ok = wdg(session, args.wdg)
    elif args.command:
        try:
            for cmd in args.command:
                good, _ = session.command(cmd, timeout=15.0)
                ok = ok and good
                if not good:
                    break
            session.events_for(args.events)
        except KeyboardInterrupt:
            session.send("stop")
            ok = False
    else:
        interactive(session)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
