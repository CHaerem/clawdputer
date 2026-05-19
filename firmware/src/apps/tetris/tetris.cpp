// Tetris. Native — no ROMs.
//
// Start screen lets the player open the remap menu (M) or toggle layout
// (R) between landscape and portrait before playing. Highscore is
// persisted in NVS namespace "tetris" alongside layout + key bindings.
//
// In-game:
//   landscape — 10×20 field at 6 px/cell (60×120 px) with a sidebar
//   portrait  — 10×20 field at 10 px/cell (100×200 px) with sidebar right
//
// A ghost piece (cell outlines in the piece colour) shows the hard-drop
// landing position.
//
// Standard scoring (100/300/500/800 per 1/2/3/4 lines × (level+1)). Level
// rises every 10 lines, gravity speeds up with it (800 ms → floor 50 ms).
//
// Controls during play (default — all rebindable via the M menu):
//   A / D     = move left / right
//   S         = soft drop
//   space     = hard drop (slam to bottom + lock)
//   K / L     = rotate counter-clockwise / clockwise
//   P         = pause / resume
//   enter (on game-over) = restart

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>

#include <algorithm>
#include <cstring>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

enum class State : uint8_t { Start, Playing, GameOver, Paused, Remap };

constexpr int FIELD_W = 10;
constexpr int FIELD_H = 20;

// Colors (RGB565)
constexpr uint16_t COL_BG      = 0x0000;
constexpr uint16_t COL_BORDER  = 0x4208;
constexpr uint16_t COL_TEXT    = 0xFFE0;
constexpr uint16_t COL_DIM     = 0x8C71;
constexpr uint16_t COL_ACCENT  = 0x07FF;
constexpr uint16_t COL_PIECE[7] = {
    0x07FF,   // I — cyan
    0xFFE0,   // O — yellow
    0xA81F,   // T — purple
    0x07E0,   // S — green
    0xF800,   // Z — red
    0xFD20,   // L — orange
    0x001F,   // J — blue
};

struct Cell { int8_t dx, dy; };

// 7 pieces × 4 rotations × 4 cells. (dx, dy) is the cell offset within the
// piece's 4×4 bounding box (origin top-left).
constexpr Cell kPieces[7][4][4] = {
    // I
    { {{0,1},{1,1},{2,1},{3,1}},
      {{2,0},{2,1},{2,2},{2,3}},
      {{0,2},{1,2},{2,2},{3,2}},
      {{1,0},{1,1},{1,2},{1,3}} },
    // O
    { {{1,0},{2,0},{1,1},{2,1}},
      {{1,0},{2,0},{1,1},{2,1}},
      {{1,0},{2,0},{1,1},{2,1}},
      {{1,0},{2,0},{1,1},{2,1}} },
    // T
    { {{0,1},{1,1},{2,1},{1,0}},
      {{1,0},{1,1},{1,2},{2,1}},
      {{0,1},{1,1},{2,1},{1,2}},
      {{1,0},{1,1},{1,2},{0,1}} },
    // S
    { {{1,0},{2,0},{0,1},{1,1}},
      {{1,0},{1,1},{2,1},{2,2}},
      {{1,0},{2,0},{0,1},{1,1}},
      {{1,0},{1,1},{2,1},{2,2}} },
    // Z
    { {{0,0},{1,0},{1,1},{2,1}},
      {{2,0},{1,1},{2,1},{1,2}},
      {{0,0},{1,0},{1,1},{2,1}},
      {{2,0},{1,1},{2,1},{1,2}} },
    // L
    { {{0,1},{1,1},{2,1},{2,0}},
      {{1,0},{1,1},{1,2},{2,2}},
      {{0,1},{1,1},{2,1},{0,2}},
      {{0,0},{1,0},{1,1},{1,2}} },
    // J
    { {{0,0},{0,1},{1,1},{2,1}},
      {{1,0},{2,0},{1,1},{1,2}},
      {{0,1},{1,1},{2,1},{2,2}},
      {{1,0},{1,1},{0,2},{1,2}} },
};

struct Bindings {
    char left   = 'a';
    char right  = 'd';
    char soft   = 's';
    char hard   = ' ';
    char rotCcw = 'k';
    char rotCw  = 'l';
    char pause  = 'p';
};

