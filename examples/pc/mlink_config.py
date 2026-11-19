#!/usr/bin/env python3
#
#  mlink_config.py send mimuz-link config commands over USB serial
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/19 : Start Writing
#
"""Send mimuz-link config commands over USB serial and read the replies.

usage: mlink_config.py PORT "COMMAND" ["COMMAND" ...]
  e.g. mlink_config.py /dev/cu.usbmodem11301 info "set rx_channels omni" get
       mlink_config.py /dev/cu.usbmodemQEwFK6XO0001 ports "port 1 info"
Sequence numbers (@1, @2, ...) are added automatically. Only the replies (@N ok / err / +) are printed.
The port is opened with DTR on (the C6 resets if opened with DTR off; the S3 does not reply unless DTR is on).
"""
import sys
import time

import serial


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    s = serial.Serial(sys.argv[1], 115200, timeout=0.1)  # DTR is on by default
    time.sleep(0.3)
    s.reset_input_buffer()
    for n, cmd in enumerate(sys.argv[2:], 1):
        wait = 16.0 if cmd.split()[0] == "resurvey" else 2.0
        s.write(f"@{n} {cmd}\n".encode())
        print(f">> @{n} {cmd}")
        buf, done, t0 = "", False, time.time()
        while not done and time.time() - t0 < wait:
            buf += s.read(4096).decode(errors="replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip()
                if line.startswith(f"@{n} "):
                    print("   " + line)
                    if line.split()[1] in ("ok", "err"):
                        done = True
        if not done:
            print("   (返事なし)")
    s.close()


if __name__ == "__main__":
    main()
