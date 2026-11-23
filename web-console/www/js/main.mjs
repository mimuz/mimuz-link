/*

 main.mjs mimuz-link web console: user interface

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/22 : Start Writing
 - 2026/11/22 : Remember the devices seen on each USB port type and show them before connecting
 - 2026/11/22 : Name ports that are not open yet from the devices Web MIDI lists
 - 2026/11/22 : Tell bridge and client ports apart by their USB IDs, and devices by the fixed name prefixes
 - 2026/11/22 : List only the ports that are plugged in; no separate list of the devices on MIDI
 - 2026/11/22 : No names remembered per USB ID; a closed bridge port is named only from Web MIDI
 - 2026/11/22 : Keep no device history: ports allowed in earlier visits are forgotten, unplugged ones leave the list
 - 2026/11/22 : Remove the names from Web MIDI (they cannot be matched to ports once several bridges are attached)
 - 2026/11/23 : Show that a device is still starting

*/
// Lists the serial ports this page may use (several bridges and clients can be attached), connects to them and sends
// config commands. Clients reached over the radio are configured through their bridge ("port <n> ...").
// Only one program can have a serial port open at a time, so close the port here before using the PC tools
// (soak_test.py, mlink_config.py, ...) on the same device, and the other way round.
import { Device, RelayTarget } from './device.mjs';
import { BRIDGE_SETTINGS, CLIENT_SETTINGS, LABEL_PATTERN, PHY_RATES, kindOfUsb, toCommand, toForm } from './settings.mjs';
import { busy, clear, el, fmtTime, status, table } from './dom.mjs';

const LOG_MAX = 2000;
const state = {
  devices: [],
  selected: null,
  tab: new Map(),    // device -> tab name
  logs: new Map(),   // device -> [{ dir, text, time }]
  timer: null,       // auto refresh of the clients list
};

const $ = (id) => document.getElementById(id);
const pref = {
  get(k, d) { try { const v = localStorage.getItem(`mlc.${k}`); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem(`mlc.${k}`, JSON.stringify(v)); } catch { /* storage unavailable */ } },
};

// ---------------- devices ----------------

function removeDevice(d) {
  state.devices = state.devices.filter((x) => x !== d);
  state.logs.delete(d);
  state.tab.delete(d);
  if (state.selected === d) state.selected = null;
  renderList();
  renderDetail();
}

function addPort(port) {
  let d = state.devices.find((x) => x.port === port);
  if (d) return d;
  d = new Device(port);
  state.logs.set(d, []);
  d.addEventListener('line', (e) => {
    const l = state.logs.get(d);
    l.push(e.detail);
    if (l.length > LOG_MAX) l.splice(0, l.length - LOG_MAX);
    if (state.selected === d && currentTab(d) === 'console') appendLog(e.detail);
  });
  d.addEventListener('state', () => {
    renderList();
    if (state.selected === d) renderDetail();
  });
  d.addEventListener('busy', renderList);
  state.devices.push(d);
  renderList();
  return d;
}

async function connect(d) {
  try {
    await d.connect();
  } catch (e) {
    d.log('sys', `could not open: ${e.message} (is another program using the port?)`);
    alert(`Could not open the port: ${e.message}\nClose other programs that use it (PC tools, serial monitors).`);
  }
}

async function init() {
  if (!('serial' in navigator)) {
    $('unsupported').hidden = false;
    $('add').disabled = true;
    return;
  }
  $('add').addEventListener('click', async () => {
    try {
      const port = await navigator.serial.requestPort();
      const d = addPort(port);
      select(d);
      await connect(d);
    } catch { /* the chooser was closed */ }
  });
  try { localStorage.removeItem('mlc.known'); } catch { /* storage unavailable */ }  // names once kept per USB ID
  try { localStorage.removeItem('mlc.usemidi'); } catch { /* storage unavailable */ }
  try { localStorage.removeItem('mlc.autoconnect'); } catch { /* storage unavailable */ }
  // No device history: the page lists only the ports added in this visit. Ports the browser still allows from
  // earlier visits are forgotten (Chrome 103 and later), and a port that is unplugged leaves the list and is forgotten.
  for (const p of await navigator.serial.getPorts()) await p.forget?.().catch(() => {});
  navigator.serial.addEventListener('disconnect', (e) => {
    const d = state.devices.find((x) => x.port === e.target);
    if (!d) return;
    d.gone();
    removeDevice(d);
    e.target.forget?.().catch(() => {});
  });
  renderList();
  renderDetail();
}

