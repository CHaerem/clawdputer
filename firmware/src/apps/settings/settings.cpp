// Settings app — actions and read-only diagnostics. Up/Down to move,
// Enter to invoke the selected action. Read-only rows are skipped during
// navigation.

#include <Arduino.h>
#include <M5Cardputer.h>

#include <string>
#include <vector>

#include "core/app.h"
#include "core/key.h"
#include "services/ble.h"
#include "services/updater.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

namespace {

struct Item {
    std::string label;
    std::string value;       // empty if action-only
    void (*action)();        // nullptr for read-only rows
};

std::vector<Item> g_items;
int  g_selected = 0;
bool g_dirty    = true;
std::string g_toast;
uint32_t    g_toastUntil = 0;

void toast(const std::string& msg) {
    g_toast       = msg;
    g_toastUntil  = millis() + 2500;
    g_dirty       = true;
}

void actClearBonds() {
    ble::clearBonds();
    toast("BLE bonds cleared");
}

void actReboot() {
    toast("rebooting…");
    delay(400);
    ESP.restart();
}

void actCheckUpdate() {
    updater::checkNow();
    toast("checking for updates…");
}

void actClearWifi() {
    wifi::clearCredentials();
    toast("WiFi cleared (reboot to apply)");
}

void rebuild() {
    g_items.clear();
    g_items.push_back({"device",     ble::deviceName(),                                          nullptr});
    g_items.push_back({"buddy",      ble::isConnected(EventSource::NusLink)   ? "connected" : "—", nullptr});
    g_items.push_back({"bridge",     ble::isConnected(EventSource::BridgeLink)? "connected" : "—", nullptr});
    g_items.push_back({"wifi ssid",  wifi::ssid().empty() ? std::string("(unset)") : wifi::ssid(), nullptr});
    g_items.push_back({"wifi ip",    wifi::isConnected() ? wifi::ip() : std::string("—"),         nullptr});
    g_items.push_back({"build",      updater::currentVersion(),                                   nullptr});

    std::string ver = updater::latestVersion();
    std::string latestStr = ver.empty()
        ? std::string(updater::statusText())
        : (ver + std::string(" (") + updater::statusText() + ")");
    g_items.push_back({"latest",     latestStr,                                                   nullptr});

#ifdef CLAWD_OTA_PASSWORD
    g_items.push_back({"ota password", "set",                                                    nullptr});
#else
    g_items.push_back({"ota password", "(unset)",                                                nullptr});
#endif
    g_items.push_back({"check for updates", "",                                                  actCheckUpdate});
    g_items.push_back({"clear WiFi creds",  "",                                                  actClearWifi});
    g_items.push_back({"clear BLE bonds",   "",                                                  actClearBonds});
    g_items.push_back({"reboot device",     "",                                                  actReboot});

    if (g_selected >= (int)g_items.size()) g_selected = (int)g_items.size() - 1;
    if (g_selected < 0) g_selected = 0;
}

bool isAction(int idx) {
    return idx >= 0 && idx < (int)g_items.size() && g_items[idx].action != nullptr;
}

void moveSelection(int dir) {
    if (g_items.empty()) return;
    int n = (int)g_items.size();
    int next = g_selected;
    for (int i = 0; i < n; i++) {
        next = (next + dir + n) % n;
        // Allow stopping on any row, but prefer actions when there are actions
        // available. For simplicity: stop on every row.
        break;
    }
    g_selected = next;
    g_dirty    = true;
}

void render() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(6, ui::statusbar::HEIGHT + 6);
    d.print("Settings");

    d.setTextSize(1);
    int y          = ui::statusbar::HEIGHT + 32;
    int row_h      = 16;
    int viewport_h = 226 - y;
    int max_rows   = viewport_h / row_h;

    // Simple scroll: keep selected in view
    int top = 0;
    if (g_selected >= max_rows) top = g_selected - max_rows + 1;

    for (int i = top; i < (int)g_items.size() && i < top + max_rows; i++) {
        bool selected = i == g_selected;
        if (selected) d.fillRoundRect(4, y - 2, 312, row_h, 3, 0x18E3);

        d.setCursor(10, y);
        d.setTextColor(selected ? 0xFFE0 : 0x8C71);
        d.print(g_items[i].label.c_str());

        d.setCursor(140, y);
        bool action = g_items[i].action != nullptr;
        if (action) {
            d.setTextColor(selected ? 0xFFFF : 0xFFE0);
            d.print("press enter ›");
        } else {
            d.setTextColor(selected ? 0xFFFF : WHITE);
            d.print(g_items[i].value.c_str());
        }
        y += row_h;
    }

    if (!g_toast.empty() && millis() < g_toastUntil) {
        d.fillRect(0, 196, 320, 28, 0x4800);
        d.setTextColor(WHITE);
        d.setCursor(8, 206);
        d.print(g_toast.c_str());
    }

    // Footer hint
    d.fillRect(0, 226, 320, 14, 0x1082);
    d.drawFastHLine(0, 226, 320, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 230);
    d.print("up/down: move   enter: invoke   tab: home");

    ui::flush();
}

void onEnter() {
    rebuild();
    g_dirty = true;
}

void onExit() {}

void onTick() {
    static uint32_t lastRefresh = 0;
    uint32_t now = millis();
    if (now - lastRefresh >= 1000) {
        lastRefresh = now;
        rebuild();
        g_dirty = true;
    }
    if (!g_toast.empty() && now >= g_toastUntil) {
        g_toast.clear();
        g_dirty = true;
    }
}

void onKey(char ch) {
    if (ch == key::Up) {
        moveSelection(-1);
    } else if (ch == key::Down) {
        moveSelection(+1);
    } else if (ch == '\n') {
        if (isAction(g_selected)) g_items[g_selected].action();
    }
}

void onDraw() {
    if (!g_dirty) return;
    render();
    g_dirty = false;
}

App settings_app = {
    .id          = "settings",
    .name        = "Settings",
    .description = "Info & actions",
    .services    = SVC_NONE,
    .onEnter     = onEnter,
    .onExit      = onExit,
    .onTick      = onTick,
    .onKey       = onKey,
    .onDraw      = onDraw,
    .onEvent     = nullptr,
};

}  // namespace

REGISTER_APP(settings_app);
