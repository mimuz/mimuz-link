/*

 monitor.mjs mimuz-link web driver in Node.js: bridges, clients and their MIDI

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/23 : Start Writing

*/
// Shows the bridges and clients as they come and go, and the MIDI each client sends, using the same driver as the
// web pages (web/www/drivers/mimuz-link.mjs) on Node.js.
//
//   cd examples/node && npm install
//   node monitor.mjs                 # watch until Ctrl-C
//   node monitor.mjs --ping N1       # also measure the round trip to the client labeled N1 (test firmware echoes
//                                    # Poly Aftertouch on channel 16)
import MimuzLink from '../../web/www/drivers/mimuz-link.mjs';

const args = process.argv.slice(2);
const pingLabel = args[0] === '--ping' ? args[1] : null;

// Pass the module so that it is found in this directory's node_modules
const driver = new MimuzLink({ nodeMidi: await import('@julusian/midi') });
const link = await driver.init();
if (!link) {
  console.error(`MIDI is not available: ${driver.error}`);
  process.exit(1);
}

const time = () => new Date().toTimeString().slice(0, 8);
const hex = (d) => Array.from(d, (b) => b.toString(16).padStart(2, '0')).join(' ');

link.addEventListener('change', () => {
  console.log(`${time()} bridges: ${link.bridges.map((b) => `${b.name} (ch ${b.info?.channel ?? '?'}, fw ${b.info?.fw ?? '?'})`).join(', ') || 'none'}`);
  for (const c of link.clients) {
    const where = c.via === 'espnow' ? `port ${c.port}` : 'BLE-MIDI';
    console.log(`${time()}   ${where}: ${c.label || '(no label)'} model=${c.model || '-'} ${c.name}`);
  }
});
link.addEventListener('midi', (e) => {
  const { client, data } = e.detail;
  if (pingLabel && client.label === pingLabel && data[0] === 0xAF) return;  // ping replies are counted below
  console.log(`${time()} ${client.label || client.name}: ${hex(data)}`);
});

if (pingLabel) {
  const target = link.client({ label: pingLabel });
  const sentAt = new Map();
  const rtt = [];
  target.addEventListener('midi', (e) => {
    const d = e.detail.data;
    const t = sentAt.get(d[1] * 128 + d[2]);
    if (d[0] === 0xAF && t !== undefined) rtt.push(e.detail.timeStamp - t);
  });
  let seq = 0;
  setInterval(() => {
    if (!target.connected) return;
    seq = (seq + 1) % 16384;
    sentAt.set(seq, performance.now());
    target.polyAftertouch(16, seq >> 7, seq & 127);
  }, 20);
  setInterval(() => {
    if (!rtt.length) return;
    const s = rtt.splice(0).sort((a, b) => a - b);
    console.log(`${time()} ping ${pingLabel}: n=${s.length} p50=${s[s.length >> 1].toFixed(2)} p99=${s[Math.floor(s.length * 0.99)].toFixed(2)} max=${s.at(-1).toFixed(2)} ms`);
  }, 5000);
}

// Open MIDI ports keep the process running: close them on Ctrl-C
process.on('SIGINT', () => {
  link.close();
  process.exit(0);
});
