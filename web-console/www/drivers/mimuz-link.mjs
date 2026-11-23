/*

 mimuz-link.mjs mimuz-link driver for web applications: bridges, clients and their MIDI (Web MIDI)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/23 : Start Writing
 - 2026/11/23 : Runs in Node.js too (node-midi-access.mjs on @julusian/midi)

*/
// The low-level driver of mimuz-link for web pages. It finds the bridges among the Web MIDI ports, learns which client
// is on which bridge port, and lets the page pick a client by its device name, label or model and exchange MIDI with
// it, whichever port it is on and however often it reconnects. Device drivers (one per kind of client: what its
// buttons, knobs or sounds are) are written on top of this file; it knows nothing about them.
//
//   import MimuzLink from './drivers/mimuz-link.mjs';
//   const link = await new MimuzLink().init();         // null when Web MIDI (with SysEx) is not available: link.error
//   const knob = link.client({ model: 'knob4' });       // follows the first connected client of that model
//   knob.addEventListener('midi', (e) => console.log(e.detail.data));
//   knob.addEventListener('connect', () => knob.noteOn(1, 60, 100));
//   link.addEventListener('change', () => render(link.bridges, link.clients));
//
// What it uses (see the README of mimuz-link, "SysEx inquiries"):
//   - Bridges are the MIDI ports whose names contain "mi:muz:bridgeESPS3-<unique ID>" (8 ports each). Which MIDI port is
//     which bridge port is learned from the bridge's port replies (cmd 01), not from the port names, which differ
//     between operating systems.
//   - A bridge tells the clients on its ports (device name, label, model). From format version 2 on it also tells by
//     itself when a client connects, leaves or tells its label; older bridges are asked again every pollMs.
//   - Clients without a bridge are BLE-MIDI devices named "mi:muz:conESP32C6-<unique ID>". They do not answer SysEx,
//     so over BLE only the device name is known (no label, no model, no config commands).
//   - Config commands go to a bridge as SysEx (cmd 03) and are relayed by the bridge to a client ("port <n> ...").
// Web MIDI must be allowed with SysEx: without it the bridges cannot be asked anything.
// In Node.js (20 or later) the same code runs on node-midi-access.mjs, which needs @julusian/midi:
//   const link = await new MimuzLink({ nodeMidi: await import('@julusian/midi') }).init();
//   ...
//   link.close();   // open MIDI ports keep the process running until they are closed
// Several pages may talk to the same bridge at once (each sees the others' replies; command ids start at random so
// that the replies are told apart). Some operating systems let only one program open a MIDI port at a time.

export const NAME_PREFIX = { bridge: 'mi:muz:bridgeESPS3-', client: 'mi:muz:conESP32C6-' };
export const UID_LEN = 11;
export const NUM_PORTS = 8;

const SX_HEAD = [0xF0, 0x7D, 0x4D, 0x4C];
const SX_PORT = 0x01;
const SX_BRIDGE = 0x02;
const SX_CMD = 0x03;
const SX_REPLY = 0x40;

const DEFAULT_TIMEOUT_MS = 3000;
const RELAY_TIMEOUT_MS = 5000;   // the bridge gives up on a client after 1.5 s; this covers the round trip and queueing
const PROBE_TIMEOUT_MS = 400;    // reply to a port inquiry
const PROBE_TRIES = 3;
const START_RETRY_MS = 2000;     // a bridge does not answer while it surveys the radio channels (about 12 s after start)
const SCAN_DELAY_MS = 300;       // Web MIDI reports ports one by one; wait until they settle

// Keys whose values are names, never numbers (a label "007" must stay "007")
const TEXT_KEYS = new Set(['label', 'name', 'mac', 'bridge', 'fw', 'model']);

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

// "<prefix><unique ID>" found in a MIDI port name, or null
function deviceKey(portName, prefix) {
  const i = portName.indexOf(prefix);
  return i < 0 ? null : portName.slice(i, i + prefix.length + UID_LEN);
}

// Length-prefixed ASCII string at d[at] -> [string, next index]
function readStr(d, at) {
  if (at >= d.length - 1) return ['', at];
  const n = d[at];
  return [String.fromCharCode(...d.subarray(at + 1, at + 1 + n)), at + 1 + n];
}

// Sends without throwing (a port may go away, or be taken by another program); false if it failed
function trySend(output, data) {
  try {
    output.send(data);
    return true;
  } catch {
    return false;
  }
}

