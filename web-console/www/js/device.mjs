/*

 device.mjs mimuz-link web console: one device on Web Serial (bridge or client)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/22 : Start Writing
 - 2026/11/22 : USB key of a port (to remember which device it was)
 - 2026/11/22 : Remove the USB key again (names are no longer remembered per USB ID)
 - 2026/11/23 : Keep asking "info" while the device is still starting
 - 2026/11/23 : Bridges on USB-MIDI (Web MIDI): the config commands go as SysEx (MidiBridge)

*/
// Config command protocol of mimuz-link over USB serial (see the README, "Config commands"):
//   PC -> device : "@<id> <command> <args...>" (one line)
//   device -> PC : "@<id> ok <key=value ...>" / "@<id> err <reason>" / "@<id> + <key=value ...>" (a line of a
//                  multi-line reply, before the ok). Lines that do not start with "@" are log output.
// The port is opened with DTR on: the bridge does not reply unless DTR is on, and the client resets when the port is
// opened with DTR off.
// Commands are sent one at a time per device: the bridge relays only one command to a client at a time, and the
// replies are easier to tell apart.
//
// A bridge can also be reached over USB-MIDI (MidiBridge, Web MIDI with SysEx): the same command lines go in a SysEx
// message and each reply line comes back in one (see the README, "SysEx inquiries"):
//   PC -> bridge : F0 7D 4D 4C 03 <"@<id> <command> <args...>"> F7
//   bridge -> PC : F0 7D 4D 4C 43 <more> <reply line> F7     (more = 1: the line continues in the next message)

const BAUD = 115200;
const DEFAULT_TIMEOUT_MS = 3000;
// A bridge does not reply while it surveys the channels at start (about 12 s, e.g. right after a firmware update), so
// "info" is asked again until this long after the port was opened.
const START_WAIT_MS = 25000;
const START_RETRY_TIMEOUT_MS = 2000;

// Keys whose values are names, never numbers (a label "007" must stay "007")
const TEXT_KEYS = new Set(['label', 'name', 'mac', 'bridge', 'fw']);

// "a=1 b=x" -> { a: 1, b: 'x' } (numbers become numbers, everything else stays a string)
export function parseFields(text) {
  const out = {};
  for (const tok of text.trim().split(/\s+/)) {
    const i = tok.indexOf('=');
    if (i <= 0) continue;
    const k = tok.slice(0, i);
    const v = tok.slice(i + 1);
    out[k] = !TEXT_KEYS.has(k) && /^-?\d+(\.\d+)?$/.test(v) ? Number(v) : v;
  }
  return out;
}

export class Device extends EventTarget {
  constructor(port) {
    super();
    this.port = port;
    this.transport = 'serial';
    const info = port?.getInfo() ?? {};
    this.usb = { vid: info.usbVendorId, pid: info.usbProductId, serial: info.usbSerialNumber };
    this.info = null;     // reply of "info" (name, kind, fw, ...)
    this.open = false;
    this.starting = false;  // opened, but "info" has not been answered yet
    this.attached = port?.connected !== false;  // plugged in (Web Serial lists ports the page was allowed to use, plugged in or not)
    this.busy = false;
    this.nextId = 1;
    this.queue = [];
    this.pending = null;  // { id, resolve, items, timer }
    this.rawCollector = null;
    this.reader = null;
    this.writer = null;
  }

  get kind() { return this.info?.kind ?? 'unknown'; }
  get name() { return this.info?.name ?? `USB ${hex4(this.usb.vid)}:${hex4(this.usb.pid)}`; }

  async connect() {
    if (this.open) return;
    await this.port.open({ baudRate: BAUD });
    await this.port.setSignals({ dataTerminalReady: true, requestToSend: false });
    this.open = true;
    const enc = new TextEncoderStream();
    this.writeClosed = enc.readable.pipeTo(this.port.writable).catch(() => {});
    this.writer = enc.writable.getWriter();
    this.readLoop();
    await this.askInfo();
  }

