#!/usr/bin/env python3
#
#  port_watch.py watch port release and rejoin on the bridge
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#
"""Watch ports being released and rejoining.

usage: port_watch.py BRIDGE_SERIAL [--secs 240] [--ports 1,2,3]
  - Reads the bridge's `ports` every second and prints, with a timestamp, whenever the set of connected ports changes.
  - At the same time, keeps sending round-trip pings to each port every 20 ms and reports when replies stop / resume / are late (over 20 ms).
"""
import argparse, threading, time
import serial
from multi_test import Port, PING

ap = argparse.ArgumentParser()
ap.add_argument("bridge")
ap.add_argument("--secs", type=float, default=240)
ap.add_argument("--ports", default="1,2,3")
a = ap.parse_args()
T0 = time.time()
lock = threading.Lock()


def say(s):
    with lock:
        print(f"{time.time() - T0:7.1f}s  {s}", flush=True)


def pinger(p):
    alive, lost_run, n, slow, lost_total, down_at = None, 0, 0, 0, 0, None
    while time.time() - T0 < a.secs:
        p.ev.clear()
        p.want = n & 0x3FFF
        t0 = time.perf_counter_ns()
        p.mo.send_message([PING, p.want & 0x7F, p.want >> 7])
        n += 1
        if p.ev.wait(0.2):
            r = (p.t - t0) / 1e6
            if alive is not True:
                say(f"Port {p.n}: 返事あり" + (f"（止まっていた時間 {time.time() - down_at:.1f} 秒）" if down_at else ""))
                alive, down_at = True, None
            lost_run = 0
            if r > 20:
                slow += 1
                say(f"Port {p.n}: 遅れ {r:.1f}ms")
            time.sleep(0.02)
        else:
            lost_run += 1
            if alive:
                lost_total += 1
            if lost_run == 3 and alive is not False:
                say(f"Port {p.n}: 返事なし")
                alive, down_at = False, time.time() - 0.6
    say(f"Port {p.n}: 終了。送った数 {n}、遅れ（20ms 超）{slow} 回")


def watcher():
    sp = serial.Serial(a.bridge, 115200, timeout=0.2)
    last, k = None, 0
    while time.time() - T0 < a.secs:
        k += 1
        sp.reset_input_buffer()
        sp.write(f"@{k} ports\n".encode())
        buf, t = "", time.time()
        while time.time() - t < 1.0 and f"@{k} ok" not in buf:
            buf += sp.read(2048).decode(errors="replace")
        cur = []
        for ln in buf.splitlines():
            if ln.startswith(f"@{k} + port="):
                f = dict(x.split("=", 1) for x in ln.split()[2:])
                cur.append(f"{f['port']}={f['mac'][-5:]}")
        cur = " ".join(cur) or "（なし）"
        if f"@{k} ok" in buf and cur != last:
            say(f"ブリッジの接続中のポート: {cur}")
            last = cur
        time.sleep(1.0)


P = [Port("bridgeESPS3", int(n)) for n in a.ports.split(",")]
ths = [threading.Thread(target=pinger, args=(p,)) for p in P] + [threading.Thread(target=watcher)]
[t.start() for t in ths]
[t.join() for t in ths]
