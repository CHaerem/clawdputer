#include "canvas.h"

#include <Arduino.h>

namespace ui {

namespace {
M5Canvas g_canvas(&M5Cardputer.Display);
bool     g_ready    = false;
bool     g_tried    = false;
}  // namespace

LovyanGFX& display() {
    if (!g_tried) {
        g_tried = true;
        // 8 bpp (RGB332) keeps the backbuffer to ~32 KB so it fits
        // alongside NimBLE + WiFi heap usage. 16 bpp would be 65 KB and
        // leave esp_wifi_init with too little room for its RX buffers.
        g_canvas.setColorDepth(8);
        if (g_canvas.createSprite(SCREEN_W, SCREEN_H)) {
            g_ready = true;
            Serial.printf("[ui] canvas ready (free heap %u)\n",
                          (unsigned)ESP.getFreeHeap());
        } else {
            Serial.printf("[ui] canvas alloc FAILED — falling back to direct draw (free heap %u)\n",
                          (unsigned)ESP.getFreeHeap());
        }
    }
    if (g_ready) return g_canvas;
    return M5Cardputer.Display;
}

void beginFrame() {
    display().fillScreen(BLACK);
}

void flush() {
    if (g_ready) g_canvas.pushSprite(0, 0);
}

}  // namespace ui
