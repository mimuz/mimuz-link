/*

 node-midi-access.mjs the part of Web MIDI that mimuz-link.mjs uses, for Node.js (on @julusian/midi = RtMidi)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/23 : Start Writing

*/
// Gives Node.js a MIDIAccess with the members mimuz-link.mjs uses (inputs, outputs, statechange; port open, close,
// send, midimessage), so that the driver runs unchanged in the browser and in Node.js. mimuz-link.mjs loads this file
// only in Node.js; a browser never loads it.
//
// Needs @julusian/midi (npm i @julusian/midi; a prebuilt build of RtMidi). Pass the module to the driver
// (new MimuzLink({ nodeMidi: await import('@julusian/midi') })) so that it is found in the application's node_modules;
// otherwise this file imports it from where it lies.
//
// What RtMidi does differently from Web MIDI, and how it is handled here:
//   - Every RtMidi Input / Output object is a client of the system's MIDI service. Creating one for every port scan
//     and never destroying it leaks memory and MIDI clients. Here two objects list the ports for the whole life of
//     the access, one native object exists per open port, and each is destroyed when its port closes or goes away.
//   - RtMidi does not report ports coming and going: the port names are compared every pollMs (statechange).
//   - Ports are opened by index, and the indexes shift when a device comes or goes. The index is looked up by name
//     just before opening, and the name is checked again after opening (retried if it changed in between).
//   - Identical names (two devices of the same kind) are told apart by their order (id "<name>#2").
//   - An input ignores SysEx unless told otherwise (ignoreTypes); its receive buffer is enlarged for long SysEx.
//   - The receive callback gives the time since the previous message, not a time stamp: messages are stamped with
//     performance.now() when they reach JavaScript (the same clock as Web MIDI time stamps).
//   - Errors of the native layer (a port taken by another program, a device pulled out while it is used) are caught
//     and turned into rejected promises or InvalidStateError, never into an exception from a timer.
//   - An open input keeps the Node.js process running: close() closes everything (MimuzLink.close() calls it).
//   - Ports are opened only when the driver opens or sends to them, so the MIDI devices the driver does not use are
//     left alone (some systems let only one program open a port).
//   - send(data, timeStamp): RtMidi sends at once, so a time stamp in the future is waited for with a timer.

const POLL_MS = 500;
const OPEN_TRIES = 3;
const RX_BUFFER_SIZE = 4096;   // bytes per receive buffer (RtMidi on Windows; long SysEx)
const RX_BUFFER_COUNT = 16;

class MIDIMessageEvent extends Event {
  constructor(data) {
    super('midimessage');
    this.data = data;
  }
}

class MIDIConnectionEvent extends Event {
  constructor(port) {
    super('statechange');
    this.port = port;
  }
}

function invalidState(msg) {
  const e = new Error(msg);
  e.name = 'InvalidStateError';
  return e;
}

// Names of the ports an RtMidi object sees ([] if the native layer fails)
function listNames(native) {
  try {
    const n = native.getPortCount();
    const out = [];
    for (let i = 0; i < n; i++) out.push(native.getPortName(i));
    return out;
  } catch {
    return [];
  }
}

// ids for a list of names: "<name>", and "<name>#2", "<name>#3" ... for repeated names
function withIds(names) {
  const seen = new Map();
  return names.map((name) => {
    const n = (seen.get(name) ?? 0) + 1;
    seen.set(name, n);
    return { name, nth: n, id: n === 1 ? name : `${name}#${n}` };
  });
}

class NodeMIDIPort extends EventTarget {
  constructor(access, type, name, nth, id) {
    super();
    this.access = access;
    this.type = type;            // 'input' | 'output'
    this.name = name;
    this.nth = nth;              // 1 = first port with this name, 2 = second ...
    this.id = `${type}:${id}`;
    this.manufacturer = '';
    this.version = '';
    this.state = 'connected';    // 'connected' | 'disconnected'
    this.connection = 'closed';  // 'open' | 'closed'
    this.native = null;
  }

  open() {
    try {
      this._open();
      return Promise.resolve(this);
    } catch (e) {
      return Promise.reject(e);
    }
  }

  close() {
    this._closeNative();
    this._setConnection('closed');
    return Promise.resolve(this);
  }

  _open() {
    if (this.connection === 'open') return;
    if (this.state !== 'connected') throw invalidState(`${this.name}: disconnected`);
    const lib = this.access.lib;
    let lastError = null;
    for (let t = 0; t < OPEN_TRIES; t++) {
      const native = this.type === 'input' ? new lib.Input() : new lib.Output();
      try {
        const index = this._indexIn(native);
        if (index < 0) throw invalidState(`${this.name}: not found`);
        if (this.type === 'input') {
          try { native.setBufferSize(RX_BUFFER_SIZE, RX_BUFFER_COUNT); } catch { /* not on every system */ }
          native.ignoreTypes(!this.access.sysexEnabled, true, true);  // SysEx as allowed; no clock, no active sensing
        }
        native.openPort(index);
        if (native.getPortName(index) !== this.name) throw new Error('the ports changed while opening');
        if (this.type === 'input') {
          native.on('message', (deltaTime, message) => {
            if (this.connection !== 'open') return;
            try {
              this.dispatchEvent(new MIDIMessageEvent(Uint8Array.from(message)));
            } catch (e) {
              queueMicrotask(() => { throw e; });  // a listener's error, reported like any other
            }
          });
        }
        this.native = native;
        this._setConnection('open');
        return;
      } catch (e) {
        lastError = e;
        try { native.removeAllListeners?.('message'); } catch { /* ignore */ }
        try { native.closePort(); } catch { /* ignore */ }
        try { native.destroy(); } catch { /* ignore */ }
        if (e.name === 'InvalidStateError') break;
      }
    }
    throw lastError ?? new Error(`${this.name}: could not open`);
  }

