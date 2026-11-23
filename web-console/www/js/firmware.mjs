/*

 firmware.mjs mimuz-link web console: firmware uploader page and page switching

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/22 : Start Writing
 - 2026/11/22 : Load ESP Web Tools from unpkg (fixed version)
 - 2026/11/23 : Say that the bridge needs about 15 s after writing

*/
// Writes the bridge and client firmware built by firmware/build.sh (www/firmware/<name>/manifest.json) with
// ESP Web Tools (loaded from unpkg, version fixed in ESP_WEB_TOOLS). Only the bootloader, the partition table,
// boot_app0 and the application are written, so the settings in NVS (pairing, label, ...) are kept.
//
// The bridge runs its own USB (MIDI and serial), so it has to be restarted into the ROM's download mode first: opening
// its serial port at 1200 baud and closing it again does that (as "arduino-cli upload" does). It then appears as a new
// port ("USB JTAG/serial debug unit"), which is the one to choose for writing. The client's USB is the ESP32-C6's
// built-in USB serial / JTAG, which ESP Web Tools can reset into download mode by itself.
import { el, clear, status, busy } from './dom.mjs';

const FIRMWARE = [
  {
    name: 'bridge', title: 'Bridge', board: 'XIAO ESP32-S3',
    steps: [
      'Disconnect the bridge in the Console page (and close the PC tools that use it).',
      'Press "Restart into update mode" and choose the bridge\'s serial port. The bridge restarts and shows up again as a new port.',
      'Press "Write firmware" and choose the new port (USB JTAG/serial debug unit).',
      'When it is done, the bridge restarts with the new firmware and is ready after about 15 s (it surveys the channels first). Its registrations and settings are kept; the channel may change.',
    ],
    updateMode: true,
  },
  {
    name: 'client', title: 'Client (test device firmware)', board: 'XIAO ESP32-C6',
    steps: [
      'Attach the client to this computer over USB (a client on battery has to be plugged in for this).',
      'Disconnect it in the Console page if it is open there.',
      'Press "Write firmware" and choose its port (USB JTAG/serial debug unit).',
      'When it is done, the client restarts with the new firmware. Its pairing, label and settings are kept.',
    ],
    updateMode: false,
  },
];

const ESP_WEB_TOOLS = 'https://unpkg.com/esp-web-tools@10.5.2/dist/web/install-button.js';

let webToolsLoaded = null;
function loadWebTools() {
  webToolsLoaded ??= import(ESP_WEB_TOOLS).catch((e) => {
    webToolsLoaded = null;
    throw new Error(`could not load ESP Web Tools from unpkg (${e.message}); the firmware uploader needs the internet`);
  });
  return webToolsLoaded;
}

async function renderFirmware() {
  const box = clear(document.getElementById('firmware'));
  box.append(el('p', { class: 'muted' },
    'Writes the latest firmware built from this repository (web-console/firmware/build.sh). Chrome or Edge is needed (Web Serial). Settings stored on the device are kept.'));
  const st = status();
  try {
    await loadWebTools();
  } catch (e) {
    st.set(e.message, 'err');
    box.append(st);
  }
  const cards = el('div', { class: 'fw-cards' });
  box.append(cards);
  for (const fw of FIRMWARE) cards.append(await card(fw));
}

async function card(fw) {
  const url = `firmware/${fw.name}/manifest.json`;
  let m = null;
  try {
    const r = await fetch(url, { cache: 'no-cache' });
    if (r.ok) m = await r.json();
  } catch { /* not built */ }
  const body = el('div', { class: 'fw-card' },
    el('h2', {}, fw.title, el('span', { class: 'muted small' }, ` · ${fw.board}`)));
  if (!m) {
    body.append(el('p', { class: 'notice' }, 'No firmware built yet. Run web-console/firmware/build.sh, then reload this page.'));
    return body;
  }
  body.append(el('dl', { class: 'kv' },
    el('dt', {}, 'version'), el('dd', {}, m.version ?? '?'),
    el('dt', {}, 'source'), el('dd', {}, m.rev ?? '?'),
    el('dt', {}, 'built'), el('dd', {}, m.built ? new Date(m.built).toLocaleString() : '?'),
    el('dt', {}, 'application'), el('dd', {}, m.app_size ? `${Math.round(m.app_size / 1024)} KB` : '?')));
  body.append(el('ol', { class: 'fw-steps' }, fw.steps.map((s) => el('li', {}, s))));
  const st = status();
  const row = el('div', { class: 'row wrap' });
  if (fw.updateMode) {
    const b = el('button', { class: 'warn' }, 'Restart into update mode');
    b.onclick = () => busy(b, st, async () => {
      const port = await navigator.serial.requestPort();
      await port.open({ baudRate: 1200 });  // the "1200 baud touch": the bridge restarts into download mode
      await port.close().catch(() => {});
      return 'restarting into update mode; now press "Write firmware" and choose the new port';
    });
    row.append(b);
  }
  const install = document.createElement('esp-web-install-button');
  install.setAttribute('manifest', url);
  install.append(
    el('button', { slot: 'activate', class: 'primary' }, 'Write firmware'),
    el('span', { slot: 'unsupported', class: 'notice' }, 'This browser cannot write firmware (Web Serial is needed: Chrome or Edge).'),
    el('span', { slot: 'not-allowed', class: 'notice' }, 'Open the page from http://localhost to write firmware.'));
  row.append(install, st);
  body.append(row);
  return body;
}

// ---- pages ----

let firmwareShown = false;
function showPage() {
  const page = location.hash === '#firmware' ? 'firmware' : 'console';
  for (const p of document.querySelectorAll('.page')) p.hidden = p.id !== `page-${page}`;
  for (const t of document.querySelectorAll('.page-tab')) t.classList.toggle('sel', t.dataset.page === page);
  if (page === 'firmware' && !firmwareShown) {
    firmwareShown = true;
    renderFirmware();
  }
}
window.addEventListener('hashchange', showPage);
showPage();
