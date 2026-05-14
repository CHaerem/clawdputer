#include "statusbar.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "services/ble.h"
#include "services/wifi.h"

namespace ui::statusbar {

namespace {

void drawBatteryIcon(int x, int y, int pct, bool charging) {
    auto& d   = M5Cardputer.Display;
    int   bw  = 22;   // body width
    int   bh  = 10;   // body height
    uint16_t color = pct < 20 ? 0xF800 : (pct < 50 ? 0xFFE0 : 0x07E0);

    d.drawRect(x, y, bw, bh, 0xC618);
    d.fillRect(x + bw, y + 2, 2, bh - 4, 0xC618);  // nipple
    int fillW = (bw - 2) * pct / 100;
    if (fillW > 0) d.fillRect(x + 1, y + 1, fillW, bh - 2, color);

    if (charging) {
        // small lightning glyph centred on the body
        int cx = x + bw / 2;
        int cy = y + bh / 2;
        d.drawLine(cx - 2, cy - 3, cx + 1, cy,     WHITE);
        d.drawLine(cx + 1, cy,     cx - 1, cy + 3, WHITE);
    }
}

void drawLinkDot(int x, int y, bool on) {
    uint16_t c = on ? 0x07E0 : 0x4208;
    M5Cardputer.Display.fillCircle(x, y, 3, c);
}

}  // namespace

void draw() {
    auto& d = M5Cardputer.Display;

    d.fillRect(0, 0, 320, HEIGHT, 0x1082);
    d.drawFastHLine(0, HEIGHT - 1, 320, 0x2945);

    d.setTextSize(1);
    d.setTextColor(0xC618);
    d.setCursor(4, 5);
    d.print("clawdputer");

    // BLE indicators: bd (buddy / NUS) and br (bridge)
    int x = 86;
    d.setCursor(x, 5);
    d.setTextColor(0x8C71);
    d.print("bd");
    drawLinkDot(x + 16, 9, ble::isConnected(EventSource::NusLink));
    x += 24;

    d.setCursor(x, 5);
    d.setTextColor(0x8C71);
    d.print("br");
    drawLinkDot(x + 16, 9, ble::isConnected(EventSource::BridgeLink));
    x += 24;

    // WiFi: green IP-ish dot or grey
    d.setCursor(x, 5);
    d.setTextColor(0x8C71);
    d.print("wifi");
    drawLinkDot(x + 28, 9, wifi::isConnected());

    // Uptime in the middle-right
    char buf[24];
    uint32_t s = millis() / 1000;
    uint32_t h = s / 3600;
    uint32_t m = (s / 60) % 60;
    if (h > 0) snprintf(buf, sizeof(buf), "%uh%02um", (unsigned)h, (unsigned)m);
    else      snprintf(buf, sizeof(buf), "%um%02us", (unsigned)m, (unsigned)(s % 60));
    int upX = 234;
    d.setCursor(upX, 5);
    d.setTextColor(0x8C71);
    d.print(buf);

    // Battery — right-most
    int  pct      = M5Cardputer.Power.getBatteryLevel();
    bool charging = M5Cardputer.Power.isCharging();
    drawBatteryIcon(290, 4, pct, charging);
}

}  // namespace ui::statusbar