// ---------------- device list ----------------

function renderList() {
  const ul = clear($('devices'));
  const shown = state.devices.filter((d) => d.attached || d.open);
  if (state.selected && !shown.includes(state.selected)) {
    state.selected = null;
    renderDetail();
  }
  if (!shown.length) {
    ul.append(el('li', { class: 'muted' }, 'No devices yet. Use "Load device" and choose the serial port of a bridge or client.'));
    return;
  }
  for (const d of shown) {
    const i = d.info ?? {};
    // Before it is opened, a client's USB port is shown as a client from its USB IDs (the ESP32-C6's built-in USB)
    // While it is still starting (no reply to "info" yet), the kind is taken from its USB IDs
    const waiting = d.open && d.starting && !d.info;
    const usbKind = kindOfUsb(d.usb);
    const kind = d.kind === 'unknown' && ((!d.open && usbKind === 'client') || (waiting && usbKind !== 'unknown')) ? usbKind : d.kind;
    const title = d.open && i.label ? `${i.label} (${d.name})` : d.name;
    const hint = null;
    ul.append(el('li', { class: `dev ${state.selected === d ? 'sel' : ''} ${d.open ? '' : 'closed'}`, onclick: () => select(d) },
      el('div', { class: 'dev-head' },
        el('span', { class: `badge ${kind}` }, kind),
        el('span', { class: `dot ${d.open ? (d.busy ? 'busy' : 'on') : 'off'}`, title: d.open ? 'connected' : 'not connected' })),
      el('div', { class: 'dev-name' }, title),
      hint,
      waiting ? el('div', { class: 'dev-wait' }, 'Starting… waiting for a reply (a bridge surveys the channels for about 12 s)') : null,
      el('div', { class: 'dev-sub muted' },
        waiting ? '' : d.open ? [i.fw ? `fw ${i.fw}` : '', i.radio_channel ? ` · ch ${i.radio_channel}` : '',
          i.kind === 'bridge' ? ` · ${i.ports_used}/${i.ports} ports` : '',
          i.kind === 'client' ? ` · ${i.transport}${i.port ? ` port ${i.port}` : ''}` : ''].join('') : 'not connected'),
      el('div', { class: 'dev-actions' },
        d.open
          ? el('button', { onclick: (e) => { e.stopPropagation(); d.disconnect(); } }, 'Disconnect')
          : el('button', { class: 'primary', onclick: (e) => { e.stopPropagation(); select(d); connect(d); } }, 'Connect'))));
  }
}

function select(d) {
  state.selected = d;
  renderList();
  renderDetail();
}

// ---------------- detail ----------------

const TABS = {
  bridge: [['overview', 'Overview'], ['clients', 'Clients'], ['radio', 'Radio'], ['settings', 'Settings'],
    ['diag', 'Diagnostics'], ['console', 'Console']],
  client: [['overview', 'Overview'], ['settings', 'Settings'], ['actions', 'Actions'], ['console', 'Console']],
  unknown: [['overview', 'Overview'], ['console', 'Console']],
};

function currentTab(d) {
  const tabs = TABS[d.kind] ?? TABS.unknown;
  const t = state.tab.get(d);
  return tabs.some(([k]) => k === t) ? t : 'overview';
}

function renderDetail() {
  stopAuto();
  const main = clear($('detail'));
  const d = state.selected;
  if (!d) {
    main.append(el('p', { class: 'muted' }, 'Select a device on the left.'));
    return;
  }
  const tab = currentTab(d);
  main.append(el('nav', { class: 'tabs' }, (TABS[d.kind] ?? TABS.unknown).map(([k, label]) =>
    el('button', { class: k === tab ? 'tab sel' : 'tab', onclick: () => { state.tab.set(d, k); renderDetail(); } }, label))));
  const body = el('section', { class: 'tab-body' });
  main.append(body);
  if (!d.open && tab !== 'console') {
    body.append(el('p', {}, 'Not connected. ', el('button', { class: 'primary', onclick: () => connect(d) }, 'Connect')));
    return;
  }
  const views = { overview, clients, radio, settings, diag, actions, console: consoleView };
  views[tab](d, body);
}

