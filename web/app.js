// clawdputer web demo
// Browser reimplementation of the firmware UI on a 240×135 canvas. Apps
// mirror the layouts and key bindings of firmware/src/apps/*.

// ── persistent storage (localStorage) ──────────────────────────────────────

const STORE_PREFIX = 'clawd:v1:';
const store = {
  get(key, def) {
    try {
      const raw = localStorage.getItem(STORE_PREFIX + key);
      if (raw === null) return def;
      return JSON.parse(raw);
    } catch { return def; }
  },
  set(key, val) {
    try { localStorage.setItem(STORE_PREFIX + key, JSON.stringify(val)); } catch {}
  },
  clearAll() {
    try {
      const keys = [];
      for (let i = 0; i < localStorage.length; i++) {
        const k = localStorage.key(i);
        if (k && k.startsWith(STORE_PREFIX)) keys.push(k);
      }
      keys.forEach(k => localStorage.removeItem(k));
    } catch {}
  },
};

// ── colour helpers ──────────────────────────────────────────────────────────

function rgb565(c) {
  const r = (c >> 11) & 0x1f;
  const g = (c >> 5) & 0x3f;
  const b = c & 0x1f;
  const R = (r << 3) | (r >> 2);
  const G = (g << 2) | (g >> 4);
  const B = (b << 3) | (b >> 2);
  return `rgb(${R},${G},${B})`;
}

const COLOR = {
  WHITE:    rgb565(0xFFFF),
  BLACK:    rgb565(0x0000),
  GREEN:    rgb565(0x07E0),
  RED:      rgb565(0xF800),
  YELLOW:   rgb565(0xFFE0),
  ORANGE:   rgb565(0xFD20),
  CYAN:     rgb565(0x07FF),
  GREY:     rgb565(0xC618),
  DIM:      rgb565(0x8C71),
  STATUSBG: rgb565(0x1082),
  DIVIDER:  rgb565(0x2945),
  PANELBG:  rgb565(0x18E3),
  PANELALT: rgb565(0x2104),
};

// ── m5-style display wrapper around an offscreen 240×135 canvas ────────────

const SCREEN_W = 240;
const SCREEN_H = 135;

class Display {
  constructor() {
    this.buf = document.createElement('canvas');
    this.buf.width = SCREEN_W;
    this.buf.height = SCREEN_H;
    this.ctx = this.buf.getContext('2d');
    this.ctx.imageSmoothingEnabled = false;
    this._size = 1;
    this._color = COLOR.WHITE;
    this._cx = 0; this._cy = 0;
  }
  fillScreen(c) {
    this.ctx.fillStyle = rgb565(c);
    this.ctx.fillRect(0, 0, SCREEN_W, SCREEN_H);
  }
  fillRect(x, y, w, h, c) {
    this.ctx.fillStyle = (typeof c === 'number') ? rgb565(c) : c;
    this.ctx.fillRect(x, y, w, h);
  }
  drawRect(x, y, w, h, c) {
    this.ctx.fillStyle = (typeof c === 'number') ? rgb565(c) : c;
    this.ctx.fillRect(x, y, w, 1);
    this.ctx.fillRect(x, y + h - 1, w, 1);
    this.ctx.fillRect(x, y, 1, h);
    this.ctx.fillRect(x + w - 1, y, 1, h);
  }
  fillRoundRect(x, y, w, h, r, c) {
    const ctx = this.ctx;
    ctx.fillStyle = (typeof c === 'number') ? rgb565(c) : c;
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.lineTo(x + w - r, y);
    ctx.quadraticCurveTo(x + w, y, x + w, y + r);
    ctx.lineTo(x + w, y + h - r);
    ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
    ctx.lineTo(x + r, y + h);
    ctx.quadraticCurveTo(x, y + h, x, y + h - r);
    ctx.lineTo(x, y + r);
    ctx.quadraticCurveTo(x, y, x + r, y);
    ctx.closePath();
    ctx.fill();
  }
  fillCircle(x, y, r, c) {
    this.ctx.fillStyle = (typeof c === 'number') ? rgb565(c) : c;
    this.ctx.beginPath();
    this.ctx.arc(x + 0.5, y + 0.5, r, 0, Math.PI * 2);
    this.ctx.fill();
  }
  drawCircle(x, y, r, c) {
    this.ctx.strokeStyle = (typeof c === 'number') ? rgb565(c) : c;
    this.ctx.lineWidth = 1;
    this.ctx.beginPath();
    this.ctx.arc(x + 0.5, y + 0.5, r, 0, Math.PI * 2);
    this.ctx.stroke();
  }
  drawFastHLine(x, y, w, c) {
    this.ctx.fillStyle = (typeof c === 'number') ? rgb565(c) : c;
    this.ctx.fillRect(x, y, w, 1);
  }
  drawFastVLine(x, y, h, c) {
    this.ctx.fillStyle = (typeof c === 'number') ? rgb565(c) : c;
    this.ctx.fillRect(x, y, 1, h);
  }
  drawLine(x0, y0, x1, y1, c) {
    this.ctx.strokeStyle = (typeof c === 'number') ? rgb565(c) : c;
    this.ctx.lineWidth = 1;
    this.ctx.beginPath();
    this.ctx.moveTo(x0 + 0.5, y0 + 0.5);
    this.ctx.lineTo(x1 + 0.5, y1 + 0.5);
    this.ctx.stroke();
  }
  drawPixel(x, y, c) {
    this.ctx.fillStyle = (typeof c === 'number') ? rgb565(c) : c;
    this.ctx.fillRect(x, y, 1, 1);
  }
  setTextSize(s) { this._size = s; }
  setTextColor(c) { this._color = (typeof c === 'number') ? rgb565(c) : c; }
  setCursor(x, y) { this._cx = x; this._cy = y; }
  // Cardputer's GFXfont at size 1: roughly 6px advance × 8px tall. We use a
  // monospace font sized so each glyph is ~6×s wide / ~8×s tall.
  _font() {
    return `${8 * this._size}px ui-monospace, "SF Mono", Consolas, monospace`;
  }
  _charW() { return 6 * this._size; }
  _charH() { return 8 * this._size; }
  print(s) {
    const ctx = this.ctx;
    ctx.font = this._font();
    ctx.textBaseline = 'top';
    ctx.fillStyle = this._color;
    const str = String(s);
    for (const ch of str) {
      ctx.fillText(ch, this._cx, this._cy);
      this._cx += this._charW();
    }
  }
  printf(fmt, ...args) {
    let i = 0;
    const out = fmt.replace(/%[-0-9]*[dsxu%]|%[-0-9.]*[fs]/g, (m) => {
      if (m === '%%') return '%';
      const v = args[i++];
      if (m.endsWith('d') || m.endsWith('u') || m.endsWith('x')) {
        const n = Number(v) | 0;
        return m.endsWith('x') ? n.toString(16) : String(n);
      }
      return String(v);
    });
    this.print(out);
  }
}

// ── status bar ──────────────────────────────────────────────────────────────

const STATUSBAR_H = 14;

const link = {
  nus: false,       // buddy BLE
  bridgeBle: false, // bridge over BLE
  bridgeTcp: false, // bridge over TCP/WiFi
  wifi: false,
};
const power = { pct: 78, charging: false };

