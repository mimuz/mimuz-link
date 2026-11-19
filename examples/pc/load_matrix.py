#!/usr/bin/env python3
#
#  load_matrix.py latency under simulated workload, as a matrix of settings
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#
"""Measure round trip (ping radio) and burst while varying the workload (the C6 runs firmware built with WORKLOAD=1).

The conditions are sent to the C6 as CC on ch16:
  #125 = loop() load (busy-wait of value x 100 us per loop iteration)
  #126 = simulated encoders (clicks/s per encoder, for 4 encoders)

usage: load_matrix.py --port XIAO_ESP32S3 --label own24M [--log results.log]
"""
import argparse
import datetime
import subprocess
import sys
import time
from pathlib import Path

import rtmidi

HERE = Path(__file__).resolve().parent
CTRL = 0xBF

# (loop load [x100us], encoders [clicks/s per encoder])
CONDITIONS = [(0, 0), (10, 0), (50, 0), (0, 50), (50, 50)]


def set_cond(port, load, enc):
    mo = rtmidi.MidiOut()
    idx = next(i for i, p in enumerate(mo.get_ports()) if port.lower() in p.lower())
    mo.open_port(idx)
    mo.send_message([CTRL, 125, load])
    time.sleep(0.05)
    mo.send_message([CTRL, 126, enc])
    time.sleep(0.5)
    mo.close_port()


def run(args, log):
    p = subprocess.run([sys.executable, str(HERE / "latency_test.py")] + args, capture_output=True, text=True)
    out = "\n".join(l for l in (p.stdout + p.stderr).splitlines()
                    if any(k in l for k in ("RTT ", "lost=", "received", "excess", "中止")))
    print(out, flush=True)
    log.write(out + "\n")
    log.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--log", default=str(HERE / "results.log"))
    a = ap.parse_args()
    with open(a.log, "a") as log:
        log.write(f"\n==== load_matrix {a.label} {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n")
        for load, enc in CONDITIONS:
            head = f"-- {a.label}: loop負荷 {load * 100}us, エンコーダー {enc}クリック/秒x4"
            print(head, flush=True)
            log.write(head + "\n")
            set_cond(a.port, load, enc)
            run(["ping", "--port", a.port, "--target", "radio", "-n", "500", "--timeout", "300"], log)
            run(["burst", "--port", a.port, "--period", "10", "--count", "300"], log)
        set_cond(a.port, 0, 0)


if __name__ == "__main__":
    main()
