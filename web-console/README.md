# mimuz-link web console

A maintenance page for mimuz-link bridges and clients attached to the PC over USB. Several devices (for example two
bridges and a client) can be listed and used at the same time.

- Finds the bridges on USB-MIDI by itself, connects to them and shows what each one is (`info`). Clients attached
  over USB (and, when needed, a bridge's serial port) are added with **Load device**.
- Bridge: connected and registered clients (model, signal strength, undelivered count; the list follows the clients as
  they connect and leave), settings and actions of a client over
  the radio (`port <n> …`), radio channel survey and channel change, settings, diagnostics (statistics, trace, late
  radio sends, radio restarts).
- Client attached over USB: settings and actions (restart, BLE-MIDI, unpair, reset).
- Console: every line to and from the device, and free config commands (the `@id` is added automatically).
- API reference: the Markdown files in `docs/` (web driver, Arduino library, config commands and SysEx), shown as
  HTML with [markdown-it](https://github.com/markdown-it/markdown-it) (loaded from jsDelivr, a fixed version; this page
  needs the internet). Links between the pages stay in the console; links to other files of the repository open on
  GitHub.

The page talks to the devices itself: to bridges over **USB-MIDI** through the web driver
`www/drivers/mimuz-link.mjs` (Web MIDI with SysEx: the config commands go as SysEx, see `docs/config-commands.md` and
`docs/js-driver.md`; applications use the same driver), and to serial ports through **Web Serial** (Chrome or Edge). Setting up
the system goes through the bridges over USB-MIDI, including the clients on the radio; Web Serial is for a client set
up on its own over USB, for a bridge's log, and for the firmware uploader. The server only hosts the files in `www/`, and the API reference in `../docs/`.
Web MIDI with SysEx and Web Serial need a secure context, and `http://localhost` is one, so the server listens on the
loopback address.

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

Open the page and allow MIDI (the browser asks once, because of SysEx): the bridges attached to the PC appear in the
list and connect by themselves, and they come and go as they are plugged in and out. For a client attached over USB,
press **Load device (serial)** and choose its serial port. The page keeps no history of serial ports: ports allowed in
an earlier visit are forgotten when the page opens, and a device that is unplugged leaves the list.

A bridge on USB-MIDI has no device log in the Console tab, and its test firmware's one-character commands (the
statistics in Diagnostics) need its serial port: load it with **Load device (serial)** for those.

## Firmware uploader

The **Firmware uploader** page writes the latest bridge and client firmware built from this repository, with
[ESP Web Tools](https://esphome.github.io/esp-web-tools/), loaded from unpkg (a fixed version; the firmware uploader
needs the internet).

```sh
npm run build   # web-console/firmware/build.sh: builds them into www/firmware/<bridge|client|basic>/ (kept in git)
```

Only the bootloader, the partition table, `boot_app0` and the application are written, at the offsets
`arduino-cli upload` uses. The settings in NVS (0x9000–0xDFFF: pairing, registrations, label, channel, …) are kept;
the merged image is not used because it covers the whole flash.

- Bridge: disconnect it in the Console page, press **Restart into update mode** and choose its port (opening it at
  1200 baud restarts it into the ROM's download mode), then **Write firmware** and choose the new port
  ("USB JTAG/serial debug unit").
- Client: attach it over USB, then **Write firmware** and choose its port.
- Two client firmwares are offered: the test device (`examples/client/c6_test_device`) and the basic example
  (`examples/client/basic`: one button and one LED).

## Notes

- MIDI ports can be used by several programs at once (the PC tools that use MIDI can run while the page is open). A serial port can be open in one program only. Close it here (Disconnect) before using the PC tools
  (`examples/pc/*.py`) on the same device, and close the tools before connecting here.
- The port is opened with DTR on, as the firmware expects (see `docs/config-commands.md`).
- Commands are sent one at a time per device. A command relayed to a client over the radio is resent by the bridge
  until a reply arrives and fails after 1.5 s.
- Settings marked ● are saved by the device; ○ last until it restarts (diagnostic).
- Everything is written as ES modules (`app.mjs`, `www/js/*.mjs`, `www/drivers/*.mjs`). `www/drivers/` holds the
  drivers for applications (`mimuz-link.mjs`, and device drivers on top of it); the console uses them too.
