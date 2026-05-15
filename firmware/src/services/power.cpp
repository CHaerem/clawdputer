#include "power.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <driver/rtc_io.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp32-hal-cpu.h>

#include "wifi.h"

namespace power {

namespace {

constexpr uint32_t ACTIVE_WINDOW_MS = 5000;
constexpr uint32_t DIM_AFTER_MS     = 60000;
constexpr uint32_t OFF_AFTER_MS     = 180000;
constexpr uint32_t PANEL_SLEEP_MS   = 600000;   // 10 min
constexpr uint32_t WIFI_PAUSE_MS    = 300000;   // 5 min
constexpr uint32_t DEEP_SLEEP_MS    = 3600000;  // 60 min — auto deep-sleep

constexpr uint16_t DELAY_ACTIVE_MS = 20;
constexpr uint16_t DELAY_IDLE_MS   = 60;
constexpr uint16_t DELAY_DEEP_MS   = 150;

constexpr int BATTERY_CRITICAL_PCT       = 5;
constexpr uint32_t BATTERY_CONFIRM_MS    = 10000;   // sustained for 10 s
constexpr int LOW_BATT_COUNTDOWN_SECONDS = 30;

// G0 side-button on ESP32-S3 is GPIO0. ext0 wakeup wakes the chip out
// of deep sleep when this pin goes LOW (active-low button).
constexpr gpio_num_t WAKEUP_GPIO = GPIO_NUM_0;

uint32_t g_lastActivity     = 0;
uint8_t  g_backlight        = 255;
bool     g_panelAsleep      = false;
bool     g_wokeFromSleep    = false;
uint32_t g_lowBatStartMs    = 0;     // when did we first see low batt
uint32_t g_warningStartMs   = 0;     // when did countdown begin (0 = inactive)

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

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    g_wokeFromSleep = (cause == ESP_SLEEP_WAKEUP_EXT0 ||
                       cause == ESP_SLEEP_WAKEUP_GPIO);
    if (g_wokeFromSleep) {
        Serial.printf("[power] woke from deep sleep (cause=%d)\n", (int)cause);
    }

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
    // Any keypress cancels a pending low-battery countdown.
    g_warningStartMs = 0;
}

bool wokeFromDeepSleep() { return g_wokeFromSleep; }

bool lowBatteryWarning() { return g_warningStartMs != 0; }

int lowBatterySecondsLeft() {
    if (g_warningStartMs == 0) return -1;
    int32_t elapsed = (int32_t)(millis() - g_warningStartMs) / 1000;
    int remaining   = LOW_BATT_COUNTDOWN_SECONDS - elapsed;
    return remaining < 0 ? 0 : remaining;
}

void cancelLowBatteryWarning() { g_warningStartMs = 0; }

[[noreturn]] void deepSleep() {
    Serial.println("[power] entering deep sleep — wake on G0 press");
    Serial.flush();

    // Tear down high-current peripherals so we don't draw current during
    // the brief shutdown window.
    wifi::pause();
    NimBLEDevice::deinit(true);
    M5Cardputer.Display.setBrightness(0);
    M5Cardputer.Display.sleep();

    // Configure G0 as RTC GPIO so it survives the sleep state machine.
    rtc_gpio_init(WAKEUP_GPIO);
    rtc_gpio_set_direction(WAKEUP_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(WAKEUP_GPIO);
    rtc_gpio_pulldown_dis(WAKEUP_GPIO);
    esp_sleep_enable_ext0_wakeup(WAKEUP_GPIO, 0);   // wake on LOW

    esp_deep_sleep_start();
    // unreachable
    while (true) {}
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

    if (idleMs >= WIFI_PAUSE_MS && !wifi::isPaused()) {
        wifi::pause();
    }

    // Battery monitoring — sustained low → start countdown → deep sleep.
    int pct = M5Cardputer.Power.getBatteryLevel();
    bool charging = M5Cardputer.Power.isCharging();
    if (!charging && pct >= 0 && pct <= BATTERY_CRITICAL_PCT) {
        if (g_lowBatStartMs == 0) g_lowBatStartMs = now;
        if (g_warningStartMs == 0 &&
            now - g_lowBatStartMs >= BATTERY_CONFIRM_MS) {
            g_warningStartMs = now;
            Serial.printf("[power] battery %d%% — starting deep-sleep countdown\n", pct);
        }
    } else {
        g_lowBatStartMs  = 0;
        g_warningStartMs = 0;
    }

    if (g_warningStartMs != 0 &&
        now - g_warningStartMs >= (uint32_t)LOW_BATT_COUNTDOWN_SECONDS * 1000) {
        deepSleep();
    }

    if (idleMs >= DEEP_SLEEP_MS) {
        Serial.println("[power] 60 min idle — auto deep-sleep");
        deepSleep();
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
