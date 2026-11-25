/*

 docs.mjs mimuz-link web: the Markdown pages (Overview, Tutorial, API reference), from the files of docs/

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/24 : Start Writing
 - 2026/11/24 : Syntax highlighting of the code examples (highlight.js)
 - 2026/11/24 : Three pages of Markdown: Overview, Tutorial and API reference
 - 2026/11/25 : English / Japanese for the Overview and the Tutorial (docs/ja/)

*/
// Shows the Markdown files of the repository's docs/ folder (served by app.mjs at /docs/), so that the pages read
// on GitHub and the ones on this site are the same files. markdown-it (jsDelivr) and highlight.js (cdnjs) are loaded
// when such a page is first opened (fixed versions, checked with their hashes). Code blocks are highlighted by the
// language of their fence (```js, ```cpp, ```sh); without highlight.js they are shown plain. Raw HTML in the Markdown
// is not rendered.
//
// Address: #<page>, #<page>/<file>.md, #<page>/<file>.md#<heading id>, where <page> is overview, tutorial or docs.
// Heading ids are made as GitHub makes them, so that links written for GitHub ("file.md#heading") work here too. A link
// to a file of another page opens that page; links to the web console's README open the Console or the Firmware
// uploader; links to other files of the repository open on GitHub.
//
// The Overview and the Tutorial are also in Japanese (docs/ja/<file>, same file names). The language is chosen with the
// English / 日本語 buttons of the header, shown on those pages only; English is the default, and the choice is kept in
// this browser. The address does not change with the language.
import { clear, el } from './dom.mjs';
import { currentPage } from './pages.mjs';

const MARKDOWN_IT = {
  src: 'https://cdn.jsdelivr.net/npm/markdown-it@14.1.0/dist/markdown-it.min.js',
  integrity: 'sha384-wLhprpjsmjc/XYIcF+LpMxd8yS1gss6jhevOp6F6zhiIoFK6AmHtm4bGKtehTani',
};
const HIGHLIGHT_JS = {
  src: 'https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.11.1/highlight.min.js',
  integrity: 'sha384-RH2xi4eIQ/gjtbs9fUXM68sLSi99C7ZWBRX1vDrVv6GQXRibxXLbwO2NGZB74MbU',
  css: 'https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.11.1/styles/github-dark.min.css',
  cssIntegrity: 'sha384-wH75j6z1lH97ZOpMOInqhgKzFkAInZPPSPlZpYKYTOqsaizPvhQZmAtLcPKXpLyH',
};
const REPO_URL = 'https://github.com/mimuz/mimuz-link/blob/main/';
// The pages and their files (the first one is shown when the page is opened without a file)
const SETS = {
  overview: { title: 'Overview', files: [['overview.md', 'Overview']] },
  tutorial: {
    title: 'Tutorial',
    files: [['tutorial-quick-start.md', '1. Quick start'], ['tutorial-new-client.md', '2. A new client device']],
  },
  docs: {
    title: 'API reference',
    files: [
      ['README.md', 'Contents'],
      ['js-driver.md', 'Web driver'],
      ['arduino-library.md', 'Arduino library'],
      ['config-commands.md', 'Config commands and SysEx'],
    ],
  },
};
// The files that are also in Japanese, and the words of the page around them
const JA_FILES = new Set(['overview.md', 'tutorial-quick-start.md', 'tutorial-new-client.md']);
const JA_WORDS = {
  overview: '概要',
  tutorial: 'チュートリアル',
  'tutorial-quick-start.md': '1. クイックスタート',
  'tutorial-new-client.md': '2. 新規デバイスの作成',
};
const LANG_KEY = 'mimuz-link.lang';
// Repository files that stand for the pages of this site that are not Markdown
const SITE_LINKS = {
  'web/README.md': '#console',
  'web/README.md#firmware-uploader': '#firmware',
};

const setOf = (file) => Object.keys(SETS).find((k) => SETS[k].files.some(([f]) => f === file));
const hasJa = (page) => SETS[page]?.files.some(([f]) => JA_FILES.has(f)) ?? false;