  // Index of this port (its name, nth occurrence) in what native sees now, or -1
  _indexIn(native) {
    let seen = 0;
    const names = listNames(native);
    for (let i = 0; i < names.length; i++) {
      if (names[i] === this.name && ++seen === this.nth) return i;
    }
    return -1;
  }

  _closeNative() {
    const n = this.native;
    this.native = null;
    if (!n) return;
    try { n.removeAllListeners?.('message'); } catch { /* ignore */ }
    try { n.closePort(); } catch { /* ignore */ }
    try { n.destroy(); } catch { /* ignore */ }
  }

  _setConnection(c) {
    if (this.connection === c) return;
    this.connection = c;
    this.dispatchEvent(new MIDIConnectionEvent(this));
  }

  // The device went away
  _gone() {
    this._closeNative();
    this.state = 'disconnected';
    this.connection = 'closed';
    this.dispatchEvent(new MIDIConnectionEvent(this));
  }
}

class NodeMIDIInput extends NodeMIDIPort {
  constructor(access, name, nth, id) { super(access, 'input', name, nth, id); }
}

class NodeMIDIOutput extends NodeMIDIPort {
  constructor(access, name, nth, id) { super(access, 'output', name, nth, id); }

  send(data, timeStamp) {
    if (this.state !== 'connected') throw invalidState(`${this.name}: disconnected`);
    if (!this.access.sysexEnabled && data[0] === 0xF0) throw invalidState('SysEx is not allowed');
    this._open();  // like Web MIDI, sending opens the port
    const bytes = Array.from(data);
    const delay = timeStamp ? timeStamp - performance.now() : 0;
    if (delay > 1) setTimeout(() => this._sendNow(bytes), delay);
    else this._sendNow(bytes);
  }

  _sendNow(bytes) {
    if (!this.native) return;
    try {
      this.native.sendMessage(bytes);
    } catch {
      // the device went away while sending; the next port scan reports it
    }
  }
}

class NodeMIDIAccess extends EventTarget {
  constructor(lib, { sysex = false, pollMs = POLL_MS } = {}) {
    super();
    this.lib = lib;
    this.sysexEnabled = !!sysex;
    this.inputs = new Map();
    this.outputs = new Map();
    this.onstatechange = null;
    this._listIn = new lib.Input();
    this._listOut = new lib.Output();
    this._scan();
    this._timer = setInterval(() => this._scan(), pollMs);  // keeps the process running until close()
  }

  // Closes every port and stops watching; the process can end afterwards
  close() {
    clearInterval(this._timer);
    this._timer = null;
    for (const p of [...this.inputs.values(), ...this.outputs.values()]) p._closeNative();
    for (const n of [this._listIn, this._listOut]) {
      try { n.destroy(); } catch { /* ignore */ }
    }
    this._listIn = this._listOut = null;
  }

  _scan() {
    if (!this._listIn) return;
    this._sync(this.inputs, withIds(listNames(this._listIn)), (n) => new NodeMIDIInput(this, n.name, n.nth, n.id));
    this._sync(this.outputs, withIds(listNames(this._listOut)), (n) => new NodeMIDIOutput(this, n.name, n.nth, n.id));
  }

  _sync(map, now, make) {
    const type = map === this.inputs ? 'input' : 'output';
    const ids = new Set(now.map((n) => `${type}:${n.id}`));
    for (const [id, p] of map) {
      if (ids.has(id)) continue;
      map.delete(id);
      p._gone();
      this._tell(p);
    }
    for (const n of now) {
      if (map.has(`${type}:${n.id}`)) continue;
      const p = make(n);
      map.set(p.id, p);
      this._tell(p);
    }
  }

  _tell(port) {
    const e = new MIDIConnectionEvent(port);
    this.dispatchEvent(e);
    try { this.onstatechange?.(e); } catch (err) { queueMicrotask(() => { throw err; }); }
  }
}

// Like navigator.requestMIDIAccess. lib = the @julusian/midi module (imported here when not given).
export async function requestMIDIAccess({ sysex = false, lib = null, pollMs = POLL_MS } = {}) {
  let m = lib;
  if (!m) {
    try {
      m = await import('@julusian/midi');
    } catch (e) {
      throw new Error(`@julusian/midi is needed for MIDI in Node.js (npm i @julusian/midi): ${e.message}`);
    }
  }
  m = m.Input ? m : m.default;
  if (!m?.Input || !m?.Output) throw new Error('not the @julusian/midi module');
  return new NodeMIDIAccess(m, { sysex, pollMs });
}
