# Config commands and SysEx

Bridges and clients are set up with one-line text commands. The same commands reach a device in three ways:

| Way | How | Used by |
|---|---|---|
| USB serial | the line as it is, ending with a line feed | `examples/pc/mlink_config.py`, the web console's **Load device (serial)** |
| USB-MIDI SysEx, to a bridge | the line inside `F0 7D 4D 4C 03 … F7` ([SysEx](#sysex-over-usb-midi)) | the web console, the [web driver](js-driver.md) |
| Relayed over the radio, to a client | `port <n> <command…>` sent to its bridge | the same |

So a PC that has only the bridge on USB can set up the bridge and every client on it.

## Contents

- [Line format](#line-format)
- [Commands of both](#commands-of-both)
- [Client settings](#client-settings)
- [Client commands](#client-commands)
- [Bridge settings](#bridge-settings)
- [Bridge commands](#bridge-commands)
- [Bridge diagnostics](#bridge-diagnostics)
- [SysEx over USB-MIDI](#sysex-over-usb-midi)

## Line format

```
PC → device : @<id> <command> <args…>
device → PC : @<id> ok <key=value …>      success (the last line of a reply)
              @<id> err <reason>          failure
              @<id> + <key=value …>       an intermediate line of a multi-line reply (an ok line comes last)
```

- `<id>` is any word chosen by the PC (letters and digits); the reply carries the same id, so that replies can be
  matched to requests.
- Lines that do not start with `@` are log output, for people to read.
- Numbers are decimal, or hexadecimal with `0x`.
- Values written with `set` are saved in non-volatile memory at once (they survive a restart), except the diagnostic
  keys marked "not saved".
- A line is up to 180 characters. Only printable ASCII is used.
- Over USB serial, open the port with DTR on: the C6 resets if the port is opened with DTR off, and the S3 does not
  reply unless DTR is on.

```
@1 info
@1 ok name=mi:muz:conESP32C6-j3f5__w9GK0 kind=client model=test fw=0.1.3 transport=espnow connected=1 port=1 radio_channel=7 bridge=68:EE:8F:50:68:58 label=N1
@2 set rx_channels omni
@2 ok rx_channels=0xFFFF
@3 port 2 get label
@3 ok label=N3
```

Common errors: `unknown_command`, `unknown_key`, `bad_value`, `usage:…`, `empty`. Relayed commands (`port <n> …`)
also fail with `port_not_connected`, `busy` (another relay is running), `send_failed` and `timeout` (the client did not
answer within 1.5 s).

## Commands of both

| Command | Reply |
|---|---|
| `info` | What the device is. See below. |
| `get` | All settings in one line. |
| `get <key>` | One setting: `ok <key>=<value>`. |
| `set <key> <value>` | Changes a setting: `ok <key>=<new value>`. |
| `radio_restart [wifi\|stall]` | Diagnostic: restarts ESP-NOW (`wifi`: Wi-Fi as well; `stall`: simulates a stuck radio driver, which the device should detect and recover from by itself). The bridge also takes `driver` (reinitialize the Wi-Fi driver) and `stall hold` (a stuck driver that survives restarts; recovery escalates up to a device restart). |

`info` of a client:

| Field | Meaning |
|---|---|
| `name` | Device name, `mi:muz:conESP32C6-<unique ID>` |
| `kind` | `client` |
| `model` | Model, set by its firmware (empty if not given) |
| `fw` | Firmware (library) version |
| `transport` | `espnow` or `ble` |
| `connected` | 1 = connected to the bridge or a BLE host |
| `port` | Bridge port 1-8 (0 = none) |
| `radio_channel` | Radio channel |
| `bridge` | MAC address of its bridge |
| `label` | Label |

`info` of a bridge:

| Field | Meaning |
|---|---|
| `name` | Device name, `mi:muz:bridgeESPS3-<unique ID>` |
| `kind` | `bridge` |
| `fw` | Firmware version |
| `radio_channel` | Radio channel in use |
| `ports_used`, `ports` | Ports with a client, ports in all (8) |
| `registered` | Registered clients |
| `temp_c` | Chip temperature in °C (the S3's internal sensor: the die, not the air; about 65-77 °C under load on the test bridge). Left out when the sensor cannot be read. |

## Client settings

`get <key>` / `set <key> <value>` on a client (over its serial, or `port <n> …` through its bridge).

| Key | Values | Meaning |
|---|---|---|
| `rx_channels` | 16-bit mask, `omni`, `none` | MIDI channels to receive from the **other clients** on the same bridge (bit 0 = channel 1). What the PC sends to the client's port always reaches it. Channel-less messages from other clients arrive when any bit is set. Default `0x0000`. |
| `noteon_max_age_ms` | 0-2000 | A Note On waiting longer than this to be sent is dropped (0 = never). Default 80. |
| `label` | up to 30 of `A-Z a-z 0-9 - _ .`; `-` clears it | The user's name for this client. Saved on the client and told to the bridge on every connect and change, and whenever the bridge asks; shown by `info` and in the bridge's `ports` list. Not used as the USB or BLE device name. Applications can pick a client by its label (several clients may share one on purpose) or by its device name (unique). |
| `model` | read only | What kind of device this is (e.g. `knob4`), fixed by its sketch (`Config::model`, up to 16 of `A-Z a-z 0-9 - _ .`; empty = not given). Told to the bridge with the label and device name. |
| `phy_rate` | 0-3 | Radio rate: 0 = 1M, 1 = 6M (default), 2 = 24M, 3 = MCS4. |
| `retry_limit` | 0-31 | How many times the radio retries a frame that is not acknowledged (default 0 = the Wi-Fi library's value, 32). `get` also shows the values in use (`short`, `long`) and the library's (`default_short`, `default_long`). Not saved. |
| `send_lock` | 0/1 | Diagnostic: 1 = hand packets to the radio under the Wi-Fi driver's global lock (default). Not saved. |

## Client commands

| Command | Effect |
|---|---|
| `reset` | Settings back to their defaults (the registration is kept). Replies with the settings. |
| `unpair` | Erases the registration with the bridge and restarts. To connect again, the client has to be paired again, right next to the bridge. |
| `restart [auto\|ble]` | Restarts. `ble` = stay on BLE-MIDI. |

## Bridge settings

| Key | Values | Meaning |
|---|---|---|
| `channel_min`, `channel_max` | 1-13 | Range of radio channels the survey may choose (takes effect at the next survey: a normal start or `resurvey`). Default 1-13. `channel_min` may not be above `channel_max` (so to move the range up, set `channel_max` first). Set both to the same value to fix the channel. |
| `noteon_max_age_ms` | 0-2000 | As on the client, for the PC → client direction. Default 80. |
| `retry_limit` | 0-31 | As on the client; bridge default 8. The radio sends the frames handed to it in order, so while it retries a frame to a far client, the frames to the other clients wait behind it. 8 shortens those waits at the cost of more lost packets to such a client (Note Offs are resent). Not saved. |
| `tx_parallel` | 0/1 | 1 = hand packets to the radio in parallel, one per client (default; a hard-to-reach client holds up the others less); 0 = one at a time overall. Not saved. |
| `send_lock` | 0/1 | As on the client. Not saved. |
| `tx_fail_max` | 1-50 | Diagnostic: consecutive failures before an unresponsive client is skipped (default 3). Not saved. |
| `tx_probe` | 0/1 | Diagnostic: probe sends while a client is skipped (1 = on; every 250 ms, doubling up to 1 s). Not saved. |
| `tx_impl` | 0/1 | Diagnostic: 0 = single-queue send task, 1 = per-client send task (default). Not saved. |
| `trace_status` | status byte, 0 = off | Diagnostic, see `trace` below. Not saved. |
| `slow_tx_ms` | ms, 0 = off | Diagnostic, see `slowtx` below (default 10). Not saved. |

## Bridge commands

| Command | Reply |
|---|---|
| `ports` | One `+` line per connected client, then `ok count=<n>`. Fields: `port` (1-8), `mac`, `rx_channels`, `phy_rate`, `idle_ms` (since it was last heard), `rssi` (dBm), `tx_fail` (packets not delivered even after the radio's retries, since it last connected), `label`, `name`, `model` (the last two are told by an ESP-NOW client to its bridge only). |
| `registry` | One `+` line per registered client: `mac`, `port` (0 = not connected), and the last `label`, `name`, `model` it told (kept with the registration). Then `ok count=<n>`. |
| `unregister <MAC\|all>` | Removes a registration (a connected client is dropped). `ok registered=<n>`. |
| `label_request [<port>\|all]` | Asks clients to tell their label again (the bridge also asks by itself when a label has not arrived 2 s after a connect, up to 5 times). `ok asked=<n>`. |
| `survey` | The last radio channel survey: one `+` line per channel (`ch`, `busy` and `peak` in %, `score`; lower = freer), then `ok radio_channel=<n>`. |
| `resurvey` | Surveys the channels again and moves to the best one (about 12 s, no traffic meanwhile). `ok radio_channel=<n>`. |
| `switch_channel <n>` | Moves to channel 1-13; the clients are told and follow. Not saved (a normal start surveys again). `ok radio_channel=<n>`. |
| `port <n> <command…>` | Relays a command over the radio to the client on port n and returns its reply, e.g. `@5 port 2 set rx_channels omni` → `@5 ok rx_channels=0xFFFF`. Resent every 150 ms until a reply arrives; `err timeout` after 1.5 s. |

## Bridge diagnostics

| Command | Reply |
|---|---|
| `trace [clear]` | With `trace_status` set to a status byte (e.g. 175 = Poly Aftertouch ch 16), a traced message that the client sends back unchanged is timed from its arrival over USB until the reply has been written to USB (`total_us`), and until the reply arrived over the radio (`radio_us`). `trace` lists the 16 slowest with their port, data bytes (`key` = data1 + data2 × 128) and USB arrival time (`in_us`), slowest first, plus p50 / p99 / max. The tables (about 10 KB) exist only while tracing is on. |
| `slowtx [clear]` | Radio sends whose result came `slow_tx_ms` or more after they were handed to the radio, the last 64, oldest first: port, `sent_us`, `air_us`, `ok`, `resend` (Note Off resend), `timeout` (no result at all), `len`, `rssi`, `nomem` (running count of sends the radio refused). |

## SysEx over USB-MIDI

A PC application can ask the bridge, over USB-MIDI, which client is on a port and which bridge it is talking to, and
can send it any config command (including those relayed to its clients). The bridge answers on the port the inquiry
was sent to. These messages are not sent over the radio; other SysEx messages are ignored.

```
inquiry : F0 7D 4D 4C <cmd> F7
reply   : F0 7D 4D 4C <cmd + 0x40> <format version> <data …> F7
```

`7D` is the manufacturer ID for non-commercial use; `4D 4C` ("ML") marks mimuz-link. Strings are a length byte
followed by ASCII characters (no terminator). The format version is 2 from firmware 0.1.3 (version 1, firmware 0.1.2:
no model in the port reply, and port changes are not told).

| cmd | Inquiry | Reply data |
|---|---|---|
| `01` port | `F0 7D 4D 4C 01 F7` on port n | `41 02 <port 1-8> <state> <client device name> <client label> <client model>`. state: 0 = no client (all strings empty), 1 = connected, name and label not told yet, 2 = connected, name and label known. |
| `02` bridge | `F0 7D 4D 4C 02 F7` on any port | `42 02 <bridge device name> <firmware version> <radio channel> <ports in use> <ports>` |
| `03` config command | `F0 7D 4D 4C 03 <command line> F7` on any port | One message per reply line: `43 <more> <text>` (no line end, no format version). The command line and the replies are the same text as on the serial, e.g. `@7 port 2 get label` → `@7 ok label=N1`; the leading `@` may be left out. more = 1: the line was longer than 240 characters and continues in the next message. |

**Port changes told by the bridge.** Once the PC has sent any of these inquiries since the bridge started, the bridge
also sends the port reply (`41`) by itself, on the port concerned, whenever a client connects to it, leaves it or
tells its label. MIDI programs that never ask get no SysEx. If nothing reads the port, these messages are dropped
rather than holding up MIDI.

Example: `F0 7D 4D 4C 41 02 01 02 1D "mi:muz:conESP32C6-j3f5__w9GK0" 02 "N1" 04 "test" F7` = port 1, client
`mi:muz:conESP32C6-j3f5__w9GK0` with label `N1` and model `test`.

- A bridge does not answer for about 12 s after it starts (radio channel survey).
- Which MIDI port is which bridge port should be learned from the port replies, not from the port names, which differ
  between operating systems.
- Web pages and Node.js programs do not need to handle these messages themselves: the [web driver](js-driver.md)
  does.
