#include "toast.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "canvas.h"

namespace ui::toast {

namespace {
std::string g_msg;
uint32_t    g_until = 0;
}

void show(const std::string& msg, uint32_t durationMs) {
    g_msg   = msg;
    g_until = millis() + durationMs;
}

bool active() { return millis() < g_until && !g_msg.empty(); }

void draw() {
    if (!active()) return;
    auto& d = display();
    int y = SCREEN_H - 28;
    d.fillRoundRect(8, y, SCREEN_W - 16, 18, 4, 0x4800);
    d.drawRoundRect(8, y, SCREEN_W - 16, 18, 4, 0xFC60);
    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(14, y + 6);
    d.print(g_msg.c_str());
}

}  // namespace ui::toast