function drawStatusbar(d) {
  d.fillRect(0, 0, SCREEN_W, STATUSBAR_H, COLOR.STATUSBG);
  d.drawFastHLine(0, STATUSBAR_H - 1, SCREEN_W, COLOR.DIVIDER);

  d.setTextSize(1);
  const y = 3;

  d.setTextColor(COLOR.DIM); d.setCursor(4, y); d.print('bd');
  d.fillCircle(20, y + 3, 2, link.nus ? COLOR.GREEN : rgb565(0x4208));

  d.setTextColor(COLOR.DIM); d.setCursor(28, y); d.print('br');
  let brColor = rgb565(0x4208);
  if (link.bridgeBle) brColor = COLOR.GREEN;
  else if (link.bridgeTcp) brColor = COLOR.ORANGE;
  d.fillCircle(44, y + 3, 2, brColor);

  d.setTextColor(COLOR.DIM); d.setCursor(52, y); d.print('wf');
  d.fillCircle(68, y + 3, 2, link.wifi ? COLOR.GREEN : rgb565(0x4208));

  const pct = power.pct;
  const pctBuf = `${pct}%`;
  const pctW = pctBuf.length * 6;
  const iconX = SCREEN_W - 20;
  const pctX = iconX - pctW - 2;
  const pctColor = pct < 20 ? COLOR.RED : (pct < 50 ? COLOR.YELLOW : COLOR.GREY);
  d.setCursor(pctX, y); d.setTextColor(pctColor); d.print(pctBuf);

  // battery icon
  const bw = 16, bh = 8;
  const bx = iconX, by = y - 1;
  d.drawRect(bx, by, bw, bh, COLOR.GREY);
  d.fillRect(bx + bw, by + 2, 2, bh - 4, COLOR.GREY);
  const fillW = ((bw - 2) * pct / 100) | 0;
  const fillColor = pct < 20 ? COLOR.RED : (pct < 50 ? COLOR.YELLOW : COLOR.GREEN);
  if (fillW > 0) d.fillRect(bx + 1, by + 1, fillW, bh - 2, fillColor);

  if (power.charging) {
    const cx = bx + bw / 2, cy = by + bh / 2;
    d.drawLine(cx - 1, cy - 2, cx + 1, cy,     COLOR.WHITE);
    d.drawLine(cx + 1, cy,     cx - 1, cy + 2, COLOR.WHITE);
  }
}

// ── app framework ──────────────────────────────────────────────────────────

const apps = [];
function registerApp(a) { apps.push(a); }
function unregisterApp(id) {
  const i = apps.findIndex(a => a.id === id);
  if (i >= 0) apps.splice(i, 1);
}

let currentApp = null;
let dirty = true;

function requestApp(id) {
  const next = apps.find(a => a.id === id);
  if (!next) return;
  if (currentApp && currentApp.onExit) currentApp.onExit();
  currentApp = next;
  if (currentApp.onEnter) currentApp.onEnter();
  dirty = true;
}

// keysAsArrows for menu apps: ; . , / map to Up Down Left Right
const KEY = { UP: '\x11', DOWN: '\x12', LEFT: '\x13', RIGHT: '\x14', ESC: '\x1B' };

function dispatchKey(ch) {
  if (!currentApp) return;
  if (ch === '\t') { requestApp('home'); return; }
  if (currentApp.keysAsArrows !== false) {
    if (ch === ';') ch = KEY.UP;
    else if (ch === '.') ch = KEY.DOWN;
    else if (ch === ',') ch = KEY.LEFT;
    else if (ch === '/') ch = KEY.RIGHT;
  }
  if (currentApp.onKey) currentApp.onKey(ch);
}

function markDirty() { dirty = true; }

// ── HOME (coverflow launcher) ───────────────────────────────────────────────

function tileBg(a) {
  return ({
    buddy:    0x328A,
    chat:     0x2986,
    ssh:      0x223A,
    settings: 0x4208,
  })[a.id] ?? 0x18E3;
}
function tileAccent(a) {
  return ({
    buddy:    0x07FF,
    chat:     0x5D9F,
    ssh:      0x9CFC,
    settings: 0xC618,
  })[a.id] ?? 0xFFFF;
}

const home = {
  id: 'home', name: 'Home', description: 'Launcher',
  sel: store.get('home.sel', 0),
  tiles: [],
  rebuild() {
    this.tiles = apps.filter(a => a.id !== 'home' && !a.hidden);
    if (this.sel >= this.tiles.length) this.sel = 0;
  },
  saveSel() { store.set('home.sel', this.sel); },
  onEnter() { this.rebuild(); markDirty(); },
  onExit() {},
  onTick() { /* nothing — status updates dirty via markDirty */ },
  onKey(ch) {
    const n = this.tiles.length;
    if (!n) return;
    if (ch === KEY.LEFT || ch === KEY.UP) { this.sel = (this.sel - 1 + n) % n; this.saveSel(); markDirty(); }
    else if (ch === KEY.RIGHT || ch === KEY.DOWN) { this.sel = (this.sel + 1) % n; this.saveSel(); markDirty(); }
    else if (ch === '\n') { requestApp(this.tiles[this.sel].id); }
    else if (ch >= '1' && ch <= '9') {
      const idx = ch.charCodeAt(0) - 49;
      if (idx < n) { this.sel = idx; this.saveSel(); requestApp(this.tiles[idx].id); }
    }
  },
  onDraw(d) {
    const CARD_W = 140, CARD_H = 88;
    const CARD_X = ((SCREEN_W - CARD_W) / 2) | 0;
    const CARD_Y = STATUSBAR_H + 4;
    const PEEK_W = 22, PEEK_H = 70;
    const LEFT_PEEK_X = 4, RIGHT_PEEK_X = SCREEN_W - PEEK_W - 4;
    const PEEK_Y = CARD_Y + ((CARD_H - PEEK_H) / 2) | 0;
    const FOOTER_Y = 124;

    d.fillScreen(0x0000);
    drawStatusbar(d);

    if (!this.tiles.length) {
      d.setTextColor(COLOR.DIM); d.setCursor(50, 60); d.print('no apps registered');
      return;
    }

    const n = this.tiles.length;
    const drawPeek = (x, a) => {
      d.fillRoundRect(x, PEEK_Y, PEEK_W, PEEK_H, 4, tileBg(a));
      d.setTextSize(2); d.setTextColor(tileAccent(a));
      d.setCursor(x + 6, PEEK_Y + 28);
      d.print(a.name[0]);
    };
    if (n > 1) {
      const prev = (this.sel - 1 + n) % n;
      const next = (this.sel + 1) % n;
      drawPeek(LEFT_PEEK_X, this.tiles[prev]);
      drawPeek(RIGHT_PEEK_X, this.tiles[next]);
    }

    const a = this.tiles[this.sel];
    const bg = tileBg(a), accent = tileAccent(a);
    d.fillRoundRect(CARD_X - 2, CARD_Y - 2, CARD_W + 4, CARD_H + 4, 6, accent);
    d.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 5, bg);

    d.setTextSize(3); d.setTextColor(accent);
    d.setCursor(CARD_X + 10, CARD_Y + 14); d.print(a.name[0]);

    d.setTextSize(2); d.setTextColor(COLOR.WHITE);
    d.setCursor(CARD_X + 42, CARD_Y + 18); d.print(a.name);

    if (a.description) {
      d.setTextSize(1);
      const max = ((CARD_W - 16) / 6) | 0;
      const words = a.description.split(' ');
      let line = '';
      const lines = [];
      for (const w of words) {
        const next = line ? line + ' ' + w : w;
        if (next.length > max) { lines.push(line); line = w; }
        else line = next;
      }
      if (line) lines.push(line);
      for (let i = 0; i < Math.min(2, lines.length); i++) {
        d.setCursor(CARD_X + 8, CARD_Y + 46 + i * 10);
        d.setTextColor(COLOR.WHITE);
        d.print(lines[i]);
      }
    }
    d.setTextSize(1); d.setTextColor(accent);
    d.setCursor(CARD_X + 8, CARD_Y + CARD_H - 10);
    d.print('press enter »');

    // dots
    if (n > 1) {
      const spacing = 8;
      const dotW = spacing * (n - 1) + 4;
      const dotX = ((SCREEN_W - dotW) / 2) | 0;
      const dotY = CARD_Y + CARD_H + 6;
      for (let i = 0; i < n; i++) {
        const sel = i === this.sel;
        const cx = dotX + i * spacing + 2;
        if (sel) d.fillCircle(cx, dotY, 2, COLOR.WHITE);
        else d.fillCircle(cx, dotY, 1, rgb565(0x4208));
      }
    }

    // footer
    d.fillRect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, COLOR.STATUSBG);
    d.drawFastHLine(0, FOOTER_Y, SCREEN_W, COLOR.DIVIDER);
    d.setTextColor(COLOR.DIM); d.setTextSize(1);
    d.setCursor(4, FOOTER_Y + 3);
    d.print(',/ switch  enter launch  1-9 jump');
  },
};
registerApp(home);

// ── BUDDY ───────────────────────────────────────────────────────────────────

const CRAB_BASE = [
  0b00100000100,
  0b00010001000,
  0b00111111100,
  0b01101110110,
  0b11111111111,
  0b10111111101,
  0b10100000101,
  0b00011001100,
];
const CRAB_ALT = [
  0b00100000100,
  0b00010001000,
  0b00111111100,
  0b01101110110,
  0b11111111111,
  0b10111111101,
  0b10100000101,
  0b11000110011,
];

