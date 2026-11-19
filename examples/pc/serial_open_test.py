#!/usr/bin/env python3
#
#  serial_open_test.py check whether opening the serial port resets the C6
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#
"""Check whether the C6 resets when its serial port is opened.

Opens the port with different control-line (DTR / RTS) settings and reads the C6's statistics ('s') each time.
The C6 is judged to have reset if boot messages appear, or if scans in the statistics (incremented every 1 ms) is below 6000.

usage: serial_open_test.py PORT [--repeat 3]
"""
import argparse
import re
import time

import serial


def open_and_read(port, dtr, rts):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    if dtr is not None:
        s.dtr = dtr
    if rts is not None:
        s.rts = rts
    s.open()
    out = ""
    t = time.time()
    while time.time() - t < 2.5:  # wait for the boot messages in case the C6 was reset
        out += s.read(4096).decode(errors="replace")
    s.write(b"s")
    t = time.time()
    while time.time() - t < 1.0:
        out += s.read(4096).decode(errors="replace")
    s.close()
    m = re.findall(r"scans=(\d+)", out)
    booted = "ESP-ROM" in out or "ready (" in out
    return (int(m[-1]) if m else None), booted


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--repeat", type=int, default=3)
    a = ap.parse_args()
    cases = [("既定（何も指定しない）", None, None), ("DTR=オフ, RTS=オフ", False, False), ("DTR=オン, RTS=オフ", True, False),
             ("DTR=オフ, RTS=オン", False, True), ("DTR=オン, RTS=オン", True, True)]
    for name, dtr, rts in cases:
        resets = 0
        seen = []
        for _ in range(a.repeat):
            scans, booted = open_and_read(a.port, dtr, rts)
            # Value 3.5 s after opening. If the C6 was reset, it only covers the time since boot (a few thousand)
            reset = booted or (scans is not None and scans < 6000)
            resets += reset
            seen.append(scans)
            time.sleep(6)  # leave enough time since boot before the next round
        print(f"{name:24s}: リセット {resets}/{a.repeat} 回  (scans={seen})")


if __name__ == "__main__":
    main()
