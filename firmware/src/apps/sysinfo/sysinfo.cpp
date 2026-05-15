// Sysinfo — full readout on the 240x135 display. Statusbar covers the
// at-a-glance summary; this app digs deeper.

#include <Arduino.h>
#include <M5Cardputer.h>

#include "core/app.h"
#include "services/ble.h"
#include "services/bridge.h"
#include "services/updater.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

bool     g_dirty    = true;
uint32_t g_lastTick = 0;

void formatUptime(char* buf, size_t n, uint32_t secs) {
    uint32_t h = secs / 3600;
    uint32_t m = (secs / 60) % 60;
    uint32_t s = secs % 60;
    if (h > 0) snprintf(buf, n, "%uh%02um%02us", (unsigned)h, (unsigned)m, (unsigned)s);
    else if (m > 0) snprintf(buf, n, "%um%02us", (unsigned)m, (unsigned)s);
    else snprintf(buf, n, "%us", (unsigned)s);
}

void drawRow(int& y, const char* label, const char* value, uint16_t valColor = 0xFFFF) {
    auto& d = ui::display();
    d.setCursor(6, y);
    d.setTextColor(0x8C71);
    d.print(label);
    d.setCursor(72, y);
    d.setTextColor(valColor);
    d.print(value);
    y += 11;
}

void render() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    d.setTextSize(1);
    int y = ui::statusbar::HEIGHT + 4;

    char buf[64];

    int  bat      = M5Cardputer.Power.getBatteryLevel();
    int  batV     = M5Cardputer.Power.getBatteryVoltage();
    bool charging = M5Cardputer.Power.isCharging();
    uint16_t batColor = bat < 20 ? 0xF800 : (bat < 50 ? 0xFFE0 : 0x07E0);
    snprintf(buf, sizeof(buf), "%d%% %s (%.2fV)",
             bat, charging ? "chg" : "dis", batV / 1000.0f);
    drawRow(y, "battery", buf, batColor);

    formatUptime(buf, sizeof(buf), millis() / 1000);
    drawRow(y, "uptime", buf);

    snprintf(buf, sizeof(buf), "%u KB free", (unsigned)(ESP.getFreeHeap() / 1024));
    drawRow(y, "heap", buf);

    drawRow(y, "buddy",
            ble::isConnected(EventSource::NusLink) ? "connected" : "—",
            ble::isConnected(EventSource::NusLink) ? 0x07E0 : 0x4208);

    // Show which transport the bridge is actually using right now — BLE in
    // the room, TCP when only WiFi reaches the host, or — when the daemon
    // isn't reachable at all.
    auto t = bridge::activeTransport();
    const char* transportLabel =
        (t == bridge::Transport::Ble) ? "BLE" :
        (t == bridge::Transport::Tcp) ? "TCP (WiFi)" :
                                        "—";
    uint16_t transportColor =
        (t == bridge::Transport::Ble) ? 0x07E0 :
        (t == bridge::Transport::Tcp) ? 0xFD20 :
                                        0x4208;
    drawRow(y, "bridge", transportLabel, transportColor);

    if (wifi::isConnected()) {
        drawRow(y, "wifi", wifi::ip().c_str(), 0x07E0);
    } else if (!wifi::ssid().empty()) {
        drawRow(y, "wifi", "connecting…", 0xFFE0);
    } else {
        drawRow(y, "wifi", "—", 0x4208);
    }

    snprintf(buf, sizeof(buf), "%s/%s",
             updater::currentVersion(),
             updater::latestVersion().empty() ? "?" : updater::latestVersion().c_str());
    drawRow(y, "build", buf);

    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.print("tab: home");

    ui::flush();
}

void onEnter() { g_dirty = true; g_lastTick = 0; }
void onExit()  {}

void onTick() {
    uint32_t now = millis();
    if (now - g_lastTick >= 1000) {
        g_lastTick = now;
        g_dirty    = true;
    }
}

void onKey(char) {}

void onDraw() { if (g_dirty) { render(); g_dirty = false; } }

App sysinfo_app = {
    .id          = "sysinfo",
    .name        = "Sysinfo",
    .description = "Battery & network",
    .services    = SVC_NONE,
    .onEnter     = onEnter,
    .onExit      = onExit,
    .onTick      = onTick,
    .onKey       = onKey,
    .onDraw      = onDraw,
    .onEvent     = nullptr,
    .hidden      = true,
};

}  // namespace

REGISTER_APP(sysinfo_app);
