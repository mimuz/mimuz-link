#!/usr/bin/env python3
#
#  duolog.py read two serial ports into one timestamped log
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#
"""Read the C6 and S3 (bridge) serial ports at the same time and print them as one stream, stamped with the Mac's clock.
Commands can be sent to either device during the run. Used to check the registration / connection sequence.

usage: duolog.py --c6 PORT --s3 PORT --seconds N [--send "t:who:cmd" ...]
  --send "2.0:c6:u"   send 'u' to the C6 2.0 s after start (who is c6 or s3)
The S3 port is opened with DTR=True (it prints no log unless DTR is set). The C6 may reset when its port is opened.
"""
import argparse
import threading
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--c6")
    ap.add_argument("--s3")
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--send", action="append", default=[])
    a = ap.parse_args()
    t0 = time.time()
    lock = threading.Lock()
    ports = {}

    def reader(name, dev):
        buf = ""
        while time.time() - t0 < a.seconds:
            try:
                s = ports.get(name)
                if s is None:
                    s = serial.Serial(dev, 115200, timeout=0.2)
                    ports[name] = s
                buf += s.read(4096).decode(errors="replace")
            except (serial.SerialException, OSError):
                ports[name] = None  # disconnected (e.g. device restart) -> reopen
                time.sleep(0.3)
                continue
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.rstrip("\r")
                if line.strip():
                    with lock:
                        print(f"[{time.time() - t0:6.2f}] {name}: {line}", flush=True)

    th = []
    for name, dev in (("S3", a.s3), ("C6", a.c6)):
        if dev:
            t = threading.Thread(target=reader, args=(name, dev), daemon=True)
            t.start()
            th.append(t)
    sends = sorted((float(x.split(":")[0]), x.split(":")[1].upper(), x.split(":", 2)[2]) for x in a.send)
    for at, who, cmd in sends:
        time.sleep(max(0, at - (time.time() - t0)))
        s = ports.get(who)
        if s:
            with lock:
                print(f"[{time.time() - t0:6.2f}] >> {who}: {cmd}", flush=True)
            s.write(cmd.encode())
    time.sleep(max(0, a.seconds - (time.time() - t0)))


if __name__ == "__main__":
    main()