// Field cells: 0 = empty, 1..7 = piece type + 1 (so 0 stays as "empty").
uint8_t  g_field[FIELD_H][FIELD_W];
int      g_pieceType, g_nextType, g_rot, g_px, g_py;
int      g_score, g_lines, g_level;
int      g_best       = 0;
uint32_t g_dropIntervalMs;
uint32_t g_lastDropMs;
State    g_state      = State::Start;
bool     g_portrait   = false;
Bindings g_bind;
int      g_remapSel   = 0;
bool     g_dirty      = true;

int cellSize() { return g_portrait ? 9 : 6; }
int fieldX()   { return g_portrait ? 4  : 20; }
int fieldY()   { return 14; }
int sideX()    { return fieldX() + FIELD_W * cellSize() + 6; }
int screenW()  { return g_portrait ? 135 : 240; }
int screenH()  { return g_portrait ? 240 : 135; }

void loadPrefs() {
    Preferences p;
    p.begin("tetris", true);
    g_best         = p.getInt  ("best", 0);
    g_portrait     = p.getBool ("portrait", false);
    g_bind.left    = (char)p.getUChar("kl", 'a');
    g_bind.right   = (char)p.getUChar("kr", 'd');
    g_bind.soft    = (char)p.getUChar("ks", 's');
    g_bind.hard    = (char)p.getUChar("kh", ' ');
    g_bind.rotCcw  = (char)p.getUChar("kc", 'k');
    g_bind.rotCw   = (char)p.getUChar("kw", 'l');
    g_bind.pause   = (char)p.getUChar("kp", 'p');
    p.end();
}

void savePrefs() {
    Preferences p;
    p.begin("tetris", false);
    p.putInt  ("best",     g_best);
    p.putBool ("portrait", g_portrait);
    p.putUChar("kl",       (uint8_t)g_bind.left);
    p.putUChar("kr",       (uint8_t)g_bind.right);
    p.putUChar("ks",       (uint8_t)g_bind.soft);
    p.putUChar("kh",       (uint8_t)g_bind.hard);
    p.putUChar("kc",       (uint8_t)g_bind.rotCcw);
    p.putUChar("kw",       (uint8_t)g_bind.rotCw);
    p.putUChar("kp",       (uint8_t)g_bind.pause);
    p.end();
}

void applyOrientation() {
    if (g_portrait) ui::reconfigureCanvas(135, 240, 0);
    else            ui::reconfigureCanvas(240, 135, 1);
    g_dirty = true;
}

bool collides(int type, int rot, int px, int py) {
    for (int i = 0; i < 4; i++) {
        int cx = px + kPieces[type][rot][i].dx;
        int cy = py + kPieces[type][rot][i].dy;
        if (cx < 0 || cx >= FIELD_W || cy >= FIELD_H) return true;
        if (cy < 0) continue;
        if (g_field[cy][cx]) return true;
    }
    return false;
}

int ghostOffset() {
    int o = 0;
    while (!collides(g_pieceType, g_rot, g_px, g_py + o + 1)) o++;
    return o;
}

void spawnNext() {
    g_pieceType = g_nextType;
    g_nextType  = random(0, 7);
    g_rot       = 0;
    g_px        = FIELD_W / 2 - 2;
    g_py        = -1;
    if (collides(g_pieceType, g_rot, g_px, g_py)) {
        g_state = State::GameOver;
        if (g_score > g_best) { g_best = g_score; savePrefs(); }
    }
}

void lockPiece() {
    for (int i = 0; i < 4; i++) {
        int cx = g_px + kPieces[g_pieceType][g_rot][i].dx;
        int cy = g_py + kPieces[g_pieceType][g_rot][i].dy;
        if (cy >= 0 && cy < FIELD_H && cx >= 0 && cx < FIELD_W)
            g_field[cy][cx] = (uint8_t)(g_pieceType + 1);
    }

    int cleared = 0;
    for (int y = FIELD_H - 1; y >= 0; ) {
        bool full = true;
        for (int x = 0; x < FIELD_W; x++) if (!g_field[y][x]) { full = false; break; }
        if (full) {
            for (int yy = y; yy > 0; yy--)
                for (int x = 0; x < FIELD_W; x++)
                    g_field[yy][x] = g_field[yy - 1][x];
            for (int x = 0; x < FIELD_W; x++) g_field[0][x] = 0;
            cleared++;
        } else {
            y--;
        }
    }
    if (cleared > 0) {
        constexpr int kLineScore[5] = { 0, 100, 300, 500, 800 };
        g_score += kLineScore[cleared] * (g_level + 1);
        g_lines += cleared;
        int newLevel = g_lines / 10;
        if (newLevel != g_level) {
            g_level = newLevel;
            g_dropIntervalMs = std::max(50, 800 - g_level * 60);
        }
    }

    spawnNext();
}