function isSysEx(d) {
  if (d.length < 6 || d[0] !== 0xF0 || d[d.length - 1] !== 0xF7) return false;
  for (let i = 1; i < 4; i++) if (d[i] !== SX_HEAD[i]) return false;
  return true;
}

class Emitter extends EventTarget {
  emit(type, detail = {}) { this.dispatchEvent(new CustomEvent(type, { detail })); }
}

// Sending MIDI to a client. Channels are 1-16 as on devices and in the mimuz-link documents.
const MidiSend = {
  noteOn(ch, note, vel = 100, at) { return this.send([0x90 | ((ch - 1) & 15), note & 127, vel & 127], at); },
  noteOff(ch, note, vel = 0, at) { return this.send([0x80 | ((ch - 1) & 15), note & 127, vel & 127], at); },
  polyAftertouch(ch, note, value, at) { return this.send([0xA0 | ((ch - 1) & 15), note & 127, value & 127], at); },
  controlChange(ch, number, value, at) { return this.send([0xB0 | ((ch - 1) & 15), number & 127, value & 127], at); },
  programChange(ch, program, at) { return this.send([0xC0 | ((ch - 1) & 15), program & 127], at); },
  channelAftertouch(ch, value, at) { return this.send([0xD0 | ((ch - 1) & 15), value & 127], at); },
  // value: -8192..8191 (0 = center)
  pitchBend(ch, value, at) {
    const v = Math.max(0, Math.min(16383, Math.round(value) + 8192));
    return this.send([0xE0 | ((ch - 1) & 15), v & 127, v >> 7], at);
  },
};

// ======================================================================================================
// Client: one client device, by its device name. The object stays the same while the client disconnects, comes back,
// or moves to another port or bridge.
//   events: 'connect', 'disconnect', 'change' (label, model, port or bridge changed), 'midi' ({ data, timeStamp })
// ======================================================================================================
export class Client extends Emitter {
  constructor(link, name) {
    super();
    this.link = link;
    this.name = name;       // device name ("mi:muz:conESP32C6-<unique ID>"), unique
    this.label = '';        // the user's name for it (several clients may share one)
    this.model = '';        // what kind of device it is (set by its firmware; empty on older firmware and over BLE)
    this.via = null;        // 'espnow' (through a bridge) or 'ble' (BLE-MIDI, directly), null while not connected
    this.bridge = null;     // Bridge, when via is 'espnow'
    this.port = 0;          // bridge port 1-8, when via is 'espnow'
    this._bleOut = null;
  }

  get connected() { return this.via !== null; }
  // MIDIOutput used for sending (null while not connected, or while its bridge port is still being learned)
  get output() {
    if (this.via === 'espnow') return this.bridge.output(this.port);
    return this.via === 'ble' ? this._bleOut : null;
  }

  // Sends one MIDI message; false if the client is not connected or the port refused it
  send(data, timeStamp) {
    const out = this.output;
    if (!out) return false;
    if (data[0] === 0xF0) throw new Error('SysEx is not carried to clients');
    try {
      out.send(data, timeStamp);
      return true;
    } catch {
      return false;
    }
  }

  // A config command run on the client (see the README, "Config commands"), relayed by its bridge.
  // Resolves { ok, fields, items, error, text }.
  async command(text, opt = {}) {
    if (this.via !== 'espnow') return { ok: false, error: this.via === 'ble' ? 'not_available_over_ble' : 'not_connected', items: [], text: '' };
    return this.bridge.command(`port ${this.port} ${text}`, { timeout: RELAY_TIMEOUT_MS, ...opt });
  }

  async setLabel(label) {
    const r = await this.command(`set label ${label || '-'}`);
    if (r.ok) this.bridge?.inquirePort(this.port);  // the client also tells the bridge, which then tells us
    return r;
  }

  _attach(via, bridge, port, bleOut = null) {
    const was = this.connected;
    const moved = this.via !== via || this.bridge !== bridge || this.port !== port;
    this.via = via;
    this.bridge = bridge;
    this.port = port;
    this._bleOut = bleOut;
    if (!was) this.emit('connect');
    else if (moved) this.emit('change');
  }

  _detach() {
    if (!this.connected) return;
    this.via = null;
    this.bridge = null;
    this.port = 0;
    this._bleOut = null;
    this.emit('disconnect');
  }