function crabSkin(s) {
  switch (s) {
    case 'sleep':     return [0x6800, 0x2000];
    case 'idle':      return [0xF800, 0xC800];
    case 'busy':      return [0xFD20, 0xC400];
    case 'attention': return [0xF800, 0xFFE0];
    case 'celebrate': return [0xFC1F, 0xFFE0];
    case 'heart':     return [0xFC1F, 0xF81F];
  }
  return [0xF800, 0xC800];
}

function drawCrab(d, ox, oy, state, now) {
  const skin = crabSkin(state);
  const alt = ((now / 350) & 1) !== 0;
  const rows = alt ? CRAB_ALT : CRAB_BASE;
  const blinkClosed = state !== 'sleep' && ((Math.floor(now / 400)) % 12 === 0);
  let bob = 0;
  if (state === 'idle' && Math.floor(now / 600) % 2 === 0) bob = -1;
  if (state === 'celebrate') bob = -Math.floor(now / 120) % 3;
  const flash = state === 'attention' && ((now / 250) & 1);
  const body = flash ? COLOR.YELLOW : rgb565(skin[0]);

  const PIX = 4;
  for (let row = 0; row < 8; row++) {
    const mask = rows[row];
    for (let col = 0; col < 11; col++) {
      if (!(mask & (1 << (10 - col)))) continue;
      d.fillRect(ox + col * PIX, oy + row * PIX + bob, PIX, PIX, body);
    }
  }
  // eyes
  const eyeY = oy + 3 * PIX + bob;
  const eyeLx = ox + 3 * PIX + 1;
  const eyeRx = ox + 7 * PIX + 1;
  const closed = blinkClosed || state === 'sleep';
  if (closed) {
    d.fillRect(eyeLx, eyeY + 1, PIX - 2, 1, COLOR.GREY);
    d.fillRect(eyeRx, eyeY + 1, PIX - 2, 1, COLOR.GREY);
  } else {
    d.fillRect(eyeLx, eyeY, PIX - 2, PIX - 1, COLOR.WHITE);
    d.fillRect(eyeRx, eyeY, PIX - 2, PIX - 1, COLOR.WHITE);
    if (state === 'heart') {
      d.fillRect(eyeLx, eyeY + 1, PIX - 2, PIX - 2, rgb565(0xF81F));
      d.fillRect(eyeRx, eyeY + 1, PIX - 2, PIX - 2, rgb565(0xF81F));
    } else {
      d.drawPixel(eyeLx, eyeY + 1, COLOR.BLACK);
      d.drawPixel(eyeRx, eyeY + 1, COLOR.BLACK);
    }
  }
  if (state === 'sleep') {
    d.setTextSize(1); d.setTextColor(COLOR.GREY);
    d.setCursor(ox + 38, oy); d.print('z');
    d.setCursor(ox + 42, oy - 4); d.print('Z');
  } else if (state === 'attention') {
    d.setTextSize(2); d.setTextColor(COLOR.YELLOW);
    d.setCursor(ox + 18, oy - 12); d.print('!');
  } else if (state === 'celebrate') {
    d.fillRect(ox + 2, oy - 3, 2, 2, COLOR.YELLOW);
    d.fillRect(ox + 38, oy - 1, 2, 2, COLOR.CYAN);
    d.fillRect(ox + 20, oy - 6, 2, 2, rgb565(0xF81F));
  } else if (state === 'heart') {
    d.fillRect(ox + 19, oy - 5, 2, 2, rgb565(0xF81F));
    d.fillRect(ox + 23, oy - 5, 2, 2, rgb565(0xF81F));
    d.fillRect(ox + 18, oy - 4, 8, 2, rgb565(0xF81F));
    d.fillRect(ox + 19, oy - 2, 6, 1, rgb565(0xF81F));
    d.fillRect(ox + 20, oy - 1, 4, 1, rgb565(0xF81F));
    d.fillRect(ox + 21, oy, 2, 1, rgb565(0xF81F));
  }
}

const BUDDY_PROMPT_POOL = [
  { tool: 'mcp__github__list_pull_requests',     hint: 'repo: CHaerem/clawdputer' },
  { tool: 'Bash',                                hint: 'pio run -e cardputer -t upload' },
  { tool: 'Edit',                                hint: 'firmware/src/apps/chat/chat.cpp' },
  { tool: 'mcp__github__create_pull_request',    hint: 'web: persistent demo memory' },
  { tool: 'Read',                                hint: 'host/Sources/clawd-bridge/Bridge.swift' },
  { tool: 'WebFetch',                            hint: 'docs.m5stack.com/en/core/Cardputer-Adv' },
  { tool: 'Bash',                                hint: 'swift build && swift test' },
  { tool: 'Write',                               hint: 'firmware/src/apps/notes/notes.cpp' },
  { tool: 'mcp__github__add_issue_comment',      hint: 'issue #42: OTA rollback edge case' },
  { tool: 'Grep',                                hint: 'pattern: REGISTER_APP, glob: **/*.cpp' },
];

const buddy = {
  id: 'buddy', name: 'Buddy', description: 'Claude Desktop companion',
  state: 'idle', pet: 'sleep',
  owner: store.get('buddy.owner', 'christopher'),
  tokens: store.get('buddy.tokens', 12480),
  total: 3, running: 1, waiting: 0,
  prompt: store.get('buddy.prompt', null),
  approvals: store.get('buddy.approvals', 0),
  denials: store.get('buddy.denials', 0),
  enteredAt: 0,
  lastPromptAt: 0,
  nextPromptDelay: 0,
  petUntil: 0,
  save() {
    store.set('buddy.tokens', this.tokens);
    store.set('buddy.approvals', this.approvals);
    store.set('buddy.denials', this.denials);
    store.set('buddy.prompt', this.prompt);
  },
  scheduleNext(firstOnEnter = false) {
    this.lastPromptAt = performance.now();
    this.nextPromptDelay = firstOnEnter
      ? 4500 + Math.random() * 2000
      : 18000 + Math.random() * 22000;
  },
  newPrompt() {
    const t = BUDDY_PROMPT_POOL[Math.floor(Math.random() * BUDDY_PROMPT_POOL.length)];
    this.prompt = { id: 'p' + Date.now().toString(36), tool: t.tool, hint: t.hint };
    this.pet = 'attention';
    this.save();
  },
  onEnter() {
    this.enteredAt = performance.now();
    link.nus = false;
    this.scheduleNext(!this.prompt);
    markDirty();
  },
  onExit() {},
  onTick() {
    const now = performance.now();
    const t = now - this.enteredAt;
    if (!link.nus && t > 1500) { link.nus = true; markDirty(); }
    if (link.nus && !this.prompt && (now - this.lastPromptAt) > this.nextPromptDelay) {
      this.newPrompt();
      markDirty();
    }
    // token meter tick — background activity
    if (link.nus && Math.random() < 0.04) {
      this.tokens += Math.floor(Math.random() * 25);
      store.set('buddy.tokens', this.tokens);
    }
    if (this.petUntil && now > this.petUntil) {
      this.petUntil = 0;
      this.pet = this.prompt ? 'attention' : 'idle';
    }
    markDirty();
  },
  onKey(ch) {
    if (!this.prompt) {
      if (ch === 'u' || ch === 'U') requestApp('usage');
      return;
    }
    if (ch === '\n' || ch === 'y' || ch === 'Y') {
      this.approvals++; this.prompt = null;
      this.pet = 'heart';
      this.petUntil = performance.now() + 2200;
      this.scheduleNext();
      this.save();
      markDirty();
    } else if (ch === '\b' || ch === 'n' || ch === 'N' || ch === KEY.ESC) {
      this.denials++; this.prompt = null;
      this.pet = 'idle';
      this.scheduleNext();
      this.save();
      markDirty();
    }
  },
  onDraw(d) {
    const now = performance.now();
    d.fillScreen(0x0000);
    drawStatusbar(d);

    // pet sprite top-right
    const petState = !link.nus ? 'sleep'
      : (this.prompt ? 'attention' : (this.pet === 'heart' ? 'heart' : (this.running > 0 ? 'busy' : 'idle')));
    drawCrab(d, SCREEN_W - 50, STATUSBAR_H + 14, petState, now);

    let y = STATUSBAR_H + 4;
    d.setTextSize(1);

    if (!link.nus) {
      d.setTextColor(COLOR.DIM); d.setCursor(6, y); d.print('advertising as'); y += 11;
      d.setTextColor(COLOR.WHITE); d.setCursor(6, y); d.print('Claude-Cardputer-a4f2'); y += 16;
      d.setTextColor(COLOR.DIM); d.setCursor(6, y); d.print('Claude > Developer >'); y += 10;
      d.setCursor(6, y); d.print('Open Hardware Buddy');
      return;
    }

    d.setTextColor(COLOR.GREEN); d.setCursor(6, y); d.print('connected');
    d.setTextColor(COLOR.WHITE); d.print(' ' + this.owner);
    y += 12;

    if (this.prompt) {
      d.fillRoundRect(4, y, SCREEN_W - 8, 80, 4, rgb565(0x4800));
      d.setTextColor(COLOR.WHITE); d.setTextSize(2);
      d.setCursor(10, y + 4); d.print('APPROVE?');
      d.setTextSize(1);
      d.setCursor(10, y + 26); d.setTextColor(COLOR.YELLOW); d.print(this.prompt.tool);
      d.setCursor(10, y + 38); d.setTextColor(COLOR.WHITE); d.print(this.prompt.hint);
      d.setCursor(10, y + 58); d.setTextColor(COLOR.GREEN); d.print('[Y] approve');
      d.setCursor(110, y + 58); d.setTextColor(COLOR.RED); d.print('[N] deny');
      return;
    }

    d.setTextColor(COLOR.DIM); d.setCursor(6, y); d.print('sessions');
    d.setCursor(64, y); d.setTextColor(COLOR.WHITE);
    d.print(`${this.total} (r${this.running} w${this.waiting})`);
    y += 11;

    d.setTextColor(COLOR.DIM); d.setCursor(6, y); d.print('tokens');
    d.setCursor(64, y); d.setTextColor(COLOR.WHITE); d.print(`${this.tokens} today`);
    y += 11;

    d.setTextColor(COLOR.YELLOW); d.setCursor(6, y); d.print('streaming bash tool output…');

    d.fillRect(0, 124, SCREEN_W, 11, COLOR.STATUSBG);
    d.drawFastHLine(0, 124, SCREEN_W, COLOR.DIVIDER);
    d.setTextColor(COLOR.DIM); d.setCursor(4, 127);
    d.print(`a:${this.approvals} d:${this.denials}  u:usage  tab:home`);
  },
};
registerApp(buddy);