void softDrop() {
    if (!collides(g_pieceType, g_rot, g_px, g_py + 1)) {
        g_py++;
        g_lastDropMs = millis();
    } else {
        lockPiece();
    }
}

void hardDrop() {
    g_py += ghostOffset();
    lockPiece();
}

void tryMove(int dx) {
    if (!collides(g_pieceType, g_rot, g_px + dx, g_py)) g_px += dx;
}

void tryRotate(int dir) {
    int r = (g_rot + dir + 4) % 4;
    if (!collides(g_pieceType, r, g_px, g_py))           { g_rot = r; return; }
    if (!collides(g_pieceType, r, g_px - 1, g_py))       { g_rot = r; g_px--; return; }
    if (!collides(g_pieceType, r, g_px + 1, g_py))       { g_rot = r; g_px++; return; }
    if (!collides(g_pieceType, r, g_px, g_py - 1))       { g_rot = r; g_py--; return; }
}

void resetRun() {
    memset(g_field, 0, sizeof(g_field));
    g_pieceType      = random(0, 7);
    g_nextType       = random(0, 7);
    g_rot            = 0;
    g_px             = FIELD_W / 2 - 2;
    g_py             = -1;
    g_score          = 0;
    g_lines          = 0;
    g_level          = 0;
    g_dropIntervalMs = 800;
    g_lastDropMs     = millis();
}

void onEnter() {
    loadPrefs();
    applyOrientation();
    g_state = State::Start;
    g_dirty = true;
}

void onExit() {
    ui::reconfigureCanvas(SCREEN_W, SCREEN_H, 1);
}

void onTick() {
    if (g_state != State::Playing) return;
    uint32_t now = millis();
    if (now - g_lastDropMs >= g_dropIntervalMs) {
        softDrop();
        g_lastDropMs = now;
        g_dirty      = true;
    }
}

void onKeyStart(char ch) {
    switch (ch) {
        case '\n':
            resetRun();
            g_state = State::Playing;
            g_dirty = true;
            break;
        case 'r': case 'R':
            g_portrait = !g_portrait;
            savePrefs();
            applyOrientation();
            break;
        case 'm': case 'M':
            g_remapSel = 0;
            g_state    = State::Remap;
            g_dirty    = true;
            break;
    }
}

void onKeyPlaying(char ch) {
    if (ch == g_bind.pause) { g_state = State::Paused; g_dirty = true; return; }
    if      (ch == g_bind.left  || ch == key::Left)  { tryMove(-1);   g_dirty = true; return; }
    else if (ch == g_bind.right || ch == key::Right) { tryMove(+1);   g_dirty = true; return; }
    else if (ch == g_bind.soft  || ch == key::Down)  { softDrop();    g_dirty = true; return; }
    else if (ch == g_bind.hard)                       { hardDrop();    g_dirty = true; return; }
    else if (ch == g_bind.rotCcw)                     { tryRotate(-1); g_dirty = true; return; }
    else if (ch == g_bind.rotCw  || ch == key::Up)    { tryRotate(+1); g_dirty = true; return; }
    // Mid-game orientation toggle. Playfield is always 10×20 cells —
    // only the pixel size changes, so the run continues without reset.
    if (ch == 'r' || ch == 'R') {
        g_portrait = !g_portrait;
        savePrefs();
        applyOrientation();
    }
}

void onKeyPaused(char ch) {
    if (ch == g_bind.pause) { g_state = State::Playing; g_lastDropMs = millis(); g_dirty = true; }
}

void onKeyGameOver(char ch) {
    if (ch == '\n') {
        resetRun();
        g_state = State::Playing;
        g_dirty = true;
    } else if (ch == 'r' || ch == 'R') {
        g_portrait = !g_portrait;
        savePrefs();
        applyOrientation();
    }
}

