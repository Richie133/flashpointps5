/* Flashpoint PS5 - the Flashpoint Database frontend (FlashpointProject/
 * flashpoint-database) merged with the launcher side of Flashpoint: it
 * searches the local flashpoint.sqlite through /api (same endpoints and
 * fields as the database API), or the online database, and plays entries
 * on the console. Written for the PS5 browser: no build step, no
 * frameworks, D-pad navigation, and paging done on the server. */
'use strict';

const IS_PS5 = /PlayStation/i.test(navigator.userAgent);
const HAS_WASM = typeof WebAssembly === 'object';
const PER_PAGE = 30;
const FRAMES = 50;

/* From flashpoint-database's search.js: which entry fields are shown. */
const META = [
  ['title', 'Title'], ['alternateTitles', 'Alternate titles'], ['series', 'Series'],
  ['developer', 'Developer'], ['publisher', 'Publisher'], ['source', 'Source'],
  ['library', 'Library'], ['tags', 'Tags'], ['platform', 'Platform'], ['playMode', 'Play mode'],
  ['status', 'Status'], ['version', 'Version'], ['releaseDate', 'Release date'],
  ['language', 'Language'], ['notes', 'Notes'], ['originalDescription', 'Original description'],
  ['dateAdded', 'Date added'], ['dateModified', 'Last modified'],
  ['applicationPath', 'Application path'], ['launchCommand', 'Launch command'],
  ['zipped', 'Format'], ['id', 'ID'],
];

const S = {
  config: null,
  fields: [],
  sorts: [],
  platforms: [],
  route: null,
  seen: {},          // pages looked at, per search: the timeline's keyframes
  results: [],
  online: null,      // cached online result list
  onlineKey: '',
  total: 0,
  playToken: 0,
};

/* ------------------------------------------------------------ helpers */

function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  for (const k in attrs || {}) {
    const v = attrs[k];
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'text') el.textContent = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat()) {
    if (kid === null || kid === undefined || kid === false) continue;
    el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
  return el;
}
const $ = (sel, root) => (root || document).querySelector(sel);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fmt = (n) => Number(n || 0).toLocaleString();
const sentence = (s) => (s && s !== s.toUpperCase() ? s.charAt(0) + s.slice(1).toLowerCase() : s);
const imgPath = (kind, id) => `/images/${kind}/${id.substring(0, 2)}/${id.substring(2, 4)}/${id}.png`;

/* Flashpoint dates are UTC, often "2020-10-25 05:29:32.404" (which Safari
 * won't parse as is). */
function showDate(v) {
  let iso = String(v).trim().replace(' ', 'T');
  if (/^\d{4}-\d{2}-\d{2}$/.test(iso)) iso += 'T00:00:00Z';
  else if (!/(Z|[+-]\d{2}:?\d{2})$/.test(iso)) iso += 'Z';
  const d = new Date(iso);
  return isNaN(d) ? v : d.toLocaleDateString(undefined, { timeZone: 'UTC', year: 'numeric', month: 'short', day: 'numeric' });
}

function mb(bytes) {
  if (!(bytes > 0)) return '';
  return bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

async function getJSON(url, opts) {
  const r = await fetch(url, opts);
  if (!r.ok && r.status !== 404) throw new Error(`${r.status} from ${url}`);
  const data = await r.json();
  return { data, ok: r.ok, total: Number(r.headers.get('X-Total-Count') || 0) };
}

let toastTimer = 0;
function toast(msg) {
  const t = $('#toast');
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 6000);
}

/* ------------------------------------------------------------ routing */

