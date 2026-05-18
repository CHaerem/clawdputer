// Tetris. Native — no ROMs.
//
// 10 × 20 playfield rendered with 6 px cells (60 × 120 px), left of a
// sidebar showing NEXT piece, SCORE, LINES, LEVEL. Standard scoring
// (100/300/500/800 per 1/2/3/4 lines × (level+1)). Level rises every 10
// lines, gravity speeds up with it (800 ms → floor at 50 ms).
//
// Controls during play:
//   A / D     = move left / right
//   S         = soft drop
//   K / L     = rotate counter-clockwise / clockwise
//   space     = hard drop (slam to bottom + lock)
//   P         = pause / resume
//   enter (on game-over) = restart

#include <Arduino.h>
#include <M5Cardputer.h>

#include <algorithm>
#include <cstring>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

constexpr int FIELD_W = 10;
constexpr int FIELD_H = 20;
constexpr int CELL    = 6;
constexpr int FIELD_X = 20;
constexpr int FIELD_Y = 14;        // just below status bar

// Sidebar
constexpr int SIDE_X       = FIELD_X + FIELD_W * CELL + 8;
constexpr int NEXT_BOX_X   = SIDE_X + 38;
constexpr int NEXT_BOX_Y   = FIELD_Y + 12;
constexpr int NEXT_CELL    = 5;

// Colors (RGB565)
constexpr uint16_t COL_BG      = 0x0000;
constexpr uint16_t COL_BORDER  = 0x4208;
constexpr uint16_t COL_GRID    = 0x10A2;
constexpr uint16_t COL_TEXT    = 0xFFE0;
constexpr uint16_t COL_DIM     = 0x8C71;
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

// Field cells: 0 = empty, 1..7 = piece type + 1 (so 0 stays as "empty").
uint8_t  g_field[FIELD_H][FIELD_W];
int      g_pieceType, g_nextType, g_rot, g_px, g_py;
int      g_score, g_lines, g_level;
uint32_t g_dropIntervalMs;
uint32_t g_lastDropMs;
bool     g_gameOver, g_paused;
bool     g_dirty;
int      g_best = 0;

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

