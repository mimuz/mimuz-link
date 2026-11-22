#!/usr/bin/env python3
#
#  trace_test.py split round trips into the bridge / radio part and the PC / USB part
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/22 : Start Writing
#  - 2026/11/22 : Split the PC side further (CoreMIDI arrival vs. Python callback, sleep overshoot, Python settings per round)
#  - 2026/11/22 : Time the round trips with mlping (C, CoreMIDI) by default, so that Python's thread scheduling is not included
#  - 2026/11/22 : Show the bridge's late radio sends (slowtx) grouped by time, with the slow pings that fall in each group
#  - 2026/11/22 : Change bridge settings per round (--bridge-set), to compare them in turn
#  - 2026/11/22 : Send any bridge commands per round (--bridge-cmd), e.g. to change client settings through the bridge
#
"""Split round trips to several clients into the part spent in the bridge (and on the radio) and the part spent on the PC side.

Each round, round trips run to all ports at the same time (like soak_test.py). Every ping carries its own number
(data bytes), so the bridge, with trace_status set to the ping's status byte, can time it from its USB arrival until the
reply has been written to USB (total_us), and until the reply arrived over the radio (radio_us).
After each round the bridge's 16 slowest traced pings are read and matched by port and number with the round trips
measured here:
  PC side = round trip on the PC - total_us   (macOS, USB in both directions, Python)
For the slowest round trips on the PC that are not among the bridge's 16 slowest, the bridge part is at most the
16th slowest total_us, so the PC side is at least the round trip minus that bound.

The PC side is split further:
  cb_lag   how late the Python callback ran compared with the time stamp CoreMIDI gave the reply (relative: the smallest
           value in the round is taken as 0). Large = the reply had reached macOS but Python ran its callback late.
  sleep    the largest overshoot of a thread that only sleeps 1 ms at a time, while the ping was out. Large = the whole
           Python process (or macOS) did not run.
Each round can use different Python settings (--switch, --gc, cycled per round) to see whether they change the result.

With --engine c (default) the round trips are sent and timed by mlping (mlping/mlping.c, CoreMIDI, real-time thread),
so Python's thread scheduling is not part of the times. The round trip is then up to the CoreMIDI time stamp of the
reply, cb_lag is the absolute time from that stamp until mlping's receive callback ran, and "sleep" is replaced by
"late": how much later than planned mlping's sender woke up to send that ping. --engine python keeps the Python sender
for comparison.

After each round the bridge's late radio sends (slowtx: packets whose result came --slow-tx ms or more after they were
handed to the radio) are read and grouped when they overlap in time. For each group the packets are listed in the
order they were handed to the radio (port, start relative to the group, time until the result, delivered or not),
with the bridge's slowest traced pings whose USB arrival falls in that time. A group that starts with an undelivered
packet to a far client points at the radio retrying that packet; one with only delivered packets points elsewhere.

--bridge-set "retry_limit 0;retry_limit 4" sends `set <key> <value>` to the bridge before each round, taking the
entries in turn (round 1 the first, round 2 the second, ...), so that settings can be compared under the same conditions.
--bridge-cmd works the same way with whole commands, several in one entry separated by '|', e.g.
"port 4 set retry_limit 0|port 5 set retry_limit 0;port 4 set retry_limit 8|port 5 set retry_limit 8".

usage: trace_test.py --bridge /dev/cu.usbmodemXXXX [--ports 1,2,3,4,5,6] [--secs 45] [--rounds 5] [--log trace.jsonl]
                     [--switch 0.005,0.0005] [--gc on,off] [--slow 8]
"""
import argparse
import bisect
import gc
import json
import os
import random
import re
import sys
import threading
import time

from multi_test import MLPING, PING, MlPing, Port, pct
from soak_test import read_serial


class Arrivals:
    """Python callback time and accumulated CoreMIDI time stamp of every reply on one port."""

    def __init__(self, p):
        self.ts = 0.0
        self.got = {}
        orig = p.cb

        def cb(msg, d):
            t = time.perf_counter_ns()
            self.ts += msg[1]
            m = msg[0]
            if m and m[0] == PING and len(m) == 3:
                self.got[m[1] | (m[2] << 7)] = (t, self.ts)
            orig(msg, d)

        p.mi.set_callback(cb)

    def clear(self):
        self.got = {}