// ---- overview ----

function overview(d, body) {
  const st = status();
  const box = el('div');
  const show = () => {
    clear(box).append(d.starting && !d.info
      ? el('p', { class: 'dev-wait' }, 'Starting… waiting for a reply. A bridge surveys the channels for about 12 s after it starts (for example right after a firmware update).')
      : kv(d.info ?? {}));
  };
  show();
  const btn = el('button', {}, 'Refresh');
  btn.onclick = () => busy(btn, st, async () => {
    const r = await d.command('info');
    if (!r.ok) throw new Error(r.error);
    d.info = r.fields;
    show();
    renderList();
    return 'updated';
  });
  body.append(el('div', { class: 'row' }, btn, st), box);
  if (d.kind === 'client') {
    body.append(el('p', { class: 'muted' },
      'This client is attached over USB. A client that is only on the radio can be reached from its bridge (Clients tab).'));
  }
}

function kv(obj) {
  const entries = Object.entries(obj);
  if (!entries.length) return el('p', { class: 'muted' }, 'no information');
  return el('dl', { class: 'kv' }, entries.map(([k, v]) => [el('dt', {}, k), el('dd', {}, String(v))]));
}

// ---- clients (bridge) ----

function clients(d, body) {
  const st = status();
  const portsBox = el('div');
  const regBox = el('div');
  const refresh = async () => {
    const r = await d.command('ports');
    if (!r.ok) throw new Error(r.error);
    clear(portsBox).append(table([
      { key: 'port', label: 'Port' },
      { key: 'label', label: 'Label', fmt: (v) => v || el('span', { class: 'muted' }, '—') },
      { key: 'name', label: 'Device name', class: 'small', fmt: (v) => v || el('span', { class: 'muted' }, '—') },
      { key: 'mac', label: 'MAC' },
      { key: 'rssi', label: 'RSSI [dBm]', fmt: (v) => el('span', { class: rssiClass(v) }, v) },
      { key: 'tx_fail', label: 'Undelivered' },
      { key: 'idle_ms', label: 'Last heard [ms]' },
      { key: 'rx_channels', label: 'Listens to', fmt: (v) => channelsText(parseInt(String(v), 16)) },
      { key: 'phy_rate', label: 'Rate', fmt: (v) => PHY_RATES.find((p) => p.value === v)?.label.replace(' (default)', '') ?? v },
    ], r.items, { onRow: (row) => openClient(d, row), empty: 'No clients connected.' }));
    const g = await d.command('registry');
    if (!g.ok) throw new Error(g.error);
    clear(regBox).append(table([
      { key: 'label', label: 'Label', fmt: (v) => v || el('span', { class: 'muted' }, '—') },
      { key: 'name', label: 'Device name', class: 'small', fmt: (v) => v || el('span', { class: 'muted' }, '—') },
      { key: 'mac', label: 'MAC' },
      { key: 'port', label: 'Port', fmt: (v) => (v > 0 ? v : el('span', { class: 'muted' }, 'not connected')) },
      { key: 'mac', label: '', class: 'right', fmt: (mac) => el('button', { class: 'danger small', onclick: (e) => { e.stopPropagation(); unregister(d, mac, st, refresh); } }, 'Unregister') },
    ], g.items, { empty: 'No registered clients.' }));
    return `${r.items.length} connected, ${g.items.length} registered (${fmtTime(new Date())})`;
  };
  const btn = el('button', {}, 'Refresh');
  btn.onclick = () => busy(btn, st, refresh);
  const ask = el('button', { title: 'Ask every connected client to send its label again' }, 'Ask labels');
  ask.onclick = () => busy(ask, st, async () => {
    const r = await d.command('label_request all');
    if (!r.ok) throw new Error(r.error);
    await new Promise((res) => setTimeout(res, 500));
    await refresh();
    return `asked ${r.fields.asked}`;
  });
  const auto = el('input', { type: 'checkbox', checked: pref.get('autorefresh', false) });
  const setAuto = () => {
    pref.set('autorefresh', auto.checked);
    stopAuto();
    if (auto.checked) state.timer = setInterval(() => { if (!d.busy) busy(btn, st, refresh); }, 5000);
  };
  auto.onchange = setAuto;
  body.append(
    el('div', { class: 'row' }, btn, ask, el('label', {}, auto, ' refresh every 5 s'), st),
    el('h3', {}, 'Connected clients'),
    el('p', { class: 'muted' }, 'Click a client to read or change its settings over the radio.'),
    portsBox,
    el('h3', {}, 'Registered clients'),
    el('p', { class: 'muted' }, 'A client must be registered (paired) before it can connect. Pairing: bring the client close to the bridge and start it without a registration. Label and device name are the last ones the client told this bridge.'),
    regBox);
  busy(btn, st, refresh);
  setAuto();
}

