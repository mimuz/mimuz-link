#!/usr/bin/env python3
#
#  rate_sweep.py radio round-trip latency at each PHY rate
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/18 : Start Writing
#
"""Measure the radio RTT (the C6's 'p' / 'P' commands) over the C6's serial port while switching the PHY rate.

usage: rate_sweep.py PORT [--rates 0 2 3] [--big] [--log FILE]
  --big : 100,000 round trips ('P'). Default is 10,000 ('p')
Prints the results (the C6's output as is) and, with --log, also appends them to the file.
"""
import argparse
import datetime
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--rates", nargs="+", default=["0", "2", "3"])
    ap.add_argument("--big", action="store_true")
    ap.add_argument("--log")
    a = ap.parse_args()

    s = serial.Serial(a.port, 115200, timeout=0.2)
    time.sleep(3)  # wait for re-pairing in case opening the port reset the C6

    def run(cmd, until, to):
        s.reset_input_buffer()
        s.write(cmd.encode())
        t, out = time.time(), ""
        while time.time() - t < to:
            out += s.read(4096).decode(errors="replace")
            if until in out:
                time.sleep(0.3)
                out += s.read(4096).decode(errors="replace")
                break
        return out

    log = [f"==== rate_sweep {datetime.datetime.now():%Y-%m-%d %H:%M:%S} port={a.port} big={a.big}"]
    for r in a.rates:
        log.append(run(f"r{r}", "rate ->", 3).strip())
        time.sleep(0.5)
        run("c", "cleared", 2)
        o = run("P" if a.big else "p", "rssi=", 1500 if a.big else 150)
        log.append("\n".join(l for l in o.splitlines() if not l.strip().endswith("%")))
        log.append(run("s", "MAC ACK", 3).strip())
    text = "\n".join(log)
    print(text)
    if a.log:
        with open(a.log, "a") as f:
            f.write(text + "\n")


if __name__ == "__main__":
    main()