class Sleeper(threading.Thread):
    """Sleeps 1 ms at a time and records how much longer each sleep took."""

    def __init__(self):
        super().__init__(daemon=True)
        self.t, self.over = [], []
        self.run_ = True

    def run(self):
        while self.run_:
            s = time.perf_counter_ns()
            time.sleep(0.001)
            e = time.perf_counter_ns()
            self.t.append(e)
            self.over.append((e - s) / 1e6 - 1.0)

    def worst(self, a, b):
        i, j = bisect.bisect_left(self.t, a), bisect.bisect_right(self.t, b)
        return max(self.over[i:j + 1], default=0.0)


def ping_run(p, secs, gmin, gmax, timeout, out):
    """Round trips with increasing numbers; returns {number: (send time ns, rtt_ms)} and the number lost."""
    rtt, lost, k = {}, 0, 0
    t0 = time.time()
    while time.time() - t0 < secs:
        key = k & 0x3FFF
        k += 1
        p.ev.clear()
        p.want = key
        s = time.perf_counter_ns()
        p.mo.send_message([PING, key & 0x7F, key >> 7])
        if p.ev.wait(timeout):
            rtt[key] = (s, (p.t - s) / 1e6)
        else:
            lost += 1
        time.sleep(random.uniform(gmin, gmax) / 1000)
    out[p.n] = (rtt, lost)


def ping_round_c(a, gmin, gmax):
    """One round with mlping. Returns res like ping_run, absolute callback lag and send lateness per (port, key)."""
    m = MlPing(a.name, a.ports.split(","), secs=a.secs, gmin=gmin, gmax=gmax, timeout=0.2).wait()
    res, lag, late = {}, {}, {}
    for n, recs in m.rec.items():
        rtt, lost = {}, 0
        for k, s, ts, cb, lt in recs:
            if ts:
                rtt[k] = (s, (ts - s) / 1e6)
                lag.setdefault(n, {})[k] = (cb - ts) / 1e6
            else:
                lost += 1
            late[(n, k)] = lt / 1e6
        res[n] = (rtt, lost)
    return res, lag, late


def bridge_trace(path):
    out = read_serial(path, ["@1 trace\n"], wait=1.0)
    top, summ = [], {}
    for ln in out.splitlines():
        if ln.startswith("@1 + port="):
            f = dict(x.split("=", 1) for x in ln.split()[2:])
            top.append({"port": int(f["port"]), "key": int(f["key"]), "total": int(f["total_us"]) / 1000,
                        "radio": int(f["radio_us"]) / 1000, "in_us": int(f.get("in_us", 0))})
        elif ln.startswith("@1 ok count="):
            summ = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", ln)}
    return top, summ


def bridge_slowtx(path):
    out = read_serial(path, ["@1 slowtx\n"], wait=1.0)
    ev = []
    for ln in out.splitlines():
        if ln.startswith("@1 + port="):
            f = dict(x.split("=", 1) for x in ln.split()[2:])
            ev.append({k: int(v) for k, v in f.items()})
    return ev


