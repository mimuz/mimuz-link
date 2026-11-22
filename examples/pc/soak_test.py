#!/usr/bin/env python3
#
#  soak_test.py long-running stability test with several clients
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/21 : Start Writing
#  - 2026/11/21 : Record heap and recovery counters; reopen MIDI ports after a bridge restart
#  - 2026/11/22 : Time round trips with mlping (C, CoreMIDI) instead of Python threads; record the radio channel
#
"""Long-running stability test for mimuz-link with several clients connected.

The test runs in windows (default 60 s) until the end time:
  - every window: round trips to all ports at the same time (10-20 ms gaps)
  - every --note-every windows: each client sends Note On / Off pairs; counts and held notes are checked
  - every --burst-every windows: each client sends 1270 messages at 1 ms intervals (high packet rate)
  - every window: bridge port list (signal strength, undelivered count) and statistics (including heap and uptime),
    and the statistics of the clients given with --client-serial (radio restarts, duplicates)
If the bridge restarted (uptime went down), the MIDI ports are opened again.
One JSON line per window is appended to the log; a short summary line is printed.

usage: soak_test.py --bridge /dev/cu.usbmodemXXXX --ports 1,2,3,4,5,6 --until 05:45 --log soak.jsonl
                    [--client-serial /dev/cu.usbmodemA,/dev/cu.usbmodemB]
"""
import argparse
import datetime
import json
import re
import time

import serial

from multi_test import CTRL, Port, pct, ping_ports

BURST = 0xAE  # burst messages sent by the test client (PolyAT ch15)


def read_serial(path, cmds, wait=1.2):
    """Send raw characters / command lines to a serial port and return the text that comes back."""
    try:
        s = serial.Serial(path, 115200, timeout=0.2)
        time.sleep(0.3)
        s.reset_input_buffer()
        out = ""
        for c in cmds:
            s.write(c.encode())
            t = time.time()
            while time.time() - t < wait:
                out += s.read(4096).decode(errors="replace")
        s.close()
        return out
    except Exception as e:  # the port may be busy or gone; keep the test running
        return f"SERIAL_ERROR {e}"


def bridge_state(path):
    out = read_serial(path, ["@1 ports\n", "@2 info\n", "s"], wait=1.5)
    ports = {}
    for ln in out.splitlines():
        if ln.startswith("@1 + port="):
            f = dict(x.split("=", 1) for x in ln.split()[2:])
            ports[f["port"]] = {"mac": f["mac"], "rssi": int(f["rssi"]), "tx_fail": int(f["tx_fail"]), "idle_ms": int(f["idle_ms"])}
    stats = {}
    for key, pat in [("espnowRestarts", r"espnowRestarts=(\d+)"), ("wifiRestarts", r"wifiRestarts=(\d+)"),
                     ("dupDropped", r"duplicate packets dropped=(\d+)"), ("usbFail", r"usbFail=(\d+)"),
                     ("usbRetry", r"usbRetry=(\d+)"), ("rxqDrop", r"rxqDrop=(\d+)"), ("txTimeouts", r"timeouts=(\d+)"),
                     ("seqGap", r"seqGap\S*=(\d+)"), ("txSkipped", r"skipped=(\d+)"),
                     ("driverReinits", r"driverReinits=(\d+)"), ("stallReboots", r"stallReboots=(\d+)"),
                     ("heapFree", r"heap: free=(\d+)"), ("heapMin", r"heap: free=\d+ min=(\d+)"),
                     ("intFree", r"internal free=(\d+)"), ("intMin", r"internal free=\d+ min=(\d+)"),
                     ("intLargest", r"internal free=\d+ min=\d+ largest=(\d+)"), ("uptime", r"uptime=(\d+)s"),
                     ("noMemRetry", r"noMemRetry=(\d+)"), ("channel", r"radio_channel=(\d+)")]:
        m = re.search(pat, out)
        if m:
            stats[key] = int(m.group(1))
    return ports, stats, ("SERIAL_ERROR" in out)


