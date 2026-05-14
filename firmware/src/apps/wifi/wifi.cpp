// WiFi setup app — scan local SSIDs, pick one, type the password, connect.
// Credentials are persisted in NVS so subsequent boots come up on the same
// network without intervention.

#include <Arduino.h>
#include <M5Cardputer.h>

#include <string>
#include <vector>

#include "core/app.h"
#include "core/key.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

enum class Stage : uint8_t {
    Idle,        // initial — prompts the user to scan
    Scanning,
    List,        // list of scanned SSIDs
    Password,    // typing password
    Connecting,
    Connected,
    Failed,
};

Stage       g_stage = Stage::Idle;
bool        g_dirty = true;
int         g_selected = 0;
int         g_scrollTop = 0;
std::string g_pickedSsid;
bool        g_pickedSecure = true;
std::string g_password;
std::string g_status;
uint32_t    g_connectStart = 0;

constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

void render();

void drawFooter(const char* hint) {
    auto& d = ui::display();
    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.print(hint);
}

void renderIdle() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, ui::statusbar::HEIGHT + 4);
    d.print("WiFi setup");

    d.setCursor(6, ui::statusbar::HEIGHT + 22);
    d.setTextColor(0x8C71);
    d.print("current:");
    d.setCursor(60, ui::statusbar::HEIGHT + 22);
    d.setTextColor(0xFFFF);
    if (wifi::isConnected()) {
        d.printf("%s (%s)", wifi::ssid().c_str(), wifi::ip().c_str());
    } else if (!wifi::ssid().empty()) {
        d.printf("%s (offline)", wifi::ssid().c_str());
    } else {
        d.print("(none)");
    }

    d.setCursor(6, ui::statusbar::HEIGHT + 50);
    d.setTextColor(0xFFE0);
    d.print("press enter to scan");

    drawFooter("enter scan   tab home");
    ui::flush();
}

void renderScanning() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xFFE0);
    d.setCursor(6, 50);
    d.print("scanning…");
    drawFooter("");
    ui::flush();
}

void renderList() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    int count = wifi::scanStatus();
    if (count <= 0) {
        d.setTextSize(1);
        d.setTextColor(0xF800);
        d.setCursor(6, 50);
        d.print("no networks found");
        drawFooter("enter rescan   tab home");
        ui::flush();
        return;
    }

    d.setTextSize(1);
    int y = ui::statusbar::HEIGHT + 2;
    int rowH = 11;
    int maxRows = (124 - y) / rowH;

    if (g_selected < g_scrollTop) g_scrollTop = g_selected;
    if (g_selected >= g_scrollTop + maxRows) g_scrollTop = g_selected - maxRows + 1;

    for (int i = g_scrollTop; i < count && i < g_scrollTop + maxRows; i++) {
        bool sel = i == g_selected;
        if (sel) d.fillRoundRect(2, y - 1, SCREEN_W - 4, rowH, 2, 0x18E3);

        auto r = wifi::scanNetwork(i);

        // RSSI bars on the left
        int bars = 0;
        if      (r.rssi > -55) bars = 4;
        else if (r.rssi > -65) bars = 3;
        else if (r.rssi > -75) bars = 2;
        else if (r.rssi > -85) bars = 1;
        for (int b = 0; b < 4; b++) {
            uint16_t c = b < bars ? 0x07E0 : 0x4208;
            d.fillRect(4 + b * 4, y + 7 - b * 2, 3, 2 + b * 2, c);
        }

        // Lock glyph
        d.setCursor(24, y + 1);
        d.setTextColor(r.secure ? 0xFFE0 : 0x4208);
        d.print(r.secure ? "\xC2\xB7" : " ");

        // SSID
        d.setCursor(34, y + 1);
        d.setTextColor(sel ? 0xFFE0 : 0xFFFF);
        std::string s = r.ssid;
        if (s.empty()) s = "(hidden)";
        if (s.size() > 28) s = s.substr(0, 27) + "…";
        d.print(s.c_str());

        y += rowH;
    }

    drawFooter(";/  move   enter connect   tab home");
    ui::flush();
}

void renderPassword() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, ui::statusbar::HEIGHT + 4);
    d.printf("password for %s", g_pickedSsid.c_str());

    int boxY = ui::statusbar::HEIGHT + 24;
    d.fillRoundRect(4, boxY, SCREEN_W - 8, 20, 3, 0x2104);
    d.setCursor(8, boxY + 6);
    d.setTextColor(0xFFFF);
    std::string masked(g_password.size(), '*');
    if (masked.size() > 32) masked = masked.substr(masked.size() - 32);
    d.print(masked.c_str());
    if ((millis() / 500) % 2 == 0) d.print("_");

    if (!g_pickedSecure) {
        d.setCursor(6, boxY + 28);
        d.setTextColor(0x8C71);
        d.print("(open network — leave blank)");
    }

    drawFooter("enter connect   bksp delete   tab home");
    ui::flush();
}

