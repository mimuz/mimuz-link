# Web driver API (`mimuz-link.mjs`)

The low-level driver for applications in the browser (Web MIDI) and in Node.js. It finds the bridges among the MIDI
ports, learns which client is on which bridge port, and lets the application pick a client by its device name, label
or model and exchange MIDI with it, whichever port it is on and however often it reconnects.

Drivers for particular devices (what their buttons, knobs or sounds are; one per model) are written on top of it. The
driver itself knows nothing about them.

- Files: `web-console/www/drivers/mimuz-link.mjs` (ES module, no dependencies) and, for Node.js only,
  `web-console/www/drivers/node-midi-access.mjs`.
- Needs: Web MIDI **with SysEx** in the browser (Chrome or Edge; the page must be in a secure context such as
  `https://` or `http://localhost`), or Node.js 20 or later with [@julusian/midi](https://www.npmjs.com/package/@julusian/midi).
- Talks to the bridges with the SysEx messages described in [Config commands and SysEx](config-commands.md#sysex-over-usb-midi).

## Contents

- [Quick start](#quick-start)
- [Concepts](#concepts)
- [MimuzLink](#mimuzlink)
- [Client](#client)
- [ClientRef](#clientref)
- [Bridge](#bridge)
- [Command results](#command-results)
- [Sending MIDI](#sending-midi)
- [Module exports](#module-exports)
- [Node.js](#nodejs)
- [Writing a device driver](#writing-a-device-driver)

## Quick start

```js
import MimuzLink from './drivers/mimuz-link.mjs';

const link = new MimuzLink();
if (!(await link.init())) throw new Error(link.error);   // init() resolves null without Web MIDI with SysEx

const knob = link.client({ model: 'knob4' });  // follows the first connected client of that model
knob.addEventListener('connect', () => knob.noteOn(1, 60, 100));   // channels are 1-16
knob.addEventListener('midi', (e) => console.log(e.detail.data, e.detail.timeStamp));
knob.addEventListener('disconnect', () => console.log('gone'));

const r = await knob.command('get rx_channels');  // config command, relayed by its bridge
if (r.ok) console.log(r.fields.rx_channels);

link.addEventListener('change', () => console.log(link.bridges, link.clients));
```

## Concepts

| Term | Meaning |
|---|---|
| bridge | A XIAO ESP32-S3 plugged into the PC. It appears as one USB-MIDI device with 8 ports, named `mi:muz:bridgeESPS3-<unique ID> Port 1` … `Port 8` (the exact port names depend on the operating system). |
| client | A XIAO ESP32-C6 device (controller, sound module, …). It is reached through a bridge over ESP-NOW (`via: 'espnow'`), or directly as a BLE-MIDI device when no bridge is reachable (`via: 'ble'`). |
| port | The bridge port (1-8) a client is on. A client gets the lowest free port when it connects, so the port of a device can change from one connection to the next. Applications should not keep port numbers. |
| device name | `mi:muz:conESP32C6-<unique ID>`: unique per device and fixed. Use it to pick one particular device. |
| label | A name the user gives a client (up to 30 of `A-Z a-z 0-9 - _ .`), saved on the client. Several clients may share one on purpose. |
| model | What kind of device the client is (up to 16 of `A-Z a-z 0-9 - _ .`, e.g. `knob4`), fixed by its firmware. Device drivers look for clients by model. Empty on firmware before 0.1.3 and over BLE. |

Over BLE only the device name is known: no label, no model, and no config commands (they need a bridge).

## MimuzLink

The default export: all bridges and clients the application can reach.

### `new MimuzLink(options)`

| Option | Default | Meaning |
|---|---|---|
| `pollMs` | `1000` | How often bridges older than SysEx format version 2 (firmware before 0.1.3) are asked about their ports. They do not tell changes by themselves. |
| `pollMs2` | `0` | The same for bridges of format version 2 or later, which tell changes by themselves. `0` = never ask. |
| `clientPrefixes` | `['mi:muz:conESP32C6-']` | Device name prefixes of clients that appear on BLE-MIDI. |
| `nodeMidi` | — | Node.js only: the `@julusian/midi` module (see [Node.js](#nodejs)). |

### Members

| Member | Description |
|---|---|
| `await init()` | Asks for Web MIDI with SysEx (in Node.js: opens RtMidi) and starts looking for bridges and clients. Resolves the link itself, or `null` when MIDI is not available. The reason is then in `error`. |
| `error` | Why `init()` resolved `null` (a message for the user), otherwise `''`. |
| `close()` | Stops everything: bridges are dropped, `ClientRef`s released, and in Node.js the MIDI ports are closed so that the process can end. |
| `bridges` | `Bridge[]`: the bridges found (also those still starting). |
| `clients` | `Client[]`: the connected clients, those on bridges first (by bridge and port), then those on BLE. |
| `find(sel)` | The first connected client matching `sel`, or `null`. |
| `findAll(sel)` | All connected clients matching `sel`. |
| `matches(client, sel)` | Whether a client matches `sel`. |
| `client(sel)` | A new [`ClientRef`](#clientref) that follows the first connected client matching `sel`. |
| `midi` | The `MIDIAccess` in use (read only; `null` before `init()`). |
| `options` | The options in effect. |

A selector `sel` is an object with any of `name`, `label`, `model` and `via` (`'espnow'` or `'ble'`). All keys given
must match exactly. An empty selector `{}` matches any client.

```js
link.find({ name: 'mi:muz:conESP32C6-j3f5__w9GK0' });   // one particular device
link.findAll({ model: 'knob4' });                        // every connected knob4
link.find({ label: 'left', via: 'espnow' });             // the client labelled "left", through a bridge
```

### Events

| Event | `detail` | When |
|---|---|---|
| `change` | `{ reason, reasons }`. `reason` is the first of `reasons` (`'midi'`: MIDI ports came or went; `'bridge'`: a bridge started or its information changed; `'port'`: a client connected, left, moved or changed its label) | Bridges or clients came, went or changed. Several changes in a row are told once, after every `ClientRef` has followed them. |
| `midi` | `{ client, data, timeStamp }` | MIDI from any client (`data`: `Uint8Array`, `timeStamp`: as in Web MIDI, ms on the `performance.now()` clock). |

## Client

One client device, by its device name. The object stays the same while the client disconnects, comes back, or moves
to another port or bridge, so it may be kept. Get clients from `link.clients`, `link.find()` or `link.findAll()`.

### Properties

| Property | Description |
|---|---|
| `name` | Device name (unique). |
| `label` | Label (`''` if none). |
| `model` | Model (`''` if not given). |
| `connected` | `true` while it is reachable. |
| `via` | `'espnow'` (through a bridge), `'ble'` (BLE-MIDI) or `null` (not connected). |
| `bridge` | The `Bridge` it is on (`via === 'espnow'`), otherwise `null`. |
| `port` | Its bridge port 1-8 (`via === 'espnow'`), otherwise `0`. |
| `output` | The `MIDIOutput` used for sending, `null` while not connected (or while its bridge port is still being learned). |

### Methods

| Method | Description |
|---|---|
| `send(data, timeStamp?)` | Sends one MIDI message (`data`: array or `Uint8Array`). Returns `false` if the client is not connected or the port refused it. Throws on SysEx, which is not carried to clients. |
| `noteOn(ch, note, vel = 100, timeStamp?)` … | Helpers for the channel messages, see [Sending MIDI](#sending-midi). |
| `await command(text, { timeout })` | Runs a [config command](config-commands.md) on the client, relayed by its bridge (`port <n> <text>`). Resolves a [command result](#command-results). Fails with `not_connected`, or `not_available_over_ble` over BLE. Default timeout 5000 ms. |
| `await setLabel(label)` | `set label <label>` (`''` clears it), then asks the bridge for the port again so that `label` is updated. Resolves the command result. |

### Events

| Event | `detail` | When |
|---|---|---|
| `connect` | — | It became reachable. |
| `disconnect` | — | It left. |
| `change` | — | Its label, model, port or bridge changed while connected. |
| `midi` | `{ data, timeStamp, client }` | MIDI from it. |

## ClientRef

The handle a device driver keeps. It follows whichever connected client matches its selector: when that client
leaves, it moves to another matching client if there is one, and it comes back by itself when one connects again.
Create one with `link.client(sel)`.

It has the same properties (`name`, `label`, `model`, `connected`, `via`, `bridge`, `port`), sending methods,
`command()`, `setLabel()` and events (`connect`, `disconnect`, `change`, `midi`) as `Client`, forwarded to the client it
follows. While it follows none, the properties are empty, sending returns `false` and `command()` resolves
`{ ok: false, error: 'not_connected' }`.

| Member | Description |
|---|---|
| `selector` | The selector it was made with. |
| `client` | The `Client` it follows now, or `null`. |
| `await ready({ timeout = 0 })` | Resolves the `ClientRef` once a matching client is connected (at once if one is). Rejects with `Error('timeout')` after `timeout` ms; `0` = wait for ever. |
| `release()` | Stops following. The link keeps no reference to it afterwards. |

- It first looks for a client in a microtask after it is created, so listeners added right after `link.client()` hear
  the first `connect`.
- `change` is also sent when it switches from one matching client to another without a gap.

```js
const pad = link.client({ model: 'pad8' });
pad.addEventListener('connect', () => console.log('pad on', pad.bridge?.name, 'port', pad.port));
await pad.ready({ timeout: 10000 });
pad.controlChange(1, 7, 100);
```

## Bridge

One bridge on USB-MIDI. Get bridges from `link.bridges` (or `client.bridge`).

| Member | Description |
|---|---|
| `key` | `mi:muz:bridgeESPS3-<unique ID>`, as in its MIDI port names. |
| `name` | Its device name once it has answered, `key` before. |
| `ready` | `true` once it has answered. A bridge does not answer for about 12 s after it starts (radio channel survey). |
| `info` | `{ name, fw, channel, portsUsed, ports, format }` once it has answered, otherwise `null`. `channel`: radio channel; `format`: SysEx format version (2 from firmware 0.1.3). |
| `ports` | `ports[1]` … `ports[8]`: `{ state, name, label, model }`. `state`: 0 = free, 1 = a client is connected but has not told its name yet, 2 = connected and known. |
| `busy` | `true` while a command is running. |
| `output(port)` | The `MIDIOutput` of bridge port 1-8, or `null` while it is not learned yet. |
| `inquirePort(port)` | Asks the bridge about one port again (the answer updates `ports` and the clients). |
| `await command(text, { timeout = 3000 })` | Runs a [config command](config-commands.md) on the bridge. `port <n> …` relays it to a client. Commands are queued and sent one at a time. Resolves a [command result](#command-results); **rejects** with `Error('timeout: …')` when no reply arrives in time. |

| Event | `detail` | When |
|---|---|---|
| `ready` | — | It answered for the first time (after starting, or after its ports came back). |
| `change` | `{ port }` | A port's client changed. |
| `line` | `{ dir: 'out' \| 'in', text }` | Every config command line sent and received (for a console). |
| `busy` | — | `busy` changed. |

## Command results

`Bridge.command()`, `Client.command()` and `ClientRef.command()` resolve an object made from the reply lines (see
[Config commands](config-commands.md#line-format)):

| Field | Description |
|---|---|
| `ok` | `true` for an `ok` reply, `false` for `err` (and for failures before sending). |
| `fields` | The `key=value` pairs of the `ok` line. Values that look like numbers become numbers, except `label`, `name`, `mac`, `bridge`, `fw` and `model`, which stay strings. Hex values such as `0x00FF` stay strings. |
| `items` | One object per intermediate `+` line (for example one per port of `ports`), parsed like `fields`. |
| `error` | The reason of an `err` reply (e.g. `unknown_key`, `timeout`), or why it could not be sent (`not_connected`, `bridge removed`, `only ASCII characters can be sent`, …). |
| `text` | The rest of the final line as received. |

```js
const r = await bridge.command('ports');
for (const p of r.items) console.log(p.port, p.label, p.rssi);   // { port, mac, rx_channels, phy_rate, idle_ms, rssi, tx_fail, label, name, model }
```

Only printable ASCII can be sent. A relayed command fails with `err timeout` when the client does not answer within
1.5 s.

## Sending MIDI

`Client` and `ClientRef` have these helpers. Channels are **1-16**. Each returns what `send()` returns.
`timeStamp` (optional) is as in Web MIDI: a time on the `performance.now()` clock at which to send.

| Method | Message |
|---|---|
| `noteOn(ch, note, vel = 100, timeStamp?)` | `9n note vel` |
| `noteOff(ch, note, vel = 0, timeStamp?)` | `8n note vel` |
| `polyAftertouch(ch, note, value, timeStamp?)` | `An note value` |
| `controlChange(ch, number, value, timeStamp?)` | `Bn number value` |
| `programChange(ch, program, timeStamp?)` | `Cn program` |
| `channelAftertouch(ch, value, timeStamp?)` | `Dn value` |
| `pitchBend(ch, value, timeStamp?)` | `En lsb msb`, `value` -8192…8191 (0 = center) |
| `send(data, timeStamp?)` | Any message except SysEx, including channel-less ones (clock `F8`, start `FA`, …). |

The link carries channel messages and channel-less messages, not SysEx. What is sent to a client's port always reaches
that client. Its `rx_channels` setting (see [Config commands](config-commands.md#client-settings)) only chooses which
messages it also receives from the other clients on the same bridge.

## Module exports

```js
import MimuzLink, { Client, ClientRef, Bridge, parseFields, NAME_PREFIX, UID_LEN, NUM_PORTS } from './drivers/mimuz-link.mjs';
```

| Export | Description |
|---|---|
| `default` | `MimuzLink` |
| `Client`, `ClientRef`, `Bridge` | The classes above (for `instanceof`; they are created by the link). |
| `parseFields(text)` | `'a=1 b=x'` → `{ a: 1, b: 'x' }`, as used for command results. |
| `NAME_PREFIX` | `{ bridge: 'mi:muz:bridgeESPS3-', client: 'mi:muz:conESP32C6-' }` |
| `UID_LEN` | `11`, the length of the unique ID in device names. |
| `NUM_PORTS` | `8`, ports per bridge. |

## Node.js

The same driver runs in Node.js 20 or later on [@julusian/midi](https://www.npmjs.com/package/@julusian/midi) (RtMidi).
`init()` loads `node-midi-access.mjs`, which gives the driver the part of Web MIDI it uses. Pass the module so that it
is found in the application's `node_modules`, and close the link at the end: open MIDI ports keep the process running.

```js
import MimuzLink from './drivers/mimuz-link.mjs';

const link = await new MimuzLink({ nodeMidi: await import('@julusian/midi') }).init();
// ... the same as in the browser ...
link.close();
```

- Ports coming and going are found by comparing the port names every 500 ms (RtMidi does not report them). A bridge
  that restarts within that time may be missed.
- The ports are listed by two RtMidi objects kept for the whole run, and each open port is closed and destroyed when
  it goes away (a new RtMidi object per scan would leak memory and MIDI clients).
- Only the ports of bridges and clients are opened; other MIDI devices are left alone.
- `send(data, timeStamp)` waits for a future time stamp with a timer (RtMidi has no scheduling). Received messages are
  stamped with `performance.now()`.
- Tested on macOS only.
- Example: `examples/node/monitor.mjs` (`cd examples/node && npm install && node monitor.mjs --ping N1`).

## Writing a device driver

A device driver wraps a `ClientRef` for one model and turns MIDI into what the device means. A sketch:

```js
// drivers/knob4.mjs
export default class Knob4 extends EventTarget {
  constructor(link, sel = {}) {
    super();
    this.ref = link.client({ ...sel, model: 'knob4' });
    this.ref.addEventListener('midi', (e) => {
      const [st, num, val] = e.detail.data;
      if ((st & 0xF0) === 0xB0 && num >= 20 && num < 24) {
        this.dispatchEvent(new CustomEvent('knob', { detail: { index: num - 20, value: val } }));
      }
    });
  }
  get connected() { return this.ref.connected; }
  setLed(index, on) { return this.ref.noteOn(1, 36 + index, on ? 127 : 0); }
  release() { this.ref.release(); }
}
```

- Pick clients by `model` (and, when the user has several of the same model, by `label` or `name`).
- Keep the `ClientRef`, not the `Client` or the port: it follows reconnections and port changes.
- Use `command()` for settings that live on the device; they are relayed over the radio and take a few ms to 1.5 s.
