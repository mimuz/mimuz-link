# Tutorial 1: Quick start

From two bare boards to MIDI messages on the PC, without writing code: write the firmware from the browser, pair the
client with the bridge, and check the MIDI messages between the client and the PC.

What you get at the end: a client (XIAO ESP32-C6) that sends a Note On message to the PC through the bridge
(XIAO ESP32-S3) when its BOOT button is pressed, and lights its LED when the PC sends that note back. The client sends
and receives MIDI messages only; it makes no sound itself.

## Contents

- [What you need](#what-you-need)
- [Step 1: Start the web site](#step-1-start-the-web-site)
- [Step 2: Write the bridge firmware](#step-2-write-the-bridge-firmware)
- [Step 3: Write the client firmware](#step-3-write-the-client-firmware)
- [Step 4: Pair the client with the bridge](#step-4-pair-the-client-with-the-bridge)
- [Step 5: Send and receive MIDI](#step-5-send-and-receive-midi)
- [Step 6: Give the client a label](#step-6-give-the-client-a-label)
- [Step 7: Without the bridge (BLE-MIDI)](#step-7-without-the-bridge-ble-midi)
- [Next](#next)

## What you need

- A **XIAO ESP32-S3** for the bridge, with its antenna attached.
- A **XIAO ESP32-C6** for the client. No wiring: the example uses the board's BOOT button and user LED.
- Two USB-C cables that carry data (not charge-only cables).
- A PC with **Chrome or Edge** (the site uses Web Serial and Web MIDI), [git](https://git-scm.com/) and
  [Node.js](https://nodejs.org/) 18 or later.
- MIDI software to see and send MIDI messages: a MIDI monitor, a DAW, or the Node.js monitor of this repository
  (`examples/node/monitor.mjs`, Node.js 20 or later). To hear the button, a DAW (or another program) with a software
  instrument (see step 5).

## Step 1: Start the web site

The web site of this repository holds this tutorial, the console and the firmware uploader. It runs on your PC:

```sh
git clone https://github.com/mimuz/mimuz-link.git
cd mimuz-link
npm start
```

Open http://localhost:8080 in Chrome or Edge. Web Serial and Web MIDI work only on `https://` or `http://localhost`,
which is why the server runs on your PC.

## Step 2: Write the bridge firmware

1. Plug the XIAO ESP32-S3 into the PC.
2. Open the **Firmware uploader** page. In the **Bridge** box, press **Restart into update mode** and choose the
   board's serial port. The board restarts into its download mode and shows up again as a new port.
   - A board that does not run an Arduino sketch yet (or does not restart) can be put into download mode by hand:
     hold its **BOOT** button while plugging it in.
3. Press **Write firmware** and choose the new port (USB JTAG/serial debug unit).
4. The bridge restarts with the new firmware. It first surveys the radio channels for a free one, so it is ready after
   about 15 s.

The PC now has a USB-MIDI device `mi:muz:bridgeESPS3-<unique ID>` with 8 ports (`Port 1` … `Port 8`).

## Step 3: Write the client firmware

1. Plug the XIAO ESP32-C6 into the PC.
2. On the **Firmware uploader** page, in the **Client (basic example)** box, press **Write firmware** and choose the
   board's port (USB JTAG/serial debug unit).
3. A brand-new XIAO ESP32-C6 does not start the firmware after its first upload until it is **unplugged and plugged in
   again**. Do that once.

The basic example (`examples/client/basic`) sends Note On / Note Off 60 (middle C) on channel 1 when the BOOT button is
pressed and released, and turns its LED on and off when the PC sends Note On / Note Off.

## Step 4: Pair the client with the bridge

A client that has never been paired pairs only with a bridge **right next to it** (signal of -55 dBm or stronger), so
that it does not pair with someone else's bridge by chance.

1. Keep the bridge plugged into the PC and running (step 2).
2. Put the client right next to the bridge (a few centimeters) and power it: plug it in again, or plug it into another
   USB power source.
3. Open the **Console** page and allow MIDI when the browser asks (the console sends its commands to the bridge as
   SysEx). The bridge appears in the device list by itself.
4. Choose the bridge and open its **Clients** tab. The client is on **Port 1**, with the model `basic`.

The bridge remembers the client. From now on the client connects to this bridge from anywhere in radio range, and
gets the lowest free port when it connects. Ports are not fixed per device: they follow the order in which the clients
connect (applications find clients by name, label or model instead, see [Tutorial 2](tutorial-new-client.md)).

To pair the client with another bridge, erase its registration (**Unpair** in the client's window of the Clients tab,
or the config command `unpair`), then pair it again next to the other bridge.

## Step 5: Send and receive MIDI

The basic example only sends and receives MIDI messages; it makes no sound itself. To hear the button, route the input
Port 1 to a software instrument in a DAW.

1. In your MIDI software, open the input **`mi:muz:bridgeESPS3-<unique ID>` Port 1** (on some systems the port names
   differ slightly; the port number is what counts).
2. Press the client's BOOT button: Note On 60 arrives on channel 1. Release it: Note Off.
3. Send Note On (any velocity above 0) on Port 1 to the client: its LED lights. Note Off turns it off.

With the Node.js monitor of this repository:

```sh
cd examples/node
npm install
node monitor.mjs
```

It lists the bridges and clients as they come and go, and prints each message a client sends:

```
12:00:01 bridges: mi:muz:bridgeESPS3-xxxxxxxxxxx (ch 7, fw 0.1.3)
12:00:01   port 1: (no label) model=basic mi:muz:conESP32C6-yyyyyyyyyyy
12:00:05 mi:muz:conESP32C6-yyyyyyyyyyy: 90 3c 64
12:00:05 mi:muz:conESP32C6-yyyyyyyyyyy: 80 3c 00
```

## Step 6: Give the client a label

A label is a name you give a client (for example `left` or `drums`). It is saved on the client, shown in the console,
and lets applications pick this client among others of the same model.

1. In the **Console** page, open the bridge's **Clients** tab and click the client's row.
2. Under **Settings**, enter a label in **Label** (up to 30 of `A-Z a-z 0-9 - _ .`) and press **Set**.

The setting is sent to the client over the radio, through the bridge (`port 1 set label left`). Every setting and
command is listed in [Config commands and SysEx](config-commands.md).

## Step 7: Without the bridge (BLE-MIDI)

When the client cannot reach its bridge, it becomes a BLE-MIDI device named `mi:muz:conESP32C6-<unique ID>`, which
the PC (or a tablet) can connect to like any BLE-MIDI device. The same buttons and notes work over BLE.

- A paired client waiting on BLE looks for its bridge again every 20 s, and goes back to ESP-NOW when it finds it.
- Start the bridge first: it needs about 15 s after power-up before clients can connect.
- On the first BLE connection, macOS may wait at "Allow accessory to connect".

## Next

- [Tutorial 2: A new client device](tutorial-new-client.md): your own controller, step by step.
- Web pages use the basic client through its device driver `web/www/drivers/basic-device.mjs` (the button as an
  event, `setLed()`); how such drivers work is in Tutorial 2, step 8.
- [API reference](README.md): the web driver, the Arduino library, the config commands.
