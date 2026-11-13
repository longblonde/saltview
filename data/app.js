// Shared helpers for the SaltView dashboard pages.

const $ = (sel, el = document) => el.querySelector(sel);
const $$ = (sel, el = document) => [...el.querySelectorAll(sel)];

async function api(path, body) {
  const opts = body === undefined ? {} : {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  };
  const r = await fetch(path, opts);
  const text = await r.text();
  if (!r.ok) throw new Error(text || r.statusText);
  return text ? JSON.parse(text) : {};
}

function toast(msg) {
  let el = $('.toast');
  if (!el) { el = document.createElement('div'); el.className = 'toast'; document.body.appendChild(el); }
  el.textContent = msg;
  el.classList.add('show');
  clearTimeout(el._t);
  el._t = setTimeout(() => el.classList.remove('show'), 2500);
}

// --- units (device stores mm) -------------------------------------------------
const MM_PER = { in: 25.4, cm: 10 };
const toUnits = (mm, u) => mm == null ? null : mm / MM_PER[u];
const fromUnits = (v, u) => v * MM_PER[u];
const fmtLen = (mm, u, digits = 1) => mm == null ? '--' : `${toUnits(mm, u).toFixed(digits)} ${u}`;
const fmtPct = p => p == null ? '--' : `${Math.round(p)}%`;
const fmtTime = ts => ts ? new Date(ts * 1000).toLocaleString([], { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' }) : '--';

// --- header + notifications ---------------------------------------------------
const PAGES = [['index.html', 'Dashboard'], ['setup.html', 'Setup'], ['display.html', 'Display']];
let seenNoteIds = null;

function renderHeader(active) {
  const header = document.createElement('header');
  header.innerHTML = `<div class="bar">
    <h1 id="devName">SaltView</h1>
    <nav>${PAGES.map(([href, name]) => `<a href="${href}" class="${href === active ? 'active' : ''}">${name}</a>`).join('')}</nav>
    <button class="bell" id="bell" title="Notifications">&#128276;<span class="badge" id="badge"></span></button>
  </div>
  <div class="card panel" id="notePanel" hidden>
    <div class="chart-head"><h2>Notifications</h2>
      <span><button class="btn" id="readAll">Mark read</button> <button class="btn" id="clearNotes">Clear</button></span></div>
    <ul class="notes" id="panelNotes"></ul>
  </div>`;
  document.body.prepend(header);
  $('#bell').onclick = () => { $('#notePanel').hidden = !$('#notePanel').hidden; };
  $('#readAll').onclick = async () => renderNotes(await api('/api/notifications', { action: 'read', id: 0 }));
  $('#clearNotes').onclick = async () => renderNotes(await api('/api/notifications', { action: 'clear' }));
  pollNotes();
  setInterval(pollNotes, 30000);
}

function noteList(items, el) {
  el.innerHTML = items.length ? '' : '<li class="hint">No notifications</li>';
  [...items].reverse().forEach(n => {
    const li = document.createElement('li');
    li.className = `${n.sev} ${n.read ? 'read' : ''}`;
    li.innerHTML = `<span class="dot"></span><span class="msg"></span><time>${fmtTime(n.ts)}</time>`;
    $('.msg', li).textContent = n.msg;
    el.appendChild(li);
  });
}

function renderNotes(data) {
  $('#badge').textContent = data.unread || '';
  noteList(data.items, $('#panelNotes'));
  const pageList = $('#pageNotes');
  if (pageList) noteList(data.items, pageList);

  // Toast anything new since the page loaded.
  const ids = new Set(data.items.map(n => n.id));
  if (seenNoteIds) data.items.filter(n => !seenNoteIds.has(n.id)).forEach(n => toast(n.msg));
  seenNoteIds = ids;
}

async function pollNotes() {
  try { renderNotes(await api('/api/notifications')); } catch (e) { /* device busy/offline */ }
}

// --- charts (no external libraries: the device may have no internet) -----------
function css(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

function setupCanvas(canvas) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr; canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, w, h);
  ctx.font = '12px system-ui, sans-serif';
  return { ctx, w, h, pad: { l: 40, r: 12, t: 10, b: 24 } };
}

function drawAxes(c, x0, x1, y0, y1, yFmt, xFmt, xTicks = 5) {
  const { ctx, w, h, pad } = c;
  ctx.strokeStyle = css('--grid'); ctx.fillStyle = css('--muted'); ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const v = y0 + (y1 - y0) * i / 4, y = h - pad.b - (h - pad.t - pad.b) * i / 4;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(w - pad.r, y); ctx.stroke();
    ctx.textAlign = 'right'; ctx.textBaseline = 'middle'; ctx.fillText(yFmt(v), pad.l - 6, y);
  }
  ctx.textAlign = 'center'; ctx.textBaseline = 'top';
  for (let i = 0; i <= xTicks; i++) {
    const v = x0 + (x1 - x0) * i / xTicks, x = pad.l + (w - pad.l - pad.r) * i / xTicks;
    ctx.fillText(xFmt(v), x, h - pad.b + 6);
  }
}

