#include "power.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp32-hal-cpu.h>

namespace power {

namespace {

// Display backlight dims after a minute and turns off after three.
// The TFT panel itself stays alive while the BL is off; switching the
// panel into sleep cuts another ~10-15 mA on top.
constexpr uint32_t ACTIVE_WINDOW_MS = 5000;
constexpr uint32_t DIM_AFTER_MS     = 60000;
constexpr uint32_t OFF_AFTER_MS     = 180000;
constexpr uint32_t PANEL_SLEEP_MS   = 600000;   // 10 min — full panel off

// Adaptive loop pacing: keeps the main loop responsive when the user is
// active, but stretches the delay otherwise so the SoC enters
// auto-light-sleep between iterations. The light-sleep is set up via
// esp_pm_configure() in begin() — it cooperates with BLE/WiFi.
constexpr uint16_t DELAY_ACTIVE_MS = 20;    // ~50 Hz
constexpr uint16_t DELAY_IDLE_MS   = 60;    // ~16 Hz, still feels smooth
constexpr uint16_t DELAY_DEEP_MS   = 150;   // anything beyond 1 min idle

uint32_t g_lastActivity = 0;
uint8_t  g_backlight    = 255;
bool     g_panelAsleep  = false;

void applyBacklight(uint8_t v) {
    if (v == g_backlight) return;
    M5Cardputer.Display.setBrightness(v);
    g_backlight = v;
}

void setPanel(bool sleep) {
    if (sleep == g_panelAsleep) return;
    if (sleep) M5Cardputer.Display.sleep();
    else        M5Cardputer.Display.wakeup();
    g_panelAsleep = sleep;
}

}  // namespace

void begin() {
    g_lastActivity = millis();
    applyBacklight(255);
    setPanel(false);

    // Auto light-sleep when the kernel has nothing to do. Min freq is 80
    // because the radio stacks need APB clock above 40 MHz to be stable;
    // 80 MHz is the conservative safe floor on ESP32-S3.
    esp_pm_config_esp32s3_t cfg = {
        .max_freq_mhz       = 240,
        .min_freq_mhz       = 80,
        .light_sleep_enable = true,
    };
    esp_err_t rc = esp_pm_configure(&cfg);
    Serial.printf("[power] esp_pm_configure rc=%d (auto light-sleep %s)\n",
                  rc, rc == ESP_OK ? "on" : "FAILED");
}

void noteActivity() {
    g_lastActivity = millis();
    applyBacklight(255);
    setPanel(false);
}

void tick() {
    uint32_t now    = millis();
    uint32_t idleMs = now - g_lastActivity;

    if (idleMs >= PANEL_SLEEP_MS) {
        setPanel(true);
        applyBacklight(0);
    } else if (idleMs >= OFF_AFTER_MS) {
        applyBacklight(0);
    } else if (idleMs >= DIM_AFTER_MS) {
        applyBacklight(64);
    } else {
        applyBacklight(255);
    }
}

uint8_t  backlightLevel() { return g_backlight; }
uint32_t idleMs()         { return millis() - g_lastActivity; }

uint16_t loopDelayMs() {
    uint32_t idle = idleMs();
    if (idle < ACTIVE_WINDOW_MS) return DELAY_ACTIVE_MS;
    if (idle < DIM_AFTER_MS)     return DELAY_IDLE_MS;
    return DELAY_DEEP_MS;
}

}  // namespace power
