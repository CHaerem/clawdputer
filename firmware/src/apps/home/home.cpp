// Home / launcher app. Lists every registered app (except itself) with a
// short status line, and launches the selected one on Enter. Tab from any
// other app brings the user back here, so this also serves as the "back"
// button.

#include <Arduino.h>
#include <M5Cardputer.h>

#include <vector>

#include "core/app.h"
#include "core/key.h"
#include "core/registry.h"
#include "services/ble.h"
#include "services/wifi.h"

// Implemented in main.cpp — queues an app switch to be applied on the next
// loop iteration.
extern void clawd_request_app(const App* app);

namespace {

std::vector<const App*> g_others;
int  g_selected = 0;
bool g_dirty    = true;

void rebuildList() {
    g_others.clear();
    for (size_t i = 0; i < registry::count(); i++) {
        const App* a = registry::at(i);
        if (strcmp(a->id, "home") == 0) continue;
        g_others.push_back(a);
    }
    if (g_selected >= (int)g_others.size()) g_selected = 0;
}

void drawHeader() {
    auto& d = M5Cardputer.Display;
    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(6, 4);
    d.print("clawdputer");

    d.setTextSize(1);
    d.setCursor(6, 24);
    d.setTextColor(ble::isConnected(EventSource::NusLink) ? 0x07E0 : 0x4208);
    d.print("buddy");
    d.setTextColor(0x7BEF);
    d.print(" · ");
    d.setTextColor(ble::isConnected(EventSource::BridgeLink) ? 0x07E0 : 0x4208);
    d.print("bridge");
    d.setTextColor(0x7BEF);
    d.print(" · ");
    if (wifi::isConnected()) {
        d.setTextColor(0x07E0);
        d.print(wifi::ip().c_str());
    } else {
        d.setTextColor(0x4208);
        d.print("wifi");
    }
}

void drawFooter() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 224, 320, 16, 0x2104);
    d.setTextColor(0x7BEF);
    d.setCursor(6, 228);
    d.print("[Fn+;/.] move  [Enter] launch  [TAB] home");
}

void render() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    drawHeader();

    int y = 48;
    for (size_t i = 0; i < g_others.size(); i++) {
        bool selected = (int)i == g_selected;
        if (selected) {
            d.fillRoundRect(4, y - 2, 312, 28, 4, 0x18E3);
        }
        d.setCursor(12, y);
        d.setTextColor(selected ? 0xFFE0 : WHITE);
        d.setTextSize(1);
        d.printf("%u.", (unsigned)(i + 1));
        d.setCursor(28, y);
        d.print(g_others[i]->name);
        if (g_others[i]->description) {
            d.setCursor(12, y + 12);
            d.setTextColor(selected ? 0xFFFF : 0x7BEF);
            d.print(g_others[i]->description);
        }
        y += 32;
        if (y > 210) break;
    }

    drawFooter();
}

void launchSelected() {
    if (g_others.empty()) return;
    clawd_request_app(g_others[g_selected]);
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
        g_dirty     = true;  // refresh status row
    }
}

void onKey(char ch) {
    if (ch == key::Up) {
        if (g_others.empty()) return;
        g_selected = (g_selected - 1 + g_others.size()) % g_others.size();
        g_dirty    = true;
    } else if (ch == key::Down) {
        if (g_others.empty()) return;
        g_selected = (g_selected + 1) % g_others.size();
        g_dirty    = true;
    } else if (ch == '\n') {
        launchSelected();
    } else if (ch >= '1' && ch <= '9') {
        int idx = ch - '1';
        if (idx < (int)g_others.size()) {
            g_selected = idx;
            launchSelected();
        }
    }
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
