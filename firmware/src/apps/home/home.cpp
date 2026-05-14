// Home — coverflow launcher for the 240x135 display. One card center-screen
// shows the selected app; peek slivers on either side hint at neighbours.

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

constexpr int CARD_W = 140;
constexpr int CARD_H = 88;
constexpr int CARD_X = (SCREEN_W - CARD_W) / 2;
constexpr int CARD_Y = ui::statusbar::HEIGHT + 4;
constexpr int PEEK_W = 22;
constexpr int PEEK_H = 70;
constexpr int LEFT_PEEK_X  = 4;
constexpr int RIGHT_PEEK_X = SCREEN_W - PEEK_W - 4;
constexpr int PEEK_Y       = CARD_Y + (CARD_H - PEEK_H) / 2;
constexpr int FOOTER_Y     = 124;

void rebuildList() {
    g_tiles.clear();
    for (size_t i = 0; i < registry::count(); i++) {
        const App* a = registry::at(i);
        if (strcmp(a->id, "home") == 0) continue;
        g_tiles.push_back(a);
    }
    if (g_selected >= (int)g_tiles.size()) g_selected = 0;
}

uint16_t tileBg(const App* a) {
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

void drawWrapped(const std::string& s, int x, int y, int maxChars, uint16_t color) {
    auto& d = ui::display();
    d.setTextColor(color);

    size_t i    = 0;
    int    line = 0;
    while (i < s.size() && line < 2) {
        size_t end = i + maxChars;
        if (end >= s.size()) end = s.size();
        else {
            size_t sp = s.rfind(' ', end);
            if (sp != std::string::npos && sp > i) end = sp;
        }
        d.setCursor(x, y + line * 10);
        d.print(s.substr(i, end - i).c_str());
        i = end;
        while (i < s.size() && s[i] == ' ') i++;
        line++;
    }
}

void drawPeek(int x, const App* a) {
    if (!a) return;
    auto& d = ui::display();
    d.fillRoundRect(x, PEEK_Y, PEEK_W, PEEK_H, 4, tileBg(a));
    d.setTextSize(2);
    d.setTextColor(tileAccent(a));
    d.setCursor(x + 6, PEEK_Y + 28);
    char glyph = a->name[0];
    d.print(glyph);
}

void drawCard(const App* a) {
    if (!a) return;
    auto& d = ui::display();

    uint16_t bg     = tileBg(a);
    uint16_t accent = tileAccent(a);

    d.fillRoundRect(CARD_X - 2, CARD_Y - 2, CARD_W + 4, CARD_H + 4, 6, accent);
    d.fillRoundRect(CARD_X,     CARD_Y,     CARD_W,     CARD_H,     5, bg);

    // Glyph (left side)
    d.setTextSize(3);
    d.setTextColor(accent);
    d.setCursor(CARD_X + 10, CARD_Y + 14);
    char glyph = a->name[0];
    d.print(glyph);

    // Title (right of glyph)
    d.setTextSize(2);
    d.setTextColor(0xFFFF);
    d.setCursor(CARD_X + 42, CARD_Y + 18);
    d.print(a->name);

    // Description (wraps under)
    if (a->description) {
        d.setTextSize(1);
        drawWrapped(a->description, CARD_X + 8, CARD_Y + 46,
                    (CARD_W - 16) / 6, 0xFFFF);
    }

    // "press enter" hint
    d.setTextSize(1);
    d.setTextColor(accent);
    d.setCursor(CARD_X + 8, CARD_Y + CARD_H - 10);
    d.print("press enter \xC3\xAB");  // small accent so it's clearly a CTA
}

void drawDots() {
    auto& d = ui::display();
    int n = (int)g_tiles.size();
    if (n <= 1) return;

    int dotSpacing = 8;
    int dotW       = dotSpacing * (n - 1) + 4;
    int dotX       = (SCREEN_W - dotW) / 2;
    int dotY       = CARD_Y + CARD_H + 6;

    for (int i = 0; i < n; i++) {
        bool sel = i == g_selected;
        int  cx  = dotX + i * dotSpacing + 2;
        if (sel) d.fillCircle(cx, dotY, 2, 0xFFFF);
        else     d.fillCircle(cx, dotY, 1, 0x4208);
    }
}

void render() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    if (g_tiles.empty()) {
        d.setTextColor(0x8C71);
        d.setCursor(50, 60);
        d.print("no apps registered");
        ui::flush();
        return;
    }

    int n = (int)g_tiles.size();
    if (n > 1) {
        int prev = (g_selected - 1 + n) % n;
        int next = (g_selected + 1) % n;
        drawPeek(LEFT_PEEK_X,  g_tiles[prev]);
        drawPeek(RIGHT_PEEK_X, g_tiles[next]);
    }

    drawCard(g_tiles[g_selected]);
    drawDots();

    // Footer hint
    d.fillRect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, 0x1082);
    d.drawFastHLine(0, FOOTER_Y, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setTextSize(1);
    d.setCursor(4, FOOTER_Y + 3);
    d.print(",/ switch  enter launch  1-9 jump");

    ui::flush();
}

void launchSelected() {
    if (g_tiles.empty()) return;
    clawd_request_app(g_tiles[g_selected]);
}

void onEnter() { rebuildList(); g_dirty = true; }
void onExit()  {}

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
    int n = (int)g_tiles.size();

    if (ch == key::Left || ch == key::Up) {
        g_selected = (g_selected - 1 + n) % n;
        g_dirty    = true;
    } else if (ch == key::Right || ch == key::Down) {
        g_selected = (g_selected + 1) % n;
        g_dirty    = true;
    } else if (ch == '\n') {
        launchSelected();
    } else if (ch >= '1' && ch <= '9') {
        int idx = ch - '1';
        if (idx < n) {
            g_selected = idx;
            launchSelected();
        }
    }
}

void onDraw() { if (g_dirty) { render(); g_dirty = false; } }

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