  _setMeta(label, model) {
    if (label === this.label && model === this.model) return false;
    this.label = label;
    this.model = model;
    if (this.connected) this.emit('change');
    return true;
  }

  _midi(data, timeStamp) {
    const detail = { data, timeStamp, client: this };
    this.emit('midi', detail);
    this.link.emit('midi', detail);
  }
}
Object.assign(Client.prototype, MidiSend);

// ======================================================================================================
// ClientRef: follows whichever connected client matches a selector ({ name, label, model, via }; all given keys must
// match). Device drivers hold one of these: it reconnects by itself when the client comes back, or when another
// client of the same kind is connected.
//   events: 'connect', 'disconnect', 'change', 'midi' (as Client)
// ======================================================================================================
export class ClientRef extends Emitter {
  constructor(link, selector) {
    super();
    this.link = link;
    this.selector = { ...selector };
    this.client = null;
    this._was = false;      // connected as last told to listeners
    this._fwd = (e) => this.emit(e.type, e.detail);
    this._lost = () => this._resolve();
    queueMicrotask(() => this._resolve());  // after the caller has added its listeners, so that it hears 'connect'
  }

  get connected() { return !!this.client?.connected; }
  get name() { return this.client?.name ?? ''; }
  get label() { return this.client?.label ?? ''; }
  get model() { return this.client?.model ?? ''; }
  get via() { return this.client?.via ?? null; }
  get port() { return this.client?.port ?? 0; }
  get bridge() { return this.client?.bridge ?? null; }

  send(data, timeStamp) { return this.client ? this.client.send(data, timeStamp) : false; }
  command(text, opt) { return this.client ? this.client.command(text, opt) : Promise.resolve({ ok: false, error: 'not_connected', items: [], text: '' }); }
  setLabel(label) { return this.client ? this.client.setLabel(label) : Promise.resolve({ ok: false, error: 'not_connected', items: [], text: '' }); }

  // Resolves when a matching client is connected (rejects after timeout ms; 0 = wait for ever)
  ready({ timeout = 0 } = {}) {
    if (this.connected) return Promise.resolve(this);
    return new Promise((resolve, reject) => {
      const t = timeout ? setTimeout(() => { this.removeEventListener('connect', ok); reject(new Error('timeout')); }, timeout) : null;
      const ok = () => { clearTimeout(t); resolve(this); };
      this.addEventListener('connect', ok, { once: true });
    });
  }

  // Stops following (the link keeps no reference afterwards)
  release() {
    this._bind(null);
    this.link._refs.delete(this);
  }

  _resolve() {
    const cur = this.client;
    if (cur?.connected && this.link.matches(cur, this.selector)) return;
    this._bind(this.link.find(this.selector));
  }

  // Follows client c (or none) and tells the listeners what changed for them
  _bind(c) {
    const old = this.client;
    if (old !== c) {
      if (old) {
        for (const t of ['midi', 'change']) old.removeEventListener(t, this._fwd);
        old.removeEventListener('disconnect', this._lost);
      }
      this.client = c;
      if (c) {
        for (const t of ['midi', 'change']) c.addEventListener(t, this._fwd);
        c.addEventListener('disconnect', this._lost);
      }
    }
    const now = !!c?.connected;
    if (now && !this._was) this.emit('connect');
    else if (!now && this._was) this.emit('disconnect');
    else if (now && old !== c) this.emit('change');
    this._was = now;
  }
}
Object.assign(ClientRef.prototype, MidiSend);

// ======================================================================================================
// Bridge: one bridge on USB-MIDI (8 ports).
//   info  : { name, fw, channel, portsUsed, ports, format } once it has answered (null while it is starting)
//   ports : [1..8] -> { state, name, label, model } (state 0 = free, 1 = client not identified yet, 2 = identified)
//   events: 'ready' (answered for the first time), 'change' (its ports changed), 'line' ({ dir: 'out' | 'in', text }:
//           the config command lines, for a console), 'busy' (a command is running or not)
// ======================================================================================================
export class Bridge extends Emitter {
  constructor(link, key) {
    super();
    this.link = link;
    this.key = key;          // "mi:muz:bridgeESPS3-<unique ID>", as in its MIDI port names
    this.info = null;
    this.ports = Array.from({ length: NUM_PORTS + 1 }, () => ({ state: 0, name: '', label: '', model: '' }));
    this.outputs = [];       // MIDIOutput of this bridge
    this.inputs = [];        // MIDIInput of this bridge
    this.outPort = new Map(); // MIDIOutput -> bridge port 1-8
    this.inPort = new Map();  // MIDIInput -> bridge port 1-8
    this.partial = new Map(); // MIDIInput -> text of a command reply continued in the next message
    this.queue = [];
    this.pending = null;     // { id, resolve, items, timer }
    this.nextId = 1000 + Math.floor(Math.random() * 8000);
    this.busy = false;
    this.gone = false;
    this.probe = null;       // { resolve } while a port inquiry waits for its reply
    this.infoWaiter = null;  // wakes _start when the bridge has answered
    this.askedAt = new Array(NUM_PORTS + 1).fill(0);  // when each port was last asked about because of unexpected MIDI
    this.pollTimer = null;
    this.onMessage = (e) => this._onMidi(e.target, e.data, e.timeStamp);
  }

