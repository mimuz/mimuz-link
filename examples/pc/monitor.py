#!/usr/bin/env python3
#
#  monitor.py long-running log of the bridge serial output and received MIDI
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/18 : Start Writing
#
"""Long-running monitor: records the S3 serial log and the MIDI received through the Mac's CoreMIDI in one log, on the same clock (the Mac's time).

Used to isolate "it stops responding after a while". When it stops:
  - not reaching the S3 either                -> the C6 (button reading / sending) or the radio
  - reaches the S3 but not CoreMIDI            -> the S3's USB output or macOS
  - reaches CoreMIDI but the browser does not react -> Chrome / Web MIDI side
A drop in the seq value in the S3 log means the C6 has restarted.

usage: monitor.py --serial /dev/cu.usbmodemXXXX --port XIAO_ESP32S3 [--c6serial /dev/cu.usbmodemYYYY] [--minutes 30] [--log monitor.log]
"""
import argparse
import datetime
import re
import threading
import time

import rtmidi
import serial

lock = threading.Lock()


def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", required=True, help="S3 の CDC シリアル")
    ap.add_argument("--port", default="XIAO_ESP32S3", help="MIDI 入力名の一部")
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--log", default="monitor.log")
    ap.add_argument("--c6serial", help="C6 の CDC シリアル（任意）。開いた時に C6 がリセットされることがある")
    a = ap.parse_args()

    out = open(a.log, "a")

    def w(line):
        with lock:
            out.write(f"[{stamp()}] {line}\n")
            out.flush()

    w(f"==== monitor start serial={a.serial} midi={a.port}")

    mi = rtmidi.MidiIn()
    mi.ignore_types(sysex=True, timing=True, active_sense=True)
    ports = mi.get_ports()
    idx = next((i for i, p in enumerate(ports) if a.port.lower() in p.lower()), None)
    if idx is None:
        raise SystemExit(f"MIDI port not found: {ports}")
    mi.open_port(idx)

    def cb(msg, _):
        d = msg[0]
        if d and d[0] in (0xAF, 0xAD, 0xAE):
            return  # do not log measurement messages
        w("CoreMIDI " + " ".join(f"{b:02X}" for b in d))

    mi.set_callback(cb)

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.serial, 115200, 0.2
    # The S3 (TinyUSB CDC) treats the port as "not connected" and drops its log unless DTR is set, so DTR=True.
    # The S3 does not restart on DTR (do not use this for the C6 port: the C6 may reset when its port is opened)
    s.dtr = True
    s.rts = False
    s.open()

    if a.c6serial:
        def c6reader():
            c = serial.Serial(a.c6serial, 115200, timeout=0.2)
            b = ""
            while True:
                try:
                    b += c.read(4096).decode(errors="replace")
                except serial.SerialException as e:
                    w(f"!! C6 serial error: {e}")
                    return
                while "\n" in b:
                    ln, b = b.split("\n", 1)
                    ln = ln.rstrip("\r")
                    if ln:
                        w("C6 " + ln)
        threading.Thread(target=c6reader, daemon=True).start()

    last_seq = None
    seq_re = re.compile(r"seq=(\d+)")
    buf = ""
    end = time.time() + a.minutes * 60
    last_beat = time.time()
    while time.time() < end:
        try:
            chunk = s.read(4096).decode(errors="replace")
        except serial.SerialException as e:
            w(f"!! S3 serial error: {e}")
            time.sleep(1)
            try:
                s.close()
                s.open()
            except Exception:
                pass
            continue
        buf += chunk
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.rstrip("\r")
            if not line:
                continue
            m = seq_re.search(line)
            if m:
                seq = int(m.group(1))
                if last_seq is not None and seq < last_seq:
                    w(f"!! seq が {last_seq} → {seq} に戻った（C6 が再起動した可能性）")
                elif last_seq is not None and seq != last_seq + 1:
                    w(f"!! seq が飛んだ {last_seq} → {seq}（欠落 {seq - last_seq - 1}）")
                last_seq = seq
            w("S3 " + line)
        if time.time() - last_beat > 60:  # heartbeat every minute
            last_beat = time.time()
            w("-- alive")
    w("==== monitor end")


if __name__ == "__main__":
    main()
