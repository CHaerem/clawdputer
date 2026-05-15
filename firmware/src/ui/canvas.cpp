#include "canvas.h"

#include <Arduino.h>

namespace ui {

namespace {
// Allocated/freed dynamically by tryAcquireCanvas/releaseCanvas. Static
// instance so we always have somewhere to call createSprite() on; the
// underlying buffer is what consumes/releases heap.
M5Canvas g_canvas(&M5Cardputer.Display);
bool     g_active = false;
}  // namespace

LovyanGFX& display() {
    return g_active ? static_cast<LovyanGFX&>(g_canvas)
                    : static_cast<LovyanGFX&>(M5Cardputer.Display);
}

void beginFrame() {
    display().fillScreen(BLACK);
}

void flush() {
    if (g_active) g_canvas.pushSprite(0, 0);
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