def group_slowtx(ev, gap_us=2000):
    """Groups of late sends that overlap in time (or follow within gap_us); 32-bit microsecond clock."""
    ev = sorted(ev, key=lambda e: e["sent_us"])
    groups, end = [], None
    for e in ev:
        if groups and e["sent_us"] <= end + gap_us:
            groups[-1].append(e)
            end = max(end, e["sent_us"] + e["air_us"])
        else:
            groups.append([e])
            end = e["sent_us"] + e["air_us"]
    return groups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bridge", required=True, help="bridge serial port")
    ap.add_argument("--ports", default="1,2,3,4,5,6")
    ap.add_argument("--name", default="bridgeESPS3")
    ap.add_argument("--secs", type=float, default=45, help="seconds of round trips per round")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--gap", default="10,20", help="gap between round trips per port, min,max [ms]")
    ap.add_argument("--switch", default="", help="Python thread switch intervals [s], cycled per round (default: leave as is)")
    ap.add_argument("--gc", default="on", help="garbage collection on/off, cycled per round")
    ap.add_argument("--slow", type=float, default=8.0, help="round trips at least this long [ms] are broken down")
    ap.add_argument("--engine", choices=["c", "python"], default="c", help="who sends and times the round trips")
    ap.add_argument("--bridge-set", default="", help="bridge settings 'key value;key value', one per round in turn")
    ap.add_argument("--bridge-cmd", default="", help="bridge commands 'cmd|cmd;cmd|cmd', one entry per round in turn")
    ap.add_argument("--slow-tx", type=int, default=10, help="record radio sends whose result took this long [ms], 0 = off")
    ap.add_argument("--log", default="")
    a = ap.parse_args()
    if a.engine == "c" and not os.path.exists(MLPING):
        raise SystemExit(f"build {MLPING} first (see mlping/mlping.c)")
    gmin, gmax = map(float, a.gap.split(","))
    switches = [float(x) for x in a.switch.split(",") if x] or [sys.getswitchinterval()]
    gcs = [x.strip() == "on" for x in a.gc.split(",")]

    r = read_serial(a.bridge, [f"@1 set trace_status {PING}\n", f"@2 set slow_tx_ms {a.slow_tx}\n"], wait=0.5)
    if "ok trace_status" not in r:
        raise SystemExit(f"bridge did not take trace_status: {r.strip()}")
    if "ok slow_tx_ms" not in r:
        print("note: the bridge does not record late radio sends (slow_tx_ms); update its firmware for that")
    P = [Port(a.name, int(n)) for n in a.ports.split(",")] if a.engine == "python" else []
    arr = {p.n: Arrivals(p) for p in P}
    time.sleep(0.3)
    try:
        for rnd in range(1, a.rounds + 1):
            sw, gc_on = switches[(rnd - 1) % len(switches)], gcs[(rnd - 1) % len(gcs)]
            sys.setswitchinterval(sw)
            gc.enable() if gc_on else gc.disable()
            bset = [x.strip() for x in a.bridge_set.split(";") if x.strip()]
            bs = bset[(rnd - 1) % len(bset)] if bset else ""
            if bs:
                r = read_serial(a.bridge, [f"@1 set {bs}\n"], wait=0.3)
                if "@1 ok" not in r:
                    raise SystemExit(f"bridge did not take 'set {bs}': {r.strip()}")
            bcmd = [x.strip() for x in a.bridge_cmd.split(";") if x.strip()]
            bc = bcmd[(rnd - 1) % len(bcmd)] if bcmd else ""
            for i, cmd in enumerate(x.strip() for x in bc.split("|") if x.strip()):
                for _ in range(3):  # a command relayed to a client is resent by the bridge for up to 1.5 s
                    r = read_serial(a.bridge, [f"@{i + 1} {cmd}\n"], wait=2.0)
                    if f"@{i + 1} ok" in r:
                        break
                else:
                    raise SystemExit(f"bridge did not take '{cmd}': {r.strip()}")
            if bc:
                bs = (bs + " | " if bs else "") + bc
            read_serial(a.bridge, ["@1 trace clear\n", "@2 slowtx clear\n"], wait=0.3)
            for x in arr.values():
                x.clear()
            sl = Sleeper()
            sl.start()
            res, late = {}, None
            if a.engine == "c":
                res, lag, late = ping_round_c(a, gmin, gmax)
            else:
                ths = [threading.Thread(target=ping_run, args=(p, a.secs, gmin, gmax, 0.2, res)) for p in P]
                [t.start() for t in ths]
                [t.join() for t in ths]
            sl.run_ = False
            sl.join()
            gc.enable()
            top, summ = bridge_trace(a.bridge)
            all_rtt = [v[1] for n in res for v in res[n][0].values()]
            lost = sum(res[n][1] for n in res)
            bound = min((t["total"] for t in top), default=0.0) if len(top) >= 16 else 0.0
            btop = {(t["port"], t["key"]): t for t in top}
            # callback lag relative to the CoreMIDI time stamp, per port (smallest = 0)
            if a.engine == "python":
                lag = {}
            for n, x in arr.items():
                raw = {k: t / 1e6 - ts * 1e3 for k, (t, ts) in x.got.items()}
                m0 = min(raw.values(), default=0.0)
                lag[n] = {k: v - m0 for k, v in raw.items()}
            sleep_all = max(sl.over, default=0.0)
            lates = list(late.values()) if late else []
            extra = (f"| send late p99={pct(lates, .99):.3f} max={max(lates, default=0):.3f}" if late else
                     f"| sleep overshoot max={sleep_all:.2f} >5ms={sum(o > 5 for o in sl.over)}")
            print(f"round {rnd} ({a.engine}{', ' + bs if bs else ''}, switch={sw * 1000:g}ms gc={'on' if gc_on else 'off'}): PC rtt n={len(all_rtt)} lost={lost} "
                  f"p50={pct(all_rtt, .5):.2f} p99={pct(all_rtt, .99):.2f} max={max(all_rtt):.2f} | bridge total "
                  f"p50={summ.get('p50_us', 0) / 1000:.2f} p99={summ.get('p99_us', 0) / 1000:.2f} "
                  f"max={summ.get('max_us', 0) / 1000:.2f} (traced {summ.get('done', 0)}, missed {summ.get('missed', 0)}) " + extra)
            slow = sorted(((v[1], n, k, v[0]) for n in res for k, v in res[n][0].items() if v[1] >= a.slow), reverse=True)
            print(f"  round trips >= {a.slow:g} ms: {len(slow)}   (bridge <= {bound:.2f} when not among its slowest)")
            print(f"    port  key   pc_rtt  bridge  radio  pc_side  cb_lag  {'late' if late else 'sleep'}")
            rows = []
            for v, n, k, s in slow[:30]:
                b = btop.get((n, k))
                cl = lag[n].get(k)
                so = late.get((n, k), 0.0) if late else sl.worst(s, s + int(v * 1e6))
                bcol = f"{b['total']:7.2f} {b['radio']:6.2f} {v - b['total']:8.2f}" if b else f" <={bound:5.2f}    -- >={v - bound:6.2f}"
                print(f"    {n:4d} {k:5d} {v:7.2f} {bcol} {cl if cl is not None else float('nan'):7.2f} {so:6.2f}")
                rows.append({"port": n, "key": k, "rtt": round(v, 3), "bridge": round(b["total"], 3) if b else None,
                             "radio": round(b["radio"], 3) if b else None, "cb_lag": round(cl, 3) if cl is not None else None,
                             ("late" if late else "sleep"): round(so, 3)})
            groups = group_slowtx(bridge_slowtx(a.bridge)) if a.slow_tx else []
            print(f"  late radio sends (>= {a.slow_tx} ms): {sum(len(g) for g in groups)} in {len(groups)} groups")
            grows = []
            for g in sorted(groups, key=lambda g: -max(e["air_us"] for e in g))[:8]:
                t0 = g[0]["sent_us"]
                t1 = max(e["sent_us"] + e["air_us"] for e in g)
                sends = " ".join(f"p{e['port']}@{(e['sent_us'] - t0) / 1000:.1f}+{e['air_us'] / 1000:.1f}{'' if e['ok'] else '!'}"
                                 + ("r" if e["resend"] else "") + ("T" if e["timeout"] else "") for e in g)
                hit = [t for t in top if t["in_us"] and t0 - 30000 <= t["in_us"] <= t1]
                pings = " ".join(f"p{t['port']}:{t['total']:.1f}" for t in sorted(hit, key=lambda t: t["in_us"]))
                print(f"    {(t1 - t0) / 1000:5.1f} ms: {sends}   pings: {pings or '-'}")
                grows.append({"span_ms": (t1 - t0) / 1000, "sends": g, "pings": hit})
            if a.log:
                rec = {"round": rnd, "time": time.strftime("%Y-%m-%dT%H:%M:%S"), "engine": a.engine, "bridge_set": bs, "switch": sw, "gc": gc_on,
                       "n": len(all_rtt), "lost": lost,
                       "pc": {"p50": pct(all_rtt, .5), "p99": pct(all_rtt, .99), "max": max(all_rtt)},
                       "bridge": summ, "bridge_top": [dict(t, pc_rtt=res.get(t["port"], ({}, 0))[0].get(t["key"], (0, None))[1])
                                                      for t in top],
                       "slow": rows, "bound": bound, "slowtx_groups": grows,
                       "sleep": {"max": sleep_all, "over5": sum(o > 5 for o in sl.over), "n": len(sl.over)},
                       "send_late": {"p99": pct(lates, .99), "max": max(lates, default=0)} if late else None,
                       "per_port": {n: {"p99": pct([v[1] for v in res[n][0].values()], .99),
                                        "max": max((v[1] for v in res[n][0].values()), default=None), "lost": res[n][1]}
                                    for n in res}}
                with open(a.log, "a") as f:
                    f.write(json.dumps(rec) + "\n")
    finally:
        cmds = ["@1 set trace_status 0\n"]
        if a.bridge_set:  # leave the bridge with the first setting given (the reference)
            cmds.append(f"@2 set {a.bridge_set.split(';')[0].strip()}\n")
        if a.bridge_cmd:  # and with the first commands given
            cmds += [f"@{i + 3} {c.strip()}\n" for i, c in enumerate(a.bridge_cmd.split(";")[0].split("|")) if c.strip()]
        read_serial(a.bridge, cmds, wait=0.3)


if __name__ == "__main__":
    main()
