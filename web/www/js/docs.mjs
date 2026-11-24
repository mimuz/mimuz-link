/*

 docs.mjs mimuz-link web: the Markdown pages (Overview, Tutorial, API reference), from the files of docs/

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/24 : Start Writing
 - 2026/11/24 : Syntax highlighting of the code examples (highlight.js)
 - 2026/11/24 : Three pages of Markdown: Overview, Tutorial and API reference

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
// Repository files that stand for the pages of this site that are not Markdown
const SITE_LINKS = {
  'web/README.md': '#console',
  'web/README.md#firmware-uploader': '#firmware',
};

const setOf = (file) => Object.keys(SETS).find((k) => SETS[k].files.some(([f]) => f === file));

let mdLoaded = null;
let md = null;
const shown = new Map(); // page -> file now shown
const cache = new Map(); // file -> Markdown text

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

// A link of the Markdown, made to work on this site
function fixLink(a, file) {
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
  const [path, id] = href.split('#');
  const resolved = new URL(path, `https://x/docs/${file}`).pathname.slice(1);  // relative to docs/<file>
  const site = SITE_LINKS[resolved + (id ? `#${id}` : '')] ?? SITE_LINKS[resolved];
  const page = resolved.startsWith('docs/') ? setOf(resolved.slice(5)) : undefined;
  if (site) {
    a.href = site;
  } else if (page) {
    a.href = `#${page}/${resolved.slice(5)}${id ? `#${id}` : ''}`;
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
  const root = document.getElementById(`${r.page}-doc`);
  if (r.file === shown.get(r.page) && root.querySelector('.doc')) {  // same file: only move to the heading
    scrollTo(r.id);
    return;
  }
  const status = el('p', { class: 'muted' }, 'Loading…');
  clear(root).append(status);
  try {
    const [text] = await Promise.all([getText(r.file), loadMarkdownIt()]);
    const now = route();
    if (now?.page !== r.page || now.file !== r.file) return;  // another page was chosen meanwhile
    const doc = el('article', { class: 'doc' });
    doc.innerHTML = md.render(text);  // html: false, so the Markdown cannot add markup of its own
    for (const a of doc.querySelectorAll('a[href]')) fixLink(a, r.file);
    for (const t of doc.querySelectorAll('table')) t.replaceWith(el('div', { class: 'doc-table' }, t.cloneNode(true)));
    const toc = [...doc.querySelectorAll('h2, h3')].map((h) =>
      el('li', { class: h.tagName === 'H3' ? 'sub' : '' }, el('a', { href: `#${r.page}/${r.file}#${h.id}` }, h.textContent)));
    const files = SETS[r.page].files;
    clear(root).append(el('div', { class: 'layout' },
      el('aside', {},
        el('h2', {}, SETS[r.page].title),
        files.length > 1 ? el('ul', { class: 'doc-list' }, files.map(([f, title]) =>
          el('li', { class: f === r.file ? 'sel' : '' }, el('a', { href: `#${r.page}/${f}` }, title)))) : null,
        toc.length ? el('h3', {}, 'On this page') : null,
        el('ul', { class: 'doc-toc' }, toc),
        el('p', { class: 'muted small' }, 'The same file as docs/', r.file, ' in the repository. ',
          el('a', { href: `${REPO_URL}docs/${r.file}`, target: '_blank', rel: 'noopener' }, 'Open on GitHub'))),
      el('main', {}, doc)));
    shown.set(r.page, r.file);
    scrollTo(r.id);
  } catch (e) {
    status.textContent = e.message;
    status.className = 'status err';
  }
}

window.addEventListener('hashchange', show);
show();