async function unregister(d, mac, st, refresh) {
  if (!confirm(`Remove the registration of ${mac}?\nIt has to be paired again (close to the bridge) to connect.`)) return;
  const r = await d.command(`unregister ${mac}`);
  st.set(r.ok ? `unregistered ${mac}` : r.error, r.ok ? 'ok' : 'err');
  await refresh().catch(() => {});
}

function stopAuto() {
  if (state.timer) clearInterval(state.timer);
  state.timer = null;
}

function rssiClass(v) { return v >= -65 ? 'good' : v >= -80 ? 'fair' : 'poor'; }

function channelsText(mask) {
  if (Number.isNaN(mask)) return '';
  if (mask === 0) return 'none';
  if (mask === 0xFFFF) return 'omni';
  const chs = [];
  for (let i = 0; i < 16; i++) if (mask & (1 << i)) chs.push(i + 1);
  return chs.join(',');
}

// Settings and actions of a client reached over the radio, in a dialog
function openClient(bridge, row) {
  const target = new RelayTarget(bridge, row.port);
  const dlg = el('dialog', { class: 'client-dialog' });
  const close = el('button', { onclick: () => dlg.close() }, 'Close');
  const infoBox = el('div');
  const st = status();
  const btn = el('button', {}, 'Read info');
  btn.onclick = () => busy(btn, st, async () => {
    const r = await target.command('info');
    if (!r.ok) throw new Error(r.error);
    clear(infoBox).append(kv(r.fields));
    return 'read';
  });
  dlg.append(
    el('header', { class: 'row' }, el('h2', {}, `Port ${row.port} · ${row.label || row.mac}`), el('span', { class: 'grow' }), close),
    el('p', { class: 'muted' }, [row.name, row.mac].filter(Boolean).join(' · ')),
    el('p', { class: 'muted' }, 'Commands are relayed by the bridge over the radio (resent until a reply arrives, error after 1.5 s).'),
    el('div', { class: 'row' }, btn, st), infoBox,
    el('h3', {}, 'Settings'), settingsForm(target, CLIENT_SETTINGS),
    el('h3', {}, 'Actions'), clientActions(target, () => dlg.close()));
  dlg.addEventListener('close', () => dlg.remove());
  document.body.append(dlg);
  dlg.showModal();
  btn.click();
}

// ---- radio (bridge) ----

