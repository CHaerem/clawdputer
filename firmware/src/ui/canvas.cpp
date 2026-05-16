#include "canvas.h"

#include <Arduino.h>

namespace ui {

namespace {
// Allocated/freed dynamically by tryAcquireCanvas/releaseCanvas. Static
// instance so we always have somewhere to call createSprite() on; the
// underlying buffer is what consumes/releases heap.
M5Canvas g_canvas(&M5Cardputer.Display);
bool     g_active = false;

#ifdef CLAWD_PROFILE_CANVAS
uint32_t g_frames = 0;
uint32_t g_pushUs = 0;
uint32_t g_lastLog = 0;
#endif
}  // namespace

LovyanGFX& display() {
    return g_active ? static_cast<LovyanGFX&>(g_canvas)
                    : static_cast<LovyanGFX&>(M5Cardputer.Display);
}

void beginFrame() {
    display().fillScreen(BLACK);
}

void flush() {
    if (!g_active) return;
#ifdef CLAWD_PROFILE_CANVAS
    uint32_t t0 = micros();
#endif
    auto& dst = M5Cardputer.Display;
    dst.startWrite();
    g_canvas.pushSprite(&dst, 0, 0);
    dst.endWrite();
#ifdef CLAWD_PROFILE_CANVAS
    g_pushUs += (micros() - t0);
    g_frames++;
    uint32_t now = millis();
    if (now - g_lastLog >= 2000) {
        uint32_t avgUs = g_frames ? g_pushUs / g_frames : 0;
        Serial.printf("[ui] %u frames in %ums, avg push %uus (~%u fps cap)\n",
                      (unsigned)g_frames, (unsigned)(now - g_lastLog),
                      (unsigned)avgUs,
                      (unsigned)(avgUs ? 1000000 / avgUs : 0));
        g_frames = 0; g_pushUs = 0; g_lastLog = now;
    }
#endif
}

bool tryAcquireCanvas() {
    if (g_active) return true;
    g_canvas.setColorDepth(8);
    if (!g_canvas.createSprite(SCREEN_W, SCREEN_H)) {
        Serial.printf("[ui] canvas alloc failed (free heap %u) — direct draw\n",
                      (unsigned)ESP.getFreeHeap());
        return false;
    }
    g_active = true;
    Serial.printf("[ui] canvas acquired (free heap %u)\n",
                  (unsigned)ESP.getFreeHeap());
    return true;
}

void releaseCanvas() {
    if (!g_active) return;
    g_canvas.deleteSprite();
    g_active = false;
    Serial.printf("[ui] canvas released (free heap %u)\n",
                  (unsigned)ESP.getFreeHeap());
}

bool canvasActive() { return g_active; }

}  // namespace ui