  get name() { return this.info?.name ?? this.key; }
  get ready() { return !!this.info; }

  // The output a port's MIDI goes to (null until learned)
  output(port) {
    for (const [o, p] of this.outPort) if (p === port) return o;
    return null;
  }

  // Asks the bridge about one port again (the answer arrives as a port reply)
  inquirePort(port) {
    const o = this.output(port);
    if (o) trySend(o, [...SX_HEAD, SX_PORT, 0xF7]);
  }

  // A config command run on the bridge (see the README, "Config commands"); "port <n> ..." is relayed to a client.
  // Resolves { ok, fields, items, error, text }; rejects on timeout.
  command(text, { timeout = DEFAULT_TIMEOUT_MS } = {}) {
    return new Promise((resolve, reject) => {
      this.queue.push({ text, timeout, resolve, reject });
      this._pump();
    });
  }

  // ---- internal ----

  _setMidiPorts(outputs, inputs) {
    const same = outputs.length === this.outputs.length && inputs.length === this.inputs.length
      && outputs.every((o) => this.outputs.includes(o)) && inputs.every((i) => this.inputs.includes(i));
    for (const i of this.inputs) if (!inputs.includes(i)) i.removeEventListener('midimessage', this.onMessage);
    for (const i of inputs) {
      if (this.inputs.includes(i)) continue;
      i.addEventListener('midimessage', this.onMessage);
      i.open().catch(() => {});  // a listener added with addEventListener does not open the port by itself
    }
    this.outputs = outputs;
    this.inputs = inputs;
    if (!same) {  // new ports: the bridge may have restarted, so learn everything again (this also turns its port news on)
      this.info = null;
      this.outPort.clear();
      this.inPort.clear();
      this._start();
    }
  }

  async _start() {
    const run = (this.runId = (this.runId ?? 0) + 1);
    // Ask "who are you" until it answers (it does not while it surveys the radio channels after a start)
    while (!this.gone && run === this.runId && !this.info) {
      const out = this.outputs[0];
      if (out) trySend(out, [...SX_HEAD, SX_BRIDGE, 0xF7]);
      await new Promise((r) => {
        const t = setTimeout(r, START_RETRY_MS);
        this.infoWaiter = () => { clearTimeout(t); r(); };
      });
      this.infoWaiter = null;
    }
    if (this.gone || run !== this.runId) return;
    await this._mapPorts(run);
    if (this.gone || run !== this.runId) return;
    this._setPoll();
    this.emit('ready');
    this.link._changed('bridge');
  }

  // Which MIDI output is which bridge port: send a port inquiry on each output and see which port answers. The reply
  // also says which input belongs to that port. When an input has the same name as the output (most systems), the
  // reply must come on that input; otherwise (the names differ, e.g. on Windows) the reply's port number is taken.
  async _mapPorts(run) {
    for (const o of this.outputs) {
      for (let t = 0; t < PROBE_TRIES && !this.outPort.has(o); t++) {
        if (this.gone || run !== this.runId) return;
        const r = await this._probe(o);
        if (!r) continue;
        const twin = this.inputs.find((i) => i.name === o.name);
        if (twin && twin !== r.input) continue;  // a reply for some other port arrived in between
        this.outPort.set(o, r.port);
      }
    }
  }

  _probe(o) {
    return new Promise((resolve) => {
      const t = setTimeout(() => { this.probe = null; resolve(null); }, PROBE_TIMEOUT_MS);
      this.probe = { resolve: (r) => { clearTimeout(t); this.probe = null; resolve(r); } };
      if (!trySend(o, [...SX_HEAD, SX_PORT, 0xF7])) this.probe.resolve(null);
    });
  }