  // Asks "info" until it is answered or START_WAIT_MS has passed since the device was opened
  async askInfo() {
    this.starting = true;
    this.emit('state');
    const until = Date.now() + START_WAIT_MS;
    let told = false;
    while (this.open) {
      try {
        const r = await this.command('info', { timeout: START_RETRY_TIMEOUT_MS });
        if (r.ok) this.info = r.fields;
        else this.log('sys', `info failed: ${r.error}`);
        break;
      } catch (e) {
        if (!this.open) break;
        if (Date.now() >= until) {
          this.log('sys', `info failed: ${e.message} (no reply for ${START_WAIT_MS / 1000} s; use Refresh in Overview)`);
          break;
        }
        if (!told) this.log('sys', 'no reply yet; the device may still be starting (a bridge surveys the channels for about 12 s)');
        told = true;
      }
    }
    this.starting = false;
    this.emit('state');
  }

  async disconnect() {
    if (!this.open) return;
    this.open = false;
    this.starting = false;
    try { await this.reader?.cancel(); } catch { /* already closed */ }
    try { await this.writer?.close(); } catch { /* already closed */ }
    try { await this.writeClosed; } catch { /* ignore */ }
    try { await this.port.close(); } catch { /* ignore */ }
    this.failAll('disconnected');
    this.emit('state');
  }

  // The device was unplugged (navigator.serial "disconnect")
  gone() {
    this.open = false;
    this.starting = false;  // opened, but "info" has not been answered yet
    this.attached = false;
    this.failAll('unplugged');
    this.emit('state');
  }

  async readLoop() {
    const dec = new TextDecoderStream();
    const closed = this.port.readable.pipeTo(dec.writable).catch(() => {});
    this.reader = dec.readable.getReader();
    let buf = '';
    try {
      for (;;) {
        const { value, done } = await this.reader.read();
        if (done) break;
        buf += value;
        let i;
        while ((i = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, i).replace(/\r$/, '');
          buf = buf.slice(i + 1);
          if (line) this.onLine(line);
        }
      }
    } catch (e) {
      this.log('sys', `read stopped: ${e.message}`);
    } finally {
      this.reader.releaseLock();
      await closed;
      if (this.open) this.gone();
    }
  }

  onLine(line) {
    this.log('in', line);
    if (this.rawCollector) this.rawCollector.push(line);
    const m = /^@(\d+)\s+(ok|err|\+)\s?(.*)$/.exec(line);
    if (!m || !this.pending || Number(m[1]) !== this.pending.id) return;
    const p = this.pending;
    if (m[2] === '+') {
      p.items.push(parseFields(m[3]));
      return;
    }
    clearTimeout(p.timer);
    this.pending = null;
    p.resolve(m[2] === 'ok'
      ? { ok: true, fields: parseFields(m[3]), items: p.items, text: m[3] }
      : { ok: false, error: m[3].trim(), items: p.items, text: m[3] });
    this.pump();
  }

  // Sends a config command (without "@id"). Resolves { ok, fields, items, error, text }; rejects on timeout.
  command(text, { timeout = DEFAULT_TIMEOUT_MS } = {}) {
    return new Promise((resolve, reject) => {
      this.queue.push({ text, timeout, resolve, reject });
      this.pump();
    });
  }

  pump() {
    if (this.pending || !this.queue.length) {
      this.setBusy(!!this.pending);
      return;
    }
    if (!this.open) {
      this.failAll('not connected');
      return;
    }
    const c = this.queue.shift();
    const id = this.nextId;
    this.nextId = this.nextId >= 9999 ? 1 : this.nextId + 1;
    this.pending = {
      id, items: [], resolve: c.resolve,
      timer: setTimeout(() => {
        if (this.pending?.id !== id) return;
        this.pending = null;
        c.reject(new Error(`timeout: ${c.text}`));
        this.pump();
      }, c.timeout),
    };
    this.setBusy(true);
    this.write(`@${id} ${c.text}\n`).catch((e) => {
      clearTimeout(this.pending?.timer);
      this.pending = null;
      c.reject(e);
      this.pump();
    });
  }

