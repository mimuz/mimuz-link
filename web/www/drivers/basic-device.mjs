/*

 basic-device.mjs mimuz-link device driver for the basic client example (examples/client/basic, model "basic")

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/24 : Start Writing (made from device-template.mjs)

*/
// The driver of the basic example: one button and one LED on a bare XIAO ESP32-C6.
//   events : button ({ pressed }) when the BOOT button is pressed or released, and those of MimuzDevice (connect,
//            disconnect, change, midi)
//   methods: setLed(on), and those of MimuzDevice
//
//   import BasicDevice from './drivers/basic-device.mjs';
//   const basic = new BasicDevice();             // or new BasicDevice({ label: 'left' }) when there are several
//   basic.addEventListener('button', (e) => basic.setLed(e.detail.pressed));
//   await basic.open();                          // asks for MIDI; rejects with the reason when it is not available
//
// What the sketch sends and receives (basic.ino): the button sends Note On 60 (velocity 100) / Note Off 60 on
// channel 1; any Note On (velocity above 0) turns the LED on, any Note Off turns it off.
// New drivers are made by copying device-template.mjs; this file shows one made that way.
import { MimuzDevice } from './mimuz-link.mjs';

const CHANNEL = 1;  // the MIDI channel of the button (1-16)
const NOTE = 60;    // middle C

export default class BasicDevice extends MimuzDevice {
  static model = 'basic';

  // Every message from the device: the button's note becomes the button event
  onMidi(data) {
    const [status, d1, d2] = data;
    if (status >= 0xF0 || (status & 0x0F) !== CHANNEL - 1) return;  // only channel messages on CHANNEL
    const type = status & 0xF0;
    if ((type === 0x90 || type === 0x80) && d1 === NOTE) this.emit('button', { pressed: type === 0x90 && d2 > 0 });
  }

  // The LED: Note On / Note Off of the button's note
  setLed(on) {
    return on ? this.noteOn(CHANNEL, NOTE, 127) : this.noteOff(CHANNEL, NOTE);
  }
}