// points: [[unixSeconds, value], ...]. Each point gets a dot; `selected` (an index) is
// highlighted. The dots' screen positions are kept on the canvas for pickPoint().
function drawLineChart(canvas, points, { yMin = 0, yMax = 100, yFmt = v => `${v}%`, threshold, selected = -1 } = {}) {
  const c = setupCanvas(canvas), { ctx, w, h, pad } = c;
  canvas._dots = [];
  if (points.length < 2) {
    ctx.fillStyle = css('--muted'); ctx.textAlign = 'center';
    ctx.fillText(points.length ? 'One measurement so far' : 'Not enough data yet', w / 2, h / 2 - 16);
    if (!points.length) return;
  }
  const x0 = points[0][0], x1 = points[points.length - 1][0];
  const sx = t => pad.l + (t - x0) / (x1 - x0 || 1) * (w - pad.l - pad.r);
  const sy = v => h - pad.b - (v - yMin) / (yMax - yMin) * (h - pad.t - pad.b);
  const spanDays = (x1 - x0) / 86400;
  const xFmt = t => new Date(t * 1000).toLocaleDateString([], spanDays > 2 ? { month: 'short', day: 'numeric' } : { hour: 'numeric' });
  drawAxes(c, x0, x1, yMin, yMax, yFmt, xFmt);

  if (threshold != null) {
    ctx.strokeStyle = css('--alert'); ctx.setLineDash([4, 4]);
    ctx.beginPath(); ctx.moveTo(pad.l, sy(threshold)); ctx.lineTo(w - pad.r, sy(threshold)); ctx.stroke();
    ctx.setLineDash([]);
  }
  const accent = css('--accent');
  ctx.beginPath();
  points.forEach(([t, v], i) => i ? ctx.lineTo(sx(t), sy(v)) : ctx.moveTo(sx(t), sy(v)));
  ctx.strokeStyle = accent; ctx.lineWidth = 2; ctx.stroke();
  ctx.lineTo(sx(x1), sy(yMin)); ctx.lineTo(sx(x0), sy(yMin)); ctx.closePath();
  ctx.fillStyle = css('--accent-soft'); ctx.fill();

  // A dot per measurement; smaller when they crowd together.
  const clampY = v => Math.min(yMax, Math.max(yMin, v));
  canvas._dots = points.map(([t, v]) => [sx(t), sy(clampY(v))]);
  const gap = (w - pad.l - pad.r) / Math.max(1, points.length - 1);
  const r = gap < 5 ? 1.5 : gap < 12 ? 2.5 : 3.5;
  const surface = css('--surface');
  ctx.fillStyle = accent; ctx.strokeStyle = surface; ctx.lineWidth = 1;
  canvas._dots.forEach(([x, y]) => { ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.fill(); if (r > 2) ctx.stroke(); });

  // `selected`: one index or an array of them. A single selection also gets a guide line.
  const sel = (Array.isArray(selected) ? selected : [selected]).filter(i => i >= 0 && i < points.length);
  sel.forEach(i => {
    const [x, y] = canvas._dots[i];
    if (sel.length === 1) {
      ctx.strokeStyle = accent; ctx.globalAlpha = .35; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(x, pad.t); ctx.lineTo(x, h - pad.b); ctx.stroke();
      ctx.globalAlpha = 1;
    }
    ctx.beginPath(); ctx.arc(x, y, 7, 0, Math.PI * 2); ctx.fillStyle = css('--accent-soft'); ctx.fill();
    ctx.beginPath(); ctx.arc(x, y, 5, 0, Math.PI * 2);
    ctx.fillStyle = accent; ctx.fill(); ctx.strokeStyle = surface; ctx.lineWidth = 2; ctx.stroke();
  });
}