let lang = 'en';
try {
  if (localStorage.getItem(LANG_KEY) === 'ja') lang = 'ja';
} catch {}
// The file read for a file of a page, in the language chosen
const pathOf = (file) => (lang === 'ja' && JA_FILES.has(file) ? `ja/${file}` : file);

let mdLoaded = null;
let md = null;
const shown = new Map(); // page -> path of the file now shown
const cache = new Map(); // path -> Markdown text

function loadScript({ src, integrity }) {
  return new Promise((resolve, reject) => {
    const e = document.createElement('script');
    e.src = src;
    e.integrity = integrity;
    e.crossOrigin = 'anonymous';
    e.onload = resolve;
    e.onerror = () => reject(new Error(`${src} could not be loaded`));
    document.head.append(e);
  });
}

// The colors of the highlighted code (dark, like the code blocks in both themes)
function loadHighlightCss() {
  const e = document.createElement('link');
  e.rel = 'stylesheet';
  e.href = HIGHLIGHT_JS.css;
  e.integrity = HIGHLIGHT_JS.cssIntegrity;
  e.crossOrigin = 'anonymous';
  document.head.append(e);
}

// markdown-it is needed; highlight.js is not (the code is then shown plain)
function loadMarkdownIt() {
  mdLoaded ??= Promise.all([
    loadScript(MARKDOWN_IT).catch(() => { throw new Error('markdown-it could not be loaded (this page needs the internet)'); }),
    loadScript(HIGHLIGHT_JS).then(loadHighlightCss).catch(() => {}),
  ]).then(() => {
    const hljs = window.hljs;
    md = window.markdownit({
      html: false,
      linkify: false,
      highlight: (code, lang) => {
        if (!hljs || !lang || !hljs.getLanguage(lang)) return '';  // markdown-it escapes it and shows it plain
        try {
          return hljs.highlight(code, { language: lang, ignoreIllegals: true }).value;
        } catch {
          return '';
        }
      },
    });
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

// "#<page>/<file>#<id>" -> { page, file, id }
function route() {
  const page = currentPage();
  if (!SETS[page]) return null;
  const h = decodeURIComponent(location.hash.slice(1));
  const rest = h.startsWith(`${page}/`) ? h.slice(page.length + 1) : '';
  const [file, id = ''] = rest.split('#');
  const files = SETS[page].files;
  return { page, file: files.some(([f]) => f === file) ? file : files[0][0], id };
}

// A link of the Markdown (in the file read from docs/<path>), made to work on this site
function fixLink(a, file, path) {
  const href = a.getAttribute('href') ?? '';
  if (/^[a-z]+:/i.test(href)) {  // http:, https:, mailto:
    a.target = '_blank';
    a.rel = 'noopener';
    return;
  }
  if (href.startsWith('#')) {
    a.href = `#${setOf(file)}/${file}${href}`;
    return;
  }
  const [to, id] = href.split('#');
  const resolved = new URL(to, `https://x/docs/${path}`).pathname.slice(1);  // relative to docs/<path>
  const site = SITE_LINKS[resolved + (id ? `#${id}` : '')] ?? SITE_LINKS[resolved];
  // A file of a page: docs/<file>, or its translation docs/ja/<file>, which opens in the language chosen
  const doc = resolved.startsWith('docs/ja/') ? resolved.slice(8) : resolved.startsWith('docs/') ? resolved.slice(5) : '';
  const page = doc ? setOf(doc) : undefined;
  if (site) {
    a.href = site;
  } else if (page) {
    a.href = `#${page}/${doc}${id ? `#${id}` : ''}`;
    if (resolved.startsWith('docs/ja/')) a.addEventListener('click', () => chooseLang('ja'));  // a link to the Japanese file
  } else {
    a.href = REPO_URL + resolved + (id ? `#${id}` : '');
    a.target = '_blank';
    a.rel = 'noopener';
  }
}

async function getText(path) {
  if (cache.has(path)) return cache.get(path);
  const r = await fetch(`docs/${path}`, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`docs/${path}: ${r.status} ${r.statusText}`);
  const text = await r.text();
  cache.set(path, text);
  return text;
}

// The English / 日本語 buttons of the header: shown on the pages that have Japanese files
function updateLangBar() {
  const bar = document.getElementById('lang');
  if (!bar) return;
  bar.hidden = !hasJa(currentPage());
  for (const b of bar.querySelectorAll('button')) {
    b.classList.toggle('sel', b.dataset.lang === lang);
    b.setAttribute('aria-pressed', String(b.dataset.lang === lang));
  }
}

function chooseLang(l) {
  lang = l;
  try {
    localStorage.setItem(LANG_KEY, l);
  } catch {}
  updateLangBar();
}

function setLang(l) {
  if (l === lang) return;
  chooseLang(l);
  show();
}

function scrollTo(id) {
  const target = id ? document.getElementById(id) : null;
  if (target) target.scrollIntoView({ block: 'start' });
  else window.scrollTo(0, 0);
}

async function show() {
  const r = route();
  if (!r) return;
  const root = document.getElementById(`${r.page}-doc`);
  const path = pathOf(r.file);
  const ja = path.startsWith('ja/');
  if (path === shown.get(r.page) && root.querySelector('.doc')) {  // same file: only move to the heading
    scrollTo(r.id);
    return;
  }
  const status = el('p', { class: 'muted' }, ja ? '読み込み中…' : 'Loading…');
  clear(root).append(status);
  try {
    const [text] = await Promise.all([getText(path), loadMarkdownIt()]);
    const now = route();
    if (now?.page !== r.page || now.file !== r.file || pathOf(now.file) !== path) return;  // another one was chosen meanwhile
    const doc = el('article', { class: 'doc', lang: ja ? 'ja' : 'en' });
    doc.innerHTML = md.render(text);  // html: false, so the Markdown cannot add markup of its own
    for (const a of doc.querySelectorAll('a[href]')) fixLink(a, r.file, path);
    for (const t of doc.querySelectorAll('table')) t.replaceWith(el('div', { class: 'doc-table' }, t.cloneNode(true)));
    const toc = [...doc.querySelectorAll('h2, h3')].map((h) =>
      el('li', { class: h.tagName === 'H3' ? 'sub' : '' }, el('a', { href: `#${r.page}/${r.file}#${h.id}` }, h.textContent)));
    const files = SETS[r.page].files;
    const word = (key, english) => (ja && JA_WORDS[key]) || english;
    const github = el('a', { href: `${REPO_URL}docs/${path}`, target: '_blank', rel: 'noopener' }, ja ? 'GitHub で開く' : 'Open on GitHub');
    clear(root).append(el('div', { class: 'layout' },
      el('aside', { lang: ja ? 'ja' : 'en' },
        el('h2', {}, word(r.page, SETS[r.page].title)),
        files.length > 1 ? el('ul', { class: 'doc-list' }, files.map(([f, title]) =>
          el('li', { class: f === r.file ? 'sel' : '' }, el('a', { href: `#${r.page}/${f}` }, word(f, title))))) : null,
        toc.length ? el('h3', {}, ja ? 'このページの内容' : 'On this page') : null,
        el('ul', { class: 'doc-toc' }, toc),
        ja ? el('p', { class: 'muted small' }, 'リポジトリの docs/', path, ' と同じファイルです。', github)
          : el('p', { class: 'muted small' }, 'The same file as docs/', path, ' in the repository. ', github)),
      el('main', {}, doc)));
    shown.set(r.page, path);
    scrollTo(r.id);
  } catch (e) {
    status.textContent = e.message;
    status.className = 'status err';
  }
}

for (const b of document.querySelectorAll('#lang button')) b.addEventListener('click', () => setLang(b.dataset.lang));
window.addEventListener('hashchange', () => {
  updateLangBar();
  show();
});
updateLangBar();
show();