void lockPiece() {
    for (int i = 0; i < 4; i++) {
        int cx = g_px + kPieces[g_pieceType][g_rot][i].dx;
        int cy = g_py + kPieces[g_pieceType][g_rot][i].dy;
        if (cy >= 0 && cy < FIELD_H && cx >= 0 && cx < FIELD_W)
            g_field[cy][cx] = (uint8_t)(g_pieceType + 1);
    }

    // Clear full lines.
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

    // Spawn next.
    g_pieceType = g_nextType;
    g_nextType  = random(0, 7);
    g_rot       = 0;
    g_px        = FIELD_W / 2 - 2;
    g_py        = -1;
    if (collides(g_pieceType, g_rot, g_px, g_py)) {
        g_gameOver = true;
        if (g_score > g_best) g_best = g_score;
    }
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
    while (!collides(g_pieceType, g_rot, g_px, g_py + 1)) g_py++;
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

void reset() {
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
    g_gameOver       = false;
    g_paused         = false;
    g_dirty          = true;
}

void onEnter() { reset(); }
void onExit()  {}

void onTick() {
    if (g_gameOver || g_paused) return;
    uint32_t now = millis();
    if (now - g_lastDropMs >= g_dropIntervalMs) {
        softDrop();
        g_lastDropMs = now;
        g_dirty      = true;
    }
}

void onKey(char ch) {
    if (g_gameOver) {
        if (ch == '\n') reset();
        return;
    }
    switch (ch) {
        case 'p': case 'P':            g_paused = !g_paused; g_dirty = true; break;
        case 'a': case 'A': case key::Left:  if (!g_paused) { tryMove(-1); g_dirty = true; } break;
        case 'd': case 'D': case key::Right: if (!g_paused) { tryMove(+1); g_dirty = true; } break;
        case 's': case 'S': case key::Down:  if (!g_paused) { softDrop();  g_dirty = true; } break;
        case 'k': case 'K':            if (!g_paused) { tryRotate(-1); g_dirty = true; } break;
        case 'l': case 'L':            if (!g_paused) { tryRotate(+1); g_dirty = true; } break;
        case ' ':                       if (!g_paused) { hardDrop();   g_dirty = true; } break;
    }
}

void drawCell(int gridX, int gridY, uint16_t color, int x0, int y0, int cellSize) {
    int px = x0 + gridX * cellSize;
    int py = y0 + gridY * cellSize;
    auto& d = ui::display();
    d.fillRect(px, py, cellSize - 1, cellSize - 1, color);
}

void onDraw() {
    if (!g_dirty) return;
    g_dirty = false;

    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    // Playfield border
    d.drawRect(FIELD_X - 1, FIELD_Y - 1, FIELD_W * CELL + 2, FIELD_H * CELL + 2, COL_BORDER);

    // Locked field
    for (int y = 0; y < FIELD_H; y++) {
        for (int x = 0; x < FIELD_W; x++) {
            uint8_t v = g_field[y][x];
            if (v) drawCell(x, y, COL_PIECE[v - 1], FIELD_X, FIELD_Y, CELL);
        }
    }

    // Falling piece
    if (!g_gameOver) {
        uint16_t pc = COL_PIECE[g_pieceType];
        for (int i = 0; i < 4; i++) {
            int cx = g_px + kPieces[g_pieceType][g_rot][i].dx;
            int cy = g_py + kPieces[g_pieceType][g_rot][i].dy;
            if (cy >= 0 && cy < FIELD_H && cx >= 0 && cx < FIELD_W)
                drawCell(cx, cy, pc, FIELD_X, FIELD_Y, CELL);
        }
    }

    // Sidebar text
    d.setTextSize(1);
    d.setTextColor(COL_DIM);
    d.setCursor(SIDE_X, FIELD_Y);
    d.print("NEXT");

    // Next-piece preview box
    d.drawRect(NEXT_BOX_X - 1, NEXT_BOX_Y - 1, 4 * NEXT_CELL + 2, 4 * NEXT_CELL + 2, COL_BORDER);
    {
        uint16_t pc = COL_PIECE[g_nextType];
        for (int i = 0; i < 4; i++) {
            int cx = kPieces[g_nextType][0][i].dx;
            int cy = kPieces[g_nextType][0][i].dy;
            drawCell(cx, cy, pc, NEXT_BOX_X, NEXT_BOX_Y, NEXT_CELL);
        }
    }

    char buf[24];
    int  y = NEXT_BOX_Y + 4 * NEXT_CELL + 8;
    d.setTextColor(COL_DIM); d.setCursor(SIDE_X, y); d.print("SCORE");
    y += 10;
    d.setTextColor(COL_TEXT); d.setCursor(SIDE_X, y);
    snprintf(buf, sizeof(buf), "%d", g_score);
    d.print(buf);
    y += 12;

    d.setTextColor(COL_DIM); d.setCursor(SIDE_X, y); d.print("LINES");
    y += 10;
    d.setTextColor(COL_TEXT); d.setCursor(SIDE_X, y);
    snprintf(buf, sizeof(buf), "%d", g_lines);
    d.print(buf);
    y += 12;

    d.setTextColor(COL_DIM); d.setCursor(SIDE_X, y); d.print("LEVEL");
    y += 10;
    d.setTextColor(COL_TEXT); d.setCursor(SIDE_X, y);
    snprintf(buf, sizeof(buf), "%d", g_level);
    d.print(buf);

    if (g_paused) {
        d.fillRect(40, 50, 160, 35, COL_BG);
        d.drawRect(40, 50, 160, 35, COL_TEXT);
        d.setTextSize(2); d.setTextColor(COL_TEXT);
        d.setCursor(83, 60); d.print("PAUSE");
        d.setTextSize(1);
    }

    if (g_gameOver) {
        d.fillRect(40, 50, 160, 35, COL_BG);
        d.drawRect(40, 50, 160, 35, COL_PIECE[4]);   // red border
        d.setTextSize(2); d.setTextColor(COL_PIECE[4]);
        d.setCursor(55, 56); d.print("GAME OVER");
        d.setTextSize(1); d.setTextColor(COL_TEXT);
        d.setCursor(48, 76); d.print("enter restart  tab home");
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
