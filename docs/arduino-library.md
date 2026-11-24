# Arduino library API (`mimuz-link`)

The link layer for the firmware: a header-only Arduino library for the client (XIAO ESP32-C6) and the bridge
(XIAO ESP32-S3). It handles pairing, the radio link, the radio channel, sending and receiving MIDI, BLE-MIDI when no
bridge is reachable, and the config commands. The sketch handles only what is particular to the device: reading
switches and encoders, LEDs, sound, and what the MIDI messages mean.

- Folder: `libs/mimuz-link/src/`. Everything is in namespace `mlink`.
- **esp32 Arduino core 3.3.8 only.** The library uses parts of the esp32 core and ESP-IDF that are not public API, and
  the build stops on any other core. Install it with `arduino-cli core install esp32:esp32@3.3.8`. To try another
  core anyway, define `MIMUZ_LINK_ALLOW_UNTESTED_CORE`; the build then only warns.
- Header-only: include it from **one** file of the sketch.

## Contents

- [Client (XIAO ESP32-C6)](#client-xiao-esp32-c6)
  - [Minimal client](#minimal-client)
  - [Config](#config)
  - [Start and loop](#start-and-loop)
  - [Sending MIDI](#sending-midi)
  - [Receiving MIDI](#receiving-midi)
  - [Settings](#settings)
  - [State](#state)
  - [Config commands from the sketch](#config-commands-from-the-sketch)
  - [Diagnostics](#diagnostics)
- [Bridge (XIAO ESP32-S3)](#bridge-xiao-esp32-s3)
- [Build options](#build-options)
- [Board settings](#board-settings)

## Client (XIAO ESP32-C6)

```cpp
#include <mimuz_link_client.h>
```

### Minimal client

From `examples/client/basic/basic.ino` (one button sends a note, notes from the PC switch the LED):

```cpp
#include <mimuz_link_client.h>

static void onMidi(const uint8_t m[3], uint32_t rxUs) {    // called from the library's receive task
  uint8_t type = m[0] & 0xF0;
  if (type == 0x90 && m[2] > 0) digitalWrite(LED_BUILTIN, LOW);                      // Note On
  else if (type == 0x80 || (type == 0x90 && m[2] == 0)) digitalWrite(LED_BUILTIN, HIGH);  // Note Off
}

void setup() {
  Serial.begin(115200);
  pinMode(9, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  mlink::Config cfg;
  cfg.model = "basic";               // what kind of device this is
  mlink::onMidi(onMidi);
  mlink::begin(cfg);                 // ESP-NOW if the bridge is reachable, otherwise BLE-MIDI
}

void loop() {
  static bool was = false;
  bool now = digitalRead(9) == LOW;
  if (now != was) {
    was = now;
    if (now) mlink::sendMidi(0x90, 60, 100);   // channel 1 (status byte 0x90 | channel - 1)
    else mlink::sendMidi(0x80, 60, 0);
  }
  while (Serial.available()) mlink::serialInput((char)Serial.read());   // config commands ("@...")
  mlink::loop();
  delay(1);
}
```

### Config

`mlink::Config` is passed to `begin()`.

| Field | Default | Meaning |
|---|---|---|
| `namePrefix` | `"mi:muz:conESP32C6"` | Device name = prefix + `-` + unique ID (11 characters, from the MAC address). This is the BLE-MIDI device name, and how applications and the bridge know the device. |
| `model` | `""` | What kind of device this is (e.g. `"knob4"`; up to 16 of `A-Z a-z 0-9 - _ .`). Told to the bridge with the label and device name, so that applications and their device drivers can find the clients of a kind. Read only from the outside (`get model`). An invalid value is ignored (a message is printed). |
| `repair` | `false` | `true`: erase the registration and pair again, e.g. when the device is started with a switch held down. |
| `txTaskPriority` | `11` | FreeRTOS priority of the send task (ESP-NOW and BLE-MIDI). |
| `rxTaskPriority` | `10` | Priority of the task that calls the `onMidi` handler. |

### Start and loop

| Function | Description |
|---|---|
| `Transport begin(const Config &cfg)` | Call once in `setup()`. Loads the saved settings, chooses the transport (`TR_ESPNOW` when the registered bridge answers, or a bridge right next to it accepts a new registration; otherwise `TR_BLE`) and starts the send and receive tasks. Returns the chosen transport. |
| `void loop()` | Call from `loop()` every few ms. Keeps the link alive, follows the bridge's channel changes, searches again when the link is lost, tells the bridge the label and listen channels, runs the config commands relayed by the bridge. While waiting on BLE with a registered bridge, it restarts every 20 s to look for the bridge again. |

Pairing: a client that has no registration pairs with a bridge only when it is right next to it (signal of -55 dBm or stronger), and
the bridge stores the registration. Afterwards it connects to that bridge from anywhere in range. `unpair` (config
command), `restart(BOOT_REPAIR)` or `Config::repair` erase the registration.

### Sending MIDI

```cpp
bool sendMidi(uint8_t status, uint8_t d1, uint8_t d2, uint16_t aux = 0, uint32_t waitMs = 0);
```

- Puts one message on the send queue and returns at once. Callable from any task (including a 1 ms scan task).
- `status` is the status byte: `0x90 | (channel - 1)` for Note On on channel 1-16, and so on. Data bytes beyond the
  message's length are ignored.
- Carried: channel messages, and channel-less messages (`F1` `F2` `F3` `F6`, clock `F8`, start `FA`, continue `FB`,
  stop `FC`, active sensing `FE`, reset `FF`). **Not carried: SysEx** (`F0`), and undefined status bytes; `sendMidi`
  returns `false` for them.
- `aux`: any 16-bit value; it travels with the message and shows in the bridge's log (e.g. the time from input to
  send, for measurements).
- `waitMs`: how long to wait when the send queue (128 messages) is full. `0` (default) never blocks; the message is
  then dropped and `false` returned.
- Returns `false` also while connected to nothing over BLE.
- Messages sent close together go out in one radio packet. A Note Off that is not delivered is resent. A Note On that
  has waited longer than `noteon_max_age_ms` is dropped instead of being played late.

### Receiving MIDI

```cpp
typedef void (*MidiHandler)(const uint8_t m[3], uint32_t rxUs);
typedef bool (*MidiFastHandler)(const uint8_t m[3], uint32_t rxUs);
void onMidi(MidiHandler h);
void onMidiFast(MidiFastHandler h);
```

| Function | Description |
|---|---|
| `onMidi(h)` | `h` is called for every received message, from the library's receive task (not from `loop()`); keep shared data safe accordingly. `m` holds 3 bytes (unused bytes are 0); `rxUs` is when it arrived over the radio (`esp_timer`, lower 32 bits, µs). Set it before `begin()`. |
| `onMidiFast(h)` | Optional. `h` is called at once inside the radio receive callback, before `onMidi`. Return `true` to keep the message from `onMidi`. Keep it very short (e.g. take a time stamp). |

What arrives: everything the PC sends to the client's bridge port, and messages from the other clients on the same
bridge on the MIDI channels it listens to (`setRxChannels`). Channel-less messages from other clients arrive when it
listens to any channel.

### Settings

| Function | Description |
|---|---|
| `setRxChannels(uint16_t mask, uint8_t flags = 0)` | MIDI channels to receive from the **other clients** (bit 0 = channel 1 … bit 15 = channel 16; `0xFFFF` = all, `0` = none). Told to the bridge. `flags = ENM_SUB_SELF` also returns its own messages (for tests). Not saved by itself; the config command `set rx_channels` saves it. Default 0. |
| `setNoteOnMaxAgeMs(uint16_t ms)` | Age after which a waiting Note On is dropped (0 = never; default 80). |
| `setRate(uint8_t idx)` | Radio PHY rate: 0 = 1M, 1 = 6M (default), 2 = 24M, 3 = MCS4. Also told to the bridge. |
| `restart(BootRequest r)` | Restarts the device. `BOOT_AUTO`: choose the transport as usual; `BOOT_BLE`: stay on BLE-MIDI; `BOOT_REPAIR`: erase the registration and pair again. |

Settings changed with the config commands (`set …`) are saved in non-volatile memory and loaded by `begin()`. The
functions above change the value in use only (except `restart`).

The label is set only with the config command `set label` (from the PC), not from the sketch.

### State

| Function | Returns |
|---|---|
| `Transport transport()` | `TR_ESPNOW` or `TR_BLE`. |
| `const char *name()` | The device name. |
| `bool isConnected()` | Connected to the bridge (ESP-NOW) or to a BLE-MIDI host. |
| `int port()` | The bridge port, **0-based** (port 1 = 0), -1 when not connected. |
| `uint8_t radioChannel()` | The radio channel (follows the bridge). |
| `uint8_t rate()` | PHY rate index. |
| `uint16_t rxChannels()`, `uint8_t rxFlags()` | As set with `setRxChannels`. |
| `uint16_t noteOnMaxAge()` | As set with `setNoteOnMaxAgeMs`. |
| `uint16_t bleIntervalUnits()`, `uint16_t bleMtuSize()`, `uint32_t bleRetryCount()`, `uint32_t bleDropCount()` | BLE connection interval (1.25 ms units), MTU, retries and drops. |

### Config commands from the sketch

```cpp
bool serialInput(char ch, Print &out = Serial);
```

Feed every character read from the USB serial. Lines starting with `@` are [config commands](config-commands.md),
handled by the library; the reply goes to `out`. Returns `true` when the character belonged to such a line, so that
the sketch can use other characters for its own commands:

```cpp
while (Serial.available()) {
  char c = Serial.read();
  if (mlink::serialInput(c)) continue;   // "@..." lines
  if (c == 's') mlink::printStats(Serial);
}
```

Commands relayed by the bridge over the radio (`port <n> …`) are run by `loop()`; nothing else is needed.

### Diagnostics

| Function | Description |
|---|---|
| `bool radioPing(uint32_t count)` | Measures the radio round trip (client → bridge → client) `count` times; the result is printed to `Serial`. ESP-NOW only. |
| `void printStats(Print &o)` | Statistics (sending, receiving, retries, BLE, round trips). |
| `void clearStats()` | Clears them. |

## Bridge (XIAO ESP32-S3)

```cpp
#define MIMUZ_BRIDGE_NAME_PREFIX "mi:muz:bridgeESPS3"   // optional, before the include
#include <mimuz_link_bridge.h>
```

The bridge is usually the ready-made firmware (`examples/bridge/s3_bridge`); the functions below are for a sketch of
its own.

```cpp
void setup() {
  Serial.begin(115200);
  mlink::begin();          // USB-MIDI (8 ports) + CDC serial; the radio channel survey takes about 12 s
}

void loop() {
  mlink::loop();
  while (Serial.available()) mlink::serialInput((char)Serial.read());
  delay(2);
}
```

| Function | Description |
|---|---|
| `void begin()` | Call once in `setup()`. Starts USB (MIDI with 8 ports and serial), loads the settings and registrations, surveys the radio channels and picks a free one (about 12 s; after a restart caused by a stuck radio it keeps the channel and the port numbers instead). |
| `void loop()` | Call from `loop()`. Config commands (serial and SysEx), relays, logs, keeping the clients' ports. |
| `bool serialInput(char ch, Print &out = Serial)` | As on the client. |
| `void onUsbMidi(UsbMidiHandler h)` | `bool h(uint8_t port, const uint8_t m[3])` sees each message from the PC before it is sent to a client (`port` 0-based; called from the USB receive task). Return `true` to not send it. |
| `void setMidiLogFilter(MidiLogFilter f)` | `bool f(const uint8_t m[3])`: which messages from clients are logged one by one when verbose is on. |
| `bool usbSend(uint8_t port, const uint8_t m[3])` | Sends one message to the PC on a port (0-based). |
| `void setNoteOnMaxAgeMs(uint16_t ms)` | As on the client, for the PC → client direction (default 80). |
| `void setVerbose(bool v)`, `bool isVerbose()` | Log every MIDI message from the clients. |
| `const char *name()` | The USB device name (`<prefix>-<unique ID>`). |
| `uint8_t radioChannel()` | The radio channel in use. |
| `bool portActive(uint8_t port)` | Whether a client is on a port (0-based). |
| `uint32_t rxMidiCount()` | Messages received from clients. |
| `void clearRegistry()` | Erases every registration (connected clients are dropped). |
| `uint8_t resurvey()` | Surveys the channels again and moves to the best one (about 12 s without traffic). Returns it. |
| `void switchChannel(uint8_t ch)` | Moves to channel 1-13. Connected clients are told and follow. |
| `void printStats(Print &o)`, `void clearStats()` | Statistics. |
| `void printList(Print &o)` | Registrations and ports. |
| `void printSurvey()` | The last channel survey (to `Serial`). |
| `void debugDeaf(uint32_t ms)` | For tests: stop listening for `ms` ms (clients are not told). |

The bridge answers the SysEx inquiries and the config commands sent as SysEx by itself (see
[Config commands and SysEx](config-commands.md#sysex-over-usb-midi)).

## Build options

Define before the include, or with arduino-cli `--build-property "compiler.cpp.extra_flags=-D<NAME>"`.

| Macro | Where | Effect |
|---|---|---|
| `MIMUZ_LINK_ALLOW_UNTESTED_CORE` | both | Build on an esp32 core other than 3.3.8 (warning instead of an error). Not supported. |
| `MIMUZ_LINK_NO_IRAM` | both | Place the hot paths in flash instead of internal RAM (slower; for comparison). |
| `MIMUZ_BRIDGE_NAME_PREFIX` | bridge | USB device name prefix (default `"mi:muz:bridgeESPS3"`). Applications find bridges by this name. |
| `MIMUZ_BRIDGE_USB_PID` | bridge | USB product ID (default `0x8057`, experimental). |

`MIMUZ_LINK_VERSION` (in `mimuz_link_proto.h`) holds the library version, e.g. `"0.1.3"`.

## Board settings

| Board | FQBN |
|---|---|
| Client, XIAO ESP32-C6 | `esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app` (Wi-Fi and BLE both included; serial on USB) |
| Bridge, XIAO ESP32-S3 | `esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default` (USB Mode = TinyUSB, required) |

```sh
cd examples/client
arduino-cli compile -u -p <port> --libraries ../../libs \
  --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app basic
```
