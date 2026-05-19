// Snake. Classic grid game, native to the firmware — no ROMs, no SD.
//
// Start screen lets the player pick control mode (keys or device tilt via
// IMU), toggle layout (landscape / portrait), or open the remap menu to
// rebind direction keys. Highscore is persisted in NVS namespace "snake"
// alongside the chosen mode / layout / bindings.
//
// During play:
//   bound keys (default e/s/a/d) or arrow keys = direction
//   enter (on game-over) = restart
//   tab = home (caught by main.cpp)

#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>

#include <cstring>
#include <deque>
#include <math.h>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

enum class State   : uint8_t { Start, Playing, GameOver, Remap };
enum class Dir     : uint8_t { Up, Down, Left, Right };
enum class Control : uint8_t { Keys, Tilt };
struct Cell { int8_t x, y; };

constexpr int CELL = 8;
constexpr int COLS_L = 30;     // landscape 240×135 → playfield 240×104
constexpr int ROWS_L = 13;
constexpr int COLS_P = 16;     // portrait  135×240 → playfield 128×224
constexpr int ROWS_P = 28;

constexpr uint16_t COL_BG     = 0x0000;
constexpr uint16_t COL_BORDER = 0x4208;
constexpr uint16_t COL_HEAD   = 0x07E0;
constexpr uint16_t COL_BODY   = 0x05E0;
constexpr uint16_t COL_FOOD   = 0xF800;
constexpr uint16_t COL_TEXT   = 0xFFE0;
constexpr uint16_t COL_DIM    = 0x8C71;
constexpr uint16_t COL_ACCENT = 0x07FF;

struct Bindings {
    char up    = 'e';
    char down  = 's';
    char left  = 'a';
    char right = 'd';
};

State    g_state     = State::Start;
Control  g_control   = Control::Keys;
bool     g_portrait  = false;
std::deque<Cell> g_snake;
Cell     g_food;
Dir      g_dir              = Dir::Right;
Dir      g_nextDir          = Dir::Right;
int      g_score            = 0;
int      g_best             = 0;
uint32_t g_lastStepMs       = 0;
uint32_t g_stepIntervalMs   = 150;
uint32_t g_lastTiltMs       = 0;
bool     g_dirty            = true;
Bindings g_bind;
int      g_remapSel         = 0;   // 0..3 = up/down/left/right

int fieldCols() { return g_portrait ? COLS_P : COLS_L; }
int fieldRows() { return g_portrait ? ROWS_P : ROWS_L; }
int fieldX()    { return g_portrait ? 3 : 0; }
int fieldY()    { return g_portrait ? 14 : 30; }
int screenW()   { return g_portrait ? 135 : 240; }
int screenH()   { return g_portrait ? 240 : 135; }

void loadPrefs() {
    Preferences p;
    p.begin("snake", true);
    g_best       = p.getInt  ("best", 0);
    g_portrait   = p.getBool ("portrait", false);
    g_control    = (Control)p.getUChar("ctrl", (uint8_t)Control::Keys);
    g_bind.up    = (char)p.getUChar("ku", 'e');
    g_bind.down  = (char)p.getUChar("kd", 's');
    g_bind.left  = (char)p.getUChar("kl", 'a');
    g_bind.right = (char)p.getUChar("kr", 'd');
    p.end();
}

void savePrefs() {
    Preferences p;
    p.begin("snake", false);
    p.putInt  ("best",     g_best);
    p.putBool ("portrait", g_portrait);
    p.putUChar("ctrl",     (uint8_t)g_control);
    p.putUChar("ku",       (uint8_t)g_bind.up);
    p.putUChar("kd",       (uint8_t)g_bind.down);
    p.putUChar("kl",       (uint8_t)g_bind.left);
    p.putUChar("kr",       (uint8_t)g_bind.right);
    p.end();
}

void applyOrientation() {
    if (g_portrait) ui::reconfigureCanvas(135, 240, 0);
    else            ui::reconfigureCanvas(240, 135, 1);
    g_dirty = true;
}

void placeFood() {
    for (int t = 0; t < 200; t++) {
        Cell c{ (int8_t)random(0, fieldCols()), (int8_t)random(0, fieldRows()) };
        bool collision = false;
        for (auto& s : g_snake)
            if (s.x == c.x && s.y == c.y) { collision = true; break; }
        if (!collision) { g_food = c; return; }
    }
    g_food = { 0, 0 };
}

