/*

 app.mjs mimuz-link web console: static file server

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/22 : Start Writing
 - 2026/11/22 : Serve ESP Web Tools from node_modules for the firmware uploader
 - 2026/11/22 : ESP Web Tools comes from unpkg again (the page is hosted with git pull and npm start only)
 - 2026/11/24 : Serve the API reference (../docs, Markdown) at /docs

*/
// Serves ./www (the page and the firmware it writes, both kept in git) and ../docs (the API reference), so the site is hosted with "git pull" and
// "npm start". The firmware uploader loads ESP Web Tools from unpkg. The page talks to the devices itself through
// Web Serial, so the server only hosts files.
// Web Serial needs a secure context; http://localhost counts as one, so the server listens on the loopback address.
//
// usage: node app.mjs            (http://localhost:8080)
//        PORT=9000 node app.mjs
import express from 'express';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const dir = path.dirname(fileURLToPath(import.meta.url));
const port = Number(process.env.PORT) || 8080;
const host = process.env.HOST || '127.0.0.1';

const app = express();
const setHeaders = (res, file) => {
  if (file.endsWith('.mjs')) res.setHeader('Content-Type', 'text/javascript; charset=utf-8');
  if (file.endsWith('manifest.json')) res.setHeader('Cache-Control', 'no-cache');  // a rebuilt firmware shows at once
};
app.use(express.static(path.join(dir, 'www'), { setHeaders }));
// The API reference: the Markdown files in the repository's docs/ (also read on GitHub), shown by www/js/docs.mjs
app.use('/docs', express.static(path.join(dir, '..', 'docs'), {
  setHeaders: (res, file) => {
    if (file.endsWith('.md')) res.setHeader('Content-Type', 'text/markdown; charset=utf-8');
    res.setHeader('Cache-Control', 'no-cache');
  },
}));

app.listen(port, host, () => {
  console.log(`mimuz-link web console: http://${host === '127.0.0.1' ? 'localhost' : host}:${port}/`);
  console.log('Open it in a browser with Web Serial (Chrome or Edge).');
});
