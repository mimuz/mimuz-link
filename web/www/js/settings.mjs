/*

 settings.mjs mimuz-link web console: settings and commands known to the firmware

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/22 : Start Writing
 - 2026/11/22 : Fixed device name prefixes and USB IDs of the bridge and the client

*/
// Device names are "<fixed prefix>-<unique ID (11 characters)>"; the prefixes are fixed so that the console (and other
// applications) can tell bridges and clients apart by name. The bridge's USB serial number is its unique ID.
export const NAME_PREFIX = { bridge: 'mi:muz:bridgeESPS3-', client: 'mi:muz:conESP32C6-' };
export const UID_LEN = 11;
// USB IDs of the serial ports: the bridge's are set by its firmware, the client's are the ESP32-C6's built-in USB
export const USB_IDS = { bridge: { vid: 0x2886, pid: 0x8057 }, client: { vid: 0x303a, pid: 0x1001 } };

export function kindOfName(name) {
  for (const [kind, p] of Object.entries(NAME_PREFIX)) if (name.startsWith(p)) return kind;
  return 'other';
}
export function uidOfName(name) {
  const kind = kindOfName(name);
  return kind === 'other' ? '' : name.slice(NAME_PREFIX[kind].length, NAME_PREFIX[kind].length + UID_LEN);
}
export function kindOfUsb(usb) {
  for (const [kind, id] of Object.entries(USB_IDS)) if (usb.vid === id.vid && usb.pid === id.pid) return kind;
  return 'unknown';
}

// Taken from docs/config-commands.md. saved = written to non-volatile memory by "set"; the others are
// diagnostic and last until the next restart.

export const PHY_RATES = [
  { value: 0, label: '11b 1M' },
  { value: 1, label: '11g 6M (default)' },
  { value: 2, label: '11g 24M' },
  { value: 3, label: 'HT20 MCS4' },
];

export const BRIDGE_SETTINGS = [
  { key: 'channel_min', label: 'Lowest radio channel', type: 'int', min: 1, max: 13, saved: true,
    help: 'Lowest channel the survey may choose (takes effect at the next survey)' },
  { key: 'channel_max', label: 'Highest radio channel', type: 'int', min: 1, max: 13, saved: true,
    help: 'Highest channel the survey may choose (takes effect at the next survey)' },
  { key: 'noteon_max_age_ms', label: 'Note On max age [ms]', type: 'int', min: 0, max: 2000, saved: true,
    help: 'A Note On that waited longer than this is dropped (PC to client). 0 = never' },
  { key: 'retry_limit', label: 'Radio retry limit', type: 'int', min: 0, max: 31, saved: false,
    help: 'Retries of an unacknowledged frame. Default 8; 0 = the Wi-Fi library value (32)' },
  { key: 'tx_parallel', label: 'Parallel sending', type: 'bool', saved: false,
    help: 'One packet per client handed to the radio in parallel (default on)' },
  { key: 'send_lock', label: 'Send under driver lock', type: 'bool', saved: false,
    help: 'Hand packets to the radio under the Wi-Fi driver lock (default on; off can stall sending)' },
  { key: 'tx_fail_max', label: 'Skip after failures', type: 'int', min: 1, max: 50, saved: false, diag: true,
    help: 'Consecutive delivery failures before an unresponsive client is skipped (default 3)' },
  { key: 'tx_probe', label: 'Probe skipped clients', type: 'bool', saved: false, diag: true,
    help: 'Send a probe now and then while a client is skipped (default on)' },
  { key: 'tx_impl', label: 'Send task', type: 'select', saved: false, diag: true,
    options: [{ value: 1, label: 'per client (default)' }, { value: 0, label: 'single queue' }],
    help: 'For comparison only' },
  { key: 'slow_tx_ms', label: 'Record sends slower than [ms]', type: 'int', min: 0, max: 1000, saved: false, diag: true,
    help: 'Radio sends whose result took this long are kept for "slowtx" (default 10, 0 = off)' },
  { key: 'trace_status', label: 'Trace status byte', type: 'int', min: 0, max: 239, saved: false, diag: true,
    help: 'Status byte of the messages to trace (175 = Poly Aftertouch ch16), 0 = off. Uses about 10 KB while on' },
];

export const LABEL_PATTERN = /^[A-Za-z0-9._-]{0,30}$/;

export const CLIENT_SETTINGS = [
  { key: 'label', label: 'Label', type: 'text', saved: true,
    help: 'Your name for this client, shown in the bridge\'s client list. Up to 30 of A-Z a-z 0-9 - _ . (empty = none)' },
  { key: 'rx_channels', label: 'MIDI channels to listen to', type: 'channels', saved: true,
    help: 'Channel messages from other clients on these channels are delivered to this client' },
  { key: 'noteon_max_age_ms', label: 'Note On max age [ms]', type: 'int', min: 0, max: 2000, saved: true,
    help: 'A Note On that waited longer than this is dropped (client to PC). 0 = never' },
  { key: 'phy_rate', label: 'Radio rate', type: 'select', options: PHY_RATES, saved: true,
    help: 'Rate of the packets this client sends' },
  { key: 'retry_limit', label: 'Radio retry limit', type: 'int', min: 0, max: 31, saved: false,
    help: '0 = the Wi-Fi library value (32, default)' },
  { key: 'send_lock', label: 'Send under driver lock', type: 'bool', saved: false,
    help: 'Default on' },
];

// Value of a setting as shown by "get <key>" -> value for the form, and back
export function toForm(def, v) {
  if (v === undefined) return '';
  if (def.type === 'channels') return typeof v === 'number' ? v : parseInt(String(v), 16);
  return v;
}
export function toCommand(def, v) {
  if (def.type === 'bool') return v ? 1 : 0;
  if (def.type === 'text') return v === '' ? '-' : v;
  if (def.type === 'channels') {
    if (v === 0xFFFF) return 'omni';
    if (v === 0) return 'none';
    return `0x${v.toString(16).padStart(4, '0')}`;
  }
  return v;
}