void resetRun() {
    g_snake.clear();
    int cx = fieldCols() / 2;
    int cy = fieldRows() / 2;
    g_snake.push_back({ (int8_t)cx,       (int8_t)cy });
    g_snake.push_back({ (int8_t)(cx - 1), (int8_t)cy });
    g_snake.push_back({ (int8_t)(cx - 2), (int8_t)cy });
    g_dir            = Dir::Right;
    g_nextDir        = Dir::Right;
    g_score          = 0;
    g_stepIntervalMs = 150;
    g_lastStepMs     = millis();
    placeFood();
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
    if (head.x < 0 || head.x >= fieldCols() || head.y < 0 || head.y >= fieldRows()) {
        g_state = State::GameOver;
        if (g_score > g_best) { g_best = g_score; savePrefs(); }
        return;
    }
    for (auto& s : g_snake) {
        if (s.x == head.x && s.y == head.y) {
            g_state = State::GameOver;
            if (g_score > g_best) { g_best = g_score; savePrefs(); }
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

// Map gravity vector → discrete direction. In portrait the device is held
// rotated 90° CW, so the IMU's natural axes need swapping to keep "tilt
// right" → snake-right intuitive.
void readTilt() {
    if (!M5.Imu.isEnabled()) return;
    uint32_t now = millis();
    if (now - g_lastTiltMs < 40) return;
    g_lastTiltMs = now;
    M5.Imu.update();
    auto data = M5.Imu.getImuData();
    float x = data.accel.x;
    float y = data.accel.y;
    if (g_portrait) { float t = x; x = -y; y = t; }
    constexpr float TH = 0.30f;
    if (fabsf(x) > fabsf(y)) {
        if      (x >  TH) g_nextDir = Dir::Right;
        else if (x < -TH) g_nextDir = Dir::Left;
    } else {
        if      (y >  TH) g_nextDir = Dir::Down;
        else if (y < -TH) g_nextDir = Dir::Up;
    }
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
    if (g_control == Control::Tilt) readTilt();
    uint32_t now = millis();
    if (now - g_lastStepMs >= g_stepIntervalMs) {
        step();
        g_lastStepMs = now;
        g_dirty = true;
    }
}

void onKeyStart(char ch) {
    switch (ch) {
        case '\n':
            resetRun();
            g_state = State::Playing;
            g_dirty = true;
            break;
        case 'g': case 'G':
            if (g_control == Control::Keys && M5.Imu.isEnabled())
                g_control = Control::Tilt;
            else
                g_control = Control::Keys;
            savePrefs();
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
    Dir    target = g_dir;
    bool   gotDir = false;
    if      (ch == g_bind.up    || ch == key::Up)    { target = Dir::Up;    gotDir = true; }
    else if (ch == g_bind.down  || ch == key::Down)  { target = Dir::Down;  gotDir = true; }
    else if (ch == g_bind.left  || ch == key::Left)  { target = Dir::Left;  gotDir = true; }
    else if (ch == g_bind.right || ch == key::Right) { target = Dir::Right; gotDir = true; }
    if (gotDir) { g_nextDir = target; return; }
    // Mid-game orientation toggle. Field dimensions change between modes,
    // so the run is restarted — no clean way to remap a snake from a
    // 30×13 field onto a 16×28 one.
    if (ch == 'r' || ch == 'R') {
        g_portrait = !g_portrait;
        savePrefs();
        applyOrientation();
        resetRun();
        g_dirty = true;
    }
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
    if (ch == key::Up)   { g_remapSel = (g_remapSel + 3) % 4; g_dirty = true; return; }
    if (ch == key::Down) { g_remapSel = (g_remapSel + 1) % 4; g_dirty = true; return; }
    if (ch == '\n')      { savePrefs(); g_state = State::Start; g_dirty = true; return; }
    if (ch >= 0x20 && ch <= 0x7E) {
        char* slot[] = { &g_bind.up, &g_bind.down, &g_bind.left, &g_bind.right };
        *slot[g_remapSel] = ch;
        g_dirty = true;
    }
}

void onKey(char ch) {
    switch (g_state) {
        case State::Start:    onKeyStart(ch);    break;
        case State::Playing:  onKeyPlaying(ch);  break;
        case State::GameOver: onKeyGameOver(ch); break;
        case State::Remap:    onKeyRemap(ch);    break;
    }
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
    drawCenteredText(g_portrait ? 30 : 14, "S N A K E", COL_HEAD, 2);
    snprintf(buf, sizeof(buf), "best: %d", g_best);
    drawCenteredText(g_portrait ? 60 : 38, buf, COL_TEXT);

    int y  = g_portrait ? 100 : 60;
    int lh = 12;
    drawCenteredText(y, "enter  play", COL_ACCENT); y += lh + 2;
    snprintf(buf, sizeof(buf), "G  ctrl: %s",
             g_control == Control::Tilt ? "tilt" : "keys");
    drawCenteredText(y, buf, COL_DIM); y += lh;
    snprintf(buf, sizeof(buf), "R  view: %s",
             g_portrait ? "portrait" : "landscape");
    drawCenteredText(y, buf, COL_DIM); y += lh;
    drawCenteredText(y, "M  remap keys",     COL_DIM); y += lh;
    drawCenteredText(y, "tab  home",         COL_DIM);
}

void drawHud() {
    auto& d = ui::display();
    if (g_portrait) {
        d.setTextSize(1);
        d.setTextColor(COL_TEXT);
        char buf[32];
        snprintf(buf, sizeof(buf), "%d  best:%d", g_score, g_best);
        d.setCursor(4, 3);
        d.print(buf);
        const char* mode = (g_control == Control::Tilt) ? "tilt" : "keys";
        d.setTextColor(COL_DIM);
        int w = (int)strlen(mode) * 6;
        d.setCursor(screenW() - 4 - w, 3);
        d.print(mode);
    } else {
        ui::statusbar::draw();
        d.setTextSize(1);
        d.setTextColor(COL_TEXT);
        d.setCursor(4, 18);
        char buf[40];
        snprintf(buf, sizeof(buf), "score:%d  best:%d  [%s]",
                 g_score, g_best,
                 g_control == Control::Tilt ? "tilt" : "keys");
        d.print(buf);
    }
}

void drawField() {
    auto& d = ui::display();
    int fx = fieldX(), fy = fieldY();
    int fw = fieldCols() * CELL, fh = fieldRows() * CELL;
    d.drawRect(fx, fy - 1, fw, fh + 2, COL_BORDER);

    for (size_t i = 0; i < g_snake.size(); i++) {
        auto& c = g_snake[i];
        uint16_t col = (i == 0) ? COL_HEAD : COL_BODY;
        d.fillRect(fx + c.x * CELL, fy + c.y * CELL, CELL - 1, CELL - 1, col);
    }
    d.fillRect(fx + g_food.x * CELL, fy + g_food.y * CELL,
               CELL - 1, CELL - 1, COL_FOOD);
}

void drawGameOver() {
    auto& d = ui::display();
    int bw = 160, bh = 35;
    int bx = (screenW() - bw) / 2;
    int by = (screenH() - bh) / 2;
    d.fillRect(bx, by, bw, bh, COL_BG);
    d.drawRect(bx, by, bw, bh, COL_FOOD);
    d.setTextSize(2);
    d.setTextColor(COL_FOOD);
    d.setCursor(bx + 15, by + 6);
    d.print("GAME OVER");
    d.setTextSize(1);
    d.setTextColor(COL_TEXT);
    d.setCursor(bx + 8, by + 26);
    d.print("enter restart  tab home");
}

void drawRemap() {
    auto& d = ui::display();
    drawCenteredText(g_portrait ? 24 : 16, "REMAP KEYS", COL_ACCENT);
    const char* labels[] = { "up", "down", "left", "right" };
    char* bvals[] = { &g_bind.up, &g_bind.down, &g_bind.left, &g_bind.right };
    int y = g_portrait ? 60 : 40;
    for (int i = 0; i < 4; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%c %-5s : %c",
                 i == g_remapSel ? '>' : ' ',
                 labels[i], *bvals[i]);
        d.setTextSize(1);
        d.setTextColor(i == g_remapSel ? COL_ACCENT : COL_DIM);
        d.setCursor(g_portrait ? 20 : 70, y);
        d.print(buf);
        y += 14;
    }
    drawCenteredText(y + 8,  "press a key to bind", COL_DIM);
    drawCenteredText(y + 22, "enter  back",          COL_DIM);
}

void onDraw() {
    if (!g_dirty) return;
    g_dirty = false;
    ui::beginFrame();
    switch (g_state) {
        case State::Start:    drawStartScreen(); break;
        case State::Remap:    drawRemap();       break;
        case State::Playing:
        case State::GameOver:
            drawHud();
            drawField();
            if (g_state == State::GameOver) drawGameOver();
            break;
    }
    ui::flush();
}

App snake_app = {
    .id           = "snake",
    .name         = "Snake",
    .description  = "Eat dots, tilt or type",
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