  // Bridges from format version 2 on tell port changes by themselves; older ones are asked every pollMs
  _setPoll() {
    clearInterval(this.pollTimer);
    this.pollTimer = null;
    const ms = this.info.format >= 2 ? this.link.options.pollMs2 : this.link.options.pollMs;
    if (!ms) return;
    this.pollTimer = setInterval(() => { for (const o of this.outPort.keys()) trySend(o, [...SX_HEAD, SX_PORT, 0xF7]); }, ms);
  }

  _remove() {
    this.gone = true;
    clearInterval(this.pollTimer);
    for (const i of this.inputs) i.removeEventListener('midimessage', this.onMessage);
    this.inputs = [];
    this.outputs = [];
    for (let p = 1; p <= NUM_PORTS; p++) this._setPort(p, 0, '', '', '');
    this._failAll('bridge removed');
  }

  _onMidi(input, d, timeStamp) {
    if (d[0] === 0xF0) {
      if (isSysEx(d)) this._onSysEx(input, d);
      return;
    }
    const p = this.inPort.get(input);
    if (!p) return;
    const c = this.link._byPort(this, p);
    if (c) c._midi(d, timeStamp);
    else if (this.ports[p].state === 0 && timeStamp - this.askedAt[p] > 500) {  // MIDI from a port we think is free: ask
      this.askedAt[p] = timeStamp;
      this.inquirePort(p);
    }
  }

  _onSysEx(input, d) {
    const cmd = d[4];
    if (cmd === (SX_PORT | SX_REPLY) && d.length >= 9) {
      const fmt = d[5];
      const port = d[6];
      const state = d[7];
      if (port < 1 || port > NUM_PORTS) return;
      let at = 8;
      let name, label, model = '';
      [name, at] = readStr(d, at);
      [label, at] = readStr(d, at);
      if (fmt >= 2) [model, at] = readStr(d, at);
      this.inPort.set(input, port);
      if (this.probe) this.probe.resolve({ port, input });
      this._setPort(port, state, name, label, model);
    } else if (cmd === (SX_BRIDGE | SX_REPLY) && d.length >= 9) {
      let at = 6;
      let name, fw;
      [name, at] = readStr(d, at);
      [fw, at] = readStr(d, at);
      const first = !this.info;
      const info = { name, fw, channel: d[at], portsUsed: d[at + 1], ports: d[at + 2], format: d[5] };
      const same = !first && JSON.stringify(info) === JSON.stringify(this.info);
      this.info = info;
      if (first) this.infoWaiter?.();
      else if (!same) this.link._changed('bridge');  // answers to repeated inquiries (while it was starting) change nothing
    } else if (cmd === (SX_CMD | SX_REPLY) && d.length >= 7) {
      const text = (this.partial.get(input) ?? '') + String.fromCharCode(...d.subarray(6, d.length - 1));
      if (d[5]) { this.partial.set(input, text); return; }  // continues in the next message
      this.partial.delete(input);
      if (text) this._onLine(text);
    }
  }

  _setPort(port, state, name, label, model) {
    const pt = this.ports[port];
    if (pt.state === state && pt.name === name && pt.label === label && pt.model === model) return;
    Object.assign(pt, { state, name, label, model });
    this.link._portChanged(this, port);
    this.emit('change', { port });
  }

  _onLine(line) {
    this.emit('line', { dir: 'in', text: line });
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
    this._pump();
  }

  _pump() {
    if (this.pending || !this.queue.length) {
      this._setBusy(!!this.pending);
      return;
    }
    const out = this.output(1) ?? this.outputs[0];
    if (this.gone || !out) {
      this._failAll(this.gone ? 'bridge removed' : 'not connected');
      return;
    }
    const c = this.queue.shift();
    const id = this.nextId;
    this.nextId = this.nextId >= 9999 ? 1000 : this.nextId + 1;
    const bytes = [...SX_HEAD, SX_CMD];
    for (const ch of `@${id} ${c.text}`) {
      const v = ch.charCodeAt(0);
      if (v < 0x20 || v > 0x7E) {
        c.resolve({ ok: false, error: 'only ASCII characters can be sent', items: [], text: '' });
        this._pump();
        return;
      }
      bytes.push(v);
    }
    bytes.push(0xF7);
    this.pending = {
      id, items: [], resolve: c.resolve,
      timer: setTimeout(() => {
        if (this.pending?.id !== id) return;
        this.pending = null;
        c.reject(new Error(`timeout: ${c.text}`));
        this._pump();
      }, c.timeout),
    };
    this._setBusy(true);
    this.emit('line', { dir: 'out', text: `@${id} ${c.text}` });
    try {
      out.send(bytes);
    } catch (e) {
      clearTimeout(this.pending.timer);
      this.pending = null;
      c.resolve({ ok: false, error: e.message, items: [], text: '' });
      this._pump();
    }
  }

