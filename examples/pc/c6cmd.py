#
#  c6cmd.py send one serial command to a device and print the reply
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/18 : Start Writing
#
# usage: c6cmd.py PORT CMD [timeout_s] [until_text]
import serial, sys, time
port, cmd = sys.argv[1], sys.argv[2]
to = float(sys.argv[3]) if len(sys.argv) > 3 else 3
until = sys.argv[4] if len(sys.argv) > 4 else None
s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(0.3); s.reset_input_buffer()
s.write(cmd.encode()); t = time.time(); out = ''
while time.time() - t < to:
    out += s.read(4096).decode(errors='replace')
    if until and until in out:
        time.sleep(0.3); out += s.read(4096).decode(errors='replace'); break
print(out)
