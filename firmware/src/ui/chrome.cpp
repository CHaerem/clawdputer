#include "chrome.h"

#include <M5Cardputer.h>
#include <string.h>

#include "canvas.h"
#include "statusbar.h"

namespace ui {

void footer(const char* hint) {
    auto& d = display();
    d.fillRect(0, FOOTER_TOP, SCREEN_W, FOOTER_H, 0x1082);
    d.drawFastHLine(0, FOOTER_TOP, SCREEN_W, 0x2945);
    d.setTextSize(1);
    d.setTextColor(0x8C71);
    d.setCursor(4, FOOTER_TOP + 3);
    d.print(hint);
}

void title(const char* text, const char* sub) {
    auto& d = display();
    d.setTextSize(1);
    d.setTextColor(0xFFFF);
    d.setCursor(6, statusbar::HEIGHT + 2);
    d.print(text);
    if (sub) {
        int subX = SCREEN_W - (int)strlen(sub) * 6 - 6;
        d.setCursor(subX, statusbar::HEIGHT + 2);
        d.setTextColor(0x8C71);
        d.print(sub);
    }
}

}  // namespace ui
