#include "ota.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <M5Cardputer.h>

#include "wifi.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

namespace {

bool     g_started   = false;
bool     g_updating  = false;
unsigned g_lastShown = 0;

void drawProgress(unsigned pct) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextColor(WHITE);
    d.setTextSize(2);
    d.setCursor(8, 8);
    d.print("OTA UPDATE");
    d.setTextSize(1);
    d.setCursor(8, 36);
    d.printf("flashing %u%%", pct);
    int barX = 8, barY = 56, barW = 224, barH = 10;
    d.drawRect(barX, barY, barW, barH, WHITE);
    int fill = (int)((barW - 2) * pct / 100);
    d.fillRect(barX + 1, barY + 1, fill, barH - 2, 0x07E0);
    d.setCursor(8, 88);
    d.setTextColor(0x7BEF);
    d.print("do not unplug");
}

void onStart() {
    g_updating  = true;
    g_lastShown = 100;  // force first redraw
    Serial.println("[ota] update start");
    drawProgress(0);
}

void onEnd() {
    Serial.println("[ota] update complete, rebooting");
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    d.setTextColor(0x07E0);
    d.setCursor(8, 50);
    d.print("OTA OK");
    d.setTextSize(1);
    d.setTextColor(WHITE);
    d.setCursor(8, 84);
    d.print("rebooting…");
}

void onProgress(unsigned progress, unsigned total) {
    if (total == 0) return;
    unsigned pct = (progress * 100u) / total;
    if (pct == g_lastShown) return;
    g_lastShown = pct;
    drawProgress(pct);
    Serial.printf("[ota] %u%%\n", pct);
}

void onError(ota_error_t err) {
    g_updating = false;
    Serial.printf("[ota] error %u\n", (unsigned)err);
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    d.setTextColor(0xF800);
    d.setCursor(8, 50);
    d.print("OTA FAIL");
    d.setTextSize(1);
    d.setTextColor(WHITE);
    d.setCursor(8, 84);
    d.printf("error %u — reboot to retry", (unsigned)err);
}

void startListener() {
    ArduinoOTA.setHostname("clawdputer");
#ifdef CLAWD_OTA_PASSWORD
    ArduinoOTA.setPassword(CLAWD_OTA_PASSWORD);
#endif
    ArduinoOTA.onStart(onStart);
    ArduinoOTA.onEnd(onEnd);
    ArduinoOTA.onProgress(onProgress);
    ArduinoOTA.onError(onError);
    ArduinoOTA.begin();
    Serial.printf("[ota] listener ready at clawdputer.local (%s)\n",
                  wifi::ip().c_str());
}

}  // namespace

namespace ota {

void tick() {
    if (!wifi::isConnected()) return;
    if (!g_started) {
        startListener();
        g_started = true;
    }
    ArduinoOTA.handle();
}

bool isUpdating() { return g_updating; }

}  // namespace ota