// ── CHAT ────────────────────────────────────────────────────────────────────

// Knowledge-base for the simulated `claude` CLI. Each entry has a regex,
// optional fake tool-use statuses, and a body. The body is streamed
// chunk-by-chunk to mimic the real stream-json output.
const CHAT_KB = [
  {
    match: /\b(register|registry|new app|add.*app|REGISTER_APP|how.*app)\b/i,
    tools: ['Read', 'Grep'],
    body:
      'Each firmware app lives in firmware/src/apps/<name>/<name>.cpp and ends with ' +
      'REGISTER_APP(my_app). The registrar runs before main() (static init), so the ' +
      'home launcher\'s tile list is fully populated by the time setup() returns.\n\n' +
      'Adding a new one: define a struct App { .id, .name, .description, .onEnter, ' +
      '.onTick, .onKey, .onDraw, .services }, call REGISTER_APP — done. Set .services ' +
      'to SVC_BLE | SVC_WIFI | SVC_SD as needed; the framework starts/stops radios on ' +
      'app switch.',
  },
  {
    match: /\b(buddy|nus|claude desktop|approve|deny|companion)\b/i,
    tools: ['Read'],
    body:
      'Buddy speaks Anthropic\'s buddy protocol over the Nordic UART Service (NUS) ' +
      'BLE service. Claude Desktop is the only peer. The device is read-only for ' +
      'status and read/write only for permission decisions (approve/deny tool prompts). ' +
      'It runs in parallel with the clawd-bridge service on the same peripheral, so ' +
      'Desktop and the bridge can both be connected at the same time.',
  },
  {
    match: /\b(bridge|clawd-bridge|host daemon|mac side|swift|cli)\b/i,
    tools: ['Read'],
    body:
      'clawd-bridge is a Swift daemon on the Mac (CoreBluetooth + a TCP listener for ' +
      'WiFi fallback). On every chat turn it spawns `claude -p --output-format ' +
      'stream-json --include-partial-messages` (with --continue after the first turn), ' +
      'parses the NDJSON events, and forwards text deltas + tool-use markers back to ' +
      'the device. Conversation history lives wherever the CLI keeps it, keyed by cwd.',
  },
  {
    match: /\b(ota|update|self.?update|gitops|flash|firmware\.bin)\b/i,
    tools: ['Read', 'WebFetch'],
    body:
      'Every push to main that touches firmware/** runs .github/workflows/firmware.yml. ' +
      'It builds firmware.bin + version.txt and uploads them to the GitHub `latest` ' +
      'release. On the device, services/updater compares CLAWD_BUILD_SHA against the ' +
      'manifest, streams the new image into the inactive OTA partition via HTTPUpdate, ' +
      'and reboots. Rollback: pending=true is written to NVS before flashing — three ' +
      'unhealthy boots in a row flips the boot partition back to the previous slot.',
  },
  {
    match: /\b(ssh|sealed|host preset|ed25519)\b/i,
    tools: ['Read'],
    body:
      'The SSH app uses LibSSH with an on-device Ed25519 keypair (generated once, ' +
      'stored in NVS). Host presets are AES-256-GCM-sealed against the per-device ' +
      'seal key — only the device that generated the key can decrypt them, so the ' +
      'sealed blob can safely live in the repo at firmware/secrets/ssh_hosts.sealed.',
  },
  {
    match: /\b(wifi|network|mdns|secret)\b/i,
    tools: ['Read'],
    body:
      'WiFi credentials persist in NVS. The wifi app scans, lets you pick an SSID, ' +
      'and saves on success. mDNS advertises the device as clawdputer.local so OTA ' +
      'and the bridge\'s TCP fallback can find it without hardcoded IPs.',
  },
  {
    match: /\b(seal|secret|aes|gcm|identity)\b/i,
    tools: ['Read'],
    body:
      'Sealed secrets are AES-256-GCM with a 32-byte key generated once per device ' +
      'and stored in NVS namespace `identity`. Multi-device support would mean ' +
      'deriving the seal key from the eFuse MAC (weaker — anyone with the MAC can ' +
      'decrypt). Single-developer setup so per-device sealing is fine.',
  },
  {
    match: /\b(retro|game|snake|tetris|gameboy|gb|rom)\b/i,
    tools: ['Read'],
    body:
      'Two native games (Snake, Tetris) ship in the launcher today. The Game Boy ' +
      'emulator stack was dropped — streaming ROMs from SD ended up too constrained ' +
      'on this hardware. The native games hit 60fps comfortably with the backbuffer ' +
      'sprite UI primitive.',
  },
  {
    match: /\b(memory|persist|browser|localstorage|demo)\b/i,
    tools: [],
    body:
      'This is the web demo — everything you do here lives in your browser\'s ' +
      'localStorage. Chat history, SSH hosts you add, Buddy approve/deny counts, ' +
      'and Settings selections all survive a page reload. Wipe it any time from ' +
      'Settings → "reset demo memory".',
  },
  {
    match: /\b(hi|hello|hey|sup|hola|yo)\b/i,
    tools: [],
    body:
      'Hi! I\'m the simulated clawdputer chat — try asking about the app registry, ' +
      'the bridge daemon, the OTA flow, sealed SSH presets, or how the buddy ' +
      'protocol works. Whatever you type stays in your browser.',
  },
  {
    match: /\b(help|what can|capabilities|features)\b/i,
    tools: [],
    body:
      'Things to ask the simulated CLI:\n' +
      '  • how does the app registry work\n' +
      '  • what is the bridge\n' +
      '  • how does ota work\n' +
      '  • how are ssh hosts sealed\n' +
      '  • tell me about the buddy protocol\n' +
      'Everything streams just like the real `claude -p` output.',
  },
];

const CHAT_FALLBACK = {
  tools: ['Read', 'Grep'],
  body:
    'I don\'t have a canned answer for that in the demo KB, but the real device runs ' +
    '`claude -p --output-format stream-json` through clawd-bridge and would give you ' +
    'a proper answer here. Try asking about: apps, buddy, the bridge, OTA, SSH, or ' +
    'sealed secrets.',
};

