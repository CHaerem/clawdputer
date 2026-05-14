#include "power.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <esp32-hal-cpu.h>

namespace power {

namespace {

constexpr uint32_t ACTIVE_WINDOW_MS = 5000;   // full clock for 5 s after input
constexpr uint32_t DIM_AFTER_MS     = 60000;  // 1 min → half brightness
constexpr uint32_t OFF_AFTER_MS     = 180000; // 3 min → backlight off

constexpr uint32_t ACTIVE_CPU_MHZ = 240;
constexpr uint32_t IDLE_CPU_MHZ   = 80;

uint32_t g_lastActivity = 0;
uint32_t g_cpuMhz       = 240;
uint8_t  g_backlight    = 255;

[[maybe_unused]] void applyClock(uint32_t mhz) {
    if (mhz == g_cpuMhz) return;
    setCpuFrequencyMhz(mhz);
    g_cpuMhz = mhz;
}

void applyBacklight(uint8_t v) {
    if (v == g_backlight) return;
    M5Cardputer.Display.setBrightness(v);
    g_backlight = v;
}

}  // namespace

void begin() {
    g_lastActivity = millis();
    applyBacklight(255);
}

void noteActivity() {
    g_lastActivity = millis();
    applyBacklight(255);
}

void tick() {
    uint32_t now      = millis();
    uint32_t idleMs   = now - g_lastActivity;

    // CPU-clock scaling is disabled for now — changing the clock while
    // WiFi/BLE are active is a known ESP32 gotcha that can crash the radio
    // stack. Display dimming is the safe part of this service.
    if (idleMs >= OFF_AFTER_MS) {
        applyBacklight(0);
    } else if (idleMs >= DIM_AFTER_MS) {
        applyBacklight(64);
    } else {
        applyBacklight(255);
    }
    (void)applyClock;  // kept defined so it's easy to re-enable later
}

uint8_t backlightLevel() { return g_backlight; }

}  // namespace power