function parseRoute() {
  const raw = location.hash.replace(/^#\/?/, '') || 'library';
  const [path, qs] = raw.split('?');
  const parts = path.split('/');
  return { view: parts[0] || 'library', arg: decodeURIComponent(parts[1] || ''), q: new URLSearchParams(qs || '') };
}

function go(view, q, replace) {
  const qs = q && q.toString();
  const hash = `#/${view}${qs ? '?' + qs : ''}`;
  if (replace) {
    history.replaceState(null, '', hash);
    route();
  } else if (location.hash === hash) {
    route();
  } else {
    location.hash = hash;
  }
}

function route() {
  const r = parseRoute();
  const prev = S.route;
  S.route = r;
  document.querySelectorAll('.tabs a').forEach((a) => {
    const tab = a.dataset.tab;
    const on = tab === r.view || (r.view === 'game' && prev && tab === prev.view);
    if (on) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
  });
  const listView = ['library', 'played', 'installed'].includes(r.view);
  // flipping pages keeps the panel (and focus) where it is
  if (listView && prev && prev.view === r.view && sameSearch(prev.q, r.q) && $('.results')) {
    loadResults();
    return;
  }
  const main = $('#main');
  main.replaceChildren();
  window.scrollTo(0, 0);
  if (listView) renderLibrary(main, r.view);
  else if (r.view === 'game') renderGame(main, r.arg, r.q.get('src') || 'local');
  else if (r.view === 'stats') renderStats(main);
  else if (r.view === 'help') renderSetup(main);
  else renderLibrary(main, 'library');
}

function sameSearch(a, b) {
  const strip = (q) => { const c = new URLSearchParams(q); c.delete('page'); c.sort(); return c.toString(); };
  return strip(a) === strip(b);
}

/* ------------------------------------------------------------ library */

function defaults(q) {
  const out = new URLSearchParams(q);
  if (!out.has('ps5')) out.set('ps5', IS_PS5 ? '1' : '0');
  if (!out.has('filter')) out.set('filter', S.config && S.config.filterExtreme === false ? '0' : '1');
  if (!out.has('src')) out.set('src', S.config && !S.config.dbOpen ? 'online' : 'local');
  return out;
}

function renderLibrary(main, mode) {
  const q = defaults(S.route.q);
  const wrap = h('div', { class: mode === 'library' ? 'library' : '' });
  if (mode === 'library') wrap.append(searchPanel(q));
  const titles = { library: 'Library', played: 'Played', installed: 'On this PS5' };
  const res = h('section', { class: 'results', 'aria-label': 'Results' },
    h('div', { class: 'results-top' },
      h('div', { class: 'results-head' },
        h('h1', { text: titles[mode] }),
        h('span', { class: 'count', id: 'count' }),
        h('span', { class: 'legend' },
          h('span', {}, h('i', { class: 'key on' }), 'Downloaded to this PS5'))),
      timeline(),
      h('div', { class: 'timeline-caption' }, h('span', { id: 'pageinfo' }), h('span', { text: IS_PS5 ? 'L2 / R2 flips pages' : 'Tap the timeline to flip pages' }))),
    h('div', { class: 'grid', id: 'grid' }));
  wrap.append(res);
  main.append(wrap);
  loadResults();
  setTimeout(() => {
    const first = mode === 'library' ? $('#q') : $('.tile');
    if (first && !IS_PS5 && mode === 'library') return; // don't pop keyboards on phones
    if (first) first.focus();
  }, 50);
}

function searchPanel(q) {
  const src = q.get('src');
  const libs = (S.fields.find((f) => f.name === 'library') || { values: [] }).values;
  const form = h('form', { class: 'panel', role: 'search', onsubmit: (e) => { e.preventDefault(); submitSearch(); } },
    h('h2', { text: 'Search' }),
    h('label', { class: 'field' },
      h('span', { text: 'Title, series or creator' }),
      h('input', { type: 'search', id: 'q', value: q.get('smartSearch') || '', placeholder: 'e.g. Bloons', enterkeyhint: 'search', autocomplete: 'off' })),
    h('details', { class: 'more', open: window.innerWidth > 900 || !!q.get('library') || q.getAll('f').length > 0 },
      h('summary', { 'data-nav': true }, 'Filters and sorting'),
      h('label', { class: 'field' },
        h('span', { text: 'Library' }),
        h('select', { id: 'lib' },
          h('option', { value: '', text: 'Games and animations' }),
          libs.map((v) => h('option', { value: v.name, text: v.displayName, selected: q.get('library') === v.name })))),
      h('label', { class: 'field' },
        h('span', { text: 'Sort by' }),
        h('div', { class: 'field-row' },
          h('select', { id: 'sort' }, sortOptions(src).map((s) => h('option', { value: s.name, text: sentence(s.displayName), selected: (q.get('sort') || 'title') === s.name }))),
          h('select', { id: 'dir', 'aria-label': 'Direction', style: 'width:auto' },
            h('option', { value: 'asc', text: 'A–Z, oldest', selected: q.get('dir') !== 'desc' }),
            h('option', { value: 'desc', text: 'Z–A, newest', selected: q.get('dir') === 'desc' })))),
      h('div', { class: 'extra-fields', id: 'extra' }),
      h('label', { class: 'field' },
        h('span', { text: 'Match another field' }),
        h('div', { class: 'field-row' },
          h('select', { id: 'addfield' },
            S.fields.filter((f) => !['smartSearch', 'library'].includes(f.name)).map((f) => h('option', { value: f.name, text: sentence(f.displayName) }))),
          h('button', { type: 'button', class: 'btn', onclick: () => { addField($('#addfield').value, ''); } }, 'Add'))),
      h('label', { class: 'check' }, h('input', { type: 'checkbox', id: 'ps5', checked: q.get('ps5') === '1' }), 'Only what plays on the PS5'),
      h('label', { class: 'check' }, h('input', { type: 'checkbox', id: 'filter', checked: q.get('filter') === '1' }), 'Hide extreme entries'),
      h('label', { class: 'check' }, h('input', { type: 'checkbox', id: 'any', checked: q.get('any') === '1' }), 'Match any field, not all'),
      h('label', { class: 'field', style: 'margin-top:0.75rem' },
        h('span', { text: 'Search in' }),
        h('select', { id: 'src' },
          h('option', { value: 'local', text: 'This PS5’s library', selected: src === 'local' }),
          h('option', { value: 'online', text: 'Flashpoint database online', selected: src === 'online' })))),
    h('button', { type: 'submit', class: 'btn wide', style: 'margin-top:0.5rem' }, 'Search'));
  setTimeout(() => q.getAll('f').forEach((f) => {
    const i = f.indexOf(':');
    if (i > 0) addField(f.slice(0, i), f.slice(i + 1));
  }));
  return form;
}

function sortOptions(src) {
  const list = S.sorts.slice();
  if (src !== 'online') {
    list.splice(list.length - 1, 0,
      { name: 'releaseDate', displayName: 'Release date', fields: ['releaseDate'] },
      { name: 'lastPlayed', displayName: 'Last played', fields: ['lastPlayed'] });
  }
  return list;
}

function addField(name, value) {
  const f = S.fields.find((x) => x.name === name);
  if (!f) return;
  if (f.name === 'platform') f.values = S.platforms.map((p) => ({ name: p, displayName: p }));
  const input = f.values && f.values.length
    ? h('select', { 'data-field': f.name }, f.values.map((v) => h('option', { value: v.name, text: v.displayName, selected: v.name === value })))
    : h('input', { type: 'text', 'data-field': f.name, value, autocomplete: 'off' });
  const row = h('label', { class: 'field' },
    h('span', { text: sentence(f.displayName) }),
    h('div', { class: 'field-row' }, input,
      h('button', { type: 'button', class: 'btn small', 'aria-label': `Remove ${f.displayName}`, onclick: () => { row.remove(); } }, 'Remove')));
  $('#extra').append(row);
  if (!value) input.focus();
}

function submitSearch() {
  const q = new URLSearchParams();
  const v = $('#q').value.trim();
  if (v) q.set('smartSearch', v);
  if ($('#lib').value) q.set('library', $('#lib').value);
  q.set('sort', $('#sort').value);
  q.set('dir', $('#dir').value);
  q.set('ps5', $('#ps5').checked ? '1' : '0');
  q.set('filter', $('#filter').checked ? '1' : '0');
  if ($('#any').checked) q.set('any', '1');
  q.set('src', $('#src').value);
  document.querySelectorAll('[data-field]').forEach((el) => {
    if (el.value.trim()) q.append('f', `${el.dataset.field}:${el.value.trim()}`);
  });
  go('library', q);
  setTimeout(() => { const t = $('.tile'); if (t && IS_PS5) t.focus(); }, 400);
}

function apiParams(q, mode) {
  const p = new URLSearchParams();
  for (const key of ['smartSearch', 'library', 'sort', 'dir']) if (q.get(key)) p.set(key, q.get(key));
  p.set('filter', q.get('filter') === '1' ? 'true' : 'false');
  if (q.get('any') === '1') p.set('any', 'true');
  if (q.get('ps5') === '1' && mode === 'library') p.set('ps5', 'true');
  const per = {};
  q.getAll('f').forEach((f) => {
    const i = f.indexOf(':');
    if (i > 0) per[f.slice(0, i)] = per[f.slice(0, i)] ? `${per[f.slice(0, i)]},${f.slice(i + 1)}` : f.slice(i + 1);
  });
  for (const k in per) p.set(k, per[k]);
  if (mode === 'played') { p.set('played', 'true'); p.set('sort', 'lastPlayed'); p.set('dir', 'desc'); }
  if (mode === 'installed') { p.set('installed', 'true'); }
  return p;
}

async function loadResults() {
  const r = S.route;
  const mode = r.view;
  const q = defaults(r.q);
  const page = Math.max(1, parseInt(q.get('page') || '1', 10));
  const grid = $('#grid');
  if (!grid) return;
  const token = Symbol('load');
  S.loading = token;
  grid.replaceChildren(h('div', { class: 'loading', text: 'Loading…' }));
  const p = apiParams(q, mode);
  let list = [];
  let total = 0;
  const online = mode === 'library' && q.get('src') === 'online';
  try {
    if (online) {
      ({ list, total } = await onlineSearch(p, q, page));
    } else {
      if (S.config && !S.config.dbOpen) { renderNoDb(grid); setCount(0, 1); return; }
      p.set('limit', PER_PAGE);
      p.set('offset', (page - 1) * PER_PAGE);
      p.set('fields', 'id,title,developer,publisher,platform,library,installed,zipped');
      const res = await getJSON(`/api/search?${p}`);
      list = res.data;
      total = res.total;
    }
  } catch (e) {
    if (S.loading !== token) return;
    grid.replaceChildren(h('div', { class: 'empty' },
      h('h2', { text: online ? 'The online database didn’t answer' : 'The search didn’t finish' }),
      h('p', { text: `${e.message}. Check the PS5’s internet connection, or search this PS5’s library instead.` })));
    return;
  }
  if (S.loading !== token) return;
  const key = p.toString();
  (S.seen[key] = S.seen[key] || {})[page] = true;
  S.total = total;
  setCount(total, page, key);
  if (!list.length) {
    grid.replaceChildren(emptyState(mode, q));
    return;
  }
  grid.replaceChildren(...list.map((g) => tile(g, online ? 'online' : 'local')));
}

async function onlineSearch(p, q, page) {
  const hasQuery = [...p.keys()].some((k) => !['filter', 'any', 'sort', 'dir', 'ps5'].includes(k));
  if (!hasQuery) return { list: [], total: 0 };
  const key = p.toString();
  if (S.onlineKey !== key) {
    const op = new URLSearchParams(p);
    op.delete('sort'); op.delete('dir'); op.delete('ps5');
    op.set('fields', 'id,title,developer,publisher,platform,library,zipped');
    op.set('limit', '1000');
    const res = await getJSON(`${S.config.onlineApi}/search?${op}`);
    let list = res.data || [];
    if (q.get('ps5') === '1') list = list.filter((g) => /Flash|HTML5/.test(g.platform || ''));
    const sort = sortOptions('online').find((s) => s.name === (q.get('sort') || 'title'));
    const dir = q.get('dir') === 'desc' ? -1 : 1;
    if (sort && sort.name === 'random') list.sort(() => Math.random() - 0.5);
    else if (sort) {
      list.sort((a, b) => {
        for (const f of sort.fields) {
          const c = String(a[f] || '').localeCompare(String(b[f] || ''), 'en', { sensitivity: 'base' });
          if (c) return c * dir;
        }
        return String(a.title).localeCompare(String(b.title), 'en', { sensitivity: 'base' }) * dir;
      });
    }
    S.online = list;
    S.onlineKey = key;
  }
  const start = (page - 1) * PER_PAGE;
  return { list: S.online.slice(start, start + PER_PAGE), total: S.online.length };
}

function setCount(total, page, key) {
  const pages = Math.max(1, Math.ceil(total / PER_PAGE));
  const c = $('#count');
  if (c) c.textContent = total === 1 ? '1 entry' : `${fmt(total)} entries`;
  const info = $('#pageinfo');
  if (info) info.textContent = `Page ${fmt(page)} of ${fmt(pages)}`;
  drawTimeline(page, pages, (key && S.seen[key]) || {});
}

function tile(g, src) {
  const by = g.developer || g.publisher;
  return h('a', { class: 'tile', href: `#/game/${encodeURIComponent(g.id)}${src === 'online' ? '?src=online' : ''}`, 'data-nav': true },
    g.installed ? h('i', { class: 'dot', title: 'Downloaded to this PS5' }) : null,
    h('div', { class: 'thumb' }, h('img', { src: imgPath('Logos', g.id), alt: '', loading: 'lazy', onerror: (e) => { e.target.remove(); } })),
    h('div', { class: 't' },
      h('span', { class: 'name', text: g.title }),
      by ? h('span', { class: 'by', text: by }) : null,
      h('span', { class: 'plat', text: `${(g.platform || '').replace(/; /g, ', ')} ${g.library === 'theatre' ? 'animation' : 'game'}` })));
}

function emptyState(mode, q) {
  if (mode === 'played') return h('div', { class: 'empty' }, h('h2', { text: 'Nothing played yet' }), h('p', { text: 'Games you start from this PS5 show up here, newest first.' }));
  if (mode === 'installed') return h('div', { class: 'empty' }, h('h2', { text: 'No games downloaded yet' }), h('p', { text: 'Choosing Play downloads a game’s data to the PS5. It then plays without waiting and appears here.' }));
  if (q.get('src') === 'online' && !q.get('smartSearch') && !q.getAll('f').length) {
    return h('div', { class: 'empty' }, h('h2', { text: 'Search the online database' }), h('p', { text: 'Type a title, series or creator. The online database needs a search term; it doesn’t list everything at once.' }));
  }
  return h('div', { class: 'empty' }, h('h2', { text: 'No matches' }),
    h('p', { text: q.get('ps5') === '1' ? 'Nothing matched among entries that play on the PS5. Untick “Only what plays on the PS5” to see everything.' : 'Try fewer words, or match any field instead of all.' }));
}

function renderNoDb(grid) {
  grid.replaceChildren(h('div', { class: 'empty' },
    h('h2', { text: 'This PS5 has no Flashpoint library yet' }),
    h('p', { text: 'Copy a Flashpoint folder (with Data/flashpoint.sqlite) to the PS5 or a USB drive, then start Flashpoint PS5 again.' }),
    h('p', {}, h('a', { class: 'btn', href: '#/help', 'data-nav': true }, 'How to set it up'), ' ',
      h('a', { class: 'btn', href: '#/library?src=online', 'data-nav': true }, 'Search online instead'))));
}

/* ------------------------------------------------------------ timeline */

function timeline() {
  const el = h('div', { class: 'timeline', id: 'timeline', tabindex: '0', role: 'slider', 'aria-label': 'Page', 'data-nav': true },
    h('div', { class: 'ruler', id: 'ruler' }),
    h('div', { class: 'frames', id: 'frames' }),
    h('div', { class: 'playhead', id: 'playhead' }, h('b', { id: 'playnum' })));
  el.addEventListener('click', (e) => {
    const box = el.getBoundingClientRect();
    const cells = Number(el.dataset.cells || 1);
    const start = Number(el.dataset.start || 1);
    const idx = Math.floor(((e.clientX - box.left) / box.width) * cells);
    gotoPage(start + Math.max(0, Math.min(cells - 1, idx)));
  });
  el.addEventListener('keydown', (e) => {
    if (e.keyCode === 37 || e.keyCode === 39) {
      e.preventDefault();
      e.stopPropagation();
      flipPage(e.keyCode === 39 ? 1 : -1);
    }
  });
  return el;
}

function drawTimeline(page, pages, seen) {
  const el = $('#timeline');
  if (!el) return;
  const cells = Math.min(FRAMES, pages);
  let start = Math.max(1, page - Math.floor(cells / 2));
  start = Math.min(start, Math.max(1, pages - cells + 1));
  el.dataset.cells = cells;
  el.dataset.start = start;
  el.dataset.pages = pages;
  el.setAttribute('aria-valuemin', '1');
  el.setAttribute('aria-valuemax', String(pages));
  el.setAttribute('aria-valuenow', String(page));
  const ruler = [], frames = [];
  for (let i = 0; i < cells; i++) {
    const n = start + i;
    const tick = n === 1 || n % 5 === 0;
    ruler.push(h('span', { class: tick ? 'tick' : '', text: tick ? n : '' }));
    frames.push(h('span', { class: [n % 5 === 0 ? 'every5' : '', seen[n] ? 'seen' : ''].join(' ').trim() }));
  }
  $('#ruler').replaceChildren(...ruler);
  $('#frames').replaceChildren(...frames);
  $('#playhead').style.left = `calc(${((page - start + 0.5) / cells) * 100}% - 1px)`;
  $('#playnum').textContent = page;
}

function gotoPage(n) {
  const el = $('#timeline');
  const pages = el ? Number(el.dataset.pages || 1) : 1;
  const page = Math.max(1, Math.min(pages, n));
  const q = new URLSearchParams(S.route.q);
  const cur = parseInt(q.get('page') || '1', 10);
  if (page === cur) return;
  q.set('page', page);
  go(S.route.view, q, true);
}

let flipTimer = 0;
let pendingPage = 0;
function flipPage(delta) {
  // holding a button scrubs: move the playhead now, load when it stops
  const el = $('#timeline');
  if (!el) return;
  const pages = Number(el.dataset.pages || 1);
  const cur = pendingPage || parseInt(S.route.q.get('page') || '1', 10);
  pendingPage = Math.max(1, Math.min(pages, cur + delta));
  const key = apiParams(defaults(S.route.q), S.route.view).toString();
  drawTimeline(pendingPage, pages, S.seen[key] || {});
  $('#pageinfo').textContent = `Page ${fmt(pendingPage)} of ${fmt(pages)}`;
  clearTimeout(flipTimer);
  flipTimer = setTimeout(() => { const p = pendingPage; pendingPage = 0; gotoPage(p); }, 280);
}

/* ------------------------------------------------------------ entry viewer */

async function renderGame(main, id, src) {
  main.append(h('div', { class: 'loading', text: 'Loading…' }));
  let game = null, play = null, addApps = [];
  try {
    const local = await getJSON(`/api/game?id=${encodeURIComponent(id)}`);
    if (local.ok) {
      game = local.data.game;
      play = local.data.play;
      addApps = game.addApps || [];
    }
    if (!game && S.config && S.config.onlineApi) {
      const on = await getJSON(`${S.config.onlineApi}/search?id=${encodeURIComponent(id)}&limit=1`);
      game = on.data && on.data[0];
      if (game) addApps = (await getJSON(`${S.config.onlineApi}/addapps?id=${encodeURIComponent(id)}`)).data || [];
    }
  } catch (e) {
    if (!game) {
      main.replaceChildren(h('div', { class: 'empty' }, h('h2', { text: 'This entry didn’t load' }), h('p', { text: e.message })));
      return;
    }
  }
  if (S.route.view !== 'game' || S.route.arg !== id) return;
  if (!game) {
    main.replaceChildren(h('div', { class: 'empty' }, h('h2', { text: 'Entry not found' }), h('p', { text: `No entry has the ID ${id}.` })));
    return;
  }
  main.replaceChildren(viewer(game, play, addApps, src));
  const btn = $('#playbtn');
  (btn && !btn.disabled ? btn : $('#backbtn')).focus();
}

function viewer(g, play, addApps, src) {
  const by = [g.developer, g.publisher].filter((x, i, a) => x && a.indexOf(x) === i).join(', ');
  const kind = `${(g.platform || '').replace(/; /g, ', ')} ${g.library === 'theatre' ? 'animation' : 'game'}`;
  const chips = [h('span', { class: 'chip', text: kind })];
  if (play && play.kind === 'flash') chips.push(h('span', { class: 'chip ok', text: 'Plays in Ruffle Flash' }));
  if (play && play.kind === 'html5') chips.push(h('span', { class: 'chip warn', text: 'Opens in the PS5 browser' }));
  if (g.installed) chips.push(h('span', { class: 'chip ok', text: 'Downloaded' }));
  const data = (g.gameData || []).find((d) => d.active) || (g.gameData || [])[0];
  if (data && data.size) chips.push(h('span', { class: 'chip', text: `GameZIP, ${mb(data.size)}` }));
  else if (g.zipped === false) chips.push(h('span', { class: 'chip', text: 'Legacy files' }));

  const status = h('div', { class: 'status', id: 'playstatus' });
  const bar = h('div', { class: 'progress', id: 'playbar', hidden: true }, h('i'));
  const actions = h('div', { class: 'actions' });
  const canPlay = play && play.playable;
  const playBtn = h('button', { class: 'btn play', id: 'playbtn', disabled: !canPlay, onclick: () => startPlay(g.id, '') },
    IS_PS5 ? h('span', { class: 'glyph', 'aria-hidden': 'true', text: '✕' }) : null, IS_PS5 ? 'Play' : 'Play on the PS5');
  actions.append(playBtn);
  if (!IS_PS5 && play && play.kind === 'flash' && HAS_WASM) {
    actions.append(h('a', { class: 'btn', href: `/play.html?id=${encodeURIComponent(g.id)}`, 'data-nav': true }, 'Play in this browser'));
  }
  if (!IS_PS5 && play && play.kind === 'html5') {
    actions.append(h('button', { class: 'btn', onclick: () => playHere(g.id, play) }, 'Play in this browser'));
  }
  actions.append(bar, status);
  if (!play) status.textContent = 'This entry is only in the online database. Copy a newer flashpoint.sqlite to the PS5 to play it.';
  else if (play.reason) status.textContent = play.reason;
  else if (play.kind === 'flash' && play.ruffleMode === 'export') status.textContent = 'Copies the game into Ruffle Flash’s library and opens the app.';

  const meta = h('table', { class: 'meta' });
  for (const [field, label] of META) {
    let v = g[field];
    if (v === undefined || v === null || v === '' || (Array.isArray(v) && !v.length)) continue;
    if (field === 'library') v = v === 'arcade' ? 'Games' : v === 'theatre' ? 'Animations' : v;
    else if (field === 'tags') v = v.join(', ');
    else if (field === 'zipped') v = v ? 'GameZIP' : 'Legacy';
    else if (field === 'dateAdded' || field === 'dateModified') v = showDate(v);
    meta.append(h('tr', {}, h('td', { text: label }), h('td', { class: field === 'notes' || field === 'originalDescription' ? 'pre' : '', text: v })));
  }

  const apps = (addApps || []).filter((a) => a.applicationPath !== ':extras:');
  const body = h('div', { class: 'viewer-body' },
    h('div', {},
      h('img', { class: 'shot', src: imgPath('Screenshots', g.id), alt: `Screenshot of ${g.title}`, onerror: (e) => { e.target.remove(); } }),
      apps.length ? h('h2', { text: 'Additional apps' }) : null,
      apps.length ? h('div', { class: 'addapps' }, apps.map((a) => h('button', { class: 'btn', disabled: !play, onclick: () => startPlay(g.id, a.id) }, a.name))) : null,
      play && g.zipped ? h('h2', {}, h('button', { class: 'btn small', onclick: (e) => listFiles(g.id, e.target) }, 'Show the files in this GameZIP')) : null,
      h('pre', { class: 'files', id: 'files', hidden: true })),
    h('div', {}, h('h2', { text: 'Details' }), meta));

  return h('article', { class: 'viewer' },
    h('p', {}, h('button', { class: 'btn small', id: 'backbtn', onclick: () => history.length > 1 ? history.back() : go('library') },
      IS_PS5 ? h('span', { class: 'glyph', text: '○' }) : null, 'Back')),
    h('div', { class: 'viewer-top' },
      h('img', { class: 'logo', src: imgPath('Logos', g.id), alt: '', onerror: (e) => { e.target.style.visibility = 'hidden'; } }),
      h('div', {},
        h('h1', { text: g.title }),
        by ? h('div', { class: 'sub', text: `by ${by}` }) : null,
        h('div', { class: 'chips' }, chips),
        g.originalDescription ? h('p', { class: 'desc', text: g.originalDescription }) : null),
      actions),
    body);
}

async function listFiles(id, btn) {
  const pre = $('#files');
  btn.disabled = true;
  try {
    const files = (await getJSON(`/api/files?id=${encodeURIComponent(id)}`)).data;
    pre.textContent = files.length ? files.join('\n') : 'Download the game first to see its files.';
  } catch (e) {
    pre.textContent = e.message;
  }
  pre.hidden = false;
}

function setProgress(job) {
  const bar = $('#playbar');
  const status = $('#playstatus');
  if (!bar || !status) return;
  status.classList.remove('error');
  if (!job) { bar.hidden = true; return; }
  bar.hidden = false;
  const pct = job.total > 0 ? Math.min(100, (job.done / job.total) * 100) : 0;
  $('i', bar).style.width = `${pct}%`;
  const words = { queued: 'Waiting to download', downloading: 'Downloading', verifying: 'Checking the download', extracting: 'Unpacking', done: 'Ready' };
  const amount = job.state === 'downloading' && job.total > 0 ? ` ${mb(job.done)} of ${mb(job.total)}` : job.state === 'extracting' ? ` ${Math.round(pct)}%` : '';
  status.textContent = `${words[job.state] || job.state}${amount}`;
}

async function startPlay(id, addApp) {
  const token = ++S.playToken;
  const btn = $('#playbtn');
  const status = $('#playstatus');
  if (btn) btn.disabled = true;
  status.classList.remove('error');
  status.textContent = 'Getting the game ready…';
  try {
    for (;;) {
      const url = `/api/play?id=${encodeURIComponent(id)}${addApp ? `&addApp=${encodeURIComponent(addApp)}` : ''}`;
      const r = (await getJSON(url, { method: 'POST' })).data;
      if (token !== S.playToken || S.route.view !== 'game') return;
      if (r.status === 'pending') {
        setProgress((r.jobs || []).find((j) => j.gameId === id) || { state: 'queued', done: 0, total: 0 });
        await sleep(1000);
        continue;
      }
      setProgress(null);
      if (r.status === 'open') {
        status.textContent = 'Opening the game…';
        location.href = r.url;
        return;
      }
      status.textContent = r.message || '';
      if (r.status === 'launched') toast(r.message);
      else status.classList.add('error');
      break;
    }
  } catch (e) {
    status.textContent = `The PS5 didn’t answer (${e.message}).`;
    status.classList.add('error');
  }
  if (btn) btn.disabled = false;
}

async function playHere(id, play) {
  const status = $('#playstatus');
  for (;;) {
    const r = (await getJSON(`/api/prepare?id=${encodeURIComponent(id)}`)).data;
    if (r.status === 'pending') {
      setProgress((r.jobs || []).find((j) => j.gameId === id));
      await sleep(1000);
      continue;
    }
    setProgress(null);
    if (r.status !== 'ready') { status.textContent = r.message; status.classList.add('error'); return; }
    break;
  }
  window.open(`http://${location.hostname}:${play.gameServerPort}${play.contentUrl}`, '_blank');
}

/* ------------------------------------------------------------ statistics */

async function renderStats(main) {
  const box = h('section', { class: 'stats' }, h('h1', { text: 'Statistics' }), h('div', { class: 'loading', text: 'Counting…' }));
  main.append(box);
  let s;
  try { s = (await getJSON('/api/stats')).data; } catch (e) { box.lastChild.textContent = e.message; return; }
  const lib = (n) => (s.libraryTotals.find((x) => x.name === n) || { count: 0 }).count;
  const fmtT = (n) => (s.formatTotals.find((x) => x.name === n) || { count: 0 }).count;
  const totals = [
    [lib('arcade'), 'games'], [lib('theatre'), 'animations'], [fmtT('gameZip'), 'in GameZIPs'],
    [fmtT('legacy'), 'as Legacy files'], [s.installedTotal || 0, 'downloaded to this PS5'], [s.playedTotal || 0, 'played here'],
  ];
  const max = Math.max(1, ...s.platformTotals.map((p) => p.count));
  box.lastChild.replaceWith(
    h('div', { class: 'totals' }, totals.map(([n, label]) => h('div', {}, h('b', { text: fmt(n) }), h('span', { text: label })))),
    h('h2', { text: 'Entries per platform' }),
    h('div', { class: 'bars' }, s.platformTotals.map((p) => h('div', { class: 'row' },
      h('span', { text: p.name }),
      h('span', { class: 'track' }, h('i', { style: `width:${Math.max(0.4, (p.count / max) * 100)}%` })),
      h('span', { class: 'n', text: fmt(p.count) })))));
}

/* ------------------------------------------------------------ setup */

async function renderSetup(main) {
  const c = S.config || (await loadConfig());
  const ui = `http://${c.lanIp}:${c.uiPort}`;
  let stopArmed = false;
  const stop = h('button', { class: 'btn', onclick: async () => {
    if (!stopArmed) { stopArmed = true; stop.textContent = 'Press again to stop Flashpoint PS5'; return; }
    await fetch('/api/quit');
    main.replaceChildren(h('div', { class: 'empty' }, h('h2', { text: 'Flashpoint PS5 has stopped' }), h('p', { text: 'Start it again from the Homebrew Launcher.' })));
  } }, 'Stop Flashpoint PS5');
  main.append(h('section', { class: 'setup' },
    h('h1', { text: 'Setup' }),
    h('div', { class: 'kv' },
      h('span', { text: 'Flashpoint folder' }), h('span', {}, h('code', { text: c.flashpointPath })),
      h('span', { text: 'Library' }), h('span', { text: c.dbOpen ? `${fmt(c.gameCount)} entries in flashpoint.sqlite` : 'No flashpoint.sqlite found' }),
      h('span', { text: 'Unpacked games' }), h('span', {}, h('code', { text: c.runPath })),
      h('span', { text: 'Flash player' }), h('span', { text: c.ruffleDirect ? `Ruffle Flash (${c.ruffleTitleId}), starts games directly` : `Ruffle Flash (${c.ruffleTitleId}), games are copied into its library` }),
      h('span', { text: 'Open on a phone or PC' }), h('span', {}, h('code', { text: ui })),
      h('span', { text: 'Version' }), h('span', { text: `${c.version}${c.configFile ? `, settings from ${c.configFile}` : ''}` })),
    h('h2', { text: 'Getting games onto the PS5' }),
    h('ol', {},
      h('li', {}, 'On a PC, get Flashpoint Infinity from flashpointarchive.org and run it once so its database is current.'),
      h('li', {}, 'Copy its ', h('code', { text: 'Data/flashpoint.sqlite' }), ' (keep the folder layout) to ', h('code', { text: '/data/flashpoint/' }), ' on the PS5, or to a ', h('code', { text: 'flashpoint' }), ' folder on a USB drive. Downloaded ', h('code', { text: 'Data/Games' }), ', ', h('code', { text: 'Data/Images' }), ' and ', h('code', { text: 'Legacy/htdocs' }), ' folders can come along; anything missing downloads when you need it.'),
      h('li', {}, 'Install Ruffle Flash for the PS5 (', h('code', { text: 'PPSA68091' }), ') with ShadowMountPlus and kstuff. Flash games open in it.'),
      h('li', {}, 'For games to start straight from here, install the Ruffle Flash build patched for Flashpoint (see the project’s ', h('code', { text: 'ruffle-ps5' }), ' folder). The stock app works too: games get copied into its library.')),
    h('p', { class: 'note', text: 'HTML5 entries open in the PS5 browser, which has no WebGL or Web Audio, so some will be blank or silent. Shockwave, Unity, Java and other plugin games have no PS5 player yet.' }),
    h('p', {}, stop)));
}

/* ------------------------------------------------------------ download badge */

async function pollJobs() {
  try {
    const jobs = (await getJSON('/api/jobs')).data.filter((j) => j.state !== 'done' && j.state !== 'error');
    const el = $('#jobs');
    if (!jobs.length) el.replaceChildren();
    else {
      const j = jobs[0];
      const pct = j.total > 0 ? ` ${Math.round((j.done / j.total) * 100)}%` : '';
      el.replaceChildren(h('b', { text: j.title }), h('br'), `${j.state === 'extracting' ? 'Unpacking' : 'Downloading'}${pct}${jobs.length > 1 ? `, ${jobs.length - 1} more waiting` : ''}`);
    }
  } catch (e) { /* the server went away; the next poll will say so */ }
  setTimeout(pollJobs, 3000);
}

/* ------------------------------------------------------------ D-pad navigation */

function focusables() {
  return [...document.querySelectorAll('a[href], button:not([disabled]), input, select, [data-nav]')]
    .filter((el) => {
      if (el.closest('[hidden]')) return false;
      const r = el.getBoundingClientRect();
      return r.width > 0 && r.height > 0;
    });
}

function moveFocus(dir) {
  const items = focusables();
  const cur = document.activeElement;
  if (!cur || cur === document.body || !items.includes(cur)) {
    const first = items.find((el) => el.closest('main')) || items[0];
    if (first) first.focus();
    return;
  }
  const a = cur.getBoundingClientRect();
  const ax = a.left + a.width / 2, ay = a.top + a.height / 2;
  let best = null, bestScore = Infinity;
  for (const el of items) {
    if (el === cur) continue;
    const b = el.getBoundingClientRect();
    const bx = b.left + b.width / 2, by = b.top + b.height / 2;
    const dx = bx - ax, dy = by - ay;
    let primary, secondary;
    if (dir === 'left') { if (b.right > a.left + 1) continue; primary = -dx; secondary = Math.abs(dy); }
    else if (dir === 'right') { if (b.left < a.right - 1) continue; primary = dx; secondary = Math.abs(dy); }
    else if (dir === 'up') { if (b.bottom > a.top + 1) continue; primary = -dy; secondary = Math.abs(dx); }
    else { if (b.top < a.bottom - 1) continue; primary = dy; secondary = Math.abs(dx); }
    const score = primary + secondary * 2.5;
    if (score < bestScore) { bestScore = score; best = el; }
  }
  if (best) {
    best.focus();
    best.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }
}

const TABS = ['library', 'played', 'installed', 'stats', 'help'];
document.addEventListener('keydown', (e) => {
  const t = e.target;
  const typing = t && t.tagName === 'INPUT' && /text|search/.test(t.type);
  const k = e.keyCode;
  const dirs = { 37: 'left', 38: 'up', 39: 'right', 40: 'down' };
  if (dirs[k]) {
    if (typing && (k === 37 || k === 39)) {
      const atStart = t.selectionStart === 0 && t.selectionEnd === 0;
      const atEnd = t.selectionStart === t.value.length;
      if ((k === 37 && !atStart) || (k === 39 && !atEnd)) return;
    }
    e.preventDefault();
    moveFocus(dirs[k]);
    return;
  }
  if (k === 116 || k === 117) { // L1 / R1
    e.preventDefault();
    const view = S.route.view === 'game' ? 'library' : S.route.view;
    const i = TABS.indexOf(view);
    go(TABS[(i + (k === 117 ? 1 : TABS.length - 1)) % TABS.length]);
    return;
  }
  if ((k === 118 || k === 119) && $('#timeline')) { // L2 / R2
    e.preventDefault();
    flipPage(k === 119 ? 1 : -1);
    return;
  }
  if (k === 13 && t && t.matches && t.matches('[data-nav]:not(a):not(button)') && t.id !== 'timeline') {
    e.preventDefault();
    t.click();
    return;
  }
  if ((k === 27 || (k === 8 && !typing)) && S.route.view === 'game') {
    e.preventDefault();
    history.back();
  }
});

/* ------------------------------------------------------------ start */

async function loadConfig() {
  try {
    S.config = (await getJSON('/api/config')).data;
  } catch (e) {
    S.config = { dbOpen: false, onlineApi: 'https://db-api.unstable.life', lanIp: location.hostname, uiPort: location.port, version: '?', flashpointPath: '?', runPath: '?', ruffleTitleId: 'PPSA68091' };
  }
  return S.config;
}

(async function start() {
  const [, fields, sorts] = await Promise.all([
    loadConfig(),
    fetch('/fields.json').then((r) => r.json()).catch(() => []),
    fetch('/sort.json').then((r) => r.json()).catch(() => []),
  ]);
  S.fields = fields;
  S.sorts = sorts;
  if (S.config.dbOpen) getJSON('/api/platforms').then((r) => { S.platforms = r.data; }).catch(() => {});
  window.addEventListener('hashchange', route);
  route();
  pollJobs();
})();