function chunkBody(body) {
  // Stream in word groups of 4–10 chars to look like real token deltas
  const out = [];
  let i = 0;
  while (i < body.length) {
    const step = 4 + Math.floor(Math.random() * 12);
    let end = Math.min(i + step, body.length);
    // try not to split inside a word
    if (end < body.length && body[end] !== ' ' && body[end] !== '\n') {
      const nextSpace = body.indexOf(' ', end);
      if (nextSpace !== -1 && nextSpace - end < 8) end = nextSpace;
    }
    out.push(body.slice(i, end));
    i = end;
  }
  return out;
}

const DEFAULT_CHAT_LINES = [
  { text: 'bridge connected (tcp via mDNS)', kind: 'status' },
  { text: 'try: "how does the app registry work?"', kind: 'status' },
];

const chat = {
  id: 'chat', name: 'Chat', description: 'Claude CLI remote',
  keysAsArrows: false,
  lines: store.get('chat.lines', null) || DEFAULT_CHAT_LINES.slice(),
  input: '', busy: false,
  scriptQueue: [], scriptTimer: null,
  save() { store.set('chat.lines', this.lines.slice(-200)); },
  onEnter() {
    link.wifi = true;
    link.bridgeTcp = true;
    if (!this.lines.length) this.lines = DEFAULT_CHAT_LINES.slice();
    this.input = '';
    this.busy = false;
    this.scriptQueue = [];
    markDirty();
  },
  onExit() {
    if (this.scriptTimer) { clearTimeout(this.scriptTimer); this.scriptTimer = null; }
  },
  pushLine(text, kind) {
    this.lines.push({ text, kind });
    if (this.lines.length > 200) this.lines.shift();
    this.save();
    markDirty();
  },
  appendAssistant(chunk) {
    const last = this.lines[this.lines.length - 1];
    if (last && last.kind === 'assistant') { last.text += chunk; }
    else this.lines.push({ text: chunk, kind: 'assistant' });
    if (this.lines.length > 200) this.lines.shift();
    this.save();
    markDirty();
  },
  pickReply(prompt) {
    for (const entry of CHAT_KB) {
      if (entry.match.test(prompt)) return entry;
    }
    return CHAT_FALLBACK;
  },
  buildScript(prompt) {
    const reply = this.pickReply(prompt);
    const queue = [];
    queue.push({ status: 'connecting to clawd-bridge…', delay: 300 });
    for (const t of reply.tools) queue.push({ status: '⚙ ' + t, delay: 380 });
    for (const c of chunkBody(reply.body)) {
      queue.push({ chunk: c, delay: 70 + Math.random() * 90 });
    }
    const tokens = Math.max(40, Math.floor(reply.body.length / 4));
    queue.push({ status: `[${tokens} tokens]`, delay: 200 });
    return queue;
  },
  scheduleNext() {
    if (!this.scriptQueue.length) {
      this.busy = false; this.save(); markDirty(); return;
    }
    const step = this.scriptQueue.shift();
    this.scriptTimer = setTimeout(() => {
      if (currentApp !== chat) {
        this.busy = false; this.scriptQueue = []; return;
      }
      if (step.chunk) this.appendAssistant(step.chunk);
      else if (step.status) this.pushLine(step.status, 'status');
      this.scheduleNext();
    }, step.delay);
  },
  send() {
    if (!this.input.trim()) return;
    if (this.busy) return;
    const prompt = this.input;
    this.pushLine(prompt, 'user');
    this.input = '';
    if (/^\/clear\b/i.test(prompt)) {
      this.lines = DEFAULT_CHAT_LINES.slice();
      this.save();
      return;
    }
    this.busy = true;
    this.scriptQueue = this.buildScript(prompt);
    this.scheduleNext();
  },
  onTick() { markDirty(); /* cursor blink */ },
  onKey(ch) {
    if (ch === '\n') this.send();
    else if (ch === '\b') {
      if (this.input.length) this.input = this.input.slice(0, -1);
      markDirty();
    } else if (ch >= ' ' && ch.charCodeAt(0) <= 0x7E) {
      this.input += ch; markDirty();
    }
  },
  onDraw(d) {
    const LINE_H = 10, MAX_W = 38, INPUT_H = 14;
    const TOP_Y = STATUSBAR_H + 4;
    d.fillScreen(0x0000);
    drawStatusbar(d);

    if (this.busy) {
      d.setTextSize(1); d.setTextColor(COLOR.YELLOW);
      d.setCursor(4, TOP_Y); d.print('streaming…');
    }

    const bottomY = SCREEN_H - INPUT_H;

    const wrap = (s, maxW) => {
      const out = [];
      const segs = s.split('\n');
      for (const seg of segs) {
        if (!seg) { out.push(''); continue; }
        let i = 0;
        while (i < seg.length) {
          let end = Math.min(i + maxW, seg.length);
          if (end < seg.length) {
            const spc = seg.lastIndexOf(' ', end);
            if (spc > i) end = spc;
          }
          out.push(seg.substring(i, end));
          i = end;
          while (i < seg.length && seg[i] === ' ') i++;
        }
      }
      return out;
    };
    const colorFor = (k) => ({
      user: COLOR.GREEN, assistant: COLOR.WHITE,
      status: rgb565(0x7BEF), error: COLOR.RED,
    })[k];

    const rows = [];
    let rowsLeft = Math.floor((bottomY - TOP_Y) / LINE_H);
    for (let i = this.lines.length - 1; i >= 0 && rowsLeft > 0; i--) {
      const w = wrap(this.lines[i].text, MAX_W);
      for (let j = w.length - 1; j >= 0 && rowsLeft > 0; j--) {
        rows.push({ text: w[j], kind: this.lines[i].kind });
        rowsLeft--;
      }
    }
    let yLine = bottomY - LINE_H;
    d.setTextSize(1);
    for (const r of rows) {
      d.setCursor(2, yLine); d.setTextColor(colorFor(r.kind)); d.print(r.text);
      yLine -= LINE_H;
    }

    d.fillRect(0, SCREEN_H - INPUT_H, SCREEN_W, INPUT_H, COLOR.PANELALT);
    d.setTextColor(COLOR.WHITE);
    d.setCursor(2, SCREEN_H - INPUT_H + 3); d.print('> ');
    let inp = this.input;
    if (inp.length > 36) inp = '…' + inp.slice(-35);
    d.print(inp);
    if (Math.floor(performance.now() / 500) % 2 === 0) d.print('_');
  },
};
registerApp(chat);

// ── SSH ─────────────────────────────────────────────────────────────────────

const SSH_SEALED_HOSTS = [
  { name: 'mac-mini',  host: '10.0.0.12',  user: 'chris',  source: 'sealed' },
  { name: 'studio',    host: '10.0.0.21',  user: 'chris',  source: 'sealed' },
  { name: 'pi-pixel',  host: '10.0.0.40',  user: 'pi',     source: 'sealed' },
];

const SSH_FIELDS = [
  { key: 'name', label: 'name',     hint: 'short label' },
  { key: 'host', label: 'host/ip',  hint: '10.0.0.x or name.local' },
  { key: 'user', label: 'username', hint: 'remote login user' },
];