  // Sends a one-character serial command of the test firmware (e.g. "s" = statistics) and returns the log lines that
  // arrive within collectMs. Only for read-only keys: some keys change state (the bridge's "x" erases all registrations).
  async raw(key, collectMs = 1500) {
    if (this.pending || this.queue.length) throw new Error('a command is still running');
    this.rawCollector = [];
    await this.write(key);
    await new Promise((r) => setTimeout(r, collectMs));
    const lines = this.rawCollector;
    this.rawCollector = null;
    return lines;
  }

  async write(text) {
    if (!this.writer) throw new Error('not connected');
    this.log('out', text.replace(/\n$/, ''));
    await this.writer.write(text);
  }

  failAll(why) {
    if (this.pending) {
      clearTimeout(this.pending.timer);
      this.pending.resolve({ ok: false, error: why, items: this.pending.items, text: '' });
      this.pending = null;
    }
    for (const c of this.queue.splice(0)) c.resolve({ ok: false, error: why, items: [], text: '' });
    this.setBusy(false);
  }

  setBusy(b) {
    if (this.busy === b) return;
    this.busy = b;
    this.emit('busy');
  }

  log(dir, text) { this.emit('line', { dir, text, time: new Date() }); }
  emit(type, detail = {}) { this.dispatchEvent(new CustomEvent(type, { detail })); }
}

const SX_HEAD = [0xF0, 0x7D, 0x4D, 0x4C];
const SX_CMD = 0x03;
const SX_CMD_REPLY = 0x43;

// A bridge on USB-MIDI. midiName = the name its MIDI ports share (without " Port n"); output / input = its Port 1.
// Commands and replies are the same as on the serial; only the transport differs. There is no device log and no
// one-character test command here (those exist only on the serial).
export class MidiBridge extends Device {
  constructor(midiName, output, input) {
    super(null);
    this.transport = 'midi';
    this.midiName = midiName;
    this.output = output;
    this.input = input;
    this.partial = '';
    this.onMessage = (e) => this.onMidi(e.data);
  }

  get name() { return this.info?.name ?? this.midiName; }

  async connect() {
    if (this.open) return;
    await this.output.open();
    await this.input.open();
    this.input.addEventListener('midimessage', this.onMessage);
    this.open = true;
    this.attached = true;
    await this.askInfo();
  }

  async disconnect() {
    if (!this.open) return;
    this.open = false;
    this.starting = false;
    this.input.removeEventListener('midimessage', this.onMessage);
    this.failAll('disconnected');
    this.emit('state');
  }

  // The bridge's MIDI ports went away (unplugged)
  gone() {
    if (this.open) this.input.removeEventListener('midimessage', this.onMessage);
    super.gone();
  }

  onMidi(d) {
    if (d.length < 7 || d[0] !== 0xF0 || d[4] !== SX_CMD_REPLY || d[d.length - 1] !== 0xF7) return;
    for (let i = 1; i < 4; i++) if (d[i] !== SX_HEAD[i]) return;
    this.partial += String.fromCharCode(...d.subarray(6, d.length - 1));
    if (d[5]) return;  // continues in the next message
    const line = this.partial;
    this.partial = '';
    if (line) this.onLine(line);
  }

  async write(text) {
    if (!this.open) throw new Error('not connected');
    const line = text.replace(/\n$/, '');
    this.log('out', line);
    const bytes = [...SX_HEAD, SX_CMD];
    for (const ch of line) {
      const c = ch.charCodeAt(0);
      if (c > 0x7E || c < 0x20) throw new Error('only ASCII characters can be sent');
      bytes.push(c);
    }
    bytes.push(0xF7);
    this.output.send(bytes);
  }

  async raw() { throw new Error('only on the serial port (Load device)'); }
}

// A client reached through a bridge: commands are relayed with "port <n> ..." (single-line replies only)
export class RelayTarget {
  constructor(bridge, port) {
    this.bridge = bridge;
    this.port = port;
  }
  command(text, opt = {}) { return this.bridge.command(`port ${this.port} ${text}`, { timeout: 5000, ...opt }); }
}

function hex4(v) { return v === undefined ? '????' : v.toString(16).padStart(4, '0'); }
