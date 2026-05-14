#include "statusbar.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "canvas.h"
#include "services/ble.h"
#include "services/wifi.h"

namespace ui::statusbar {

namespace {

void drawBatteryIcon(int x, int y, int pct, bool charging) {
    auto& d   = ui::display();
    int   bw  = 16;
    int   bh  = 8;
    uint16_t color = pct < 20 ? 0xF800 : (pct < 50 ? 0xFFE0 : 0x07E0);

    d.drawRect(x, y, bw, bh, 0xC618);
    d.fillRect(x + bw, y + 2, 2, bh - 4, 0xC618);
    int fillW = (bw - 2) * pct / 100;
    if (fillW > 0) d.fillRect(x + 1, y + 1, fillW, bh - 2, color);

    if (charging) {
        int cx = x + bw / 2;
        int cy = y + bh / 2;
        d.drawLine(cx - 1, cy - 2, cx + 1, cy,     WHITE);
        d.drawLine(cx + 1, cy,     cx - 1, cy + 2, WHITE);
    }
}

void drawLinkDot(int x, int y, bool on) {
    uint16_t c = on ? 0x07E0 : 0x4208;
    ui::display().fillCircle(x, y, 2, c);
}

}  // namespace

void draw() {
    auto& d = ui::display();

    d.fillRect(0, 0, SCREEN_W, HEIGHT, 0x1082);
    d.drawFastHLine(0, HEIGHT - 1, SCREEN_W, 0x2945);

    d.setTextSize(1);
    int y = 3;

    // BD/BR/WiFi indicators on the left
    d.setTextColor(0x8C71);
    d.setCursor(4, y);
    d.print("bd");
    drawLinkDot(20, y + 3, ble::isConnected(EventSource::NusLink));

    d.setCursor(28, y);
    d.setTextColor(0x8C71);
    d.print("br");
    drawLinkDot(44, y + 3, ble::isConnected(EventSource::BridgeLink));

    d.setCursor(52, y);
    d.setTextColor(0x8C71);
    d.print("wf");
    drawLinkDot(68, y + 3, wifi::isConnected());

    // Uptime in the centre-right
    char buf[16];
    uint32_t s = millis() / 1000;
    uint32_t h = s / 3600;
    uint32_t m = (s / 60) % 60;
    if (h > 0) snprintf(buf, sizeof(buf), "%uh%02um", (unsigned)h, (unsigned)m);
    else      snprintf(buf, sizeof(buf), "%um%02us", (unsigned)m, (unsigned)(s % 60));
    int textW = strlen(buf) * 6;
    d.setCursor(SCREEN_W - 28 - textW, y);
    d.setTextColor(0x8C71);
    d.print(buf);

    // Battery right-most
    int  pct      = M5Cardputer.Power.getBatteryLevel();
    bool charging = M5Cardputer.Power.isCharging();
    drawBatteryIcon(SCREEN_W - 22, y - 1, pct, charging);
}

}  // namespace ui::statusbar