// Index of the dot nearest a pointer event (by x first, as on a time axis), or -1.
function pickPoint(canvas, e) {
  const dots = canvas._dots || [];
  if (!dots.length) return -1;
  const rect = canvas.getBoundingClientRect();
  const px = e.clientX - rect.left, py = e.clientY - rect.top;
  let best = -1, bestD = Infinity;
  dots.forEach(([x, y], i) => {
    const d = Math.abs(x - px) * 3 + Math.abs(y - py);
    if (d < bestD) { bestD = d; best = i; }
  });
  return Math.abs(dots[best][0] - px) <= 24 ? best : -1;
}

// bars: [[label, value], ...]
function drawBarChart(canvas, bars, { yFmt = v => v.toFixed(1) } = {}) {
  const c = setupCanvas(canvas), { ctx, w, h, pad } = c;
  if (!bars.length) {
    ctx.fillStyle = css('--muted'); ctx.textAlign = 'center';
    ctx.fillText('Not enough data yet', w / 2, h / 2);
    return;
  }
  const yMax = Math.max(1, ...bars.map(b => b[1])) * 1.15;
  const sy = v => h - pad.b - v / yMax * (h - pad.t - pad.b);
  const step = (w - pad.l - pad.r) / bars.length;
  drawAxes(c, 0, 0, 0, yMax, yFmt, () => '', 0);
  ctx.fillStyle = css('--accent');
  bars.forEach(([, v], i) => {
    const bw = Math.max(2, step * 0.7);
    ctx.fillRect(pad.l + i * step + (step - bw) / 2, sy(v), bw, h - pad.b - sy(v));
  });
  ctx.fillStyle = css('--muted'); ctx.textAlign = 'center'; ctx.textBaseline = 'top';
  const every = Math.ceil(bars.length / 8);
  bars.forEach(([label], i) => { if (i % every === 0) ctx.fillText(label, pad.l + i * step + step / 2, h - pad.b + 6); });
}

// Salt used per day, ignoring refills; mirrors History::usagePctPerDay on the device.
function dailyUsage(rows) {
  const NOISE = 1, REFILL = 10, days = new Map();
  let ref = null;
  for (const [ts, pct] of rows) {
    const key = new Date(ts * 1000).toLocaleDateString([], { month: 'short', day: 'numeric' });
    if (!days.has(key)) days.set(key, 0);
    if (ref == null) ref = pct;
    else if (pct < ref - NOISE) { days.set(key, days.get(key) + ref - pct); ref = pct; }
    else if (pct > ref + REFILL) ref = pct;
  }
  return [...days.entries()];
}

// --- quad alphanumeric preview ----------------------------------------------------
// Renders 4 cells like the FeatherWing: '.' merges into the previous cell, short text right-aligns.
function toCells(text) {
  const cells = [];
  for (const ch of text.toUpperCase()) {
    const last = cells[cells.length - 1];
    if (ch === '.' && last && !last.dot && last.c !== ' ') last.dot = true;
    else cells.push({ c: ch, dot: false });
  }
  return cells;
}

// LED colours the FeatherWing is sold in.
const DISPLAY_COLORS = {
  white: ['White', '#f2f6ff'],
  green: ['Green', '#2bff6a'],
  red: ['Red', '#ff3326'],
  blue: ['Blue', '#3d8bff'],
  yellowgreen: ['Yellow-Green', '#b8ff2e'],
  yellow: ['Yellow', '#ffd21f'],
};

