#!/usr/bin/env python3
#
#  latency_test.py ESP-NOW / BLE-MIDI latency and delivery tests over MIDI
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#
"""ESP-NOW / BLE-MIDI latency test (PC side).

Usage:
  python latency_test.py list
  python latency_test.py ping  --port ESPNOW --target usb    # USB leg only (the S3 echoes back)
  python latency_test.py ping  --port ESPNOW --target radio  # PC->S3->C6->S3->PC
  python latency_test.py ping  --port conESP32C6             # PC->BLE->C6->BLE->PC
  python latency_test.py burst --port ESPNOW --period 10 --count 500
  python latency_test.py listen --port ESPNOW                # log of received button messages

--port is part of the port name (case-insensitive). It is used for both input and output.
"""
import argparse
import csv
import random
import sys
import threading
import time

import rtmidi

PING_RADIO = 0xAF  # PolyAT ch16
PING_USB = 0xAD    # PolyAT ch14
BURST = 0xAE       # PolyAT ch15
CTRL = 0xBF        # CC ch16


def open_ports(name):
    mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut()
    mi.ignore_types(sysex=True, timing=True, active_sense=True)

    def pick(dev, kind):
        ports = dev.get_ports()
        for i, p in enumerate(ports):
            if name.lower() in p.lower():
                dev.open_port(i)
                return p
        sys.exit(f"{kind} port '{name}' not found: {ports}")

    pi, po = pick(mi, "input"), pick(mo, "output")
    print(f"in : {pi}\nout: {po}")
    return mi, mo


