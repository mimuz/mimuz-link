/*

 pages.mjs mimuz-link web: the top pages (Overview, Tutorial, API reference, Console, Firmware uploader)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/24 : Start Writing (page switching moved here from firmware.mjs)

*/
// One page is shown at a time, chosen by the address: #overview (also the empty address), #tutorial..., #docs...,
// #console, #firmware. The Markdown pages take a file and a heading after the page name (#docs/js-driver.md#client).
// A page's module can ask to be told when the page is first shown (whenShown), so that the console does not ask for
// MIDI, and the uploader does not load ESP Web Tools, while the visitor only reads.
export const PAGES = ['overview', 'tutorial', 'docs', 'console', 'firmware'];

const waiting = new Map();  // page -> callbacks run when it is first shown
const seen = new Set();

export function currentPage() {
  const name = location.hash.slice(1).split(/[/#]/)[0];
  return PAGES.includes(name) ? name : 'overview';
}

export function whenShown(page, fn) {
  if (seen.has(page)) {
    fn();
    return;
  }
  if (!waiting.has(page)) waiting.set(page, []);
  waiting.get(page).push(fn);
}

function showPage() {
  const page = currentPage();
  for (const p of document.querySelectorAll('.page')) p.hidden = p.id !== `page-${page}`;
  for (const t of document.querySelectorAll('.page-tab')) t.classList.toggle('sel', t.dataset.page === page);
  if (seen.has(page)) return;
  seen.add(page);
  for (const fn of waiting.get(page) ?? []) fn();
  waiting.delete(page);
}

window.addEventListener('hashchange', showPage);
// After the other modules have registered (they are imported before this runs its first showPage)
queueMicrotask(showPage);
