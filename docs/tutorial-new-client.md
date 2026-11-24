# Tutorial 2: A new client device

Make a mimuz-link client of your own, step by step. The example is **knob1**: a XIAO ESP32-C6 with one knob (a
potentiometer), one button and one LED. It sends the knob as a Control Change and the button as a note, lights the LED
when the PC sends that note, and comes with a small driver so that a web page can use it by name.

The same steps make any client: a controller with more knobs and keys, a sound module that receives notes, a lamp, …
The library does the radio, pairing, BLE-MIDI and config commands; the sketch does only what the device itself does.

Before this, do [Tutorial 1: Quick start](tutorial-quick-start.md): the bridge, the web site and pairing are used
here as set up there.

## Contents

- [Step 1: Decide what the device sends and receives](#step-1-decide-what-the-device-sends-and-receives)
- [Step 2: Set up the build tools](#step-2-set-up-the-build-tools)
- [Step 3: Wire the knob](#step-3-wire-the-knob)
- [Step 4: Write the sketch](#step-4-write-the-sketch)
- [Step 5: Build and upload](#step-5-build-and-upload)
- [Step 6: Pair and check](#step-6-pair-and-check)
- [Step 7: Settings on the device](#step-7-settings-on-the-device)
- [Step 8: A driver for web pages](#step-8-a-driver-for-web-pages)
- [Step 9: Try the driver](#step-9-try-the-driver)
- [Things to keep in mind](#things-to-keep-in-mind)
- [Next](#next)

## Step 1: Decide what the device sends and receives

Write down the device's **model** and its MIDI messages first. The sketch and the driver both follow this table.

- Model: `knob1` (up to 16 of `A-Z a-z 0-9 - _ .`). It tells applications what kind of device a client is. Give each
  kind of device its own model; several devices of the same kind share it.

| Part | Direction | MIDI (channel 1) |
|---|---|---|
| Knob | device → PC | Control Change 20, value 0-127 |
| Button | device → PC | Note On 60 velocity 100 when pressed, Note Off 60 when released |
| LED | PC → device | Note On 60 (velocity above 0) = on, Note Off 60 = off |

mimuz-link carries channel messages and channel-less messages (clock, start, stop, …), **not SysEx**.

## Step 2: Set up the build tools

mimuz-link builds only with **esp32 Arduino core 3.3.8**. With [arduino-cli](https://arduino.github.io/arduino-cli/):

```sh
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.8
```

The library is in this repository (`libs/mimuz-link`) and is passed to the compiler with `--libraries`, so nothing
needs to be installed for it.

With the Arduino IDE instead: install **esp32 by Espressif Systems**, version **3.3.8**, in the Boards Manager, and copy
the folder `libs/mimuz-link` into the `libraries` folder of your sketchbook (for example `~/Documents/Arduino/libraries/`).

## Step 3: Wire the knob

A potentiometer (10 kΩ, linear) on the XIAO ESP32-C6:

| Potentiometer | XIAO ESP32-C6 |
|---|---|
| one end | 3V3 |
| wiper (middle) | D0 (A0) |
| other end | GND |

The button and the LED are the board's own BOOT button and user LED. Without a potentiometer, everything except the
knob still works.

## Step 4: Write the sketch

Start from the basic example (`examples/client/basic/basic.ino`), which already has the button, the LED, the config
commands and the library calls. Copy it into a new sketch folder; the file name must match the folder name:

```sh
cd examples/client
cp -R basic knob1
mv knob1/basic.ino knob1/knob1.ino
```

Then, in `knob1.ino`:

1. Rewrite the comment at the top: what the device does, and `knob1` in the build line.
2. Under **Board settings**, add the knob's pin (`KNOB_PIN`, A0). The button and LED settings (`BASIC_BUTTON_PIN`,
   `BASIC_LED_PIN`, `BASIC_LED_ON`) stay as they are.
3. Under **Application**, add `KNOB_CC` (20) and `KNOB_MS` (how often the knob is read).
4. In `onMidi()`, react only to the button's note on channel 1 (step 1).
5. In `setup()`, set `cfg.model = "knob1"` and `analogReadResolution(12)`, and change the name in the ready message.
6. Add `readKnob()`, and call it every `KNOB_MS` from `loop()`.

The result:

```cpp
/*

 knob1 mimuz-link client: one knob, one button and one LED (made from the basic example)

*/
// What it does:
//   - turning the knob (a potentiometer on A0) sends Control Change 20 (channel 1)
//   - pressing the button sends Note On (middle C, channel 1), releasing it sends Note Off
//   - Note On / Note Off of middle C from the PC turn the LED on / off
//   - lines starting with "@" on the USB serial are config commands (web console, examples/pc/mlink_config.py)
//
// Build (from examples/client), XIAO ESP32-C6:
//   arduino-cli compile --libraries ../../libs --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app knob1
#include <mimuz_link_client.h>

// ---------------- Board settings ----------------

// Button: the BOOT button by default (GPIO9 on ESP32-C3/C6/H2, GPIO0 on the others), pressed = LOW
#ifndef BASIC_BUTTON_PIN
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
#define BASIC_BUTTON_PIN 9
#else
#define BASIC_BUTTON_PIN 0
#endif
#endif

// LED: the board's built-in LED; -1 = no LED
#ifndef BASIC_LED_PIN
#ifdef LED_BUILTIN
#define BASIC_LED_PIN LED_BUILTIN
#else
#define BASIC_LED_PIN -1
#endif
#endif

// Level that turns the LED on (the XIAO boards' user LED is on at LOW)
#ifndef BASIC_LED_ON
#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32C6) || defined(ARDUINO_XIAO_ESP32S3)
#define BASIC_LED_ON LOW
#else
#define BASIC_LED_ON HIGH
#endif
#endif

// Knob: a potentiometer on A0 (D0 of the XIAO ESP32-C6)
#ifndef KNOB_PIN
#define KNOB_PIN A0
#endif

// ---------------- Application ----------------

static const uint8_t NOTE = 60;      // middle C
static const uint8_t VELOCITY = 100;
static const uint8_t CHANNEL = 0;    // 0 = MIDI channel 1
static const uint32_t DEBOUNCE_MS = 10;
static const uint8_t KNOB_CC = 20;   // Control Change number of the knob
static const uint32_t KNOB_MS = 5;   // how often the knob is read

static void led(bool on) {
  if (BASIC_LED_PIN >= 0) digitalWrite(BASIC_LED_PIN, on ? BASIC_LED_ON : !BASIC_LED_ON);
}

// Received MIDI (called from the mimuz-link receive task, not from loop())
static void onMidi(const uint8_t m[3], uint32_t rxUs) {
  (void)rxUs;
  if ((m[0] & 0x0F) != CHANNEL || m[1] != NOTE) return;  // only the button's note
  uint8_t type = m[0] & 0xF0;
  if (type == 0x90 && m[2] > 0) led(true);                          // Note On
  else if (type == 0x80 || (type == 0x90 && m[2] == 0)) led(false);  // Note Off (or Note On with velocity 0)
}

void setup() {
  Serial.begin(115200);
  pinMode(BASIC_BUTTON_PIN, INPUT_PULLUP);
  if (BASIC_LED_PIN >= 0) pinMode(BASIC_LED_PIN, OUTPUT);
  led(false);
  analogReadResolution(12);  // 0-4095

  mlink::Config cfg;  // defaults: device name "mi:muz:conESP32C6-<unique ID>", pairs with a bridge on first start
  cfg.model = "knob1";  // what kind of device this is: applications find clients of this kind by it
  mlink::onMidi(onMidi);
  mlink::Transport tr = mlink::begin(cfg);
  Serial.printf("knob1: ready (%s)\n", tr == mlink::TR_BLE ? "BLE-MIDI" : "ESP-NOW");
}

// The knob: smoothed, and sent only when it really moved (so that a resting knob sends nothing)
static void readKnob() {
  static int avg = -1;    // smoothed reading, 0-4095
  static int sent = -1;   // last value sent, 0-127
  static int sentAt = 0;  // the reading it was sent at
  int raw = analogRead(KNOB_PIN);
  avg = avg < 0 ? raw : (avg * 7 + raw) / 8;
  int v = avg * 127 / 4095;
  if (v == sent) return;
  if (sent >= 0 && abs(avg - sentAt) < 24 && v != 0 && v != 127) return;  // noise around a step
  sent = v;
  sentAt = avg;
  mlink::sendMidi(0xB0 | CHANNEL, KNOB_CC, v);
}

void loop() {
  static uint32_t knobMs = 0;
  if (millis() - knobMs >= KNOB_MS) {
    knobMs = millis();
    readKnob();
  }

  // Button, with a simple debounce: the level has to stay the same for DEBOUNCE_MS
  static bool pressed = false;
  static bool lastRead = false;
  static uint32_t changedMs = 0;
  bool now = digitalRead(BASIC_BUTTON_PIN) == LOW;
  uint32_t ms = millis();
  if (now != lastRead) {
    lastRead = now;
    changedMs = ms;
  } else if (now != pressed && ms - changedMs >= DEBOUNCE_MS) {
    pressed = now;
    if (pressed) mlink::sendMidi(0x90 | CHANNEL, NOTE, VELOCITY);
    else mlink::sendMidi(0x80 | CHANNEL, NOTE, 0);
  }

  // Config commands from the USB serial (lines starting with "@")
  while (Serial.available()) mlink::serialInput((char)Serial.read());

  mlink::loop();  // keeps the link alive, follows the bridge, searches again when the link is lost
  delay(1);
}
```

What each part does:

- Board settings (`#define`): each can be changed at build time, for example
  `--build-property "compiler.cpp.extra_flags=-DKNOB_PIN=A1"`.
- `#include <mimuz_link_client.h>`: the client side of the library (namespace `mlink`). Include it from one file only.
- `cfg.model = "knob1"`: the model from step 1. The bridge learns it when the client connects, and applications find
  the client by it.
- `mlink::begin(cfg)`: once in `setup()`. It connects to the paired bridge over ESP-NOW, pairs with a bridge right next
  to it if the client has none, or becomes a BLE-MIDI device when no bridge answers.
- `mlink::sendMidi(status, d1, d2)`: puts one message on the send queue and returns at once. The status byte carries the
  channel: `0xB0 | 0` is Control Change on channel 1.
- `onMidi`: called for each message from the PC, from the library's **receive task**, not from `loop()`. Keep it short
  and do not wait in it.
- `mlink::serialInput()`: lines starting with `@` on the USB serial are config commands (used by the console's
  **Load device (serial)** and `examples/pc/mlink_config.py`).
- `mlink::loop()`: call it all the time from `loop()`. It keeps the connection, follows the bridge's radio channel,
  and runs the config commands that the bridge relays over the radio.
- The knob is smoothed and sent only when it moved by more than a little, so that a resting knob sends nothing.

Every function is in the [Arduino library API](arduino-library.md).

## Step 5: Build and upload

Plug the XIAO ESP32-C6 into the PC and find its port with `arduino-cli board list`. Then, from `examples/client`:

```sh
arduino-cli compile -u -p <port> --libraries ../../libs \
  --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app knob1
```

- `PartitionScheme=huge_app`: the firmware holds both Wi-Fi (ESP-NOW) and BLE, which do not fit the default partition.
- `CDCOnBoot=cdc`: `Serial` is the USB port, where the config commands arrive.
- In the Arduino IDE: board **XIAO_ESP32C6**, **USB CDC On Boot: Enabled**, **Partition Scheme: Huge APP (3MB No
  OTA/1MB SPIFFS)**.
- A brand-new XIAO ESP32-C6 has to be unplugged and plugged in again once after its first upload.

The Firmware uploader of the web site writes only the firmware of this repository; write your own with arduino-cli or
the Arduino IDE.

## Step 6: Pair and check

1. With the bridge running, put the client right next to it and power it, as in
   [Tutorial 1, step 4](tutorial-quick-start.md#step-4-pair-the-client-with-the-bridge). A client that was already
   paired with this bridge (for example with the basic example) stays paired: writing new firmware keeps the pairing.
2. In the **Console** page, the bridge's **Clients** tab lists the client with the model **knob1**.
3. Turn the knob: Control Change 20 arrives on the client's port (`b0 14 <value>` in `examples/node/monitor.mjs`).
   Press the button: Note On / Note Off 60.
4. Send Note On 60 on the client's port: the LED lights.

The client can also be checked over its own USB serial: in the **Console** page, press **Load device (serial)** and
choose its port. Its **Overview** shows `info`, including `model=knob1`.

## Step 7: Settings on the device

Settings that belong to one device are kept on the device and changed with [config commands](config-commands.md), from
the console or from an application:

- `label`: a name for this one device (step 6 of Tutorial 1). Use it when there are several knob1s.
- `rx_channels`: the MIDI channels the client also receives from the **other clients** on the same bridge (for example
  a sound module that plays what a keyboard client sends on channel 1). Messages from the PC to the client's port
  always arrive; knob1 needs nothing else.

Settings made with `set` are saved on the client and kept when new firmware is written.

## Step 8: A driver for web pages

Applications do not use the client's port number: ports follow the order in which clients connect. They use a small
**device driver** per model, which finds the device by its model (and label or device name), follows it when it
reconnects, and turns its MIDI messages into what the device means.

A device driver extends `MimuzDevice` of the web driver `web/www/drivers/mimuz-link.mjs`. The base class does the
MIDI access, finding the device and sending; the driver adds only what is particular to knob1. Start from the template
`web/www/drivers/device-template.mjs`:

```sh
cp web/www/drivers/device-template.mjs web/www/drivers/knob1.mjs
```

For reference, `web/www/drivers/basic-device.mjs` is the driver of the basic example (Tutorial 1), made from the
template in the same way. Then, in `knob1.mjs`, as the comment at the template's top says:

1. Rename the class `Template` to `Knob1`, and set `static model` to `'knob1'` (the `cfg.model` of the sketch).
2. Keep `CHANNEL = 1`, and add the numbers of step 1 (`KNOB_CC`, `BUTTON_NOTE`).
3. In `onMidi()`, replace the example events with `knob` and `button`.
4. Replace the example methods with `setLed()`.
5. Replace the comment at the top with what the driver's events and methods are.

The result:

```js
// knob1.mjs: device driver for the "knob1" clients (made from device-template.mjs)
//   events: knob ({ value }), button ({ pressed }), and those of MimuzDevice (connect, disconnect, change, midi)
//   methods: setLed(on), and those of MimuzDevice
import { MimuzDevice } from './mimuz-link.mjs';

const CHANNEL = 1;      // the MIDI channel the device uses (1-16)
const KNOB_CC = 20;
const BUTTON_NOTE = 60;

export default class Knob1 extends MimuzDevice {
  static model = 'knob1';

  // Every message from the device: the knob and the button become events
  onMidi(data) {
    const [status, d1, d2] = data;
    if (status >= 0xF0 || (status & 0x0F) !== CHANNEL - 1) return;  // only channel messages on CHANNEL
    const type = status & 0xF0;
    if (type === 0xB0 && d1 === KNOB_CC) this.emit('knob', { value: d2 });
    else if ((type === 0x90 || type === 0x80) && d1 === BUTTON_NOTE) this.emit('button', { pressed: type === 0x90 && d2 > 0 });
  }

  // The LED follows Note On / Note Off of the button's note
  setLed(on) {
    return on ? this.noteOn(CHANNEL, BUTTON_NOTE, 127) : this.noteOff(CHANNEL, BUTTON_NOTE);
  }
}
```

- `static model = 'knob1'`: the driver follows the first connected client of this model, whichever bridge and port it
  is on.
- `onMidi(data)` is called with every message from the device. The driver reads only the messages of step 1 and sends
  them on as its own events (`emit`); everything else is ignored.
- `setLed` uses the sending methods of the base class (`noteOn`, `noteOff`, `controlChange`, …; channels 1-16).
- The base class also gives `connected`, `label`, `name`, `command()` (config commands), `setLabel()`, and the events
  `connect`, `disconnect` and `midi`.

The application imports only `knob1.mjs`; it does not need to know `mimuz-link.mjs`. The base class is described in
[Device drivers (MimuzDevice)](js-driver.md#device-drivers-mimuzdevice).

## Step 9: Try the driver

Make a test page `web/www/knob1.html`:

```html
<!doctype html>
<meta charset="utf-8">
<title>knob1 test</title>
<p id="state">Waiting for a knob1…</p>
<p>Knob: <meter id="knob" min="0" max="127" value="0"></meter> <span id="value">–</span></p>
<p>Button: <span id="button">released</span></p>
<label><input type="checkbox" id="led"> LED</label>
<script type="module">
  import Knob1 from './drivers/knob1.mjs';

  const $ = (id) => document.getElementById(id);
  const knob = new Knob1();
  knob.addEventListener('connect', () => { $('state').textContent = `Connected: ${knob.label || 'knob1'}`; });
  knob.addEventListener('disconnect', () => { $('state').textContent = 'Waiting for a knob1…'; });
  knob.addEventListener('knob', (e) => { $('knob').value = e.detail.value; $('value').textContent = e.detail.value; });
  knob.addEventListener('button', (e) => { $('button').textContent = e.detail.pressed ? 'pressed' : 'released'; });
  $('led').addEventListener('change', (e) => knob.setLed(e.target.checked));
  try {
    await knob.open();   // the browser asks for MIDI (with SysEx)
  } catch (e) {
    $('state').textContent = e.message;
  }
</script>
```

- `new Knob1()` takes the first knob1; `new Knob1({ label: 'left' })` picks one by its label when there are several.
- `open()` asks for MIDI. Several drivers on one page (for example a knob1 and a pad) share one MIDI access, so the
  browser asks only once. When MIDI is not allowed, `open()` rejects with the reason.
- Add the listeners before or after `open()`; the `connect` event comes after `open()` either way.

With `npm start` running, open http://localhost:8080/knob1.html and allow MIDI. When the knob1 connects, turning the
knob moves the meter, the button shows its state, and the check box switches the LED.

The same driver runs in Node.js 20 or later (see [Node.js](js-driver.md#nodejs)). Pass the MIDI library to `open()`,
and call `close()` at the end: open MIDI ports keep the process running.

```js
import Knob1 from '../../web/www/drivers/knob1.mjs';

const knob = new Knob1();
knob.addEventListener('knob', (e) => console.log('knob', e.detail.value));
await knob.open({ nodeMidi: await import('@julusian/midi') });
process.on('SIGINT', () => knob.close());
```

Put it in `examples/node/` (where `npm install` has installed `@julusian/midi`), and stop it with Ctrl-C.

## Things to keep in mind

- **Over BLE-MIDI** (no bridge reachable), applications see only the device name: no model and no label, and no config
  commands. A driver that looks for `model: 'knob1'` finds the device only through a bridge.
- `onMidi` runs in the library's receive task. Data it shares with `loop()` (or with other tasks) has to be handled
  with that in mind; setting a pin or a flag is fine.
- `sendMidi` can be called from any task, for example from a 1 ms scan task of a device with many switches. It does not
  block: when the send queue (128 messages) is full, the message is dropped and `false` returned.
- Messages sent close together go out in one radio packet. A Note Off that is not delivered is resent; a Note On that
  waited too long (80 ms by default) is dropped instead of being played late.
- The library handles each message on its own: it does not pair Note On with Note Off.

## Next

- [Arduino library API](arduino-library.md): every function of the library, the bridge side, build options.
- [Web driver API](js-driver.md): bridges, clients, commands and events in web pages and Node.js.
- [Config commands and SysEx](config-commands.md): every setting of clients and bridges.