void renderConnecting() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xFFE0);
    d.setCursor(6, 50);
    d.printf("connecting to %s…", g_pickedSsid.c_str());

    uint32_t elapsed = millis() - g_connectStart;
    int dots = (elapsed / 500) % 4;
    d.print(std::string(dots, '.').c_str());

    drawFooter("");
    ui::flush();
}

void renderConnected() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0x07E0);
    d.setCursor(6, ui::statusbar::HEIGHT + 4);
    d.print("connected");
    d.setCursor(6, ui::statusbar::HEIGHT + 22);
    d.setTextColor(0xFFFF);
    d.printf("%s", wifi::ssid().c_str());
    d.setCursor(6, ui::statusbar::HEIGHT + 36);
    d.setTextColor(0x8C71);
    d.print("ip:");
    d.setCursor(34, ui::statusbar::HEIGHT + 36);
    d.setTextColor(0xFFFF);
    d.print(wifi::ip().c_str());

    drawFooter("tab home");
    ui::flush();
}

void renderFailed() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    d.setTextColor(0xF800);
    d.setCursor(6, ui::statusbar::HEIGHT + 4);
    d.print("connect failed");
    d.setCursor(6, ui::statusbar::HEIGHT + 22);
    d.setTextColor(0xFFFF);
    d.print(g_status.c_str());

    drawFooter("enter retry password   tab home");
    ui::flush();
}

void render() {
    switch (g_stage) {
        case Stage::Idle:       renderIdle();       break;
        case Stage::Scanning:   renderScanning();   break;
        case Stage::List:       renderList();       break;
        case Stage::Password:   renderPassword();   break;
        case Stage::Connecting: renderConnecting(); break;
        case Stage::Connected:  renderConnected();  break;
        case Stage::Failed:     renderFailed();     break;
    }
}

void startScan() {
    g_stage = Stage::Scanning;
    g_selected = 0;
    g_scrollTop = 0;
    g_status.clear();
    wifi::startScan();
    g_dirty = true;
}

void onEnter() {
    g_stage = wifi::isConnected() ? Stage::Connected : Stage::Idle;
    g_dirty = true;
}
void onExit() {}

void onTick() {
    g_dirty = true;  // smooth blinking + animation

    if (g_stage == Stage::Scanning) {
        int rc = wifi::scanStatus();
        if (rc == -1) return;            // still running
        if (rc < 0) {                    // -2 = failed
            g_status = "scan failed";
            g_stage  = Stage::Failed;
            return;
        }
        g_stage = Stage::List;
        return;
    }

    if (g_stage == Stage::Connecting) {
        if (wifi::isConnected()) {
            g_stage = Stage::Connected;
            return;
        }
        if (millis() - g_connectStart > CONNECT_TIMEOUT_MS) {
            g_status = "timed out (wrong password?)";
            g_stage  = Stage::Failed;
            return;
        }
    }
}

void onKey(char ch) {
    switch (g_stage) {
        case Stage::Idle:
            if (ch == '\n') startScan();
            break;

        case Stage::List: {
            int count = wifi::scanStatus();
            if (count <= 0) {
                if (ch == '\n') startScan();
                break;
            }
            if (ch == key::Up) {
                g_selected = (g_selected - 1 + count) % count;
            } else if (ch == key::Down) {
                g_selected = (g_selected + 1) % count;
            } else if (ch == '\n') {
                auto r = wifi::scanNetwork(g_selected);
                g_pickedSsid   = r.ssid;
                g_pickedSecure = r.secure;
                g_password.clear();
                g_stage = Stage::Password;
            }
            break;
        }

        case Stage::Password:
            if (ch == '\n') {
                g_connectStart = millis();
                wifi::connectNow(g_pickedSsid, g_password);
                g_stage = Stage::Connecting;
            } else if (ch == '\b') {
                if (!g_password.empty()) g_password.pop_back();
            } else if (ch >= 0x20 && ch <= 0x7E) {
                if (g_password.size() < 63) g_password.push_back(ch);
            }
            break;

        case Stage::Failed:
            if (ch == '\n') {
                g_stage = Stage::Password;
            }
            break;

        case Stage::Connected:
            if (ch == '\n') startScan();
            break;

        default: break;
    }
    g_dirty = true;
}

void onDraw() { if (g_dirty) { render(); g_dirty = false; } }

App wifi_app = {
    .id           = "wifi",
    .name         = "WiFi",
    .description  = "Scan & connect",
    .services     = SVC_WIFI,
    .onEnter      = onEnter,
    .onExit       = onExit,
    .onTick       = onTick,
    .onKey        = onKey,
    .onDraw       = onDraw,
    .onEvent      = nullptr,
    .keysAsArrows = false,  // password field accepts ;./,/ as text
};

}  // namespace

REGISTER_APP(wifi_app);
