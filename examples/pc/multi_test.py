#!/usr/bin/env python3
#
#  multi_test.py checks with several clients connected at once
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#  - 2026/11/21 : Add --only-b (run only the simultaneous round-trip test)
#  - 2026/11/22 : Time round trips with mlping (C, CoreMIDI) instead of Python threads; add MlPing / ping_ports for the other tools
#  - 2026/11/22 : MlPing channel mode (one MIDI port shared by all clients, told apart by MIDI channel)
#
"""Checks with several clients (C6) connected at the same time.

usage: multi_test.py [--ports 1,2,3] [--name bridgeESPS3]
  A. Round trip per port (one client at a time), and check that nothing leaks to other ports
  B. Round trip on all ports at once
  C. All clients send at the same time (Note On/Off pairs)
  D. Delivery between clients (receive channels), and check that messages from the PC are not relayed
  E. Delivery of messages without a channel
"""
import argparse, os, signal, subprocess, threading, time
import rtmidi

PING, CTRL = 0xAF, 0xBF
SYS_SET = [[0xFA], [0xF8], [0xF8], [0xF8], [0xF2, 0x12, 0x34], [0xFB], [0xF8], [0xF3, 0x05], [0xF1, 0x23], [0xF6], [0xFE], [0xFC]]


def pct(xs, p):
    s = sorted(xs)
    return s[min(len(s) - 1, max(0, int(p * len(s) + 0.999999) - 1))] if s else float("nan")


def stat(xs):
    return f"n={len(xs)} p50={pct(xs, .5):.2f} p95={pct(xs, .95):.2f} p99={pct(xs, .99):.2f} max={max(xs):.2f}ms" if xs else "no data"


MLPING = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mlping", "mlping")