function radio(d, body) {
  const st = status();
  const box = el('div');
  const read = async () => {
    const r = await d.command('survey');
    if (!r.ok) throw new Error(r.error);
    const cur = r.fields.radio_channel;
    clear(box).append(
      el('p', {}, 'Current radio channel: ', el('strong', {}, cur ?? '?')),
      table([
        { key: 'ch', label: 'Channel', fmt: (v) => (v === cur ? el('strong', {}, `${v} ◀`) : v) },
        { key: 'busy', label: 'Busy [%]', fmt: (v) => bar(v, 20) },
        { key: 'peak', label: 'Peak [%]' },
        { key: 'score', label: 'Score (lower is better)' },
      ], r.items, { empty: 'No survey results yet.' }));
    return `read (${fmtTime(new Date())})`;
  };
  const rd = el('button', {}, 'Read survey');
  rd.onclick = () => busy(rd, st, read);
  const rs = el('button', { class: 'warn' }, 'Survey again and choose');
  rs.onclick = () => {
    if (!confirm('Survey all channels again and choose one?\nThis takes about 12 s, and the clients move if the channel changes.')) return;
    busy(rs, st, async () => {
      const r = await d.command('resurvey', { timeout: 30000 });
      if (!r.ok) throw new Error(r.error);
      await read();
      return `radio channel ${r.fields.radio_channel}`;
    });
  };
  const sel = el('select', {}, Array.from({ length: 13 }, (_, i) => el('option', { value: i + 1 }, `channel ${i + 1}`)));
  const sw = el('button', { class: 'warn' }, 'Move');
  sw.onclick = () => {
    if (!confirm(`Move to radio channel ${sel.value}?\nThe bridge tells the connected clients first; others search again.`)) return;
    busy(sw, st, async () => {
      const r = await d.command(`switch_channel ${sel.value}`, { timeout: 10000 });
      if (!r.ok) throw new Error(r.error);
      await read();
      return `radio channel ${r.fields.radio_channel}`;
    });
  };
  body.append(el('div', { class: 'row' }, rd, rs, el('span', { class: 'sep' }), sel, sw, st), box);
  busy(rd, st, read);
}

function bar(v, full) {
  const w = Math.min(100, (Number(v) / full) * 100);
  return el('span', { class: 'bar' }, el('span', { class: 'bar-fill', style: `width:${w}%` }), el('span', { class: 'bar-text' }, v));
}

// ---- settings ----

function settings(d, body) {
  body.append(settingsForm(d, d.kind === 'bridge' ? BRIDGE_SETTINGS : CLIENT_SETTINGS));
}

function settingsForm(target, defs) {
  const wrap = el('div', { class: 'settings' });
  const st = status();
  const rows = defs.map((def) => settingRow(target, def));
  const readAll = el('button', {}, 'Read all');
  readAll.onclick = () => busy(readAll, st, async () => {
    for (const r of rows) await r.read();
    return 'read';
  });
  const normal = rows.filter((r) => !r.def.diag).map((r) => r.node);
  const diag = rows.filter((r) => r.def.diag).map((r) => r.node);
  wrap.append(el('div', { class: 'row' }, readAll, st),
    el('p', { class: 'muted' }, '● saved (kept after a restart) · ○ until the next restart'),
    el('div', { class: 'grid' }, normal));
  if (diag.length) wrap.append(el('details', {}, el('summary', {}, 'Diagnostic settings'), el('div', { class: 'grid' }, diag)));
  readAll.click();
  return wrap;
}

function settingRow(target, def) {
  const st = status();
  let input;
  let getValue;
  let setValue;
  if (def.type === 'bool') {
    input = el('input', { type: 'checkbox' });
    getValue = () => input.checked;
    setValue = (v) => { input.checked = Number(v) !== 0; };
  } else if (def.type === 'select') {
    input = el('select', {}, def.options.map((o) => el('option', { value: o.value }, o.label)));
    getValue = () => Number(input.value);
    setValue = (v) => { input.value = String(v); };
  } else if (def.type === 'channels') {
    const boxes = Array.from({ length: 16 }, (_, i) => el('input', { type: 'checkbox', title: `channel ${i + 1}` }));
    const all = el('button', { class: 'small', onclick: () => boxes.forEach((b) => { b.checked = true; }) }, 'omni');
    const none = el('button', { class: 'small', onclick: () => boxes.forEach((b) => { b.checked = false; }) }, 'none');
    input = el('div', { class: 'channels' }, boxes.map((b, i) => el('label', {}, b, i + 1)), all, none);
    getValue = () => boxes.reduce((m, b, i) => (b.checked ? m | (1 << i) : m), 0);
    setValue = (v) => boxes.forEach((b, i) => { b.checked = !!(v & (1 << i)); });
  } else if (def.type === 'text') {
    input = el('input', { type: 'text', maxlength: 30, spellcheck: false, placeholder: 'none' });
    getValue = () => input.value.trim();
    setValue = (v) => { input.value = String(v ?? ''); };
  } else {
    input = el('input', { type: 'number', min: def.min, max: def.max, step: 1 });
    getValue = () => Number(input.value);
    setValue = (v) => { input.value = String(v); };
  }
  const read = async () => {
    const r = await target.command(`get ${def.key}`);
    if (!r.ok) { st.set(r.error, 'err'); return; }
    setValue(toForm(def, r.fields[def.key]));
    const extra = Object.entries(r.fields).filter(([k]) => k !== def.key).map(([k, v]) => `${k}=${v}`).join(' ');
    st.set(extra, 'muted');
  };
  const apply = el('button', { class: 'primary small' }, 'Set');
  apply.onclick = () => busy(apply, st, async () => {
    const v = getValue();
    if (def.type === 'int' && (!Number.isInteger(v) || v < def.min || v > def.max)) throw new Error(`${def.min}–${def.max}`);
    if (def.type === 'text' && !LABEL_PATTERN.test(v)) throw new Error('up to 30 of A-Z a-z 0-9 - _ .');
    const r = await target.command(`set ${def.key} ${toCommand(def, v)}`);
    if (!r.ok) throw new Error(r.error);
    setValue(toForm(def, r.fields[def.key]));
    return 'set';
  });
  const node = el('div', { class: 'setting' },
    el('div', { class: 'setting-label' }, el('span', { title: def.saved ? 'saved' : 'until the next restart' }, def.saved ? '● ' : '○ '),
      def.label, el('code', {}, def.key)),
    el('div', { class: 'setting-input' }, input, apply),
    el('div', { class: 'setting-help muted' }, def.help), st);
  return { def, node, read };
}

