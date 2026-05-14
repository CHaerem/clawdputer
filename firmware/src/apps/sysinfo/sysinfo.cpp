// Sysinfo app — live readout of battery, memory, network, and BLE state.

#include <Arduino.h>
#include <M5Cardputer.h>

#include "core/app.h"
#include "services/ble.h"
#include "services/updater.h"
#include "services/wifi.h"

namespace {

bool     g_dirty   = true;
uint32_t g_lastTick = 0;

void formatUptime(char* buf, size_t n, uint32_t secs) {
    uint32_t h = secs / 3600;
    uint32_t m = (secs / 60) % 60;
    uint32_t s = secs % 60;
    if (h > 0) snprintf(buf, n, "%uh %02um %02us", (unsigned)h, (unsigned)m, (unsigned)s);
    else if (m > 0) snprintf(buf, n, "%um %02us", (unsigned)m, (unsigned)s);
    else snprintf(buf, n, "%us", (unsigned)s);
}

void drawRow(int& y, const char* label, const char* value, uint16_t valColor = 0xFFFF) {
    auto& d = M5Cardputer.Display;
    d.setCursor(8, y);
    d.setTextColor(0x7BEF);
    d.print(label);
    d.setCursor(110, y);
    d.setTextColor(valColor);
    d.print(value);
    y += 14;
}

void render() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);

    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(6, 4);
    d.print("sysinfo");

    d.setTextSize(1);
    int y = 32;

    int  bat       = M5Cardputer.Power.getBatteryLevel();
    int  batV_mV   = M5Cardputer.Power.getBatteryVoltage();
    bool charging  = M5Cardputer.Power.isCharging();

    char buf[64];
    uint16_t batColor = bat < 20 ? 0xF800 : (bat < 50 ? 0xFFE0 : 0x07E0);
    snprintf(buf, sizeof(buf), "%d%% %s (%.2fV)",
             bat, charging ? "charging" : "discharging", batV_mV / 1000.0f);
    drawRow(y, "battery",  buf, batColor);

    formatUptime(buf, sizeof(buf), (millis() / 1000));
    drawRow(y, "uptime",   buf);

    snprintf(buf, sizeof(buf), "%u KB free", (unsigned)(ESP.getFreeHeap() / 1024));
    drawRow(y, "heap",     buf);

    snprintf(buf, sizeof(buf), "%s",
             ble::isConnected(EventSource::NusLink) ? "connected" : "—");
    drawRow(y, "buddy",    buf,
            ble::isConnected(EventSource::NusLink) ? 0x07E0 : 0x4208);

    snprintf(buf, sizeof(buf), "%s",
             ble::isConnected(EventSource::BridgeLink) ? "connected" : "—");
    drawRow(y, "bridge",   buf,
            ble::isConnected(EventSource::BridgeLink) ? 0x07E0 : 0x4208);

    if (wifi::isConnected()) {
        drawRow(y, "wifi", wifi::ip().c_str(), 0x07E0);
    } else if (!wifi::ssid().empty()) {
        drawRow(y, "wifi", "connecting…", 0xFFE0);
    } else {
        drawRow(y, "wifi", "not configured", 0x4208);
    }

    drawRow(y, "device", ble::deviceName().c_str());

    snprintf(buf, sizeof(buf), "%s (latest: %s)",
             updater::currentVersion(),
             updater::latestVersion().empty() ? "?" : updater::latestVersion().c_str());
    drawRow(y, "build", buf);

    d.fillRect(0, 224, 320, 16, 0x2104);
    d.setTextColor(0x7BEF);
    d.setCursor(6, 228);
    d.print("[TAB] home");
}

void onEnter() {
    g_dirty   = true;
    g_lastTick = 0;
}

void onExit() {}

void onTick() {
    uint32_t now = millis();
    if (now - g_lastTick >= 1000) {
        g_lastTick = now;
        g_dirty    = true;
    }
}

void onKey(char) {}

void onDraw() {
    if (!g_dirty) return;
    render();
    g_dirty = false;
}

App sysinfo_app = {
    .id          = "sysinfo",
    .name        = "Sysinfo",
    .description = "Battery, memory, network",
    .services    = SVC_NONE,
    .onEnter     = onEnter,
    .onExit      = onExit,
    .onTick      = onTick,
    .onKey       = onKey,
    .onDraw      = onDraw,
    .onEvent     = nullptr,
};

}  // namespace

REGISTER_APP(sysinfo_app);