  _failAll(why) {
    if (this.pending) {
      clearTimeout(this.pending.timer);
      this.pending.resolve({ ok: false, error: why, items: this.pending.items, text: '' });
      this.pending = null;
    }
    for (const c of this.queue.splice(0)) c.resolve({ ok: false, error: why, items: [], text: '' });
    this._setBusy(false);
  }

  _setBusy(b) {
    if (this.busy === b) return;
    this.busy = b;
    this.emit('busy');
  }
}

// ======================================================================================================
// MimuzLink: all bridges and clients this page can reach.
//   options: nodeMidi = the @julusian/midi module, for Node.js (imported by node-midi-access.mjs when not given)
//            pollMs  = how often bridges older than format version 2 are asked about their ports (default 1000)
//            pollMs2 = the same for newer bridges, which tell changes by themselves (default 0 = never)
//            clientPrefixes = device name prefixes of clients on BLE-MIDI (default [NAME_PREFIX.client])
//   events : 'change' ({ reason: 'midi' | 'bridge' | 'port' }: bridges or clients came, went or changed),
//            'midi' ({ client, data, timeStamp }: MIDI from any client)
// ======================================================================================================
export default class MimuzLink extends Emitter {
  constructor(options = {}) {
    super();
    this.options = { pollMs: 1000, pollMs2: 0, clientPrefixes: [NAME_PREFIX.client], ...options };
    this.midi = null;
    this.error = '';
    this._bridges = new Map();   // key -> Bridge
    this._clients = new Map();   // device name -> Client
    this._ble = new Map();       // device name -> { output, input, onMessage }
    this._refs = new Set();      // ClientRef
    this._scanTimer = null;
    this._changeTimer = null;
    this._changeReasons = new Set();
    this._onState = () => { clearTimeout(this._scanTimer); this._scanTimer = setTimeout(() => this._scan(), SCAN_DELAY_MS); };
  }

  // Asks for Web MIDI with SysEx (in Node.js: opens RtMidi). Resolves this, or null (the reason is in this.error).
  async init() {
    try {
      if (globalThis.navigator?.requestMIDIAccess) {
        this.midi = await navigator.requestMIDIAccess({ sysex: true });
      } else if (globalThis.process?.versions?.node) {
        const { requestMIDIAccess } = await import('./node-midi-access.mjs');
        this.midi = await requestMIDIAccess({ sysex: true, lib: this.options.nodeMidi });
      } else {
        this.error = 'This browser has no Web MIDI.';
        return null;
      }
    } catch (e) {
      this.error = globalThis.navigator?.requestMIDIAccess ? `MIDI with SysEx was not allowed (${e.message}).` : e.message;
      return null;
    }
    this.midi.addEventListener('statechange', this._onState);
    this._scan();
    return this;
  }

  // Stops everything (in Node.js this closes the MIDI ports, so that the process can end)
  close() {
    this._closed = true;
    clearTimeout(this._scanTimer);
    clearTimeout(this._changeTimer);
    this.midi?.removeEventListener('statechange', this._onState);
    for (const b of this._bridges.values()) b._remove();
    for (const [, x] of this._ble) x.input?.removeEventListener('midimessage', x.onMessage);
    this._bridges.clear();
    this._ble.clear();
    for (const r of [...this._refs]) r.release();
    this.midi?.close?.();
  }

  get bridges() { return [...this._bridges.values()]; }
  // Connected clients, through bridges first (by bridge and port), then over BLE
  get clients() {
    return [...this._clients.values()].filter((c) => c.connected)
      .sort((a, b) => (a.via === b.via ? (a.bridge === b.bridge ? a.port - b.port : 0) : a.via === 'espnow' ? -1 : 1));
  }

