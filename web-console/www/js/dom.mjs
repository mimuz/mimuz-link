/*

 dom.mjs mimuz-link web console: small DOM helpers

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/22 : Start Writing

*/

// el('button', { class: 'x', onclick: f }, 'text', child, ...) -> element. Strings become text nodes (never HTML).
export function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs ?? {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith('on') && typeof v === 'function') e.addEventListener(k.slice(2), v);
    else if (k === 'class') e.className = v;
    else if (k === 'dataset') Object.assign(e.dataset, v);
    else if (k in e && typeof v !== 'string') e[k] = v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat(Infinity)) {
    if (c === undefined || c === null || c === false) continue;
    e.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return e;
}

export function clear(node) {
  while (node.firstChild) node.firstChild.remove();
  return node;
}

// Table from rows of objects: cols = [{ key, label, fmt?(v,row) }]
export function table(cols, rows, { onRow, empty = 'none' } = {}) {
  if (!rows.length) return el('p', { class: 'muted' }, empty);
  return el('table', {},
    el('thead', {}, el('tr', {}, cols.map((c) => el('th', {}, c.label)))),
    el('tbody', {}, rows.map((r) => el('tr', { class: onRow ? 'clickable' : '', onclick: onRow ? () => onRow(r) : null },
      cols.map((c) => el('td', { class: c.class }, c.fmt ? c.fmt(r[c.key], r) : (r[c.key] ?? '')))))));
}

// Small status line that shows the result of an action
export function status() {
  const s = el('span', { class: 'status' });
  s.set = (text, kind = '') => { s.textContent = text; s.className = `status ${kind}`; };
  return s;
}

// Runs fn with a button disabled meanwhile; shows errors in st
export async function busy(btn, st, fn) {
  btn.disabled = true;
  st?.set('…');
  try {
    const r = await fn();
    st?.set(typeof r === 'string' ? r : 'done', 'ok');
    return r;
  } catch (e) {
    st?.set(e.message, 'err');
    return undefined;
  } finally {
    btn.disabled = false;
  }
}

export function fmtTime(d) {
  return d.toTimeString().slice(0, 8) + '.' + String(d.getMilliseconds()).padStart(3, '0');
}
