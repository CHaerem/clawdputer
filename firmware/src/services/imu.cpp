#include "imu.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <math.h>

#include "settings.h"

namespace imu {

namespace {

bool     g_available  = false;
bool     g_shakeFlag  = false;
uint32_t g_lastShake  = 0;

constexpr float    SHAKE_THRESHOLD_G = 2.0f;       // peak |a| - 1.0 (gravity) in g
constexpr uint32_t SHAKE_COOLDOWN_MS = 1500;

}  // namespace

void begin() {
    g_available = M5.Imu.isEnabled();
    Serial.printf("[imu] %s\n", g_available ? "ready" : "not detected (older Cardputer?)");
}

void tick() {
    if (!g_available)            return;
    if (!settings::shakeEnabled()) return;

    M5.Imu.update();
    auto data = M5.Imu.getImuData();
    float mag = sqrtf(data.accel.x * data.accel.x +
                      data.accel.y * data.accel.y +
                      data.accel.z * data.accel.z);
    float net = fabsf(mag - 1.0f);

    uint32_t now = millis();
    if (net > SHAKE_THRESHOLD_G && now - g_lastShake > SHAKE_COOLDOWN_MS) {
        g_lastShake = now;
        g_shakeFlag = true;
    }
}

bool consumeShake() {
    bool s     = g_shakeFlag;
    g_shakeFlag = false;
    return s;
}

}  // namespace imu
