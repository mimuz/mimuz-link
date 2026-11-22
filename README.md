# mimuz-link

A wireless MIDI connection environment that supports ESP-NOW and BLE-MIDI.

Clients (XIAO ESP32-C6, used as controllers or sound modules) connect over ESP-NOW to a bridge (XIAO ESP32-S3, a USB-MIDI dongle plugged into the PC). The PC sees the bridge as a USB-MIDI device with 8 ports. When no bridge is found, a client connects to the PC directly as a BLE-MIDI device.

- Low latency: the ESP-NOW hop is under 1 ms one way (median). The round trip PC → bridge → client → bridge → PC has a median of about 1.6 ms.
- Multiple devices: up to 8 clients can be connected to one bridge at the same time. Messages can also be delivered between clients, per MIDI channel.
- Delivery: Note Off messages are resent if they are not delivered. Note On messages that have waited too long are dropped.
- Configuration: one-line commands over USB serial. Settings are stored in non-volatile memory.

## Layout

The communication part (library) is kept separate from the device-specific part (applications).

```
libs/mimuz-link/               link layer (header-only Arduino library)
  src/mimuz_link_proto.h         packet format, types and constants (shared by client and bridge)
  src/mimuz_link_client.h        client side (C6)
  src/mimuz_link_bridge.h        bridge side (S3)
  src/mimuz_link_cmd.h           USB-serial config command helpers
  src/mimuz_link_lathist.h       statistics helper
test/mimuz-link-test/          test message definitions (not part of the link layer)
  src/mimuz_link_testproto.h
examples/
  client/c6_test_device/       test client: bare C6 board (no external wiring)
    c6_test_device.ino           test message handling, sound-module stand-in, serial commands
    workload.h                   simulated controller workload (1 ms scan, switch, encoder simulation)
  bridge/s3_bridge/            bridge: test handling and serial commands
    s3_bridge.ino
  pc/                          PC-side measurement and config tools (Python)
  pc/web/                      viewer for checking the feel by ear and eye (Web MIDI)
```

### What the link layer handles

| | Client (C6) | Bridge (S3) |
|---|---|---|
| Connection | registering with the bridge, connecting, keeping the connection, searching again when it is lost | registering clients (stored), assigning and releasing ports |
| Radio channel | follows the bridge's radio channel | picks a free radio channel at startup; switches while running |
| Sending | send queue, batching, Note Off resend, dropping stale Note On | the same (PC → client); waits for USB writes |
| Delivery | tells the bridge which MIDI channels it listens to | forwards messages from a client to the other clients listening to that channel |
| Other | BLE-MIDI when no bridge is reachable; choosing the transport | fixed 8 USB-MIDI ports |
| Recovery | detects a stuck radio driver and restarts it. A driver that refuses every packet is reinitialized at once (about 1 s); if that does not help, or internal memory is low, the device restarts and comes back on the same channel and port numbers within a few seconds | detects a stuck radio driver and restarts it |
| Duplicates | drops packets that arrive twice because of radio-layer retries | the same |
| Diagnostics | radio round-trip measurement, statistics | PING replies, statistics, logging |

Carried messages: channel messages and channel-less messages (clock, start, stop, song position, and so on). SysEx is not carried.

The link layer handles each MIDI message on its own. It does not pair Note On with Note Off and does not track which notes are sounding.

### Application API

Client (`#include <mimuz_link_client.h>`, namespace `mlink`):

```cpp
mlink::Config cfg;                       // namePrefix, repair (register again), task priorities
mlink::onMidi(handler);                  // received MIDI (called from the receive task)
mlink::onMidiFast(handler);              // optional: called right away inside radio receive handling
mlink::Transport tr = mlink::begin(cfg); // once in setup(); returns TR_ESPNOW or TR_BLE
mlink::sendMidi(status, d1, d2, aux, waitMs);  // can be called from any task
mlink::loop();                           // call from loop()

// Settings
mlink::setRxChannels(mask, flags);       // MIDI channels to listen to (bit0 = ch1; 0xFFFF = OMNI)
mlink::setNoteOnMaxAgeMs(ms);            // age after which a waiting Note On is dropped (0 = never)
mlink::setRate(idx);                     // PHY rate (0=1M 1=6M 2=24M 3=MCS4)
mlink::restart(mlink::BOOT_AUTO);        // BOOT_AUTO / BOOT_BLE / BOOT_REPAIR

// State and diagnostics
mlink::name(); mlink::isConnected(); mlink::port(); mlink::radioChannel(); mlink::transport();
mlink::radioPing(count); mlink::printStats(Serial); mlink::clearStats();
```