const ssh = {
  id: 'ssh', name: 'SSH', description: 'Key-auth SSH client',
  keysAsArrows: false,
  userHosts: store.get('ssh.userHosts', []),
  sel: 0,
  connecting: null,
  mode: 'list',
  form: { name: '', host: '', user: '' },
  formIdx: 0,
  hosts() {
    return SSH_SEALED_HOSTS.concat(this.userHosts).concat([
      { name: '+ add new host…', host: '', user: '', source: 'action' },
    ]);
  },
  save() { store.set('ssh.userHosts', this.userHosts); },
  onEnter() {
    this.sel = 0; this.connecting = null;
    this.mode = 'list';
    this.form = { name: '', host: '', user: '' };
    this.formIdx = 0;
    markDirty();
  },
  onExit() {},
  onTick() {},
  enterForm() {
    this.mode = 'form';
    this.form = { name: '', host: '', user: '' };
    this.formIdx = 0;
    markDirty();
  },
  saveForm() {
    const f = this.form;
    if (!f.name.trim() || !f.host.trim() || !f.user.trim()) return false;
    this.userHosts.push({
      name: f.name.trim(),
      host: f.host.trim(),
      user: f.user.trim(),
      source: 'saved',
    });
    this.save();
    this.mode = 'list';
    this.sel = SSH_SEALED_HOSTS.length + this.userHosts.length - 1;
    markDirty();
    return true;
  },
  deleteSel() {
    const sealedN = SSH_SEALED_HOSTS.length;
    const idx = this.sel - sealedN;
    if (idx < 0 || idx >= this.userHosts.length) return;
    this.userHosts.splice(idx, 1);
    this.save();
    const all = this.hosts();
    if (this.sel >= all.length) this.sel = all.length - 1;
    markDirty();
  },
  onKey(ch) {
    if (this.connecting) {
      if (ch === '\b' || ch === KEY.ESC) { this.connecting = null; markDirty(); }
      return;
    }
    if (this.mode === 'form') {
      // navigate or type
      if (ch === KEY.UP || ch === ';') {
        this.formIdx = (this.formIdx - 1 + SSH_FIELDS.length) % SSH_FIELDS.length;
        markDirty();
      } else if (ch === KEY.DOWN || ch === '.') {
        this.formIdx = (this.formIdx + 1) % SSH_FIELDS.length;
        markDirty();
      } else if (ch === '\n') {
        if (this.formIdx < SSH_FIELDS.length - 1) {
          this.formIdx++;
        } else if (!this.saveForm()) {
          // incomplete — flash by leaving as-is
        }
        markDirty();
      } else if (ch === '\b') {
        const k = SSH_FIELDS[this.formIdx].key;
        if (this.form[k].length) this.form[k] = this.form[k].slice(0, -1);
        else this.mode = 'list';
        markDirty();
      } else if (ch === KEY.ESC) {
        this.mode = 'list'; markDirty();
      } else if (ch >= ' ' && ch.charCodeAt(0) <= 0x7E) {
        const k = SSH_FIELDS[this.formIdx].key;
        this.form[k] += ch;
        markDirty();
      }
      return;
    }
    // list mode — arrow translation by hand (we disabled keysAsArrows)
    const hosts = this.hosts();
    if (ch === KEY.UP || ch === ';') { this.sel = (this.sel - 1 + hosts.length) % hosts.length; markDirty(); }
    else if (ch === KEY.DOWN || ch === '.') { this.sel = (this.sel + 1) % hosts.length; markDirty(); }
    else if (ch === '\n') {
      const h = hosts[this.sel];
      if (h.source === 'action') { this.enterForm(); return; }
      this.connecting = h;
      markDirty();
      setTimeout(() => {
        if (currentApp === ssh) { this.connecting = null; markDirty(); }
      }, 1800);
    } else if (ch === 'd' || ch === 'D') {
      // delete user-added host
      const h = hosts[this.sel];
      if (h && h.source === 'saved') this.deleteSel();
    }
  },
  onDraw(d) {
    d.fillScreen(0x0000);
    drawStatusbar(d);

    if (this.connecting) {
      d.setTextSize(1); d.setTextColor(COLOR.YELLOW);
      d.setCursor(6, STATUSBAR_H + 20);
      d.print(`connecting to ${this.connecting.name}…`);
      d.setTextColor(COLOR.DIM);
      d.setCursor(6, STATUSBAR_H + 36);
      d.print(`${this.connecting.user}@${this.connecting.host}`);
      d.setCursor(6, STATUSBAR_H + 52);
      d.print('ed25519 key auth');
      d.setCursor(6, STATUSBAR_H + 80);
      d.setTextColor(COLOR.GREY);
      d.print('(simulated — no network in demo)');
      d.fillRect(0, 124, SCREEN_W, 11, COLOR.STATUSBG);
      d.drawFastHLine(0, 124, SCREEN_W, COLOR.DIVIDER);
      d.setTextColor(COLOR.DIM); d.setCursor(4, 127);
      d.print('backspace cancel  tab:home');
      return;
    }

    if (this.mode === 'form') {
      let y = STATUSBAR_H + 4;
      d.setTextSize(1); d.setTextColor(COLOR.ORANGE);
      d.setCursor(4, y); d.print('add host'); y += 14;
      for (let i = 0; i < SSH_FIELDS.length; i++) {
        const f = SSH_FIELDS[i];
        const active = i === this.formIdx;
        const val = this.form[f.key];
        if (active) d.fillRect(0, y - 1, SCREEN_W, 14, rgb565(0x2104));
        d.setTextColor(active ? COLOR.YELLOW : COLOR.DIM);
        d.setCursor(6, y + 1); d.print(f.label);
        d.setTextColor(active ? COLOR.WHITE : COLOR.GREY);
        d.setCursor(70, y + 1);
        let shown = val;
        if (shown.length > 26) shown = '…' + shown.slice(-25);
        if (!shown && !active) { d.setTextColor(COLOR.DIM); d.print(f.hint); }
        else { d.print(shown); }
        if (active && Math.floor(performance.now() / 500) % 2 === 0) d.print('_');
        y += 14;
      }
      d.fillRect(0, 124, SCREEN_W, 11, COLOR.STATUSBG);
      d.drawFastHLine(0, 124, SCREEN_W, COLOR.DIVIDER);
      d.setTextColor(COLOR.DIM); d.setCursor(4, 127);
      d.print(';. field  enter next/save  bksp cancel');
      return;
    }

    let y = STATUSBAR_H + 4;
    d.setTextSize(1); d.setTextColor(COLOR.DIM);
    d.setCursor(4, y); d.print('hosts'); y += 12;
    const hosts = this.hosts();
    const ROW_H = 11;
    const visible = Math.floor((124 - y) / ROW_H);
    let first = 0;
    if (this.sel >= visible) first = this.sel - visible + 1;
    for (let i = 0; i < visible; i++) {
      const idx = first + i;
      if (idx >= hosts.length) break;
      const h = hosts[idx];
      const sel = idx === this.sel;
      if (sel) d.fillRect(0, y - 1, SCREEN_W, ROW_H, rgb565(0x2104));
      d.setTextColor(sel ? COLOR.WHITE : COLOR.GREY);
      d.setCursor(8, y + 1); d.print(h.name);
      if (h.source !== 'action') {
        d.setTextColor(sel ? COLOR.DIM : rgb565(0x5ACB));
        const lbl = `${h.user}@${h.host}`;
        const x = SCREEN_W - lbl.length * 6 - 4;
        d.setCursor(x, y + 1); d.print(lbl);
      }
      if (h.source === 'saved' && sel) {
        d.setTextColor(COLOR.ORANGE);
        d.setCursor(2, y + 1); d.print('•');
      }
      y += ROW_H;
    }
    d.fillRect(0, 124, SCREEN_W, 11, COLOR.STATUSBG);
    d.drawFastHLine(0, 124, SCREEN_W, COLOR.DIVIDER);
    d.setTextColor(COLOR.DIM); d.setCursor(4, 127);
    const cur = hosts[this.sel];
    if (cur && cur.source === 'saved') d.print(';. select  enter connect  d delete');
    else d.print(';. select  enter connect  tab:home');
  },
};
registerApp(ssh);


// ── SETTINGS ────────────────────────────────────────────────────────────────

function fmtUptime(ms) {
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const pad = (n) => String(n).padStart(2, '0');
  return `${pad(h)}:${pad(m)}:${pad(sec)}`;
}

const SESSION_START = performance.now();