// ---- actions (client) ----

function actions(d, body) {
  body.append(clientActions(d));
}

function clientActions(target, done) {
  const st = status();
  const act = (label, cmd, question, cls = 'warn') => {
    const b = el('button', { class: cls }, label);
    b.onclick = () => {
      if (question && !confirm(question)) return;
      busy(b, st, async () => {
        const r = await target.command(cmd);
        if (!r.ok) throw new Error(r.error);
        if (done) done();
        return r.text || 'ok';
      });
    };
    return b;
  };
  return el('div', {},
    el('div', { class: 'row wrap' },
      act('Restart', 'restart auto', 'Restart the client? It looks for its bridge, or uses BLE-MIDI if there is none.'),
      act('Restart in BLE-MIDI', 'restart ble', 'Restart the client fixed to BLE-MIDI? It leaves ESP-NOW until it is restarted in automatic mode.'),
      act('Restart radio', 'radio_restart', 'Restart ESP-NOW on the client (diagnostic)?', ''),
      act('Unpair', 'unpair', 'Erase the client\'s registration and restart it?\nIt pairs again only with a bridge close by. The bridge keeps its own registration.', 'danger'),
      act('Reset settings', 'reset', 'Reset the client\'s settings to the defaults?', 'danger')),
    st);
}

// ---- diagnostics (bridge) ----