// Segment bitmaps for ASCII 32..127, from Adafruit_LEDBackpack's alphafonttable.
// Bits: 0 A, 1 B, 2 C, 3 D, 4 E, 5 F, 6 G1, 7 G2, 8 H, 9 J, 10 K, 11 L, 12 M, 13 N, 14 DP.
const SEG_FONT = [
  0x0000, 0x0006, 0x0220, 0x12ce, 0x12ed, 0x0c24, 0x235d, 0x0400,
  0x2400, 0x0900, 0x3fc0, 0x12c0, 0x0800, 0x00c0, 0x4000, 0x0c00,
  0x0c3f, 0x0006, 0x00db, 0x008f, 0x00e6, 0x2069, 0x00fd, 0x0007,
  0x00ff, 0x00ef, 0x1200, 0x0a00, 0x2400, 0x00c8, 0x0900, 0x1083,
  0x02bb, 0x00f7, 0x128f, 0x0039, 0x120f, 0x00f9, 0x0071, 0x00bd,
  0x00f6, 0x1209, 0x001e, 0x2470, 0x0038, 0x0536, 0x2136, 0x003f,
  0x00f3, 0x203f, 0x20f3, 0x00ed, 0x1201, 0x003e, 0x0c30, 0x2836,
  0x2d00, 0x1500, 0x0c09, 0x0039, 0x2100, 0x000f, 0x0c03, 0x0008,
  0x0100, 0x1058, 0x2078, 0x00d8, 0x088e, 0x0858, 0x0071, 0x048e,
  0x1070, 0x1000, 0x000e, 0x3600, 0x0030, 0x10d4, 0x1050, 0x00dc,
  0x0170, 0x0486, 0x0050, 0x2088, 0x0078, 0x001c, 0x2004, 0x2814,
  0x28c0, 0x200c, 0x0848, 0x0949, 0x1200, 0x2489, 0x0520, 0x3fff,
];
const segBits = ch => SEG_FONT[ch.charCodeAt(0) - 32] ?? 0;

// One digit's segment shapes in a 56x100 box, indexed by font bit.
const SEG_SHAPES = (() => {
  const T = 7, h = T / 2, g = 1.6;              // thickness, half, gap between segments
  const L = 6, R = 46, C = 26, Y0 = 6, M = 50, Y1 = 94;
  const hs = (x1, x2, y) => `<polygon points="${x1},${y} ${x1 + h},${y - h} ${x2 - h},${y - h} ${x2},${y} ${x2 - h},${y + h} ${x1 + h},${y + h}"/>`;
  const vs = (x, y1, y2) => `<polygon points="${x},${y1} ${x + h},${y1 + h} ${x + h},${y2 - h} ${x},${y2} ${x - h},${y2 - h} ${x - h},${y1 + h}"/>`;
  const dg = (x1, y1, x2, y2) => `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke-width="${T * 0.8}" stroke-linecap="round"/>`;
  return [
    hs(L + g, R - g, Y0),                      // A
    vs(R, Y0 + g, M - g),                      // B
    vs(R, M + g, Y1 - g),                      // C
    hs(L + g, R - g, Y1),                      // D
    vs(L, M + g, Y1 - g),                      // E
    vs(L, Y0 + g, M - g),                      // F
    hs(L + g, C - g, M),                       // G1
    hs(C + g, R - g, M),                       // G2
    dg(L + T, Y0 + T + 1, C - T * 0.6, M - T), // H
    vs(C, Y0 + g + h, M - g - h),              // J
    dg(R - T, Y0 + T + 1, C + T * 0.6, M - T), // K
    dg(C - T * 0.6, M + T, L + T, Y1 - T - 1), // L
    vs(C, M + g + h, Y1 - g - h),              // M
    dg(C + T * 0.6, M + T, R - T, Y1 - T - 1), // N
    `<circle cx="${R + 7}" cy="${Y1}" r="3.6"/>`, // DP
  ];
})();

// SVG markup for a 4-character 14-segment display. chars: [{c, dot}] x4.
function segmentSvg(chars, color) {
  const on = DISPLAY_COLORS[color]?.[1] || DISPLAY_COLORS.red[1];
  const digits = chars.map((cell, i) => {
    const bits = segBits(cell.c) | (cell.dot ? 1 << 14 : 0);
    const segs = SEG_SHAPES.map((s, b) => s.replace(/^<(\w+)/, `<$1 class="${bits & (1 << b) ? 'on' : 'off'}"`)).join('');
    return `<g transform="translate(${14 + i * 64} 10) skewX(-7)">${segs}</g>`;
  }).join('');
  return `<svg viewBox="0 0 280 120" role="img" aria-label="${chars.map(c => c.c).join('').trim()}" style="--seg-on:${on}">
    <rect width="280" height="120" rx="8" class="seg-bg"/>${digits}</svg>`;
}

function drawAlnum(el, text, offset = 0, color = 'red') {
  const cells = toCells(text), n = cells.length, shown = [];
  for (let i = 0; i < 4; i++) {
    const k = n <= 4 ? i - (4 - n) : i + offset;
    shown.push(cells[k] || { c: ' ', dot: false });
  }
  const key = color + JSON.stringify(shown);
  if (el._key === key) return;  // called every animation frame
  el._key = key;
  el.innerHTML = segmentSvg(shown, color);
}