void onKeyRemap(char ch) {
    constexpr int N = 7;
    char* slot[N] = { &g_bind.left, &g_bind.right, &g_bind.soft, &g_bind.hard,
                      &g_bind.rotCcw, &g_bind.rotCw, &g_bind.pause };
    if (ch == key::Up)   { g_remapSel = (g_remapSel + N - 1) % N; g_dirty = true; return; }
    if (ch == key::Down) { g_remapSel = (g_remapSel + 1)     % N; g_dirty = true; return; }
    if (ch == '\n')      { savePrefs(); g_state = State::Start; g_dirty = true; return; }
    if (ch >= 0x20 && ch <= 0x7E) {
        *slot[g_remapSel] = ch;
        g_dirty = true;
    }
}

void onKey(char ch) {
    switch (g_state) {
        case State::Start:    onKeyStart(ch);    break;
        case State::Playing:  onKeyPlaying(ch);  break;
        case State::Paused:   onKeyPaused(ch);   break;
        case State::GameOver: onKeyGameOver(ch); break;
        case State::Remap:    onKeyRemap(ch);    break;
    }
}

void drawCell(int gridX, int gridY, uint16_t color) {
    int sz = cellSize();
    int px = fieldX() + gridX * sz;
    int py = fieldY() + gridY * sz;
    ui::display().fillRect(px, py, sz - 1, sz - 1, color);
}

void drawCellOutline(int gridX, int gridY, uint16_t color) {
    int sz = cellSize();
    int px = fieldX() + gridX * sz;
    int py = fieldY() + gridY * sz;
    ui::display().drawRect(px, py, sz - 1, sz - 1, color);
}

void drawCenteredText(int y, const char* s, uint16_t color, int size = 1) {
    auto& d = ui::display();
    d.setTextSize(size);
    d.setTextColor(color);
    int w = (int)strlen(s) * 6 * size;
    d.setCursor((screenW() - w) / 2, y);
    d.print(s);
}

void drawStartScreen() {
    char buf[40];
    drawCenteredText(g_portrait ? 30 : 14, "T E T R I S", COL_PIECE[0], 2);
    snprintf(buf, sizeof(buf), "best: %d", g_best);
    drawCenteredText(g_portrait ? 60 : 38, buf, COL_TEXT);

    int y  = g_portrait ? 100 : 60;
    int lh = 12;
    drawCenteredText(y, "enter  play", COL_ACCENT); y += lh + 2;
    snprintf(buf, sizeof(buf), "R  view: %s",
             g_portrait ? "portrait" : "landscape");
    drawCenteredText(y, buf, COL_DIM); y += lh;
    drawCenteredText(y, "M  remap keys", COL_DIM);  y += lh;
    drawCenteredText(y, "tab  home",     COL_DIM);
}

void drawSidebar() {
    auto& d = ui::display();
    int sx = sideX();

    int nextCell = g_portrait ? 4 : 5;
    int nextBoxX = g_portrait ? sx : (sx + 38);
    int nextBoxY = fieldY() + 12;

    d.setTextSize(1);
    d.setTextColor(COL_DIM);
    d.setCursor(sx, fieldY());
    d.print("NEXT");

    d.drawRect(nextBoxX - 1, nextBoxY - 1, 4 * nextCell + 2, 4 * nextCell + 2, COL_BORDER);
    uint16_t pc = COL_PIECE[g_nextType];
    for (int i = 0; i < 4; i++) {
        int cx = kPieces[g_nextType][0][i].dx;
        int cy = kPieces[g_nextType][0][i].dy;
        d.fillRect(nextBoxX + cx * nextCell, nextBoxY + cy * nextCell,
                   nextCell - 1, nextCell - 1, pc);
    }

    // Tight spacing: 9 label→value, 10 value→next label. Lets all four
    // entries (SCORE / LINES / LEVEL / BEST) fit in the landscape sidebar
    // without overflowing screen height.
    struct Item { const char* label; int value; };
    Item items[] = {
        { "SCORE", g_score },
        { "LINES", g_lines },
        { "LEVEL", g_level },
        { "BEST",  g_best  },
    };
    char buf[16];
    int y = nextBoxY + 4 * nextCell + 8;
    for (auto& it : items) {
        d.setTextColor(COL_DIM);  d.setCursor(sx, y); d.print(it.label); y += 9;
        d.setTextColor(COL_TEXT); d.setCursor(sx, y);
        snprintf(buf, sizeof(buf), "%d", it.value);
        d.print(buf);
        y += 10;
    }
}

