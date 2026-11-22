# mimuz-link web console

A maintenance page for mimuz-link bridges and clients attached to the PC over USB. Several devices (for example two
bridges and a client) can be listed and used at the same time.

- Lists the serial ports added in this visit, connects to them and shows what each device is (`info`).
- Bridge: connected and registered clients (signal strength, undelivered count), settings and actions of a client over
  the radio (`port <n> …`), radio channel survey and channel change, settings, diagnostics (statistics, trace, late
  radio sends, radio restarts).
- Client attached over USB: settings and actions (restart, BLE-MIDI, unpair, reset).
- Console: every line to and from the device, and free config commands (the `@id` is added automatically).

The page talks to the devices itself through **Web Serial** (Chrome or Edge). The server only hosts the files in
`www/`. Web Serial needs a secure context, and `http://localhost` is one, so the server listens on the loopback address.

## Run

From the top of the repository:

```sh
npm start            # installs express when needed, then serves http://localhost:8080 (PORT=9000 npm start for another port)
npm run server       # the same, kept running by pm2 (name mimuz-link-console)
npm run build        # builds the bridge and client firmware for the firmware uploader (arduino-cli)
```

(`cd web-console && npm start` works as well.) Everything the site needs is in git (the page and the firmware it
writes), so a host only needs `git pull` and `npm start` (or `npm run server`). The server listens on 127.0.0.1; to publish the site, put an https reverse proxy in front of it
(Web Serial needs https or localhost).

Open the page, press **Load device** and choose the serial port of a bridge or a client (the browser's chooser shows
the USB product name, which for a bridge is its device name). The page keeps no device history: ports allowed in an
earlier visit are forgotten when the page opens, and a device that is unplugged leaves the list, so add the devices
again in each visit.

## Firmware uploader

The **Firmware uploader** page writes the latest bridge and client firmware built from this repository, with
[ESP Web Tools](https://esphome.github.io/esp-web-tools/), loaded from unpkg (a fixed version; the firmware uploader
needs the internet).

```sh
npm run build   # web-console/firmware/build.sh: builds both into www/firmware/<bridge|client>/ (kept in git)
```

Only the bootloader, the partition table, `boot_app0` and the application are written, at the offsets
`arduino-cli upload` uses. The settings in NVS (0x9000–0xDFFF: pairing, registrations, label, channel, …) are kept;
the merged image is not used because it covers the whole flash.

- Bridge: disconnect it in the Console page, press **Restart into update mode** and choose its port (opening it at
  1200 baud restarts it into the ROM's download mode), then **Write firmware** and choose the new port
  ("USB JTAG/serial debug unit").
- Client: attach it over USB, then **Write firmware** and choose its port.
- The client firmware is the test device (`examples/client/c6_test_device`).

## Notes

- A serial port can be open in one program only. Close it here (Disconnect) before using the PC tools
  (`examples/pc/*.py`) on the same device, and close the tools before connecting here.
- The port is opened with DTR on, as the firmware expects (see the README, "Config commands").
- Commands are sent one at a time per device. A command relayed to a client over the radio is resent by the bridge
  until a reply arrives and fails after 1.5 s.
- Settings marked ● are saved by the device; ○ last until it restarts (diagnostic).
- Everything is written as ES modules (`app.mjs`, `www/js/*.mjs`).
