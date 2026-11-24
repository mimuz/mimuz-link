# mimuz-link

A wireless MIDI connection environment that supports ESP-NOW and BLE-MIDI.

Clients (XIAO ESP32-C6, used as controllers or sound modules) connect over ESP-NOW to a bridge (XIAO ESP32-S3, a USB-MIDI dongle plugged into the PC). The PC sees the bridge as a USB-MIDI device with 8 ports. When no bridge is found, a client connects to the PC directly as a BLE-MIDI device.

```
PC ── USB-MIDI (8 ports) ── bridge (XIAO ESP32-S3) ~~ ESP-NOW ~~ clients (XIAO ESP32-C6), up to 8
PC ~~ BLE-MIDI ~~ client (when no bridge is found)
```

- Low latency: the ESP-NOW hop is under 1 ms one way (median). The round trip PC → bridge → client → bridge → PC has a median of about 1.6 ms.
- Multiple devices: up to 8 clients can be connected to one bridge at the same time. Messages can also be delivered between clients, per MIDI channel.
- Delivery: Note Off messages are resent if they are not delivered. Note On messages that have waited too long are dropped.
- Configuration: one-line commands over USB serial, or as SysEx through the bridge. Settings are stored in non-volatile memory.

> **Note: esp32 Arduino core 3.3.8 only.** mimuz-link uses parts of the esp32 core and ESP-IDF that are not public API, so it works only with the version it was verified on, **esp32 core 3.3.8**. The build stops with an error on any other core. Install it with `arduino-cli core install esp32:esp32@3.3.8` (in the Arduino IDE: Boards Manager → esp32 by Espressif Systems → version 3.3.8).

## Where to go next

| Page | What is there |
|---|---|
| [Tutorial 1: Quick start](tutorial-quick-start.md) | Write the firmware from the browser, pair a client with a bridge, and play MIDI. |
| [Tutorial 2: A new client device](tutorial-new-client.md) | Make a device of your own, step by step: the sketch, building, pairing, and a driver for web pages. |
| [API reference](README.md) | The [web driver](js-driver.md), the [Arduino library](arduino-library.md), and the [config commands and SysEx](config-commands.md). |
| [Console](../web/README.md) | Set up the bridges and their clients in the browser: clients, labels, radio channel, diagnostics. |
| [Firmware uploader](../web/README.md#firmware-uploader) | Write the bridge and client firmware from the browser. |
| [Repository README](../README.md) | Layout of the repository, building with arduino-cli, the PC tools, versions and releases. |
