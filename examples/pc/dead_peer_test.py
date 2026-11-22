#!/usr/bin/env python3
#
#  dead_peer_test.py measure how an unresponsive client delays the other clients
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#  - 2026/11/22 : Time round trips with mlping (C, CoreMIDI) instead of a Python thread
#
"""Measure how much sending to an unresponsive client delays the other clients (serial vs. parallel TX).

usage: dead_peer_test.py BRIDGE_SERIAL VICTIM_SERIAL --victim 3 --other 2 [--trials 8]

One trial:
  1. Keep sending round-trip pings to another client (--other), about every 3 ms.
  2. Make the victim client (--victim) leave ESP-NOW with `restart ble` (its port on the bridge stays for about 16 s).
  3. Send from the PC to the victim's port every 5 ms for 1.5 s. After 5 consecutive delivery failures the bridge skips messages for that client.
  4. Bring the victim back with `restart auto`.
Serial (tx_parallel=0) and parallel (tx_parallel=1) are alternated.
"""
import argparse, time
import serial
from multi_test import Port, MlPing, pct

ap = argparse.ArgumentParser()
ap.add_argument("bridge")
ap.add_argument("victim_serial")
ap.add_argument("--victim", type=int, required=True)
ap.add_argument("--other", type=int, required=True)
ap.add_argument("--trials", type=int, default=8)
ap.add_argument("--setA", default="tx_parallel 0")
ap.add_argument("--setB", default="tx_parallel 1")
ap.add_argument("--names", default="直列,並行")
a = ap.parse_args()
bs = serial.Serial(a.bridge, 115200, timeout=0.1)
k = [0]


def bcmd(text, wait=2.0):
    k[0] += 1
    bs.reset_input_buffer()
    bs.write(f"@{k[0]} {text}\n".encode())
    buf, t = "", time.time()
    while time.time() - t < wait and f"@{k[0]} ok" not in buf and f"@{k[0]} err" not in buf:
        buf += bs.read(2048).decode(errors="replace")
    return buf


def vcmd(text):
    for _ in range(3):
        try:
            s = serial.Serial(a.victim_serial, 115200, timeout=0.2)
            s.write(f"@1 {text}\n".encode())
            time.sleep(0.15)
            s.close()
            return
        except Exception:
            time.sleep(0.5)


def victim_info():
    for ln in bcmd("ports").splitlines():
        if f" port={a.victim} " in ln:
            f = dict(x.split("=", 1) for x in ln.split()[2:])
            return int(f["tx_fail"]), int(f["idle_ms"])
    return None, None


NAMES = a.names.split(",")
V, O = Port("bridgeESPS3", a.victim), Port("bridgeESPS3", a.other)
time.sleep(0.3)
res = {0: [], 1: []}
base = {0: [], 1: []}
for t in range(a.trials * 2):
    mode = t % 2
    for kv in (a.setB if mode else a.setA).split(";"):
        bcmd(f"set {kv.strip()}")
    # Before the victim leaves (baseline for comparison): 1.5 s
    now = lambda: time.clock_gettime_ns(time.CLOCK_UPTIME_RAW) / 1e9  # the clock mlping uses
    pinger = MlPing("bridgeESPS3", [a.other], gmin=1, gmax=1, timeout=0.3)
    time.sleep(1.7)  # mlping takes about 0.2 s to start
    tb = now()
    f0, _ = victim_info()
    vcmd("restart ble")
    time.sleep(0.4)  # wait for the restart to begin
    t1 = now()
    for i in range(300):  # every 5 ms for 1.5 s
        V.mo.send_message([0xB0, 1, i & 0x7F])
        time.sleep(0.005)
    t2 = now()
    time.sleep(0.2)
    samples = [(snd / 1e9, (ts - snd) / 1e6 if ts else None) for _, snd, ts, _, _ in pinger.stop().rec[a.other]]
    f1, idle = victim_info()
    vcmd("restart auto")
    bfr = [r for (ts, r) in samples if ts < tb and r is not None]
    win = [(ts, r) for (ts, r) in samples if t1 - 0.02 <= ts <= t2 + 0.1]
    rt = [r for _, r in win if r is not None]
    lost = sum(1 for _, r in win if r is None)
    base[mode] += bfr
    res[mode].append({"rt": rt, "lost": lost, "fail": (f1 - f0) if f0 is not None and f1 is not None else None})
    slow = sorted([round(r, 1) for r in rt if r > 8], reverse=True)[:8]
    print(f"試行 {t + 1:2d} {NAMES[mode]}: 離脱の前 p50={pct(bfr, .5):.2f} max={max(bfr):.1f}ms | 送信中 n={len(rt)} max={max(rt):.1f}ms "
          f"8ms 超={sum(1 for r in rt if r > 8)} 回 {slow} 取りこぼし={lost} | 犠牲の台あての失敗={res[mode][-1]['fail']}", flush=True)
    # Wait for the victim to come back
    t0 = time.time()
    while time.time() - t0 < 25:
        _, idle = victim_info()
        if idle is not None and idle < 1500 and time.time() - t0 > 3:
            break
        time.sleep(0.5)
    time.sleep(1.0)
for mode in (0, 1):
    allrt = [r for x in res[mode] for r in x["rt"]]
    over = [sum(1 for r in x["rt"] if r > 8) for x in res[mode]]
    summ = [sum(r for r in x["rt"] if r > 8) for x in res[mode]]
    print(f"== {NAMES[mode]}: 離脱の前 p50={pct(base[mode], .5):.2f} p99={pct(base[mode], .99):.2f} | 送信中 n={len(allrt)} p50={pct(allrt, .5):.2f} "
          f"p99={pct(allrt, .99):.2f} max={max(allrt):.1f}ms | 8ms を超えた回数（試行ごと）={over} | 取りこぼし={sum(x['lost'] for x in res[mode])} | 失敗={[x['fail'] for x in res[mode]]}")