def pct(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    k = min(len(s) - 1, max(0, int(p * len(s) + 0.999999) - 1))
    return s[k]


def summary(name, xs_ms, div=1.0):
    xs = [x / div for x in xs_ms]
    if not xs:
        print(f"{name}: no data")
        return
    print(f"{name}: n={len(xs)} min={min(xs):.3f} p50={pct(xs, .5):.3f} p95={pct(xs, .95):.3f} "
          f"p99={pct(xs, .99):.3f} p99.9={pct(xs, .999):.3f} max={max(xs):.3f} avg={sum(xs) / len(xs):.3f} [ms]")


def hist(xs_ms, width=0.5, maxv=None):
    if not xs_ms:
        return
    maxv = maxv or max(xs_ms)
    nb = int(maxv / width) + 1
    b = [0] * (nb + 1)
    for x in xs_ms:
        b[min(nb, int(x / width))] += 1
    top = max(b)
    for i, c in enumerate(b):
        if c:
            lab = f">{nb * width:5.1f}" if i == nb else f"{i * width:5.1f}"
            print(f"  {lab} ms | {'#' * max(1, int(50 * c / top))} {c}")


def cmd_list(_):
    print("inputs :", rtmidi.MidiIn().get_ports())
    print("outputs:", rtmidi.MidiOut().get_ports())


def cmd_ping(a):
    mi, mo = open_ports(a.port)
    st = PING_USB if a.target == "usb" else PING_RADIO
    ev = threading.Event()
    state = {"want": -1, "t": 0}

    def cb(msg, _):
        t = time.perf_counter_ns()
        m = msg[0]
        if len(m) == 3 and m[0] == st and (m[1] | (m[2] << 7)) == state["want"]:
            state["t"] = t
            ev.set()

    mi.set_callback(cb)
    time.sleep(0.3)
    rtts, rows, lost = [], [], 0
    for i in range(a.n):
        seq = i & 0x3FFF
        ev.clear()
        state["want"] = seq
        t0 = time.perf_counter_ns()
        mo.send_message([st, seq & 0x7F, seq >> 7])
        if ev.wait(a.timeout / 1000):
            r = (state["t"] - t0) / 1e6
            rtts.append(r)
            rows.append((i, r))
        else:
            lost += 1
            rows.append((i, ""))
            if i + 1 == 20 and lost == 20:
                sys.exit("最初の 20 回が全て応答なし → 中止（接続・モード・ポートを確認）")
        if a.n >= 10 and (i + 1) % (a.n // 10) == 0:
            print(f"  {100 * (i + 1) // a.n}%", flush=True)
        # Random gap so the send timing does not sync with the BLE connection interval etc.
        time.sleep(random.uniform(a.gap_min, a.gap_max) / 1000)
    print(f"---- ping {a.target} via '{a.port}' ----")
    summary("RTT        ", rtts)
    summary("RTT/2      ", rtts, 2)
    print(f"lost={lost}/{a.n}")
    hist(rtts, a.bin)
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["i", "rtt_ms"])
            w.writerows(rows)


def cmd_burst(a):
    """The C6 sends at a precise period -> distribution of (arrival time - ideal time) = jitter of the one-way latency (relative to the minimum)."""
    mi, mo = open_ports(a.port)
    arr = {}

    def cb(msg, _):
        t = time.perf_counter_ns()
        m = msg[0]
        if len(m) == 3 and m[0] == BURST:
            arr.setdefault(m[1] | (m[2] << 7), t)

    mi.set_callback(cb)
    time.sleep(0.3)
    mo.send_message([CTRL, 118, a.count // 10])
    time.sleep(0.05)
    mo.send_message([CTRL, 119, a.period])
    deadline = time.time() + a.count * a.period / 1000 * 1.5 + 3
    expect = (a.count // 10) * 10
    while time.time() < deadline and len(arr) < expect:
        time.sleep(0.05)
    seqs = sorted(arr)
    if len(seqs) < 10:
        sys.exit(f"received only {len(seqs)}")
    # Least-squares fit of a line (ideal send time + clock drift), then look at the residuals
    xs = seqs
    ys = [(arr[s] - arr[seqs[0]]) / 1e6 for s in seqs]
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    res = [y - (my + b * (x - mx)) for x, y in zip(xs, ys)]
    m0 = min(res)
    exc = [r - m0 for r in res]
    gaps = [(ys[i + 1] - ys[i]) for i in range(n - 1)]
    print(f"---- burst via '{a.port}' period={a.period}ms ----")
    print(f"received={n}/{expect} lost={expect - n}  fitted period={b:.4f}ms (clock drift {(b / a.period - 1) * 1e6:+.0f} ppm)")
    summary("excess latency (最速を0とした遅延) ", exc)
    print(f"inter-arrival: min={min(gaps):.3f} max={max(gaps):.3f} ms  (0 に近い間隔 = まとめて届いた)")
    hist(exc, a.bin)
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["seq", "arrival_ms", "excess_ms"])
            w.writerows(zip(xs, ys, exc))


def cmd_rxtest(a):
    """Receive-side (PC -> C6) measurement. The PC sends at a fixed interval; the C6 records arrival times and reports them afterwards.

    Shows: the jitter of (arrival time - send time) (delay relative to the fastest one, taken as 0), and the jitter of the arrival interval as seen by the C6.
    The absolute latency is not measured (the PC and C6 clocks are independent, so the minimum is used as the reference).
    """
    mi, mo = open_ports(a.port)
    recs, state = {}, {"seq": None, "t0": None, "done": False}

    def cb(msg, _):
        m = msg[0]
        if len(m) != 3:
            return
        if m[0] == 0xAC:
            state["seq"] = m[1] | (m[2] << 7)
        elif m[0] == 0xAB:
            state["t0"] = m[1] | (m[2] << 7)
        elif m[0] == 0xAA and state["seq"] is not None and state["t0"] is not None:
            recs.setdefault(state["seq"], state["t0"] | (m[1] << 14) | (m[2] << 21))
            state["seq"] = state["t0"] = None
        elif m[0] == CTRL and m[1] == 127 and m[2] == 127:
            state["done"] = True

    mi.set_callback(cb)
    time.sleep(0.3)
    mo.send_message([CTRL, 127, 0])  # clear the records
    time.sleep(0.3)
    period = a.period / 1000.0
    sent = []
    start = time.perf_counter() + 0.05
    for i in range(a.count):
        target = start + i * period
        while True:  # busy-wait the last 1 ms so the send time is precise
            rest = target - time.perf_counter()
            if rest <= 0:
                break
            if rest > 0.002:
                time.sleep(rest - 0.001)
        t = time.perf_counter_ns()
        mo.send_message([0xA9, i & 0x7F, (i >> 7) & 0x7F])
        sent.append(t)
    time.sleep(1.0)
    mo.send_message([CTRL, 127, 1])  # request the report
    deadline = time.time() + a.count * 0.02 + 10
    while time.time() < deadline and not state["done"]:
        time.sleep(0.05)
    seqs = sorted(s for s in recs if s < a.count)
    print(f"---- rxtest (PC -> C6) via '{a.port}' period={a.period}ms ----")
    print(f"C6 に届いた数={len(seqs)}/{a.count} lost={a.count - len(seqs)}" + ("" if state["done"] else "  （報告の終わりの合図が来なかった）"))
    if len(seqs) < 10:
        return
    xs = [(sent[s] - sent[seqs[0]]) / 1e6 for s in seqs]          # send time [ms] (PC clock)
    ys = [(recs[s] - recs[seqs[0]]) / 1e3 for s in seqs]          # arrival time [ms] (C6 clock)
    d = [y - x for x, y in zip(xs, ys)]                            # clock offset + delay
    n = len(xs)
    mx, md = sum(xs) / n, sum(d) / n
    b = sum((x - mx) * (v - md) for x, v in zip(xs, d)) / sum((x - mx) ** 2 for x in xs)
    res = [v - (md + b * (x - mx)) for x, v in zip(xs, d)]
    m0 = min(res)
    exc = [r - m0 for r in res]
    send_err = [abs(xs[i] - (seqs[i] - seqs[0]) * a.period) for i in range(n)]
    gaps = [ys[i + 1] - ys[i] for i in range(n - 1) if seqs[i + 1] == seqs[i] + 1]
    gdev = [abs(g - a.period) for g in gaps]
    print(f"clock drift {b * 1e6:+.0f} ppm,  PC の送信時刻のずれ: max {max(send_err):.3f} ms")
    summary("excess latency PC->C6 (最速を0とした遅れ)", exc)
    if gaps:
        print(f"C6 から見た到着間隔: min={min(gaps):.3f} max={max(gaps):.3f} ms（理想は {a.period}ms）")
        summary("到着間隔の、理想からのずれ           ", gdev)
    hist(exc, a.bin)
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["seq", "sent_ms", "arrival_ms", "excess_ms"])
            w.writerows(zip(seqs, xs, ys, exc))


def cmd_notetest(a):
    """Check Note Off retransmission. Sends Note On / Note Off pairs continuously while temporarily putting the bridge into an "unreachable" state.

    --dir in  : C6 -> PC. The C6 sends the pairs and the PC counts them. At the end, counts notes whose Note Off never arrived (stuck notes).
    --dir out : PC -> C6. The PC sends the pairs and the C6 counts them. At the end, the C6 is queried.
    If --deaf-serial is set to the bridge's serial port, 'z' (unreachable for 150 ms) is sent every --deaf-every seconds.
    """
    import serial as pyserial
    mi, mo = open_ports(a.port)
    held, cnt, info = set(), {"on": 0, "off": 0, "last": time.time()}, {}
    on_arr = []  # (arrival time, note): to look at the Note On delay

    def cb(msg, _):
        m = msg[0]
        if len(m) != 3:
            return
        h = m[0] & 0xF0
        if h == 0x90 and m[2] > 0:
            held.add(m[1]); cnt["on"] += 1; cnt["last"] = time.time()
            on_arr.append((time.perf_counter_ns(), m[1]))
        elif h == 0x80 or (h == 0x90 and m[2] == 0):
            held.discard(m[1]); cnt["off"] += 1; cnt["last"] = time.time()
        elif m[0] == CTRL and 110 <= m[1] <= 114:
            info[m[1]] = m[2]

    mi.set_callback(cb)
    time.sleep(0.3)
    stop = threading.Event()
    deaf = {"n": 0}

    def deafer():
        sp = pyserial.Serial(a.deaf_serial, 115200, timeout=0.1)
        while not stop.wait(a.deaf_every):
            sp.write(b"z")
            deaf["n"] += 1
        sp.close()

    th = None
    if a.deaf_serial:
        th = threading.Thread(target=deafer, daemon=True)
        th.start()
    pairs = (a.count // 10) * 10
    if a.max_age is not None:  # age after which stale Note On is dropped (applied to both the C6 and the bridge)
        mo.send_message([CTRL, 108, max(0, min(127, a.max_age // 10))])
        time.sleep(0.2)
    if a.dir == "in":
        mo.send_message([CTRL, 118, pairs // 10])
        time.sleep(0.05)
        mo.send_message([CTRL, 117, int(a.period)])
        deadline = time.time() + pairs * 2 * a.period / 1000 + 4
        while time.time() < deadline and not (cnt["on"] + cnt["off"] > 0 and time.time() - cnt["last"] > 2.5):
            time.sleep(0.05)
        stop.set()
        print(f"---- notetest C6 -> PC via '{a.port}': {pairs} 組、間隔 {a.period}ms、届かない状態 {deaf['n']} 回 ----")
        print(f"Note On  届いた数: {cnt['on']}/{pairs}")
        print(f"Note Off 届いた数: {cnt['off']}/{pairs}")
        print(f"鳴りっぱなしの音（On は届いたのに Off が届いていない）: {len(held)} {sorted(held) if held else ''}")
        # Note On delay: recover the pair index from the note number (36 to 83 are used in turn) and compare with the scheduled time (every 2 x period)
        if len(on_arr) > 10:
            ks, k, prev = [], 0, on_arr[0][1]
            for _, note in on_arr:
                k += (note - prev) % 48
                prev = note
                ks.append(k)
            ts = [(t - on_arr[0][0]) / 1e6 for t, _ in on_arr]
            res = [t - kk * 2 * a.period for t, kk in zip(ts, ks)]
            m0 = min(res)
            late = [r - m0 for r in res]
            summary("Note On の遅れ（最速を0）", late)
    else:
        mo.send_message([CTRL, 109, 0])  # clear the C6's counters
        time.sleep(0.2)
        t0 = time.perf_counter() + 0.05
        for i in range(pairs * 2):
            target = t0 + i * a.period / 1000
            while time.perf_counter() < target:
                time.sleep(0.0005)
            note = 36 + ((i // 2) % 48)
            mo.send_message([0x90, note, 100] if i % 2 == 0 else [0x80, note, 0])
        stop.set()
        time.sleep(2.0)
        mo.send_message([CTRL, 122, 0])
        time.sleep(1.5)
        on = info.get(111, 0) | (info.get(112, 0) << 7)
        off = info.get(113, 0) | (info.get(114, 0) << 7)
        print(f"---- notetest PC -> C6 via '{a.port}': {pairs} 組、間隔 {a.period}ms、届かない状態 {deaf['n']} 回 ----")
        if 110 not in info:
            print("C6 から応答なし")
        else:
            print(f"Note On  届いた数: {on}/{pairs}")
            print(f"Note Off 届いた数: {off}/{pairs}")
            print(f"鳴りっぱなしの音（C6 側）: {info[110]}")


def cmd_looptest(a):
    """Start the two-hop (C6 -> bridge -> C6) delay measurement. The result appears in 's' on the C6's serial port (loop C6->bridge->C6).

    Configures the C6 to "listen to channel 9 and also receive its own messages", and makes it send TM_LOOP (PolyAT ch9) at a fixed interval.
    The bridge forwards these to the PC and at the same time sends them back to the C6s listening to channel 9 (i.e. the sender itself).
    The C6 measures, on its own clock, the time between queuing the message for sending and getting it back.
    """
    mi, mo = open_ports(a.port)
    seen = {"n": 0}

    def cb(msg, _):
        if len(msg[0]) == 3 and msg[0][0] == 0xA8:
            seen["n"] += 1

    mi.set_callback(cb)
    time.sleep(0.3)
    count = (a.count // 10) * 10
    for cc, v in ((107, 9), (106, 1), (105, 0), (118, count // 10)):
        mo.send_message([CTRL, cc, v])
        time.sleep(0.15)
    mo.send_message([CTRL, 116, int(a.period)])
    time.sleep(count * a.period / 1000 + 1.5)
    if not a.keep:
        mo.send_message([CTRL, 107, 0])  # restore the listening channel
        time.sleep(0.1)
        mo.send_message([CTRL, 106, 0])
        time.sleep(0.2)
    print(f"---- looptest via '{a.port}': {count} 件、間隔 {a.period}ms。PC にも届いた数 = {seen['n']}/{count} ----")
    print("結果は C6 のシリアルで 's' を送って見る（loop C6->bridge->C6 の行）")


SYS_SET = [[0xFA], [0xF8], [0xF8], [0xF8], [0xF2, 0x12, 0x34], [0xFB], [0xF8], [0xF3, 0x05], [0xF1, 0x23], [0xF6], [0xFE], [0xFC]]


def cmd_systest(a):
    """Check that messages without a channel (clock, start, song position, etc.) pass in both directions and between C6s."""
    mi, mo = open_ports(a.port)
    mi.ignore_types(sysex=True, timing=False, active_sense=False)
    got, cnt = [], {}

    def cb(msg, _):
        m = msg[0]
        if m[0] >= 0xF0:
            got.append(list(m))
        elif len(m) == 3 and m[0] == CTRL and m[1] == 103:
            cnt["n"] = m[2]

    mi.set_callback(cb)
    time.sleep(0.3)
    fmt = lambda ms: " ".join("".join(f"{b:02X}" for b in m) for m in ms)
    ok_all = True

    def check(title, expect):
        nonlocal ok_all
        ok = got == expect
        ok_all &= ok
        print(f"{title}: {'OK' if ok else 'NG'}  ({len(got)}/{len(expect)} 件)")
        if not ok:
            print(f"  期待: {fmt(expect)}\n  実際: {fmt(got)}")

    # 1) PC -> C6 -> PC (the C6 echoes back)
    mo.send_message([CTRL, 104, 0]); time.sleep(0.05)
    mo.send_message([CTRL, 104, 1]); time.sleep(0.05)
    for m in SYS_SET:
        mo.send_message(m)
        time.sleep(a.gap / 1000)
    time.sleep(0.5)
    check("PC → C6 → PC（折り返し）", SYS_SET)
    mo.send_message([CTRL, 104, 3]); time.sleep(0.3)
    print(f"  C6 が受けた数: {cnt.get('n')}（期待 {len(SYS_SET)}）")
    ok_all &= cnt.get("n") == len(SYS_SET)

    # 2) C6 -> PC (the C6 sends on its own). The C6 listens to nothing, so nothing comes back to it
    got.clear(); cnt.clear()
    mo.send_message([CTRL, 104, 0]); time.sleep(0.05)
    mo.send_message([CTRL, 107, 0]); time.sleep(0.2)
    mo.send_message([CTRL, 104, 2]); time.sleep(0.5)
    check("C6 → PC（C6 から送る）", SYS_SET)
    mo.send_message([CTRL, 104, 3]); time.sleep(0.3)
    print(f"  C6 に戻った数: {cnt.get('n')}（期待 0: 何も聞いていない）")
    ok_all &= cnt.get("n") == 0

    # 3) C6 -> bridge -> C6 (listens to channel 5 only, set to also receive its own messages)
    got.clear(); cnt.clear()
    mo.send_message([CTRL, 104, 0]); time.sleep(0.05)
    mo.send_message([CTRL, 106, 1]); time.sleep(0.2)
    mo.send_message([CTRL, 107, 5]); time.sleep(0.3)
    mo.send_message([CTRL, 104, 2]); time.sleep(0.5)
    check("C6 → PC（聞いている状態）", SYS_SET)
    mo.send_message([CTRL, 104, 3]); time.sleep(0.3)
    print(f"  C6 に戻った数: {cnt.get('n')}（期待 {len(SYS_SET)}: 聞いているクライアント全員へ配る）")
    ok_all &= cnt.get("n") == len(SYS_SET)
    mo.send_message([CTRL, 107, 0]); time.sleep(0.1)
    mo.send_message([CTRL, 106, 0]); time.sleep(0.1)
    mo.send_message([CTRL, 104, 0]); time.sleep(0.1)
    print("総合:", "OK" if ok_all else "NG")


def cmd_info(a):
    """Query the C6 (BLE mode) with CC ch16 #122 and receive the connection interval etc. as CCs."""
    mi, mo = open_ports(a.port)
    got = {}

    def cb(msg, _):
        m = msg[0]
        if len(m) == 3 and m[0] == CTRL and 120 <= m[1] <= 124:
            got[m[1]] = m[2]

    mi.set_callback(cb)
    time.sleep(0.3)
    mo.send_message([CTRL, 122, 0])
    time.sleep(1.5)
    if not got:
        print("応答なし（C6 が BLE モードで、このファームか確認）")
        return
    if 120 in got:
        print(f"BLE 接続間隔: {got[120] * 1.25:.2f} ms ({got[120]} x 1.25ms){'（127 は上限値）' if got[120] == 127 else ''}")
    if 121 in got:
        print(f"MTU: 約 {got[121] * 4}")
    if 123 in got:
        print(f"notify 再送回数: {got[123]}{'+' if got[123] == 127 else ''}")
    if 124 in got:
        print(f"キューあふれで捨てた数: {got[124]}{'+' if got[124] == 127 else ''}")


def cmd_listen(a):
    mi, _ = open_ports(a.port)
    t0 = [None, None]

    def cb(msg, _):
        t = time.perf_counter_ns() / 1e6
        if t0[0] is None:
            t0[0] = t
        d = "" if t0[1] is None else f"(+{t - t0[1]:.3f})"
        t0[1] = t
        print(f"{t - t0[0]:12.3f} ms {d:>14}  " + " ".join(f"{b:02X}" for b in msg[0]), flush=True)

    mi.set_callback(cb)
    print("listening... Ctrl-C で終了")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    sp.add_parser("list")
    p = sp.add_parser("ping")
    p.add_argument("--port", required=True)
    p.add_argument("--target", choices=["radio", "usb"], default="radio")
    p.add_argument("-n", type=int, default=1000)
    p.add_argument("--timeout", type=float, default=200, help="ms")
    p.add_argument("--gap-min", type=float, default=5, help="ms")
    p.add_argument("--gap-max", type=float, default=25, help="ms")
    p.add_argument("--bin", type=float, default=0.5, help="histogram bin ms")
    p.add_argument("--csv")
    b = sp.add_parser("burst")
    b.add_argument("--port", required=True)
    b.add_argument("--period", type=int, default=10, help="ms (1-127)")
    b.add_argument("--count", type=int, default=500, help="10-1270")
    b.add_argument("--bin", type=float, default=0.5)
    b.add_argument("--csv")
    r = sp.add_parser("rxtest")
    r.add_argument("--port", required=True)
    r.add_argument("--period", type=float, default=10, help="ms")
    r.add_argument("--count", type=int, default=500, help="最大 1300")
    r.add_argument("--bin", type=float, default=0.5)
    r.add_argument("--csv")
    nt = sp.add_parser("notetest")
    nt.add_argument("--port", required=True)
    nt.add_argument("--dir", choices=["in", "out"], default="in")
    nt.add_argument("--count", type=int, default=500, help="組の数（10 の倍数、最大 1270）")
    nt.add_argument("--period", type=float, default=10, help="ms")
    nt.add_argument("--deaf-serial", help="ブリッジのシリアル。指定すると、届かない状態を作る")
    nt.add_argument("--deaf-every", type=float, default=0.7, help="秒")
    nt.add_argument("--max-age", type=int, help="古くなった Note On を捨てるまでの時間 [ms]（10ms 単位、0 = 捨てない）")
    lt = sp.add_parser("looptest")
    lt.add_argument("--port", required=True)
    lt.add_argument("--period", type=float, default=10, help="ms")
    lt.add_argument("--count", type=int, default=500)
    lt.add_argument("--keep", action="store_true", help="終わっても、聞くチャンネルの設定を戻さない")
    st = sp.add_parser("systest")
    st.add_argument("--port", required=True)
    st.add_argument("--gap", type=float, default=5, help="送る間隔 [ms]")
    i = sp.add_parser("info")
    i.add_argument("--port", required=True)
    l = sp.add_parser("listen")
    l.add_argument("--port", required=True)
    a = ap.parse_args()
    {"list": cmd_list, "ping": cmd_ping, "burst": cmd_burst, "rxtest": cmd_rxtest, "notetest": cmd_notetest, "looptest": cmd_looptest, "info": cmd_info, "systest": cmd_systest, "listen": cmd_listen}[a.cmd](a)


if __name__ == "__main__":
    main()