class MlPing:
    """Round trips timed by mlping (mlping/mlping.c: CoreMIDI, real-time sender thread), so that Python's thread scheduling is
    not part of the times. Every port given keeps one ping out and waits a random gap (gmin..gmax ms) after each reply.
    It runs in the background from the constructor until secs have passed, count pings were sent on every port (0 = no
    limit), or stop() is called. Round trips are measured up to the CoreMIDI time stamp of the reply.
    Times in rec are ns of the host clock, the same as time.clock_gettime_ns(time.CLOCK_UPTIME_RAW).
    channels=True: nums are MIDI channels on one port shared by all clients (see mlping.c, channel mode)."""

    def __init__(self, name, nums, secs=0, count=0, gmin=5, gmax=10, timeout=0.2, channels=False):
        if not os.path.exists(MLPING):
            raise SystemExit(f"build {MLPING} first (see mlping/mlping.c)")
        self.nums = [int(n) for n in nums]
        self.rec = {}
        self.pr = subprocess.Popen([MLPING, name, ",".join(map(str, self.nums)), str(secs if secs > 0 else 86400), str(gmin),
                                    str(gmax), str(timeout * 1000), str(PING), str(count)] + (["ch"] if channels else []),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def stop(self):
        if self.pr.poll() is None:
            self.pr.send_signal(signal.SIGINT)
        return self.wait()

    def wait(self):
        """Waits for the end and reads the results: rec = {port: [(key, send_ns, recv_ts_ns, recv_cb_ns, late_ns), ...]}
        (recv 0 = lost). Raises RuntimeError if mlping failed (e.g. a port is not there)."""
        out, err = self.pr.communicate()
        if self.pr.returncode != 0:
            raise RuntimeError(f"mlping failed: {err.strip()}")
        self.rec = {n: [] for n in self.nums}
        for ln in out.splitlines()[1:]:
            n, *v = map(int, ln.split(","))
            self.rec[n].append(tuple(v))
        return self

    def result(self, n):
        """(round trips in ms, number lost) of port n."""
        r = self.rec.get(n, [])
        return [(ts - s) / 1e6 for _, s, ts, _, _ in r if ts], sum(1 for x in r if not x[2])


def ping_ports(name, nums, **kw):
    """Round trips on several ports at the same time (see MlPing); returns {port: (round trips in ms, number lost)}."""
    m = MlPing(name, nums, **kw).wait()
    return {n: m.result(n) for n in m.nums}


class Port:
    def __init__(self, name, n):
        self.name = name
        self.n = n
        self.mi, self.mo = rtmidi.MidiIn(), rtmidi.MidiOut()
        self.mi.ignore_types(sysex=True, timing=False, active_sense=False)
        key = f"{name}".lower()
        for dev in (self.mi, self.mo):
            ps = dev.get_ports()
            idx = [i for i, p in enumerate(ps) if key in p.lower() and p.strip().endswith(f"Port {n}")]
            if not idx:
                raise SystemExit(f"Port {n} not found: {ps}")
            dev.open_port(idx[0])
        self.reset()
        self.ev = threading.Event()
        self.want = -1
        self.mi.set_callback(self.cb)

    def reset(self):
        self.on = self.off = self.pong = 0
        self.held = set()
        self.sys = []
        self.info = {}
        self.sysn = None

    def cb(self, msg, _):
        t = time.perf_counter_ns()
        m = msg[0]
        if m[0] >= 0xF0:
            self.sys.append(list(m))
            return
        if len(m) != 3:
            return
        h = m[0] & 0xF0
        if m[0] == PING:
            self.pong += 1
            if (m[1] | (m[2] << 7)) == self.want:
                self.t = t
                self.ev.set()
        elif m[0] == CTRL:
            if 110 <= m[1] <= 114:
                self.info[m[1]] = m[2]
            elif m[1] == 103:
                self.sysn = m[2]
        elif h == 0x90 and m[2] > 0:
            self.on += 1
            self.held.add(m[1])
        elif h == 0x80 or h == 0x90:
            self.off += 1
            self.held.discard(m[1])

    def ctrl(self, cc, v, wait=0.05):
        self.mo.send_message([CTRL, cc, v])
        time.sleep(wait)

    def ping(self, count, gmin=5, gmax=10, timeout=0.5):
        """count round trips on this port alone (timed by mlping); returns (round trips in ms, number lost)."""
        return ping_ports(self.name, [self.n], count=count, gmin=gmin, gmax=gmax, timeout=timeout)[self.n]

    def query(self):
        """Number of Note On / Off messages the client received, and the number of notes still held."""
        self.info = {}
        self.ctrl(122, 0, 0.5)
        i = self.info
        if not all(k in i for k in (110, 111, 112, 113, 114)):
            return None
        return {"held": i[110], "on": i[111] | (i[112] << 7), "off": i[113] | (i[114] << 7)}

    def sys_count(self):
        self.sysn = None
        self.ctrl(104, 3, 0.4)
        return self.sysn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", default="1,2,3")
    ap.add_argument("--name", default="bridgeESPS3")
    ap.add_argument("--pings", type=int, default=1000)
    ap.add_argument("--pairs", type=int, default=500)
    ap.add_argument("--period", type=int, default=5, help="C6 が送る間隔 [ms]")
    ap.add_argument("--only-b", action="store_true", help="run only section B (simultaneous round trips)")
    a = ap.parse_args()
    P = [Port(a.name, int(n)) for n in a.ports.split(",")]
    time.sleep(0.3)
    ok_all = True

    def verdict(title, ok, detail=""):
        nonlocal ok_all
        ok_all &= bool(ok)
        print(f"  [{'OK' if ok else 'NG'}] {title} {detail}")

    def clean():
        for p in P:
            p.ctrl(107, 0); p.ctrl(106, 0); p.ctrl(104, 0); p.ctrl(109, 0)
        time.sleep(0.3)
        for p in P:
            p.reset()

    clean()
    if a.only_b:
        print(f"== B. 全ポート同時の往復（各 {a.pings} 回、5〜10ms 間隔）")
        res = ping_ports(a.name, [p.n for p in P], count=a.pings, timeout=0.5)
        for p in P:
            r, lost = res[p.n]
            verdict(f"Port {p.n}", lost == 0, f"{stat(r)} 取りこぼし={lost}")
        return

    print(f"== A. ポートごとの往復（1 台ずつ、各 {a.pings // 5} 回）")
    for p in P:
        for q in P:
            q.pong = 0
        r, lost = p.ping(a.pings // 5)
        leak = sum(q.pong for q in P if q is not p)
        verdict(f"Port {p.n}", lost == 0 and leak == 0, f"{stat(r)} 取りこぼし={lost} ほかのポートへの漏れ={leak}")

    print(f"== B. 全ポート同時の往復（各 {a.pings} 回、5〜10ms 間隔）")
    res = ping_ports(a.name, [p.n for p in P], count=a.pings, timeout=0.5)
    for p in P:
        r, lost = res[p.n]
        verdict(f"Port {p.n}", lost == 0, f"{stat(r)} 取りこぼし={lost}")

    print(f"== C. 全クライアントが同時に送る（Note On/Off を各 {a.pairs} 組、{a.period}ms 間隔）")
    clean()
    for p in P:
        p.ctrl(118, a.pairs // 10, 0.02)
    for p in P:
        p.mo.send_message([CTRL, 117, a.period])
    time.sleep(a.pairs * 2 * a.period / 1000 + 2.5)
    for p in P:
        verdict(f"Port {p.n}", p.on == a.pairs and p.off == a.pairs and not p.held, f"On={p.on}/{a.pairs} Off={p.off}/{a.pairs} 鳴りっぱなし={len(p.held)}")

    if len(P) >= 2:
        src, listeners = P[0], P[1:]
        n = 100
        print(f"== D1. 配信: Port {src.n} のクライアントが送る → チャンネル 1 を聞いている {[q.n for q in listeners]} に届く（{n} 組）")
        clean()
        for q in listeners:
            q.ctrl(107, 1, 0.2)
        src.ctrl(118, n // 10, 0.02)
        src.ctrl(117, a.period)
        time.sleep(n * 2 * a.period / 1000 + 2)
        verdict(f"PC（Port {src.n}）", src.on == n and src.off == n, f"On={src.on} Off={src.off}")
        for q in listeners:
            verdict(f"PC（Port {q.n}）には出ない", q.on == 0 and q.off == 0, f"On={q.on} Off={q.off}")
        for p in P:
            c = p.query()
            exp = 0 if p is src else n
            verdict(f"Port {p.n} のクライアントが受けた数", c and c["on"] == exp and c["off"] == exp and c["held"] == 0, f"{c}（期待 On={exp} Off={exp}）")

        print(f"== D2. 配信: 全員がチャンネル 1 を聞き、全員が同時に送る（各 {n} 組）")
        clean()
        for p in P:
            p.ctrl(107, 1, 0.2)
        for p in P:
            p.ctrl(118, n // 10, 0.02)
        for p in P:
            p.mo.send_message([CTRL, 117, a.period])
        time.sleep(n * 2 * a.period / 1000 + 2.5)
        exp = n * (len(P) - 1)
        for p in P:
            c = p.query()
            verdict(f"Port {p.n} のクライアントが受けた数", c and c["on"] == exp and c["off"] == exp, f"{c}（期待 On={exp} Off={exp}。自分のぶんは戻らない）")
            verdict(f"PC（Port {p.n}）", p.on == n and p.off == n, f"On={p.on} Off={p.off}")

        print(f"== D3. PC からのメッセージは配信しない: PC → Port {listeners[0].n} だけに 50 組（全員がチャンネル 1 を聞いている）")
        for p in P:
            p.ctrl(109, 0)
        time.sleep(0.2)
        for i in range(50):
            listeners[0].mo.send_message([0x90, 40 + i % 40, 100]); time.sleep(0.004)
            listeners[0].mo.send_message([0x80, 40 + i % 40, 0]); time.sleep(0.004)
        time.sleep(0.5)
        for p in P:
            c = p.query()
            exp = 50 if p is listeners[0] else 0
            verdict(f"Port {p.n} のクライアントが受けた数", c and c["on"] == exp and c["off"] == exp, f"{c}（期待 {exp}）")

        print(f"== E. チャンネルを持たないメッセージ: Port {src.n} のクライアントが 12 件送る。Port {listeners[0].n} はチャンネル 5 を聞く。ほかは聞かない")
        clean()
        listeners[0].ctrl(107, 5, 0.3)
        src.ctrl(104, 2, 0.8)
        verdict(f"PC（Port {src.n}）", src.sys == SYS_SET, f"{len(src.sys)}/12 件")
        for p in P:
            c = p.sys_count()
            exp = 12 if p is listeners[0] else 0
            verdict(f"Port {p.n} のクライアントが受けた数", c == exp, f"{c}（期待 {exp}）")
            if p is not src:
                verdict(f"PC（Port {p.n}）には出ない", not p.sys, f"{len(p.sys)} 件")
    clean()
    print("総合:", "OK" if ok_all else "NG")


if __name__ == "__main__":
    main()
