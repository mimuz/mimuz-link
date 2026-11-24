/*

 docs.mjs mimuz-link web console: API reference page (the Markdown files of docs/, shown as HTML)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/24 : Start Writing

*/
// Shows the Markdown files of the repository's docs/ folder (served by app.mjs at /docs/), so that the reference read
// on GitHub and the one in the console are the same files. markdown-it is loaded from jsDelivr (fixed version, checked
// with its hash) when the page is first opened. Raw HTML in the Markdown is not rendered.
//
// Address: #docs, #docs/<file>.md, #docs/<file>.md#<heading id>. Heading ids are made as GitHub makes them, so that
// links written for GitHub ("file.md#heading") work here too. Links to other files of the repository open on GitHub.
import { clear, el } from './dom.mjs';

const MARKDOWN_IT = {
  src: 'https://cdn.jsdelivr.net/npm/markdown-it@14.1.0/dist/markdown-it.min.js',
  integrity: 'sha384-wLhprpjsmjc/XYIcF+LpMxd8yS1gss6jhevOp6F6zhiIoFK6AmHtm4bGKtehTani',
};
const REPO_URL = 'https://github.com/mimuz/mimuz-link/blob/main/';
const DOCS = [
  ['README.md', 'Overview'],
  ['js-driver.md', 'Web driver'],
  ['arduino-library.md', 'Arduino library'],
  ['config-commands.md', 'Config commands and SysEx'],
];

let mdLoaded = null;
let md = null;
let shown = '';          // file now shown
const cache = new Map(); // file -> Markdown text

function loadMarkdownIt() {
  mdLoaded ??= new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = MARKDOWN_IT.src;
    s.integrity = MARKDOWN_IT.integrity;
    s.crossOrigin = 'anonymous';
    s.onload = () => resolve(window.markdownit);
    s.onerror = () => reject(new Error('markdown-it could not be loaded (this page needs the internet)'));
    document.head.append(s);
  }).then((markdownit) => {
    md = markdownit({ html: false, linkify: false });
    addHeadingIds(md);
  }).catch((e) => {
    mdLoaded = null;  // try again next time
    throw e;
  });
  return mdLoaded;
}

// GitHub's heading ids: lower case, punctuation removed, each space becomes "-", repeats get "-1", "-2" ...
function slug(text, seen) {
  const base = text.trim().toLowerCase().replace(/[^\p{L}\p{N}\s_-]/gu, '').replace(/\s/g, '-');
  const n = seen.get(base) ?? 0;
  seen.set(base, n + 1);
  return n ? `${base}-${n}` : base;
}

function addHeadingIds(mdi) {
  mdi.core.ruler.push('heading_ids', (state) => {
    const seen = new Map();
    const t = state.tokens;
    for (let i = 0; i < t.length; i++) {
      if (t[i].type !== 'heading_open') continue;
      const text = t[i + 1].children.filter((c) => c.type === 'text' || c.type === 'code_inline').map((c) => c.content).join('');
      t[i].attrSet('id', slug(text, seen));
    }
  });
}

// "#docs/<file>#<id>" -> { file, id }
function route() {
  const h = decodeURIComponent(location.hash.slice(1));
  if (!h.startsWith('docs')) return null;
  const rest = h.slice(5);  // after "docs/"
  const [file, id = ''] = rest.split('#');
  return { file: DOCS.some(([f]) => f === file) ? file : 'README.md', id };
}

// A link of the Markdown, made to work in the console
function fixLink(a, file) {
  const href = a.getAttribute('href') ?? '';
  if (/^[a-z]+:/i.test(href)) {  // http:, https:, mailto:
    a.target = '_blank';
    a.rel = 'noopener';
    return;
  }
  if (href.startsWith('#')) {
    a.href = `#docs/${file}${href}`;
    return;
  }
  const [path, id] = href.split('#');
  const resolved = new URL(path, `https://x/docs/${file}`).pathname.slice(1);  // relative to docs/<file>
  const inDocs = resolved.startsWith('docs/') && DOCS.some(([f]) => `docs/${f}` === resolved);
  if (inDocs) {
    a.href = `#docs/${resolved.slice(5)}${id ? `#${id}` : ''}`;
  } else {
    a.href = REPO_URL + resolved + (id ? `#${id}` : '');
    a.target = '_blank';
    a.rel = 'noopener';
  }
}

async function getText(file) {
  if (cache.has(file)) return cache.get(file);
  const r = await fetch(`docs/${file}`, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`docs/${file}: ${r.status} ${r.statusText}`);
  const text = await r.text();
  cache.set(file, text);
  return text;
}

function scrollTo(id) {
  const target = id ? document.getElementById(id) : null;
  if (target) target.scrollIntoView({ block: 'start' });
  else window.scrollTo(0, 0);
}

async function show() {
  const r = route();
  if (!r) return;
  const root = document.getElementById('docs');
  if (r.file === shown && root.querySelector('.doc')) {  // same page: only move to the heading
    scrollTo(r.id);
    return;
  }
  const status = el('p', { class: 'muted' }, 'Loading…');
  clear(root).append(status);
  try {
    const [text] = await Promise.all([getText(r.file), loadMarkdownIt()]);
    if (route()?.file !== r.file) return;  // another page was chosen meanwhile
    const doc = el('article', { class: 'doc' });
    doc.innerHTML = md.render(text);  // html: false, so the Markdown cannot add markup of its own
    for (const a of doc.querySelectorAll('a[href]')) fixLink(a, r.file);
    for (const t of doc.querySelectorAll('table')) t.replaceWith(el('div', { class: 'doc-table' }, t.cloneNode(true)));
    const toc = [...doc.querySelectorAll('h2, h3')].map((h) =>
      el('li', { class: h.tagName === 'H3' ? 'sub' : '' }, el('a', { href: `#docs/${r.file}#${h.id}` }, h.textContent)));
    clear(root).append(el('div', { class: 'layout' },
      el('aside', {},
        el('h2', {}, 'API reference'),
        el('ul', { class: 'doc-list' }, DOCS.map(([f, title]) =>
          el('li', { class: f === r.file ? 'sel' : '' }, el('a', { href: `#docs/${f}` }, title)))),
        toc.length ? el('h3', {}, 'On this page') : null,
        el('ul', { class: 'doc-toc' }, toc),
        el('p', { class: 'muted small' }, 'The same files as docs/ in the repository. ',
          el('a', { href: `${REPO_URL}docs/${r.file}`, target: '_blank', rel: 'noopener' }, 'Open on GitHub'))),
      el('main', {}, doc)));
    shown = r.file;
    scrollTo(r.id);
  } catch (e) {
    status.textContent = e.message;
    status.className = 'status err';
  }
}

window.addEventListener('hashchange', show);
show();
