# mimuz-link

A wireless MIDI connection environment that supports ESP-NOW and BLE-MIDI.

Clients (XIAO ESP32-C6, used as controllers or sound modules) connect over ESP-NOW to a bridge (XIAO ESP32-S3, a USB-MIDI dongle plugged into the PC). The PC sees the bridge as a USB-MIDI device with 8 ports. When no bridge is found, a client connects to the PC directly as a BLE-MIDI device.

- Low latency: the ESP-NOW hop is under 1 ms one way (median). The round trip PC → bridge → client → bridge → PC has a median of about 1.6 ms.
- Multiple devices: up to 8 clients can be connected to one bridge at the same time. Messages can also be delivered between clients, per MIDI channel.
- Delivery: Note Off messages are resent if they are not delivered. Note On messages that have waited too long are dropped.
- Configuration: one-line commands over USB serial. Settings are stored in non-volatile memory.

Documentation: [Overview](docs/overview.md) · Tutorial: [1. Quick start](docs/tutorial-quick-start.md),
[2. A new client device](docs/tutorial-new-client.md) · [API reference](docs/README.md). The same pages are shown on
the web site of this repository (`npm start`, then http://localhost:8080).
日本語: [概要](docs/ja/overview.md) · チュートリアル: [1. クイックスタート](docs/ja/tutorial-quick-start.md),
[2. 新規デバイスの作成](docs/ja/tutorial-new-client.md).

> **Note: esp32 Arduino core 3.3.8 only.** mimuz-link uses parts of the esp32 core and ESP-IDF that are not public API, so it works only with the version it was verified on, **esp32 core 3.3.8**. Other cores (older ones, and 3.3.9 and later including 4.x) are not supported, and the build stops with an error on them. Install the verified core from the official package index with `arduino-cli core install esp32:esp32@3.3.8` (in the Arduino IDE: Boards Manager → esp32 by Espressif Systems → version 3.3.8), and do not upgrade it while you use mimuz-link. See [Build and upload](#build-and-upload-arduino-cli-esp32-core-338).

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
  client/basic/                smallest client: one button (Note On/Off) and one LED, no external wiring
    basic.ino
  client/c6_test_device/       test client: bare C6 board (no external wiring)
    c6_test_device.ino           test message handling, sound-module stand-in, serial commands
    workload.h                   simulated controller workload (1 ms scan, switch, encoder simulation)
  bridge/s3_bridge/            bridge: test handling and serial commands
    s3_bridge.ino
  pc/                          PC-side measurement and config tools (Python)
  pc/web/                      viewer for checking the feel by ear and eye (Web MIDI)
  node/                        the web driver on Node.js (monitor.mjs: bridges, clients, MIDI, ping)
docs/                          overview, tutorials and API reference (Markdown; also shown on the web site)
docs/ja/                       the overview and the tutorials in Japanese
web/                           web site (browser): overview, tutorial, API reference, console, firmware uploader
  www/drivers/                 web driver for applications: mimuz-link.mjs (+ node-midi-access.mjs for Node.js), device-template.mjs,
                                 basic-device.mjs (driver of examples/client/basic)
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

Carried messages: channel messages and channel-less messages (clock, start, stop, song position, and so on). SysEx is not carried; the bridge answers the mimuz-link SysEx inquiries itself (see [docs/config-commands.md](docs/config-commands.md#sysex-over-usb-midi)).

The link layer handles each MIDI message on its own. It does not pair Note On with Note Off and does not track which notes are sounding.

### Application API

The full reference is in [docs/](docs/README.md): [Arduino library](docs/arduino-library.md) (client and bridge
firmware), [web driver](docs/js-driver.md) (web pages and Node.js) and [config commands and SysEx](docs/config-commands.md).

```cpp
#include <mimuz_link_client.h>               // client (C6), namespace mlink
mlink::Config cfg;
cfg.model = "knob4";                         // what kind of device this is
mlink::onMidi(handler);                      // received MIDI (called from the receive task)
mlink::begin(cfg);                           // once in setup(): ESP-NOW if the bridge is reachable, otherwise BLE-MIDI
mlink::sendMidi(0x90, 60, 100);              // from any task
mlink::loop();                               // from loop()
```

- This is a header-only library: include it from only one file of the sketch.
- The library uses parts of the esp32 core and ESP-IDF that are not public API (the radio retry counter, the Wi-Fi driver lock, the TinyUSB interface hooks), so it is verified only with esp32 core 3.3.8 and the build stops with an error on any other core. Install it with `arduino-cli core install esp32:esp32@3.3.8`. To try another core anyway, define `MIMUZ_LINK_ALLOW_UNTESTED_CORE` (with arduino-cli: `--build-property "compiler.cpp.extra_flags=-DMIMUZ_LINK_ALLOW_UNTESTED_CORE"`); the build then only warns.
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

# Smallest client example (same board settings; the test library is not needed)
arduino-cli compile -u -p <port> --libraries ../../libs \
  --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app basic
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

## Config commands

Bridges and clients are set up with one-line text commands, over USB serial, as SysEx over USB-MIDI (bridge), or
relayed by the bridge over the radio to a client (`port <n> …`):

```
@1 info
@1 ok name=mi:muz:bridgeESPS3-QEwFK6XO000 kind=bridge fw=0.1.3 radio_channel=7 ports_used=6 ports=8 registered=6 temp_c=67.4
@2 port 1 set label left
@2 ok label=left
```

Every command and setting, and the SysEx messages of the bridge (`F0 7D 4D 4C …`, including the port changes it
tells by itself): [docs/config-commands.md](docs/config-commands.md).

Tool for trying commands from the PC: `examples/pc/mlink_config.py <serial port> "command" …`

## Serial commands (test firmware)

- C6 (`c6_test_device`): `p` / `P` = measure the radio round trip 10,000 / 100,000 times, `s` = statistics, `c` = clear statistics, `r0`–`r3` = PHY rate, `b` = reboot fixed to BLE, `e` = reboot in automatic mode, `u` = erase registration and register again.
- Bridge (`s3_bridge`): `s` = statistics, `c` = clear statistics, `l` = list registrations and ports, `x` = erase all registrations, `v` = toggle logging of every message, `h` = radio channel survey results, `H` = survey again and choose, `1`–`9` and `A`–`D` = move to radio channel 1–13, `z` = stop receiving for 150 ms.

## Web site (`web/`)

Pages in the browser: **Overview**, **Tutorial** and **API reference** (the Markdown files of `docs/`), **Console** and
**Firmware uploader**. The console is a maintenance page (Web MIDI and Web Serial, Chrome or Edge) for the bridges and clients: device list, clients of each bridge, settings, radio channel, diagnostics and a command console. Bridges on USB-MIDI are reached through the web driver below; a device attached over USB serial can be loaded as well. `npm start` at the top of the repository, then open http://localhost:8080. See `web/README.md`.

## Web driver (`web/www/drivers/mimuz-link.mjs`)

The low-level driver for web pages and Node.js (Web MIDI with SysEx; one ES module, no dependencies). It finds the
bridges, learns which client is on which bridge port, and lets the page pick a client by its device name, label or
model and exchange MIDI with it, whichever port it is on and however often it reconnects. Drivers for particular
devices, one per model, extend its base class `MimuzDevice`; applications then import only those drivers.

```js
import MimuzLink from './drivers/mimuz-link.mjs';

const link = new MimuzLink();
if (!(await link.init())) throw new Error(link.error);
const knob = link.client({ model: 'knob4' });       // follows the first connected client of that model
knob.addEventListener('connect', () => knob.noteOn(1, 60, 100));
knob.addEventListener('midi', (e) => console.log(e.detail.data));
```

In Node.js (20 or later) it runs on [@julusian/midi](https://www.npmjs.com/package/@julusian/midi):
`new MimuzLink({ nodeMidi: await import('@julusian/midi') })`. Example: `examples/node/monitor.mjs`.

Reference: [docs/js-driver.md](docs/js-driver.md).

## Versions and releases

The version is kept in the top `package.json` and in the git tag `v<version>`.

```sh
npm run release -- 0.2.0                        # see below; nothing is pushed
git push origin main && git push origin v0.2.0  # the tag last
```

`npm run release -- <version>` (web/firmware/release.sh), on a clean working tree:

1. writes the version into package.json, the firmware (`MIMUZ_LINK_VERSION`), library.properties and
   web/package.json (`npm version <version> --no-git-tag-version`), and commits "Version <version>";
2. builds the bridge and client firmware from that commit (`npm run build`, arduino-cli) and commits it
   (web/www/firmware, "Firmware <version>");
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
