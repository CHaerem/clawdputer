#include "list.h"

#include <M5Cardputer.h>

namespace ui::list {

void draw(State& s, int x, int y, int w, int h) {
    if (s.items.empty()) return;
    auto& d = display();

    int rows = h / s.rowH;
    if (rows < 1) rows = 1;

    if (s.selected < 0) s.selected = 0;
    if (s.selected >= (int)s.items.size()) s.selected = (int)s.items.size() - 1;
    if (s.selected < s.scrollTop) s.scrollTop = s.selected;
    if (s.selected >= s.scrollTop + rows) s.scrollTop = s.selected - rows + 1;
    if (s.scrollTop < 0) s.scrollTop = 0;

    for (int i = s.scrollTop; i < (int)s.items.size() && i < s.scrollTop + rows; i++) {
        int  rowY = y + (i - s.scrollTop) * s.rowH;
        bool sel  = i == s.selected;
        if (sel) d.fillRoundRect(x, rowY - 1, w, s.rowH, 2, 0x18E3);

        d.setTextSize(1);
        d.setCursor(x + 4, rowY + 1);
        d.setTextColor(sel ? 0xFFE0 : 0x8C71);
        d.print(s.items[i].label.c_str());

        d.setCursor(x + w / 2 + 4, rowY + 1);
        if (s.items[i].action) {
            d.setTextColor(sel ? 0xFFFF : 0xFFE0);
            d.print("enter \xC3\xAB");
        } else {
            d.setTextColor(sel ? 0xFFFF : 0xFFFF);
            std::string v = s.items[i].value;
            int maxChars = (w / 2 - 8) / 6;
            if ((int)v.size() > maxChars) v = v.substr(0, maxChars - 1) + "…";
            d.print(v.c_str());
        }
    }
}

bool onKey(State& s, char ch) {
    if (s.items.empty()) return false;
    int n = (int)s.items.size();
    if (ch == key::Up) {
        s.selected = (s.selected - 1 + n) % n;
        return true;
    }
    if (ch == key::Down) {
        s.selected = (s.selected + 1) % n;
        return true;
    }
    return false;
}

}  // namespace ui::list
