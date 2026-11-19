#!/usr/bin/env python3
#
#  run_suite.py run the standard BLE / ESP-NOW measurement suite
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#
"""Run the BLE vs. ESP-NOW comparison measurements in one go and append the results to results.log.

usage:
  run_suite.py ble                     # the C6 must already be connected to the Mac in BLE mode
  run_suite.py espnow [--c6 PORT]      # the S3 must be connected. With --c6, the C6 is set to r2 (24M) before measuring

Tests (each calls latency_test.py):
  ble    : ping(radio,1000) / burst 10ms x500 / burst 7ms x500
  espnow : ping usb(1000) / ping radio(1000) / burst 10ms x500 / burst 7ms x500
"""
import argparse
import datetime
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PY = sys.executable


def run(args, log):
    cmd = [PY, str(HERE / "latency_test.py")] + args
    log.write(f"\n$ latency_test.py {' '.join(args)}\n")
    log.flush()
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = "\n".join(l for l in (p.stdout + p.stderr).splitlines() if not l.strip().endswith("%"))
    print(out, flush=True)
    log.write(out + "\n")
    log.flush()
    return p.returncode == 0


def set_c6_rate(port, rate):
    import serial
    s = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(3)  # wait for re-pairing in case the C6 was reset
    s.write(f"r{rate}".encode())
    time.sleep(0.5)
    print(s.read(4096).decode(errors="replace"))
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["ble", "espnow"])
    ap.add_argument("--c6", help="C6 のシリアル（espnow でレートを設定する場合）")
    ap.add_argument("--rate", default="2")
    ap.add_argument("--port", help="MIDI ポート名の一部（espnow の既定は XIAO_ESP32S3）")
    ap.add_argument("--log", default=str(HERE / "results.log"))
    a = ap.parse_args()
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    with open(a.log, "a") as log:
        log.write(f"\n==== run_suite {a.mode} {stamp}\n")
        if a.mode == "ble":
            port = a.port or "conESP32C6"
            run(["info", "--port", port], log)
            ok = run(["ping", "--port", port, "--target", "radio", "-n", "1000", "--timeout", "300", "--bin", "2",
                      "--csv", str(HERE / f"ble_ping_{stamp}.csv")], log)
            if not ok:
                return
        else:
            port = a.port or "bridgeESPS3"
            if a.c6:
                set_c6_rate(a.c6, a.rate)
            run(["ping", "--port", port, "--target", "usb", "-n", "1000", "--bin", "0.25"], log)
            run(["ping", "--port", port, "--target", "radio", "-n", "1000", "--bin", "0.5",
                 "--csv", str(HERE / f"espnow_ping_{stamp}.csv")], log)
        for period in (10, 7):
            run(["burst", "--port", port, "--period", str(period), "--count", "500", "--bin", "1",
                 "--csv", str(HERE / f"{a.mode}_burst{period}_{stamp}.csv")], log)
        if a.mode == "ble":
            run(["info", "--port", port], log)


if __name__ == "__main__":
    main()