Bridge (`#include <mimuz_link_bridge.h>`, namespace `mlink`):

```cpp
#define MIMUZ_BRIDGE_NAME_PREFIX "mi:muz:bridgeESPS3"   // optional (define before the include)
mlink::onUsbMidi(handler);               // optional: see MIDI from the PC first (return true to not forward it)
mlink::setMidiLogFilter(filter);         // optional: choose which messages are logged one by one
mlink::begin();                          // once in setup() (the radio channel survey takes about 12 s)
mlink::loop();                           // call from loop()

mlink::usbSend(port, m); mlink::setNoteOnMaxAgeMs(ms);
mlink::clearRegistry(); mlink::resurvey(); mlink::switchChannel(ch); mlink::debugDeaf(ms);
mlink::printStats(Serial); mlink::printList(Serial); mlink::printSurvey(); mlink::clearStats();
```

- This is a header-only library: include it from only one file of the sketch.
- Hot paths are placed in internal fast memory (IRAM). On the bridge this shortens the round trip by about 0.15 ms. For comparison, define `MIMUZ_LINK_NO_IRAM` to place them in flash instead (with arduino-cli: `--build-property "compiler.cpp.extra_flags=-DMIMUZ_LINK_NO_IRAM"`).

## Build and upload (arduino-cli, esp32 core 3.3.8)

```sh
# Bridge (S3): select USB Mode = TinyUSB explicitly
cd examples/bridge
arduino-cli compile -u -p <port> --libraries ../../libs --libraries ../../test \
  --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default s3_bridge

# Test client (C6): huge_app because both Wi-Fi and BLE are included; CDCOnBoot=cdc to put serial on USB
cd examples/client
arduino-cli compile -u -p <port> --libraries ../../libs --libraries ../../test \
  --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app c6_test_device
```

- A brand-new XIAO ESP32-C6 does not start the firmware after its first upload until it is unplugged and plugged in again.
- If uploading to the S3 reports `Failed uploading`, check `arduino-cli board list` for the bootloader port and run `arduino-cli upload` again on that port.
- Start the bridge first (the radio channel survey takes about 12 s), then the C6, and they connect right away. In the other order a registered C6 waits on BLE and searches for its bridge again every 20 s, so it still connects later. An unregistered C6 does not search again; restart it near the bridge.
- On the first connection, macOS may hold the device at "Allow accessory to connect".

## Wiring

None. The test client (C6) and the bridge (S3) both run on bare boards.

## Usage

1. Plug the bridge into the PC and power on the C6.
   - An unregistered C6 registers and connects automatically if it is right next to the bridge.
   - A registered C6 connects to its stored bridge.
   - To erase a C6's registration and register again, send `u` on its serial port (or the config command `unpair`).
2. The PC sees `mi:muz:bridgeESPS3-<unique ID> Port 1` to `Port 8`. A connected C6 gets the lowest free port. Port numbers are not fixed per device; they follow the order in which devices connect.
3. Without a bridge, the C6 waits as a BLE-MIDI device named `mi:muz:conESP32C6-<unique ID>`.

## Config commands (USB serial)

Both the C6 and the bridge are configured over USB serial (115200). MIDI goes over USB-MIDI and the radio; serial is used for configuration only.
Open the port with DTR on (the C6 resets if the port is opened with DTR off; the S3 does not reply unless DTR is on).

- PC → device: `@<id> <command> <args…>` (one line)
- Device → PC: `@<id> ok <key=value …>` / `@<id> err <reason>` / `@<id> + <key=value …>` (an intermediate line of a multi-line reply)
- Lines that do not start with `@` are log output.
- Values written with `set` are saved to non-volatile memory immediately and applied at the next boot (except the diagnostic items marked "not saved").