  matches(c, sel = {}) {
    if (sel.name !== undefined && c.name !== sel.name) return false;
    if (sel.label !== undefined && c.label !== sel.label) return false;
    if (sel.model !== undefined && c.model !== sel.model) return false;
    if (sel.via !== undefined && c.via !== sel.via) return false;
    return true;
  }
  find(sel) { return this.clients.find((c) => this.matches(c, sel)) ?? null; }
  findAll(sel) { return this.clients.filter((c) => this.matches(c, sel)); }

  // A ClientRef that follows the first connected client matching sel (see ClientRef)
  client(sel) {
    const r = new ClientRef(this, sel);
    this._refs.add(r);
    return r;
  }

  // ---- internal ----

  _scan() {
    if (!this.midi || this._closed) return;
    const group = (ports, prefix) => {
      const m = new Map();
      for (const p of ports) {
        if (p.state !== 'connected') continue;
        const k = deviceKey(p.name, prefix);
        if (k) (m.get(k) ?? m.set(k, []).get(k)).push(p);
      }
      return m;
    };
    const outs = group(this.midi.outputs.values(), NAME_PREFIX.bridge);
    const ins = group(this.midi.inputs.values(), NAME_PREFIX.bridge);
    let changed = false;
    for (const [k, b] of this._bridges) {
      if (outs.has(k) && ins.has(k)) continue;
      b._remove();
      this._bridges.delete(k);
      changed = true;
    }
    for (const [k, o] of outs) {
      const i = ins.get(k);
      if (!i) continue;
      let b = this._bridges.get(k);
      if (!b) {
        b = new Bridge(this, k);
        this._bridges.set(k, b);
        changed = true;
      }
      b._setMidiPorts(o, i);
    }
    // Clients on BLE-MIDI
    const bleOuts = new Map();
    const bleIns = new Map();
    for (const prefix of this.options.clientPrefixes) {
      for (const [k, v] of group(this.midi.outputs.values(), prefix)) bleOuts.set(k, v[0]);
      for (const [k, v] of group(this.midi.inputs.values(), prefix)) bleIns.set(k, v[0]);
    }
    for (const [name, x] of this._ble) {
      if (bleOuts.get(name) === x.output && bleIns.get(name) === x.input) continue;
      x.input?.removeEventListener('midimessage', x.onMessage);
      this._ble.delete(name);
      const c = this._clients.get(name);
      if (c?.via === 'ble') c._detach();
      changed = true;
    }
    for (const [name, output] of bleOuts) {
      if (this._ble.has(name)) continue;
      const input = bleIns.get(name) ?? null;
      const c = this._client(name);
      const onMessage = (e) => { if (e.data[0] !== 0xF0) c._midi(e.data, e.timeStamp); };
      input?.addEventListener('midimessage', onMessage);
      input?.open().catch(() => {});
      this._ble.set(name, { output, input, onMessage });
      if (c.via !== 'espnow') c._attach('ble', null, 0, output);
      changed = true;
    }
    if (changed) this._changed('midi');
  }

  _client(name) {
    let c = this._clients.get(name);
    if (!c) {
      c = new Client(this, name);
      this._clients.set(name, c);
    }
    return c;
  }

  _byPort(bridge, port) {
    for (const c of this._clients.values()) if (c.bridge === bridge && c.port === port) return c;
    return null;
  }

  // A bridge port's client changed (from its port reply)
  _portChanged(bridge, port) {
    const pt = bridge.ports[port];
    const old = this._byPort(bridge, port);
    const name = pt.state === 2 ? pt.name : '';
    if (old && old.name !== name) {
      old._detach();
      const x = this._ble.get(old.name);  // still on BLE as well? (only for a moment while it switches)
      if (x) old._attach('ble', null, 0, x.output);
    }
    if (name) {
      const c = this._client(name);
      c._setMeta(pt.label, pt.model);
      c._attach('espnow', bridge, port);
    }
    this._changed('port');
  }

  // Tells 'change' once for several changes in a row, after the ClientRefs have followed them
  _changed(reason) {
    if (this._closed) return;
    this._changeReasons.add(reason);
    if (this._changeTimer) return;
    this._changeTimer = setTimeout(() => {
      this._changeTimer = null;
      const reasons = [...this._changeReasons];
      this._changeReasons.clear();
      for (const r of this._refs) r._resolve();
      this.emit('change', { reason: reasons[0], reasons });
    }, 0);
  }
}