function diag(d, body) {
  const st = status();
  const out = el('div');
  const stats = el('button', {}, 'Statistics');
  stats.onclick = () => busy(stats, st, async () => {
    const lines = await d.raw('s', 1500);
    clear(out).append(el('pre', { class: 'log' }, lines.filter((l) => !l.startsWith('@')).join('\n') || '(no output)'));
    return 'statistics';
  });
  const trace = el('button', {}, 'Trace: slowest');
  trace.onclick = () => busy(trace, st, async () => {
    const r = await d.command('trace');
    if (!r.ok) throw new Error(r.error);
    clear(out).append(kv(r.fields), table([
      { key: 'port', label: 'Port' }, { key: 'key', label: 'Key' },
      { key: 'total_us', label: 'Total [µs]' }, { key: 'radio_us', label: 'Radio [µs]' }, { key: 'in_us', label: 'USB in [µs]' },
    ], r.items, { empty: 'Nothing traced (set trace_status in Settings > Diagnostic settings).' }));
    return 'trace';
  });
  const slow = el('button', {}, 'Late radio sends');
  slow.onclick = () => busy(slow, st, async () => {
    const r = await d.command('slowtx');
    if (!r.ok) throw new Error(r.error);
    clear(out).append(kv(r.fields), table([
      { key: 'port', label: 'Port' }, { key: 'sent_us', label: 'Sent [µs]' },
      { key: 'air_us', label: 'Until result [µs]' }, { key: 'ok', label: 'Delivered' },
      { key: 'resend', label: 'Resend' }, { key: 'timeout', label: 'No result' },
      { key: 'len', label: 'Bytes' }, { key: 'rssi', label: 'RSSI' }, { key: 'nomem', label: 'Refused (total)' },
    ], r.items, { empty: 'No late sends recorded.' }));
    return 'late sends';
  });
  const clearBtn = el('button', {}, 'Clear trace and late sends');
  clearBtn.onclick = () => busy(clearBtn, st, async () => {
    await d.command('trace clear');
    await d.command('slowtx clear');
    clear(out);
    return 'cleared';
  });
  const restart = (label, cmd, q) => {
    const b = el('button', { class: 'danger' }, label);
    b.onclick = () => { if (confirm(q)) busy(b, st, async () => { const r = await d.command(cmd); if (!r.ok) throw new Error(r.error); return r.text; }); };
    return b;
  };
  body.append(
    el('div', { class: 'row wrap' }, stats, trace, slow, clearBtn, st),
    el('details', {}, el('summary', {}, 'Radio restarts (diagnostic)'),
      el('p', { class: 'muted' }, 'Sending stops for a moment. Use only to check the recovery.'),
      el('div', { class: 'row wrap' },
        restart('Restart ESP-NOW', 'radio_restart', 'Restart ESP-NOW on the bridge?'),
        restart('Restart Wi-Fi', 'radio_restart wifi', 'Restart Wi-Fi and ESP-NOW on the bridge?'),
        restart('Reinitialize driver', 'radio_restart driver', 'Reinitialize the Wi-Fi driver on the bridge?'))),
    out);
}

// ---- console ----

function consoleView(d, body) {
  const log = el('pre', { id: 'log', class: 'log tall' });
  const showLog = el('input', { type: 'checkbox', checked: pref.get('showlog', true) });
  showLog.onchange = () => { pref.set('showlog', showLog.checked); fill(); };
  const fill = () => {
    clear(log);
    for (const l of state.logs.get(d)) appendLog(l, log, showLog.checked);
  };
  log.showLog = () => showLog.checked;
  const input = el('input', { type: 'text', placeholder: d.kind === 'bridge' ? 'e.g. ports   get retry_limit   port 2 info' : 'e.g. info   get rx_channels', class: 'grow' });
  const history = pref.get('history', []);
  let hi = history.length;
  const send = async () => {
    const text = input.value.trim();
    if (!text) return;
    history.push(text);
    if (history.length > 50) history.shift();
    pref.set('history', history);
    hi = history.length;
    input.value = '';
    const r = await d.command(text, { timeout: 15000 }).catch((e) => ({ ok: false, error: e.message }));
    if (!r.ok) d.log('sys', `→ ${r.error}`);
  };
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') send();
    else if (e.key === 'ArrowUp' && hi > 0) { input.value = history[--hi]; e.preventDefault(); }
    else if (e.key === 'ArrowDown') { hi = Math.min(history.length, hi + 1); input.value = history[hi] ?? ''; e.preventDefault(); }
  });
  body.append(
    el('div', { class: 'row' }, el('label', {}, showLog, ' show device log lines'),
      el('button', { onclick: () => { state.logs.set(d, []); fill(); } }, 'Clear')),
    log,
    el('div', { class: 'row' }, el('code', {}, '@id'), input, el('button', { class: 'primary', onclick: send, disabled: !d.open }, 'Send')),
    el('p', { class: 'muted' }, 'The id is added automatically. Replies are matched to the command by it.'));
  fill();
  input.focus();
}

function appendLog(l, log = document.getElementById('log'), showLog = log?.showLog?.() ?? true) {
  if (!log) return;
  const isReply = l.text.startsWith('@');
  if (l.dir === 'in' && !isReply && !showLog) return;
  const line = el('div', { class: `ll ${l.dir} ${isReply ? 'reply' : ''}` }, el('span', { class: 'lt' }, fmtTime(l.time)),
    l.dir === 'out' ? '→ ' : l.dir === 'sys' ? '· ' : '', l.text);
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
  log.append(line);
  while (log.childNodes.length > LOG_MAX) log.firstChild.remove();
  if (atBottom) log.scrollTop = log.scrollHeight;
}

init();
