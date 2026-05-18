// clawdputer web demo
// Browser reimplementation of the firmware UI on a 240×135 canvas. Apps
// mirror the layouts and key bindings of firmware/src/apps/*.

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
    retro:    0x2A60,
    settings: 0x4208,
  })[a.id] ?? 0x18E3;
}
function tileAccent(a) {
  return ({
    buddy:    0x07FF,
    chat:     0x5D9F,
    ssh:      0x9CFC,
    retro:    0xAFE5,
    settings: 0xC618,
  })[a.id] ?? 0xFFFF;
}

const home = {
  id: 'home', name: 'Home', description: 'Launcher',
  sel: 0,
  tiles: [],
  rebuild() {
    this.tiles = apps.filter(a => a.id !== 'home' && !a.hidden);
    if (this.sel >= this.tiles.length) this.sel = 0;
  },
  onEnter() { this.rebuild(); markDirty(); },
  onExit() {},
  onTick() { /* nothing — status updates dirty via markDirty */ },
  onKey(ch) {
    const n = this.tiles.length;
    if (!n) return;
    if (ch === KEY.LEFT || ch === KEY.UP) { this.sel = (this.sel - 1 + n) % n; markDirty(); }
    else if (ch === KEY.RIGHT || ch === KEY.DOWN) { this.sel = (this.sel + 1) % n; markDirty(); }
    else if (ch === '\n') { requestApp(this.tiles[this.sel].id); }
    else if (ch >= '1' && ch <= '9') {
      const idx = ch.charCodeAt(0) - 49;
      if (idx < n) { this.sel = idx; requestApp(this.tiles[idx].id); }
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

const buddy = {
  id: 'buddy', name: 'Buddy', description: 'Claude Desktop companion',
  state: 'idle', pet: 'sleep',
  owner: 'christopher', tokens: 12480, total: 3, running: 1, waiting: 0,
  prompt: null, approvals: 0, denials: 0,
  enteredAt: 0, scriptAt: 0,
  onEnter() {
    this.enteredAt = performance.now();
    this.scriptAt = 0;
    // simulate connect handshake + activity
    link.nus = false;
    this.prompt = null;
    markDirty();
  },
  onExit() {},
  onTick() {
    const t = performance.now() - this.enteredAt;
    if (!link.nus && t > 1800) { link.nus = true; markDirty(); }
    if (link.nus && !this.prompt && t > 5500 && this.scriptAt < 1) {
      this.scriptAt = 1;
      this.prompt = {
        id: 'p1',
        tool: 'mcp__github__list_pull_requests',
        hint: 'repo: CHaerem/clawdputer',
      };
      this.pet = 'attention';
      markDirty();
    }
    // pet bobs animate continuously
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
      setTimeout(() => { this.pet = 'busy'; markDirty(); }, 2200);
      markDirty();
    } else if (ch === '\b' || ch === 'n' || ch === 'N' || ch === KEY.ESC) {
      this.denials++; this.prompt = null; this.pet = 'idle';
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

const CHAT_REPLIES = [
  { status: 'connecting to clawd-bridge…' },
  { status: '⚙ Read' },
  { chunk: 'The Cardputer firmware registers apps via a static `REGISTER_APP()` macro ' },
  { chunk: 'in each app translation unit. See firmware/src/core/app.h:53 — the registrar ' },
  { chunk: "runs before main(), so the home launcher's app list is fully populated by " },
  { chunk: 'the time setup() returns.\n\n' },
  { status: '⚙ Grep' },
  { chunk: 'Adding a new app is one directory under firmware/src/apps/<name>/ with a ' },
  { chunk: '.cpp that defines the App struct and ends with REGISTER_APP(). The launcher ' },
  { chunk: 'picks it up automatically.' },
  { end: 187 },
];

const chat = {
  id: 'chat', name: 'Chat', description: 'Claude CLI remote',
  keysAsArrows: false,
  lines: [], input: '', busy: false, scriptIdx: 0, scriptTimer: null,
  onEnter() {
    link.wifi = true;
    link.bridgeTcp = true;
    this.lines = [
      { text: 'bridge connected (tcp via mDNS)', kind: 'status' },
      { text: 'try: "how does the app registry work?"', kind: 'status' },
    ];
    this.input = '';
    this.busy = false;
    markDirty();
  },
  onExit() {
    if (this.scriptTimer) { clearTimeout(this.scriptTimer); this.scriptTimer = null; }
  },
  pushLine(text, kind) {
    this.lines.push({ text, kind });
    if (this.lines.length > 200) this.lines.shift();
    markDirty();
  },
  appendAssistant(chunk) {
    const last = this.lines[this.lines.length - 1];
    if (last && last.kind === 'assistant') { last.text += chunk; }
    else this.lines.push({ text: chunk, kind: 'assistant' });
    markDirty();
  },
  scheduleNext() {
    if (this.scriptIdx >= CHAT_REPLIES.length) {
      this.busy = false; markDirty(); return;
    }
    const step = CHAT_REPLIES[this.scriptIdx++];
    const delay = step.chunk ? 90 + Math.random() * 120 : 350;
    this.scriptTimer = setTimeout(() => {
      if (currentApp !== chat) return;
      if (step.chunk) this.appendAssistant(step.chunk);
      else if (step.status) this.pushLine(step.status, 'status');
      else if (step.end !== undefined) this.pushLine(`[${step.end} tokens]`, 'status');
      this.scheduleNext();
    }, delay);
  },
  send() {
    if (!this.input.trim()) return;
    this.pushLine(this.input, 'user');
    this.input = '';
    this.busy = true;
    this.scriptIdx = 0;
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

const ssh = {
  id: 'ssh', name: 'SSH', description: 'Key-auth SSH client',
  hosts: [
    { name: 'mac-mini',  host: '10.0.0.12',  user: 'chris',  source: 'sealed' },
    { name: 'studio',    host: '10.0.0.21',  user: 'chris',  source: 'sealed' },
    { name: 'pi-pixel',  host: '10.0.0.40',  user: 'pi',     source: 'sealed' },
    { name: '+ add new host…', host: '',     user: '',       source: 'action' },
  ],
  sel: 0,
  connecting: null,
  onEnter() { this.sel = 0; this.connecting = null; markDirty(); },
  onExit() {},
  onTick() {},
  onKey(ch) {
    if (this.connecting) return;
    if (ch === KEY.UP) { this.sel = (this.sel - 1 + this.hosts.length) % this.hosts.length; markDirty(); }
    else if (ch === KEY.DOWN) { this.sel = (this.sel + 1) % this.hosts.length; markDirty(); }
    else if (ch === '\n') {
      const h = this.hosts[this.sel];
      if (h.source === 'action') return;
      this.connecting = h;
      markDirty();
      setTimeout(() => {
        if (currentApp === ssh) { this.connecting = null; markDirty(); }
      }, 1800);
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
      return;
    }

    let y = STATUSBAR_H + 4;
    d.setTextSize(1); d.setTextColor(COLOR.DIM);
    d.setCursor(4, y); d.print('hosts'); y += 12;
    for (let i = 0; i < this.hosts.length; i++) {
      const h = this.hosts[i];
      const sel = i === this.sel;
      if (sel) d.fillRect(0, y - 1, SCREEN_W, 11, rgb565(0x2104));
      d.setTextColor(sel ? COLOR.WHITE : COLOR.GREY);
      d.setCursor(8, y + 1); d.print(h.name);
      if (h.source !== 'action') {
        d.setTextColor(sel ? COLOR.DIM : rgb565(0x5ACB));
        d.setCursor(110, y + 1); d.print(`${h.user}@${h.host}`);
      }
      y += 11;
    }
    d.fillRect(0, 124, SCREEN_W, 11, COLOR.STATUSBG);
    d.drawFastHLine(0, 124, SCREEN_W, COLOR.DIVIDER);
    d.setTextColor(COLOR.DIM); d.setCursor(4, 127);
    d.print(';. select  enter connect  tab:home');
  },
};
registerApp(ssh);

// ── RETRO ───────────────────────────────────────────────────────────────────

const retro = {
  id: 'retro', name: 'Retro', description: 'Game console emulators',
  // Loaded asynchronously from roms-manifest.txt; falls back to a small
  // placeholder list while pending or on failure.
  roms: [
    { label: 'blargg_cpu_instrs.gb', ext: 'GB' },
  ],
  manifestLoaded: false,
  sel: 0,
  launching: null,

  async loadManifest() {
    try {
      const res = await fetch('roms-manifest.txt', { cache: 'no-cache' });
      if (!res.ok) throw new Error(`http ${res.status}`);
      const text = await res.text();
      const parsed = [];
      for (const raw of text.split(/\r?\n/)) {
        const line = raw.trim();
        if (!line || line.startsWith('#')) continue;
        let name;
        if (line.includes('|')) {
          name = line.split('|')[0].trim();
        } else {
          name = line.split('/').pop().split('?')[0];
        }
        if (!name) continue;
        const m = name.match(/\.(gb|gbc|nes|sms|gg|ngp|ngc)$/i);
        if (!m) continue;
        parsed.push({ label: name, ext: m[1].toUpperCase() });
      }
      if (parsed.length) this.roms = parsed;
      this.manifestLoaded = true;
      markDirty();
    } catch (err) {
      console.warn('manifest load failed:', err);
      this.manifestLoaded = true;   // give up; show the fallback
      markDirty();
    }
  },

  onEnter() {
    this.sel = 0;
    this.launching = null;
    if (!this.manifestLoaded) this.loadManifest();
    markDirty();
  },
  onExit() {},
  onTick() {},
  onKey(ch) {
    if (this.launching) return;
    // +1 because index 0 is the "[+ Download games]" sentinel row.
    const n = this.roms.length + 1;
    if (ch === KEY.UP) { this.sel = (this.sel - 1 + n) % n; markDirty(); }
    else if (ch === KEY.DOWN) { this.sel = (this.sel + 1) % n; markDirty(); }
    else if (ch === '\n') {
      if (this.sel === 0) {
        this.launching = '__downloader__';
      } else {
        this.launching = this.roms[this.sel - 1].label;
      }
      markDirty();
    }
  },
  onDraw(d) {
    d.fillScreen(0x0000);
    drawStatusbar(d);

    if (this.launching === '__downloader__') {
      d.fillRect(0, STATUSBAR_H, SCREEN_W, SCREEN_H - STATUSBAR_H, rgb565(0x1082));
      d.setTextSize(1); d.setTextColor(COLOR.WHITE);
      d.setCursor(8, 30); d.print('fetching manifest…');
      d.setCursor(8, 50); d.setTextColor(COLOR.DIM);
      d.print('(simulated — on the real device this');
      d.setCursor(8, 62); d.print(' pulls roms-manifest.txt over HTTPS)');
      return;
    }
    if (this.launching) {
      // emulator-running tribute
      d.fillRect(0, STATUSBAR_H, SCREEN_W, SCREEN_H - STATUSBAR_H, rgb565(0x8E26));
      d.setTextSize(2); d.setTextColor(rgb565(0x0320));
      d.setCursor(72, 40); d.print('RETRO');
      d.setTextSize(1); d.setTextColor(rgb565(0x2104));
      d.setCursor(40, 70); d.print(this.launching);
      d.setCursor(40, 100); d.setTextColor(rgb565(0x0320));
      d.print('(emulator not in web demo)');
      return;
    }

    let y = STATUSBAR_H + 4;
    d.setTextSize(1);
    d.setTextColor(COLOR.DIM);
    d.setCursor(4, y);
    d.print(this.manifestLoaded
      ? `manifest: ${this.roms.length} ROMs`
      : 'loading manifest…');
    y += 12;

    const rows = [{ label: '[+ Download games]', ext: '' },
                  ...this.roms.map(r => ({ label: `[${r.ext}] ${r.label}`, ext: r.ext }))];
    const maxRows = 7;
    const scrollTop = Math.max(0, Math.min(this.sel - 3, rows.length - maxRows));
    for (let i = scrollTop; i < Math.min(rows.length, scrollTop + maxRows); i++) {
      const sel = i === this.sel;
      if (sel) d.fillRect(0, y - 1, SCREEN_W, 11, rgb565(0x2104));
      d.setTextColor(sel ? COLOR.WHITE : COLOR.GREY);
      d.setCursor(8, y + 1); d.print(rows[i].label);
      y += 11;
    }

    d.fillRect(0, 124, SCREEN_W, 11, COLOR.STATUSBG);
    d.drawFastHLine(0, 124, SCREEN_W, COLOR.DIVIDER);
    d.setTextColor(COLOR.DIM); d.setCursor(4, 127);
    d.print(';. select  enter open  tab:home');
  },
};
registerApp(retro);

// ── SETTINGS ────────────────────────────────────────────────────────────────

const settings = {
  id: 'settings', name: 'Settings', description: 'Info & actions',
  items: [],
  sel: 1,
  toast: '', toastUntil: 0,
  build() {
    this.items = [
      { header: true, label: 'DEVICE' },
      { label: 'firmware sha',   value: 'd4a7c9e' },
      { label: 'build date',     value: '2026-05-16' },
      { label: 'uptime',         value: '00:04:21' },
      { label: 'free heap',      value: '142 KB' },
      { header: true, label: 'NETWORK' },
      { label: 'WiFi',           value: 'home-5g · -54 dBm' },
      { label: 'IP',             value: '10.0.0.18' },
      { label: 'mDNS',           value: 'clawdputer.local' },
      { header: true, label: 'ACTIONS' },
      { label: 'configure WiFi', action: () => this.flashToast('WiFi app launches here') },
      { label: 'show SSH pubkey', action: () => this.flashToast('ssh-ed25519 AAA…/clawd') },
      { label: 'show seal key',  action: () => this.flashToast('SEAL_KEY=hUx2…') },
      { label: 'check & install →', action: () => this.flashToast('reboot → recovery OTA') },
      { label: 'clear BLE bonds', action: () => this.flashToast('BLE bonds cleared') },
      { label: 'reboot',         action: () => this.flashToast('rebooting…') },
    ];
  },
  flashToast(s) { this.toast = s; this.toastUntil = performance.now() + 2200; markDirty(); },
  onEnter() { this.build(); markDirty(); },
  onExit() {},
  onTick() { if (this.toastUntil && performance.now() > this.toastUntil) { this.toast = ''; this.toastUntil = 0; markDirty(); } },
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
    if (ch === KEY.UP) { this.sel = this.selectableNext(this.sel, -1); markDirty(); }
    else if (ch === KEY.DOWN) { this.sel = this.selectableNext(this.sel, +1); markDirty(); }
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
      if (it.value) {
        const w = it.value.length * 6;
        d.setTextColor(sel ? COLOR.WHITE : COLOR.DIM);
        d.setCursor(SCREEN_W - w - 4, y + 2); d.print(it.value);
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