const settings = {
  id: 'settings', name: 'Settings', description: 'Info & actions',
  items: [],
  sel: store.get('settings.sel', 1),
  toast: '', toastUntil: 0,
  build() {
    const wifi = store.get('settings.wifi', 'home-5g');
    this.items = [
      { header: true, label: 'DEVICE' },
      { label: 'firmware sha',   value: 'web-demo' },
      { label: 'build date',     value: '2026-05-18' },
      { label: 'uptime',         dynamic: () => fmtUptime(performance.now() - SESSION_START) },
      { label: 'free heap',      value: '142 KB' },
      { header: true, label: 'BUDDY' },
      { label: 'approvals',      dynamic: () => String(buddy.approvals) },
      { label: 'denials',        dynamic: () => String(buddy.denials) },
      { label: 'tokens today',   dynamic: () => buddy.tokens.toLocaleString() },
      { header: true, label: 'NETWORK' },
      { label: 'WiFi',           dynamic: () => wifi + ' · -54 dBm' },
      { label: 'IP',             value: '10.0.0.18' },
      { label: 'mDNS',           value: 'clawdputer.local' },
      { header: true, label: 'CHAT' },
      { label: 'saved lines',    dynamic: () => String(chat.lines.length) },
      { label: 'ssh hosts',      dynamic: () => String(ssh.userHosts.length) + ' user' },
      { header: true, label: 'ACTIONS' },
      { label: 'switch WiFi',    action: () => {
        const opts = ['home-5g', 'home-2g', 'workshop', 'mobile-hotspot'];
        const cur = store.get('settings.wifi', 'home-5g');
        const next = opts[(opts.indexOf(cur) + 1) % opts.length];
        store.set('settings.wifi', next);
        this.flashToast('WiFi → ' + next);
        this.build();
      }},
      { label: 'show SSH pubkey', action: () => this.flashToast('ssh-ed25519 AAA…/clawd') },
      { label: 'show seal key',  action: () => this.flashToast('SEAL_KEY=hUx2…') },
      { label: 'check & install →', action: () => this.flashToast('would reboot → OTA') },
      { label: 'clear chat',     action: () => {
        chat.lines = DEFAULT_CHAT_LINES.slice();
        chat.save();
        this.flashToast('chat cleared');
        this.build();
      }},
      { label: 'reset buddy stats', action: () => {
        buddy.approvals = 0; buddy.denials = 0; buddy.tokens = 12480;
        buddy.prompt = null;
        buddy.save();
        this.flashToast('buddy counters reset');
        this.build();
      }},
      { label: 'reset demo memory', action: () => {
        store.clearAll();
        this.flashToast('memory wiped — reloading');
        setTimeout(() => location.reload(), 800);
      }},
    ];
  },
  saveSel() { store.set('settings.sel', this.sel); },
  flashToast(s) { this.toast = s; this.toastUntil = performance.now() + 2200; markDirty(); },
  onEnter() {
    this.build();
    if (this.sel >= this.items.length || (this.items[this.sel] && this.items[this.sel].header)) {
      this.sel = this.selectableNext(0, +1);
      this.saveSel();
    }
    markDirty();
  },
  onExit() {},
  onTick() {
    if (this.toastUntil && performance.now() > this.toastUntil) { this.toast = ''; this.toastUntil = 0; markDirty(); }
    // dynamic values (uptime, counters) tick the canvas
    markDirty();
  },
  selectableNext(from, dir) {
    const n = this.items.length;
    let i = from;
    for (let k = 0; k < n; k++) {
      i = (i + dir + n) % n;
      if (!this.items[i].header) return i;
    }
    return from;
  },
  onKey(ch) {
    if (ch === KEY.UP) { this.sel = this.selectableNext(this.sel, -1); this.saveSel(); markDirty(); }
    else if (ch === KEY.DOWN) { this.sel = this.selectableNext(this.sel, +1); this.saveSel(); markDirty(); }
    else if (ch === '\n') {
      const it = this.items[this.sel];
      if (it.action) it.action();
    }
  },
  onDraw(d) {
    d.fillScreen(0x0000);
    drawStatusbar(d);

    const TOP = STATUSBAR_H + 2;
    const ROW_H = 11;
    const visible = Math.floor((SCREEN_H - TOP - 12) / ROW_H);
    let first = 0;
    if (this.sel >= visible) first = this.sel - visible + 1;

    d.setTextSize(1);
    for (let i = 0; i < visible; i++) {
      const idx = first + i;
      if (idx >= this.items.length) break;
      const it = this.items[idx];
      const y = TOP + i * ROW_H;
      if (it.header) {
        d.setTextColor(COLOR.ORANGE);
        d.setCursor(4, y + 1); d.print(it.label);
        continue;
      }
      const sel = idx === this.sel;
      if (sel) d.fillRect(0, y, SCREEN_W, ROW_H, rgb565(0x2104));
      d.setTextColor(sel ? COLOR.WHITE : COLOR.GREY);
      d.setCursor(8, y + 2); d.print(it.label);
      const val = it.dynamic ? it.dynamic() : it.value;
      if (val) {
        const w = val.length * 6;
        d.setTextColor(sel ? COLOR.WHITE : COLOR.DIM);
        d.setCursor(SCREEN_W - w - 4, y + 2); d.print(val);
      } else if (it.action) {
        d.setTextColor(sel ? COLOR.YELLOW : rgb565(0x6B4D));
        d.setCursor(SCREEN_W - 12, y + 2); d.print('›');
      }
    }
    d.fillRect(0, 124, SCREEN_W, 11, COLOR.STATUSBG);
    d.drawFastHLine(0, 124, SCREEN_W, COLOR.DIVIDER);
    d.setTextColor(COLOR.DIM); d.setCursor(4, 127);
    d.print(';. move  enter select  tab:home');

    if (this.toast) {
      const w = Math.min(SCREEN_W - 20, this.toast.length * 6 + 16);
      const x = (SCREEN_W - w) / 2 | 0;
      const y = 100;
      d.fillRoundRect(x, y, w, 14, 3, rgb565(0x3186));
      d.setTextColor(COLOR.WHITE); d.setCursor(x + 8, y + 4); d.print(this.toast);
    }
  },
};
registerApp(settings);

// ── HIDDEN: usage (launched from buddy via 'u') ─────────────────────────────

const usage = {
  id: 'usage', name: 'Usage', description: 'Claude activity', hidden: true,
  onEnter() { markDirty(); },
  onExit() {},
  onTick() {},
  onKey(ch) { if (ch === '\b' || ch === KEY.ESC) requestApp('buddy'); },
  onDraw(d) {
    d.fillScreen(0x0000);
    drawStatusbar(d);
    let y = STATUSBAR_H + 4;
    d.setTextSize(1); d.setTextColor(COLOR.ORANGE);
    d.setCursor(4, y); d.print('USAGE — last 24h'); y += 14;
    d.setTextColor(COLOR.DIM); d.setCursor(4, y); d.print('input tokens');
    d.setTextColor(COLOR.WHITE); d.setCursor(110, y); d.print('482,103'); y += 11;
    d.setTextColor(COLOR.DIM); d.setCursor(4, y); d.print('output tokens');
    d.setTextColor(COLOR.WHITE); d.setCursor(110, y); d.print('38,742'); y += 11;
    d.setTextColor(COLOR.DIM); d.setCursor(4, y); d.print('cache reads');
    d.setTextColor(COLOR.WHITE); d.setCursor(110, y); d.print('2.1M'); y += 11;
    d.setTextColor(COLOR.DIM); d.setCursor(4, y); d.print('cost (est.)');
    d.setTextColor(COLOR.GREEN); d.setCursor(110, y); d.print('$3.24'); y += 14;

    // tiny sparkline
    const baseY = y + 28;
    const bars = [3, 6, 4, 8, 12, 9, 14, 18, 11, 7, 9, 16, 22, 19, 12, 6];
    for (let i = 0; i < bars.length; i++) {
      const h = bars[i];
      const x = 4 + i * 14;
      d.fillRect(x, baseY - h, 10, h, COLOR.ORANGE);
    }
    d.fillRect(0, 124, SCREEN_W, 11, COLOR.STATUSBG);
    d.drawFastHLine(0, 124, SCREEN_W, COLOR.DIVIDER);
    d.setTextColor(COLOR.DIM); d.setCursor(4, 127);
    d.print('backspace:back  tab:home');
  },
};
registerApp(usage);

// ── on-screen keyboard rendering ────────────────────────────────────────────

// Real Cardputer-ADV layout: 4 rows × 14 columns. Approximated for the
// web demo with the labels & glyphs you actually need.
const KB_ROWS = [
  // row 1: ` 1 2 3 4 5 6 7 8 9 0 - = del
  [
    { code: '`' }, { code: '1' }, { code: '2' }, { code: '3' }, { code: '4' },
    { code: '5' }, { code: '6' }, { code: '7' }, { code: '8' }, { code: '9' },
    { code: '0' }, { code: '-' }, { code: '=' }, { code: '\b', label: 'del', mod: true },
  ],
  // row 2: tab q w e r t y u i o p [ ] \
  [
    { code: '\t', label: 'tab', mod: true }, { code: 'q' }, { code: 'w' }, { code: 'e' },
    { code: 'r' }, { code: 't' }, { code: 'y' }, { code: 'u' }, { code: 'i' }, { code: 'o' },
    { code: 'p' }, { code: '[' }, { code: ']' }, { code: '\\' },
  ],
  // row 3: fn a s d f g h j k l ; ' enter (enter takes 2 col)
  [
    { code: 'fn', label: 'fn', mod: true, noop: true }, { code: 'a' }, { code: 's' },
    { code: 'd' }, { code: 'f' }, { code: 'g' }, { code: 'h' }, { code: 'j' },
    { code: 'k' }, { code: 'l' },
    { code: ';', sub: '↑' }, { code: "'" },
    { code: '\n', label: 'enter', mod: true, accent: true, wide: true },
  ],
  // row 4: shift z x c v b n m , . / ↑ space-row mash
  [
    { code: 'shift', label: 'shift', mod: true, noop: true }, { code: 'z' }, { code: 'x' },
    { code: 'c' }, { code: 'v' }, { code: 'b' }, { code: 'n' }, { code: 'm' },
    { code: ',', sub: '←' }, { code: '.', sub: '↓' }, { code: '/', sub: '→' },
    { code: ' ', label: 'space', wide: true, mod: true },
    { code: 'opt', label: 'opt', mod: true, noop: true },
  ],
];