def client_state(path):
    out = read_serial(path, ["s"], wait=1.2)
    st = {}
    for key, pat in [("espnowRestarts", r"espnowRestarts=(\d+)"), ("wifiRestarts", r"wifiRestarts=(\d+)"),
                     ("dupDropped", r"duplicate packets dropped=(\d+)"), ("connected", r"connected=(\d)"),
                     ("port", r" port=(\d+)"), ("sendErr", r"sendErr=(\d+)"), ("evDrop", r"evDrop=(\d+)")]:
        m = re.search(pat, out)
        if m:
            st[key] = int(m.group(1))
    if "ready (" in out or "ESP-ROM" in out:
        st["rebooted"] = 1
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bridge", required=True, help="bridge serial port")
    ap.add_argument("--ports", default="1,2,3,4,5,6")
    ap.add_argument("--name", default="bridgeESPS3")
    ap.add_argument("--until", required=True, help="end time HH:MM (today, or tomorrow if already past)")
    ap.add_argument("--window", type=float, default=60, help="seconds per window")
    ap.add_argument("--note-every", type=int, default=5)
    ap.add_argument("--burst-every", type=int, default=15)
    ap.add_argument("--client-serial", default="", help="comma-separated client serial ports to read statistics from")
    ap.add_argument("--log", required=True)
    a = ap.parse_args()

    now = datetime.datetime.now()
    hh, mm = map(int, a.until.split(":"))
    end = now.replace(hour=hh, minute=mm, second=0, microsecond=0)
    if end <= now:
        end += datetime.timedelta(days=1)
    clients = [c for c in a.client_serial.split(",") if c]

    nums = [int(n) for n in a.ports.split(",")]
    burst_rx = {n: 0 for n in nums}

    def open_ports(timeout=120):
        t0 = time.time()
        while True:
            try:
                ps = [Port(a.name, n) for n in nums]
                break
            except SystemExit:  # ports not there yet (bridge restarting)
                if time.time() - t0 > timeout:
                    raise
                time.sleep(2)
        for p in ps:  # also count burst messages
            orig = p.cb

            def cb(msg, d, p=p, orig=orig):
                if msg[0] and msg[0][0] == BURST:
                    burst_rx[p.n] += 1
                orig(msg, d)

            p.mi.set_callback(cb)
        time.sleep(0.3)
        for p in ps:  # start from a clean state on every client
            p.ctrl(107, 0)
            p.ctrl(106, 0)
            p.ctrl(109, 0)
        return ps

    P = open_ports()
    last_uptime = None

    print(f"soak test until {end:%Y-%m-%d %H:%M}, ports {a.ports}, log {a.log}", flush=True)
    w = 0
    while datetime.datetime.now() < end:
        w += 1
        t_start = time.time()
        rec = {"window": w, "time": datetime.datetime.now().isoformat(timespec="seconds")}

        # 1) simultaneous round trips
        ping_secs = a.window - 15
        res = {}

        try:
            for n, (rtts, lost) in ping_ports(a.name, nums, secs=ping_secs, gmin=10, gmax=20, timeout=0.2).items():
                res[n] = (rtts, lost, len(rtts) + lost)
        except RuntimeError as e:  # ports not there (bridge restarting): count the window as lost
            rec["ping_error"] = str(e)
            for n in nums:
                res[n] = ([], 1, 1)
            time.sleep(ping_secs)
        rec["ping"] = {}
        all_rtt = []
        for p in P:
            rtts, lost, n = res[p.n]
            all_rtt += rtts
            rec["ping"][p.n] = {"n": n, "lost": lost, "p50": round(pct(rtts, .5), 3), "p95": round(pct(rtts, .95), 3),
                                "p99": round(pct(rtts, .99), 3), "max": round(max(rtts), 3) if rtts else None}
        rec["stall_all"] = all(res[p.n][1] > res[p.n][2] * 0.5 for p in P)

        # 2) Note On / Off pairs from every client
        if w % a.note_every == 0:
            pairs = 200
            for p in P:
                p.reset()
                p.ctrl(118, pairs // 10, 0.02)
            for p in P:
                p.mo.send_message([CTRL, 117, 5])
            time.sleep(pairs * 2 * 5 / 1000 + 2.5)
            rec["notes"] = {p.n: {"on": p.on, "off": p.off, "held": len(p.held)} for p in P}

        # 3) high packet rate burst from every client
        if w % a.burst_every == 0:
            for p in P:
                burst_rx[p.n] = 0
                p.ctrl(118, 127, 0.02)
            for p in P:
                p.mo.send_message([CTRL, 119, 1])
            time.sleep(1.27 + 2.5)
            rec["burst"] = {p.n: burst_rx[p.n] for p in P}

        # 4) bridge and client state
        ports, stats, err = bridge_state(a.bridge)
        rec["bridge_ports"] = ports
        rec["bridge_stats"] = stats
        rec["bridge_serial_error"] = err
        rec["clients"] = {c: client_state(c) for c in clients}
        up = stats.get("uptime")
        if up is not None and last_uptime is not None and up < last_uptime:
            rec["bridge_restarted"] = True
            for p in P:
                p.mi.close_port()
                p.mo.close_port()
            time.sleep(20)  # channel survey and client reconnection
            P = open_ports()
        if up is not None:
            last_uptime = up

        with open(a.log, "a") as f:
            f.write(json.dumps(rec) + "\n")
        lost_total = sum(v["lost"] for v in rec["ping"].values())
        n_total = sum(v["n"] for v in rec["ping"].values())
        line = (f"{rec['time']} w{w} ports={len(ports)} ping p50={pct(all_rtt, .5):.2f} p99={pct(all_rtt, .99):.2f} "
                f"max={max(all_rtt) if all_rtt else 0:.1f} lost={lost_total}/{n_total}")
        if "notes" in rec:
            bad = [k for k, v in rec["notes"].items() if v["on"] != 200 or v["off"] != 200 or v["held"]]
            line += f" notes={'OK' if not bad else 'NG' + str(bad)}"
        if "burst" in rec:
            line += f" burst={min(rec['burst'].values())}-{max(rec['burst'].values())}/1270"
        line += (f" ch={stats.get('channel', '?')} restarts={stats.get('espnowRestarts', '?')}/{stats.get('wifiRestarts', '?')}/{stats.get('driverReinits', '?')}"
                 f"/{stats.get('stallReboots', '?')} heap={stats.get('intFree', '?')}/{stats.get('intMin', '?')}")
        if rec.get("bridge_restarted"):
            line += " BRIDGE_RESTARTED"
        if rec["stall_all"]:
            line += " STALL_ALL"
        print(line, flush=True)
        rest = a.window - (time.time() - t_start)
        if rest > 0:
            time.sleep(rest)
    print("done", flush=True)


if __name__ == "__main__":
    main()
