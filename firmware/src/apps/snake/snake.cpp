// Snake. Classic grid game, native to the firmware — no ROMs, no SD.
//
// Field is 30 × 13 cells of 8 px each (240 × 104 px, sat below the status
// bar). The snake grows by one cell when it eats food; wall and self
// collision are game over. Step interval shortens as the score climbs,
// floored at 40 ms.
//
// Controls during play:
//   E/S/A/D or arrow keys = direction
//   Enter (on game-over) = restart
//   Tab = home (caught by main.cpp)

#include <Arduino.h>
#include <M5Cardputer.h>

#include <cstring>
#include <deque>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

constexpr int CELL    = 8;
constexpr int COLS    = 30;        // 30 * 8 = 240
constexpr int ROWS    = 13;        // 13 * 8 = 104
constexpr int FIELD_X = 0;
constexpr int FIELD_Y = 30;        // below status bar + a score line

constexpr uint16_t COL_BG    = 0x0000;
constexpr uint16_t COL_GRID  = 0x1082;
constexpr uint16_t COL_HEAD  = 0x07E0;
constexpr uint16_t COL_BODY  = 0x05E0;
constexpr uint16_t COL_FOOD  = 0xF800;
constexpr uint16_t COL_TEXT  = 0xFFE0;
constexpr uint16_t COL_DIM   = 0x8C71;
constexpr uint16_t COL_BORDER = 0x4208;

enum class Dir { Up, Down, Left, Right };
struct Cell { int8_t x, y; };

std::deque<Cell> g_snake;
Cell             g_food;
Dir              g_dir       = Dir::Right;
Dir              g_nextDir   = Dir::Right;
int              g_score     = 0;
int              g_best      = 0;
bool             g_gameOver  = false;
uint32_t         g_lastStepMs    = 0;
uint32_t         g_stepIntervalMs = 150;
bool             g_dirty     = true;

void placeFood() {
    for (int tries = 0; tries < 200; tries++) {
        Cell c{ (int8_t)random(0, COLS), (int8_t)random(0, ROWS) };
        bool collision = false;
        for (auto& s : g_snake) {
            if (s.x == c.x && s.y == c.y) { collision = true; break; }
        }
        if (!collision) { g_food = c; return; }
    }
    // Field is nearly full; just pick something — game's about to win.
    g_food = { 0, 0 };
}

void reset() {
    g_snake.clear();
    g_snake.push_back({ (int8_t)(COLS / 2),     (int8_t)(ROWS / 2) });
    g_snake.push_back({ (int8_t)(COLS / 2 - 1), (int8_t)(ROWS / 2) });
    g_snake.push_back({ (int8_t)(COLS / 2 - 2), (int8_t)(ROWS / 2) });
    g_dir            = Dir::Right;
    g_nextDir        = Dir::Right;
    g_score          = 0;
    g_gameOver       = false;
    g_stepIntervalMs = 150;
    g_lastStepMs     = millis();
    placeFood();
    g_dirty = true;
}

bool isReverse(Dir a, Dir b) {
    return (a == Dir::Up    && b == Dir::Down)  ||
           (a == Dir::Down  && b == Dir::Up)    ||
           (a == Dir::Left  && b == Dir::Right) ||
           (a == Dir::Right && b == Dir::Left);
}

void step() {
    if (!isReverse(g_dir, g_nextDir)) g_dir = g_nextDir;

    Cell head = g_snake.front();
    switch (g_dir) {
        case Dir::Up:    head.y--; break;
        case Dir::Down:  head.y++; break;
        case Dir::Left:  head.x--; break;
        case Dir::Right: head.x++; break;
    }

    if (head.x < 0 || head.x >= COLS || head.y < 0 || head.y >= ROWS) {
        g_gameOver = true;
        if (g_score > g_best) g_best = g_score;
        return;
    }
    for (auto& s : g_snake) {
        if (s.x == head.x && s.y == head.y) {
            g_gameOver = true;
            if (g_score > g_best) g_best = g_score;
            return;
        }
    }

    g_snake.push_front(head);

    if (head.x == g_food.x && head.y == g_food.y) {
        g_score++;
        if (g_stepIntervalMs > 50) g_stepIntervalMs -= 4;
        placeFood();
    } else {
        g_snake.pop_back();
    }
}

void onEnter() { reset(); }
void onExit()  {}

void onTick() {
    if (g_gameOver) return;
    uint32_t now = millis();
    if (now - g_lastStepMs >= g_stepIntervalMs) {
        step();
        g_lastStepMs = now;
        g_dirty = true;
    }
}

void onKey(char ch) {
    if (g_gameOver) {
        if (ch == '\n') reset();
        return;
    }
    switch (ch) {
        case 'e': case 'E': case key::Up:    g_nextDir = Dir::Up;    break;
        case 's': case 'S': case key::Down:  g_nextDir = Dir::Down;  break;
        case 'a': case 'A': case key::Left:  g_nextDir = Dir::Left;  break;
        case 'd': case 'D': case key::Right: g_nextDir = Dir::Right; break;
    }
}

void onDraw() {
    if (!g_dirty) return;
    g_dirty = false;

    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    // Score line
    d.setTextSize(1);
    d.setTextColor(COL_TEXT);
    d.setCursor(4, 18);
    char buf[40];
    snprintf(buf, sizeof(buf), "score:%d   best:%d", g_score, g_best);
    d.print(buf);

    // Playfield border
    d.drawRect(FIELD_X, FIELD_Y - 1, COLS * CELL, ROWS * CELL + 2, COL_BORDER);

    // Snake
    for (size_t i = 0; i < g_snake.size(); i++) {
        auto& c = g_snake[i];
        uint16_t col = (i == 0) ? COL_HEAD : COL_BODY;
        d.fillRect(FIELD_X + c.x * CELL,
                   FIELD_Y + c.y * CELL,
                   CELL - 1, CELL - 1, col);
    }

    // Food
    d.fillRect(FIELD_X + g_food.x * CELL,
               FIELD_Y + g_food.y * CELL,
               CELL - 1, CELL - 1, COL_FOOD);

    // Game over overlay
    if (g_gameOver) {
        d.fillRect(40, 50, 160, 35, COL_BG);
        d.drawRect(40, 50, 160, 35, COL_FOOD);
        d.setTextSize(2);
        d.setTextColor(COL_FOOD);
        d.setCursor(55, 56);
        d.print("GAME OVER");
        d.setTextSize(1);
        d.setTextColor(COL_TEXT);
        d.setCursor(48, 76);
        d.print("enter restart  tab home");
    }

    ui::flush();
}

App snake_app = {
    .id           = "snake",
    .name         = "Snake",
    .description  = "Eat dots, don't bite yourself",
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

REGISTER_APP(snake_app);