function buildKeyboard() {
  const root = document.getElementById('keyboard');
  for (const row of KB_ROWS) {
    const rowEl = document.createElement('div');
    rowEl.className = 'kbd-row';
    // Each row must span 14 grid columns; widen wide keys.
    let cols = 0;
    for (const k of row) {
      const span = k.wide ? 2 : 1;
      cols += span;
    }
    // If short of 14, pad first key
    const pad = 14 - cols;
    let first = true;
    for (const k of row) {
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'key' + (k.mod ? ' modifier' : '') + (k.accent ? ' accent' : '') + (k.wide ? ' wide' : '');
      if (first && pad > 0) { btn.style.gridColumn = `span ${1 + pad}`; first = false; }
      btn.innerHTML = (k.label || k.code).replace(/</g, '&lt;') +
        (k.sub ? `<span class="sub">${k.sub}</span>` : '');
      btn.addEventListener('click', (e) => {
        e.preventDefault();
        if (k.noop) return;
        dispatchKey(k.code);
      });
      btn.addEventListener('mousedown', (e) => e.preventDefault());
      rowEl.appendChild(btn);
    }
    root.appendChild(rowEl);
  }
}

// ── physical keyboard wiring ────────────────────────────────────────────────

window.addEventListener('keydown', (e) => {
  if (e.metaKey || e.ctrlKey || e.altKey) return;
  const tag = (e.target && e.target.tagName) || '';
  if (tag === 'INPUT' || tag === 'TEXTAREA') return;

  let ch = null;
  if (e.key === 'Enter') ch = '\n';
  else if (e.key === 'Backspace') ch = '\b';
  else if (e.key === 'Tab') ch = '\t';
  else if (e.key === 'Escape') ch = KEY.ESC;
  else if (e.key === 'ArrowUp') ch = KEY.UP;
  else if (e.key === 'ArrowDown') ch = KEY.DOWN;
  else if (e.key === 'ArrowLeft') ch = KEY.LEFT;
  else if (e.key === 'ArrowRight') ch = KEY.RIGHT;
  else if (e.key.length === 1) ch = e.key;
  if (ch === null) return;
  e.preventDefault();
  dispatchKey(ch);
});

// ── G0 side button ──────────────────────────────────────────────────────────

document.getElementById('g0btn').addEventListener('click', () => {
  requestApp('home');
});

// ── reset-memory button (sidebar) ───────────────────────────────────────────

const resetBtn = document.getElementById('reset-memory');
if (resetBtn) {
  resetBtn.addEventListener('click', () => {
    store.clearAll();
    location.reload();
  });
}

// ── firmware-driven catalog (apps.json) ────────────────────────────────────

// The hosted demo loads apps.json — generated at build time from the real
// firmware sources in firmware/src/apps/ — and adapts to whatever apps are
// currently shipped. Apps with a JS reimplementation pick up name /
// description / hidden / keysAsArrows from the manifest; firmware apps
// without a JS reimplementation get a generic stub tile so they still show
// up in the launcher. Apps that no longer exist in firmware are dropped.

function stubApp(meta) {
  return {
    id: meta.id,
    name: meta.name || meta.id,
    description: meta.description || '',
    hidden: !!meta.hidden,
    keysAsArrows: meta.keysAsArrows !== false,
    isStub: true,
    source: meta.source || '',
    services: meta.services || [],
    onEnter() { markDirty(); },
    onExit() {},
    onTick() { /* idle */ },
    onKey() { /* no-op until reimplemented */ },
    onDraw(d) {
      d.fillScreen(0x0000);
      drawStatusbar(d);
      let y = STATUSBAR_H + 6;
      d.setTextSize(2); d.setTextColor(COLOR.ORANGE);
      d.setCursor(8, y); d.print(this.name);
      y += 20;
      d.setTextSize(1); d.setTextColor(COLOR.WHITE);
      d.setCursor(8, y); d.print(this.description);
      y += 14;
      d.setTextColor(COLOR.DIM);
      const wrap = (s, n) => {
        const w = []; let line = ''; for (const tok of s.split(' ')) {
          if ((line + ' ' + tok).trim().length > n) { w.push(line); line = tok; }
          else line = (line ? line + ' ' : '') + tok;
        } if (line) w.push(line); return w;
      };
      const blurb = 'runs on the real Cardputer — not yet reimplemented in the browser demo.';
      for (const l of wrap(blurb, 36)) { d.setCursor(8, y); d.print(l); y += 10; }
      y += 4;
      if (this.services.length) {
        d.setTextColor(COLOR.DIM); d.setCursor(8, y);
        d.print('needs ' + this.services.map(s => s.replace('SVC_', '').toLowerCase()).join(' + '));
        y += 10;
      }
      if (this.source) {
        d.setTextColor(rgb565(0x6B4D)); d.setCursor(8, y); d.print(this.source);
      }
      d.fillRect(0, 124, SCREEN_W, 11, COLOR.STATUSBG);
      d.drawFastHLine(0, 124, SCREEN_W, COLOR.DIVIDER);
      d.setTextColor(COLOR.DIM); d.setCursor(4, 127);
      d.print('tab:home');
    },
  };
}

function applyManifest(manifest) {
  if (!manifest || !Array.isArray(manifest.apps)) return;
  const byId = new Map(manifest.apps.map(a => [a.id, a]));

  // Drop JS apps that no longer exist in the firmware manifest.
  // (home is always kept — it's the launcher itself.)
  for (let i = apps.length - 1; i >= 0; i--) {
    const a = apps[i];
    if (a.id === 'home') continue;
    if (!byId.has(a.id)) apps.splice(i, 1);
  }

  // Overlay metadata onto existing JS apps; register stubs for the rest.
  for (const m of manifest.apps) {
    const existing = apps.find(a => a.id === m.id);
    if (existing) {
      if (m.name) existing.name = m.name;
      if (m.description) existing.description = m.description;
      if (m.hidden !== undefined) existing.hidden = !!m.hidden;
      if (m.keysAsArrows !== undefined) existing.keysAsArrows = !!m.keysAsArrows;
      if (m.source) existing.source = m.source;
      if (m.services) existing.services = m.services;
    } else {
      registerApp(stubApp(m));
    }
  }

  // Rebuild the launcher in case tiles changed.
  if (typeof home.rebuild === 'function') home.rebuild();
  markDirty();
}

async function loadManifest() {
  try {
    const r = await fetch('./apps.json', { cache: 'no-cache' });
    if (!r.ok) return null;
    return await r.json();
  } catch { return null; }
}

// ── main loop ───────────────────────────────────────────────────────────────

const display = new Display();
const screen = document.getElementById('screen');
const sctx = screen.getContext('2d');
sctx.imageSmoothingEnabled = false;

function frame() {
  if (currentApp && currentApp.onTick) currentApp.onTick();
  if (dirty && currentApp && currentApp.onDraw) {
    currentApp.onDraw(display);
    sctx.imageSmoothingEnabled = false;
    sctx.clearRect(0, 0, SCREEN_W, SCREEN_H);
    sctx.drawImage(display.buf, 0, 0);
    dirty = false;
  }
  requestAnimationFrame(frame);
}

buildKeyboard();
requestApp('home');
requestAnimationFrame(frame);

// Auto-adapt to the live firmware catalog. Runs after the built-in apps
// are registered so overrides + stubs land cleanly.
loadManifest().then(applyManifest);
