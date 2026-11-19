#!/usr/bin/env python3
#
#  weak_test.py effect of a weak client on the other clients
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#
"""Measure, cause by cause, how the latency of the other clients changes when one client has a weak link.

usage: weak_test.py BRIDGE_SERIAL --weak 2 --others 1,3 [--pings 1500] [--rounds 2] [--rates 1,2]

Keeps sending round-trip pings to the other clients (--others) in parallel. Meanwhile, the weak client (--weak) is put in one of 4 conditions:
  a. idle (only the keep-alive traffic)
  b. PC -> weak client every 5 ms (messages with no reply)        ... does the bridge's sending get held up?
  c. the weak client sends on its own every 5 ms                  ... effect of sharing the channel
  d. round trips with the weak client (both b and c)
For each PHY rate of the weak client (--rates), a to d are repeated in order, --rounds times each.
"""
import argparse, random, threading, time
import serial
from multi_test import Port, CTRL, pct

ap = argparse.ArgumentParser()
ap.add_argument("bridge")
ap.add_argument("--weak", type=int, required=True)
ap.add_argument("--others", default="1,3")
ap.add_argument("--pings", type=int, default=1500)
ap.add_argument("--rounds", type=int, default=2)
ap.add_argument("--rates", default="1,2")
ap.add_argument("--conds", default="abcd")
a = ap.parse_args()
RATE = {0: "1M", 1: "6M", 2: "24M", 3: "MCS4"}
sp = serial.Serial(a.bridge, 115200, timeout=0.2)
cmd_id = 0


def cmd(text):
    global cmd_id
    cmd_id += 1
    sp.reset_input_buffer()
    sp.write(f"@{cmd_id} {text}\n".encode())
    buf, t = "", time.time()
    while time.time() - t < 2.0 and f"@{cmd_id} ok" not in buf and f"@{cmd_id} err" not in buf:
        buf += sp.read(2048).decode(errors="replace")
    return buf


def weak_state():
    for ln in cmd("ports").splitlines():
        if f" port={a.weak} " in ln:
            f = dict(x.split("=", 1) for x in ln.split()[2:])
            return int(f["rssi"]), int(f["tx_fail"])
    return None, None


W = Port("bridgeESPS3", a.weak)
O = [Port("bridgeESPS3", int(n)) for n in a.others.split(",")]
burst_rx = [0]
orig = W.cb


def wcb(msg, _):
    if msg[0][0] == 0xAE:
        burst_rx[0] += 1
    orig(msg, _)


W.mi.set_callback(wcb)
time.sleep(0.3)
res = {}  # (rate, cond) -> {"rtt": [], "lost": 0, "n": 0, "wfail": 0, "wsent": 0, "wrx": 0, "wlost": 0, "wrtt": []}


def run(rate, cond):
    r = res.setdefault((rate, cond), {"rtt": [], "lost": 0, "n": 0, "wfail": 0, "wsent": 0, "wrx": 0, "wlost": 0, "wrtt": []})
    stop = threading.Event()
    _, f0 = weak_state()
    burst_rx[0] = 0

    def load():
        k = 0
        while not stop.is_set():
            if cond == "b":
                W.mo.send_message([0xB0, 1, k & 0x7F])  # a message the C6 application ignores
                r["wsent"] += 1
                k += 1
                time.sleep(0.005)
            elif cond == "c":
                W.mo.send_message([CTRL, 118, 100])  # have it send 1000 messages
                time.sleep(0.02)
                W.mo.send_message([CTRL, 119, 5])    # every 5 ms (5 s worth; re-issued before it runs out)
                r["wsent"] += 1000
                stop.wait(4.9)
            elif cond == "d":
                rt, lost = W.ping(50, 4, 6, 0.2)
                r["wrtt"] += rt
                r["wlost"] += lost
            else:
                stop.wait(0.2)

    th = threading.Thread(target=load)
    th.start()
    time.sleep(0.3)
    out = {}
    ths = [threading.Thread(target=lambda p=p: out.__setitem__(p.n, p.ping(a.pings, 5, 10, 0.2))) for p in O]
    [t.start() for t in ths]
    [t.join() for t in ths]
    stop.set()
    th.join()
    if cond == "c":
        W.mo.send_message([CTRL, 118, 1])  # stop the rest (finish with 10 messages)
        time.sleep(0.02)
        W.mo.send_message([CTRL, 119, 5])
        time.sleep(0.6)
        r["wrx"] += burst_rx[0]
    for p in O:
        rt, lost = out[p.n]
        r["rtt"] += rt
        r["lost"] += lost
        r["n"] += a.pings
    _, f1 = weak_state()
    if f0 is not None and f1 is not None and f1 >= f0:
        r["wfail"] += f1 - f0


NAME = {"a": "a. 何もしない", "b": "b. PC → 届きにくい台", "c": "c. 届きにくい台 → PC", "d": "d. 届きにくい台と往復"}
for rate in [int(x) for x in a.rates.split(",")]:
    cmd(f"port {a.weak} set phy_rate {rate}")
    time.sleep(1)
    rssi, _ = weak_state()
    print(f"== 届きにくい台（Port {a.weak}）の速さ {RATE[rate]}、受信の強さ {rssi}dBm", flush=True)
    for rd in range(a.rounds):
        for cond in a.conds:
            run(rate, cond)
    for cond in a.conds:
        r = res[(rate, cond)]
        x = r["rtt"]
        extra = f"ブリッジ → 届きにくい台で届かなかった数 {r['wfail']}"
        if cond == "c":
            extra += f"、届きにくい台から PC に届いた数 約 {r['wrx']}"
        if cond == "d":
            w = r["wrtt"]
            extra += f"、届きにくい台の往復 n={len(w)} p50={pct(w, .5):.2f} p95={pct(w, .95):.2f} 取りこぼし={r['wlost']}"
        print(f"  {NAME[cond]:<16} ほかの台: n={len(x)} p50={pct(x, .5):.2f} p95={pct(x, .95):.2f} p99={pct(x, .99):.2f} "
              f"p99.9={pct(x, .999):.2f} max={max(x):.1f}ms 取りこぼし={r['lost']}/{r['n']} | {extra}", flush=True)
cmd(f"port {a.weak} set phy_rate 1")