void drawPlayfield() {
    auto& d = ui::display();
    int sz = cellSize();
    d.drawRect(fieldX() - 1, fieldY() - 1, FIELD_W * sz + 2, FIELD_H * sz + 2, COL_BORDER);
    for (int y = 0; y < FIELD_H; y++) {
        for (int x = 0; x < FIELD_W; x++) {
            uint8_t v = g_field[y][x];
            if (v) drawCell(x, y, COL_PIECE[v - 1]);
        }
    }
    if (g_state != State::GameOver) {
        // Ghost (shadow) first so the active piece overdraws it.
        int off = ghostOffset();
        if (off > 0) {
            for (int i = 0; i < 4; i++) {
                int cx = g_px + kPieces[g_pieceType][g_rot][i].dx;
                int cy = g_py + off + kPieces[g_pieceType][g_rot][i].dy;
                if (cy >= 0 && cy < FIELD_H && cx >= 0 && cx < FIELD_W)
                    drawCellOutline(cx, cy, COL_PIECE[g_pieceType]);
            }
        }
        uint16_t pc = COL_PIECE[g_pieceType];
        for (int i = 0; i < 4; i++) {
            int cx = g_px + kPieces[g_pieceType][g_rot][i].dx;
            int cy = g_py + kPieces[g_pieceType][g_rot][i].dy;
            if (cy >= 0 && cy < FIELD_H && cx >= 0 && cx < FIELD_W)
                drawCell(cx, cy, pc);
        }
    }
}

void drawOverlayBox(uint16_t border, const char* big, const char* sub) {
    auto& d = ui::display();
    int bw = 160, bh = 35;
    int bx = (screenW() - bw) / 2;
    int by = (screenH() - bh) / 2;
    d.fillRect(bx, by, bw, bh, COL_BG);
    d.drawRect(bx, by, bw, bh, border);
    d.setTextSize(2);
    d.setTextColor(border);
    int tw = (int)strlen(big) * 12;
    d.setCursor(bx + (bw - tw) / 2, by + 6);
    d.print(big);
    if (sub) {
        d.setTextSize(1);
        d.setTextColor(COL_TEXT);
        int sw = (int)strlen(sub) * 6;
        d.setCursor(bx + (bw - sw) / 2, by + 26);
        d.print(sub);
    }
}

void drawRemap() {
    drawCenteredText(g_portrait ? 16 : 4, "REMAP KEYS", COL_ACCENT);
    const char* labels[] = { "left ", "right", "soft ", "hard ", "rotL ", "rotR ", "pause" };
    char* bvals[] = { &g_bind.left, &g_bind.right, &g_bind.soft, &g_bind.hard,
                      &g_bind.rotCcw, &g_bind.rotCw, &g_bind.pause };
    auto& d = ui::display();
    int y = g_portrait ? 40 : 22;
    for (int i = 0; i < 7; i++) {
        char buf[24];
        char show = *bvals[i] == ' ' ? '_' : *bvals[i];
        snprintf(buf, sizeof(buf), "%c %s : %c",
                 i == g_remapSel ? '>' : ' ',
                 labels[i], show);
        d.setTextSize(1);
        d.setTextColor(i == g_remapSel ? COL_ACCENT : COL_DIM);
        d.setCursor(g_portrait ? 16 : 70, y);
        d.print(buf);
        y += 12;
    }
    drawCenteredText(y + 6,  "press a key to bind", COL_DIM);
    drawCenteredText(y + 18, "enter  back",          COL_DIM);
}

void onDraw() {
    if (!g_dirty) return;
    g_dirty = false;
    ui::beginFrame();

    if (g_state == State::Start)  { drawStartScreen(); ui::flush(); return; }
    if (g_state == State::Remap)  { drawRemap();       ui::flush(); return; }

    if (!g_portrait) ui::statusbar::draw();
    drawPlayfield();
    drawSidebar();

    if (g_state == State::Paused) {
        drawOverlayBox(COL_TEXT, "PAUSE", "press again to resume");
    } else if (g_state == State::GameOver) {
        drawOverlayBox(COL_PIECE[4], "GAME OVER", "enter restart  tab home");
    }

    ui::flush();
}

App tetris_app = {
    .id           = "tetris",
    .name         = "Tetris",
    .description  = "Stack 'em, clear 'em",
    .services     = SVC_CANVAS,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .onDraw       = onDraw,
    .onEvent      = nullptr,
    .keysAsArrows = true,
    .hidden       = false,
};

}  // namespace

REGISTER_APP(tetris_app);