| Device | Command | Description |
|---|---|---|
| both | `info` | name, kind, firmware version, connection state |
| both | `get [key]` / `set <key> <value>` | read and write settings |
| C6 | key `rx_channels` | MIDI channels to listen to (bit0 = channel 1; `omni` and `none` are also accepted) |
| C6 | key `noteon_max_age_ms` | age after which a waiting Note On is dropped (0 = never; default 80) |
| C6 | key `label` | your name for this client (up to 30 of `A-Z a-z 0-9 - _ .`; `-` clears it). Saved on the client and told to the bridge on every connect and change, and whenever the bridge asks; shown by `info` and in the bridge's `ports` list. Not used as the USB or BLE device name: applications can pick a client by its label (free; several clients may share one on purpose) or by its device name (unique) |
| C6 | key `phy_rate` | radio rate (0=1M 1=6M 2=24M 3=MCS4; default 1) |
| C6 | `reset` / `unpair` / `restart [auto\|ble]` | reset settings to defaults / erase registration and reboot / reboot |
| both | `radio_restart [wifi\|stall]` | diagnostic: restart ESP-NOW. `wifi` = restart Wi-Fi as well, `stall` = simulate a stuck driver (the device should detect it and restart by itself). The bridge also takes `driver` = reinitialize the Wi-Fi driver, and `stall hold` = a stuck driver that survives restarts (recovery escalates up to a device restart) |
| bridge | keys `channel_min`, `channel_max` | range of radio channels that may be chosen (takes effect at the next survey) |
| bridge | key `noteon_max_age_ms` | same as above, for the PC → client direction |
| bridge | `ports` / `registry` / `survey` | connected ports (also shows per-port signal strength `rssi`, undelivered count `tx_fail`, and the client's `label` and device `name`, which an ESP-NOW client tells only to its bridge) / registered clients (with the last `label` and `name` each told, kept with the registration) / latest radio channel survey |
| bridge | `unregister <MAC\|all>` | remove a registration |
| bridge | `label_request [<port>\|all]` | ask clients for their label again (the bridge also asks by itself when a client's label has not arrived 2 s after it connected, up to 5 times) |
| bridge | `resurvey` / `switch_channel <n>` | survey the radio channels again and choose one (about 12 s) / move to the given radio channel |
| bridge | `port <n> <command…>` | relay a command over the radio to the client on port n (e.g. `@5 port 2 set rx_channels omni`). Resent every 150 ms until a reply arrives; `err timeout` after 1.5 s |
| bridge | key `tx_parallel` | 1 = hand packets to the radio in parallel, one per client (default; a hard-to-reach client holds up the others less); 0 = one packet at a time overall, for comparison. Not saved |
| both | key `send_lock` | 1 = hand packets to the radio under the Wi-Fi driver's global lock (default). ESP-NOW otherwise adds frames to the driver's transmit queue without that lock, which can lose the queue and stall all sending until the driver is initialized again. 0 = without the lock, for comparison. Not saved |
| bridge | keys `tx_fail_max`, `tx_probe` | diagnostic: number of consecutive failures before an unresponsive client is skipped (default 3); probe sends while skipped (1 = on; starts at 250 ms, doubling up to 1 s). Not saved |
| bridge | key `trace_status` / `trace [clear]` | diagnostic: status byte of the messages to trace (e.g. 175 = Poly Aftertouch ch16), 0 = off (default). A traced message that the client sends back unchanged is timed from its arrival over USB until the reply has been written to USB (`total_us`), and until the reply arrived over the radio (`radio_us`). `trace` lists the 16 slowest with their port, data bytes (`key` = data1 + data2 × 128) and USB arrival time (`in_us`, bridge clock), slowest first, plus p50 / p99 / max. The tables (about 10 KB) are allocated only while tracing is on. Subtract `total_us` from the round trip measured on the PC to get the time spent on the PC and USB side. Not saved |
| both | key `retry_limit` | how many times the radio retries a frame that is not acknowledged (bridge default 8, client default 0; 0 = the Wi-Fi library's value, 32). The radio sends the frames handed to it in order, so while it retries a frame to a far client, the frames to the other clients wait behind it; 8 shortens those waits (near clients' longest delays about 2.6 ms shorter with a client at about -87 dBm) at the cost of more lost packets to such a far client (about 0.6% instead of 0.08%; Note Offs are resent). `get retry_limit` also shows the values in use and the library's. Uses functions of the Wi-Fi library that are not in its public headers. Not saved |
| bridge | key `slow_tx_ms` / `slowtx [clear]` | diagnostic: radio sends whose result (delivered or not) came this many ms or more after they were handed to the radio are recorded (default 10, 0 = off). `slowtx` lists the last 64, oldest first: port, `sent_us` (bridge clock, same as `in_us` of `trace`), `air_us`, `ok`, `resend` (Note Off resend packet), `timeout` (no result at all), `len`, `rssi`, `nomem` (running count of sends the radio refused). Not saved |
| bridge | key `tx_impl` | diagnostic: 0 = single-queue send task, 1 = per-client send task (default). For comparing both inside one firmware. Not saved |

Tool for trying commands from the PC: `examples/pc/mlink_config.py <serial port> "command" …`

## Serial commands (test firmware)

- C6 (`c6_test_device`): `p` / `P` = measure the radio round trip 10,000 / 100,000 times, `s` = statistics, `c` = clear statistics, `r0`–`r3` = PHY rate, `b` = reboot fixed to BLE, `e` = reboot in automatic mode, `u` = erase registration and register again.
- Bridge (`s3_bridge`): `s` = statistics, `c` = clear statistics, `l` = list registrations and ports, `x` = erase all registrations, `v` = toggle logging of every message, `h` = radio channel survey results, `H` = survey again and choose, `1`–`9` and `A`–`D` = move to radio channel 1–13, `z` = stop receiving for 150 ms.

## Web console (`web-console/`)

A maintenance page in the browser (Web Serial, Chrome or Edge) for the bridges and clients attached over USB: device list, clients of each bridge, settings, radio channel, diagnostics and a command console. `npm start` at the top of the repository, then open http://localhost:8080. See `web-console/README.md`.

## Versions and releases

The version is kept in the top `package.json` and in the git tag `v<version>`.

```sh
npm run release -- 0.2.0                        # see below; nothing is pushed
git push origin main && git push origin v0.2.0  # the tag last
```

`npm run release -- <version>` (web-console/firmware/release.sh), on a clean working tree:

1. writes the version into package.json, the firmware (`MIMUZ_LINK_VERSION`), library.properties and
   web-console/package.json (`npm version <version> --no-git-tag-version`), and commits "Version <version>";
2. builds the bridge and client firmware from that commit (`npm run build`, arduino-cli) and commits it
   (web-console/www/firmware, "Firmware <version>");
3. tags that commit `v<version>`, so the tag holds the firmware built for it.

`npm run build` alone builds for testing; it stops if the sources do not carry the version in `package.json`, and
warns about uncommitted changes. Each manifest records the version and the source commit.

## PC tools (`examples/pc/`)

Setup: `cd examples/pc && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`

| Script | Purpose |
|---|---|
| `latency_test.py` | `ping` / `burst` / `rxtest` / `notetest` / `looptest` / `systest` / `info` / `listen` / `list`. `--port` takes part of the port name (e.g. `"Port 1"`, `conESP32C6`) |
| `multi_test.py` | checks with several clients connected at once (round trips, simultaneous sending, delivery between clients) |
| `weak_test.py` | how one hard-to-reach client changes the latency of the other clients, per condition |
| `trace_test.py` | split round trips into the part spent in the bridge and on the radio and the part spent on the PC side (uses the bridge's `trace_status`). Round trips are sent and timed by `mlping` by default; `--engine python` keeps the Python sender for comparison |
| `mlping/mlping` | round trips to several ports through CoreMIDI from a real-time thread, one CSV line per ping, so that Python's thread scheduling is not part of the times (sender wake-up error and receive callback delay below 0.1 ms). `multi_test.py`, `soak_test.py`, `weak_test.py`, `dead_peer_test.py` and `trace_test.py` time their round trips with it (`MlPing` / `ping_ports` in `multi_test.py`); build it first. Build: `clang -O2 -o mlping/mlping mlping/mlping.c -framework CoreMIDI -framework CoreFoundation` |
| `dead_peer_test.py` | how long sending to a client that stopped responding holds up the other clients |
| `port_watch.py` | watch port release and rejoin |
| `mlink_config.py` | send config commands |
| `run_suite.py` | run the BLE and ESP-NOW measurement suite |
| `duolog.py` | read two serial ports into one timestamped log; commands can be sent during the run |
| `rate_sweep.py`, `load_matrix.py`, `serial_open_test.py`, `c6cmd.py`, `monitor.py` | measurement per PHY rate, measurement per workload, checking for a reset when the serial port is opened, sending a single command, long-running logging |

- Run long measurements with `caffeinate -i` so that macOS does not sleep (macOS MIDI stops while it sleeps).

Viewer for checking the feel: run `cd examples/pc/web && npm install && npm start`, then open http://localhost:3000 in Chrome.

## License

[LICENSE](LICENSE) (MIT)
