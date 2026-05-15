// Battery — live readout + 1-hour rolling log so you can see how each
// power optimisation actually affects drain. Every 60 s we record
// percent / voltage / charging into a ring buffer (60 entries) and
// also print a CSV-shaped line on Serial for easy off-device plotting.

#include <Arduino.h>
#include <M5Cardputer.h>

#include "core/app.h"
#include "services/power.h"
#include "services/wifi.h"
#include "ui/canvas.h"
#include "ui/statusbar.h"

namespace {

constexpr int    SAMPLES = 60;        // 1-hour window at 60 s/sample
constexpr uint32_t SAMPLE_INTERVAL_MS = 60000;

struct Sample {
    uint8_t  pct;        // 0..100
    uint16_t mV;
    bool     charging;
};

Sample   g_ring[SAMPLES] = {};
int      g_count         = 0;        // valid entries
int      g_head          = 0;        // next slot to write
uint32_t g_lastSample    = 0;
bool     g_dirty         = true;

void recordSample() {
    Sample s;
    s.pct      = (uint8_t)M5Cardputer.Power.getBatteryLevel();
    s.mV       = (uint16_t)M5Cardputer.Power.getBatteryVoltage();
    s.charging = M5Cardputer.Power.isCharging();
    g_ring[g_head] = s;
    g_head         = (g_head + 1) % SAMPLES;
    if (g_count < SAMPLES) g_count++;
    // CSV line for tools/battery-plot.py — easy to grep + parse from
    // a running `pio device monitor` capture.
    Serial.printf("[batterylog] t=%u pct=%u mV=%u chg=%d\n",
                  (unsigned)(millis() / 1000), (unsigned)s.pct,
                  (unsigned)s.mV, (int)s.charging);
}

// Mean of the most recent N samples — used for the "drain/hr" estimate.
float meanPct(int n) {
    if (g_count == 0) return 0.0f;
    if (n > g_count) n = g_count;
    int sum = 0;
    for (int i = 0; i < n; i++) {
        int idx = (g_head - 1 - i + SAMPLES) % SAMPLES;
        sum += g_ring[idx].pct;
    }
    return sum / (float)n;
}

// Compute % per hour drain based on the slope between the oldest valid
// sample and the newest. Negative when charging.
float drainPerHour() {
    if (g_count < 2) return 0.0f;
    int oldestIdx = (g_head - g_count + SAMPLES) % SAMPLES;
    int newestIdx = (g_head - 1 + SAMPLES) % SAMPLES;
    int   dp = (int)g_ring[newestIdx].pct - (int)g_ring[oldestIdx].pct;
    float dt = (g_count - 1) * (SAMPLE_INTERVAL_MS / 1000.0f) / 3600.0f;
    if (dt <= 0) return 0.0f;
    return -dp / dt;   // positive = discharging
}

void drawGraph(int x, int y, int w, int h) {
    auto& d = ui::display();
    d.drawRect(x, y, w, h, 0x4208);
    if (g_count < 2) return;
    int oldestIdx = (g_head - g_count + SAMPLES) % SAMPLES;
    int prevX = -1, prevY = -1;
    for (int i = 0; i < g_count; i++) {
        int idx = (oldestIdx + i) % SAMPLES;
        int px  = x + (i * (w - 2)) / (SAMPLES - 1) + 1;
        int py  = y + h - 1 - (g_ring[idx].pct * (h - 2)) / 100;
        if (prevX >= 0) d.drawLine(prevX, prevY, px, py, 0x07E0);
        prevX = px;
        prevY = py;
    }
}

void render() {
    auto& d = ui::display();
    ui::beginFrame();
    ui::statusbar::draw();

    int y = ui::statusbar::HEIGHT + 4;
    d.setTextSize(1);

    int  pct      = M5Cardputer.Power.getBatteryLevel();
    int  mV       = M5Cardputer.Power.getBatteryVoltage();
    bool charging = M5Cardputer.Power.isCharging();

    char buf[64];
    snprintf(buf, sizeof(buf), "%d%% %s %.2fV",
             pct, charging ? "chg" : "dis", mV / 1000.0f);
    d.setCursor(6, y);
    d.setTextColor(pct < 20 ? 0xF800 : (pct < 50 ? 0xFFE0 : 0x07E0));
    d.print(buf);

    snprintf(buf, sizeof(buf), "%.1f %%/hr", drainPerHour());
    d.setCursor(120, y);
    d.setTextColor(0xC618);
    d.print(buf);

    snprintf(buf, sizeof(buf), "wifi: %s",
             wifi::isPaused() ? "off" : (wifi::isConnected() ? "on" : "—"));
    d.setCursor(6, y + 12);
    d.setTextColor(0x8C71);
    d.print(buf);

    snprintf(buf, sizeof(buf), "idle: %us", (unsigned)(power::idleMs() / 1000));
    d.setCursor(120, y + 12);
    d.setTextColor(0x8C71);
    d.print(buf);

    // 1-hour rolling graph (each pixel column = ~1 minute)
    int graphY = y + 28;
    drawGraph(6, graphY, SCREEN_W - 12, 60);

    d.setCursor(6, graphY + 64);
    d.setTextColor(0x8C71);
    snprintf(buf, sizeof(buf), "%d samples — newest right, 1h window", g_count);
    d.print(buf);

    // Footer
    d.fillRect(0, 124, SCREEN_W, 11, 0x1082);
    d.drawFastHLine(0, 124, SCREEN_W, 0x2945);
    d.setTextColor(0x8C71);
    d.setCursor(4, 127);
    d.print("logs every 60s · tab: home");

    ui::flush();
}

void onEnter() { g_dirty = true; }
void onExit()  {}

void onTick() {
    uint32_t now = millis();
    if (g_lastSample == 0 || now - g_lastSample >= SAMPLE_INTERVAL_MS) {
        g_lastSample = now;
        recordSample();
        g_dirty = true;
    }
    // Repaint once a second so live readouts stay fresh.
    static uint32_t lastFrame = 0;
    if (now - lastFrame >= 1000) { lastFrame = now; g_dirty = true; }
}

void onKey(char) {}

void onDraw() { if (g_dirty) { render(); g_dirty = false; } }

App battery_app = {
    .id          = "battery",
    .name        = "Battery",
    .description = "Live + 1h graph",
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

REGISTER_APP(battery_app);
