/*

 device-template.mjs mimuz-link device driver template: copy it to <model>.mjs to make the driver of a new device

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/24 : Start Writing

*/
// To make the driver of a new kind of client:
//   1. Copy this file to drivers/<model>.mjs (for example knob1.mjs) and rename the class (Template -> Knob1).
//   2. Set "model" to the model in the device's sketch (cfg.model = "knob1"), and CHANNEL to its MIDI channel.
//   3. In onMidi(), turn the messages the device sends into events of the driver (this.emit).
//   4. Add methods for what the application sends to the device (LEDs, sounds, ...).
//   5. Replace this comment with what the driver's events and methods are.
// The application then imports only the driver:
//   import Knob1 from './drivers/knob1.mjs';
//   const dev = new Knob1();                  // or new Knob1({ label: 'left' }) when there are several
//   dev.addEventListener('control', (e) => console.log(e.detail.number, e.detail.value));
//   await dev.open();                         // asks for MIDI; rejects with the reason when it is not available
// The base class (MimuzDevice, in mimuz-link.mjs) finds the device and follows it when it reconnects or changes port,
// shares the MIDI access with the other drivers of the page, and gives connected, label, name, the sending methods
// (noteOn, noteOff, controlChange, ...; channels 1-16), command(), setLabel(), ready(), close() and the events
// connect, disconnect, change and midi. See docs/js-driver.md, "Device drivers (MimuzDevice)".
import { MimuzDevice } from './mimuz-link.mjs';

const CHANNEL = 1;  // the MIDI channel the device uses (1-16)

export default class Template extends MimuzDevice {
  static model = 'template';  // the model of the device (cfg.model in its sketch)

  // Every message from the device (data: [status, data 1, data 2]). The events below are examples: replace them with
  // what the device's buttons, knobs, ... are.
  onMidi(data) {
    const [status, d1, d2] = data;
    if (status >= 0xF0 || (status & 0x0F) !== CHANNEL - 1) return;  // only channel messages on CHANNEL
    const type = status & 0xF0;
    if (type === 0x90 && d2 > 0) this.emit('note', { note: d1, on: true, velocity: d2 });
    else if (type === 0x80 || type === 0x90) this.emit('note', { note: d1, on: false, velocity: 0 });
    else if (type === 0xB0) this.emit('control', { number: d1, value: d2 });
  }

  // What the application sends to the device (examples)
  setNote(note, on, velocity = 127) {
    return on ? this.noteOn(CHANNEL, note, velocity) : this.noteOff(CHANNEL, note);
  }

  setControl(number, value) {
    return this.controlChange(CHANNEL, number, value);
  }
}
