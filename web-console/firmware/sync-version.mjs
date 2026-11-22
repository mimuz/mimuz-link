/*

 sync-version.mjs mimuz-link: copy the version in package.json to the firmware and the web console

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/22 : Start Writing

*/
// The version is kept in the top package.json and in the git tag v<version>; "npm version <new>" changes package.json,
// runs this script (the "version" script), and then commits and tags. This script writes the same version into:
//   - libs/mimuz-link/src/mimuz_link_proto.h   MIMUZ_LINK_VERSION (what the firmware reports in "info")
//   - libs/mimuz-link/library.properties       version (Arduino library)
//   - web-console/package.json                 version (and its package-lock.json)
// usage: node web-console/firmware/sync-version.mjs [--check]   (--check: only report differences, exit 1 if any)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const { version } = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
const check = process.argv.includes('--check');

const targets = [
  { file: 'libs/mimuz-link/src/mimuz_link_proto.h', re: /^(#define MIMUZ_LINK_VERSION ")([^"]*)(")/m },
  { file: 'libs/mimuz-link/library.properties', re: /^(version=)(.*)()$/m },
  { file: 'web-console/package.json', re: /^(  "version": ")([^"]*)(")/m },
];

let differ = 0;
for (const t of targets) {
  const p = path.join(root, t.file);
  const s = fs.readFileSync(p, 'utf8');
  const m = t.re.exec(s);
  if (!m) throw new Error(`no version found in ${t.file}`);
  if (m[2] === version) continue;
  differ++;
  if (check) {
    console.error(`${t.file}: ${m[2]} (package.json: ${version})`);
  } else {
    fs.writeFileSync(p, s.replace(t.re, `$1${version}$3`));
    console.log(`${t.file}: ${m[2]} -> ${version}`);
  }
}
// web-console/package-lock.json follows web-console/package.json
const lockPath = path.join(root, 'web-console/package-lock.json');
if (fs.existsSync(lockPath)) {
  const lock = JSON.parse(fs.readFileSync(lockPath, 'utf8'));
  if (lock.version !== version || lock.packages?.['']?.version !== version) {
    differ++;
    if (check) {
      console.error(`web-console/package-lock.json: ${lock.version} (package.json: ${version})`);
    } else {
      lock.version = version;
      if (lock.packages?.['']) lock.packages[''].version = version;
      fs.writeFileSync(lockPath, `${JSON.stringify(lock, null, 2)}\n`);
      console.log(`web-console/package-lock.json -> ${version}`);
    }
  }
}
if (check && differ) process.exit(1);
