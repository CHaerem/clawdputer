// Home — grid launcher. Arrow keys move between tiles, Enter launches.
// Tab from any other app brings the user back here.
//
// Cardputer's `;./,/` keys (printed with arrow glyphs) work as arrows here
// without holding Fn, because home is declared as keysAsArrows=true. The
// global dispatcher applies the alias; this app just consumes key::Up/etc.

#include <Arduino.h>
#include <M5Cardputer.h>

#include <string>
#include <vector>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

extern void clawd_request_app(const App* app);

namespace {

std::vector<const App*> g_tiles;
int  g_selected = 0;
bool g_dirty    = true;

constexpr int COLS     = 3;
constexpr int TILE_W   = 96;
constexpr int TILE_H   = 72;
constexpr int GAP_X    = 8;
constexpr int GAP_Y    = 10;
constexpr int GRID_TOP = ui::statusbar::HEIGHT + 8;

void rebuildList() {
    g_tiles.clear();
    for (size_t i = 0; i < registry::count(); i++) {
        const App* a = registry::at(i);
        if (strcmp(a->id, "home") == 0) continue;
        g_tiles.push_back(a);
    }
    if (g_selected >= (int)g_tiles.size()) g_selected = 0;
}

uint16_t tileColor(const App* a) {
    if (!a || !a->id) return 0x18E3;
    if (!strcmp(a->id, "buddy"))    return 0x328A;
    if (!strcmp(a->id, "chat"))     return 0x2986;
    if (!strcmp(a->id, "sysinfo"))  return 0x3A86;
    if (!strcmp(a->id, "settings")) return 0x4208;
    return 0x18E3;
}

uint16_t tileAccent(const App* a) {
    if (!a || !a->id) return 0xFFFF;
    if (!strcmp(a->id, "buddy"))    return 0x07FF;
    if (!strcmp(a->id, "chat"))     return 0x5D9F;
    if (!strcmp(a->id, "sysinfo"))  return 0xFD20;
    if (!strcmp(a->id, "settings")) return 0xC618;
    return 0xFFFF;
}

void drawTile(int col, int row, const App* a, bool selected) {
    int x = 6 + col * (TILE_W + GAP_X);
    int y = GRID_TOP + row * (TILE_H + GAP_Y);
    auto& d = ui::display();

    uint16_t bg     = tileColor(a);
    uint16_t accent = tileAccent(a);
    if (selected) {
        d.fillRoundRect(x - 2, y - 2, TILE_W + 4, TILE_H + 4, 6, accent);
    }
    d.fillRoundRect(x, y, TILE_W, TILE_H, 5, bg);

    d.setTextSize(2);
    d.setTextColor(accent);
    d.setCursor(x + 8, y + 10);
    char glyph = a->name[0];
    d.print(glyph);

    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(x + 28, y + 12);
    d.print(a->name);

    if (a->description) {
        d.setTextColor(selected ? 0xFFFF : 0xC618);
        std::string desc = a->description;
        int maxChars = 14;
        if ((int)desc.size() > maxChars) {
            int split = desc.rfind(' ', maxChars);
            if (split < 0) split = maxChars;
            d.setCursor(x + 6, y + 36);
            d.print(desc.substr(0, split).c_str());
            d.setCursor(x + 6, y + 48);
            d.print(desc.substr(split + 1).c_str());
        } else {
            d.setCursor(x + 6, y + 42);
            d.print(desc.c_str());
        }
    }
}

void render() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    for (size_t i = 0; i < g_tiles.size(); i++) {
        int col = i % COLS;
        int row = i / COLS;
        drawTile(col, row, g_tiles[i], (int)i == g_selected);
    }

    d.fillRect(0, 226, 320, 14, 0x1082);
    d.drawFastHLine(0, 226, 320, 0x2945);
    d.setTextColor(0x8C71);
    d.setTextSize(1);
    d.setCursor(4, 230);
    d.print(";,./ or Fn+arrows: move   enter: launch   1-9: jump");

    ui::flush();
}

void launchSelected() {
    if (g_tiles.empty()) return;
    clawd_request_app(g_tiles[g_selected]);
}

void onEnter() {
    rebuildList();
    g_dirty = true;
}

void onExit() {}

void onTick() {
    static uint32_t lastRefresh = 0;
    uint32_t now = millis();
    if (now - lastRefresh >= 1000) {
        lastRefresh = now;
        g_dirty     = true;
    }
}

void onKey(char ch) {
    if (g_tiles.empty()) return;
    int n    = (int)g_tiles.size();
    int row  = g_selected / COLS;
    int col  = g_selected % COLS;
    int rows = (n + COLS - 1) / COLS;

    if (ch == key::Up) {
        row = (row - 1 + rows) % rows;
    } else if (ch == key::Down) {
        row = (row + 1) % rows;
    } else if (ch == key::Left) {
        col = (col - 1 + COLS) % COLS;
    } else if (ch == key::Right) {
        col = (col + 1) % COLS;
    } else if (ch == '\n') {
        launchSelected();
        return;
    } else if (ch >= '1' && ch <= '9') {
        int idx = ch - '1';
        if (idx < n) {
            g_selected = idx;
            launchSelected();
        }
        return;
    } else {
        return;
    }
    int next = row * COLS + col;
    if (next >= n) next = n - 1;
    g_selected = next;
    g_dirty    = true;
}

void onDraw() {
    if (!g_dirty) return;
    render();
    g_dirty = false;
}

App home_app = {
    .id          = "home",
    .name        = "Home",
    .description = "Launcher",
    .services    = SVC_NONE,
    .onEnter     = onEnter,
    .onExit      = onExit,
    .onTick      = onTick,
    .onKey       = onKey,
    .onDraw      = onDraw,
    .onEvent     = nullptr,
};

}  // namespace

REGISTER_APP(home_app);
